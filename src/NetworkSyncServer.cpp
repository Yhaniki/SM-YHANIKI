#include "global.h"
#include "NetworkSyncServer.h"
#include "RageLog.h"
#include "PrefsManager.h"
#include <time.h>
#include <windows.h>
#include <unordered_set>

#if defined(WITHOUT_NETWORKING)
bool StepManiaLanServer::ServerStart() { return false; }
void StepManiaLanServer::ServerStop() { }
void StepManiaLanServer::ServerUpdate() { }
StepManiaLanServer::StepManiaLanServer() { }
StepManiaLanServer::~StepManiaLanServer() { }
#else

// ============================================================
// 指令字串集中定義 (改字串只要改這裡，避免散落各處 typo)
// ============================================================
#define CMD_SHARE      "share"
#define CMD_SHAREFULL  "sharefull"
#define CMD_LIST       "list"
#define CMD_HAVE       "have"
#define CMD_CANCEL     "cancel"
#define CMD_HELP       "help"
#define CMD_CODE       "code"
#define CMD_START      "start"
#define CMD_KICK       "kick"
#define CMD_BAN        "ban"
#define CMD_HOST       "host"

// 指令說明表，給 /help 用；新增指令時記得在這裡補一筆
struct ServerCommandInfo
{
	const char* name;        // 指令字串 (不含斜線)
	bool        hostOnly;    // 是否限 host (clientNum == 0) 才能執行
	const char* description; // /help 顯示的說明
};

static const ServerCommandInfo g_serverCommands[] =
{
	{ CMD_SHARE,     false, "/share - share current song to one player" },
	{ CMD_SHAREFULL, false, "/sharefull - share current song to all players" },
	{ CMD_LIST,      false, "/list - list players in the room" },
	{ CMD_HAVE,      false, "/have - mark you have the song" },
	{ CMD_CANCEL,    false, "/cancel - cancel ongoing share transfer" },
	{ CMD_HELP,      false, "/help - show this help" },
	{ CMD_CODE,      false, "/code - show the room code" },
	{ CMD_START,     true,  "/start (host) - force start the game" },
	{ CMD_KICK,      true,  "/kick <name> (host) - kick a player" },
	{ CMD_BAN,       true,  "/ban <name> (host) - ban a player" },
	{ CMD_HOST,      true,  "/host <name> (host) - transfer host" },
};

std::unordered_set<std::string> cmdList =
	{ CMD_SHARE, CMD_SHAREFULL, CMD_LIST, CMD_HAVE, CMD_CANCEL, CMD_HELP, CMD_CODE };

std::unordered_set<std::string> hostCmdList =
	{ CMD_START, CMD_KICK, CMD_BAN, CMD_HOST };

// 簡單把整個 packet 直接 forward 給某個 client，不重新 parse。
// 注意：呼叫前必須先把 cmd byte 寫進 newPacket (加上 NSServerOffset 區別 server 端發出)。
static void ForwardPacketBytes(PacketFunctions& out, const unsigned char* data, int bytes)
{
	for (int i = 0; i < bytes; ++i)
	{
		if (out.Position >= NETMAXBUFFERSIZE) break;
		out.Data[out.Position++] = data[i];
	}
}

LanPlayer::LanPlayer()
{
	score = 0;
	health = 0;
	feet = 0;
	projgrade = 0;
	combo = 0;
	currstep = 0;
	maxCombo = 0;
	Grade = 0;
	offset = 0;
	options = "";
	percentage = "";
	for (int i = 0; i < NETGRAPHSIZE; i++)
	{
		Graph[i] = 0;
	}
}

StepManiaLanServer::StepManiaLanServer()
{
	stop = true;
	SecondSameSelect = false;
	ChangeHost = false;
	ClientHost = -1; // [NETDBG] 原本沒初始化，會是垃圾值
	m_shareSenderIdx = -1;
	m_shareReceiverIdx = -1;
	m_shareCurBytes = 0;
	m_shareTotalBytes = 0;
	m_shareLastActivityMs = 0;
	AssignPlayerIDs();
}

StepManiaLanServer::~StepManiaLanServer()
{
	ServerStop();
}

//Generate a five-digit room number
std::string StepManiaLanServer::GenerateRoomCode()
{
	int code = 1000 + std::rand() % 90000;
	return std::string(5 - std::to_string(code).length(), '0') + std::to_string(code);
}

bool StepManiaLanServer::ServerStart()
{
	server.blocking = 0; /* Turn off blocking */
	if (server.create())
		if (server.bind(8765))
			if (server.listen())
			{
				stop = false;
				statsTime = time(NULL);
				return true;
			}
			else
				lastError = "Failed to make socket listen.";
		else
			lastError = "Failed to bind socket";
	else
		lastError = "Failed to create socket";

	lastErrorCode = server.lastCode;
	//Hopefully we will not get here. If we did, something went wrong above.
	return false;
}

bool StepManiaLanServer::ServerStart(CString roomCode)
{
	if(server.create(roomCode))
	{
		stop = false;
		statsTime = time(NULL);
		return true;
	}
	return false;
}

void StepManiaLanServer::ServerStop()
{
	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		delete Client[x];
		Client[x] = NULL;
	}

	Client.clear();
	server.close();
	stop = true;
}

void StepManiaLanServer::ServerUpdate()
{
	if (!stop)
	{
		NewClientCheck(); /* See if there is another client wanting to play */
		UpdateClients();
		if (time(NULL) > statsTime)
		{
			SendStatsToClients();
			statsTime = time(NULL);
		}
		CheckShareTimeout();
	}
}

void StepManiaLanServer::UpdateClients()
{
	//Go through all the clients and check to see if it is being used.
	//If so then try to get a backet and parse the data.
	const size_t clientCount = Client.size();
	for (unsigned int x = 0; x < clientCount; ++x)
	{
		if (x >= Client.size())
		{
			LOG->Warn("[NETDBG] SRV::UpdateClients#1 index x=%u >= size=%u, aborting loop", x, (unsigned)Client.size());
			break;
		}
		if (Client[x] == nullptr)
		{
			LOG->Warn("[NETDBG] SRV::UpdateClients#2 Client[%u] is null, skipping", x);
			continue;
		}
		if (CheckConnection(x))
		{
			int got = Client[x]->GetData(Packet);
			if (got >= 0)
			{
				LOG->Info("[NETDBG] SRV::UpdateClients#3 client=%u got %d bytes -> ParseData", x, got);
				ParseData(Packet, x);
			}
		}
	}
}

GameClient::GameClient()
{
	GotStartRequest = 0;
	clientSocket.blocking = 0;
	twoPlayers = false;
	version = 0;
	startPosition = 0;
	InGame = 0;
	hasSong = forceHas = false;
	inNetMusicSelect = false;
	isStarting = false;  //Used for after ScreenNetMusicSelect but before InGame
	wasIngame = false;
	lowerJudge = false;
	shareAll = false;
	usingShareSongSystem = false;
	filefilter = true;
	ShareNum = 0;
}

void StepManiaLanServer::Disconnect(const unsigned int clientNum)
{
	LOG->Info("[NETDBG] SRV::Disconnect#1 begin clientNum=%u size=%u",
		clientNum, (unsigned)Client.size());
	if (Client.empty() || clientNum >= Client.size())
	{
		LOG->Warn("[NETDBG] SRV::Disconnect#2 invalid clientNum=%u (size=%u), abort",
			clientNum, (unsigned)Client.size());
		return;
	}

	if (clientNum == (Client.size()-1))//host leave
	{
		LOG->Info("[NETDBG] SRV::Disconnect#3 last index path (host leave)");
		delete Client[Client.size()-1];
		Client[Client.size()-1] = NULL;
		Client.pop_back();
		ClearHasSong();
	}
	else
	{
		LOG->Info("[NETDBG] SRV::Disconnect#4 erase via iterator");
		vector<GameClient*>::iterator Iterator;
		Iterator = Client.begin();
		for (unsigned int x = 0; x < Client.size(); ++x)
		{
			if (x == clientNum)
			{
				delete Client[x];
				Client[x] = NULL;
				Client.erase(Iterator);
				break; // [NETDBG] safer: stop after erase to avoid invalidated iterator
			}
			++Iterator;
		}
	}
	LOG->Info("[NETDBG] SRV::Disconnect#5 erased, new size=%u, sending UserList+Cond",
		(unsigned)Client.size());
	SendUserList();
	SendPlayerCondition();
	LOG->Info("[NETDBG] SRV::Disconnect#6 done");
}

