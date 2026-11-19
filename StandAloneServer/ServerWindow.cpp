/* ServerWindow - the standalone server's window: start/stop, the players in
 * the room, a log of what happens, and a few host tools.
 * Win32 only, W APIs throughout (the project is MultiByte like the game). */

#include "global.h"
#include "DedicatedServer.h"
#include "ServerLog.h"
#include "resource.h"

#include <commctrl.h>
#include <iphlpapi.h>
#include <memory>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
	"version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{

const wchar_t *WINDOW_CLASS = L"YhanikiStandAloneServer";
const wchar_t *APP_TITLE = L"YHANIKI 連線伺服器";
const wchar_t *DEFAULT_SERVER_NAME = L"YHANIKI Server";
const UINT_PTR REFRESH_TIMER = 1;
const UINT REFRESH_MS = 200;
const int MAX_LOG_CHARS = 200000;
/* The name starts every server chat line and a chat line has to fit in one
 * packet (1020 bytes), so keep both short even when pasted or set by a program. */
const size_t MAX_NAME_CHARS = 40;
const size_t MAX_CHAT_CHARS = 200;

enum
{
	IDC_NAME = 1001, IDC_START, IDC_AUTOSTART, IDC_ADDR, IDC_STATUS, IDC_LIST,
	IDC_HOST, IDC_KICK, IDC_BAN, IDC_UNBAN, IDC_FORCESTART, IDC_CANCELSHARE,
	IDC_SONG, IDC_SHARE, IDC_LOG, IDC_CHAT, IDC_SEND
};

struct App
{
	HINSTANCE hInst;
	HWND hWnd;
	HWND hNameLabel, hName, hStart, hAutoStart;
	HWND hAddrLabel, hAddr, hStatus;
	HWND hList;
	HWND hHost, hKick, hBan, hUnban, hForceStart, hCancelShare;
	HWND hSong, hShare, hLog, hChat, hSend;
	HFONT hFont, hBoldFont;
	int iDpi;
	int iLineH;			// text line height of hFont
	int iAddrLines;		// lines in the address box
	std::wstring sIniPath;
	std::unique_ptr<DedicatedServer> pServer;
	DedicatedStatus status;
	std::wstring sListKey;	// what the list shows now, to skip needless rebuilds
	std::wstring sAppliedName;
} g;

void Layout();

/* ---------------- helpers ---------------- */

std::wstring Utf8ToWide( const std::string &s )
{
	if( s.empty() )
		return std::wstring();
	/* The game sends UTF-8; fall back to the local code page for anything else. */
	UINT cp = CP_UTF8;
	int n = MultiByteToWideChar( cp, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), NULL, 0 );
	if( n <= 0 )
	{
		cp = CP_ACP;
		n = MultiByteToWideChar( cp, 0, s.data(), (int)s.size(), NULL, 0 );
	}
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( cp, 0, s.data(), (int)s.size(), &w[0], n );
	return w;
}

std::string WideToUtf8( const std::wstring &w )
{
	if( w.empty() )
		return std::string();
	int n = WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL );
	std::string s( n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL );
	return s;
}

std::wstring Trim( const std::wstring &s )
{
	size_t b = s.find_first_not_of( L" \t\r\n" );
	if( b == std::wstring::npos )
		return std::wstring();
	size_t e = s.find_last_not_of( L" \t\r\n" );
	return s.substr( b, e - b + 1 );
}

std::wstring GetText( HWND h )
{
	int n = GetWindowTextLengthW( h );
	std::wstring s( n + 1, L'\0' );
	GetWindowTextW( h, &s[0], n + 1 );
	s.resize( n );
	return s;
}

void SetTextIfChanged( HWND h, const std::wstring &s )
{
	if( GetText( h ) != s )
		SetWindowTextW( h, s.c_str() );
}

std::wstring Format( const wchar_t *fmt, ... )
{
	wchar_t buf[1024];
	va_list va;
	va_start( va, fmt );
	_vsnwprintf_s( buf, _countof(buf), _TRUNCATE, fmt, va );
	va_end( va );
	return buf;
}

int S( int x ) { return MulDiv( x, g.iDpi, 96 ); }

std::wstring ExeDir()
{
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW( NULL, path, MAX_PATH );
	std::wstring s( path, n );
	size_t slash = s.find_last_of( L"\\/" );
	return slash == std::wstring::npos ? std::wstring() : s.substr( 0, slash + 1 );
}

