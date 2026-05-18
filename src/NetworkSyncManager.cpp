#include "global.h"
#include "NetworkSyncManager.h"
#include "NetworkSyncServer.h"
#include "LuaFunctions.h"

NetworkSyncManager *NSMAN;

#if defined(WITHOUT_NETWORKING)
NetworkSyncManager::NetworkSyncManager( LoadingWindow *ld ) { useSMserver=false; }
NetworkSyncManager::~NetworkSyncManager () { }
void NetworkSyncManager::CloseConnection() { }
void NetworkSyncManager::PostStartUp(const CString& ServerIP ) { }
bool NetworkSyncManager::Connect(const CString& addy, unsigned short port) { return false; }
void NetworkSyncManager::ReportNSSOnOff(int i) { }
void NetworkSyncManager::ReportTiming(float offset, int PlayerNumber) { }
void NetworkSyncManager::ReportScore(int playerID, int step, int score, int combo) { }
void NetworkSyncManager::ReportPercentage() { }
void NetworkSyncManager::ReportSongOver() { }
void NetworkSyncManager::ReportStyle() {}
void NetworkSyncManager::StartRequest(short position) { }
void NetworkSyncManager::DisplayStartupStatus() { }
void NetworkSyncManager::Update( float fDeltaTime ) { }
bool NetworkSyncManager::ChangedScoreboard(int Column) { return false; }
void NetworkSyncManager::SendChat(const CString& message) { }
void NetworkSyncManager::SelectUserSong() { }
void NetworkSyncManager::CancelShareSong() { }
void NetworkSyncManager::SendShareCancel() { }
bool NetworkSyncManager::IsShareSongActive() const { return false; }
#else
#include "ezsockets.h"
#include "ProfileManager.h"
#include "RageLog.h"
#include "StepMania.h"
#include "ScreenManager.h"
#include "Screen.h"
#include "song.h"
#include "Course.h"
#include "GameState.h"
#include "StageStats.h"
#include "Steps.h"
#include "PrefsManager.h"
#include "ProductInfo.h"
#include "ScreenMessage.h"
#include "GameManager.h"
#include "arch/LoadingWindow/LoadingWindow.h"
#include "steam/steam_api.h"
#include "steam/steamnetworkingtypes.h"
#include "ShareZipUtil.h" // zip + temp.sh helper
HANDLE g_hMutex = NULL;
const ScreenMessage	SM_AddToChat	= ScreenMessage(SM_User+4);
const ScreenMessage SM_ChangeSong	= ScreenMessage(SM_User+5);
const ScreenMessage SM_GotEval		= ScreenMessage(SM_User+6);
const ScreenMessage SM_ReloadConnectPack	        = ScreenMessage(SM_User+9);
// const ScreenMessage SM_BackFromReloadSongs			= ScreenMessage(SM_User+7);

bool IsPathExists(const std::string &path)
{
	struct stat buffer;
	return (stat(path.c_str(), &buffer) == 0);
}

unsigned long GetFileSizeInKB(CString path)
{
	if(!IsPathExists(path)) return 0;
	FILE *fp = fopen(path, "rb");

	unsigned long currentPosition = ftell(fp); // Save the current file pointer position
	unsigned long fileSize = 0;

	fseek(fp, 0L, SEEK_END);			  // Move the file pointer to the end of the file
	fileSize = ftell(fp);				  // Get file size
	fseek(fp, currentPosition, SEEK_SET); // Restore file pointer position

	fclose(fp);
	return fileSize / 1024; // Return file size (KB)
}

NetworkSyncManager::NetworkSyncManager( LoadingWindow *ld )
{
	// [FIX 多執行緒撞 outBuffer] g_hMutex 原本只有宣告沒 CreateMutex，
	// 結果 WaitForSingleObject(NULL,...) 一律 WAIT_FAILED，等同沒鎖。
	// 在這裡建立真正的 mutex，且要在 StartUp() 之前 (StartUp -> PostStartUp 會 SendPack)。
	if (g_hMutex == NULL)
		g_hMutex = CreateMutex(NULL, FALSE, NULL);

	usingShareSongSystem=false;
	ClientNum=0;
	m_shareCancelRequested = false;
	m_shareSentBytes = 0;
	// 新流程：zip+temp.sh 上傳結果快取，剛起來時通通是空的
	m_cachedShareSongDir = "";
	m_cachedShareFolderName = "";
	m_cachedShareUrl = "";
	m_cachedSharePassword = "";
	m_cachedShareZipBytes = 0;
	m_downloadThreadRunning = false;
	m_shareTotalBytes = 0;
	m_shareReceiverIndex = -1;
	ResetRecvState();
	LANserver = NULL;	//So we know if it has been created yet
	if( GetCommandlineArgument( "runserver" ))
	{
		ld->SetText("Initilizing server...");
		LANserver = new StepManiaLanServer;
		isLanServer = true;
		GetCommandlineArgument( "runserver", &LANserver->servername );
	}
	else
		isLanServer = false;
	
	ld->SetText("Initilizing Client Network...");
	NetPlayerClient = new EzSockets;
	NetPlayerClient->blocking = false;
	m_ServerVersion = 0;

	useSMserver = false;
	m_startupStatus = 0;	//By default, connection not tried.

	m_ActivePlayers = 0;

	StartUp();
}

NetworkSyncManager::~NetworkSyncManager ()
{
	//Close Connection to server nicely.
	if (useSMserver)
		NetPlayerClient->close();
	delete NetPlayerClient;

	if( isLanServer )
	{
		LANserver->ServerStop();
		delete LANserver;
	}

	if (g_hMutex != NULL)
	{
		CloseHandle(g_hMutex);
		g_hMutex = NULL;
	}
}

void NetworkSyncManager::CloseConnection()
{
	if (!useSMserver)
		return ;
	m_ServerVersion = 0;
	useSMserver = false;
	m_startupStatus = 0;
	NetPlayerClient->close();
}

void NetworkSyncManager::PostStartUp(const CString& ServerIP)
{
	LOG->Info("[NETDBG] NSM::PostStartUp#1 begin ServerIP='%s'", ServerIP.c_str());
	CloseConnection();
	LOG->Info("[NETDBG] NSM::PostStartUp#2 CloseConnection done");
	// if( ServerIP!="LISTEN" )
	// {
	// 	if( !Connect(ServerIP.c_str(), 8765) )
	// 	{
	// 		m_startupStatus = 2;
	// 		LOG->Warn( "Network Sync Manager failed to connect" );
	// 		return;
	// 	}

	// }
	// else
	// {
	// 	if( !Listen(8765) )
	// 	{
	// 		m_startupStatus = 2;
	// 		LOG->Warn( "Listen() failed" );
	// 		return;
	// 	}
	// }
	if(!Connect(ServerIP.c_str()))
	{
		LOG->Warn("[NETDBG] NSM::PostStartUp#3 Connect() failed");
		m_startupStatus = 2;
		return;
	}
	LOG->Info("[NETDBG] NSM::PostStartUp#4 Connect() ok, proceeding to handshake");

	useSMserver = true;

	m_startupStatus = 1;	//Connection attepmpt sucessful

	// If network play is desired and the connection works,
	// halt until we know what server version we're dealing with

	m_packet.ClearPacket();

	m_packet.Write1( NSCHello );	//Hello Packet

	m_packet.Write1(NETPROTOCOLVERSION);

	m_packet.WriteNT(CString(PRODUCT_NAME_VER)); 

	//Block until responce is received
	//Move mode to blocking in order to give CPU back to the 
	//system, and not wait.
	
	bool dontExit = true;

	// [NETDBG] Steam mode 下不能 blocking：PeekPack 的 spin loop 不會 pump Steam callbacks，
	// 也不會收到資料，會永遠卡死。改用 non-blocking + 外層 while 主動 pump。
	const bool steamMode = SteamReady();
	if( isLanServer || steamMode )
		NetPlayerClient->blocking = false;
	else
		NetPlayerClient->blocking = true;
	LOG->Info("[NETDBG] NSM::PostStartUp#5a blocking=%d steamMode=%d isLanServer=%d",
		(int)NetPlayerClient->blocking, (int)steamMode, (int)isLanServer);

	//Following packet must get through, so we block for it.
	//If we are serving we do not block for this.
	LOG->Info("[NETDBG] NSM::PostStartUp#5 sending NSCHello (isLanServer=%d)", (int)isLanServer);
	SendNSMPacket(m_packet);
	LOG->Info("[NETDBG] NSM::PostStartUp#6 NSCHello sent");

	//If we are serving, do this so we properly connect
	//to the server.
	if( isLanServer )
	{
		LOG->Info("[NETDBG] NSM::PostStartUp#7 server-side ServerUpdate() pump");
		LANserver->ServerUpdate();
	}

	m_packet.ClearPacket();

	int handshakeMs = 0;
	const int handshakeTimeoutMs = 15000;
	int handshakeLoops = 0;
	while (dontExit && handshakeMs < handshakeTimeoutMs)
	{
		if (isLanServer)
			LANserver->ServerUpdate();

		// [NETDBG] Steam mode 下必須在這裡主動 pump，否則收不到任何資料
		if (steamMode)
		{
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
		}

		m_packet.ClearPacket();
		int nRead = NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE);
		if (nRead < 1)
		{
			// [NETDBG] Steam mode client：沒讀到資料不能立刻退出，要繼續 poll 等 server 回應
			if (!isLanServer && !steamMode)
				dontExit = false;
		}
		else
		{
			int cmd = m_packet.Read1();
			LOG->Info("[NETDBG] NSM::PostStartUp#8 handshake got nRead=%d cmd=%d (expect %d)",
				nRead, cmd, NSServerOffset + NSCHello);
			if (cmd == (NSServerOffset + NSCHello))
				dontExit = false;
		}
		//Only allow passing on handshake. 
		//Otherwise scoreboard updates and such will confuse us.

		// [NETDBG] 不論哪種模式都 Sleep 一下，避免吃滿 CPU
		Sleep(10);
		handshakeMs += 10;
		if ((++handshakeLoops % 200) == 0)
			LOG->Info("[NETDBG] NSM::PostStartUp#9 handshake waiting %dms", handshakeMs);
	}

	NetPlayerClient->blocking = false;

	if (dontExit)
	{
		LOG->Warn("[NETDBG] NSM::PostStartUp#10 Network handshake timed out (%dms).", handshakeMs);
		m_startupStatus = 2;
		useSMserver = false;
		return;
	}

	m_ServerVersion = m_packet.Read1();
	m_ServerName = m_packet.ReadNT();

	LOG->Info("[NETDBG] NSM::PostStartUp#11 Server Version: %d name=%s", m_ServerVersion, m_ServerName.c_str());
}

void NetworkSyncManager::StartUp()
{
	CString ServerIP;

	if( isLanServer )
		if (!LANserver->ServerStart())
		{
			//If the server happens to not start when told,
			//Print to log and release the memory where the
			//server was held.
			isLanServer = false;
			LOG->Warn("Server failed to start.");
			delete LANserver;
		}

	if( GetCommandlineArgument( "netip", &ServerIP ) )
		PostStartUp(ServerIP);
	else if( GetCommandlineArgument( "listen" ) )
		PostStartUp("LISTEN");
}

bool NetworkSyncManager::Connect(const CString& addy, unsigned short port)
{
	LOG->Info("Beginning to connect");
	if (port != 8765) 
		return false;
	//Make sure using port 8765
	//This may change in future versions
	//It is this way now for protocol's purpose.
	//If there is a new protocol developed down the road

	NetPlayerClient->create(); // Initilize Socket
	useSMserver = NetPlayerClient->connect(addy, port);

	m_packet.fromIp = NetPlayerClient->getIp();

	return useSMserver;
}

