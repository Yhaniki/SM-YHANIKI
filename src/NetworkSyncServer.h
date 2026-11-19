#ifndef NetworkSyncServer_H
#define NetworkSyncServer_H

#include "NetworkSyncManager.h"

#if !defined(WITHOUT_NETWORKING)
#include "ezsockets.h"
#define NETMAXBUFFERSIZE 1020

class LanPlayer
{
public:
	CString name;
	long score;
	int health;
	int feet;
	int projgrade;
	int combo;
	int currstep;
	int steps[9];
	int maxCombo;
	int Grade;
	double offset;
	int PlayerID;
	int diff;
	CString options;
	CString percentage;
	int Graph[NETGRAPHSIZE];
	LanPlayer();
};

class GameInfo
{
public:
	CString title;
	CString subtitle;
	CString artist;
	CString course;
	int     hash;
};

class GameClient
{
public:
	bool GotStartRequest;
	EzSockets clientSocket;
	void UpdateStats(PacketFunctions &Packet);
	void SetClientVersion(int ver, const CString& b);
	void StartRequest(PacketFunctions &Packet);
	int GetData(PacketFunctions &Packet);
	GameClient();
	LanPlayer Player[2];
	bool IsPlaying(int Player);
	void StyleUpdate(PacketFunctions &Packet);
	bool InGame;
	int twoPlayers;
	bool hasSong;
	bool forceHas;
	bool inNetMusicSelect;
	int startPosition;
	bool isStarting;
	bool wasIngame;
	bool lowerJudge;
	bool autoShareTried;	//Already got an automatic song transfer for the current song

private:
	string build;
	GameInfo gameInfo;
	int version;
};
#endif

class StepManiaLanServer
{
public:
	bool ServerStart();
	void ServerStop();
	void ServerUpdate();
	StepManiaLanServer();
	virtual ~StepManiaLanServer();
	CString servername;
	CString lastError;
	int lastErrorCode;

protected:
#if !defined(WITHOUT_NETWORKING)
	/* Notifications for front ends such as the standalone server's window
	 * (StandAloneServer/); the in-game server ignores them. */
	virtual void OnClientConnected( unsigned int clientNum ) { }
	virtual void OnClientDisconnecting( unsigned int clientNum ) { }	// still in Client[]
	virtual void OnChatLine( const CString &sLine ) { }				// a chat line sent to everyone

	bool stop;
	PacketFunctions Packet;
	PacketFunctions Reply;
	vector<GameClient*> Client; 
	EzSockets server;
	int ClientHost;
	vector<LanPlayer*> playersPtr;
	time_t statsTime;
	GameInfo CurrentSongInfo;
	GameInfo LastSongInfo;
	bool SecondSameSelect;
	vector<CString> bannedIPs;
	bool ChangeHost;
	void Hello(PacketFunctions& Packet, const unsigned int clientNum);
	void UpdateClients();
	void NewClientCheck();
	void ClientSort(int clientNum);
	void ParseData(PacketFunctions& Packet, const unsigned int clientNum);
	void SendValue(uint8_t value, const unsigned int clientNum);
	void CheckReady();
	void MoveClientToHost();
	void StatsComboColumn(PacketFunctions &data, vector<LanPlayer*> &playersPtr);
	void SendStatsToClients();
	void StatsProjgradeColumn(PacketFunctions& data, vector<LanPlayer*> &playersPtr);
	void StatsNameColumn(PacketFunctions& data, vector<LanPlayer*> &playersPtr);
	void SendNetPacket(const unsigned int clientNum, PacketFunctions &Packet);
	int SortStats(vector<LanPlayer*> &playresPtr);
	void RelayChat(CString &passedmessage, const unsigned int clientNum);
	void SelectSong(PacketFunctions& Packet, const unsigned int clientNum);
	void ServerChat(const CString& message);
	void ServerChatOne(const CString& message, const unsigned int clientNum);
	void SendToAllClients(PacketFunctions& Packet);
	bool CheckHasSongState();
	void ClearHasSong();
	void AssignPlayerIDs();
	void SendUserList();
	void GameOver(PacketFunctions& Packet, const unsigned int clientNum);
	void ScreenNetMusicSelectStatus(PacketFunctions& Packet, const unsigned int clientNum);
	void AnalizeChat(PacketFunctions &Packet, const unsigned int clientNum);
	CString StepManiaLanServer::ListPlayers();
	void Have(const unsigned int clientNum);
	void Kick(CString &name);
	void Ban(CString &name);
	void Host(CString &name, PacketFunctions& Packet, unsigned int clientNum);
	bool IsBanned(CString &ip);
	void ForceStart();
	void CheckLowerJudge(const unsigned int clientNum);
	bool CheckConnection(const unsigned int clientNum);
	void PopulatePlayersPtr(vector<LanPlayer*> &playersPtr);
	void Disconnect(const unsigned int clientNum);
	void ClientsSongSelectStart();
	void ResetLastSongInfo();
	void ServerGetGraph(PacketFunctions& Packet, unsigned int clientNum);
	void SendPlayerCondition();
	void GetHasSong(PacketFunctions&Packet, unsigned int clientNum);
	void GetAskSong(PacketFunctions&Packet, unsigned int clientNum);
	//Song sharing: one transfer at a time, the rest waits in m_ShareQueue.
	//The server only relays control messages; the data goes straight from
	//the sender to the receiver (see ShareSongTransfer.h).
	struct ShareJob
	{
		int sender;
		int receiver;
		bool includeVideo;
		bool manual;			//from /share; automatic jobs come from "lacks song"
		unsigned long queuedAt;
	};
	struct ShareSession
	{
		bool active;
		int id;
		int sender;
		int receiver;
		bool includeVideo;
		bool manual;
		bool ready;				//the sender is listening
		unsigned long lastActivity;
		unsigned int doneKB;
		unsigned int totalKB;
	};
	vector<ShareJob> m_ShareQueue;
	ShareSession m_Share;
	int m_iNextShareId;
	unsigned long m_iLastShareCheck;
	CString ClientDisplayName(unsigned int clientNum);
	bool IsShareParty(unsigned int clientNum) const;
	void CommandShare(bool includeVideo, const CString &arg, const unsigned int clientNum);
	void CommandCancel(const unsigned int clientNum);
	void EnqueueShare(unsigned int sender, unsigned int receiver, bool includeVideo, bool manual);
	void ProcessShareQueue();
	void StartShareSession(const ShareJob &job);
	void EndShareSession(const CString &message, bool notifyParties);
	void CancelAllShares(const CString &message);
	void BroadcastShareProgress(bool active);
	void CheckShareTimeout();
	void ShareOnClientRemoved(unsigned int clientNum);
	void ShareReady(PacketFunctions &Packet, unsigned int clientNum);
	void ShareProgress(PacketFunctions &Packet, unsigned int clientNum);
	void ShareResult(PacketFunctions &Packet, unsigned int clientNum);
#endif
};

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