/* ---------------- settings (StandAloneServer.ini next to the exe) ---------------- */

std::wstring ReadSetting( const wchar_t *key, const wchar_t *def )
{
	wchar_t buf[256];
	GetPrivateProfileStringW( L"Server", key, def, buf, _countof(buf), g.sIniPath.c_str() );
	return buf;
}

void WriteSetting( const wchar_t *key, const std::wstring &value )
{
	/* The profile API only writes Unicode into a file that starts with a UTF-16 BOM. */
	if( GetFileAttributesW( g.sIniPath.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		HANDLE h = CreateFileW( g.sIniPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
		if( h != INVALID_HANDLE_VALUE )
		{
			DWORD written;
			WriteFile( h, "\xFF\xFE", 2, &written, NULL );
			CloseHandle( h );
		}
	}
	WritePrivateProfileStringW( L"Server", key, value.c_str(), g.sIniPath.c_str() );
}

/* ---------------- addresses players can connect to ---------------- */

bool Contains( const wchar_t *s, const wchar_t *what )
{
	return s && wcsstr( s, what ) != NULL;
}

/* Adapters only this PC and its virtual machines can reach (VirtualBox host-only,
 * Hyper-V "Default Switch", WSL...).  Those have no gateway; a virtual switch that
 * carries the real network does, so it stays. */
bool IsLocalOnlyAdapter( const IP_ADAPTER_ADDRESSES *a )
{
	if( a->FirstGatewayAddress != NULL )
		return false;
	const wchar_t *aKeywords[] = { L"VirtualBox", L"VMware", L"Hyper-V", L"vEthernet", L"WSL" };
	for( int i = 0; i < 5; ++i )
		if( Contains( a->FriendlyName, aKeywords[i] ) || Contains( a->Description, aKeywords[i] ) )
			return true;
	return false;
}

/* One line per address: "IP<tab>adapter".  The main connection first: adapters
 * with a gateway before the rest (ZeroTier...), then by Windows' route metric
 * (Hamachi has a gateway too, but a metric of 9000). */
struct AddressLine
{
	std::wstring text;
	bool hasGateway;
	ULONG metric;
	bool operator<( const AddressLine &o ) const
	{
		if( hasGateway != o.hasGateway )
			return hasGateway;
		return metric < o.metric;
	}
};

std::vector<std::wstring> LocalAddresses()
{
	ULONG size = 16 * 1024;
	std::vector<BYTE> buf( size );
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG ret = GetAdaptersAddresses( AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)&buf[0], &size );
	if( ret == ERROR_BUFFER_OVERFLOW )
	{
		buf.resize( size );
		ret = GetAdaptersAddresses( AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES *)&buf[0], &size );
	}

	std::vector<AddressLine> vFound;
	if( ret == NO_ERROR )
	{
		for( IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)&buf[0]; a; a = a->Next )
		{
			if( a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || IsLocalOnlyAdapter( a ) )
				continue;
			/* "ZeroTier One [159924d6301db17b]" -> "ZeroTier One" */
			std::wstring sName = a->FriendlyName ? a->FriendlyName : L"";
			size_t bracket = sName.find( L" [" );
			if( bracket != std::wstring::npos )
				sName.erase( bracket );

			for( IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next )
			{
				const sockaddr_in *sin = (const sockaddr_in *)u->Address.lpSockaddr;
				const BYTE *b = (const BYTE *)&sin->sin_addr;
				if( b[0] == 169 && b[1] == 254 )
					continue;	// no DHCP answer, unusable
				AddressLine line;
				line.text = Format( L"%u.%u.%u.%u\t%s", b[0], b[1], b[2], b[3], sName.c_str() );
				line.hasGateway = a->FirstGatewayAddress != NULL;
				line.metric = a->Ipv4Metric;
				vFound.push_back( line );
			}
		}
	}

	std::stable_sort( vFound.begin(), vFound.end() );
	std::vector<std::wstring> vLines;
	for( size_t i = 0; i < vFound.size(); ++i )
		vLines.push_back( vFound[i].text );
	if( vLines.empty() )
		vLines.push_back( L"找不到可用的網路連線" );
	vLines.push_back( L"127.0.0.1\t在這台電腦上玩的人用這個" );
	return vLines;
}

const int MAX_ADDR_LINES = 6;	// the box doesn't grow further; the rest scrolls with the wheel