int GameClient::GetData(PacketFunctions& Packet)
{
	int length = -1;
	Packet.ClearPacket();
	length = clientSocket.ReadPack((char*)Packet.Data, NETMAXBUFFERSIZE);
	Packet.PayloadLength = (length > 0) ? length : 0;
	if (length > 0)
		LOG->Info("[NETDBG] SRV::GetData got %d bytes from a client", length);
	return length;
}

void StepManiaLanServer::ParseData(PacketFunctions& Packet, const unsigned int clientNum)
{
	if (clientNum >= Client.size() || Client[clientNum] == nullptr)
	{
		LOG->Warn("[NETDBG] SRV::ParseData#0 invalid clientNum=%u (size=%u), abort",
			clientNum, (unsigned)Client.size());
		return;
	}
	int command = Packet.Read1();
	LOG->Info("[NETDBG] SRV::ParseData#1 client=%u cmd=%d", clientNum, command);
	switch (command)
	{
	case NSCPing:
		// No Operation
		SendValue(NSServerOffset + NSCPingR, clientNum);
		break;
	case NSCPingR:
		// No Operation response
		break;
	case NSCHello:
		// Hello
		Hello(Packet, clientNum);
		break;
	case NSCGSR:
		// Start Request
		Client[clientNum]->StartRequest(Packet);
		CheckReady();  //This is what ACTUALLY starts the games
		SendPlayerCondition();
		break;
	case NSCGON:
		// GameOver 
		GameOver(Packet, clientNum);
		ClearHasSong();
		SendPlayerCondition();
		break;
	case NSCGSU:
		// StatsUpdate
		Client[clientNum]->UpdateStats(Packet);
		if (!Client[clientNum]->lowerJudge)
			CheckLowerJudge(clientNum);
		SendPlayerCondition();
		break;
	case NSCSU:
		// Style Update
		Client[clientNum]->StyleUpdate(Packet);
		SendUserList();
		SendPlayerCondition();
		break;
	case NSCCM:
		// Chat message
		AnalizeChat(Packet, clientNum);
		break;
	case NSCRSG:
		SelectSong(Packet, clientNum);
		SendPlayerCondition();
		break;
	case NSCSMS:
		ScreenNetMusicSelectStatus(Packet, clientNum);
		SendPlayerCondition();
		break;
	case NSCUPOpts:
		Client[clientNum]->Player[0].options = Packet.ReadNT();		
		Client[clientNum]->Player[1].options = Packet.ReadNT();		
		break;
	case NSCUPPer:
		Client[clientNum]->Player[0].percentage = Packet.ReadNT();
		Client[clientNum]->Player[1].percentage = Packet.ReadNT();
		break;
	case NSSSC:
		{
			CString server_ip = Packet.ReadNT();
			int client_index = Packet.Read1();
			int file_size = Packet.Read4();
			// LOG->Info("NSSSC server_ip %s",server_ip.c_str());
			// LOG->Info("NSSSC file_size %d",file_size);
			// LOG->Info("NSSSC client_index %d",client_index);

			Reply.ClearPacket();
			Reply.Write1(NSSSC + NSServerOffset);
			Reply.WriteNT(server_ip);
			Reply.Write4(file_size);
			SendNetPacket(client_index, Reply);

			LastSongInfo.title="";
			LastSongInfo.artist="";
			LastSongInfo.subtitle="";//if sent file success, ask "play?" again
			if(client_index<Client.size())
			{
				Client[client_index]->usingShareSongSystem=true;
			}
		}
		break;
	case NSCGraph:
		ServerGetGraph(Packet, clientNum);
		break;
	case NSCCHS:
		GetHasSong(Packet, clientNum);
		SendPlayerCondition();
		break;
	case NSCAS:
		GetAskSong(Packet, clientNum);
		break;
	case NSRSSF:
		Client[clientNum]->usingShareSongSystem = false;
		// 若這個 client 是目前 active 的 sender/receiver 就清掉 server-side state
		if ((int)clientNum == m_shareSenderIdx || (int)clientNum == m_shareReceiverIdx)
		{
			m_shareSenderIdx = -1;
			m_shareReceiverIdx = -1;
			m_shareCurBytes = 0;
			m_shareTotalBytes = 0;
			m_shareLastActivityMs = 0;
		}
		if(Client[clientNum]->shareAll)
		{
			ShareAll(clientNum, Packet.fromIp);
		}
		break;
	case NSSMeta:
	case NSSData:
	case NSSDone:
		// sender 送來的檔案資料/控制訊息，server 直接轉發給 receiver
		ForwardShareToReceiver(Packet, command, clientNum);
		break;
	case NSSCancel:
		{
			// 任何一方都可以送 cancel 過來。server 兩邊都轉發、並清自己的狀態
			ForwardShareToReceiver(Packet, command, clientNum);
			if (m_shareSenderIdx == (int)clientNum || m_shareReceiverIdx == (int)clientNum)
			{
				DoServerCancelShare("cancelled by client");
			}
		}
		break;
	case NSSProgress:
		{
			int receiverIdx = Packet.Read1();
			int curBytes = (int)Packet.Read4();
			int totBytes = (int)Packet.Read4();
			(void)receiverIdx; // sender 自己回報，receiver index 同時也記在 server 自己的 state
			BroadcastShareProgress(clientNum, curBytes, totBytes);
		}
		break;
	default:
		break;
	}
}	 

void StepManiaLanServer::Hello(PacketFunctions& Packet, const unsigned int clientNum)
{
	int ClientVersion = Packet.Read1();
	CString build = Packet.ReadNT();
	LOG->Info("[NETDBG] SRV::Hello#1 client=%u version=%d build='%s'",
		clientNum, ClientVersion, build.c_str());

	if (clientNum >= Client.size() || Client[clientNum] == nullptr)
	{
		LOG->Warn("[NETDBG] SRV::Hello#2 invalid clientNum=%u, abort", clientNum);
		return;
	}
	Client[clientNum]->SetClientVersion(ClientVersion, build);

	Reply.ClearPacket();
	Reply.Write1( NSCHello + NSServerOffset );
	Reply.Write1(1);
	Reply.WriteNT(servername);

	LOG->Info("[NETDBG] SRV::Hello#3 reply NSCHello to client=%u", clientNum);
	SendNetPacket(clientNum, Reply);

	if (ClientHost == -1)
		ClientHost = clientNum;
	LOG->Info("[NETDBG] SRV::Hello#4 done, ClientHost=%d", ClientHost);
}

void GameClient::StyleUpdate(PacketFunctions& Packet)
{
	int playernumber = 0;
	Player[0].name = Player[1].name = "";
	twoPlayers = Packet.Read1()-1;
	for (int x = 0; x < twoPlayers+1; ++x)
	{
		playernumber = Packet.Read1();
		Player[playernumber].name = Packet.ReadNT();
	}
}

void GameClient::SetClientVersion(int ver, const CString& b)
{
	version = ver;
	build = b;
}

void GameClient::StartRequest(PacketFunctions& Packet)
{
	int firstbyte = Packet.Read1();
	int secondbyte = Packet.Read1();
	int thirdbyte = Packet.Read1();
	Player[0].feet = firstbyte/16;
	Player[1].feet = firstbyte%16;

	if ((Player[0].feet > 0)&&(Player[1].feet > 0))
		twoPlayers = true;

	Player[0].diff = secondbyte/16;
	Player[1].diff = secondbyte%16;

	startPosition = thirdbyte/16;
	gameInfo.title = Packet.ReadNT();
	gameInfo.subtitle = Packet.ReadNT();
	gameInfo.artist = Packet.ReadNT();
	gameInfo.course = Packet.ReadNT();

	for (int x = 0; x < 2; ++x)
	 {
		Player[x].score = 0;
		Player[x].combo = 0;
		Player[x].projgrade = 0;
		Player[x].maxCombo = 0;

		memset(Player[x].steps, 0, sizeof(int)*9);
	}

	GotStartRequest = true;
}

