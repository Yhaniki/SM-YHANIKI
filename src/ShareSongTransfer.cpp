#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "global.h"

#if !defined(WITHOUT_NETWORKING)
#include "ShareSongTransfer.h"
#include "RageLog.h"
#include "RageUtil.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <random>
#include <string>

#pragma comment(lib, "ws2_32.lib")

/*
 * Wire protocol (all integers big endian)
 *
 *   receiver -> sender : HELLO  u32 MAGIC_HELLO, u32 token, u8 transferId
 *   sender -> receiver : u32 headerLength, then header:
 *                        u32 MAGIC_HEADER, u16 version, str folderName, u32 fileCount,
 *                        fileCount x { str relativePath, u64 size, u8 isVideo }
 *   receiver -> sender : NEED   u32 MAGIC_NEED, u32 count, count x u32 fileIndex (ascending)
 *   sender -> receiver : raw bytes of every needed file, in that order
 *   receiver -> sender : DONE   u32 MAGIC_DONE
 *
 * str = u16 byteLength + UTF-8 bytes.  Paths use '/' as separator.
 */

namespace
{
const int POLL_MS = 200;
const DWORD CONNECT_TIMEOUT_MS = 6000;		// per address the receiver tries
const DWORD ACCEPT_TIMEOUT_MS = 30000;		// sender waiting for the receiver to connect
const DWORD HANDSHAKE_TIMEOUT_MS = 10000;
const DWORD STALL_TIMEOUT_MS = 15000;		// no byte moved for this long -> abort
const DWORD FINISH_TIMEOUT_MS = 20000;		// sender waiting for the final DONE

const unsigned int MAGIC_HELLO = 0x59485331;	// "YHS1"
const unsigned int MAGIC_HEADER = 0x59485348;	// "YHSH"
const unsigned int MAGIC_NEED = 0x5948534E;		// "YHSN"
const unsigned int MAGIC_DONE = 0x59485344;		// "YHSD"
const unsigned int PROTOCOL_VERSION = 1;

const unsigned int MAX_FILES = 10000;
const unsigned int MAX_HEADER_BYTES = 8 * 1024 * 1024;
const unsigned long long MAX_TOTAL_BYTES = 16ULL * 1024 * 1024 * 1024;
const unsigned int MAX_PATH_BYTES = 1024;
const int MAX_DIR_DEPTH = 8;
const unsigned int CHUNK_BYTES = 64 * 1024;
const int SOCKET_BUFFER_BYTES = 512 * 1024;
const wchar_t PART_SUFFIX_W[] = L".yhpart";
const char PART_SUFFIX[] = ".yhpart";

unsigned long long MinU64( unsigned long long a, unsigned long long b ) { return a < b ? a : b; }

/* ---------- string conversion ---------- */

std::wstring ToWide( const std::string &s, UINT iCodePage )
{
	if( s.empty() )
		return std::wstring();
	int n = MultiByteToWideChar( iCodePage, 0, s.data(), (int)s.size(), NULL, 0 );
	if( n <= 0 )
		return std::wstring();
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( iCodePage, 0, s.data(), (int)s.size(), &w[0], n );
	return w;
}

std::string FromWide( const std::wstring &w, UINT iCodePage )
{
	if( w.empty() )
		return std::string();
	int n = WideCharToMultiByte( iCodePage, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL );
	if( n <= 0 )
		return std::string();
	std::string s( n, '\0' );
	WideCharToMultiByte( iCodePage, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL );
	return s;
}

std::wstring AnsiToWide( const std::string &s ) { return ToWide( s, CP_ACP ); }
std::string WideToAnsi( const std::wstring &w ) { return FromWide( w, CP_ACP ); }
std::string AnsiToUtf8( const std::string &s ) { return FromWide( ToWide( s, CP_ACP ), CP_UTF8 ); }
std::wstring Utf8ToWide( const std::string &s ) { return ToWide( s, CP_UTF8 ); }

bool EndsWithNoCase( const std::string &s, const char *szSuffix )
{
	size_t n = strlen( szSuffix );
	return s.size() >= n && _stricmp( s.c_str() + s.size() - n, szSuffix ) == 0;
}

std::string StripTrailingSlashes( std::string s )
{
	while( !s.empty() && (s[s.size()-1] == '/' || s[s.size()-1] == '\\') )
		s.erase( s.size()-1 );
	return s;
}

/* ---------- receiver side path sanitizing ---------- */

/* Can the game (which uses the ANSI file APIs) open a name containing this character? */
bool IsCharOkForAnsi( wchar_t c )
{
	if( c < 0x80 )
		return true;
	if( c >= 0xD800 && c <= 0xDFFF )	// surrogate pairs never fit an ANSI code page
		return false;
	char buf[8];
	BOOL bUsedDefault = FALSE;
	int n = WideCharToMultiByte( CP_ACP, WC_NO_BEST_FIT_CHARS, &c, 1, buf, sizeof(buf), NULL, &bUsedDefault );
	return n > 0 && !bUsedDefault;
}

bool IsReservedDeviceName( const std::wstring &sName )
{
	std::wstring sBase = sName.substr( 0, sName.find( L'.' ) );
	for( size_t i = 0; i < sBase.size(); ++i )
		sBase[i] = (wchar_t)towupper( sBase[i] );
	static const wchar_t *szReserved[] = { L"CON", L"PRN", L"AUX", L"NUL",
		L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
		L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
	for( size_t i = 0; i < sizeof(szReserved)/sizeof(szReserved[0]); ++i )
		if( sBase == szReserved[i] )
			return true;
	return false;
}

/* Make one path component safe.  Returns false for components that must be rejected. */
bool SanitizeComponent( const std::wstring &sIn, std::wstring &sOut )
{
	if( sIn.empty() || sIn == L"." || sIn == L".." )
		return false;
	sOut.clear();
	for( size_t i = 0; i < sIn.size(); ++i )
	{
		wchar_t c = sIn[i];
		if( c < 32 || wcschr( L"<>:\"/\\|?*", c ) != NULL || !IsCharOkForAnsi( c ) )
			sOut += L'_';
		else
			sOut += c;
	}
	while( !sOut.empty() && (sOut[sOut.size()-1] == L' ' || sOut[sOut.size()-1] == L'.') )
		sOut.erase( sOut.size()-1 );
	if( sOut.empty() )
		sOut = L"_";
	if( IsReservedDeviceName( sOut ) )
		sOut = L"_" + sOut;
	return true;
}

/* Split a '/' separated relative path from the network into safe components.
 * Absolute paths, drive letters and ".." can't escape: every component is sanitized
 * and ".." is rejected outright. */
bool SanitizeRelPath( const std::string &sUtf8, std::vector<std::wstring> &vParts )
{
	vParts.clear();
	std::wstring w = Utf8ToWide( sUtf8 );
	std::wstring sCur;
	for( size_t i = 0; i <= w.size(); ++i )
	{
		if( i == w.size() || w[i] == L'/' || w[i] == L'\\' )
		{
			if( !sCur.empty() )
			{
				std::wstring sSafe;
				if( !SanitizeComponent( sCur, sSafe ) )
					return false;
				vParts.push_back( sSafe );
			}
			sCur.clear();
		}
		else
			sCur += w[i];
	}
	return !vParts.empty() && (int)vParts.size() <= MAX_DIR_DEPTH + 1;
}

bool PathExistsW( const std::wstring &s )
{
	return GetFileAttributesW( s.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

/* ---------- sender side folder scan ---------- */

struct FileEntry
{
	std::string sRelUtf8;	// path inside the song folder, '/' separated
	std::string sFullAnsi;	// sender only
	unsigned long long iSize;
	bool bVideo;
};

void ScanDir( const std::string &sDirAnsi, const std::string &sRelUtf8, int iDepth,
	bool bIncludeVideo, std::vector<FileEntry> &vOut )
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA( (sDirAnsi + "\\*").c_str(), &fd );
	if( h == INVALID_HANDLE_VALUE )
		return;
	do
	{
		std::string sName = fd.cFileName;
		if( sName == "." || sName == ".." )
			continue;
		if( fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) )
			continue;
		/* '?' means the name doesn't fit the code page; the game can't open it either. */
		if( sName.find( '?' ) != std::string::npos )
			continue;

		std::string sNameUtf8 = AnsiToUtf8( sName );
		std::string sRel = sRelUtf8.empty() ? sNameUtf8 : sRelUtf8 + "/" + sNameUtf8;
		std::string sFull = sDirAnsi + "\\" + sName;

		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			if( iDepth < MAX_DIR_DEPTH )
				ScanDir( sFull, sRel, iDepth + 1, bIncludeVideo, vOut );
			continue;
		}
		if( EndsWithNoCase( sName, PART_SUFFIX ) )
			continue;

		FileEntry e;
		e.sRelUtf8 = sRel;
		e.sFullAnsi = sFull;
		e.iSize = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
		e.bVideo = ShareSongTransfer::IsVideoFile( sName.c_str() );
		if( e.bVideo && !bIncludeVideo )
			continue;
		if( e.sRelUtf8.size() > MAX_PATH_BYTES )
			continue;
		vOut.push_back( e );
	} while( FindNextFileA( h, &fd ) && vOut.size() < MAX_FILES );
	FindClose( h );
}

/* ---------- byte buffers ---------- */

struct ByteWriter
{
	std::string buf;
	void U8( unsigned int v )			{ buf.push_back( (char)(v & 0xFF) ); }
	void U16( unsigned int v )			{ U8( v >> 8 ); U8( v ); }
	void U32( unsigned int v )			{ U16( v >> 16 ); U16( v & 0xFFFF ); }
	void U64( unsigned long long v )	{ U32( (unsigned int)(v >> 32) ); U32( (unsigned int)(v & 0xFFFFFFFF) ); }
	void Str( const std::string &s )	{ U16( (unsigned int)s.size() ); buf.append( s ); }
};

struct ByteReader
{
	const std::string &buf;
	size_t pos;
	bool ok;
	explicit ByteReader( const std::string &b ) : buf(b), pos(0), ok(true) { }
	unsigned int U8()
	{
		if( pos >= buf.size() ) { ok = false; return 0; }
		return (unsigned char)buf[pos++];
	}
	unsigned int U16()				{ unsigned int a = U8(); return (a << 8) | U8(); }
	unsigned int U32()				{ unsigned int a = U16(); return (a << 16) | U16(); }
	unsigned long long U64()		{ unsigned long long a = U32(); return (a << 32) | U32(); }
	std::string Str()
	{
		unsigned int n = U16();
		if( !ok || pos + n > buf.size() ) { ok = false; return std::string(); }
		std::string s = buf.substr( pos, n );
		pos += n;
		return s;
	}
};

/* ---------- non-blocking socket with timeouts and cancel ---------- */

class Socket
{
public:
	explicit Socket( std::atomic<bool> &bCancel ) : m_s(INVALID_SOCKET), m_bCancel(bCancel) { }
	~Socket() { Close(); }