void ShowAddresses()
{
	std::vector<std::wstring> vLines = LocalAddresses();
	std::wstring sText;
	for( size_t i = 0; i < vLines.size(); ++i )
		sText += (i ? L"\r\n" : L"") + vLines[i];
	SetWindowTextW( g.hAddr, sText.c_str() );
	g.iAddrLines = (int)vLines.size();
}

/* ---------------- log ---------------- */

void AppendLog()
{
	std::vector<std::string> vLines;
	ServerLog::Take( vLines );
	if( vLines.empty() )
		return;

	std::wstring sText;
	for( size_t i = 0; i < vLines.size(); ++i )
		sText += Utf8ToWide( vLines[i] ) + L"\r\n";

	int iLen = GetWindowTextLengthW( g.hLog );
	if( iLen + (int)sText.size() > MAX_LOG_CHARS )
	{
		/* Drop the older half, at a line start. */
		int iLine = (int)SendMessageW( g.hLog, EM_LINEFROMCHAR, iLen / 2, 0 );
		int iCut = (int)SendMessageW( g.hLog, EM_LINEINDEX, iLine + 1, 0 );
		if( iCut > 0 )
		{
			SendMessageW( g.hLog, EM_SETSEL, 0, iCut );
			SendMessageW( g.hLog, EM_REPLACESEL, FALSE, (LPARAM)L"" );
		}
		iLen = GetWindowTextLengthW( g.hLog );
	}
	SendMessageW( g.hLog, EM_SETSEL, iLen, iLen );
	SendMessageW( g.hLog, EM_REPLACESEL, FALSE, (LPARAM)sText.c_str() );
	SendMessageW( g.hLog, EM_SCROLLCARET, 0, 0 );
}

/* ---------------- player list ---------------- */

const DedicatedClientInfo *FindClient( unsigned int id )
{
	for( size_t i = 0; i < g.status.clients.size(); ++i )
		if( g.status.clients[i].id == id )
			return &g.status.clients[i];
	return NULL;
}

std::wstring ClientName( const DedicatedClientInfo &c )
{
	/* The game sends the names when the players pick their style. */
	return c.names.empty() ? std::wstring( L"（未命名）" ) : Utf8ToWide( c.names );
}

std::wstring ClientState( const DedicatedClientInfo &c )
{
	if( c.starting )		return L"準備開始";
	if( c.inGame )			return L"遊戲中";
	if( c.inSongSelect )	return L"選歌中";
	return L"其他畫面";
}

std::wstring ClientSong( const DedicatedClientInfo &c )
{
	if( c.receiving )
	{
		if( g.status.shareTotalKB == 0 )
			return L"接收中…";
		return Format( L"接收中 %u%%", (unsigned int)((unsigned long long)g.status.shareDoneKB * 100 / g.status.shareTotalKB) );
	}
	if( c.sending )
		return L"傳送中";
	/* The server forgets who has the song once it starts. */
	if( g.status.songTitle.empty() || c.inGame || c.starting )
		return L"—";
	return c.hasSong ? L"有" : L"沒有";
}

unsigned int SelectedClientId()
{
	int iItem = (int)SendMessageW( g.hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED );
	if( iItem < 0 )
		return 0;
	LVITEMW item = {};
	item.mask = LVIF_PARAM;
	item.iItem = iItem;
	SendMessageW( g.hList, LVM_GETITEMW, 0, (LPARAM)&item );
	return (unsigned int)item.lParam;
}

