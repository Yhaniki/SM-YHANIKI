/* NetworkSyncManager - Uses ezsockets for primitive song syncing and score reporting. */

#ifndef NetworkSyncManager_H
#define NetworkSyncManager_H

#include "PlayerNumber.h"
#include "Difficulty.h"
#include <windows.h>

class LoadingWindow;
class Song;

#define NETMAXPLAYERS 32
const int NETPROTOCOLVERSION=1;
const int NETMAXBUFFERSIZE=1020; //1024 - 4 bytes for EzSockets
const int NETNUMTAPSCORES=8;
const int NETGRAPHSIZE=100;
enum NSCommand
{
	NSCPing = 0,
	NSCPingR,		//1
	NSCHello,		//2
	NSCGSR,			//3
	NSCGON,			//4
	NSCGSU,			//5
	NSCSU,			//6
	NSCCM,			//7
	NSCRSG,			//8
	NSCUUL,			//9
	NSCSMS,			//10
	NSCUPOpts,		//11
	NSCUPPer,       //12
	NSSSS,			//13 obsolete (old exe based song sharing), number kept for compatibility
	NSSSC,			//14 obsolete
	NSCGraph,       //15
	NSCPC,			//16 player conditions //is the data size enough to use 16?
	NSCCHS,			//17 checkhassong
	NSCAS,			//18 ask song
	NSRSSF,			//19 obsolete
	// Song sharing over a direct TCP connection (see ShareSongTransfer.h)
	NSSOffer,		//20 server->sender: [id][receiver][includeVideo] send your current song
	NSSReady,		//21 sender->server: [id][port2][token4][ip]  server->receiver: [id][sender][port2][token4][ip][ip2]
	NSSProgress,	//22 receiver->server: [id][doneKB4][totalKB4]  server->all: [sender][receiver][doneKB4][totalKB4][active]
	NSSResult,		//23 sender/receiver->server: [id][isReceiver][result][message]
	NSSCancel,		//24 server->sender/receiver: [id] abort the transfer
	NUM_NS_COMMANDS
};

const NSCommand NSServerOffset = (NSCommand)128;

struct EndOfGame_PlayerData
{
	int name;
	int score;
	int grade;
	Difficulty difficulty;
	int tapScores[NETNUMTAPSCORES];	//This will be a const soon enough
	CString playerOptions;
	CString percentage;
	float Graph[NETGRAPHSIZE];
};

enum NSScoreBoardColumn
{
	NSSB_NAMES=0,
	NSSB_COMBO,
	NSSB_GRADE,
	NUM_NSSB_CATEGORIES
};
#define FOREACH_NSScoreBoardColumn( sc ) FOREACH_ENUM( NSScoreBoardColumn, NUM_NSSB_CATEGORIES, sc )

class EzSockets;
class StepManiaLanServer;
class ShareSongTransfer;

/* Progress of a song transfer, one entry per client (row of the user list). */
struct ShareProgressInfo
{
	bool active;
	bool uploading;			// true: this client sends, false: this client receives
	unsigned int doneKB;
	unsigned int totalKB;	// 0 while the transfer is still connecting
	ShareProgressInfo() : active(false), uploading(false), doneKB(0), totalKB(0) { }
};

class PacketFunctions
{
public:
	unsigned char Data[NETMAXBUFFERSIZE];	//Data
	int Position;				//Other info (Used for following functions)

	//Commands used to operate on NetPackets
	uint8_t Read1();
	uint16_t Read2();
	uint32_t Read4();
	CString ReadNT();

	void Write1(uint8_t Data);
	void Write2(uint16_t Data);
	void Write4(uint32_t Data);
	void WriteNT(const CString& Data);

	void ClearPacket();

	CString fromIp;
};

class NetworkSyncManager 
{
public:
	NetworkSyncManager( LoadingWindow *ld = NULL );
	~NetworkSyncManager();

    //If "useSMserver" then send score to server
	void ReportTiming(float offset, int PlayerNumber);
	void ReportScore(int playerID, int step, int score, int combo);
		