	void Attach( SOCKET s )
	{
		Close();
		m_s = s;
		if( m_s == INVALID_SOCKET )
			return;
		u_long iNonBlocking = 1;
		ioctlsocket( m_s, FIONBIO, &iNonBlocking );
		int iBuf = SOCKET_BUFFER_BYTES;
		setsockopt( m_s, SOL_SOCKET, SO_SNDBUF, (const char *)&iBuf, sizeof(iBuf) );
		setsockopt( m_s, SOL_SOCKET, SO_RCVBUF, (const char *)&iBuf, sizeof(iBuf) );
	}
	SOCKET Get() const { return m_s; }
	bool IsOpen() const { return m_s != INVALID_SOCKET; }
	void Close()
	{
		if( m_s != INVALID_SOCKET )
			closesocket( m_s );
		m_s = INVALID_SOCKET;
	}

	/* 1 = ready, 0 = nothing yet, -1 = socket error */
	int Wait( bool bWrite, int iMs )
	{
		fd_set fds, efds;
		FD_ZERO( &fds );
		FD_ZERO( &efds );
		FD_SET( m_s, &fds );
		FD_SET( m_s, &efds );
		timeval tv;
		tv.tv_sec = iMs / 1000;
		tv.tv_usec = (iMs % 1000) * 1000;
		int r = select( 0, bWrite ? NULL : &fds, bWrite ? &fds : NULL, &efds, &tv );
		if( r == SOCKET_ERROR )
			return -1;
		if( r == 0 )
			return 0;
		if( FD_ISSET( m_s, &efds ) )
			return -1;
		return 1;
	}