void RefreshList()
{
	std::vector<std::vector<std::wstring> > vRows;
	std::wstring sKey;
	for( size_t i = 0; i < g.status.clients.size(); ++i )
	{
		const DedicatedClientInfo &c = g.status.clients[i];
		std::vector<std::wstring> row;
		/* Same as the game's player list (ScreenNetSelectMusic): "HOST", "1.", "2."... */
		row.push_back( c.index == 0 ? std::wstring( L"HOST" ) : Format( L"%u.", c.index ) );
		row.push_back( ClientName( c ) );
		row.push_back( Utf8ToWide( c.ip ) );
		row.push_back( ClientState( c ) );
		row.push_back( ClientSong( c ) );
		sKey += Format( L"%u|", c.id );
		for( size_t j = 0; j < row.size(); ++j )
			sKey += row[j] + L"|";
		sKey += L"\n";
		vRows.push_back( row );
	}
	if( sKey == g.sListKey )
		return;
	g.sListKey = sKey;

	const unsigned int iSelected = SelectedClientId();
	SendMessageW( g.hList, WM_SETREDRAW, FALSE, 0 );
	SendMessageW( g.hList, LVM_DELETEALLITEMS, 0, 0 );
	for( size_t i = 0; i < vRows.size(); ++i )
	{
		LVITEMW item = {};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = (int)i;
		item.pszText = (LPWSTR)vRows[i][0].c_str();
		item.lParam = (LPARAM)g.status.clients[i].id;
		SendMessageW( g.hList, LVM_INSERTITEMW, 0, (LPARAM)&item );
		for( size_t j = 1; j < vRows[i].size(); ++j )
		{
			LVITEMW sub = {};
			sub.iSubItem = (int)j;
			sub.pszText = (LPWSTR)vRows[i][j].c_str();
			SendMessageW( g.hList, LVM_SETITEMTEXTW, i, (LPARAM)&sub );
		}
		if( g.status.clients[i].id == iSelected )
		{
			LVITEMW state = {};
			state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
			state.state = LVIS_SELECTED | LVIS_FOCUSED;
			SendMessageW( g.hList, LVM_SETITEMSTATE, i, (LPARAM)&state );
		}
	}
	SendMessageW( g.hList, WM_SETREDRAW, TRUE, 0 );
	InvalidateRect( g.hList, NULL, FALSE );
}

/* ---------------- the rest of the window ---------------- */

void UpdateControls()
{
	const bool bRunning = g.status.running;
	const DedicatedClientInfo *pSel = bRunning ? FindClient( SelectedClientId() ) : NULL;

	SetTextIfChanged( g.hStart, bRunning ? L"停止伺服器" : L"啟動伺服器" );
	SetTextIfChanged( g.hStatus, bRunning ?
		Format( L"● 執行中　　玩家 %u 人", (unsigned int)g.status.clients.size() ) :
		std::wstring( L"○ 已停止" ) );
	SetTextIfChanged( g.hWnd, bRunning ? std::wstring( APP_TITLE ) + L" - 執行中" : std::wstring( APP_TITLE ) );

	std::wstring sSong = L"目前歌曲：";
	if( !bRunning )
		sSong += L"—";
	else if( g.status.songTitle.empty() )
		sSong += L"（房主還沒選歌）";
	else
	{
		sSong += Utf8ToWide( g.status.songTitle );
		if( !g.status.songSubtitle.empty() )
			sSong += L" " + Utf8ToWide( g.status.songSubtitle );
		if( !g.status.songArtist.empty() )
			sSong += L"　/　" + Utf8ToWide( g.status.songArtist );
	}
	SetTextIfChanged( g.hSong, sSong );

	std::wstring sShare = L"傳歌：";
	if( bRunning && g.status.shareActive )
	{
		const DedicatedClientInfo *pFrom = FindClient( g.status.shareSenderId );
		const DedicatedClientInfo *pTo = FindClient( g.status.shareReceiverId );
		sShare += (pFrom ? ClientName( *pFrom ) : L"?") + L" → " + (pTo ? ClientName( *pTo ) : L"?");
		if( g.status.shareTotalKB > 0 )
			sShare += Format( L"　%u / %u KB", g.status.shareDoneKB, g.status.shareTotalKB );
		else
			sShare += L"　連線中…";
	}
	else
		sShare += L"無";
	if( bRunning && g.status.shareQueued > 0 )
		sShare += Format( L"　（另有 %u 筆排隊）", g.status.shareQueued );
	SetTextIfChanged( g.hShare, sShare );

	SetTextIfChanged( g.hUnban, g.status.bannedCount > 0 ?
		Format( L"解除封鎖 (%u)", g.status.bannedCount ) : std::wstring( L"解除封鎖" ) );

	EnableWindow( g.hHost, pSel && pSel->index != 0 );
	EnableWindow( g.hKick, pSel != NULL );
	EnableWindow( g.hBan, pSel != NULL );
	EnableWindow( g.hUnban, bRunning && g.status.bannedCount > 0 );
	EnableWindow( g.hForceStart, bRunning && !g.status.clients.empty() );
	EnableWindow( g.hCancelShare, bRunning && (g.status.shareActive || g.status.shareQueued > 0) );
	EnableWindow( g.hChat, bRunning );
	EnableWindow( g.hSend, bRunning );
}

