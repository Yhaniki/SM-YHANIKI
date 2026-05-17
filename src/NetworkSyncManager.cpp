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
	usingShareSongSystem=false;
	ClientNum=0;
	m_shareCancelRequested = false;
	m_shareSentBytes = 0;
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
	NetPlayerClient->SendPack((char*)m_packet.Data,m_packet.Position);
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
	NetPlayerClient->SendPack((char*)m_packet.Data, m_packet.Position);
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

	NetPlayerClient->SendPack((char*)m_packet.Data, m_packet.Position); 

}

void NetworkSyncManager::ReportSongOver() 
{
	if (!useSMserver)	//Make sure that we are using the network
		return ;

	m_packet.ClearPacket();

	m_packet.Write1( NSCGON );

	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
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

	NetPlayerClient->SendPack( (char*)&m_packet.Data, m_packet.Position );
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
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
	
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

void NetworkSyncManager::SendShareProgress()
{
	if (!useSMserver) return;
	WaitForSingleObject(g_hMutex, INFINITE);
	PacketFunctions pkt; pkt.ClearPacket();
	pkt.Write1(NSSProgress);
	pkt.Write1((uint8_t)m_shareReceiverIndex);
	pkt.Write4((uint32_t)m_shareSentBytes);
	pkt.Write4((uint32_t)m_shareTotalBytes);
	NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
	ReleaseMutex(g_hMutex);
}

DWORD NetworkSyncManager::ThreadProcNSSSS(void)
{
	LOG->Info("[SHARE] sender thread start. receiver=%d filter=%d", player_num, (int)video_file_filter);
	m_shareReceiverIndex = player_num;
	m_shareCancelRequested = false;
	m_shareSentBytes = 0;
	m_shareTotalBytes = 0;

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

	// 2. 列出所有檔案、計算總 bytes
	vector<CString> relPaths;
	vector<uint32_t> sizes;
	EnumerateFilesRecursive(songDirPath, "", video_file_filter, relPaths, sizes);
	uint32_t totalBytes = 0;
	for (size_t i = 0; i < sizes.size(); ++i) totalBytes += sizes[i];
	m_shareTotalBytes = (int)totalBytes;
	LOG->Info("[SHARE] sender: %u files, total %u bytes", (unsigned)relPaths.size(), totalBytes);

	// 3. 送 NSSMeta：receiver_idx + 資料夾名 + 檔案數 + 總 bytes
	// 用 local packet 避免和 main thread 的 m_packet 競爭。
	{
		PacketFunctions pkt; pkt.ClearPacket();
		pkt.Write1(NSSMeta);
		pkt.Write1((uint8_t)player_num);
		pkt.WriteNT(songFolderName);
		pkt.Write4((uint32_t)relPaths.size());
		pkt.Write4(totalBytes);
		WaitForSingleObject(g_hMutex, INFINITE);
		NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
		ReleaseMutex(g_hMutex);
	}

	// 4. 一個一個檔案傳出去，依 NETSHARECHUNKSIZE 切塊
	char buf[NETSHARECHUNKSIZE];
	int lastProgressReport = 0;
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
			WaitForSingleObject(g_hMutex, INFINITE);
			NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
			ReleaseMutex(g_hMutex);

			offset += got;
			m_shareSentBytes += got;

			// 約每 64KB 回報一次進度，避免訊息洪水
			if (m_shareSentBytes - lastProgressReport >= 64 * 1024)
			{
				SendShareProgress();
				lastProgressReport = m_shareSentBytes;
			}

			// 稍微 yield 一下，避免吃滿 CPU 並讓 main thread 有空處理 callbacks
			Sleep(1);
		}
		if (fp) fclose(fp);
	}

	// 5. 廣播最後一次進度與結束/取消
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
			LOG->Info("[SHARE] sender: send NSSDone to receiver=%d", player_num);
		}
		WaitForSingleObject(g_hMutex, INFINITE);
		NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
		ReleaseMutex(g_hMutex);
	}

	usingShareSongSystem = false;
	m_shareReceiverIndex = -1;
	m_shareCancelRequested = false;
	ReportShareSongFinish();
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
}