void StepManiaLanServer::CheckReady()
{
	bool canStart = true;
	unsigned int x;

	//Only check clients that are starting (after ScreenNetMusicSelect before InGame).
	for (x = 0; (x < Client.size()) && canStart; ++x)
	{
			if (Client[x]->isStarting && !Client[x]->GotStartRequest)
				canStart = false;

			//Start for courses
			if (!Client[x]->inNetMusicSelect && !Client[x]->hasSong && Client[x]->GotStartRequest)
				canStart = true;
	}
			
	if (canStart)
	{
		//(Test this) 
		//For whatever reason we need to pause in a way
		//that will not use a lot of CPU.
		//When you try playing the music as soon as it's loaded
		//it will not always play ... immediately
		usleep ( 2000000 );

		//The next three loops are seperate because we want to minimize what is done
		//during the actual loop that starts the clients. This is in an atempt
		//to start all the clients as close together as possible.
		for (x = 0; x < Client.size(); ++x)
		{
			if (Client[x]->isStarting)
			{
				Client[x]->clientSocket.blocking = true;
				Client[x]->GotStartRequest = false;
			}

			//For Start for courses
			if (!Client[x]->inNetMusicSelect && !Client[x]->hasSong && Client[x]->GotStartRequest)
			{
				Client[x]->clientSocket.blocking = true;
				Client[x]->GotStartRequest = false;
			}
		}
		
		//Start clients waiting for a start between ScreenNetMusicSelect and the game.
		for (x = 0; x < Client.size(); ++x)
		{
			if (Client[x]->isStarting)
				SendValue(NSCGSR + NSServerOffset, x);

			//For Start for courses
			if (!Client[x]->inNetMusicSelect && !Client[x]->hasSong)
				SendValue(NSCGSR + NSServerOffset, x);	
		}

		for (x = 0; x < Client.size(); ++x)
		{
			if (Client[x]->isStarting)
			{
				if (Client[x]->startPosition == 1)
				{
					Client[x]->isStarting = false;
					Client[x]->InGame = true;
					Client[x]->lowerJudge = false;
					//After we start the clients, clear each client's hasSong.
					Client[x]->hasSong = false;
				}
				Client[x]->clientSocket.blocking = false;
			}

			//For Start for courses
			if (!Client[x]->inNetMusicSelect && !Client[x]->hasSong)
			{
				if (Client[x]->startPosition == 1)
				{
					Client[x]->isStarting = false;
					Client[x]->InGame = true;
					Client[x]->lowerJudge = false;
					//After we start the clients, clear each client's hasSong.
					Client[x]->hasSong = false;
				}
				Client[x]->clientSocket.blocking = false;
			}
		}
	}
}

void StepManiaLanServer::GameOver(PacketFunctions& Packet, const unsigned int clientNum)
{
	bool allOver = true;
	unsigned int x;

	unsigned int numPlayers = playersPtr.size();

	Client[clientNum]->hasSong = Client[clientNum]->forceHas = 0;
	Client[clientNum]->GotStartRequest = false;
	Client[clientNum]->InGame = false;
	Client[clientNum]->wasIngame = true;

	for (x = 0; (x < Client.size())&&allOver ; ++x)
		if (Client[x]->InGame)
			allOver = false;

	//Wait until everyone is done before sending
	if (allOver)
	{
		for (x = 0; x < Client.size(); ++x)
			if (Client[x]->wasIngame && Client[x]->lowerJudge)
				for (int y = 0; y < 2; ++y)
					Client[x]->Player[y].options = "TIMING " + playersPtr[x]->options;

		SortStats(playersPtr);
		Reply.ClearPacket();
		Reply.Write1( NSCGON + NSServerOffset );
		Reply.Write1( (uint8_t) numPlayers );
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write1((uint8_t)playersPtr[x]->PlayerID);
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write4(playersPtr[x]->score);
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write1( (uint8_t) playersPtr[x]->projgrade );
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write1( (uint8_t) playersPtr[x]->diff );
		for (int y = 6; y >= 1; --y)
			for (x = 0; x < numPlayers; ++x)
				Reply.Write2( (uint16_t) playersPtr[x]->steps[y] );
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write2( (uint16_t) playersPtr[x]->steps[8] );  //Tack on OK
		for (x = 0; x < numPlayers; ++x) 
			Reply.Write2( (uint16_t) playersPtr[x]->maxCombo );
		for (x = 0; x < numPlayers; ++x)
		{
			Reply.WriteNT( playersPtr[x]->options );
			// Reply.WriteNT( playersPtr[x]->percentage );
		}
		for (x = 0; x < numPlayers; ++x)
		{
			Reply.WriteNT( playersPtr[x]->percentage );
		}
		for (x = 0; x < Client.size(); ++x)
			if(Client[x]->wasIngame)
			{
				SendNetPacket(x, Reply);
				Client[x]->wasIngame = false;
			}
		//============
		for(x = 0; x < numPlayers; ++x)
		{
			Reply.ClearPacket();
			Reply.Write1( NSCGraph + NSServerOffset );
			Reply.Write1( (uint8_t) numPlayers );
			Reply.Write1( (uint8_t) x );
			for (int i=0; i<NETGRAPHSIZE; i++)
			{
				Reply.Write4( playersPtr[x]->Graph[i] );
			}
			for(int j=0; j<numPlayers; ++j)
			{
				SendNetPacket(j, Reply);
			}
		}
		//============
	}
}

void StepManiaLanServer::ServerGetGraph(PacketFunctions& Packet, unsigned int clientNum)
{
	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < NETGRAPHSIZE; j++)
		{
			Client[clientNum]->Player[i].Graph[j] = Packet.Read4();
		}
	}
}

void StepManiaLanServer::GetHasSong(PacketFunctions&Packet, unsigned int clientNum)
{
	if(clientNum==0)return;
	else
	{
		int gethasSong = Packet.Read1();
		if(gethasSong)
		{
			Client[clientNum]->hasSong=true;
		}
	}
}
void StepManiaLanServer::GetAskSong(PacketFunctions&Packet, unsigned int clientNum)
{
	if(clientNum==0)return;
	else
	{
		if(Client[0]->hasSong&&CurrentSongInfo.title!="")
		{
			Reply.ClearPacket();
			Reply.Write1(NSCRSG + NSServerOffset);
			Reply.Write1(1);
			Reply.WriteNT(CurrentSongInfo.title);
			Reply.WriteNT(CurrentSongInfo.artist);
			Reply.WriteNT(CurrentSongInfo.subtitle);
			Reply.Write4(CurrentSongInfo.hash);	
			if (Client[clientNum]->inNetMusicSelect)
				SendNetPacket(clientNum, Reply);	
		}
	}
}
void StepManiaLanServer::AssignPlayerIDs()
{
	unsigned int counter = 0;
	//Future: Figure out how to do dynamic numbering.
	for (unsigned int x = 0; x < Client.size(); ++x)
		for(int y = 0; y < 2; ++y)
			Client[x]->Player[y].PlayerID = counter++;
}

void StepManiaLanServer::PopulatePlayersPtr(vector<LanPlayer*> &playersPtr) {

	for (unsigned int x = 0; x < playersPtr.size(); ++x)
		playersPtr[x] = NULL;

	playersPtr.clear();

	//Populate with in game players only
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (Client[x]->InGame||Client[x]->wasIngame)
			for (int y = 0; y < 2; ++y)
				if (Client[x]->IsPlaying(y))
					playersPtr.push_back(&Client[x]->Player[y]);
}

int StepManiaLanServer::SortStats(vector<LanPlayer*> &playersPtr)
{
	LanPlayer *tmp;
	bool isChanged;

	PopulatePlayersPtr(playersPtr);

	do
	{
		isChanged = false;
		for (int x = 0; x < int(playersPtr.size())-1; ++x)
			if ((playersPtr[x]->score) < (playersPtr[x+1]->score))
			{
				tmp = playersPtr[x];
				playersPtr[x] = playersPtr[x+1];
				playersPtr[x+1] = tmp;
				isChanged = true;
			}
	} while (isChanged);

	return playersPtr.size();
}