void Refresh()
{
	AppendLog();
	g.status = g.pServer ? g.pServer->GetStatus() : DedicatedStatus();
	RefreshList();
	UpdateControls();
}

std::wstring CurrentName()
{
	const std::wstring sText = GetText( g.hName );
	std::wstring sName = Trim( sText.substr( 0, MAX_NAME_CHARS ) );
	if( sName.empty() )
		sName = DEFAULT_SERVER_NAME;
	if( sName != sText )
		SetWindowTextW( g.hName, sName.c_str() );
	return sName;
}

/* The name is part of every server chat line, so a running server takes a new one right away. */
void ApplyName()
{
	std::wstring sName = CurrentName();
	if( sName == g.sAppliedName )
		return;
	g.sAppliedName = sName;
	WriteSetting( L"Name", sName );
	if( g.pServer )
		g.pServer->Post( DedicatedServer::CMD_SET_NAME, 0, WideToUtf8( sName ) );
}

void StartServer()
{
	if( g.pServer )
		return;
	std::wstring sName = CurrentName();
	g.sAppliedName = sName;
	WriteSetting( L"Name", sName );

	std::unique_ptr<DedicatedServer> pServer( new DedicatedServer );
	std::string sError;
	if( !pServer->Start( WideToUtf8( sName ), sError ) )
	{
		ServerLog::Write( "無法啟動伺服器：" + sError );
		Refresh();
		MessageBoxW( g.hWnd, (L"無法啟動伺服器：\n\n" + Utf8ToWide( sError )).c_str(), APP_TITLE, MB_ICONWARNING | MB_OK );
		return;
	}
	g.pServer = std::move( pServer );
	ShowAddresses();	// the network may have changed since the window opened
	Layout();
	Refresh();
}

bool ConfirmDisconnectAll( const wchar_t *sAction )
{
	if( !g.pServer || g.status.clients.empty() )
		return true;
	std::wstring sMsg = Format( L"目前有 %u 位玩家連線中，%s會中斷他們的連線。\n\n確定嗎？",
		(unsigned int)g.status.clients.size(), sAction );
	return MessageBoxW( g.hWnd, sMsg.c_str(), APP_TITLE, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2 ) == IDYES;
}

void StopServer()
{
	if( !g.pServer )
		return;
	if( !ConfirmDisconnectAll( L"停止伺服器" ) )
		return;
	g.pServer->Stop();
	g.pServer.reset();
	Refresh();
}

void SendChat()
{
	std::wstring sMsg = Trim( GetText( g.hChat ).substr( 0, MAX_CHAT_CHARS ) );
	if( sMsg.empty() || !g.pServer )
		return;
	g.pServer->Post( DedicatedServer::CMD_CHAT, 0, WideToUtf8( sMsg ) );
	SetWindowTextW( g.hChat, L"" );
}

void OnClientButton( int id )
{
	const DedicatedClientInfo *pSel = FindClient( SelectedClientId() );
	if( !g.pServer || !pSel )
		return;
	switch( id )
	{
	case IDC_HOST:
		g.pServer->Post( DedicatedServer::CMD_MAKE_HOST, pSel->id );
		break;
	case IDC_KICK:
		g.pServer->Post( DedicatedServer::CMD_KICK, pSel->id );
		break;
	case IDC_BAN:
		{
			std::wstring sMsg = L"封鎖 " + ClientName( *pSel ) + L"（" + Utf8ToWide( pSel->ip ) + L"）？\n\n"
				L"在伺服器停止或按「解除封鎖」之前，這個 IP 都無法再連進來"
				L"（同一個網路出口的其他人也會被擋）。";
			if( MessageBoxW( g.hWnd, sMsg.c_str(), APP_TITLE, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2 ) == IDYES )
				g.pServer->Post( DedicatedServer::CMD_BAN, pSel->id );
		}
		break;
	}
}

/* ---------------- layout ---------------- */

HWND MakeControl( const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD exStyle = 0 )
{
	HWND h = CreateWindowExW( exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
		g.hWnd, (HMENU)(INT_PTR)id, g.hInst, NULL );
	SendMessageW( h, WM_SETFONT, (WPARAM)g.hFont, FALSE );
	return h;
}

