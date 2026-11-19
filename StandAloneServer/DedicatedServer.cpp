#include "global.h"
#include "DedicatedServer.h"
#include "ServerLog.h"

static const int IDLE_WAIT_MS = 20;					// longest wait for network traffic or window commands
static const unsigned long STATUS_INTERVAL_MS = 200;	// how often the window's snapshot is refreshed

static std::string Str( const CString &s ) { return std::string( s.c_str() ); }

DedicatedServer::DedicatedServer() :
	m_bQuit( false ),
	m_iLastPublish( 0 ),
	m_bStatusDirty( true ),
	m_iNextClientId( 1 ),
	m_bWasInGame( false )
{
}

DedicatedServer::~DedicatedServer()
{
	Stop();
}

bool DedicatedServer::Start( const std::string &sServerName, std::string &sError )
{
	if( m_Thread.joinable() )
		return true;

	servername = sServerName.c_str();
	if( !ServerStart() )
	{
		const int iWsaError = WSAGetLastError();
		ServerStop();
		if( iWsaError == WSAEADDRINUSE || iWsaError == WSAEACCES )
			sError = "TCP 8765 已被佔用。可能已經有另一個伺服器在執行"
				"（遊戲裡的 Start Server，或另一個 StandAloneServer）。";
		else
			sError = Str( lastError ) + ssprintf( "（錯誤碼 %d）", iWsaError ).c_str();
		return false;
	}

	ServerLog::Write( "伺服器已啟動：" + sServerName + "，TCP 8765" );
	m_bQuit = false;
	m_bStatusDirty = true;
	PublishStatus();
	m_Thread = std::thread( &DedicatedServer::ThreadMain, this );
	return true;
}

void DedicatedServer::Stop()
{
	if( !m_Thread.joinable() )
		return;
	m_bQuit = true;
	m_Thread.join();

	ServerStop();	// closes every connection
	m_ClientIds.clear();
	{
		std::lock_guard<std::mutex> lock( m_CommandMutex );
		m_Commands.clear();
	}
	{
		std::lock_guard<std::mutex> lock( m_StatusMutex );
		m_Status = DedicatedStatus();
	}
	ServerLog::Write( "伺服器已停止" );
}

void DedicatedServer::Post( CommandType type, unsigned int clientId, const std::string &sText )
{
	Command cmd;
	cmd.type = type;
	cmd.clientId = clientId;
	cmd.text = sText;
	std::lock_guard<std::mutex> lock( m_CommandMutex );
	m_Commands.push_back( cmd );
}

DedicatedStatus DedicatedServer::GetStatus()
{
	std::lock_guard<std::mutex> lock( m_StatusMutex );
	return m_Status;
}

/* ---------------- server thread ---------------- */

void DedicatedServer::ThreadMain()
{
	while( !m_bQuit )
	{
		WaitForNetwork( IDLE_WAIT_MS );
		RunCommands();
		ServerUpdate();		// the game's own room logic
		FlushPendingSends();
		PublishStatus();
	}

	if( !Client.empty() )
	{
		ServerChat( "The server is shutting down." );
		FlushPendingSends();
	}
}

/* Sleep until a socket has something for us (or the timeout passes), so the
 * server reacts at once but uses no CPU while the room is quiet. */
void DedicatedServer::WaitForNetwork( int iTimeoutMs )
{
	fd_set readSet, writeSet;
	FD_ZERO( &readSet );
	FD_ZERO( &writeSet );
	FD_SET( (SOCKET)server.GetSocketHandle(), &readSet );

	bool bPacketBuffered = false;
	for( unsigned int x = 0; x < Client.size() && readSet.fd_count < FD_SETSIZE; ++x )
	{
		EzSockets &sock = Client[x]->clientSocket;
		const SOCKET s = (SOCKET)sock.GetSocketHandle();
		FD_SET( s, &readSet );
		if( !sock.outBuffer.empty() && writeSet.fd_count < FD_SETSIZE )
			FD_SET( s, &writeSet );

		/* ezsockets reads in 1 KB chunks: whole packets can already sit in its buffer. */
		if( sock.inBuffer.size() >= 4 )
		{
			uint32_t iSize;
			memcpy( &iSize, sock.inBuffer.data(), 4 );
			if( sock.inBuffer.size() >= ntohl( iSize ) + 4 )
				bPacketBuffered = true;
		}
	}

	timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = bPacketBuffered ? 0 : iTimeoutMs * 1000;
	select( 0, &readSet, writeSet.fd_count ? &writeSet : NULL, NULL, &tv );
}

/* ezsockets only sends what didn't fit into the socket the next time
 * something is sent to the same client; push it out as soon as possible. */
void DedicatedServer::FlushPendingSends()
{
	for( unsigned int x = 0; x < Client.size(); ++x )
		if( !Client[x]->clientSocket.outBuffer.empty() )
			Client[x]->clientSocket.update();
}

void DedicatedServer::RunCommands()
{
	std::deque<Command> commands;
	{
		std::lock_guard<std::mutex> lock( m_CommandMutex );
		commands.swap( m_Commands );
	}
	for( unsigned int i = 0; i < commands.size(); ++i )
	{
		RunCommand( commands[i] );
		m_bStatusDirty = true;
	}
}