void StepManiaLanServer::SendStatsToClients()
{
	unsigned int x;

	SortStats(playersPtr); //Return number of players

	/* Write and Send name packet */
	Reply.ClearPacket();
	Reply.Write1(NSCGSU + NSServerOffset);
	Reply.Write1(0);
	Reply.Write1( (uint8_t) playersPtr.size());
	StatsNameColumn(Reply, playersPtr);

	//Send to in game clients only.
	for (x = 0; x < Client.size(); ++x)
		if (Client[x]->InGame)
			SendNetPacket(x, Reply);


	/* Write and send Combo packet */
	Reply.ClearPacket();

	Reply.Write1(NSCGSU + NSServerOffset);
	Reply.Write1(1);
	Reply.Write1( (uint8_t) playersPtr.size() );
	StatsComboColumn(Reply, playersPtr);

	//Send to in game clients only.
	for (x = 0; x < Client.size(); ++x)
		if (Client[x]->InGame)
			SendNetPacket(x, Reply);
	

	/* Write and send projgrade packet*/
	//Is it worth the programing troube to save a small amount of bandwidth here?
	//Probably not. Sends all everytime unless developer feelings change.
	Reply.ClearPacket();

	Reply.Write1(NSCGSU + NSServerOffset);
	Reply.Write1(2);
	Reply.Write1( (uint8_t) playersPtr.size());
	StatsProjgradeColumn(Reply, playersPtr);

	//Send to in game clients only.
	for (x = 0; x < Client.size(); ++x)
		if (Client[x]->InGame)
			SendNetPacket(x, Reply);

}

void StepManiaLanServer::SendNetPacket(const unsigned int client, PacketFunctions& Packet)
{
	if ( client >= Client.size() )
	{
		LOG->Warn("[NETDBG] SRV::SendNetPacket#1 client=%u >= size=%u, abort",
			client, (unsigned)Client.size());
		return;
	}
	if (Client[client] == nullptr)
	{
		LOG->Warn("[NETDBG] SRV::SendNetPacket#2 Client[%u] is null, abort", client);
		return;
	}
	LOG->Info("[NETDBG] SRV::SendNetPacket#3 client=%u bytes=%d", client, Packet.Position);
	Client[client]->clientSocket.SendPack((char*)Packet.Data, Packet.Position);
}

void StepManiaLanServer::StatsNameColumn(PacketFunctions &data, vector<LanPlayer*> &playersPtr)
{
	for (unsigned int x = 0; x < playersPtr.size(); ++x)
		data.Write1( (uint8_t) playersPtr[x]->PlayerID );
}

void StepManiaLanServer::StatsComboColumn(PacketFunctions &data, vector<LanPlayer*> &playersPtr)
{
	for(unsigned int x = 0; x < playersPtr.size(); ++x )
		data.Write2( (uint16_t) playersPtr[x]->combo);
}

void StepManiaLanServer::StatsProjgradeColumn(PacketFunctions& data, vector<LanPlayer*> &playersPtr)
{
	for(unsigned int x = 0; x < playersPtr.size(); ++x )
		data.Write1( (uint8_t) playersPtr[x]->projgrade );
}

bool GameClient::IsPlaying(int x)
{
	//If the feet setting is above 0, there must be a player.
	if (Player[x].feet > 0)
		return true;

	return false;
}

void GameClient::UpdateStats(PacketFunctions& Packet)
{
	//Get the Stats from a packet
	char firstbyte = Packet.Read1();
	char secondbyte = Packet.Read1();
	int pID = int(firstbyte/16); /* MSN */

	Player[pID].currstep = int(firstbyte%16); /* LSN */
	Player[pID].projgrade = int(secondbyte/16);
	Player[pID].score = Packet.Read4();
	Player[pID].combo = Packet.Read2();

	if (Player[pID].combo > Player[pID].maxCombo)
		Player[pID].maxCombo = Player[pID].combo;

	Player[pID].health = Packet.Read2();
	Player[pID].offset = ((double)abs(int(Packet.Read2())-32767)/2000);
	Player[pID].steps[Player[pID].currstep]++;
}

void StepManiaLanServer::NewClientCheck()
{
	//Make a new client and accept a connection to it.
	//If no connection is accepted, delete the client.
	//todo mike
	// GameClient *tmp = new GameClient;

	// if (server.accept(tmp->clientSocket) == 1)
	// {
	// 	if (!IsBanned(tmp->clientSocket.address))
	// 	{
	// 		Client.push_back(tmp);
	// 		AssignPlayerIDs();
	// 	}
	// 	else
	// 	{
	// 		delete tmp;
	// 		tmp = NULL;
	// 	}
	// }
	// else
	// {
	// 	delete tmp;
	// 	tmp = NULL;
	// }
	if (server.CheckUpdate())
	{
		LOG->Info("[NETDBG] SRV::NewClientCheck#1 lobby updated, refreshing member list");
		CSteamID lobbyId = server.GetLobbyId();
		if (!lobbyId.IsValid())
		{
			LOG->Warn("[NETDBG] SRV::NewClientCheck#2 lobbyId INVALID, skipping refresh");
			server.ClearUpdate();
		}
		else
		{
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			if (matchmaking == nullptr)
			{
				LOG->Warn("[NETDBG] SRV::NewClientCheck#3 SteamMatchmaking() returned null!");
				server.ClearUpdate();
				return;
			}
			int lobbyCount = matchmaking->GetNumLobbyMembers(lobbyId);
			LOG->Info("[NETDBG] SRV::NewClientCheck#4 lobby=%llu memberCount=%d existingClientCount=%u",
				lobbyId.ConvertToUint64(), lobbyCount, (unsigned)Client.size());

			// Collect all member IDs in Lobby
			std::vector<CSteamID> lobbyMembers;
			for (int i = 0; i < lobbyCount; ++i) {
				CSteamID m = matchmaking->GetLobbyMemberByIndex(lobbyId, i);
				LOG->Info("[NETDBG] SRV::NewClientCheck#5 lobby member[%d]=%llu", i, m.ConvertToUint64());
				lobbyMembers.push_back(m);
			}

			// === Remove Clients that are not in Lobby ===
			for (int i = static_cast<int>(Client.size()) - 1; i >= 0; --i) {
				if (Client[i] == nullptr)
				{
					LOG->Warn("[NETDBG] SRV::NewClientCheck#6 Client[%d] null when scanning for removal", i);
					continue;
				}
				CSteamID id = Client[i]->clientSocket.GetSelfId();
				auto it = std::find(lobbyMembers.begin(), lobbyMembers.end(), id);
				if (it == lobbyMembers.end()) {
					LOG->Info("[NETDBG] SRV::NewClientCheck#7 client idx=%d (steamID=%llu) not in lobby, Disconnect()",
						i, id.ConvertToUint64());
					Disconnect(i);  // Pass in the client index
				}
			}

			//=== Join New Lobby Members ===
			for (const auto& id : lobbyMembers) {
				bool found = false;
				for (const auto& client : Client) {
					if (client && client->clientSocket.GetSelfId() == id) {
						found = true;
						break;
					}
				}

				if (!found && server.GetHostId() == id) {
					LOG->Info("[NETDBG] SRV::NewClientCheck#8 add HOST client steamID=%llu", id.ConvertToUint64());
					GameClient* tmp = new GameClient();
					tmp->clientSocket.SetSelfId(id);
					// [NETDBG] host 自己作為 client：對方就是自己，走 loopback
					tmp->clientSocket.SetHostId(id);
					Client.push_back(tmp);
					AssignPlayerIDs();  // Every time someone is added, the ID is reassigned
					LOG->Info("[NETDBG] SRV::NewClientCheck#9 HOST client added, Client.size()=%u",
						(unsigned)Client.size());
				}
			}
			server.ClearUpdate();
		}
	}

	const size_t connCount = server.m_conns.size();
	if (connCount > 0)
		LOG->Info("[NETDBG] SRV::NewClientCheck#10 m_conns has %u pending entries", (unsigned)connCount);

	for (size_t i = 0; i < server.m_conns.size(); /* no ++ here */)
	{
		HSteamNetConnection conn = server.m_conns[i];
		SteamNetConnectionInfo_t info;

		if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
		{
			CSteamID remoteID = info.m_identityRemote.GetSteamID();
			LOG->Info("[NETDBG] SRV::NewClientCheck#11 m_conns[%u] conn=%u remoteID=%llu state=%d",
				(unsigned)i, (unsigned)conn, remoteID.ConvertToUint64(), (int)info.m_eState);

			// Check if this remoteID already exists
			bool exists = false;
			for (const auto &client : Client)
			{
				if (client && client->clientSocket.GetSelfId() == remoteID)
				{
					exists = true;
					break;
				}
			}

			if (!exists)
			{
				LOG->Info("[NETDBG] SRV::NewClientCheck#12 NEW remote client, creating GameClient (remoteID=%llu)",
					remoteID.ConvertToUint64());
				GameClient *tmp = new GameClient();
				tmp->clientSocket.SetSelfId(remoteID);
				// [NETDBG] BUG FIX：server 端對該 client 的「對方 Steam ID」=該 client 自己，
				// 不能設為 server.GetHostId()（server 自己），否則 Send/Recv 會走 loopback queue，
				// 永遠收不到真正的 P2P 訊息也送不出去。
				tmp->clientSocket.SetHostId(remoteID);
				tmp->clientSocket.SetHandle(conn);
				Client.push_back(tmp);
				AssignPlayerIDs();
				LOG->Info("[NETDBG] SRV::NewClientCheck#13 GameClient pushed, Client.size()=%u host=%llu",
					(unsigned)Client.size(), server.GetHostId().ConvertToUint64());
			}
			else
			{
				LOG->Info("[NETDBG] SRV::NewClientCheck#14 remoteID already has GameClient, skip create");
			}

			// After processing this conn, remove it from m_conns
			server.m_conns.erase(server.m_conns.begin() + i);
		}
		else
		{
			LOG->Warn("[NETDBG] SRV::NewClientCheck#15 GetConnectionInfo failed for conn=%u, skipping",
				(unsigned)conn);
			++i; // Invalid connection or query failed, skipping
		}
	}
}