	bool SendAll( const void *pData, size_t iLen, std::atomic<unsigned long long> *pProgress )
	{
		const char *p = (const char *)pData;
		DWORD iLastMove = GetTickCount();
		while( iLen > 0 )
		{
			if( m_bCancel ) { m_sError = "cancelled"; return false; }
			if( GetTickCount() - iLastMove > STALL_TIMEOUT_MS ) { m_sError = "transfer stalled"; return false; }
			int r = Wait( true, POLL_MS );
			if( r < 0 ) { m_sError = "connection error"; return false; }
			if( r == 0 )
				continue;
			int iWant = (int)MinU64( iLen, CHUNK_BYTES );
			int n = send( m_s, p, iWant, 0 );
			if( n == SOCKET_ERROR )
			{
				int e = WSAGetLastError();
				if( e == WSAEWOULDBLOCK )
					continue;
				m_sError = ssprintf( "connection lost (%d)", e );
				return false;
			}
			p += n;
			iLen -= n;
			iLastMove = GetTickCount();
			if( pProgress )
				*pProgress += n;
		}
		return true;
	}

	/* Receive exactly iLen bytes; fails when nothing arrives for iIdleTimeoutMs. */
	bool RecvAll( void *pData, size_t iLen, DWORD iIdleTimeoutMs, std::atomic<unsigned long long> *pProgress )
	{
		char *p = (char *)pData;
		DWORD iLastMove = GetTickCount();
		while( iLen > 0 )
		{
			if( m_bCancel ) { m_sError = "cancelled"; return false; }
			if( GetTickCount() - iLastMove > iIdleTimeoutMs ) { m_sError = "transfer stalled"; return false; }
			int r = Wait( false, POLL_MS );
			if( r < 0 ) { m_sError = "connection error"; return false; }
			if( r == 0 )
				continue;
			int iWant = (int)MinU64( iLen, CHUNK_BYTES );
			int n = recv( m_s, p, iWant, 0 );
			if( n == 0 ) { m_sError = "connection closed by the other side"; return false; }
			if( n == SOCKET_ERROR )
			{
				int e = WSAGetLastError();
				if( e == WSAEWOULDBLOCK )
					continue;
				m_sError = ssprintf( "connection lost (%d)", e );
				return false;
			}
			p += n;
			iLen -= n;
			iLastMove = GetTickCount();
			if( pProgress )
				*pProgress += n;
		}
		return true;
	}

