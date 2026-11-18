#include "global.h"
#include "NetworkSyncServer.h"
#include "RageLog.h"
#include "PrefsManager.h"
#include "RageUtil.h"
#include "ShareSongTransfer.h"
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

static const bool AUTO_SHARE_MISSING_SONG = true;			// send the song (without video) to players who lack it
static const unsigned long SHARE_READY_TIMEOUT_MS = 15000;	// the sender has to answer an offer
static const unsigned long SHARE_IDLE_TIMEOUT_MS = 45000;	// no progress at all (both ends give up after 15s on their own)
static const unsigned long SHARE_AUTO_WAIT_MS = 10000;		// automatic job waiting for the host's "I have it" report

std::unordered_set<std::string> cmdList =
	{"share",
	 "sharefull",
	 "cancel",
	 "list",
	 "have"};

std::unordered_set<std::string> hostCmdList =
	{"start",
	 "kick",
	 "ban",
	 "host"};

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
	m_Share.active = false;
	m_Share.id = 0;
	m_Share.sender = -1;
	m_Share.receiver = -1;
	m_Share.includeVideo = false;
	m_Share.manual = false;
	m_Share.ready = false;
	m_Share.lastActivity = 0;
	m_Share.doneKB = 0;
	m_Share.totalKB = 0;
	m_iNextShareId = 1;
	m_iLastShareCheck = GetTickCount();
	CurrentSongInfo.hash = 0;
	LastSongInfo.hash = 0;
	AssignPlayerIDs();
}

StepManiaLanServer::~StepManiaLanServer()
{
	ServerStop();
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
		ProcessShareQueue();
	}
}

void StepManiaLanServer::UpdateClients()
{
	//Go through all the clients and check to see if it is being used.
	//If so then try to get a backet and parse the data.
	for (unsigned int x = 0; x < Client.size(); ++x)
		if (CheckConnection(x) && (Client[x]->GetData(Packet) >= 0))
			ParseData(Packet, x);
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
	autoShareTried = false;
}

void StepManiaLanServer::Disconnect(const unsigned int clientNum)
{
	ShareOnClientRemoved(clientNum);
	if (clientNum == (Client.size()-1))//host leave
	{
		delete Client[Client.size()-1];
		Client[Client.size()-1] = NULL;
		Client.pop_back();
		ClearHasSong();
	}
	else
	{
		vector<GameClient*>::iterator Iterator;
		Iterator = Client.begin();
		for (unsigned int x = 0; x < Client.size(); ++x)
		{
			if (x == clientNum)
			{
				delete Client[x];
				Client[x] = NULL;
				Client.erase(Iterator);
			}
			++Iterator;
		}
	}
	SendUserList();
	SendPlayerCondition();
}

int GameClient::GetData(PacketFunctions& Packet)
{
	int length = -1;
	Packet.ClearPacket();
	length = clientSocket.ReadPack((char*)Packet.Data, NETMAXBUFFERSIZE);
	return length;
}

void StepManiaLanServer::ParseData(PacketFunctions& Packet, const unsigned int clientNum)
{
	int command = Packet.Read1();
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
	case NSSReady:
		ShareReady(Packet, clientNum);
		break;
	case NSSProgress:
		ShareProgress(Packet, clientNum);
		break;
	case NSSResult:
		ShareResult(Packet, clientNum);
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
	default:
		break;
	}
}	 

void StepManiaLanServer::Hello(PacketFunctions& Packet, const unsigned int clientNum)
{
	int ClientVersion = Packet.Read1();
	CString build = Packet.ReadNT();

	Client[clientNum]->SetClientVersion(ClientVersion, build);

	Reply.ClearPacket();
	Reply.Write1( NSCHello + NSServerOffset );
	Reply.Write1(1);
	Reply.WriteNT(servername);

	SendNetPacket(clientNum, Reply);

	if (ClientHost == -1)
		ClientHost = clientNum;

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
	if ( client < Client.size() )
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

	GameClient *tmp = new GameClient;

	if (server.accept(tmp->clientSocket) == 1)
	{
		if (!IsBanned(tmp->clientSocket.address))
		{
			Client.push_back(tmp);
			AssignPlayerIDs();
		}
		else
		{
			delete tmp;
			tmp = NULL;
		}
	}
	else
	{
		delete tmp;
		tmp = NULL;
	}
}