void StepManiaLanServer::ClientSort(int clientNum)
{
	if ( clientNum < Client.size() && clientNum!=0)
	{
		vector<GameClient*> Client_tmp;
		Client_tmp.push_back(Client.at(clientNum));//host
		for(int i=1;i<Client.size();i++)
		{
			if(i==clientNum)continue;
			Client_tmp.push_back(Client.at(i));
		}
		Client_tmp.push_back(Client.at(0));//set the pre host to the last
		Client.clear();
		Client.assign(Client_tmp.begin(), Client_tmp.end());
		ClearHasSong();
		AssignPlayerIDs();
		SendUserList();
		SendPlayerCondition();
	}
}

void StepManiaLanServer::SendValue(uint8_t value, const unsigned int clientNum)
{
	if ( clientNum < Client.size() )
		Client[clientNum]->clientSocket.SendPack((char*)&value, sizeof(uint8_t));
}

bool StepManiaLanServer::CheckShare(unsigned int hostIdx, unsigned int clientIdx, bool shareAll)
{
	bool result = true;
	if (hostIdx >= Client.size())
	{
		ServerChatOne("The host index is invalid.", hostIdx);
		result = false;
	}
	else if (!Client[hostIdx]->hasSong)
	{
		ServerChatOne("The Host hasn't selected a song yet.", hostIdx);
		result = false;
	}
	else if (Client[hostIdx]->usingShareSongSystem)
	{
		ServerChatOne("File transfer system is in use.", hostIdx);
		result = false;
	}
	else if (!Client[hostIdx]->inNetMusicSelect)
	{
		ServerChatOne("The host is not in the room.", hostIdx);
		result = false;
	}
	else if (!shareAll)
	{
		if (clientIdx >= Client.size() || hostIdx == clientIdx)
		{
			ServerChatOne("The share song parameter is invalid.", hostIdx);
			result = false;
		}
		else if (!Client[clientIdx]->inNetMusicSelect)
		{
			ServerChatOne("The client is not in the room.", hostIdx);
			result = false;
		}
		else if (Client[clientIdx]->hasSong)
		{
			ServerChatOne("The client already has the song.", hostIdx);
			result = false;
		}
	}
	return result;
}

bool IsCmd(CString &command)
{
	bool result = false;
	auto it = cmdList.find(command);

	if (it != cmdList.end())
	{
		result = true;
	}
	return result;
}

bool IsHostCmd(CString &command)
{
	bool result = false;
	auto it = hostCmdList.find(command);

	if (it != hostCmdList.end())
	{
		result = true;
	}
	return result;
}

CString GetArg(CString&command)
{
	size_t spacePos = command.find(" ");
	CString arg;

	if (spacePos != std::string::npos) {
		arg = command.substr(spacePos + 1);
	} else {
		arg = "";
	}
	return arg;
}

void StepManiaLanServer::AnalizeChat(PacketFunctions &Packet, const unsigned int clientNum)
{
	CString message = Packet.ReadNT();
	if (message.at(0) == '/')
	{
		CString command = message.substr(1, message.find(" ")-1);
		if(IsCmd(command) || IsHostCmd(command))
		{
			if ((command.compare(CMD_SHARE) == 0) ||
			(command.compare(CMD_SHAREFULL) == 0))
			{
				CommandShare(command, clientNum);
			}
			else if (command.compare(CMD_LIST) == 0)
			{
				ServerChatOne(ListPlayers(), clientNum);
			}
			else if (command.compare(CMD_HAVE) == 0)
			{
				Have(clientNum);
			}
			else if (command.compare(CMD_CANCEL) == 0)
			{
				CommandCancel(clientNum);
			}
			else if (command.compare(CMD_HELP) == 0)
			{
				CommandHelp(clientNum);
			}
			else if (command.compare(CMD_CODE) == 0)
			{
				CommandCode(clientNum);
			}
			else if (clientNum == 0)
			{
				CString arg = GetArg(command);
				if (command.compare(CMD_START) == 0)
				{
					ForceStart();
				}
				else if (command.compare(CMD_KICK) == 0)
				{
					Kick(arg);
				}
				else if (command.compare(CMD_BAN) == 0)
				{
					Ban(arg);
				}
				else if (command.compare(CMD_HOST) == 0)
				{
					Host(arg, Packet, clientNum);
				}
			}
			else
			{
				message = "No server command permission.";
				ServerChatOne(message, clientNum);
			}
		}
		else
		{
			message = "Unknown command.";
			ServerChatOne(message, clientNum);
		}
	}
	else
		RelayChat(message, clientNum); //normal chat
}

// 把所有指令一條一條送回給發起者；host (clientNum==0) 才會看到 host-only 指令
void StepManiaLanServer::CommandHelp(const unsigned int clientNum)
{
	ServerChatOne("Available commands:", clientNum);
	const int n = sizeof(g_serverCommands) / sizeof(g_serverCommands[0]);
	for (int i = 0; i < n; ++i)
	{
		const ServerCommandInfo& c = g_serverCommands[i];
		if (c.hostOnly && clientNum != 0) continue;
		ServerChatOne(c.description, clientNum);
	}
}

// 顯示目前房間代碼，沒有設定的話顯示 N/A
void StepManiaLanServer::CommandCode(const unsigned int clientNum)
{
	if (roomCode.empty())
		ServerChatOne("Room code: (LAN mode, no code)", clientNum);
	else
		ServerChatOne(CString("Room code: ") + roomCode, clientNum);
}

void StepManiaLanServer::ShareSong(unsigned int ShareSongServerNum, unsigned int ShareSongClientNum, CString ServerIp)
{
	int clientNum = ShareSongServerNum;
	int client_index = ShareSongClientNum;
	CString host_ip = ServerIp;

	if (Client[clientNum]->hasSong == true && // the player have song and another doesn't
		Client[clientNum]->usingShareSongSystem == false &&
		Client[clientNum]->inNetMusicSelect == true &&
		clientNum != client_index &&
		client_index < Client.size() &&
		Client[client_index]->hasSong == false)
	{
		Reply.ClearPacket();
		Reply.Write1(NSSSS + NSServerOffset);
		Reply.WriteNT(host_ip);
		Reply.Write1(client_index);
		if(Client[clientNum]->filefilter)
		{
			Reply.Write1(1);//open the video file filter
		}else
		{
			Reply.Write1(0);
		}
		SendNetPacket(clientNum, Reply);
		Client[clientNum]->usingShareSongSystem=true;

		// 記錄目前正在進行中的分享，並重置進度
		m_shareSenderIdx = clientNum;
		m_shareReceiverIdx = client_index;
		m_shareCurBytes = 0;
		m_shareTotalBytes = 0;
		m_shareLastActivityMs = GetTickCount();
		LOG->Info("[SHARE-SRV] register active share sender=%d receiver=%d",
			m_shareSenderIdx, m_shareReceiverIdx);
	}
}