void Layout()
{
	RECT rc;
	GetClientRect( g.hWnd, &rc );
	const int W = rc.right, H = rc.bottom;
	const int m = S(10), rowH = S(24), labelW = S(76), btnH = S(28);
	const int fieldX = m + labelW + S(4);
	HDWP dwp = BeginDeferWindowPos( 24 );
#define PLACE( h, x, y, w, hh ) dwp = DeferWindowPos( dwp, h, NULL, x, y, (w) > 0 ? (w) : 0, (hh) > 0 ? (hh) : 0, SWP_NOZORDER | SWP_NOACTIVATE )

	int y = m;
	PLACE( g.hNameLabel, m, y + S(4), labelW, S(18) );
	PLACE( g.hName, fieldX, y, S(220), rowH );
	PLACE( g.hStart, fieldX + S(230), y - S(2), S(110), btnH );
	PLACE( g.hAutoStart, fieldX + S(352), y + S(2), W - (fieldX + S(352)) - m, S(20) );
	y += btnH + S(4);

	const int iShownLines = g.iAddrLines < MAX_ADDR_LINES ? g.iAddrLines : MAX_ADDR_LINES;
	const int addrH = iShownLines * g.iLineH + S(6);
	PLACE( g.hAddrLabel, m, y + S(3), labelW, S(18) );
	PLACE( g.hAddr, fieldX, y, W - fieldX - m, addrH );
	y += addrH + S(6);

	PLACE( g.hStatus, m, y, W - 2 * m, S(20) );
	y += S(24);

	const int fixedBelow = S(6) + btnH + S(8) + S(20) + S(20) + S(8) + S(6) + rowH + m;
	const int flexible = H - y - fixedBelow;
	const int listH = flexible * 45 / 100 > S(90) ? flexible * 45 / 100 : S(90);
	const int logH = flexible - listH > S(60) ? flexible - listH : S(60);

	PLACE( g.hList, m, y, W - 2 * m, listH );
	y += listH + S(6);

	const int bw = S(92), gap = S(6);
	PLACE( g.hHost, m, y, bw, btnH );
	PLACE( g.hKick, m + (bw + gap), y, bw, btnH );
	PLACE( g.hBan, m + 2 * (bw + gap), y, bw, btnH );
	PLACE( g.hUnban, m + 3 * (bw + gap), y, S(112), btnH );
	PLACE( g.hCancelShare, W - m - bw, y, bw, btnH );
	PLACE( g.hForceStart, W - m - 2 * bw - gap, y, bw, btnH );
	y += btnH + S(8);

	PLACE( g.hSong, m, y, W - 2 * m, S(20) );
	y += S(20);
	PLACE( g.hShare, m, y, W - 2 * m, S(20) );
	y += S(20) + S(8);

	PLACE( g.hLog, m, y, W - 2 * m, logH );
	y += logH + S(6);

	PLACE( g.hChat, m, y, W - 2 * m - S(96), rowH );
	PLACE( g.hSend, W - m - S(90), y - S(2), S(90), btnH );
#undef PLACE
	EndDeferWindowPos( dwp );

	/* Let the player-name column take the spare width. */
	RECT rcList;
	GetClientRect( g.hList, &rcList );
	int iOthers = S(60) + S(130) + S(90) + S(100);
	int iNameW = rcList.right - iOthers - GetSystemMetrics( SM_CXVSCROLL );
	SendMessageW( g.hList, LVM_SETCOLUMNWIDTH, 1, iNameW > S(120) ? iNameW : S(120) );
}