bool NetworkSyncManager::Connect(const CString& roomCode)
{
	CString code = roomCode;
	TrimLeft(code);
	TrimRight(code);
	LOG->Info("[NETDBG] NSM::Connect#1 begin (address='%s' isLanServer=%d LANserver=%p SteamReady=%d)",
		code.c_str(), (int)isLanServer, (void*)LANserver, (int)SteamReady());

	if (isLanServer && LANserver != nullptr && SteamReady())
	{
		const CSteamID lobbyId = LANserver->GetLobbyId();
		LOG->Info("[NETDBG] NSM::Connect#2 lobbyId valid=%d id=%llu roomCode=%s",
			(int)lobbyId.IsValid(),
			lobbyId.ConvertToUint64(),
			LANserver->roomCode.c_str());
		if (lobbyId.IsValid())
		{
			const bool localConnect =
				code.empty() ||
				code == "127.0.0.1" ||
				code.CompareNoCase("localhost") == 0 ||
				code == LANserver->roomCode;
			LOG->Info("[NETDBG] NSM::Connect#3 localConnect=%d", (int)localConnect);
			if (localConnect)
			{
				LOG->Info("[NETDBG] NSM::Connect#4 self-host attachToLobby start");
				useSMserver = NetPlayerClient->attachToLobby(lobbyId);
				LOG->Info("[NETDBG] NSM::Connect#5 self-host attachToLobby done useSMserver=%d", (int)useSMserver);
				return useSMserver;
			}
		}
	}

	LOG->Info("[NETDBG] NSM::Connect#6 client connect(roomCode) start");
	useSMserver = NetPlayerClient->connect(std::string(code));
	LOG->Info("[NETDBG] NSM::Connect#7 client connect(roomCode) done useSMserver=%d", (int)useSMserver);
	if (!useSMserver)
		LOG->Warn("[NETDBG] NSM::Connect#8 Steam connect failed for room code '%s'.", code.c_str());
	return useSMserver;
}

//Listen (Wait for connection in-bound)
//NOTE: Right now, StepMania cannot connect back to StepMania!
bool NetworkSyncManager::Listen(unsigned short port)
{
	LOG->Info("Beginning to Listen");
	if (port != 8765) 
		return false;
	//Make sure using port 8765
	//This may change in future versions
	//It is this way now for protocol's purpose.
	//If there is a new protocol developed down the road


	EzSockets * EZListener = new EzSockets;

	EZListener->create();
	NetPlayerClient->create(); // Initilize Socket

	EZListener->bind(8765);

	useSMserver = EZListener->listen();
	useSMserver = EZListener->accept( *NetPlayerClient );  //Wait for someone to connect

	EZListener->close();	//Kill Listener
	delete EZListener;

	//LOG->Info("Accept Responce: ",useSMserver);
	useSMserver=true;
	return useSMserver;
}

void NetworkSyncManager::ReportNSSOnOff(int i) 
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCSMS );
	m_packet.Write1( (uint8_t) i );
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::ReportTiming(float offset, int PlayerNumber)
{
	m_lastOffset[PlayerNumber] = offset;
}

void NetworkSyncManager::ReportScore(int playerID, int step, int score, int combo)
{
	if (!useSMserver) //Make sure that we are using the network
		return;
	
	m_packet.ClearPacket();

	m_packet.Write1( NSCGSU );
	uint8_t ctr = (uint8_t) (playerID * 16 + step - 1);
	m_packet.Write1(ctr);

	ctr = uint8_t( g_CurStageStats.GetGrade((PlayerNumber)playerID)*16 );

	if ( g_CurStageStats.bFailedEarlier[(PlayerNumber)playerID] )
		ctr = uint8_t( 112 );	//Code for failed (failed constant seems not to work)

	m_packet.Write1(ctr);

	m_packet.Write4(score);

	m_packet.Write2((uint16_t) combo);

	m_packet.Write2((uint16_t) m_playerLife[playerID]);

	//Offset Info
	//Note: if a 0 is sent, then disregard data.
	//
	//ASSUMED: No step will be more than 16 seconds off center
	//If assumption false: read 16 seconds either direction
	int iOffset = int((m_lastOffset[playerID]+16.384)*2000.0);

	iOffset = (iOffset > 65535) ? 65535 : ((iOffset < 1) ? 1 : iOffset);

	//Report 0 if hold, or miss (don't forget mines should report)
	if (((step<TNS_BOO)||(step>TNS_MARVELOUS))&&(step!=TNS_HIT_MINE))
		iOffset = 0;

	m_packet.Write2((uint16_t) iOffset);

	SendNSMPacket(m_packet);

}

void NetworkSyncManager::ReportSongOver() 
{
	if (!useSMserver)	//Make sure that we are using the network
		return ;

	m_packet.ClearPacket();

	m_packet.Write1( NSCGON );

	SendNSMPacket(m_packet);
	return;
}

void NetworkSyncManager::ReportStyle() 
{
	if (!useSMserver)
		return;
	m_packet.ClearPacket();
	m_packet.Write1( NSCSU );
	m_packet.Write1( (int8_t) GAMESTATE->GetNumPlayersEnabled() );

	FOREACH_EnabledPlayer( pn ) 
	{
		m_packet.Write1((uint8_t) pn );
		m_packet.WriteNT(GAMESTATE->GetPlayerDisplayName(pn) );
	}

	SendNSMPacket(m_packet);
}

void NetworkSyncManager::StartRequest(short position) 
{
	if( !useSMserver )
		return;

	if( GAMESTATE->m_bDemonstrationOrJukebox )
		return;

	LOG->Trace("Requesting Start from Server.");

	m_packet.ClearPacket();

	m_packet.Write1( NSCGSR );

	unsigned char ctr=0;

	Steps * tSteps;
	tSteps = GAMESTATE->m_pCurSteps[PLAYER_1];
	if ((tSteps!=NULL) && (GAMESTATE->IsPlayerEnabled(PLAYER_1)))
	{
		int tmp = tSteps->GetMeter();
		if(tmp>0 && tmp%16==0)tmp = 1;
		ctr = uint8_t(ctr+tmp*16);
	}
		
	tSteps = GAMESTATE->m_pCurSteps[PLAYER_2];
	if ((tSteps!=NULL) && (GAMESTATE->IsPlayerEnabled(PLAYER_2)))
	{
		int tmp = tSteps->GetMeter();
		if(tmp>0 && tmp%16==0)tmp = 1;
		ctr = uint8_t(ctr+tmp);
	}
		
	m_packet.Write1(ctr);

	ctr=0;

	tSteps = GAMESTATE->m_pCurSteps[PLAYER_1];
	if ((tSteps!=NULL) && (GAMESTATE->IsPlayerEnabled(PLAYER_1)))
		ctr = uint8_t(ctr + (int) tSteps->GetDifficulty()*16);

	tSteps = GAMESTATE->m_pCurSteps[PLAYER_2];
	if ((tSteps!=NULL) && (GAMESTATE->IsPlayerEnabled(PLAYER_2)))
		ctr = uint8_t(ctr + (int) tSteps->GetDifficulty());

	m_packet.Write1(ctr);
	
	//Notify server if this is for sync or not.
	ctr = char(position*16);
	m_packet.Write1(ctr);

	if (GAMESTATE->m_pCurSong != NULL)
	{
		m_packet.WriteNT(GAMESTATE->m_pCurSong->m_sMainTitle);
		m_packet.WriteNT(GAMESTATE->m_pCurSong->m_sSubTitle);
		m_packet.WriteNT(GAMESTATE->m_pCurSong->m_sArtist);
	}
	else
	{
		m_packet.WriteNT("");
		m_packet.WriteNT("");
		m_packet.WriteNT("");
	}

	if (GAMESTATE->m_pCurCourse != NULL)
		m_packet.WriteNT(GAMESTATE->m_pCurCourse->GetFullDisplayTitle());
	else
		m_packet.WriteNT(CString(""));

	//Send Player (and song) Options
	m_packet.WriteNT(GAMESTATE->m_SongOptions.GetString());

	int players=0;
	FOREACH_PlayerNumber (p)
	{
		++players;
		m_packet.WriteNT(GAMESTATE->m_PlayerOptions[p].GetString());
	}
	for (int i=0; i<2-players; ++i)
		m_packet.WriteNT("");	//Write a NULL if no player

	//This needs to be reset before ScreenEvaluation could possibly be called
	for (int i=0; i<NETMAXPLAYERS; ++i)
	{
		m_EvalPlayerData[i].name=0;
		m_EvalPlayerData[i].grade=0;
		m_EvalPlayerData[i].score=0;
		m_EvalPlayerData[i].difficulty=(Difficulty)0;
		for (int j=0; j<NETNUMTAPSCORES; ++j)
			m_EvalPlayerData[i].tapScores[j] = 0;
	}

	//Block until go is recieved.
	//Switch to blocking mode (this is the only
	//way I know how to get precievably instantanious results

	bool dontExit=true;

	// [NETDBG] BUG FIX：Steam mode 下 client (isLanServer=0) 不能 blocking=true
	// 否則 ReadPack 內部 PeekPack/CanRead 永遠拿不到資料就 hang，原因跟 PostStartUp
	// 一樣：Steam SDK 收到 data 後需要 ReceiveMessagesOnConnection 來拉，這條已經 OK；
	// 但底層 PeekPack 在 blocking 模式下不會自動 pump，所以改用 non-blocking + 外層
	// while 主動 pump SteamAPI_RunCallbacks() / Sleep 等。
	const bool steamMode = SteamReady();

	if (isLanServer || steamMode)
		NetPlayerClient->blocking = false;
	else
		NetPlayerClient->blocking = true;
	LOG->Info("[NETDBG] NSM::StartRequest blocking=%d steamMode=%d isLanServer=%d",
		(int)NetPlayerClient->blocking, (int)steamMode, (int)isLanServer);

	//The following packet HAS to get through, so we turn blocking on for it as well
	//Don't block if we are serving
	SendNSMPacket(m_packet); 
	
	LOG->Trace("Waiting for RECV");

	m_packet.ClearPacket();

	// [NETDBG] 加一個逾時，避免任何情況下永久 hang
	const int waitTimeoutMs = 30000;
	int waitedMs = 0;
	while (dontExit)
	{
		//Keep the server going during the loop.
		if (isLanServer)
			LANserver->ServerUpdate();

		// [NETDBG] Steam mode 必須主動 pump callbacks，否則新連線 / ClosedByPeer / 訊息
		// 都不會被處理，這條對 client (純收 reply) 與 host (處理 remote client) 都需要
		if (steamMode)
		{
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
		}

		m_packet.ClearPacket();
		int got = NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE);
		if (got < 1)
		{
			// 沒收到任何東西。Steam mode 不立即離開（必須等 reply），改睡一下繼續等
			if (!isLanServer && !steamMode)
				dontExit = false; // 舊行為：非 steam 的 client 沒資料就退出
			else
			{
				Sleep(10);
				waitedMs += 10;
				if (waitedMs >= waitTimeoutMs)
				{
					LOG->Warn("[NETDBG] NSM::StartRequest timeout %dms waiting NSCGSR reply", waitTimeoutMs);
					break;
				}
				continue; // 重新從 loop 頭開始
			}
		}
		else
		{
			waitedMs = 0; // 收到資料就重置 timeout
		}

		if (m_packet.Read1() == (NSServerOffset + NSCGSR))
			dontExit=false;
		//Only allow passing on Start request. 
		//Otherwise scoreboard updates and such will confuse us.
	}
	NetPlayerClient->blocking = false;
}

void NetworkSyncManager::DisplayStartupStatus()
{
	CString sMessage("");

	switch (m_startupStatus)
	{
	case 0:
		//Networking wasn't attepmpted
		return;
	case 1:
		sMessage = "Connection to " + m_ServerName + " sucessful.";
		break;
	case 2:
		sMessage = "Connection failed.";
		break;
	}
	SCREENMAN->SystemMessage(sMessage);
}

void NetworkSyncManager::Update(float fDeltaTime)
{
	static int s_updateTick = 0;
	const bool logThisTick = ((s_updateTick++ % 600) == 0); // ~1 log per 10s @60fps
	if (logThisTick)
		LOG->Info("[NETDBG] NSM::Update#1 tick=%d isLanServer=%d useSMserver=%d SteamReady=%d",
			s_updateTick, (int)isLanServer, (int)useSMserver, (int)SteamReady());

	if (isLanServer)
	{
		if (logThisTick) LOG->Info("[NETDBG] NSM::Update#2 enter ServerUpdate()");
		LANserver->ServerUpdate();
		if (logThisTick) LOG->Info("[NETDBG] NSM::Update#3 leave ServerUpdate()");
	}

	if (useSMserver)
	{
		if (logThisTick) LOG->Info("[NETDBG] NSM::Update#4 enter ProcessInput()");
		ProcessInput();
		if (logThisTick) LOG->Info("[NETDBG] NSM::Update#5 leave ProcessInput()");
	}

	if(SteamReady())
	{
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
	}
}

CString GetSongDirPath(std::string &songDir,
					   std::string &additionalSongFolders)
{
	const int maxBufferSize = 512;
	string currentPath;
	char buf[maxBufferSize];
	getcwd(buf, sizeof(buf));
	currentPath.assign(buf);
	CString path = currentPath + "/" + songDir;
	replace(path.begin(), path.end(), '/', '\\');
	if (!IsPathExists(path))
	{
		// Find the position of the first slash
		size_t found = songDir.find('/');
		
		// Determine whether slash is found
		if (found != std::string::npos) {
			// Remove the slash and the string before it
			songDir.erase(0, found + 1);
		}
		path = additionalSongFolders + "/" + songDir;
		replace(path.begin(), path.end(), '/', '\\');
		if (!IsPathExists(path))
			path = "";
	}
	return path;
}