void StepManiaLanServer::ForwardShareToReceiver(PacketFunctions& origPacket, int cmd, unsigned int senderClient)
{
	// origPacket 是 sender 端送來的：[cmd (1byte 已被消化)][receiver_idx (1byte)] + payload...
	// 把 server-side cmd byte 寫好，然後把剩下的 raw bytes (從目前 Position 到 PayloadLength) 塞進新 packet。
	if (origPacket.PayloadLength < origPacket.Position + 1)
	{
		LOG->Warn("[SHARE-SRV] forward: malformed packet (no receiver idx) payload=%d pos=%d",
			origPacket.PayloadLength, origPacket.Position);
		return;
	}
	int receiverIdx = origPacket.Data[origPacket.Position]; // peek，不前進 Position
	if (receiverIdx < 0 || receiverIdx >= (int)Client.size())
	{
		LOG->Warn("[SHARE-SRV] forward: invalid receiverIdx=%d", receiverIdx);
		return;
	}

	Reply.ClearPacket();
	Reply.Write1((uint8_t)(cmd + NSServerOffset));
	// 第二個 byte 在 sender 寫入時是 receiver 的 index (給 server 路由用)；
	// 在 forward 給 receiver 時要換成 sender 的 client index，方便 receiver 端紀錄/UI 顯示
	Reply.Write1((uint8_t)senderClient);
	// 其餘 payload 直接複製
	int payloadStart = origPacket.Position + 1; // 跳過原本的 receiver_idx byte
	int forwardBytes = origPacket.PayloadLength - payloadStart;
	if (forwardBytes > 0)
	{
		if (forwardBytes > NETMAXBUFFERSIZE - Reply.Position)
			forwardBytes = NETMAXBUFFERSIZE - Reply.Position;
		ForwardPacketBytes(Reply, origPacket.Data + payloadStart, forwardBytes);
	}

	SendNetPacket((unsigned int)receiverIdx, Reply);
	m_shareLastActivityMs = GetTickCount();
}

void StepManiaLanServer::BroadcastShareProgress(unsigned int senderClient, int curBytes, int totalBytes)
{
	// 找出 receiver；若 senderClient 跟我們記錄的 m_shareSenderIdx 一致就直接用
	int senderIdx = (int)senderClient;
	int receiverIdx = m_shareReceiverIdx;
	if (m_shareSenderIdx != senderIdx)
	{
		// sender 沒有透過 /share 啟動就送 progress；忽略以免影響 UI
		LOG->Warn("[SHARE-SRV] progress from %u but expected sender=%d, ignore",
			senderClient, m_shareSenderIdx);
		return;
	}
	m_shareCurBytes = curBytes;
	m_shareTotalBytes = totalBytes;
	m_shareLastActivityMs = GetTickCount();

	Reply.ClearPacket();
	Reply.Write1(NSSProgress + NSServerOffset);
	Reply.Write1((uint8_t)senderIdx);
	Reply.Write1((uint8_t)receiverIdx);
	Reply.Write4((uint32_t)curBytes);
	Reply.Write4((uint32_t)totalBytes);
	SendToAllClients(Reply);

	if (totalBytes <= 0 || curBytes >= totalBytes)
	{
		LOG->Info("[SHARE-SRV] transfer complete sender=%d receiver=%d %d/%d",
			senderIdx, receiverIdx, curBytes, totalBytes);
		m_shareSenderIdx = -1;
		m_shareReceiverIdx = -1;
		m_shareCurBytes = 0;
		m_shareTotalBytes = 0;
	}
}

void StepManiaLanServer::CommandCancel(const unsigned int clientNum)
{
	// 只有 host (clientNum 0) 或目前 sender/receiver 可以發 /cancel
	if (clientNum != 0 &&
		(int)clientNum != m_shareSenderIdx &&
		(int)clientNum != m_shareReceiverIdx)
	{
		ServerChatOne("Only host or transfer parties can cancel.", clientNum);
		return;
	}
	if (m_shareSenderIdx < 0)
	{
		ServerChatOne("No share-song transfer in progress.", clientNum);
		return;
	}
	DoServerCancelShare("/cancel by client " + std::to_string(clientNum));
}

void StepManiaLanServer::DoServerCancelShare(const CString& reason)
{
	if (m_shareSenderIdx < 0 && m_shareReceiverIdx < 0) return;
	LOG->Info("[SHARE-SRV] cancel share sender=%d receiver=%d reason='%s'",
		m_shareSenderIdx, m_shareReceiverIdx, reason.c_str());

	// 通知雙方
	Reply.ClearPacket();
	Reply.Write1(NSSCancel + NSServerOffset);
	Reply.Write1((uint8_t)(m_shareReceiverIdx >= 0 ? m_shareReceiverIdx : 0));
	if (m_shareSenderIdx >= 0 && m_shareSenderIdx < (int)Client.size())
	{
		SendNetPacket((unsigned int)m_shareSenderIdx, Reply);
		Client[m_shareSenderIdx]->usingShareSongSystem = false;
		Client[m_shareSenderIdx]->shareAll = false;
		Client[m_shareSenderIdx]->ShareNum = 0;
	}
	if (m_shareReceiverIdx >= 0 && m_shareReceiverIdx < (int)Client.size())
	{
		SendNetPacket((unsigned int)m_shareReceiverIdx, Reply);
		Client[m_shareReceiverIdx]->usingShareSongSystem = false;
	}

	// 廣播一個「達成 total」的 progress，讓 UI 收掉進度條
	if (m_shareTotalBytes > 0)
	{
		Reply.ClearPacket();
		Reply.Write1(NSSProgress + NSServerOffset);
		Reply.Write1((uint8_t)m_shareSenderIdx);
		Reply.Write1((uint8_t)m_shareReceiverIdx);
		Reply.Write4((uint32_t)m_shareTotalBytes);
		Reply.Write4((uint32_t)m_shareTotalBytes);
		SendToAllClients(Reply);
	}

	ServerChat("Share-song transfer cancelled (" + reason + ").");

	m_shareSenderIdx = -1;
	m_shareReceiverIdx = -1;
	m_shareCurBytes = 0;
	m_shareTotalBytes = 0;
	m_shareLastActivityMs = 0;
}

void StepManiaLanServer::CheckShareTimeout()
{
	if (m_shareSenderIdx < 0) return;
	// 30 秒沒有任何活動 -> 視為卡住，強制中止
	const DWORD timeoutMs = 30000;
	DWORD now = GetTickCount();
	if (m_shareLastActivityMs == 0) m_shareLastActivityMs = now;
	if (now - m_shareLastActivityMs >= timeoutMs)
	{
		LOG->Warn("[SHARE-SRV] timeout detected (%u ms idle) -> force cancel", now - m_shareLastActivityMs);
		DoServerCancelShare("timeout");
	}
}
void StepManiaLanServer::ShareAll(unsigned int ShareSongServerNum, CString ServerIp)
{
	if (Client[ShareSongServerNum]->hasSong == false) return;
	for (int i = Client[ShareSongServerNum]->ShareNum; i < Client.size(); i++)
	{
		if (i != 0 &&
			Client[i]->hasSong == false &&
			Client[i]->usingShareSongSystem == false &&
			Client[i]->inNetMusicSelect == true &&
			i != ShareSongServerNum)
		{
			ShareSong(ShareSongServerNum, i, ServerIp);
			Client[ShareSongServerNum]->ShareNum = i + 1;
			return;
		}
		Client[ShareSongServerNum]->ShareNum = i;
	}
	Client[ShareSongServerNum]->shareAll = false;
	Client[ShareSongServerNum]->ShareNum = 0;
}
void StepManiaLanServer::RelayChat(CString &passedmessage, const unsigned int clientNum)
{
	Reply.ClearPacket();
	CString message = "";

	message += Client[clientNum]->Player[0].name;

	if (Client[clientNum]->twoPlayers)
			message += "&";

	message += Client[clientNum]->Player[1].name;

	message += ": ";
	message += passedmessage;
	Reply.Write1(NSCCM + NSServerOffset);
	Reply.WriteNT(message);

	SendToAllClients(Reply);
}

