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

std::unordered_set<std::string> cmdList =
	{"share",
	 "sharefull",
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
	shareAll = false;
	usingShareSongSystem = false;
	filefilter = true;
	ShareNum = 0;
}

void StepManiaLanServer::Disconnect(const unsigned int clientNum)
{
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
		if(Client[clientNum]->shareAll)
		{
			ShareAll(clientNum, Packet.fromIp);
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
			if ((command.compare("share") == 0) ||
			(command.compare("sharefull") == 0))
			{
				CommandShare(command, clientNum);
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
				CString arg = GetArg(command);
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