CString GetTempFilePath(void)
{
	const int maxBufferSize = 512;
	string currentPath;
	char buf[maxBufferSize];
	getcwd(buf, sizeof(buf));
	currentPath.assign(buf);
	CString path = currentPath + "\\Songs\\connect\\temp.zip";
	return path;
}

CString GetConnectFolderPath(void)
{
	const int maxBufferSize = 512;
	string currentPath;
	char buf[maxBufferSize];
	getcwd(buf, sizeof(buf));
	currentPath.assign(buf);
	CString path = currentPath + "\\Songs\\connect";
	return path;
}

// 判斷副檔名是否為影片 (sharefull 時不會過濾)
static bool IsVideoExtension(const CString& path)
{
	int dot = path.ReverseFind('.');
	if (dot < 0) return false;
	CString ext = path.substr(dot);
	ext.MakeLower();
	return ext == ".mp4" || ext == ".mpg" || ext == ".mpeg" ||
		ext == ".avi" || ext == ".wmv" || ext == ".mov";
}

// 遞迴列出資料夾內所有檔案 (相對路徑)
static void EnumerateFilesRecursive(const CString& root, const CString& subPath,
									bool filterVideo, vector<CString>& outRel, vector<uint32_t>& outSize)
{
	CString dir = root;
	if (!subPath.empty()) dir += "\\" + subPath;
	CString pattern = dir + "\\*";

	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;

	do
	{
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
			continue;
		// 跳過備份資料夾
		if (subPath.empty() && _stricmp(fd.cFileName, "FileBackup") == 0)
			continue;
		CString relChild = subPath.empty() ? CString(fd.cFileName) : (subPath + "\\" + fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			EnumerateFilesRecursive(root, relChild, filterVideo, outRel, outSize);
		}
		else
		{
			if (filterVideo && IsVideoExtension(relChild))
				continue;
			LARGE_INTEGER sz;
			sz.LowPart = fd.nFileSizeLow;
			sz.HighPart = fd.nFileSizeHigh;
			// 4GB 以上的單一檔案我們直接跳過 (Write4 放不下)
			if (sz.QuadPart >= 0xFFFFFFFFLL) continue;
			outRel.push_back(relChild);
			outSize.push_back((uint32_t)sz.QuadPart);
		}
	} while (FindNextFile(h, &fd));
	FindClose(h);
}

// 把字串轉小寫 (用在副檔名)
static CString LowerExt(const CString& p)
{
	int dot = p.ReverseFind('.');
	if (dot < 0) return "";
	CString ext = p.substr(dot);
	ext.MakeLower();
	return ext;
}

static bool IsChartExt(const CString& ext)
{
	return ext == ".sm" || ext == ".ssc" || ext == ".sma" || ext == ".ms" ||
		ext == ".dwi" || ext == ".bms" || ext == ".ksf";
}

// 收檔完成後列出磁碟內容，協助排查 reload 讀不到歌
// openedFiles / openedExpected / openedWritten / openedActualBytes 是「本次 transfer」的紀錄
static void LogRecvFolderDiagnostics(const CString& rootDir, int expectedFiles,
	int receivedBytes, int totalBytes, const CString& lastRelPath,
	int lastWritten, int lastSize, bool wasActive,
	const vector<CString>& openedFiles,
	const vector<int>& openedExpected,
	const vector<int>& openedWritten,
	const vector<int>& openedActualBytes)
{
	LOG->Info("[SHARE] recv diag: active=%d metaFiles=%d bytes=%d/%d openedThisXfer=%u rootDir='%s'",
		(int)wasActive, expectedFiles, receivedBytes, totalBytes,
		(unsigned)openedFiles.size(), rootDir.c_str());

	if (receivedBytes != totalBytes)
		LOG->Warn("[SHARE] recv diag: byte mismatch (got %d, expected %d, diff %d) -- 大量封包遺失或 sender 提早送 NSSDone",
			receivedBytes, totalBytes, receivedBytes - totalBytes);

	if (!lastRelPath.empty() && lastWritten < lastSize)
		LOG->Warn("[SHARE] recv diag: last file incomplete '%s' (%d/%d)",
			lastRelPath.c_str(), lastWritten, lastSize);

	if (rootDir.empty())
	{
		LOG->Warn("[SHARE] recv diag: rootDir empty");
		return;
	}
	if (!IsPathExists((const char*)rootDir.c_str()))
	{
		LOG->Warn("[SHARE] recv diag: rootDir missing on disk");
		return;
	}

	// 把本次傳輸的 relPath 建成快速查表
	// (檔案數通常很少，O(N*M) 不會痛)
	vector<CString> relPaths;
	vector<uint32_t> sizes;
	EnumerateFilesRecursive(rootDir, "", false, relPaths, sizes);

	int chartCount = 0;            // 有效 chart (磁碟 > 0 bytes 且副檔名是譜面)
	int chartZeroBytes = 0;        // 0 bytes 的 chart (這會讓 SongManager 跳過)
	int leftoverCount = 0;         // 沒在 openedFiles 內的 = 殘檔
	int sparseFileCount = 0;       // 有 sparse padding 的檔 (中間是 0，資料遺失)
	unsigned long long diskBytes = 0;
	unsigned long long leftoverBytes = 0;
	unsigned long long sparseHoleBytes = 0;

	for (size_t i = 0; i < relPaths.size(); ++i)
	{
		diskBytes += sizes[i];
		CString ext = LowerExt(relPaths[i]);

		// 是否屬於本次 transfer
		int matchIdx = -1;
		for (size_t j = 0; j < openedFiles.size(); ++j)
		{
			if (openedFiles[j] == relPaths[i]) { matchIdx = (int)j; break; }
		}

		const char *kind = (matchIdx >= 0) ? "xfer" : "LEFT";
		if (matchIdx < 0)
		{
			++leftoverCount;
			leftoverBytes += sizes[i];
		}

		// chart 統計
		if (IsChartExt(ext))
		{
			if (sizes[i] == 0) ++chartZeroBytes;
			else ++chartCount;
		}

		if (matchIdx >= 0)
		{
			int actualBytes = (matchIdx < (int)openedActualBytes.size()) ? openedActualBytes[matchIdx] : -1;
			int maxOff = openedWritten[matchIdx];
			bool sparse = (actualBytes >= 0 && actualBytes < maxOff);
			if (sparse)
			{
				++sparseFileCount;
				sparseHoleBytes += (unsigned long long)(maxOff - actualBytes);
			}
			LOG->Info("[SHARE] recv diag:   [%u %s%s] '%s' ondisk=%u xferExpect=%d maxOff=%d actualBytes=%d%s",
				(unsigned)i, kind, sparse ? "/SPARSE" : "", relPaths[i].c_str(), (unsigned)sizes[i],
				openedExpected[matchIdx], maxOff, actualBytes,
				sparse ? " ← 中間有缺，檔被 padding 成 0" : "");
		}
		else
		{
			LOG->Info("[SHARE] recv diag:   [%u %s] '%s' ondisk=%u (殘檔，本次未傳)",
				(unsigned)i, kind, relPaths[i].c_str(), (unsigned)sizes[i]);
		}
	}

	// 列出本次傳輸過但磁碟上找不到的檔
	for (size_t j = 0; j < openedFiles.size(); ++j)
	{
		bool found = false;
		for (size_t i = 0; i < relPaths.size(); ++i)
			if (relPaths[i] == openedFiles[j]) { found = true; break; }
		if (!found)
			LOG->Warn("[SHARE] recv diag: opened during xfer but MISSING on disk: '%s'",
				openedFiles[j].c_str());
	}

	LOG->Info("[SHARE] recv diag: disk %u files (%llu bytes); leftover=%d (%llu bytes); validChart=%d zeroChart=%d sparseFiles=%d holeBytes=%llu (meta fileCount=%d)",
		(unsigned)relPaths.size(), diskBytes, leftoverCount, leftoverBytes,
		chartCount, chartZeroBytes, sparseFileCount, sparseHoleBytes, expectedFiles);

	if (chartCount == 0)
		LOG->Warn("[SHARE] recv diag: NO valid chart (.sm/.ssc/.dwi/...) - SongManager will skip this folder");
	if (chartZeroBytes > 0)
		LOG->Warn("[SHARE] recv diag: 有 %d 個 chart 是 0 bytes - SongManager 會解析失敗", chartZeroBytes);
	if (leftoverCount > 0)
		LOG->Warn("[SHARE] recv diag: 有 %d 個殘檔 (前一次 transfer 沒清乾淨) - 建議手動刪除 %s 後重試",
			leftoverCount, rootDir.c_str());
	if (sparseFileCount > 0)
		LOG->Warn("[SHARE] recv diag: 有 %d 個 sparse 檔 (共 %llu bytes 是 0) — 中間 NSSData chunks 遺失",
			sparseFileCount, sparseHoleBytes);
	if (expectedFiles > 0 && (int)openedFiles.size() != expectedFiles)
		LOG->Warn("[SHARE] recv diag: 本次只開了 %u 個檔，但 meta 說要 %d 個 - sender 中途斷或 NSSData 大量遺失",
			(unsigned)openedFiles.size(), expectedFiles);
}

static void EnsureDirectoryExists(const CString& dir)
{
	if (dir.empty()) return;
	if (IsPathExists((const char*)dir.c_str())) return;
	int slash = dir.ReverseFind('\\');
	if (slash > 0)
	{
		CString parent = dir.substr(0, slash);
		EnsureDirectoryExists(parent);
	}
	CreateDirectory(dir.c_str(), NULL);
}

// 在 parent 下找一個沒被佔用的資料夾名 — 規則：
//   先試 baseName，若不存在就直接用；
//   否則試 "baseName (1)"、"baseName (2)"、... 直到找到。
// 回傳完整路徑 (parent + "\\" + finalName)
static CString MakeUniqueSubfolderPath(const CString& parent, const CString& baseName, CString& outFinalName)
{
	CString tryPath = parent + "\\" + baseName;
	if (!IsPathExists((const char*)tryPath.c_str()))
	{
		outFinalName = baseName;
		return tryPath;
	}
	for (int n = 1; n < 10000; ++n)
	{
		CString candidate;
		candidate.Format("%s (%d)", baseName.c_str(), n);
		tryPath = parent + "\\" + candidate;
		if (!IsPathExists((const char*)tryPath.c_str()))
		{
			outFinalName = candidate;
			return tryPath;
		}
	}
	// 極端情況：10000 個都被佔用，退而求其次用 base + 時戳
	CString fallback;
	fallback.Format("%s (%lu)", baseName.c_str(), (unsigned long)GetTickCount());
	outFinalName = fallback;
	return parent + "\\" + fallback;
}

// 把絕對路徑切出歌曲資料夾名稱 (path 最後一段)
static CString GetLastPathComponent(const CString& path)
{
	CString p = path;
	while (!p.empty() && (p[p.GetLength()-1] == '\\' || p[p.GetLength()-1] == '/'))
		p.erase(p.GetLength()-1, 1);
	int s = p.ReverseFind('\\');
	if (s < 0) s = p.ReverseFind('/');
	if (s < 0) return p;
	return p.substr(s + 1);
}

// 統一的 SendPack 出口：所有外部呼叫點都應該走這裡。
// EzSockets::SendPack 會做 outBuffer.append(size) + SendData(data) 兩段，
// 若 main thread 跟 share sender thread 同時撞進來，[len][payload] 序列會被插斷
// 變成 [A.len][B.len][B.payload][A.payload]，receiver 解析就會亂掉。
// 這裡用 g_hMutex 把整段包起來保證原子性。
void NetworkSyncManager::SendNSMPacket(PacketFunctions& pkt)
{
	if (!NetPlayerClient) return;
	WaitForSingleObject(g_hMutex, INFINITE);
	NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
	ReleaseMutex(g_hMutex);
}

void NetworkSyncManager::SendShareProgress()
{
	if (!useSMserver) return;
	PacketFunctions pkt; pkt.ClearPacket();
	pkt.Write1(NSSProgress);
	pkt.Write1((uint8_t)m_shareReceiverIndex);
	// 進度用「receiver 真的收到的 bytes」(從它送回來的 NSSXferAck 累積得到)，
	// 不要用 m_shareSentBytes — 那是「丟進 Steam buffer」的量，會早到 100% 但實際還沒到對方。
	pkt.Write4((uint32_t)m_shareReceiverAckedBytes);
	pkt.Write4((uint32_t)m_shareTotalBytes);
	SendNSMPacket(pkt);
}