void StepManiaLanServer::SelectSong(PacketFunctions& Packet, unsigned int clientNum)
{
	int use = Packet.Read1();
	CString message;

	if (use == 2)
	{
		if (clientNum == 0)
		{ 
			SecondSameSelect = false;

			CurrentSongInfo.title = Packet.ReadNT();
			CurrentSongInfo.artist = Packet.ReadNT();
			CurrentSongInfo.subtitle = Packet.ReadNT();
			int tmp_hash =Packet.Read4();
			if(tmp_hash!=0)
			{
				CurrentSongInfo.hash = tmp_hash;
			}

			Reply.ClearPacket();
			Reply.Write1(NSCRSG + NSServerOffset);
			Reply.Write1(1);
			Reply.WriteNT(CurrentSongInfo.title);
			Reply.WriteNT(CurrentSongInfo.artist);
			Reply.WriteNT(CurrentSongInfo.subtitle);
			Reply.Write4(CurrentSongInfo.hash);		

			//Only send data to clients currently in ScreenNetMusicSelect
			for (unsigned int x = 0; x < Client.size(); ++x)
				if (Client[x]->inNetMusicSelect)
					SendNetPacket(x, Reply);

			//The following code forces the host to select the same song twice in order to play it.
			if ((strcmp(CurrentSongInfo.title, LastSongInfo.title) == 0) &&
				(strcmp(CurrentSongInfo.artist, LastSongInfo.artist) == 0) &&
				(strcmp(CurrentSongInfo.subtitle, LastSongInfo.subtitle) == 0)&&
				CurrentSongInfo.hash==LastSongInfo.hash&&
				!ChangeHost)
					SecondSameSelect = true;

			if (!SecondSameSelect)
			{
				LastSongInfo.title = CurrentSongInfo.title;
				LastSongInfo.artist = CurrentSongInfo.artist;
				LastSongInfo.subtitle = CurrentSongInfo.subtitle;
				LastSongInfo.hash = CurrentSongInfo.hash;
				message = "Play \"";
				message += CurrentSongInfo.title + " " + CurrentSongInfo.subtitle;
				message += "\"?";
				ServerChat(message);
				ChangeHost=false;
			}

		}
		else
		{
			message = servername;
			message += ": You do not have permission to pick a song.";
			Reply.ClearPacket();
			Reply.Write1(NSCCM + NSServerOffset);
			Reply.WriteNT(message);
			SendNetPacket(clientNum, Reply);

			// Reply.ClearPacket();
			// Reply.Write1(NSCRSG + NSServerOffset);
			// Reply.Write1(1);
			// Reply.WriteNT(CurrentSongInfo.title);
			// Reply.WriteNT(CurrentSongInfo.artist);
			// Reply.WriteNT(CurrentSongInfo.subtitle);
			// SendNetPacket(clientNum, Reply);
		}
	}

	if (use == 1)
	{
		//If user dosn't have song
		Client[clientNum]->hasSong = false;
		message = Client[clientNum]->Player[0].name;

		if (Client[clientNum]->twoPlayers)
		{
			message += "&";
			message += Client[clientNum]->Player[1].name;
		}

		message += " lacks song \"";
		message += CurrentSongInfo.title;
		message += "\"";
		ServerChat(message);
	}

	//If client has song
	if (use == 0)
		Client[clientNum]->hasSong = true;

	//Only play if everyone has the same song and the host has select the same song twice.
	if ( CheckHasSongState() && SecondSameSelect && (clientNum == 0) )
	{
		for(int i=0; i<Client.size(); i++)
		{
			if(Client[i]->inNetMusicSelect==false)
			{
				if(use==0)
				{
					message = servername;
					message += ": Someone is not ready.";
					Reply.ClearPacket();
					Reply.Write1(NSCCM + NSServerOffset);
					Reply.WriteNT(message);
					SendNetPacket(clientNum, Reply);
				}
				return;
			}
		}
		ClientsSongSelectStart();

		//Reset last song in case host picks same song again (otherwise dual select is bypassed)
		ResetLastSongInfo();
	}
}

void StepManiaLanServer::ClientsSongSelectStart()
{
	Reply.ClearPacket();
	Reply.Write1(NSCRSG + NSServerOffset);
	Reply.Write1(2);
	Reply.WriteNT(CurrentSongInfo.title);
	Reply.WriteNT(CurrentSongInfo.artist);
	Reply.WriteNT(CurrentSongInfo.subtitle);
	//Only send data to clients currently in ScreenNetMusicSelect that use hasSong
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (Client[x]->inNetMusicSelect && Client[x]->hasSong)
		{
			SendNetPacket(x, Reply);
			//Designate the client is starting,
			//after ScreenNetMusicSelect but before game play (InGame).
			Client[x]->isStarting = true;
		}
}

bool StepManiaLanServer::CheckHasSongState()
{
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (Client[x]->inNetMusicSelect && !Client[x]->hasSong)
			return false;

	return true;
}

void StepManiaLanServer::ClearHasSong()
{
	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		Client[x]->hasSong = false;
		Client[x]->usingShareSongSystem = false;
	}
}

void StepManiaLanServer::SendToAllClients(PacketFunctions& Packet)
{
	for (unsigned int x = 0; x < Client.size(); ++x)
		SendNetPacket(x, Packet);

}

void StepManiaLanServer::ServerChat(const CString& message)
{
	CString x = servername + ": " + message;
	Reply.ClearPacket();
	Reply.Write1(NSCCM + NSServerOffset);
	Reply.WriteNT(x);
	SendToAllClients(Reply);
}

void  StepManiaLanServer::ServerChatOne(const CString& message, const unsigned int clientNum)
{
	CString msg = servername + ": " + message;
	Reply.ClearPacket();
	Reply.Write1(NSCCM + NSServerOffset);
	Reply.WriteNT(msg);
	SendNetPacket(clientNum, Reply);
}

bool StepManiaLanServer::CheckConnection(const unsigned int clientNum)
{
	//If there is an error close the socket.
	
	if ( clientNum >= Client.size() )
	{
		LOG->Warn("[NETDBG] SRV::CheckConnection#1 OOB clientNum=%u size=%u",
			clientNum, (unsigned)Client.size());
		AssignPlayerIDs();
		SendUserList();
		SendPlayerCondition();
		return false;
	}
	if (Client[clientNum] == nullptr)
	{
		LOG->Warn("[NETDBG] SRV::CheckConnection#2 Client[%u] null", clientNum);
		return false;
	}
	if (Client[clientNum]->clientSocket.IsError())
	{
		LOG->Warn("[NETDBG] SRV::CheckConnection#3 client=%u IsError, Disconnect", clientNum);
		Disconnect(clientNum);
		return false;
	}
	return true;
}

void StepManiaLanServer::SendUserList()
{
	LOG->Info("[NETDBG] SRV::SendUserList#1 size=%u", (unsigned)Client.size());
	Reply.ClearPacket();
	Reply.Write1(NSCUUL + NSServerOffset);
	Reply.Write1( (uint8_t) Client.size()*2 );
	Reply.Write1( (uint8_t) Client.size()*2 );

	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		if (Client[x] == nullptr)
		{
			LOG->Warn("[NETDBG] SRV::SendUserList#2 Client[%u] null, write empty", x);
			Reply.Write1(0); Reply.WriteNT("");
			Reply.Write1(0); Reply.WriteNT("");
			continue;
		}
		for (int y = 0; y < 2; ++y)
		{
			if (Client[x]->Player[y].name.length() == 0)
				Reply.Write1(0);
			else
				Reply.Write1(1);
			Reply.WriteNT(Client[x]->Player[y].name);
		}
	}

	LOG->Info("[NETDBG] SRV::SendUserList#3 broadcast");
	SendToAllClients(Reply);
	LOG->Info("[NETDBG] SRV::SendUserList#4 done");
}
void StepManiaLanServer::SendPlayerCondition()
{
	LOG->Info("[NETDBG] SRV::SendPlayerCondition#1 size=%u", (unsigned)Client.size());
	if (Client.empty())
	{
		LOG->Info("[NETDBG] SRV::SendPlayerCondition#2 empty Client, skip");
		return;
	}
	Reply.ClearPacket();
	Reply.Write1(NSCPC + NSServerOffset);
	Reply.Write1( (uint8_t) Client.size() );
	//0 = normal
	//1 = lack song
	//2 = leave room
	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		if (Client[x] == nullptr)
		{
			LOG->Warn("[NETDBG] SRV::SendPlayerCondition#3 Client[%u] null, skip", x);
			continue;
		}
		for (int y = 0; y < 2; ++y)
		{
			if (Client[x]->Player[y].name.empty())
			{
				continue;
			}

			PLAYER_CONDITION status = CONDITION_NORMAL;

			if (!Client[x]->inNetMusicSelect) status = CONDITION_LEAVE_ROOM;
			else if (Client[0] == nullptr || !Client[0]->hasSong) status = CONDITION_NORMAL;
			else if (!Client[x]->hasSong) status = CONDITION_LACK_SONG;

			Reply.Write1((int)status);
		}
	}

	PacketFunctions tmp = Reply;
	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		PacketFunctions tmp = Reply;
		tmp.Write1(x);
		SendNetPacket(x, tmp);
	}
	LOG->Info("[NETDBG] SRV::SendPlayerCondition#4 done");
	// SendToAllClients(Reply);
}