	bool SendBuffer( const ByteWriter &w ) { return SendAll( w.buf.data(), w.buf.size(), NULL ); }

	bool RecvU32( unsigned int &v, DWORD iTimeoutMs )
	{
		unsigned char b[4];
		if( !RecvAll( b, 4, iTimeoutMs, NULL ) )
			return false;
		v = ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | b[3];
		return true;
	}

	CString m_sError;

private:
	SOCKET m_s;
	std::atomic<bool> &m_bCancel;
};

bool ResolveIPv4( const CString &sHost, in_addr &out )
{
	unsigned long a = inet_addr( sHost.c_str() );
	if( a != INADDR_NONE )
	{
		out.s_addr = a;
		return true;
	}
	addrinfo hints;
	memset( &hints, 0, sizeof(hints) );
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *pResult = NULL;
	if( getaddrinfo( sHost.c_str(), NULL, &hints, &pResult ) != 0 || pResult == NULL )
		return false;
	out = ((sockaddr_in *)pResult->ai_addr)->sin_addr;
	freeaddrinfo( pResult );
	return true;
}
} // namespace

/* ======================================================================= */

ShareSongTransfer::ShareSongTransfer() :
	m_Role(ROLE_NONE), m_iTransferId(0),
	m_bCancel(false), m_bFinished(false), m_bListening(false), m_bHaveTotal(false),
	m_iListenPort(0), m_iToken(0), m_iDoneBytes(0), m_iTotalBytes(0),
	m_Result(RESULT_NONE)
{
}

ShareSongTransfer::~ShareSongTransfer()
{
	Cancel();
	JoinWorker();
}

bool ShareSongTransfer::IsVideoFile( const CString &sFileName )
{
	static const char *szExt[] = { ".avi", ".mpg", ".mpeg", ".mpe", ".m1v", ".m2v", ".mp4", ".m4v",
		".wmv", ".asf", ".mkv", ".mov", ".flv", ".webm", ".ogv", ".divx", ".vob" };
	for( size_t i = 0; i < sizeof(szExt)/sizeof(szExt[0]); ++i )
		if( EndsWithNoCase( sFileName, szExt[i] ) )
			return true;
	return false;
}

void ShareSongTransfer::JoinWorker()
{
	if( m_Thread.joinable() )
		m_Thread.join();
}

void ShareSongTransfer::Cancel()
{
	m_bCancel = true;
}

void ShareSongTransfer::Finish( Result result, const CString &sMessage )
{
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Result = result;
		m_sMessage = sMessage;
	}
	m_bListening = false;
	m_bFinished = true;
}

CString ShareSongTransfer::GetReceivedFolder()
{
	std::lock_guard<std::mutex> lock( m_Mutex );
	return m_sReceivedFolder;
}

bool ShareSongTransfer::PollFinished( Result &result, CString &sMessage )
{
	if( m_Role == ROLE_NONE || !m_bFinished )
		return false;
	JoinWorker();
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		result = m_Result;
		sMessage = m_sMessage;
	}
	m_Role = ROLE_NONE;
	return true;
}

bool ShareSongTransfer::StartSend( int iTransferId, const CString &sSongDir, bool bIncludeVideo )
{
	if( IsBusy() )
		return false;
	JoinWorker();
	m_Role = ROLE_SENDER;
	m_iTransferId = iTransferId;
	m_bCancel = false;
	m_bFinished = false;
	m_bListening = false;
	m_bHaveTotal = false;
	m_iDoneBytes = 0;
	m_iTotalBytes = 0;
	m_Thread = std::thread( &ShareSongTransfer::SenderThread, this, sSongDir, bIncludeVideo );
	return true;
}

bool ShareSongTransfer::StartReceive( int iTransferId, const vector<CString> &vsHosts, unsigned short iPort,
	unsigned int iToken, const CString &sExistingSongDir, const CString &sConnectDir )
{
	if( IsBusy() )
		return false;
	JoinWorker();
	m_Role = ROLE_RECEIVER;
	m_iTransferId = iTransferId;
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_sReceivedFolder = "";
	}
	m_bCancel = false;
	m_bFinished = false;
	m_bListening = false;
	m_bHaveTotal = false;
	m_iDoneBytes = 0;
	m_iTotalBytes = 0;
	m_Thread = std::thread( &ShareSongTransfer::ReceiverThread, this, vsHosts, iPort, iToken,
		sExistingSongDir, sConnectDir );
	return true;
}