// receiver 回報實際收到 bytes 給 sender (經 server 路由)。
// 格式: [NSSXferAck][senderIdx (=要回給誰)][recvBytes (4)][totalBytes (4)]
// server 收到後會：
//   1. 把資訊轉成 NSSProgress 廣播 (這樣 UI 顯示的是「真實」的 receiver 進度)
//   2. 把它轉發給 sender (讓 sender thread 可以等到真的收完)
void NetworkSyncManager::SendRecvAck()
{
	if (!useSMserver) return;
	PacketFunctions pkt; pkt.ClearPacket();
	pkt.Write1(NSSXferAck);
	pkt.Write1((uint8_t)m_recv.senderIndex);
	pkt.Write4((uint32_t)m_recv.receivedBytes);
	pkt.Write4((uint32_t)m_recv.totalBytes);
	SendNSMPacket(pkt);
	m_recv.lastAckedBytes = m_recv.receivedBytes;
}

// 印出 Steam 連線即時狀態 (sender 用，協助看出封包堆積/遺失)
static void LogSenderConnStatus(const char *tag, HSteamNetConnection conn, int sentSoFar, int totalBytes)
{
	if (conn == k_HSteamNetConnection_Invalid) return;
	SteamNetConnectionRealTimeStatus_t st = {};
	if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &st, 0, nullptr) != k_EResultOK)
	{
		LOG->Warn("[SHARE] sender status(%s): GetConnectionRealTimeStatus failed (conn closed?)", tag);
		return;
	}
	LOG->Info("[SHARE] sender status(%s): sent=%d/%d state=%d ping=%d ms qual L=%.2f R=%.2f "
		"out=%.0fB/s in=%.0fB/s rate=%dB/s pendRel=%d unackRel=%d queue=%lldus",
		tag, sentSoFar, totalBytes, (int)st.m_eState, st.m_nPing,
		st.m_flConnectionQualityLocal, st.m_flConnectionQualityRemote,
		st.m_flOutBytesPerSec, st.m_flInBytesPerSec, st.m_nSendRateBytesPerSecond,
		st.m_cbPendingReliable, st.m_cbSentUnackedReliable,
		(long long)st.m_usecQueueTime);
	// 低品質警告 -- Steam 認為 receiver 端收到的封包比例低
	if (st.m_flConnectionQualityRemote >= 0 && st.m_flConnectionQualityRemote < 0.9f)
		LOG->Warn("[SHARE] sender status(%s): remote quality only %.2f (<0.9) - 封包大量遺失中",
			tag, st.m_flConnectionQualityRemote);
}

DWORD NetworkSyncManager::ThreadProcNSSSS(void)
{
	// ======================================================================
	// 新流程 (zip + temp.sh):
	//   舊版: NSSMeta -> 一塊塊 NSSData -> NSSDone (60KB chunks 走 Steam reliable)
	//   新版:
	//     (a) 用 minizip 把整個歌曲資料夾打包加密 → songs/connect/temp.zip
	//     (b) curl PUT 到 temp.sh，拿到一條 URL
	//     (c) 送一個 NSSShareLink (URL + 密碼 + 資料夾名 + zip 大小) 給 server
	//     (d) server 端轉發給 receiver；receiver 自己 curl GET + minizip 解壓
	//   優點: sender 只上傳一次，多 receiver 同時下載；省 sender 上行頻寬，且
	//         走 HTTPS / temp.sh CDN 比 Steam reliable 對大檔友善。
	//
	// 為了配合 /shareall (server 對每個缺檔者各跑一次 ShareSong)，本端會
	// 把 zip 結果 cache 起來：同一首歌的後續 share 直接重用 m_cachedShareUrl，
	// 不再重新 zip / 重新上傳。
	// ======================================================================
	LOG->Info("[SHARE] sender thread start (zip+temp.sh mode). receiver=%d filter=%d",
		player_num, (int)video_file_filter);
	m_shareReceiverIndex = player_num;
	m_shareCancelRequested = false;
	m_shareSentBytes = 0;
	m_shareTotalBytes = 0;
	m_shareReceiverAckedBytes = 0;

	// 1. 找到歌曲資料夾
	string songDir = (GAMESTATE->m_pCurSong->GetSongDir()).c_str();
	if (!songDir.empty() && (songDir[songDir.size()-1] == '/' || songDir[songDir.size()-1] == '\\'))
		songDir = songDir.substr(0, songDir.size() - 1);
	CString songDirPath = GetSongDirPath(songDir, PREFSMAN->m_sAdditionalSongFolders);
	if (songDirPath.empty())
	{
		LOG->Warn("[SHARE] sender: cannot find song dir '%s'", songDir.c_str());
		usingShareSongSystem = false;
		ReportShareSongFinish();
		return 0L;
	}

	CString songFolderName = GetLastPathComponent(songDirPath);
	LOG->Info("[SHARE] sender: songDirPath='%s' folder='%s'", songDirPath.c_str(), songFolderName.c_str());

	// 2. 看看是不是同一首歌的續傳 (/shareall 第 2 個以後的 receiver)；
	//    是的話直接拿 cache 跳過 zip+upload。
	bool reuseCache = (!m_cachedShareUrl.empty()
		&& !m_cachedSharePassword.empty()
		&& m_cachedShareSongDir == songDirPath);

	CString shareUrl, sharePassword;
	int zipBytes = 0;
	if (reuseCache)
	{
		LOG->Info("[SHARE] sender: reuse cached zip url for '%s'", songDirPath.c_str());
		shareUrl = m_cachedShareUrl;
		sharePassword = m_cachedSharePassword;
		zipBytes = m_cachedShareZipBytes;
		songFolderName = m_cachedShareFolderName;
	}
	else
	{
		// 3. 把整個歌曲資料夾打包加密到 songs/connect/temp.zip
		CString connectFolder = GetConnectFolderPath();
		EnsureDirectoryExists(connectFolder);
		CString zipPath = connectFolder + "\\temp.zip";

		// 上次殘留就先刪，避免 zipOpen64 在 CREATE 模式拒絕
		DeleteFileA(zipPath.c_str());

		sharePassword = ShareZipUtil::GenerateRandomPassword(16);
		LOG->Info("[SHARE] sender: zipping '%s' -> '%s' (password length=%d)",
			songDirPath.c_str(), zipPath.c_str(), (int)sharePassword.size());

		CString errMsg;
		// 注意：這裡是同步的，整個 zip 結束才會回來。對 sender 來說沒影響 (本來就在
		// worker thread)；對 UI 來說會看到「準備中」狀態幾秒到幾十秒。
		bool zipOk = ShareZipUtil::ZipFolderWithPassword(songDirPath, zipPath, sharePassword, errMsg);
		if (!zipOk || m_shareCancelRequested)
		{
			LOG->Warn("[SHARE] sender: zip failed: %s (cancel=%d)",
				errMsg.c_str(), (int)m_shareCancelRequested);
			DeleteFileA(zipPath.c_str());
			usingShareSongSystem = false;
			m_shareReceiverIndex = -1;
			m_shareCancelRequested = false;
			ReportShareSongFinish();
			return 0L;
		}

		// 取得 zip 檔案大小，作為 UI 進度的 totalBytes
		HANDLE hFile = CreateFileA(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			LARGE_INTEGER li;
			if (GetFileSizeEx(hFile, &li))
				zipBytes = (int)li.QuadPart;
			CloseHandle(hFile);
		}
		LOG->Info("[SHARE] sender: zip done, %d bytes", zipBytes);

		// 4. 上傳到 temp.sh
		//    remoteFileName 加 timestamp 避免和別人撞名 (temp.sh 同名會覆蓋)。
		CString remoteName;
		remoteName = ssprintf("sm_%u_%u.zip", (unsigned)time(NULL), (unsigned)GetTickCount());

		LOG->Info("[SHARE] sender: uploading to temp.sh as '%s' ...", remoteName.c_str());
		bool uploadOk = ShareZipUtil::UploadToTempSh(zipPath, remoteName, shareUrl, errMsg);
		if (!uploadOk || m_shareCancelRequested)
		{
			LOG->Warn("[SHARE] sender: upload failed: %s (cancel=%d)",
				errMsg.c_str(), (int)m_shareCancelRequested);
			DeleteFileA(zipPath.c_str());
			usingShareSongSystem = false;
			m_shareReceiverIndex = -1;
			m_shareCancelRequested = false;
			ReportShareSongFinish();
			return 0L;
		}

		// 5. 更新 cache (給同首歌 /shareall 下一個 receiver 用)
		m_cachedShareSongDir = songDirPath;
		m_cachedShareFolderName = songFolderName;
		m_cachedShareUrl = shareUrl;
		m_cachedSharePassword = sharePassword;
		m_cachedShareZipBytes = zipBytes;

		// 6. zip 上傳成功後本地 temp.zip 可以丟掉 (不刪也行，反正下次 zip 會覆蓋)
		DeleteFileA(zipPath.c_str());
	}

	m_shareTotalBytes = zipBytes;
	m_shareSentBytes = zipBytes; // sender 端「上傳工作」當作 100% 完成
	SendShareProgress();

	// 7. 送 NSSShareLink：[opcode][receiver_idx][folderName NT][url NT][password NT][zipBytes 4]
	//    server 端 ForwardShareToReceiver 看 receiver_idx 轉發。
	{
		PacketFunctions pkt; pkt.ClearPacket();
		pkt.Write1(NSSShareLink);
		pkt.Write1((uint8_t)player_num);
		pkt.WriteNT(songFolderName);
		pkt.WriteNT(shareUrl);
		pkt.WriteNT(sharePassword);
		pkt.Write4((uint32_t)zipBytes);
		SendNSMPacket(pkt);
		LOG->Info("[SHARE] sender: sent NSSShareLink to receiver=%d folder='%s' url='%s' bytes=%d",
			player_num, songFolderName.c_str(), shareUrl.c_str(), zipBytes);
	}

	// 8. (新流程下) 不需要等 receiver ack — 它會自己 curl 下載並廣播 NSSXferAck，
	//    sender thread 在這裡直接退出就好。usingShareSongSystem 要等 thread 真的退出
	//    才放掉 (見最後)，否則 /shareall 的下一個 receiver 會撞到舊 session。

	// 註：以下保留 (舊 NSSData 串流路徑) 全部刪除；本函式新版到此為止。
	// 直接跳到 thread 退出區塊。
	goto SHARE_SENDER_EXIT;

#if 0  /* === LEGACY: 舊 60KB-chunk NSSData 串流流程，留作參考已停用 === */
	// 4. 一個一個檔案傳出去，依 NETSHARECHUNKSIZE 切塊
	char buf[NETSHARECHUNKSIZE];
	int lastProgressReport = 0;
	int chunkCounter = 0; // [OPT] 用來決定何時 yield
	for (size_t fi = 0; fi < relPaths.size(); ++fi)
	{
		if (m_shareCancelRequested) break;
		CString relPath = relPaths[fi];
		uint32_t fsize = sizes[fi];
		CString fullPath = songDirPath + "\\" + relPath;
		FILE *fp = fopen(fullPath.c_str(), "rb");
		if (!fp)
		{
			LOG->Warn("[SHARE] sender: cannot open '%s' skip", fullPath.c_str());
			continue;
		}

		uint32_t offset = 0;
		while (offset < fsize)
		{
			if (m_shareCancelRequested) { fclose(fp); fp = NULL; break; }
			int want = (int)(fsize - offset);
			if (want > NETSHARECHUNKSIZE) want = NETSHARECHUNKSIZE;
			int got = (int)fread(buf, 1, want, fp);
			if (got <= 0) break;

			PacketFunctions pkt; pkt.ClearPacket();
			pkt.Write1(NSSData);
			pkt.Write1((uint8_t)player_num);
			pkt.WriteNT(relPath);
			pkt.Write4(fsize);
			pkt.Write4(offset);
			pkt.Write2((uint16_t)got);
			pkt.WriteBytes(buf, got);
			SendNSMPacket(pkt);

			offset += got;
			m_shareSentBytes += got;

			// 約每 512KB 回報一次進度，避免訊息洪水
			// 從 64KB 拉大，因為 chunk size 變大後一次 send 已經就是 60KB 等級
			if (m_shareSentBytes - lastProgressReport >= 512 * 1024)
			{
				SendShareProgress();
				lastProgressReport = m_shareSentBytes;
			}

			++chunkCounter;

			// === [FLOW-CONTROL] 應用層 1MB 送窗 ===
			// 用「receiver 真實 ack 的 bytes」做 backpressure，而不是只看 Steam 本機 buffer。
			// 為什麼必要：
			//   (1) Steam reliable buffer 上限是 pending + sent-unacked，原本只看 pending 不夠用。
			//   (2) 即使加大 buffer，sender thread 還是會一口氣把 50MB 丟給 Steam，
			//       而 receiver 那邊一邊收、一邊寫檔、進度顯示也跟著走，差距太大會卡 UI。
			//   (3) 使用者明確要求「送 1MB 就停下來等 client 收成功再繼續送」。
			// 註：曾嘗試把 window 拉到 4MB 加速，但 receiver 端在主執行緒處理高速封包時
			//     會 ProcessInput 內 while-loop 卡很久，導致 render thread 無法及時更新
			//     觸發 Windows TDR / GPU watchdog → 整個 client 閃退。
			//     保守起見回到 1MB；要加速請改善 receiver 端 (例如限制每幀處理 chunk 數)。
			// m_shareReceiverAckedBytes 由 main thread 在 ProcessInput::NSSXferAck 更新，
			// 是 volatile int，sender thread 直接讀就好。
			{
				const int kSendWindow = 1024 * 1024;
				int waitedMs = 0;
				bool loggedOnce = false;
				int lastAckSeen = m_shareReceiverAckedBytes;
				int stuckMs = 0;
				while ((m_shareSentBytes - m_shareReceiverAckedBytes) > kSendWindow
					&& !m_shareCancelRequested)
				{
					if (!loggedOnce)
					{
						LOG->Info("[SHARE] sender: window full sent=%d acked=%d diff=%d (>1MB), waiting receiver",
							m_shareSentBytes, m_shareReceiverAckedBytes,
							m_shareSentBytes - m_shareReceiverAckedBytes);
						loggedOnce = true;
					}
					Sleep(5);
					waitedMs += 5;

					// 偵測 receiver 完全停 ack：超過 20 秒沒進度就強制放棄 window，
					// 不然 thread 會卡到 NSSDone 路徑那層 60 秒 ack 等待。
					if (m_shareReceiverAckedBytes == lastAckSeen)
					{
						stuckMs += 5;
					}
					else
					{
						lastAckSeen = m_shareReceiverAckedBytes;
						stuckMs = 0;
					}

					if (stuckMs >= 20000)
					{
						LOG->Warn("[SHARE] sender: window wait receiver ack stuck for 20s at acked=%d, "
							"give up window backpressure (Steam reliable 會繼續送，但 receiver 可能掉線)",
							m_shareReceiverAckedBytes);
						break;
					}

					// 每秒印一次 receiver 進度，協助診斷
					if ((waitedMs % 1000) == 0)
					{
						LOG->Info("[SHARE] sender: window wait %dms, sent=%d acked=%d",
							waitedMs, m_shareSentBytes, m_shareReceiverAckedBytes);
					}
				}
				if (loggedOnce)
				{
					LOG->Info("[SHARE] sender: window opened (waited=%dms) sent=%d acked=%d",
						waitedMs, m_shareSentBytes, m_shareReceiverAckedBytes);
				}
			}

			// === Steam 本機 reliable buffer backpressure (保留作為第二道保險) ===
			// 即使應用層送窗只有 1MB，若 Steam 本機暫時擠住也提早等一下。
			HSteamNetConnection ezConn = NetPlayerClient->GetHandle();
			if (ezConn != k_HSteamNetConnection_Invalid)
			{
				SteamNetConnectionRealTimeStatus_t st = {};
				if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(ezConn, &st, 0, nullptr) == k_EResultOK)
				{
					int loopGuard = 0;
					while ((st.m_cbPendingReliable + st.m_cbSentUnackedReliable) > 256 * 1024
						&& !m_shareCancelRequested
						&& loopGuard < 2000) // 最多等 ~2 秒，避免異常卡死
					{
						Sleep(1);
						loopGuard++;
						SteamAPI_RunCallbacks();
						SteamNetworkingSockets()->RunCallbacks();
						if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(ezConn, &st, 0, nullptr) != k_EResultOK)
							break;
					}
				}
			}

			// 每 16 個 chunk 純 yield 一次，給 main thread 一點點機會
			if ((chunkCounter & 0x0F) == 0)
				Sleep(0);

			// 每 64 個 chunk 印一次連線狀態，協助看出 Steam 是不是在掉包 / 堆積
			if ((chunkCounter & 0x3F) == 0)
			{
				HSteamNetConnection ezConn2 = NetPlayerClient->GetHandle();
				LogSenderConnStatus("midxfer", ezConn2, m_shareSentBytes, m_shareTotalBytes);
			}
		}
		if (fp) fclose(fp);
	}

	// 5. 在送 NSSDone 前先等 Steam reliable buffer 清空 + 等 receiver 確認真的收到。
	HSteamNetConnection ezConnEnd = NetPlayerClient->GetHandle();
	LogSenderConnStatus("preFlush", ezConnEnd, m_shareSentBytes, m_shareTotalBytes);

	// (a) 等 Steam reliable 把本機 buffer 清乾淨 (最多 10 秒，避免極端情況卡住)
	if (ezConnEnd != k_HSteamNetConnection_Invalid && !m_shareCancelRequested)
	{
		const int kMaxFlushMs = 10000;
		int waited = 0;
		// [FIX] 內層 loop 也要 check m_shareCancelRequested，否則 user /cancel 後
		// thread 還會在這裡耗滿 10 秒，期間 usingShareSongSystem 一直是 true，
		// 新的 /share 會被 NSSSS handler 擋掉，看起來「沒反應」。
		while (waited < kMaxFlushMs && !m_shareCancelRequested)
		{
			SteamNetConnectionRealTimeStatus_t st = {};
			if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(ezConnEnd, &st, 0, nullptr) != k_EResultOK)
				break;
			if (st.m_cbPendingReliable == 0 && st.m_cbSentUnackedReliable == 0)
				break;
			Sleep(50);
			waited += 50;
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
		}
		LOG->Info("[SHARE] sender: steam flush wait done waited=%dms cancel=%d",
			waited, (int)m_shareCancelRequested);
	}
	LogSenderConnStatus("postFlush", ezConnEnd, m_shareSentBytes, m_shareTotalBytes);

	// (b) 等 receiver 端 NSSXferAck 回報它真的收齊 (最多 60 秒)。
	//     m_shareReceiverAckedBytes 由 main thread 在 ProcessInput::NSSXferAck 更新。
	//     這是修掉「receiver 進度條跳 100% 但其實還沒收完」的關鍵。
	if (!m_shareCancelRequested && m_shareTotalBytes > 0)
	{
		const int kMaxAckWaitMs = 60000;
		int waited = 0;
		int lastLoggedAck = -1;
		int stuckMs = 0;
		int lastAckSnapshot = m_shareReceiverAckedBytes;
		while (waited < kMaxAckWaitMs && !m_shareCancelRequested)
		{
			if (m_shareReceiverAckedBytes >= m_shareTotalBytes)
				break;
			// 每 1 秒印一次目前 receiver 收到多少
			if (m_shareReceiverAckedBytes != lastLoggedAck && (waited % 1000) == 0)
			{
				LOG->Info("[SHARE] sender: waiting receiver ack %d/%d (%dms elapsed)",
					m_shareReceiverAckedBytes, m_shareTotalBytes, waited);
				lastLoggedAck = m_shareReceiverAckedBytes;
			}
			// 偵測「receiver 停止增長」: 連續 15 秒沒進度就放棄
			if (m_shareReceiverAckedBytes == lastAckSnapshot)
			{
				stuckMs += 100;
				if (stuckMs >= 15000)
				{
					LOG->Warn("[SHARE] sender: receiver ack stuck at %d/%d for 15s, give up waiting",
						m_shareReceiverAckedBytes, m_shareTotalBytes);
					break;
				}
			}
			else
			{
				stuckMs = 0;
				lastAckSnapshot = m_shareReceiverAckedBytes;
			}
			Sleep(100);
			waited += 100;
		}
		LOG->Info("[SHARE] sender: ack wait finished, receiverAcked=%d/%d (waited=%dms)",
			m_shareReceiverAckedBytes, m_shareTotalBytes, waited);
	}

	// 6. 廣播最後一次進度與結束/取消
	SendShareProgress();
	{
		PacketFunctions pkt; pkt.ClearPacket();
		if (m_shareCancelRequested)
		{
			pkt.Write1(NSSCancel);
			pkt.Write1((uint8_t)player_num);
			LOG->Info("[SHARE] sender: send NSSCancel to receiver=%d", player_num);
		}
		else
		{
			pkt.Write1(NSSDone);
			pkt.Write1((uint8_t)player_num);
			LOG->Info("[SHARE] sender: send NSSDone to receiver=%d sentBytes=%d totalBytes=%d receiverAcked=%d",
				player_num, m_shareSentBytes, m_shareTotalBytes, m_shareReceiverAckedBytes);
			if (m_shareSentBytes != m_shareTotalBytes)
				LOG->Warn("[SHARE] sender: sentBytes != totalBytes (%d != %d) - 中途有檔案 fread 失敗或被取消",
					m_shareSentBytes, m_shareTotalBytes);
			if (m_shareReceiverAckedBytes < m_shareTotalBytes)
				LOG->Warn("[SHARE] sender: receiver 還沒收齊 (%d/%d) 但已超時或對方卡住，仍送 NSSDone",
					m_shareReceiverAckedBytes, m_shareTotalBytes);
		}
		SendNSMPacket(pkt);
	}

	usingShareSongSystem = false;
	m_shareReceiverIndex = -1;
	m_shareCancelRequested = false;
	ReportShareSongFinish();
	LOG->Info("[SHARE] sender thread exit (legacy path)");
	return 0L;