void StepManiaLanServer::ScreenNetMusicSelectStatus(PacketFunctions& Packet, unsigned int clientNum)
{
	CString message = "";
	int EntExitCode = Packet.Read1();
	static int pre_clientNum = -1;
	static int pre_EntExitCode = -1;
	if(clientNum>=Client.size())
	{
		return;
	}
	message += Client[clientNum]->Player[0].name;
	if (Client[clientNum]->twoPlayers)
		message += "&";
	message += Client[clientNum]->Player[1].name;

	if (EntExitCode % 2 == 1)
		Client[clientNum]->inNetMusicSelect = true;
	else
		Client[clientNum]->inNetMusicSelect = false;

	if(pre_clientNum==clientNum && pre_EntExitCode==EntExitCode)
	{
		return;
	}
	switch (EntExitCode)
	{
	case 0:
		message += " left the song selection.";
		break;
	case 1:
		message += " entered the song selection.";
		break;
	case 2:
		message += " went into options.";
		break;
	case 3:
		message += " came back from options.";
		break;
	}
	pre_clientNum = clientNum;
	pre_EntExitCode = EntExitCode;
	ServerChat(message);
}

CString StepManiaLanServer::ListPlayers()
{
	CString list= "Player List:\n";
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (Client[x]->inNetMusicSelect)
			for (int y = 0; y < 2; ++y)
				if (Client[x]->Player[y].name.length() > 0){
					list += Client[x]->Player[y].name + "\n";
				}
	return list;
}

void StepManiaLanServer::CommandShare(CString &command, const unsigned int clientNum)
{
	// LOG->Info("command.GetLength() %d", command.GetLength());
	Client[clientNum]->filefilter = (command.compare("share") == 0) ? true : false;
	if (command.GetLength() == CString("share").GetLength() ||
		command.GetLength() == CString("sharefull").GetLength()) // no arg, share all
	{
		Client[clientNum]->shareAll = true;
		if (CheckShare(clientNum, 0, true))
		{
			ShareAll(clientNum, Packet.fromIp);
		}
	}
	else
	{
		int index = atof(command.substr(command.find(" ") + 1).c_str());
		if (CheckShare(clientNum, index, false))
		{
			ShareSong(clientNum, index, Packet.fromIp);
		}
	}
}

void StepManiaLanServer::Have(const unsigned int clientNum)
{
	CString message = "";
	message += Client[clientNum]->Player[0].name;
	if (Client[clientNum]->twoPlayers)
		message += "&";
	message += Client[clientNum]->Player[1].name;
	message += " has song by force.";
	Client[clientNum]->forceHas = true;
	ServerChat(message);
}

void StepManiaLanServer::Kick(CString &name)
{
	bool kicked = false;
	for (unsigned int x = 0; x < Client.size(); ++x)
		for (int y = 0; (y < 2)&&(kicked == false); ++y)
			if (name == Client[x]->Player[y].name)
			{
				ServerChat("Kicked " + name + ".");
				Disconnect(x);
				kicked = true;
			}
}

void StepManiaLanServer::Ban(CString &name)
{
	bool kicked = false;
	for (unsigned int x = 0; x < Client.size(); ++x)
		for (int y = 0; (y < 2)&&(kicked == false); ++y)
			if (name == Client[x]->Player[y].name)
			{
				ServerChat("Banned " + name + ".");
				bannedIPs.push_back(Client[x]->clientSocket.address);
				Disconnect(x);
				kicked = true;
			}
}

bool StepManiaLanServer::IsBanned(CString &ip)
{
	for (unsigned int x = 0; x < bannedIPs.size(); ++x)
		if (ip == bannedIPs[x])
			return true;
	return false;
}

void StepManiaLanServer::Host(CString &name, PacketFunctions& Packet, unsigned int clientNum)
{
	bool result = false;
	CString message = "";
	int index = 1;

	if(!name.empty())
	{
		for (unsigned int x = 1; x < Client.size() && !result; ++x)
		{
			for (unsigned int y = 0; (y < 2); ++y)
			{
				if (Client[x]->Player[y].name.compare(name) == 0)
				{
					result = true;
					index = x;
					break;
				}
			}
		}
	}
	
	if(!result)
	{
		if(!name.empty()) index = static_cast<unsigned int>(atof(name.c_str()));
		
		if (index > 0 && index < Client.size())
		{
			for (unsigned int y = 0; (y < 2); ++y)
			{
				if (Client[index]->Player[y].name.length() > 0)
				{
					result = true;
					name = Client[index]->Player[y].name;
					break;
				}
			}
		}
	}

	if(result)
	{
		ChangeHost = true;
		message = "Host changed to " + name + ".";
		ClientSort(index);
		ServerChat(message);
	}
	else
	{
		message = "Failed to change host.";
		ServerChatOne(message, clientNum);
	}
}

void StepManiaLanServer::ForceStart()
{
	//Send the normal stat to clients using hasSong.
	ClientsSongSelectStart();

	//Reset last song in case host picks same song again (otherwise dual select is bypassed)
	ResetLastSongInfo();

	//Prepate force_start packet
	Reply.ClearPacket();
	Reply.Write1(NSCRSG + NSServerOffset);
	Reply.Write1(3);

	//Only send force_start data to clients currently in ScreenNetMusicSelect using forceHas
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (Client[x]->inNetMusicSelect && Client[x]->forceHas)
		{
			SendNetPacket(x, Reply);
			//Designate the client is starting,
			//after ScreenNetMusicSelect but before game play (InGame).
			Client[x]->isStarting = true;
		}
}

void StepManiaLanServer::ResetLastSongInfo()
{
	LastSongInfo.title = "";
	LastSongInfo.artist = "";
	LastSongInfo.subtitle = "";
}

void StepManiaLanServer::CheckLowerJudge(const unsigned int clientNum)
{
	for (int x = 0; x < 2; ++x)
		if (Client[clientNum]->IsPlaying(x))
		{
			if ((Client[clientNum]->Player[x].currstep == 2)&&
				(PREFSMAN->m_fJudgeWindowSecondsBoo < Client[clientNum]->Player[x].offset))
				Client[clientNum]->lowerJudge = true;
			if ((Client[clientNum]->Player[x].currstep == 3)&&
				(PREFSMAN->m_fJudgeWindowSecondsGood < Client[clientNum]->Player[x].offset))
				Client[clientNum]->lowerJudge = true;
			if ((Client[clientNum]->Player[x].currstep == 4)&&
				(PREFSMAN->m_fJudgeWindowSecondsGreat < Client[clientNum]->Player[x].offset))
				Client[clientNum]->lowerJudge = true;
			if ((Client[clientNum]->Player[x].currstep == 5)&&
				(PREFSMAN->m_fJudgeWindowSecondsPerfect < Client[clientNum]->Player[x].offset))
				Client[clientNum]->lowerJudge = true;
			if ((Client[clientNum]->Player[x].currstep == 6)&&
				(PREFSMAN->m_fJudgeWindowSecondsMarvelous < Client[clientNum]->Player[x].offset))
				Client[clientNum]->lowerJudge = true;
		}
}
#endif

/*
 * (c) 2003-2004 Joshua Allen
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