/* ----------------------------------------------------------------------- */

void ShareSongTransfer::SenderThread( CString sSongDir, bool bIncludeVideo )
{
	WSADATA wsa;
	WSAStartup( MAKEWORD(2, 2), &wsa );
	CString sError;

	auto run = [&]() -> Result
	{
		std::string sDir = StripTrailingSlashes( sSongDir );
		size_t iSlash = sDir.find_last_of( "/\\" );
		std::string sFolderName = (iSlash == std::string::npos) ? sDir : sDir.substr( iSlash + 1 );

		std::vector<FileEntry> vFiles;
		ScanDir( sDir, "", 0, bIncludeVideo, vFiles );
		if( vFiles.empty() )
		{
			sError = "no files found in the song folder";
			return RESULT_FAILED;
		}

		/* Listen.  Prefer the old tool's port so existing port forwarding works. */
		Socket listener( m_bCancel );
		listener.Attach( socket( AF_INET, SOCK_STREAM, IPPROTO_TCP ) );
		if( !listener.IsOpen() )
		{
			sError = "cannot create socket";
			return RESULT_FAILED;
		}
		BOOL bExclusive = TRUE;
		setsockopt( listener.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&bExclusive, sizeof(bExclusive) );
		sockaddr_in addr;
		memset( &addr, 0, sizeof(addr) );
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl( INADDR_ANY );
		addr.sin_port = htons( DEFAULT_PORT );
		if( ::bind( listener.Get(), (sockaddr *)&addr, sizeof(addr) ) == SOCKET_ERROR )
		{
			LOG->Warn( "ShareSong: port %d busy, using a random port", DEFAULT_PORT );
			addr.sin_port = 0;
			if( ::bind( listener.Get(), (sockaddr *)&addr, sizeof(addr) ) == SOCKET_ERROR )
			{
				sError = "cannot open a port for the transfer";
				return RESULT_FAILED;
			}
		}
		if( ::listen( listener.Get(), 4 ) == SOCKET_ERROR )
		{
			sError = "cannot listen for the receiver";
			return RESULT_FAILED;
		}
		int iAddrLen = sizeof(addr);
		getsockname( listener.Get(), (sockaddr *)&addr, &iAddrLen );

		std::random_device rd;
		m_iToken = (unsigned int)rd();
		m_iListenPort = ntohs( addr.sin_port );
		m_bListening = true;
		LOG->Info( "ShareSong: sending \"%s\" (%u files), listening on port %u",
			sFolderName.c_str(), (unsigned)vFiles.size(), (unsigned)m_iListenPort );

		/* Wait for the receiver.  Connections with a wrong token are dropped. */
		Socket conn( m_bCancel );
		DWORD iStart = GetTickCount();
		while( !conn.IsOpen() )
		{
			if( m_bCancel )
				return RESULT_CANCELLED;
			if( GetTickCount() - iStart > ACCEPT_TIMEOUT_MS )
			{
				sError = "the receiver never connected (firewall or port forwarding?)";
				return RESULT_FAILED;
			}
			int r = listener.Wait( false, POLL_MS );
			if( r < 0 )
			{
				sError = "listening socket error";
				return RESULT_FAILED;
			}
			if( r == 0 )
				continue;
			SOCKET s = ::accept( listener.Get(), NULL, NULL );
			if( s == INVALID_SOCKET )
				continue;
			conn.Attach( s );

			unsigned int iMagic = 0, iToken = 0;
			unsigned char iId = 0;
			if( conn.RecvU32( iMagic, HANDSHAKE_TIMEOUT_MS ) &&
				conn.RecvU32( iToken, HANDSHAKE_TIMEOUT_MS ) &&
				conn.RecvAll( &iId, 1, HANDSHAKE_TIMEOUT_MS, NULL ) &&
				iMagic == MAGIC_HELLO && iToken == m_iToken && iId == (unsigned char)m_iTransferId )
				break;
			if( m_bCancel )
				return RESULT_CANCELLED;
			LOG->Warn( "ShareSong: dropped a connection with a bad handshake" );
			conn.Close();
		}
		listener.Close();
		m_bListening = false;

		/* Header */
		ByteWriter header;
		header.U32( MAGIC_HEADER );
		header.U16( PROTOCOL_VERSION );
		header.Str( AnsiToUtf8( sFolderName ) );
		header.U32( (unsigned int)vFiles.size() );
		for( size_t i = 0; i < vFiles.size(); ++i )
		{
			header.Str( vFiles[i].sRelUtf8 );
			header.U64( vFiles[i].iSize );
			header.U8( vFiles[i].bVideo ? 1 : 0 );
		}
		ByteWriter len;
		len.U32( (unsigned int)header.buf.size() );
		if( !conn.SendBuffer( len ) || !conn.SendBuffer( header ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}

		/* Which files does the receiver want? */
		unsigned int iMagic = 0, iCount = 0;
		if( !conn.RecvU32( iMagic, STALL_TIMEOUT_MS ) || !conn.RecvU32( iCount, STALL_TIMEOUT_MS ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}
		if( iMagic != MAGIC_NEED || iCount > vFiles.size() )
		{
			sError = "protocol error";
			return RESULT_FAILED;
		}
		std::vector<unsigned int> vNeed;
		unsigned long long iTotal = 0;
		for( unsigned int i = 0; i < iCount; ++i )
		{
			unsigned int iIndex = 0;
			if( !conn.RecvU32( iIndex, STALL_TIMEOUT_MS ) )
			{
				sError = conn.m_sError;
				return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
			}
			if( iIndex >= vFiles.size() || (!vNeed.empty() && iIndex <= vNeed.back()) )
			{
				sError = "protocol error";
				return RESULT_FAILED;
			}
			vNeed.push_back( iIndex );
			iTotal += vFiles[iIndex].iSize;
		}
		m_iTotalBytes = iTotal;
		m_bHaveTotal = true;

		/* File data */
		std::vector<char> buf( CHUNK_BYTES );
		for( size_t i = 0; i < vNeed.size(); ++i )
		{
			const FileEntry &e = vFiles[vNeed[i]];
			FILE *f = fopen( e.sFullAnsi.c_str(), "rb" );
			if( f == NULL )
			{
				sError = ssprintf( "cannot read %s", e.sFullAnsi.c_str() );
				return RESULT_FAILED;
			}
			unsigned long long iLeft = e.iSize;
			while( iLeft > 0 )
			{
				size_t iWant = (size_t)MinU64( iLeft, CHUNK_BYTES );
				size_t iGot = fread( &buf[0], 1, iWant, f );
				if( iGot != iWant )
				{
					fclose( f );
					sError = ssprintf( "read error in %s", e.sFullAnsi.c_str() );
					return RESULT_FAILED;
				}
				if( !conn.SendAll( &buf[0], iGot, &m_iDoneBytes ) )
				{
					fclose( f );
					sError = conn.m_sError;
					return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
				}
				iLeft -= iGot;
			}
			fclose( f );
		}

		/* The receiver confirms once everything is on its disk. */
		unsigned int iDone = 0;
		if( !conn.RecvU32( iDone, FINISH_TIMEOUT_MS ) || iDone != MAGIC_DONE )
		{
			sError = conn.m_sError.empty() ? CString("no confirmation from the receiver") : conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}
		return vNeed.empty() ? RESULT_NOTHING_NEEDED : RESULT_OK;
	};

	Result result = run();
	if( result == RESULT_CANCELLED )
		sError = "cancelled";
	if( result == RESULT_FAILED || result == RESULT_CANCELLED )
		LOG->Warn( "ShareSong: send failed: %s", sError.c_str() );
	else
		LOG->Info( "ShareSong: send finished (%llu bytes)", (unsigned long long)m_iDoneBytes );
	WSACleanup();
	Finish( result, sError );
}

/* ----------------------------------------------------------------------- */

void ShareSongTransfer::ReceiverThread( vector<CString> vsHosts, unsigned short iPort, unsigned int iToken,
	CString sExistingSongDir, CString sConnectDir )
{
	WSADATA wsa;
	WSAStartup( MAKEWORD(2, 2), &wsa );
	CString sError;

	/* Everything created by this transfer, so a failed one leaves nothing behind. */
	std::vector<std::wstring> vCreatedFiles;
	std::vector<std::wstring> vCreatedDirs;
	std::wstring sPartFile;
	FILE *pPartFile = NULL;

	auto run = [&]() -> Result
	{
		/* Connect, trying each address in turn. */
		Socket conn( m_bCancel );
		CString sTried;
		for( size_t i = 0; i < vsHosts.size() && !conn.IsOpen(); ++i )
		{
			if( vsHosts[i].empty() )
				continue;
			if( !sTried.empty() )
				sTried += ", ";
			sTried += vsHosts[i];
			sTried += ssprintf( ":%u", (unsigned)iPort );
			in_addr ip;
			if( !ResolveIPv4( vsHosts[i], ip ) )
				continue;

			conn.Attach( socket( AF_INET, SOCK_STREAM, IPPROTO_TCP ) );
			if( !conn.IsOpen() )
				break;
			sockaddr_in addr;
			memset( &addr, 0, sizeof(addr) );
			addr.sin_family = AF_INET;
			addr.sin_addr = ip;
			addr.sin_port = htons( iPort );
			bool bConnected = false;
			if( ::connect( conn.Get(), (sockaddr *)&addr, sizeof(addr) ) == 0 )
				bConnected = true;
			else if( WSAGetLastError() == WSAEWOULDBLOCK )
			{
				DWORD iStart = GetTickCount();
				while( !m_bCancel && GetTickCount() - iStart < CONNECT_TIMEOUT_MS )
				{
					int r = conn.Wait( true, POLL_MS );
					if( r < 0 )
						break;
					if( r > 0 )
					{
						int iErr = 0;
						int iLen = sizeof(iErr);
						getsockopt( conn.Get(), SOL_SOCKET, SO_ERROR, (char *)&iErr, &iLen );
						bConnected = (iErr == 0);
						break;
					}
				}
			}
			if( m_bCancel )
				return RESULT_CANCELLED;
			if( !bConnected )
			{
				LOG->Warn( "ShareSong: cannot connect to %s:%u", vsHosts[i].c_str(), (unsigned)iPort );
				conn.Close();
			}
		}
		if( !conn.IsOpen() )
		{
			sError = "could not connect to the sender (" + sTried + ")";
			return RESULT_FAILED;
		}

		ByteWriter hello;
		hello.U32( MAGIC_HELLO );
		hello.U32( iToken );
		hello.U8( (unsigned int)m_iTransferId );
		if( !conn.SendBuffer( hello ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}

		/* Header */
		unsigned int iHeaderLen = 0;
		if( !conn.RecvU32( iHeaderLen, HANDSHAKE_TIMEOUT_MS ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}
		if( iHeaderLen == 0 || iHeaderLen > MAX_HEADER_BYTES )
		{
			sError = "protocol error";
			return RESULT_FAILED;
		}
		std::string sHeader( iHeaderLen, '\0' );
		if( !conn.RecvAll( &sHeader[0], iHeaderLen, STALL_TIMEOUT_MS, NULL ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}
		ByteReader rd( sHeader );
		unsigned int iMagic = rd.U32();
		unsigned int iVersion = rd.U16();
		std::string sFolderUtf8 = rd.Str();
		unsigned int iFileCount = rd.U32();
		if( !rd.ok || iMagic != MAGIC_HEADER )
		{
			sError = "protocol error";
			return RESULT_FAILED;
		}
		if( iVersion != PROTOCOL_VERSION )
		{
			sError = "the sender runs a different version of the game";
			return RESULT_FAILED;
		}
		if( iFileCount > MAX_FILES )
		{
			sError = "too many files";
			return RESULT_FAILED;
		}
		std::vector<FileEntry> vFiles( iFileCount );
		for( unsigned int i = 0; i < iFileCount; ++i )
		{
			vFiles[i].sRelUtf8 = rd.Str();
			vFiles[i].iSize = rd.U64();
			vFiles[i].bVideo = rd.U8() != 0;
		}
		if( !rd.ok )
		{
			sError = "protocol error";
			return RESULT_FAILED;
		}

		/* Where does it go? */
		const bool bSupplement = !sExistingSongDir.empty();
		std::wstring sRoot;
		bool bRootExists = false;
		if( bSupplement )
		{
			sRoot = AnsiToWide( StripTrailingSlashes( sExistingSongDir ) );
			bRootExists = PathExistsW( sRoot );
			if( !bRootExists )
			{
				sError = "local song folder not found";
				return RESULT_FAILED;
			}
		}
		else
		{
			std::wstring sConnect = AnsiToWide( StripTrailingSlashes( sConnectDir ) );
			CreateDirectoryW( sConnect.c_str(), NULL );
			std::wstring sName;
			if( !SanitizeComponent( Utf8ToWide( sFolderUtf8 ), sName ) )
				sName = L"shared song";
			sRoot = sConnect + L"\\" + sName;
			for( int i = 2; PathExistsW( sRoot ); ++i )
			{
				if( i > 99 )
				{
					sError = "too many copies of this song in Songs/connect";
					return RESULT_FAILED;
				}
				sRoot = sConnect + L"\\" + sName + L" (" + std::to_wstring( i ) + L")";
			}
		}

		/* Pick the files we want: everything for a new song, or only the missing videos. */
		std::vector<unsigned int> vNeed;
		std::vector<std::vector<std::wstring>> vNeedParts;
		unsigned long long iTotal = 0;
		for( unsigned int i = 0; i < iFileCount; ++i )
		{
			std::vector<std::wstring> vParts;
			if( !SanitizeRelPath( vFiles[i].sRelUtf8, vParts ) )
			{
				LOG->Warn( "ShareSong: skipping unsafe path \"%s\"", vFiles[i].sRelUtf8.c_str() );
				continue;
			}
			if( bSupplement )
			{
				if( !vFiles[i].bVideo )
					continue;
				std::wstring sFull = sRoot;
				for( size_t j = 0; j < vParts.size(); ++j )
					sFull += L"\\" + vParts[j];
				if( PathExistsW( sFull ) )
					continue;
			}
			vNeed.push_back( i );
			vNeedParts.push_back( vParts );
			iTotal += vFiles[i].iSize;
		}
		if( iTotal > MAX_TOTAL_BYTES )
		{
			sError = "the song is too large";
			return RESULT_FAILED;
		}

		ByteWriter need;
		need.U32( MAGIC_NEED );
		need.U32( (unsigned int)vNeed.size() );
		for( size_t i = 0; i < vNeed.size(); ++i )
			need.U32( vNeed[i] );
		if( !conn.SendBuffer( need ) )
		{
			sError = conn.m_sError;
			return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
		}
		m_iTotalBytes = iTotal;
		m_bHaveTotal = true;

		if( !vNeed.empty() && !bRootExists )
		{
			if( !CreateDirectoryW( sRoot.c_str(), NULL ) )
			{
				sError = "cannot create the song folder";
				return RESULT_FAILED;
			}
			vCreatedDirs.push_back( sRoot );
			std::lock_guard<std::mutex> lock( m_Mutex );
			m_sReceivedFolder = WideToAnsi( sRoot.substr( sRoot.find_last_of( L'\\' ) + 1 ) ).c_str();
		}

		/* Receive the data, each file into a .yhpart first. */
		std::vector<char> buf( CHUNK_BYTES );
		for( size_t i = 0; i < vNeed.size(); ++i )
		{
			const FileEntry &e = vFiles[vNeed[i]];
			const std::vector<std::wstring> &vParts = vNeedParts[i];

			std::wstring sPath = sRoot;
			for( size_t j = 0; j + 1 < vParts.size(); ++j )
			{
				sPath += L"\\" + vParts[j];
				if( !PathExistsW( sPath ) )
				{
					if( !CreateDirectoryW( sPath.c_str(), NULL ) )
					{
						sError = CString( ("cannot create folder " + WideToAnsi( sPath )).c_str() );
						return RESULT_FAILED;
					}
					vCreatedDirs.push_back( sPath );
				}
			}
			sPath += L"\\" + vParts.back();

			sPartFile = sPath + PART_SUFFIX_W;
			pPartFile = _wfopen( sPartFile.c_str(), L"wb" );
			if( pPartFile == NULL )
			{
				sPartFile.clear();
				sError = CString( ("cannot write " + WideToAnsi( sPath )).c_str() );
				return RESULT_FAILED;
			}
			unsigned long long iLeft = e.iSize;
			while( iLeft > 0 )
			{
				size_t iWant = (size_t)MinU64( iLeft, CHUNK_BYTES );
				if( !conn.RecvAll( &buf[0], iWant, STALL_TIMEOUT_MS, &m_iDoneBytes ) )
				{
					sError = conn.m_sError;
					return m_bCancel ? RESULT_CANCELLED : RESULT_FAILED;
				}
				if( fwrite( &buf[0], 1, iWant, pPartFile ) != iWant )
				{
					sError = "disk write error (disk full?)";
					return RESULT_FAILED;
				}
				iLeft -= iWant;
			}
			bool bClosedOk = fclose( pPartFile ) == 0;
			pPartFile = NULL;
			if( !bClosedOk || !MoveFileExW( sPartFile.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING ) )
			{
				sError = CString( ("cannot finish " + WideToAnsi( sPath )).c_str() );
				return RESULT_FAILED;
			}
			sPartFile.clear();
			vCreatedFiles.push_back( sPath );
		}

		ByteWriter done;
		done.U32( MAGIC_DONE );
		if( !conn.SendBuffer( done ) )
		{
			/* Everything is on disk already; the sender just won't hear about it. */
			LOG->Warn( "ShareSong: could not send the final ack: %s", conn.m_sError.c_str() );
		}
		LOG->Info( "ShareSong: received %u files into \"%s\"", (unsigned)vNeed.size(), WideToAnsi( sRoot ).c_str() );
		return vNeed.empty() ? RESULT_NOTHING_NEEDED : RESULT_OK;
	};

	Result result = run();

	if( result == RESULT_FAILED || result == RESULT_CANCELLED )
	{
		if( result == RESULT_CANCELLED )
			sError = "cancelled";
		LOG->Warn( "ShareSong: receive failed: %s", sError.c_str() );
		if( pPartFile != NULL )
			fclose( pPartFile );
		if( !sPartFile.empty() )
			DeleteFileW( sPartFile.c_str() );
		for( size_t i = 0; i < vCreatedFiles.size(); ++i )
			DeleteFileW( vCreatedFiles[i].c_str() );
		for( size_t i = vCreatedDirs.size(); i-- > 0; )
			RemoveDirectoryW( vCreatedDirs[i].c_str() );
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_sReceivedFolder = "";
	}
	WSACleanup();
	Finish( result, sError );
}

#endif