#endif /* === LEGACY end === */

SHARE_SENDER_EXIT:
	// 新流程的退出點：上面已經把 NSSShareLink 送出去了，剩下就是清旗標讓
	// /shareall 第 2 個 receiver 觸發的 NSSSS 可以建出新的 sender thread
	// (新 thread 會看到 m_cachedShareUrl 有東西，直接重用前一次的 URL)。
	usingShareSongSystem = false;
	m_shareReceiverIndex = -1;
	m_shareCancelRequested = false;
	ReportShareSongFinish();
	LOG->Info("[SHARE] sender thread exit (zip+temp.sh path)");
	return 0L;
}

// =====================================================================
// ThreadProcShareDownload — receiver 端
// =====================================================================
// 流程：
//   1) 把 NSSShareLink 帶來的 URL 下載到 songs/connect/.recv_<pid>.zip
//      (用 curl.exe shell-out。同時開另一個 watcher thread 每 500ms poll
//       下載中檔案大小，回 NSSXferAck 給 sender 顯示真實進度)
//   2) 用 minizip + password 解到 songs/connect/<folderName>/
//   3) 刪 zip、回報 finish 給 UI / server
// =====================================================================

struct DownloadProgressContext
{
	CString localFile;
	volatile bool stop;
	NetworkSyncManager* nsm;
	int totalBytes;
	int senderIdx;
};

// poll 用的小 thread：每 500ms stat 一次 localFile，把目前大小當作「已下載」
// 透過 NSSXferAck 廣播給 server，server 再 forward 給 sender 跟所有 client，
// receiver UI 顯示進度條也是看 m_PlayerShareProgress (server 廣播的 NSSProgress)。
static DWORD WINAPI DownloadProgressWatcher(LPVOID param)
{
	DownloadProgressContext* ctx = (DownloadProgressContext*)param;
	while (!ctx->stop)
	{
		struct stat st;
		int curBytes = 0;
		if (stat(ctx->localFile.c_str(), &st) == 0)
			curBytes = (int)st.st_size;

		// 直接組 NSSXferAck packet 送出去 (走 mutex 安全的 SendNSMPacket)
		PacketFunctions pkt;
		pkt.ClearPacket();
		pkt.Write1(NSSXferAck);
		pkt.Write1((uint8_t)ctx->senderIdx);
		pkt.Write4((uint32_t)curBytes);
		pkt.Write4((uint32_t)ctx->totalBytes);
		ctx->nsm->SendNSMPacket(pkt);

		Sleep(500);
	}
	return 0;
}