void CreateControls()
{
	const DWORD BTN = WS_TABSTOP | BS_PUSHBUTTON;
	g.hNameLabel = MakeControl( L"STATIC", L"伺服器名稱", SS_LEFT, -1 );
	g.hName = MakeControl( L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_NAME, WS_EX_CLIENTEDGE );
	SendMessageW( g.hName, EM_LIMITTEXT, MAX_NAME_CHARS, 0 );
	g.hStart = MakeControl( L"BUTTON", L"啟動伺服器", BTN, IDC_START );
	g.hAutoStart = MakeControl( L"BUTTON", L"開啟程式時自動啟動", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_AUTOSTART );

	g.hAddrLabel = MakeControl( L"STATIC", L"連線位址", SS_LEFT, -1 );
	/* One address per line, IP and adapter name in two tab-aligned columns.
	 * An edit rather than a label, so the IP can be selected and copied. */
	g.hAddr = MakeControl( L"EDIT", L"", ES_READONLY | ES_MULTILINE, IDC_ADDR, WS_EX_CLIENTEDGE );
	int iTabStop = 72;	// dialog units: room for "255.255.255.255"
	SendMessageW( g.hAddr, EM_SETTABSTOPS, 1, (LPARAM)&iTabStop );
	g.hStatus = MakeControl( L"STATIC", L"", SS_LEFT, IDC_STATUS );
	SendMessageW( g.hStatus, WM_SETFONT, (WPARAM)g.hBoldFont, FALSE );

	g.hList = MakeControl( WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
		IDC_LIST, WS_EX_CLIENTEDGE );
	SendMessageW( g.hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
	const wchar_t *aCols[] = { L"#", L"玩家", L"IP", L"狀態", L"歌曲" };
	const int aWidths[] = { 60, 200, 130, 90, 100 };
	for( int i = 0; i < 5; ++i )
	{
		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH;
		col.pszText = (LPWSTR)aCols[i];
		col.cx = S( aWidths[i] );
		SendMessageW( g.hList, LVM_INSERTCOLUMNW, i, (LPARAM)&col );
	}

	g.hHost = MakeControl( L"BUTTON", L"設為房主", BTN, IDC_HOST );
	g.hKick = MakeControl( L"BUTTON", L"踢出", BTN, IDC_KICK );
	g.hBan = MakeControl( L"BUTTON", L"封鎖", BTN, IDC_BAN );
	g.hUnban = MakeControl( L"BUTTON", L"解除封鎖", BTN, IDC_UNBAN );
	g.hForceStart = MakeControl( L"BUTTON", L"強制開始", BTN, IDC_FORCESTART );
	g.hCancelShare = MakeControl( L"BUTTON", L"取消傳歌", BTN, IDC_CANCELSHARE );

	g.hSong = MakeControl( L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_SONG );
	g.hShare = MakeControl( L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_SHARE );

	g.hLog = MakeControl( L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_LOG, WS_EX_CLIENTEDGE );
	SendMessageW( g.hLog, EM_SETLIMITTEXT, 0, 0 );
	g.hChat = MakeControl( L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_CHAT, WS_EX_CLIENTEDGE );
	SendMessageW( g.hChat, EM_LIMITTEXT, MAX_CHAT_CHARS, 0 );
	SendMessageW( g.hChat, EM_SETCUEBANNER, TRUE, (LPARAM)L"輸入訊息，按 Enter 以伺服器名義傳給所有玩家" );
	g.hSend = MakeControl( L"BUTTON", L"送出", BTN, IDC_SEND );

	g.sAppliedName = ReadSetting( L"Name", DEFAULT_SERVER_NAME );
	SetWindowTextW( g.hName, g.sAppliedName.c_str() );
	SendMessageW( g.hAutoStart, BM_SETCHECK, ReadSetting( L"AutoStart", L"0" ) == L"1" ? BST_CHECKED : BST_UNCHECKED, 0 );
	ShowAddresses();
}

void CreateFonts()
{
	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0 );
	g.hFont = CreateFontIndirectW( &ncm.lfMessageFont );
	LOGFONTW bold = ncm.lfMessageFont;
	bold.lfWeight = FW_BOLD;
	g.hBoldFont = CreateFontIndirectW( &bold );

	HDC hdc = GetDC( NULL );
	HGDIOBJ hOld = SelectObject( hdc, g.hFont );
	TEXTMETRICW tm;
	GetTextMetricsW( hdc, &tm );
	SelectObject( hdc, hOld );
	ReleaseDC( NULL, hdc );
	g.iLineH = tm.tmHeight;
}

/* ---------------- window procedure ---------------- */

