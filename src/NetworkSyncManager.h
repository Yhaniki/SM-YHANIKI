/* NetworkSyncManager - Uses ezsockets for primitive song syncing and score reporting. */

#ifndef NetworkSyncManager_H
#define NetworkSyncManager_H

#include "PlayerNumber.h"
#include "Difficulty.h"
#include <windows.h>

class LoadingWindow;

#define NETMAXPLAYERS 32
const int NETPROTOCOLVERSION=1;
// 從 1020 提高到 65500（~64KB）以大幅加速 share song 檔案傳輸。
// Steam P2P reliable 單一 message 上限是 524288 bytes (512KB)，64KB 仍有充足 margin。
// 影響：每個 PacketFunctions 物件記憶體從 ~1KB 變 ~64KB，目前實例數量不多 OK。
const int NETMAXBUFFERSIZE=65500;
const int NETNUMTAPSCORES=8;
const int NETGRAPHSIZE=100;

// 分享歌曲時每個資料 chunk 的大小上限（其餘空間留給 packet header + 檔案路徑）
// 從 800 提高到 60000，配合 NETMAXBUFFERSIZE 一起放大，吞吐量提升約 75x
const int NETSHARECHUNKSIZE = 60000;

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
	NSCUPPer,		//12
	NSSSS,			//13 share song server (legacy 觸發訊息，仍用來通知 sender 開始)
	NSSSC,			//14 share song client (legacy 通知 receiver 對方要傳檔)
	NSCGraph,		//15
	NSCPC,			//16 player conditions //is the data size enough to use 16?
	NSCCHS,			//17 checkhassong
	NSCAS,			//18 ask song
	NSRSSF,			//19 share song finish
	NSSMeta,		//20 share song: 一次傳輸的 metadata (檔案數/總 bytes)
	NSSData,		//21 share song: 單一檔案資料 chunk
	NSSDone,		//22 share song: 全部檔案傳輸完成
	NSSCancel,		//23 share song: 中止傳輸
	NSSProgress,	//24 share song: server 回傳給所有 client 的進度
	NSSXferAck,		//25 share song: receiver 回報「我實際收到 N bytes」(用來做真實進度 + sender 等待 ACK)
	NUM_NS_COMMANDS
};

typedef enum
{
	CONDITION_NORMAL,
	CONDITION_LACK_SONG,
	CONDITION_LEAVE_ROOM,
	CONDITION_NUM
} PLAYER_CONDITION;

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

class PacketFunctions
{
public:
	unsigned char Data[NETMAXBUFFERSIZE];	//Data
	int Position;				//Other info (Used for following functions)
	int PayloadLength;          //收到 packet 時記錄實際長度，方便 server 轉發時知道有效範圍

	//Commands used to operate on NetPackets
	uint8_t Read1();
	uint16_t Read2();
	uint32_t Read4();
	CString ReadNT();
	// 讀取原始 bytes (回傳實際讀到的數量)
	int ReadBytes(char *out, int bytes);

	void Write1(uint8_t Data);
	void Write2(uint16_t Data);
	void Write4(uint32_t Data);
	void WriteNT(const CString& Data);
	// 寫入原始 bytes (不附 length，呼叫者需自己先寫長度)
	void WriteBytes(const char *src, int bytes);

	void ClearPacket();

	CString fromIp;
};

// 分享歌曲時，UI 端需要的進度資訊 (server 端會廣播給所有 client)
struct ShareProgressInfo
{
	bool active;          // 是否正在傳送
	bool uploading;       // true: 此玩家為發送端, false: 此玩家為接收端
	int  peerIndex;       // 對方在 m_PlayerNames 中的 index
	int  currentBytes;    // 目前已傳輸 bytes
	int  totalBytes;      // 總 bytes
	ShareProgressInfo() : active(false), uploading(false), peerIndex(-1), currentBytes(0), totalBytes(0) {}
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
	void ReportShareSongFinish();
	void ReportStyle();		//Report to server the style, players, and names
	void ReportNSSOnOff(int i);	//Report song selection screen on/off
	void StartRequest(short position);	//Request a start.  Block until granted.
	bool Connect(const CString& addy, unsigned short port); // Connect to SM Server
	bool Connect(const CString& roomCode);
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
	// 每位玩家的分享歌曲進度 (由 server 廣播更新；index 對應 m_PlayerNames 的 player index)
	vector <ShareProgressInfo> m_PlayerShareProgress;
	int ClientNum;

	// 由 UI 或 server 命令發起：嘗試取消目前進行中的分享 (若本機是 sender 會中止 thread；
	// 不論身份都會廣播 NSSCancel 給 server)
	void CancelShareSong();
	// 由 server 端透過 chat command (/cancel) 觸發，直接送 NSSCancel
	void SendShareCancel();
	bool IsShareSongActive() const;

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
	static DWORD WINAPI StaticThreadStartNSSSS(void *Param)
	{
		NetworkSyncManager *This = (NetworkSyncManager *)Param;
		return This->ThreadProcNSSSS();
	}
	DWORD ThreadProcNSSSS(void);

	CString server_ip;
	int file_size;
	int player_num;
	bool video_file_filter;
	bool usingShareSongSystem;

	// === Share-song：sender 端使用 ===
	volatile bool m_shareCancelRequested; // 由 UI/server 設成 true 來通知 sender thread 退出
	volatile int  m_shareSentBytes;       // 給 UI 觀察用 (queue 到 Steam 的量)
	volatile int  m_shareTotalBytes;
	volatile int  m_shareReceiverAckedBytes; // receiver 回報它實際已收到的 bytes (NSSXferAck 更新)
	int m_shareReceiverIndex;             // sender 要傳給誰

	// === Share-song：receiver 端使用 (由 main thread 在 ProcessInput 中操作) ===
	struct RecvState
	{
		bool active;
		int  senderIndex;
		int  totalBytes;
		int  receivedBytes;
		int  fileCount;
		CString rootDir;       // 接收後解開後的最頂層資料夾 (歌曲名)
		CString currentRelPath;
		FILE *currentFile;
		int  currentFileSize;
		int  currentFileWritten;        // 最大 offset + got (用來判斷 "sender 是否送完此檔")
		int  currentFileBytesWritten;   // 真實累積 fwrite bytes (用來偵測 sparse padding 假象)
		vector<CString> openedFiles;     // 本次 transfer 開過的相對路徑 (排查殘檔用)
		vector<int>     openedExpected;  // 對應的 expected size
		vector<int>     openedWritten;   // 對應的最終 written (=max offset+got，可能高估)
		vector<int>     openedActualBytes; // 對應的「實際 fwrite bytes」(可以 < written，代表中間有缺)
		int  lastAckedBytes;             // 上次送 NSSXferAck 時的 receivedBytes (節流用)
	};
	RecvState m_recv;

	// 內部 helper
	void ResetRecvState();
	void OpenRecvFile(const CString& relPath, int fileSize);
	void CloseRecvFile();
	void RemovePartialRecv();
	void SendShareProgress(); // sender 端呼叫，回報目前進度給 server
	void SendRecvAck();       // receiver 端呼叫，回報實際收到 bytes 給 sender (經 server 轉發)
	// 統一的 SendPack 出口：負責加上 g_hMutex 保護，避免 main thread 與 share sender thread
	// 同時 append outBuffer 把 [len][payload] 序列撞壞。
	// 所有 NetPlayerClient->SendPack(...) 都應改走這個函式。
	void SendNSMPacket(PacketFunctions& pkt);
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