DWORD NetworkSyncManager::ThreadProcShareDownload(void)
{
	int senderIdx = m_downloadParams.senderIdx;
	CString folderName = m_downloadParams.folderName;
	CString url = m_downloadParams.url;
	CString password = m_downloadParams.password;
	int totalBytes = m_downloadParams.totalBytes;

	LOG->Info("[SHARE] recv-download thread start. sender=%d folder='%s' url='%s' bytes=%d",
		senderIdx, folderName.c_str(), url.c_str(), totalBytes);

	CString connectFolder = GetConnectFolderPath();
	EnsureDirectoryExists(connectFolder);

	// localZip：用 .recv_<pid>_<tick>.zip 避免多 receiver 同時跑時撞名
	CString localZip = connectFolder + ssprintf("\\.recv_%u_%u.zip",
		(unsigned)GetCurrentProcessId(), (unsigned)GetTickCount());
	// 上次殘留先掃掉 (基本上不會撞，但保險)
	DeleteFileA(localZip.c_str());

	// 起 progress watcher
	DownloadProgressContext ctx;
	ctx.localFile = localZip;
	ctx.stop = false;
	ctx.nsm = this;
	ctx.totalBytes = totalBytes;
	ctx.senderIdx = senderIdx;
	DWORD watcherTid;
	HANDLE hWatcher = CreateThread(NULL, 0, DownloadProgressWatcher, &ctx, 0, &watcherTid);

	CString errMsg;
	bool dlOk = ShareZipUtil::DownloadFile(url, localZip, errMsg);

	// 停 watcher
	ctx.stop = true;
	if (hWatcher)
	{
		WaitForSingleObject(hWatcher, 2000);
		CloseHandle(hWatcher);
	}

	if (!dlOk || m_shareCancelRequested)
	{
		LOG->Warn("[SHARE] recv-download: curl failed: %s (cancel=%d)",
			errMsg.c_str(), (int)m_shareCancelRequested);
		DeleteFileA(localZip.c_str());
		m_recv.active = false;
		usingShareSongSystem = false;
		m_downloadThreadRunning = false;
		ReportShareSongFinish();
		return 0L;
	}

	// 解壓到 songs/connect/<folderName>/，如果撞名就 (1)(2)(3)...
	CString finalName;
	CString destDir = MakeUniqueSubfolderPath(connectFolder, folderName, finalName);
	EnsureDirectoryExists(destDir);
	m_recv.rootDir = destDir;
	LOG->Info("[SHARE] recv-download: extract '%s' -> '%s' (password length=%d)",
		localZip.c_str(), destDir.c_str(), (int)password.size());

	int n = ShareZipUtil::ExtractZipWithPassword(localZip, destDir, password, errMsg);
	DeleteFileA(localZip.c_str());

	if (n < 0)
	{
		LOG->Warn("[SHARE] recv-download: extract failed: %s", errMsg.c_str());
		// 部份解出來的檔案丟了，刪除已建的目錄避免半成品
		RemovePartialRecv();
		usingShareSongSystem = false;
		m_downloadThreadRunning = false;
		ReportShareSongFinish();
		return 0L;
	}

	// 成功：把 receivedBytes 直接設成 totalBytes，並送一次最終 ack
	m_recv.receivedBytes = totalBytes;
	{
		PacketFunctions pkt;
		pkt.ClearPacket();
		pkt.Write1(NSSXferAck);
		pkt.Write1((uint8_t)senderIdx);
		pkt.Write4((uint32_t)totalBytes);
		pkt.Write4((uint32_t)totalBytes);
		SendNSMPacket(pkt);
	}

	// 通知 UI / Screen reload
	LOG->Info("[SHARE] recv-download: done %d files in '%s'", n, destDir.c_str());
	SCREENMAN->SystemMessage("Share song received!");
	// 通知 UI 重新載入 connect/ 目錄；走 SCREENMAN 是 thread-safe 的廣播。
	SCREENMAN->SendMessageToTopScreen(SM_ReloadConnectPack);

	m_recv.active = false;
	usingShareSongSystem = false;
	m_downloadThreadRunning = false;
	ReportShareSongFinish();
	LOG->Info("[SHARE] recv-download thread exit");
	return 0L;
}

// ============== Receiver helpers ==============
void NetworkSyncManager::ResetRecvState()
{
	m_recv.active = false;
	m_recv.senderIndex = -1;
	m_recv.totalBytes = 0;
	m_recv.receivedBytes = 0;
	m_recv.fileCount = 0;
	m_recv.rootDir = "";
	m_recv.currentRelPath = "";
	m_recv.currentFile = NULL;
	m_recv.currentFileSize = 0;
	m_recv.currentFileWritten = 0;
	m_recv.currentFileBytesWritten = 0;
	m_recv.openedFiles.clear();
	m_recv.openedExpected.clear();
	m_recv.openedWritten.clear();
	m_recv.openedActualBytes.clear();
	m_recv.lastAckedBytes = 0;
}

void NetworkSyncManager::CloseRecvFile()
{
	if (m_recv.currentFile)
	{
		// 先記下要驗證的資訊，再 close
		CString relPath = m_recv.currentRelPath;
		int expected = m_recv.currentFileSize;
		int written = m_recv.currentFileWritten;
		int bytesWritten = m_recv.currentFileBytesWritten;
		CString full = m_recv.rootDir + "\\" + relPath;
		fclose(m_recv.currentFile);
		m_recv.currentFile = NULL;

		// 同步更新 openedFiles 裡的最終 written / actualBytes
		for (size_t i = 0; i < m_recv.openedFiles.size(); ++i)
		{
			if (m_recv.openedFiles[i] == relPath)
			{
				m_recv.openedWritten[i] = written;
				m_recv.openedActualBytes[i] = bytesWritten;
				break;
			}
		}

		// close 後再 stat 一次磁碟實際大小
		// 如果 bytesWritten < written，代表中間有缺、磁碟檔是 sparse (中間是 0) — 嚴重資料遺失
		struct stat st;
		if (stat(full.c_str(), &st) == 0)
		{
			LOG->Info("[SHARE] recv: close '%s' expect=%d written(maxOff)=%d actualBytes=%d ondisk=%lld",
				relPath.c_str(), expected, written, bytesWritten, (long long)st.st_size);
			if (bytesWritten != written)
				LOG->Warn("[SHARE] recv: SPARSE FILE '%s' 實際只寫 %d bytes 但檔大小 %d (有 %d bytes 是 0 — 中間 chunk 遺失)",
					relPath.c_str(), bytesWritten, written, written - bytesWritten);
			if ((int)st.st_size != written)
				LOG->Warn("[SHARE] recv: WRITE LOST '%s' wrote=%d but ondisk=%lld",
					relPath.c_str(), written, (long long)st.st_size);
			if (written >= expected && (int)st.st_size != expected)
				LOG->Warn("[SHARE] recv: SIZE MISMATCH '%s' expect=%d ondisk=%lld",
					relPath.c_str(), expected, (long long)st.st_size);
		}
		else
		{
			LOG->Warn("[SHARE] recv: close '%s' but stat failed (file missing?)",
				relPath.c_str());
		}
	}
	m_recv.currentRelPath = "";
	m_recv.currentFileSize = 0;
	m_recv.currentFileWritten = 0;
	m_recv.currentFileBytesWritten = 0;
}

void NetworkSyncManager::OpenRecvFile(const CString& relPath, int fileSize)
{
	CloseRecvFile();
	CString full = m_recv.rootDir + "\\" + relPath;
	int slash = full.ReverseFind('\\');
	if (slash > 0)
	{
		CString parent = full.substr(0, slash);
		EnsureDirectoryExists(parent);
	}

	// 開檔前先看磁碟上是否已存在 (殘檔)
	bool preExisted = false;
	long long preSize = -1;
	{
		struct stat st;
		if (stat(full.c_str(), &st) == 0)
		{
			preExisted = true;
			preSize = (long long)st.st_size;
		}
	}

	m_recv.currentFile = fopen(full.c_str(), "wb");
	if (!m_recv.currentFile)
		LOG->Warn("[SHARE] recv: cannot create '%s'", full.c_str());
	else
		LOG->Info("[SHARE] recv: open '%s' expect %d bytes (preExist=%d preSize=%lld)",
			full.c_str(), fileSize, (int)preExisted, preSize);

	m_recv.currentRelPath = relPath;
	m_recv.currentFileSize = fileSize;
	m_recv.currentFileWritten = 0;
	m_recv.currentFileBytesWritten = 0;

	m_recv.openedFiles.push_back(relPath);
	m_recv.openedExpected.push_back(fileSize);
	m_recv.openedWritten.push_back(0);
	m_recv.openedActualBytes.push_back(0);
}

void NetworkSyncManager::RemovePartialRecv()
{
	CloseRecvFile();
	if (!m_recv.rootDir.empty() && IsPathExists((const char*)m_recv.rootDir.c_str()))
	{
		// 用 system 移除整個資料夾。比起手刻遞迴刪除安全簡單，且只在本機執行
		CString cmd = CString("rmdir /S /Q \"") + m_recv.rootDir + "\"";
		LOG->Info("[SHARE] recv: cleanup '%s'", m_recv.rootDir.c_str());
		system(cmd.c_str());
	}
	ResetRecvState();
}