void DedicatedServer::RunCommand( const Command &cmd )
{
	switch( cmd.type )
	{
	case CMD_CHAT:
		if( !cmd.text.empty() )
			ServerChat( cmd.text.c_str() );
		break;

	case CMD_SET_NAME:
		if( !cmd.text.empty() && Str( servername ) != cmd.text )
		{
			servername = cmd.text.c_str();
			ServerLog::Write( "伺服器名稱改為：" + cmd.text );
		}
		break;

	case CMD_KICK:
	case CMD_BAN:
		{
			const int x = FindClient( cmd.clientId );
			if( x < 0 )
				break;
			const CString sName = ClientDisplayName( x );
			if( cmd.type == CMD_BAN )
			{
				CString sIp = Client[x]->clientSocket.address;
				if( !sIp.empty() && !IsBanned( sIp ) )
					bannedIPs.push_back( sIp );
				ServerChat( "Banned " + sName + "." );
			}
			else
			{
				ServerChat( "Kicked " + sName + "." );
			}
			Disconnect( x );
		}
		break;

	case CMD_MAKE_HOST:
		{
			const int x = FindClient( cmd.clientId );
			if( x <= 0 )
				break;	// gone, or already the host
			const CString sName = ClientDisplayName( x );
			ChangeHost = true;		// same as the host's /host command
			ClientSort( x );
			ServerChat( "Host changed to " + sName + "." );
		}
		break;

	case CMD_FORCE_START:
		if( Client.empty() )
			break;
		ServerLog::Write( "伺服器強制開始（等同房主的 /start）" );
		ForceStart();
		break;

	case CMD_CANCEL_SHARES:
		if( m_Share.active || !m_ShareQueue.empty() )
			CancelAllShares( "Song transfer cancelled by the server." );
		break;

	case CMD_CLEAR_BANS:
		if( !bannedIPs.empty() )
		{
			ServerLog::Write( ssprintf( "已解除 %u 個 IP 的封鎖", (unsigned int)bannedIPs.size() ).c_str() );
			bannedIPs.clear();
		}
		break;
	}
}

int DedicatedServer::FindClient( unsigned int clientId ) const
{
	for( unsigned int x = 0; x < Client.size(); ++x )
		if( IdOf( x ) == clientId )
			return (int)x;
	return -1;
}

unsigned int DedicatedServer::IdOf( int clientNum ) const
{
	if( clientNum < 0 || clientNum >= (int)Client.size() )
		return 0;
	std::map<const GameClient *, unsigned int>::const_iterator it = m_ClientIds.find( Client[clientNum] );
	return it == m_ClientIds.end() ? 0 : it->second;
}

void DedicatedServer::OnClientConnected( unsigned int clientNum )
{
	m_ClientIds[Client[clientNum]] = m_iNextClientId++;
	ServerLog::Write( ssprintf( "新連線：%s（目前 %u 人）",
		Client[clientNum]->clientSocket.address.c_str(), (unsigned int)Client.size() ).c_str() );
	m_bStatusDirty = true;
}

void DedicatedServer::OnClientDisconnecting( unsigned int clientNum )
{
	GameClient *pClient = Client[clientNum];
	CString sWho = ClientDisplayName( clientNum );
	ServerLog::Write( ssprintf( "離線：%s（%s）%s", sWho.c_str(), pClient->clientSocket.address.c_str(),
		clientNum == 0 ? "，原本是房主" : "" ).c_str() );
	m_ClientIds.erase( pClient );
	m_bStatusDirty = true;
}

void DedicatedServer::OnChatLine( const CString &sLine )
{
	ServerLog::Write( Str( sLine ) );
}

void DedicatedServer::PublishStatus()
{
	const unsigned long iNow = GetTickCount();
	if( !m_bStatusDirty && iNow - m_iLastPublish < STATUS_INTERVAL_MS )
		return;
	m_bStatusDirty = false;
	m_iLastPublish = iNow;

	DedicatedStatus st;
	st.running = true;
	bool bAnyInGame = false;
	for( unsigned int x = 0; x < Client.size(); ++x )
	{
		const GameClient *pClient = Client[x];
		DedicatedClientInfo info;
		info.id = IdOf( x );
		info.index = x;
		info.names = Str( pClient->Player[0].name );
		if( !pClient->Player[1].name.empty() )
			info.names += (info.names.empty() ? "" : "&") + Str( pClient->Player[1].name );
		info.ip = Str( pClient->clientSocket.address );
		info.inSongSelect = pClient->inNetMusicSelect;
		info.inGame = pClient->InGame;
		info.starting = pClient->isStarting;
		info.hasSong = pClient->hasSong || pClient->forceHas;
		info.sending = m_Share.active && m_Share.sender == (int)x;
		info.receiving = m_Share.active && m_Share.receiver == (int)x;
		bAnyInGame = bAnyInGame || pClient->InGame;
		st.clients.push_back( info );
	}

	st.songTitle = Str( CurrentSongInfo.title );
	st.songSubtitle = Str( CurrentSongInfo.subtitle );
	st.songArtist = Str( CurrentSongInfo.artist );
	st.shareActive = m_Share.active;
	if( m_Share.active )
	{
		st.shareSenderId = IdOf( m_Share.sender );
		st.shareReceiverId = IdOf( m_Share.receiver );
		st.shareDoneKB = m_Share.doneKB;
		st.shareTotalKB = m_Share.totalKB;
	}
	st.shareQueued = (unsigned int)m_ShareQueue.size();
	st.bannedCount = (unsigned int)bannedIPs.size();

	if( bAnyInGame != m_bWasInGame )
	{
		m_bWasInGame = bAnyInGame;
		if( bAnyInGame )
			ServerLog::Write( "開始遊戲：" + (st.songTitle.empty() ? std::string( "（課程或未知歌曲）" ) : st.songTitle) );
		else
			ServerLog::Write( "所有玩家都打完了" );
	}

	std::lock_guard<std::mutex> lock( m_StatusMutex );
	m_Status = std::move( st );
}