void StepManiaLanServer::ClientSort(int clientNum)
{
	if ( clientNum < Client.size() && clientNum!=0)
	{
		CancelAllShares("Host changed, song transfer cancelled.");
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

void StepManiaLanServer::AnalizeChat(PacketFunctions &Packet, const unsigned int clientNum)
{
	CString message = Packet.ReadNT();
	if (!message.empty() && message.at(0) == '/')
	{
		//Split "/command argument"
		size_t spacePos = message.find(" ");
		CString command = message.substr(1, spacePos == string::npos ? string::npos : spacePos - 1);
		CString arg = (spacePos == string::npos) ? CString("") : CString(message.substr(spacePos + 1));
		size_t argBegin = arg.find_first_not_of(" \t");
		size_t argEnd = arg.find_last_not_of(" \t");
		arg = (argBegin == string::npos) ? CString("") : CString(arg.substr(argBegin, argEnd - argBegin + 1));
		if(IsCmd(command) || IsHostCmd(command))
		{
			if ((command.compare("share") == 0) ||
			(command.compare("sharefull") == 0))
			{
				CommandShare(command.compare("sharefull") == 0, arg, clientNum);
			}
			else if (command.compare("cancel") == 0)
			{
				CommandCancel(clientNum);
			}
			else if ((command.compare("list") == 0))
			{
				ServerChatOne(ListPlayers(), clientNum);
			}
			else if ((command.compare("have") == 0))
			{
				Have(clientNum);
			}
			else if (clientNum == 0)
			{
				if (command.compare("start") == 0)
				{
					ForceStart();
				}
				else if (command.compare("kick") == 0)
				{
					Kick(arg);
				}
				else if (command.compare("ban") == 0)
				{
					Ban(arg);
				}
				else if (command.compare("host") == 0)
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

			GameInfo previousSong = CurrentSongInfo;
			CurrentSongInfo.title = Packet.ReadNT();
			CurrentSongInfo.artist = Packet.ReadNT();
			CurrentSongInfo.subtitle = Packet.ReadNT();
			int tmp_hash =Packet.Read4();
			if(tmp_hash!=0)
			{
				CurrentSongInfo.hash = tmp_hash;
			}

			//A different song: transfers of the old one are useless now
			if (previousSong.title != CurrentSongInfo.title ||
				previousSong.artist != CurrentSongInfo.artist ||
				previousSong.subtitle != CurrentSongInfo.subtitle ||
				previousSong.hash != CurrentSongInfo.hash)
			{
				CancelAllShares("Song changed, song transfer cancelled.");
				for (unsigned int x = 0; x < Client.size(); ++x)
					Client[x]->autoShareTried = false;
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

		//Send it automatically (without video) from the host
		if (AUTO_SHARE_MISSING_SONG && clientNum != 0 && !CurrentSongInfo.title.empty() &&
			Client[clientNum]->inNetMusicSelect && !Client[clientNum]->autoShareTried)
		{
			Client[clientNum]->autoShareTried = true;
			EnqueueShare(0, clientNum, false, false);
		}
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
		Client[x]->autoShareTried = false;
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
		AssignPlayerIDs();
		SendUserList();
		SendPlayerCondition();
		return false;
	}
	if (Client[clientNum]->clientSocket.IsError())
	{
		Disconnect(clientNum);
		return false;
	}
	return true;
}

void StepManiaLanServer::SendUserList()
{
	Reply.ClearPacket();
	Reply.Write1(NSCUUL + NSServerOffset);
	Reply.Write1( (uint8_t) Client.size()*2 );
	Reply.Write1( (uint8_t) Client.size()*2 );

	for (unsigned int x = 0; x < Client.size(); ++x)
		for (int y = 0; y < 2; ++y)
		{
			if (Client[x]->Player[y].name.length() == 0)
				Reply.Write1(0);
			else
				Reply.Write1(1);
			Reply.WriteNT(Client[x]->Player[y].name);
		}

	SendToAllClients(Reply);
}
void StepManiaLanServer::SendPlayerCondition()
{
	Reply.ClearPacket();
	Reply.Write1(NSCPC + NSServerOffset);
	Reply.Write1( (uint8_t) Client.size() );
	//0 = normal
	//1 = lack song
	//2 = leave room
	for (unsigned int x = 0; x < Client.size(); ++x)
		for (int y = 0; y < 2; ++y)
		{
			if (Client[x]->Player[y].name.length() != 0)
			{
				if(Client[0]->hasSong==true)
				{
					if(Client[x]->inNetMusicSelect==false)
					{
						Reply.Write1(2);
					}else if(Client[x]->hasSong==false)
					{
						Reply.Write1(1);
					}else if(Client[x]->hasSong==true)
					{
						Reply.Write1(0);
					}
				}else
				{
					if(Client[x]->inNetMusicSelect==false)
					{
						Reply.Write1(2);
					}else
					{
						Reply.Write1(0);
					}
				}
			}
		}
	PacketFunctions tmp = Reply;
	for (unsigned int x = 0; x < Client.size(); ++x)
	{
		PacketFunctions tmp = Reply;
		tmp.Write1(x);
		SendNetPacket(x, tmp);
	}
		
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

/* ---------------- Song sharing (server side) ---------------- */

CString StepManiaLanServer::ClientDisplayName( unsigned int clientNum )
{
	if( clientNum >= Client.size() )
		return "?";
	CString name = Client[clientNum]->Player[0].name;
	if( !Client[clientNum]->Player[1].name.empty() )
	{
		if( !name.empty() )
			name += "&";
		name += Client[clientNum]->Player[1].name;
	}
	if( name.empty() )
		name = ssprintf( "Player %u", clientNum );
	return name;
}

bool StepManiaLanServer::IsShareParty( unsigned int clientNum ) const
{
	return m_Share.active && ((int)clientNum == m_Share.sender || (int)clientNum == m_Share.receiver);
}

/* /share [N]      send the song without video to everyone (or N) who lacks it
 * /sharefull [N]  send the song with video; players who already have the song
 *                 only get the video files they are missing */
void StepManiaLanServer::CommandShare( bool includeVideo, const CString &arg, const unsigned int clientNum )
{
	if( CurrentSongInfo.title.empty() )
	{
		ServerChatOne( "No song has been selected yet.", clientNum );
		return;
	}
	if( !Client[clientNum]->inNetMusicSelect )
	{
		ServerChatOne( "You are not in the room.", clientNum );
		return;
	}
	if( !Client[clientNum]->hasSong )
	{
		ServerChatOne( "You don't have the selected song.", clientNum );
		return;
	}

	vector<unsigned int> targets;
	if( arg.empty() )
	{
		for( unsigned int x = 0; x < Client.size(); ++x )
			if( x != clientNum && Client[x]->inNetMusicSelect && (includeVideo || !Client[x]->hasSong) )
				targets.push_back( x );
		if( targets.empty() )
		{
			ServerChatOne( includeVideo ? "Nobody else is in the room." : "Everyone already has the song.", clientNum );
			return;
		}
	}
	else
	{
		bool isNumber = arg.find_first_not_of( "0123456789" ) == string::npos && arg.size() < 4;
		unsigned int index = isNumber ? (unsigned int)atoi( arg.c_str() ) : (unsigned int)Client.size();
		if( index >= Client.size() || index == clientNum )
		{
			ServerChatOne( "The share song parameter is invalid.", clientNum );
			return;
		}
		if( !Client[index]->inNetMusicSelect )
		{
			ServerChatOne( "The client is not in the room.", clientNum );
			return;
		}
		if( !includeVideo && Client[index]->hasSong )
		{
			ServerChatOne( ClientDisplayName( index ) + " already has the song. Use /sharefull " + arg + " to send the video.", clientNum );
			return;
		}
		targets.push_back( index );
	}

	for( unsigned int i = 0; i < targets.size(); ++i )
		EnqueueShare( clientNum, targets[i], includeVideo, true );
	if( m_Share.active )
		ServerChatOne( ssprintf( "Another song transfer is running, %u queued.", (unsigned int)targets.size() ), clientNum );
}

void StepManiaLanServer::CommandCancel( const unsigned int clientNum )
{
	if( !m_Share.active && m_ShareQueue.empty() )
	{
		ServerChatOne( "No song transfer in progress.", clientNum );
		return;
	}
	if( clientNum != 0 && !IsShareParty( clientNum ) )
	{
		ServerChatOne( "Only the host or the players in the transfer can cancel it.", clientNum );
		return;
	}
	CString message = "Song transfer cancelled by " + ClientDisplayName( clientNum ) + ".";
	if( clientNum == 0 )
		m_ShareQueue.clear();	// the host cancels everything, others only the running transfer
	if( m_Share.active )
		EndShareSession( message, true );
	else
		ServerChat( message );
}

void StepManiaLanServer::EnqueueShare( unsigned int sender, unsigned int receiver, bool includeVideo, bool manual )
{
	if( m_Share.active && m_Share.receiver == (int)receiver && (m_Share.includeVideo || !includeVideo) )
		return;	// already being served
	for( unsigned int i = 0; i < m_ShareQueue.size(); ++i )
	{
		ShareJob &job = m_ShareQueue[i];
		if( job.receiver == (int)receiver )
		{
			job.sender = (int)sender;
			job.includeVideo = job.includeVideo || includeVideo;
			job.manual = job.manual || manual;
			return;
		}
	}
	ShareJob job;
	job.sender = (int)sender;
	job.receiver = (int)receiver;
	job.includeVideo = includeVideo;
	job.manual = manual;
	job.queuedAt = GetTickCount();
	m_ShareQueue.push_back( job );
}

void StepManiaLanServer::ProcessShareQueue()
{
	while( !m_Share.active && !m_ShareQueue.empty() )
	{
		ShareJob job = m_ShareQueue.front();
		if( job.sender < 0 || job.receiver < 0 || job.sender >= (int)Client.size() ||
			job.receiver >= (int)Client.size() || job.sender == job.receiver )
		{
			m_ShareQueue.erase( m_ShareQueue.begin() );
			continue;
		}
		GameClient *pSender = Client[job.sender];
		GameClient *pReceiver = Client[job.receiver];
		if( !pSender->hasSong && !job.manual && GetTickCount() - job.queuedAt < SHARE_AUTO_WAIT_MS )
			return;	// the host's "I have it" report may still be on its way
		m_ShareQueue.erase( m_ShareQueue.begin() );
		if( !pSender->hasSong || !pSender->inNetMusicSelect || !pReceiver->inNetMusicSelect )
			continue;
		if( !job.includeVideo && pReceiver->hasSong )
			continue;	// got it in the meantime
		StartShareSession( job );
	}
}

void StepManiaLanServer::StartShareSession( const ShareJob &job )
{
	m_Share.active = true;
	m_Share.id = m_iNextShareId;
	m_iNextShareId = (m_iNextShareId % 255) + 1;	// 1..255
	m_Share.sender = job.sender;
	m_Share.receiver = job.receiver;
	m_Share.includeVideo = job.includeVideo;
	m_Share.manual = job.manual;
	m_Share.ready = false;
	m_Share.lastActivity = GetTickCount();
	m_Share.doneKB = 0;
	m_Share.totalKB = 0;

	Reply.ClearPacket();
	Reply.Write1( NSSOffer + NSServerOffset );
	Reply.Write1( (uint8_t)m_Share.id );
	Reply.Write1( (uint8_t)m_Share.receiver );
	Reply.Write1( m_Share.includeVideo ? 1 : 0 );
	SendNetPacket( m_Share.sender, Reply );

	//As before: once a song was shared the host has to confirm "Play ...?" again
	ResetLastSongInfo();

	BroadcastShareProgress( true );

	CString what;
	if( m_Share.includeVideo && Client[m_Share.receiver]->hasSong )
		what = "the video of \"" + CurrentSongInfo.title + "\"";
	else
		what = "\"" + CurrentSongInfo.title + "\"" + (m_Share.includeVideo ? " (with video)" : " (without video)");
	ServerChat( ClientDisplayName( m_Share.sender ) + " is sending " + what + " to " +
		ClientDisplayName( m_Share.receiver ) + "..." );
}

void StepManiaLanServer::EndShareSession( const CString &message, bool notifyParties )
{
	if( !m_Share.active )
		return;
	m_Share.active = false;
	if( notifyParties )
	{
		Reply.ClearPacket();
		Reply.Write1( NSSCancel + NSServerOffset );
		Reply.Write1( (uint8_t)m_Share.id );
		if( m_Share.sender >= 0 && m_Share.sender < (int)Client.size() )
			SendNetPacket( m_Share.sender, Reply );
		if( m_Share.receiver >= 0 && m_Share.receiver < (int)Client.size() )
			SendNetPacket( m_Share.receiver, Reply );
	}
	BroadcastShareProgress( false );
	if( !message.empty() )
		ServerChat( message );
	// The next queued job starts from ServerUpdate().
}

void StepManiaLanServer::CancelAllShares( const CString &message )
{
	m_ShareQueue.clear();
	EndShareSession( message, true );
}

void StepManiaLanServer::BroadcastShareProgress( bool active )
{
	Reply.ClearPacket();
	Reply.Write1( NSSProgress + NSServerOffset );
	Reply.Write1( (uint8_t)(m_Share.sender >= 0 ? m_Share.sender : 0) );
	Reply.Write1( (uint8_t)(m_Share.receiver >= 0 ? m_Share.receiver : 0) );
	Reply.Write4( m_Share.doneKB );
	Reply.Write4( m_Share.totalKB );
	Reply.Write1( active ? 1 : 0 );
	SendToAllClients( Reply );
}

void StepManiaLanServer::CheckShareTimeout()
{
	unsigned long now = GetTickCount();
	unsigned long gap = now - m_iLastShareCheck;
	m_iLastShareCheck = now;
	if( !m_Share.active )
		return;
	/* The host's game may have been busy (loading a screen); don't count that against the transfer. */
	if( gap > 2000 )
		m_Share.lastActivity += gap;
	unsigned long idle = now - m_Share.lastActivity;
	if( !m_Share.ready && idle > SHARE_READY_TIMEOUT_MS )
		EndShareSession( "Song transfer to " + ClientDisplayName( m_Share.receiver ) + " failed: the sender did not respond.", true );
	else if( idle > SHARE_IDLE_TIMEOUT_MS )
		EndShareSession( "Song transfer to " + ClientDisplayName( m_Share.receiver ) + " timed out.", true );
}

/* Called before a client is removed from Client[]: stop its transfer and fix the indices. */
void StepManiaLanServer::ShareOnClientRemoved( unsigned int clientNum )
{
	if( IsShareParty( clientNum ) )
		EndShareSession( "Song transfer cancelled: " + ClientDisplayName( clientNum ) + " left.", true );
	if( m_Share.active )
	{
		if( m_Share.sender > (int)clientNum )
			--m_Share.sender;
		if( m_Share.receiver > (int)clientNum )
			--m_Share.receiver;
	}
	for( unsigned int i = 0; i < m_ShareQueue.size(); )
	{
		ShareJob &job = m_ShareQueue[i];
		if( job.sender == (int)clientNum || job.receiver == (int)clientNum )
		{
			m_ShareQueue.erase( m_ShareQueue.begin() + i );
			continue;
		}
		if( job.sender > (int)clientNum )
			--job.sender;
		if( job.receiver > (int)clientNum )
			--job.receiver;
		++i;
	}
}

void StepManiaLanServer::ShareReady( PacketFunctions &Packet, unsigned int clientNum )
{
	int id = Packet.Read1();
	unsigned int port = Packet.Read2();
	unsigned int token = Packet.Read4();
	CString reportedIp = Packet.ReadNT();
	if( !m_Share.active || id != m_Share.id || (int)clientNum != m_Share.sender )
		return;
	if( m_Share.receiver < 0 || m_Share.receiver >= (int)Client.size() )
		return;
	m_Share.ready = true;
	m_Share.lastActivity = GetTickCount();

	/* When the sender runs on the server's machine (usually the host) the receiver
	 * should use the address it already uses for the server; "127.0.0.1" tells it so. */
	EzSockets &sock = Client[clientNum]->clientSocket;
	CString observedIp = sock.getPeerIp();
	bool sameMachine = observedIp.Left(4) == "127." || observedIp == sock.getIp();

	Reply.ClearPacket();
	Reply.Write1( NSSReady + NSServerOffset );
	Reply.Write1( (uint8_t)id );
	Reply.Write1( (uint8_t)m_Share.sender );
	Reply.Write2( (uint16_t)port );
	Reply.Write4( token );
	Reply.WriteNT( sameMachine ? CString("127.0.0.1") : reportedIp );
	Reply.WriteNT( sameMachine ? reportedIp : observedIp );
	SendNetPacket( m_Share.receiver, Reply );
}

void StepManiaLanServer::ShareProgress( PacketFunctions &Packet, unsigned int clientNum )
{
	int id = Packet.Read1();
	unsigned int doneKB = Packet.Read4();
	unsigned int totalKB = Packet.Read4();
	if( !m_Share.active || id != m_Share.id || (int)clientNum != m_Share.receiver )
		return;
	if( doneKB != m_Share.doneKB || totalKB != m_Share.totalKB )
		m_Share.lastActivity = GetTickCount();
	m_Share.doneKB = doneKB;
	m_Share.totalKB = totalKB;
	BroadcastShareProgress( true );
}

void StepManiaLanServer::ShareResult( PacketFunctions &Packet, unsigned int clientNum )
{
	int id = Packet.Read1();
	bool isReceiver = Packet.Read1() != 0;
	int result = Packet.Read1();
	CString message = Packet.ReadNT();
	if( !m_Share.active || id != m_Share.id )
		return;
	const bool failed = result != ShareSongTransfer::RESULT_OK && result != ShareSongTransfer::RESULT_NOTHING_NEEDED;
	CString name = ClientDisplayName( m_Share.receiver );
	if( isReceiver && (int)clientNum == m_Share.receiver )
	{
		if( failed )
			EndShareSession( "Song transfer to " + name + " failed: " + message, true );
		else if( result == ShareSongTransfer::RESULT_OK )
			EndShareSession( name + " received the song.", false );
		else
			EndShareSession( name + " already has all the files.", false );
	}
	else if( !isReceiver && (int)clientNum == m_Share.sender && failed )
	{
		/* A successful sender waits for the receiver's report. */
		EndShareSession( "Song transfer to " + name + " failed: " + message, true );
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