	void ReportSongOver();	//Report to server that song is over
	void ReportStyle();		//Report to server the style, players, and names
	void ReportNSSOnOff(int i);	//Report song selection screen on/off
	void StartRequest(short position);	//Request a start.  Block until granted.
	bool Connect(const CString& addy, unsigned short port); // Connect to SM Server

	void PostStartUp(const CString& ServerIP);

	void CloseConnection();

	void DisplayStartupStatus();	//Used to note user if connect attempt was sucessful or not.

	int m_playerLife[NUM_PLAYERS];	//Life (used for sending to server)

	void Update(float fDeltaTime);

	bool useSMserver;

	vector <int> m_PlayerStatus;
	int m_ActivePlayers;
	vector <int> m_ActivePlayer;
	vector <CString> m_PlayerNames;
	vector <int> m_PlayerCondition;
	int ClientNum;

	//Used for ScreenNetEvaluation
	EndOfGame_PlayerData m_EvalPlayerData[NETMAXPLAYERS];

	//Used togeather for 
	bool ChangedScoreboard(int Column);	//If scoreboard changed since this function last called, then true.
	CString m_Scoreboard[NUM_NSSB_CATEGORIES];

	//Used for chatting
	void SendChat(const CString& message);
	CString m_WaitingChat;

	//Used for options
	void ReportPlayerOptions();
	void ReportPercentage();
	void ReportGraph();

	//Used for song checking/changing
	CString m_sMainTitle;
	CString m_sArtist;
	CString m_sSubTitle;

	CString m_sCurMainTitle;
	CString m_sCurArtist;
	CString m_sCurSubTitle;
	int m_iSelectMode;
	int m_ihash;
	void SelectUserSong();
	void SendHasSong(bool hasSong);
	void SendAskSong();
	CString			m_sChatText;

	bool isLanServer;	//Must be public for ScreenNetworkOptions
	StepManiaLanServer *LANserver;

	//Song sharing
	vector<ShareProgressInfo> m_PlayerShareProgress;	// indexed like the user list
	bool m_bShareReloadPending;		// a received song is on disk and has to be loaded
	CString m_sShareReloadSongDir;	// that song's folder, e.g. "Songs/connect/My Song/"
private:
#if !defined(WITHOUT_NETWORKING)

	void ProcessInput();

	void StartUp();

	float m_lastOffset[2];	//This is used to determine how much
						//the last step was off.

	int m_playerID;  //these are currently unused, but need to stay
	int m_step;
	int m_score;
	int m_combo;
    
	int m_startupStatus;	//Used to see if attempt was sucessful or not.

	bool m_scoreboardchange[NUM_NSSB_CATEGORIES];

	CString m_ServerName;
 
    EzSockets *NetPlayerClient;

	int m_ServerVersion; //ServerVersion

	bool Listen(unsigned short port);

	PacketFunctions m_packet;
	CString m_sServerAddress;	// address used to reach the game server

	//Song sharing (see ShareSongTransfer.h)
	struct PendingShare
	{
		bool valid;
		bool isSender;
		int id;
		bool includeVideo;			// sender
		vector<CString> hosts;		// receiver
		unsigned short port;		// receiver
		unsigned int token;			// receiver
		PendingShare() : valid(false), isSender(false), id(0), includeVideo(false), port(0), token(0) { }
	};
	ShareSongTransfer *m_pShare;
	PendingShare m_PendingShare;	// request that arrived while the previous transfer was still closing
	bool m_bShareReadySent;
	bool m_bShareSupplement;		// receiver: adding videos to a song we already have
	CString m_sShareSupplementDir;	// ...whose folder is this (game path)
	float m_fShareReportTimer;
	unsigned int m_iShareLastReportedKB;
	void UpdateShare(float fDeltaTime);
	void StartShare(const PendingShare &req);
	void SendSharePacket(PacketFunctions &packet);
	void SendShareResult(int id, bool isReceiver, int result, const CString &message);
	Song *FindCurrentNetSong();
#endif
};

extern NetworkSyncManager *NSMAN;
 
#endif
 
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