void NetworkSyncManager::ProcessInput()
{
	//If we're disconnected, just exit
	if ((NetPlayerClient->state!=NetPlayerClient->skCONNECTED) || 
			NetPlayerClient->IsError())
	{
		LOG->Warn("[NETDBG] NSM::ProcessInput#1 connection dropped (state=%d)", (int)NetPlayerClient->state);
		SCREENMAN->SystemMessageNoAnimate("Connection to server dropped.");
		useSMserver=false;
		m_sChatText="";
		return;
	}

	//load new data into buffer
	NetPlayerClient->update();

	// 每個 iteration 都要重置 m_packet 的 Position 和 buffer，
	// 不然第 2 個以後的封包會用上一個封包剩下的 Position 來解析 → 整個錯位。
	// (這就是大檔變 sparse 的根因：chunk 數量沒少，但 relPath/offset/chunkLen 都讀錯位)
	while (true)
	{
		m_packet.ClearPacket();
		int nRead = NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE);
		if (nRead <= 0) break;
		m_packet.PayloadLength = nRead;

		int command = m_packet.Read1();
		// [FPS] 收到 NSSData 時這條會 per-chunk 觸發 + fsync，殺 FPS。降為 Trace。
		LOG->Trace("[NETDBG] NSM::ProcessInput#2 got command raw=%d (server offset cmd=%d)",
			command, command - NSServerOffset);
		//Check to make sure command is valid from server
		if (command < NSServerOffset)
		{		
			LOG->Trace("CMD (below 128) Invalid> %d",command);
 			break;
		}

		command = command - NSServerOffset;

		switch (command)
		{
		case NSCPing: //Ping packet responce
			m_packet.ClearPacket();
			m_packet.Write1( NSCPingR );
			SendNSMPacket(m_packet);
			break;
		case NSCPingR:	//These are in responce to when/if we send packet 0's
		case NSCHello: //This is already taken care of by the blocking code earlier on
		case NSCGSR: //This is taken care of by the blocking start code
			break;
		case NSCGON: 
			{
				int PlayersInPack = m_packet.Read1();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].name = m_packet.Read1();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].score = m_packet.Read4();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].grade = m_packet.Read1();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].difficulty = (Difficulty) m_packet.Read1();
				for (int j=0; j<NETNUMTAPSCORES; ++j) 
					for (int i=0; i<PlayersInPack; ++i)
						m_EvalPlayerData[i].tapScores[j] = m_packet.Read2();
				for (int i=0; i<PlayersInPack; ++i)
					// m_EvalPlayerData[i].percentage = m_packet.ReadNT();
					m_EvalPlayerData[i].playerOptions = m_packet.ReadNT();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].percentage = m_packet.ReadNT();
				SCREENMAN->SendMessageToTopScreen( SM_GotEval );
			}
			break;
		case NSCGSU: //Scoreboard Update
			{	//Ease scope
				int ColumnNumber=m_packet.Read1();
				int NumberPlayers=m_packet.Read1();
				CString ColumnData;
				int i;
				switch (ColumnNumber)
				{
				case NSSB_NAMES:
					ColumnData = "Names\n";
					for (i=0; i<NumberPlayers; ++i)
					{
						unsigned int k = m_packet.Read1();
						if ( k < m_PlayerNames.size() )
							ColumnData += m_PlayerNames[k] + "\n";
					}
					break;
				case NSSB_COMBO:
					ColumnData = "Combo\n";
					for (i=0; i<NumberPlayers; ++i)
						ColumnData += ssprintf("%d\n",m_packet.Read2());
					break;
				case NSSB_GRADE:
					ColumnData = "Grade\n";
					for (i=0;i<NumberPlayers;i++)
						switch (m_packet.Read1())
						{
						case 0:
							ColumnData+="AAAA\n"; break;
						case 1:
							ColumnData+="AAA\n"; break;
						case 2:
							ColumnData+="AA\n"; break;
						case 3:
							ColumnData+="A\n"; break;
						case 4:
							ColumnData+="B\n"; break;
						case 5:
							ColumnData+="C\n"; break;
						case 6:
							ColumnData+="D\n"; break;
						case 7: 
							ColumnData+="E\n";	break;	//Is there a better way?
						}
					break;
				}
				m_Scoreboard[ColumnNumber] = ColumnData;
				m_scoreboardchange[ColumnNumber]=true;
			}
			break;
		case NSCSU:	//System message from server
			{
				CString SysMSG = m_packet.ReadNT();
				SCREENMAN->SystemMessage( SysMSG );
			}
			break;
		case NSCCM:	//Chat message from server
			{
				m_sChatText += m_packet.ReadNT() + " \n ";
				//10000 chars backlog should be more than enough
				m_sChatText = m_sChatText.Right(10000);
				SCREENMAN->SendMessageToTopScreen( SM_AddToChat );
			}
			break;
		case NSCRSG: //Select Song/Play song
			{
				m_iSelectMode = m_packet.Read1();
				m_sMainTitle = m_packet.ReadNT();
				m_sArtist = m_packet.ReadNT();
				m_sSubTitle = m_packet.ReadNT();
				int temp_hash = m_packet.Read4();
				if(temp_hash!=0)
				{
					m_ihash = temp_hash;
				}
				m_sCurMainTitle=m_sMainTitle;
				m_sCurArtist=m_sArtist;
				m_sCurSubTitle=m_sSubTitle;
				SCREENMAN->SendMessageToTopScreen( SM_ChangeSong );
			}
			break;
		case NSCUUL:
			{
				/*int ServerMaxPlayers=*/m_packet.Read1();
				int PlayersInThisPacket=m_packet.Read1();
				m_PlayerStatus.clear();
				m_PlayerNames.clear();
				m_ActivePlayers = 0;
				for (int i=0; i<PlayersInThisPacket; ++i)
				{
					int PStatus = m_packet.Read1();
					if ( PStatus > 0 )
					{
						m_ActivePlayers++;
						m_ActivePlayer.push_back( i );
					}
					m_PlayerStatus.push_back( PStatus );
					m_PlayerNames.push_back( m_packet.ReadNT() );	
				}
			}
			break;
		case NSCSMS:
			{
				CString StyleName, GameName;
				GameName = m_packet.ReadNT();
				StyleName = m_packet.ReadNT();

				GAMESTATE->m_pCurGame = GAMEMAN->StringToGameType( GameName );
				GAMESTATE->m_pCurStyle = GAMEMAN->GameAndStringToStyle( GAMESTATE->m_pCurGame, StyleName );

				SCREENMAN->SetNewScreen( "ScreenNetSelectMusic" ); //Should this be metric'd out?
			}
			break;
		case NSSSS:
			{
				server_ip = m_packet.ReadNT();
				player_num = m_packet.Read1();
				video_file_filter = (bool)m_packet.Read1();
				if (usingShareSongSystem == false)
				{
					usingShareSongSystem = true;
					DWORD ThreadID;
					HANDLE thread = CreateThread(NULL, 0, StaticThreadStartNSSSS, (void *)this, 0, &ThreadID);
					CloseHandle(thread);
				}
			}
			break;
		case NSSSC:
			{
				// 舊版的 receiver 啟動指令。現在只把它當作「對方要傳檔給我了」的提示，
				// 真正的接收工作改由 NSSMeta / NSSData / NSSDone 完成。
				server_ip = m_packet.ReadNT();
				file_size = m_packet.Read4();
				usingShareSongSystem = true;
				LOG->Info("[SHARE] recv: incoming notice from '%s' size=%d", server_ip.c_str(), file_size);
			}
			break;
		case NSSMeta:
			{
				int sender = m_packet.Read1();
				CString folderName = m_packet.ReadNT();
				int fileCount = (int)m_packet.Read4();
				int totalBytes = (int)m_packet.Read4();
				LOG->Info("[SHARE] recv: NSSMeta sender=%d folder='%s' files=%d bytes=%d",
					sender, folderName.c_str(), fileCount, totalBytes);

				if (m_recv.active) RemovePartialRecv();
				ResetRecvState();
				m_recv.active = true;
				m_recv.senderIndex = sender;
				m_recv.totalBytes = totalBytes;
				m_recv.fileCount = fileCount;

				CString connectFolder = GetConnectFolderPath();
				EnsureDirectoryExists(connectFolder);

				// 如果 connect 下已有相同名稱資料夾，自動編號 (1)、(2)、... 避免和舊下載混在一起
				CString finalName;
				m_recv.rootDir = MakeUniqueSubfolderPath(connectFolder, folderName, finalName);
				if (finalName != folderName)
					LOG->Info("[SHARE] recv: NSSMeta '%s' 已存在，改用新資料夾 '%s'",
						folderName.c_str(), finalName.c_str());

				EnsureDirectoryExists(m_recv.rootDir);
				LOG->Info("[SHARE] recv: NSSMeta rootDir='%s' exists=%d",
					m_recv.rootDir.c_str(),
					(int)IsPathExists((const char*)m_recv.rootDir.c_str()));

				usingShareSongSystem = true;
			}
			break;
		case NSSData:
			{
				(void)m_packet.Read1(); // sender_index (僅 server->receiver 路由用，這裡不需要)
				CString relPath = m_packet.ReadNT();
				int fileSize = (int)m_packet.Read4();
				int offset = (int)m_packet.Read4();
				int chunkLen = (int)m_packet.Read2();
				// 防範非預期過大的 chunk 寫爆 buffer
				if (chunkLen < 0) chunkLen = 0;
				if (chunkLen > NETSHARECHUNKSIZE) chunkLen = NETSHARECHUNKSIZE;
				char buf[NETSHARECHUNKSIZE];
				int gotBytes = m_packet.ReadBytes(buf, chunkLen);
				if (!m_recv.active)
				{
					LOG->Warn("[SHARE] recv: NSSData but recv inactive; ignore");
					break;
				}
				if (relPath != m_recv.currentRelPath)
				{
					if (m_recv.currentFile && m_recv.currentFileWritten < m_recv.currentFileSize)
						LOG->Warn("[SHARE] recv: previous file '%s' incomplete (%d/%d)",
							m_recv.currentRelPath.c_str(),
							m_recv.currentFileWritten, m_recv.currentFileSize);
					OpenRecvFile(relPath, fileSize);
				}
				if (m_recv.currentFile && gotBytes > 0)
				{
					fseek(m_recv.currentFile, offset, SEEK_SET);
					size_t written = fwrite(buf, 1, gotBytes, m_recv.currentFile);
					if ((int)written != gotBytes)
						LOG->Warn("[SHARE] recv: fwrite short '%s' wrote %u/%d at off %d",
							m_recv.currentRelPath.c_str(), (unsigned)written, gotBytes, offset);
					// currentFileWritten = 「最遠寫到哪」(判斷 sender 送完)
					// currentFileBytesWritten = 「實際寫入幾 bytes」(偵測中間缺洞)
					if (offset + gotBytes > m_recv.currentFileWritten)
						m_recv.currentFileWritten = offset + gotBytes;
					m_recv.currentFileBytesWritten += (int)written;
					if (m_recv.currentFileWritten >= m_recv.currentFileSize)
					{
						fflush(m_recv.currentFile);
						LOG->Info("[SHARE] recv: file done '%s' expect=%d maxOff=%d actualBytes=%d (total recv %d/%d)",
							m_recv.currentRelPath.c_str(), m_recv.currentFileSize,
							m_recv.currentFileWritten, m_recv.currentFileBytesWritten,
							m_recv.receivedBytes + gotBytes, m_recv.totalBytes);
					}
				}
				else if (gotBytes > 0 && !m_recv.currentFile)
				{
					LOG->Warn("[SHARE] recv: NSSData %d bytes but no file handle for '%s'",
						gotBytes, relPath.c_str());
				}
				m_recv.receivedBytes += gotBytes;

				// 觸發 NSSXferAck 回報給 sender：
				//   (1) 每收 256KB 一次 (進度條 + sender 知道有進度)
				//   (2) 一旦 receivedBytes 達到 totalBytes，立刻送一次 (sender thread 才能停等)
				// 為什麼 256KB 不是更大：sender 的應用層送窗是 1MB，receiver ack 太疏會讓 sender
				// 每 1MB 就卡一下、等 ~1 個 RTT，整體 throughput 變抖。256KB ack 讓 sender 大約
				// 每 ~256KB 就拿到一次 credit，window 像滾動視窗一樣平滑前進。
				bool reachedTotal = (m_recv.totalBytes > 0 && m_recv.receivedBytes >= m_recv.totalBytes);
				bool over256k = (m_recv.receivedBytes - m_recv.lastAckedBytes >= 256 * 1024);
				if (over256k || (reachedTotal && m_recv.lastAckedBytes < m_recv.totalBytes))
					SendRecvAck();
			}
			break;
		case NSSDone:
			{
				int sender = m_packet.Read1();
				CString rootDir = m_recv.rootDir;
				int expectedFiles = m_recv.fileCount;
				int receivedBytes = m_recv.receivedBytes;
				int totalBytes = m_recv.totalBytes;
				bool wasActive = m_recv.active;
				CString lastRelPath = m_recv.currentRelPath;
				int lastWritten = m_recv.currentFileWritten;
				int lastSize = m_recv.currentFileSize;

				LOG->Info("[SHARE] recv: NSSDone sender=%d active=%d bytes=%d/%d files=%d root='%s'",
					sender, (int)wasActive, receivedBytes, totalBytes,
					expectedFiles, rootDir.c_str());

				if (!wasActive)
					LOG->Warn("[SHARE] recv: NSSDone but recv inactive (duplicate or out of order?)");

				// 重要：先 fclose 把資料真正刷到磁碟，再做磁碟掃描，
				// 不然「fwrite + fflush 但還沒 fclose」的檔在 FindFirstFile 看到的 size 會落後 (常見 0 bytes)。
				CloseRecvFile();

				// 把 openedFiles 拷貝出來再給診斷用 (ResetRecvState 會清掉)
				vector<CString> opened       = m_recv.openedFiles;
				vector<int>     openedExp    = m_recv.openedExpected;
				vector<int>     openedWrt    = m_recv.openedWritten;
				vector<int>     openedActual = m_recv.openedActualBytes;

				LogRecvFolderDiagnostics(rootDir, expectedFiles, receivedBytes, totalBytes,
					lastRelPath, lastWritten, lastSize, wasActive,
					opened, openedExp, openedWrt, openedActual);

				ResetRecvState();
				usingShareSongSystem = false;
				ReportShareSongFinish();

				Screen *pTop = SCREENMAN->GetTopScreen();
				LOG->Info("[SHARE] recv: SM_ReloadConnectPack -> topScreen='%s'",
					pTop ? pTop->GetName().c_str() : "(null)");
				SCREENMAN->SendMessageToTopScreen(SM_ReloadConnectPack);
			}
			break;
		case NSSCancel:
			{
				int peer = m_packet.Read1();
				LOG->Info("[SHARE] cancel from peer=%d (using=%d recv=%d)",
					peer, (int)usingShareSongSystem, (int)m_recv.active);

				// 任何角色都先設 cancel 旗標 (sender thread 內部會輪詢這個 flag)
				m_shareCancelRequested = true;

				// [FIX cancel→share 沒反應] 必須區分「我是 sender」與「我是 receiver」：
				//   * receiver: 沒有 thread，直接清狀態、刪部份檔案、回報 finish。
				//   * sender:   sender thread 還活著，這時 *不能* 直接 usingShareSongSystem=false。
				//               否則 server 端 (DoServerCancelShare 已把 Client[me].using=false) 跟
				//               client 端旗標都歸位後，使用者馬上 /share，server 會送 NSSSS 進來，
				//               case NSSSS 看 usingShareSongSystem==false → 又生一條新的 sender thread。
				//               兩條 thread 共用 m_shareSentBytes / m_shareReceiverIndex /
				//               m_shareReceiverAckedBytes 等狀態互相蓋掉；舊 thread 結束時還會
				//               ReportShareSongFinish 讓 server 把 m_shareSenderIdx 清成 -1，
				//               server 後續收到新 thread 的 NSSProgress 全部丟掉 → UI「沒反應」。
				//               正解：讓 ThreadProcNSSSS 自己在退出時 (line ~1352) 清 usingShareSongSystem。
				if (m_recv.active)
				{
					RemovePartialRecv();
					usingShareSongSystem = false;
					ReportShareSongFinish();
				}
				else if (!usingShareSongSystem)
				{
					// 不是 sender 也不是 receiver — 收到迷路的 cancel，做基本回報
					ReportShareSongFinish();
				}
				// 否則 (usingShareSongSystem==true && !m_recv.active) 代表是 sender，
				// 不動 usingShareSongSystem，等 sender thread 自己退出時清。

				SCREENMAN->SystemMessage("Share song cancelled.");
			}
			break;
		case NSSProgress:
			{
				int senderIdx = m_packet.Read1();
				int receiverIdx = m_packet.Read1();
				int curBytes = (int)m_packet.Read4();
				int totBytes = (int)m_packet.Read4();
				int maxIdx = (senderIdx > receiverIdx ? senderIdx : receiverIdx) + 1;
				if ((int)m_PlayerShareProgress.size() < maxIdx)
					m_PlayerShareProgress.resize(maxIdx);
				bool finished = (totBytes > 0 && curBytes >= totBytes);

				// [FIX 百分比倒退] NSSProgress 有兩個來源：
				//   (a) server 收到 receiver 的 NSSXferAck 後直接廣播 (即時)
				//   (b) sender thread 每送 512KB 也 SendShareProgress 廣播 (讀的是
				//       m_shareReceiverAckedBytes，可能比 a 舊)
				// 兩條路在網路上會交錯到達，若 b 晚到就會把 UI 的進度蓋回去 → 百分比倒退。
				// 解法：同一次傳輸內，currentBytes 只能往上走。
				//
				// [FIX cancel 後再傳同檔顯示 100%]
				// 上一輪傳輸結束 / 被 cancel 時 server 會廣播 cur==total 的「完成」NSSProgress
				// (DoServerCancelShare 也會這樣做)，slot 會變成 active=false、currentBytes=totalBytes。
				// 重傳同一個檔 totalBytes 不變 → sameXfer=true → 新的 cur=很小 被
				// 「不准倒退」吃掉，看起來永遠 100%。
				// 解法：slot 已 inactive 但收到「未完成」的 NSSProgress，就是新的一輪傳輸；
				// 不要套 monotonic 防退，直接以新 cur 重置。
				auto applyMonotonic = [&](ShareProgressInfo& slot, bool uploading)
				{
					bool sameXfer = (slot.totalBytes == totBytes);
					bool newSession = !slot.active && !finished;
					int newCur = curBytes;
					if (sameXfer && !newSession && newCur < slot.currentBytes)
						newCur = slot.currentBytes; // 不准倒退
					slot.active = !finished;
					slot.uploading = uploading;
					slot.peerIndex = (uploading ? receiverIdx : senderIdx);
					slot.currentBytes = newCur;
					slot.totalBytes = totBytes;
				};
				if (senderIdx >= 0 && senderIdx < (int)m_PlayerShareProgress.size())
					applyMonotonic(m_PlayerShareProgress[senderIdx], true);
				if (receiverIdx >= 0 && receiverIdx < (int)m_PlayerShareProgress.size())
					applyMonotonic(m_PlayerShareProgress[receiverIdx], false);
			}
			break;
		case NSSXferAck:
			{
				// receiver 回報它真的收到 N bytes (經 server 轉發到 sender)
				// 我是 sender → 把這個數字塞進 m_shareReceiverAckedBytes 讓 sender thread 看
				(void)m_packet.Read1(); // senderIdx (=自己，不用)
				int recvBytes = (int)m_packet.Read4();
				int totBytes = (int)m_packet.Read4();
				if (recvBytes > m_shareReceiverAckedBytes)
					m_shareReceiverAckedBytes = recvBytes;
				// [FPS] receiver 每收 256KB 回一次 ack，5GB 約 20k 次。fsync 會卡 FPS。降為 Trace。
				LOG->Trace("[SHARE] sender: got NSSXferAck recvBytes=%d totBytes=%d (累積最高 ack=%d)",
					recvBytes, totBytes, m_shareReceiverAckedBytes);
			}
			break;
		case NSSShareLink:
			{
				// 我是 receiver。Sender 已把整首歌打包加密上傳到 temp.sh，
				// 這個 packet 告訴我 URL+密碼+資料夾名+zip 大小，我自己去下載+解壓。
				int senderIdx = m_packet.Read1();
				CString folderName = m_packet.ReadNT();
				CString url = m_packet.ReadNT();
				CString password = m_packet.ReadNT();
				int zipBytes = (int)m_packet.Read4();
				LOG->Info("[SHARE] recv: got NSSShareLink sender=%d folder='%s' url='%s' bytes=%d",
					senderIdx, folderName.c_str(), url.c_str(), zipBytes);

				if (m_downloadThreadRunning)
				{
					LOG->Warn("[SHARE] recv: download thread already running, ignore new NSSShareLink");
					break;
				}

				// 把參數塞進 m_downloadParams，然後啟一條 thread 去 curl + 解壓 (不能在
				// main thread 跑，會卡 UI / fps 跟舊 NSSData 一樣的問題)。
				m_downloadParams.senderIdx = senderIdx;
				m_downloadParams.folderName = folderName;
				m_downloadParams.url = url;
				m_downloadParams.password = password;
				m_downloadParams.totalBytes = zipBytes;
				m_downloadThreadRunning = true;
				usingShareSongSystem = true;
				m_recv.active = true;
				m_recv.senderIndex = senderIdx;
				m_recv.totalBytes = zipBytes;
				m_recv.receivedBytes = 0;
				m_recv.lastAckedBytes = 0;
				m_recv.rootDir = "";  // ThreadProcShareDownload 解壓時會填

				DWORD tid;
				HANDLE hThr = CreateThread(NULL, 0, StaticThreadStartShareDownload, this, 0, &tid);
				if (hThr) CloseHandle(hThr);
				else
				{
					LOG->Warn("[SHARE] recv: CreateThread failed for download");
					m_downloadThreadRunning = false;
					usingShareSongSystem = false;
					m_recv.active = false;
					ReportShareSongFinish();
				}
			}
			break;
		case NSCGraph:
			{
				int PlayersInPack = m_packet.Read1();
				int PlayerNum = m_packet.Read1();
				if(PlayerNum<PlayersInPack)
				{
					for(int i=0; i<NETGRAPHSIZE; i++)
					{
						m_EvalPlayerData[PlayerNum].Graph[i] = (float)m_packet.Read4()/10000;
					}
				}
			}
			break;
		case NSCPC:
			{	
				m_PlayerCondition.clear();
				int player_number = m_packet.Read1();
				for(int i=0; i<player_number; i++)
				{
					m_PlayerCondition.push_back(m_packet.Read1());
				}
				ClientNum = m_packet.Read1();
			}
		}
		m_packet.ClearPacket();
	}
}