LRESULT CALLBACK WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch( msg )
	{
	case WM_CREATE:
		g.hWnd = hWnd;
		CreateControls();
		Layout();
		Refresh();
		SetTimer( hWnd, REFRESH_TIMER, REFRESH_MS, NULL );
		if( SendMessageW( g.hAutoStart, BM_GETCHECK, 0, 0 ) == BST_CHECKED )
			StartServer();
		return 0;

	case WM_SIZE:
		if( wParam != SIZE_MINIMIZED )
			Layout();
		return 0;

	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lParam)->ptMinTrackSize.x = S(720);
		((MINMAXINFO *)lParam)->ptMinTrackSize.y = S(560);
		return 0;

	case WM_TIMER:
		if( wParam == REFRESH_TIMER )
			Refresh();
		return 0;

	case WM_CTLCOLORSTATIC:
		if( (HWND)lParam == g.hStatus )
		{
			HDC hdc = (HDC)wParam;
			SetTextColor( hdc, g.status.running ? RGB( 0, 140, 60 ) : RGB( 120, 120, 120 ) );
			SetBkMode( hdc, TRANSPARENT );
			return (LRESULT)GetSysColorBrush( COLOR_BTNFACE );
		}
		break;

	case WM_NOTIFY:
		if( ((NMHDR *)lParam)->idFrom == IDC_LIST && ((NMHDR *)lParam)->code == LVN_ITEMCHANGED )
			UpdateControls();
		return 0;

	case WM_COMMAND:
		switch( LOWORD( wParam ) )
		{
		case IDOK:	// Enter
			if( GetFocus() == g.hChat )
				SendChat();
			else if( GetFocus() == g.hName )
				ApplyName();
			return 0;
		case IDCANCEL:	// Esc: don't close by accident
			return 0;
		case IDC_START:
			if( g.pServer )
				StopServer();
			else
				StartServer();
			return 0;
		case IDC_AUTOSTART:
			WriteSetting( L"AutoStart", SendMessageW( g.hAutoStart, BM_GETCHECK, 0, 0 ) == BST_CHECKED ? L"1" : L"0" );
			return 0;
		case IDC_NAME:
			if( HIWORD( wParam ) == EN_KILLFOCUS )
				ApplyName();
			return 0;
		case IDC_HOST:
		case IDC_KICK:
		case IDC_BAN:
			OnClientButton( LOWORD( wParam ) );
			return 0;
		case IDC_UNBAN:
			if( g.pServer )
				g.pServer->Post( DedicatedServer::CMD_CLEAR_BANS );
			return 0;
		case IDC_FORCESTART:
			if( g.pServer )
				g.pServer->Post( DedicatedServer::CMD_FORCE_START );
			return 0;
		case IDC_CANCELSHARE:
			if( g.pServer )
				g.pServer->Post( DedicatedServer::CMD_CANCEL_SHARES );
			return 0;
		case IDC_SEND:
			SendChat();
			return 0;
		}
		break;

	case WM_CLOSE:
		if( !ConfirmDisconnectAll( L"關閉程式" ) )
			return 0;
		if( g.pServer )
		{
			g.pServer->Stop();
			g.pServer.reset();
		}
		DestroyWindow( hWnd );
		return 0;

	case WM_DESTROY:
		KillTimer( hWnd, REFRESH_TIMER );
		PostQuitMessage( 0 );
		return 0;
	}
	return DefWindowProcW( hWnd, msg, wParam, lParam );
}

}	// namespace

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow )
{
	SetProcessDPIAware();
	g.hInst = hInstance;

	HDC hdc = GetDC( NULL );
	g.iDpi = GetDeviceCaps( hdc, LOGPIXELSY );
	ReleaseDC( NULL, hdc );

	const std::wstring sDir = ExeDir();
	g.sIniPath = sDir + L"StandAloneServer.ini";
	ServerLog::Open( sDir + L"StandAloneServer.log" );

	WSADATA wsa;
	WSAStartup( MAKEWORD( 2, 2 ), &wsa );

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx( &icc );
	CreateFonts();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIconW( hInstance, MAKEINTRESOURCEW( IDI_APPICON ) );
	wc.hIconSm = (HICON)LoadImageW( hInstance, MAKEINTRESOURCEW( IDI_APPICON ), IMAGE_ICON,
		GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), 0 );
	wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
	wc.hbrBackground = GetSysColorBrush( COLOR_BTNFACE );
	wc.lpszClassName = WINDOW_CLASS;
	RegisterClassExW( &wc );

	HWND hWnd = CreateWindowExW( WS_EX_CONTROLPARENT, WINDOW_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(700), NULL, NULL, hInstance, NULL );
	if( !hWnd )
		return 1;
	ShowWindow( hWnd, nCmdShow );
	UpdateWindow( hWnd );

	MSG msg;
	while( GetMessageW( &msg, NULL, 0, 0 ) > 0 )
	{
		if( IsDialogMessageW( hWnd, &msg ) )
			continue;
		TranslateMessage( &msg );
		DispatchMessageW( &msg );
	}

	DeleteObject( g.hFont );
	DeleteObject( g.hBoldFont );
	ServerLog::Close();
	WSACleanup();
	return 0;
}
