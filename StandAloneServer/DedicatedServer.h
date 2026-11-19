/* DedicatedServer - runs the game's own server (StepManiaLanServer from
 * src/NetworkSyncServer.cpp) on a worker thread for the standalone window.
 *
 * All room logic stays in the shared code; this class only
 *  - drives ServerUpdate() from a select() loop instead of the game's frame loop,
 *  - publishes a status snapshot the window can read at any time,
 *  - runs the window's commands (kick, host, ...) on the server thread.
 * Strings are UTF-8, like everything the game sends. */
#ifndef DEDICATED_SERVER_H
#define DEDICATED_SERVER_H

#include "NetworkSyncServer.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DedicatedClientInfo
{
	unsigned int id;		// stays the same while the player is connected
	unsigned int index;		// position in the room, as used by /share N; 0 is the host
	std::string names;		// "A" or "A&B"; empty until the game has sent them
	std::string ip;
	bool inSongSelect;
	bool inGame;
	bool starting;			// left song select, waiting for the others to start
	bool hasSong;
	bool sending;			// song transfer
	bool receiving;
};

struct DedicatedStatus
{
	bool running;
	std::vector<DedicatedClientInfo> clients;
	std::string songTitle;	// empty: the host hasn't picked a song
	std::string songSubtitle;
	std::string songArtist;
	bool shareActive;
	unsigned int shareSenderId;
	unsigned int shareReceiverId;
	unsigned int shareDoneKB;
	unsigned int shareTotalKB;
	unsigned int shareQueued;
	unsigned int bannedCount;

	DedicatedStatus() : running(false), shareActive(false), shareSenderId(0), shareReceiverId(0),
		shareDoneKB(0), shareTotalKB(0), shareQueued(0), bannedCount(0) { }
};

class DedicatedServer : public StepManiaLanServer
{
public:
	enum CommandType
	{
		CMD_CHAT,			// text: message from the server to everyone
		CMD_KICK,			// clientId
		CMD_BAN,			// clientId: kick and refuse its IP until the server stops
		CMD_MAKE_HOST,		// clientId
		CMD_FORCE_START,	// like the host's /start
		CMD_CANCEL_SHARES,
		CMD_CLEAR_BANS,
		CMD_SET_NAME		// text: new server name
	};

	DedicatedServer();
	~DedicatedServer();

	/* Opens TCP 8765 and starts the server thread.  On failure sError says why. */
	bool Start( const std::string &sServerName, std::string &sError );
	void Stop();

	void Post( CommandType type, unsigned int clientId = 0, const std::string &sText = std::string() );
	DedicatedStatus GetStatus();

protected:
	void OnClientConnected( unsigned int clientNum );
	void OnClientDisconnecting( unsigned int clientNum );
	void OnChatLine( const CString &sLine );

private:
	struct Command
	{
		CommandType type;
		unsigned int clientId;
		std::string text;
	};

	void ThreadMain();
	void WaitForNetwork( int iTimeoutMs );
	void FlushPendingSends();
	void RunCommands();
	void RunCommand( const Command &cmd );
	void PublishStatus();
	int FindClient( unsigned int clientId ) const;	// index in Client[] or -1
	unsigned int IdOf( int clientNum ) const;

	std::thread m_Thread;
	std::atomic<bool> m_bQuit;

	std::mutex m_CommandMutex;
	std::deque<Command> m_Commands;

	std::mutex m_StatusMutex;
	DedicatedStatus m_Status;
	unsigned long m_iLastPublish;
	bool m_bStatusDirty;

	/* Server thread only */
	std::map<const GameClient *, unsigned int> m_ClientIds;
	unsigned int m_iNextClientId;
	bool m_bWasInGame;
};

#endif