void NetworkSyncManager::CloseRecvFile()
{
	if (m_recv.currentFile)
	{
		fclose(m_recv.currentFile);
		m_recv.currentFile = NULL;
	}
	m_recv.currentRelPath = "";
	m_recv.currentFileSize = 0;
	m_recv.currentFileWritten = 0;
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
	m_recv.currentFile = fopen(full.c_str(), "wb");
	if (!m_recv.currentFile)
	{
		LOG->Warn("[SHARE] recv: cannot create '%s'", full.c_str());
	}
	m_recv.currentRelPath = relPath;
	m_recv.currentFileSize = fileSize;
	m_recv.currentFileWritten = 0;
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

	m_packet.ClearPacket();

	while (NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE)>0)
	{
		int command = m_packet.Read1();
		LOG->Info("[NETDBG] NSM::ProcessInput#2 got command raw=%d (server offset cmd=%d)",
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
			NetPlayerClient->SendPack((char*)m_packet.Data,m_packet.Position);
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
				m_recv.rootDir = connectFolder + "\\" + folderName;
				EnsureDirectoryExists(m_recv.rootDir);

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
					fwrite(buf, 1, gotBytes, m_recv.currentFile);
					m_recv.currentFileWritten = offset + gotBytes;
					if (m_recv.currentFileWritten >= m_recv.currentFileSize)
						fflush(m_recv.currentFile);
				}
				m_recv.receivedBytes += gotBytes;
			}
			break;
		case NSSDone:
			{
				int sender = m_packet.Read1();
				LOG->Info("[SHARE] recv: NSSDone sender=%d received=%d/%d",
					sender, m_recv.receivedBytes, m_recv.totalBytes);
				CloseRecvFile();
				ResetRecvState();
				usingShareSongSystem = false;
				ReportShareSongFinish();
				SCREENMAN->SendMessageToTopScreen(SM_ReloadConnectPack);
			}
			break;
		case NSSCancel:
			{
				int peer = m_packet.Read1();
				LOG->Info("[SHARE] cancel from peer=%d (sender? %d, receiving? %d)",
					peer, (int)usingShareSongSystem, (int)m_recv.active);
				if (m_recv.active) RemovePartialRecv();
				m_shareCancelRequested = true;
				usingShareSongSystem = false;
				ReportShareSongFinish();
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
				if (senderIdx >= 0 && senderIdx < (int)m_PlayerShareProgress.size())
				{
					m_PlayerShareProgress[senderIdx].active = !finished;
					m_PlayerShareProgress[senderIdx].uploading = true;
					m_PlayerShareProgress[senderIdx].peerIndex = receiverIdx;
					m_PlayerShareProgress[senderIdx].currentBytes = curBytes;
					m_PlayerShareProgress[senderIdx].totalBytes = totBytes;
				}
				if (receiverIdx >= 0 && receiverIdx < (int)m_PlayerShareProgress.size())
				{
					m_PlayerShareProgress[receiverIdx].active = !finished;
					m_PlayerShareProgress[receiverIdx].uploading = false;
					m_PlayerShareProgress[receiverIdx].peerIndex = senderIdx;
					m_PlayerShareProgress[receiverIdx].currentBytes = curBytes;
					m_PlayerShareProgress[receiverIdx].totalBytes = totBytes;
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
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
}

void NetworkSyncManager::ReportPlayerOptions()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCUPOpts );
	FOREACH_PlayerNumber (pn)
		m_packet.WriteNT( GAMESTATE->m_PlayerOptions[pn].GetString() );
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
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
		
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
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
		
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
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
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position);
}

void NetworkSyncManager::SendHasSong(bool hasSong)
{
	if(hasSong)
	{
		m_packet.ClearPacket();
		m_packet.Write1( NSCCHS );
		m_packet.Write1( 1 );
		NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position);
	}
}

void NetworkSyncManager::SendAskSong()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSCAS );
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position);
}
void NetworkSyncManager::ReportShareSongFinish()
{
	m_packet.ClearPacket();
	m_packet.Write1( NSRSSF );
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position);
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
	usingShareSongSystem = false;
	ReportShareSongFinish();
}

// 純粹只送 NSSCancel；server 端 (/cancel chat command) 也可呼叫
void NetworkSyncManager::SendShareCancel()
{
	if (!useSMserver) return;
	PacketFunctions pkt; pkt.ClearPacket();
	pkt.Write1(NSSCancel);
	pkt.Write1((uint8_t)m_shareReceiverIndex);
	WaitForSingleObject(g_hMutex, INFINITE);
	NetPlayerClient->SendPack((char*)pkt.Data, pkt.Position);
	ReleaseMutex(g_hMutex);
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
