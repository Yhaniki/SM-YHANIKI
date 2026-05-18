// =============================================================================
//  StandAloneServer/main.cpp
//   Headless dedicated server for StepMania YHANIKI, using Steam P2P lobbies.
//
//   Usage:
//     StandAloneServer.exe [room_code]
//
//   - If no room_code is supplied, a random 5-digit code is generated.
//   - This server account becomes the Steam lobby host and is automatically
//     registered as Client[0] inside StepManiaLanServer (it doesn't actually
//     play - players who join the lobby will be the real clients).
//   - Press Ctrl+C to gracefully stop the server.
//
//   Build notes:
//     * Requires Steam client to be running and a valid steam_appid.txt next
//       to the executable (same appid as the main StepMania binary uses).
//     * The reused source files live in ../src/ (NetworkSyncServer.cpp and
//       ezsockets.cpp).  See StandAloneServer.vcxproj for the include order
//       that lets shim/ headers override a small subset of src/ headers.
// =============================================================================

#include "global.h"
#include "RageLog.h"
#include "PrefsManager.h"
#include "NetworkSyncServer.h"

#include <winsock2.h>
#include <windows.h>
#include "steam/steam_api.h"
#include "steam/isteamnetworkingsockets.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <string>
#include <signal.h>

extern RageLog     *LOG;
extern PrefsManager*PREFSMAN;

// =====================================================================
// Ctrl+C / window-close handling: set a flag and let the main loop exit
// gracefully so ServerStop() can clean up sockets / Steam state.
// =====================================================================
static std::atomic<bool> g_stopRequested(false);

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		fprintf(stdout, "\n[StandAloneServer] Stop requested (Ctrl signal received)\n");
		fflush(stdout);
		g_stopRequested.store(true);
		return TRUE;
	}
	return FALSE;
}

// =====================================================================
// Initialize / shutdown WinSock.  EzSockets uses it for the legacy LAN
// path; Steam path doesn't strictly need it, but ezsockets.cpp calls
// WSAStartup unconditionally so we must mirror that here.
// =====================================================================
static bool InitWinsock()
{
	WSADATA wsa;
	int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0) {
		fprintf(stderr, "[StandAloneServer] WSAStartup failed: %d\n", err);
		return false;
	}
	return true;
}

// =====================================================================
// 5-digit numeric room code, exactly the same format the in-game server
// uses (see StepManiaLanServer::GenerateRoomCode).
// =====================================================================
static std::string GenerateRoomCode()
{
	int code = 1000 + std::rand() % 90000;
	std::string s = std::to_string(code);
	while (s.length() < 5) s = "0" + s;
	return s;
}

int main(int argc, char* argv[])
{
	// --- Console setup ---
	SetConsoleTitleA("StepMania YHANIKI - Standalone Server");
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
	std::srand((unsigned)time(NULL));

	// --- Wire up the globals reused .cpp files expect ---
	LOG      = new RageLog();
	PREFSMAN = new PrefsManager();

	LOG->Info("[StandAloneServer] starting...");

	if (!InitWinsock())
	{
		delete LOG; delete PREFSMAN;
		return 1;
	}

	// --- Decide room code: prefer argv[1] over a random one. ---
	std::string roomCode = (argc >= 2 && argv[1] && argv[1][0])
		? std::string(argv[1])
		: GenerateRoomCode();

	LOG->Info("[StandAloneServer] using room code: %s", roomCode.c_str());

	// --- Create + start the server (Steam lobby variant). ---
	StepManiaLanServer server;
	server.servername = "Dedicated YHANIKI Server";
	server.roomCode   = CString(roomCode.c_str());

	bool ok = server.ServerStart(CString(roomCode.c_str()));
	if (!ok)
	{
		LOG->Warn("[StandAloneServer] ServerStart failed.  Make sure Steam is "
		          "running and steam_appid.txt sits next to the .exe.");
		WSACleanup();
		delete LOG; delete PREFSMAN;
		return 2;
	}

	// Report the lobby ID so admin can verify the lobby actually exists.
	CSteamID lobbyId = server.GetLobbyId();
	LOG->Info("[StandAloneServer] server up - room=%s  lobbyId=%llu",
	          roomCode.c_str(),
	          (unsigned long long)lobbyId.ConvertToUint64());
	LOG->Info("[StandAloneServer] Press Ctrl+C to stop.");

	// --- Main update loop ---
	// Tick rate ~20Hz is enough for chat / room control; file-transfer chunks
	// already self-throttle inside the share thread on the client side.
	//
	// IMPORTANT: Two different callback queues need to be pumped here:
	//   1) SteamAPI_RunCallbacks                 -> Steam Lobby / Friends / etc.
	//   2) SteamNetworkingSockets()->RunCallbacks-> P2P connection state
	//      changes (Connecting / Connected / ClosedByPeer ...).
	// Only pumping #1 leaves incoming P2P connection requests stuck inside
	// the Steam library, so the server NEVER sees a new client, even though
	// it shows up in the Steam lobby member list.  That's the symptom of
	// "remote client joins but gets no response and times out".
	const DWORD kTickMs = 50;
	while (!g_stopRequested.load())
	{
		server.ServerUpdate();
		SteamAPI_RunCallbacks();
		ISteamNetworkingSockets *sockets = SteamNetworkingSockets();
		if (sockets != nullptr)
			sockets->RunCallbacks();
		Sleep(kTickMs);
	}

	LOG->Info("[StandAloneServer] shutting down...");
	server.ServerStop();
	SteamAPI_Shutdown();
	WSACleanup();

	LOG->Info("[StandAloneServer] bye.");
	delete LOG;       LOG = nullptr;
	delete PREFSMAN;  PREFSMAN = nullptr;
	return 0;
}