bool NetworkSyncManager::ChangedScoreboard(int Column) 
{
	if (!m_scoreboardchange[Column])
		return false;
	m_scoreboardchange[Column]=false;
	return true;
}

void NetworkSyncManager::SendChat(const CString& message) 
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCCM );
	m_packet.WriteNT( message );
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::ReportPlayerOptions()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCUPOpts );
	FOREACH_PlayerNumber (pn)
		m_packet.WriteNT( GAMESTATE->m_PlayerOptions[pn].GetString() );
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::ReportPercentage()
{
	m_packet.ClearPacket();
	// m_packet.Write1( NSCUPOpts );
	m_packet.Write1( NSCUPPer );
	FOREACH_PlayerNumber (pn)
	{
		m_packet.WriteNT( GAMESTATE->m_PlayerPercentage[pn] );
	}
		
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::ReportGraph()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCGraph );
	FOREACH_PlayerNumber (pn)
	{
		for(int i=0; i<GameState::VALUE_RESOLUTION; i++)
		{
			m_packet.Write4( GAMESTATE->m_PlayerGraph[pn][i] );
		}
	}
		
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::SelectUserSong()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCRSG );
	m_packet.Write1( (uint8_t) m_iSelectMode );
	m_packet.WriteNT( m_sMainTitle );
	m_packet.WriteNT( m_sArtist );
	m_packet.WriteNT( m_sSubTitle );
	m_packet.Write4( m_ihash );
	SendNSMPacket(m_packet);
}

void NetworkSyncManager::SendHasSong(bool hasSong)
{
	if(hasSong)
	{
		m_packet.ClearPacket();
		m_packet.Write1( NSCCHS );
		m_packet.Write1( 1 );
		SendNSMPacket(m_packet);
	}
}

void NetworkSyncManager::SendAskSong()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCAS );
	SendNSMPacket(m_packet);
}
void NetworkSyncManager::ReportShareSongFinish()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSRSSF );
	SendNSMPacket(m_packet);
}

bool NetworkSyncManager::IsShareSongActive() const
{
	return usingShareSongSystem || m_recv.active;
}

// 主動取消：先設 sender thread 的旗標，再透過 server 廣播 cancel
void NetworkSyncManager::CancelShareSong()
{
	LOG->Info("[SHARE] CancelShareSong called (using=%d recv=%d)",
		(int)usingShareSongSystem, (int)m_recv.active);
	m_shareCancelRequested = true;
	if (m_recv.active) RemovePartialRecv();
	SendShareCancel();
	// [FIX cancel→share 沒反應] 同 case NSSCancel：sender thread 還活著時 *不能* 立刻
	// usingShareSongSystem=false，否則下一個 /share 會撞出雙 sender thread。詳細註解見
	// ProcessInput 內 case NSSCancel。
	if (m_recv.active || !usingShareSongSystem)
	{
		usingShareSongSystem = false;
		ReportShareSongFinish();
	}
	// else: 我是 sender，sender thread 還在 — 由 ThreadProcNSSSS 退出時自己清。
}

// 純粹只送 NSSCancel；server 端 (/cancel chat command) 也可呼叫
void NetworkSyncManager::SendShareCancel()
{
	if (!useSMserver) return;
	PacketFunctions pkt; pkt.ClearPacket();
	pkt.Write1(NSSCancel);
	pkt.Write1((uint8_t)m_shareReceiverIndex);
	SendNSMPacket(pkt);
}

//Packet functions

uint8_t PacketFunctions::Read1()
{
	if (Position>=NETMAXBUFFERSIZE)
		return 0;
	
	return Data[Position++];
}

uint16_t PacketFunctions::Read2()
{
	if (Position>=NETMAXBUFFERSIZE-1)
		return 0;

	uint16_t Temp;
	memcpy( &Temp, Data + Position,2 );
	Position+=2;		
	return ntohs(Temp);	
}

uint32_t PacketFunctions::Read4()
{
	if (Position>=NETMAXBUFFERSIZE-3)
		return 0;

	uint32_t Temp;
	memcpy( &Temp, Data + Position,4 );
	Position+=4;
	return ntohl(Temp);
}

CString PacketFunctions::ReadNT()
{
	//int Orig=Packet.Position;
	CString TempStr;
	while ((Position<NETMAXBUFFERSIZE)&& (((char*)Data)[Position]!=0))
		TempStr= TempStr + (char)Data[Position++];

	++Position;
	return TempStr;
}


void PacketFunctions::Write1(uint8_t data)
{
	if (Position>=NETMAXBUFFERSIZE)
		return;
	memcpy( &Data[Position], &data, 1 );
	++Position;
}

void PacketFunctions::Write2(uint16_t data)
{
	if (Position>=NETMAXBUFFERSIZE-1)
		return;
	data = htons(data);
	memcpy( &Data[Position], &data, 2 );
	Position+=2;
}

void PacketFunctions::Write4(uint32_t data)
{
	if (Position>=NETMAXBUFFERSIZE-3)
		return ;

	data = htonl(data);
	memcpy( &Data[Position], &data, 4 );
	Position+=4;
}

void PacketFunctions::WriteNT(const CString& data)
{
	int index=0;
	while ((Position<NETMAXBUFFERSIZE)&&(index<data.GetLength()))
		Data[Position++] = (unsigned char)(data.c_str()[index++]);
	Data[Position++] = 0;
}

void PacketFunctions::WriteBytes(const char *src, int bytes)
{
	if (bytes <= 0) return;
	int room = NETMAXBUFFERSIZE - Position;
	if (room <= 0) return;
	int n = bytes < room ? bytes : room;
	memcpy(&Data[Position], src, n);
	Position += n;
}

int PacketFunctions::ReadBytes(char *out, int bytes)
{
	if (bytes <= 0) return 0;
	int room = NETMAXBUFFERSIZE - Position;
	if (room <= 0) return 0;
	int n = bytes < room ? bytes : room;
	memcpy(out, &Data[Position], n);
	Position += n;
	return n;
}

void PacketFunctions::ClearPacket()
{
	memset((void*)(&Data),0, NETMAXBUFFERSIZE);
	Position = 0;
	PayloadLength = 0;
}
#endif

LuaFunction_NoArgs( IsNetConnected,			NSMAN->useSMserver )

/*
 * (c) 2003-2004 Charles Lohr, Joshua Allen
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
