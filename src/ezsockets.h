/*******************************************************************\
| ezsockets.h: EzSockets Class Header                               |
|   Designed by Josh Allen, Charles Lohr and Adam Lowman.           |
|   Socket programming methods based on Charles Lohr's EZW progam.  |
|   Modified by Charles Lohr for use with Windows-Based OSes.       |
|   UDP/NON-TCP Support by Adam Lowman.                             |
\*******************************************************************/
 
#ifndef EZSOCKETS_H
#define EZSOCKETS_H

#if defined(WITHOUT_NETWORKING)
#error do not include ezsockets.h when WITHOUT_NETWORKING
#endif

#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <ctype.h>

#if defined(_XBOX)
// Summary : WinsockX is bad, XTL is good.
// Explained : WinsockX may rely on some declares 
//			   that are present in XTL. Also, using
//			   XTL includes some files maybe needed
//			   for other operations on Xbox.
#include <xtl.h>
#elif defined(_WINDOWS)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#include "steam/steam_api.h"
#include "steam/steamnetworkingtypes.h"
using namespace std;

typedef enum {
    ROLE_UNKNOWN = 0,
    ROLE_SERVER,
    ROLE_CLIENT
} RoleType;

class EzSockets
{
public:

	EzSockets();
	~EzSockets();

	//Crate the socket
	bool create();
	bool create(int Protocol);
	bool create(int Protocol, int Type);

	//Bind Socket to local port
	bool bind(unsigned short port);

	//Listen with socket
	bool listen();

	//Accept incomming socket
	bool accept(EzSockets &socket);

	//Connect
	bool connect(const string& host, unsigned short port);

	//Kill socket
	void close();

	//see if socket has been created
	bool check();

	long uAddr();

	bool CanRead();
	bool IsError();
	bool CanWrite();
	
	void update();

	//Raw data system 
	void SendData(const string& outData);
	void SendData(const char *data, unsigned int bytes);
	int ReadData(char *data, unsigned int bytes);
	int PeekData(char *data, unsigned int bytes);

	//Packet system (for structures and classes)
	void SendPack(const char *data, unsigned int bytes); 
	int ReadPack(char *data, unsigned int max);
	int PeekPack(char *data, unsigned int max);

	//String (Flash) system / Null-terminated strings
	void SendStr(const string& data, char delim = '\0');
	int ReadStr(string& data, char delim = '\0');
	int PeekStr(string& data, char delim = '\0');


	//Operators
	char operator[] (int i); //Access buffer
	friend istream& operator>>(istream& is, EzSockets& obj);
	friend ostream& operator<<(ostream& os, const EzSockets& obj);

	bool blocking;
	enum SockState
	{ 
		skDISCONNECTED = 0, 
		skUNDEF1, //Not implemented
		skLISTENING, 
		skUNDEF3, //Not implemented
		skUNDEF4, //Not implemented
		skUNDEF5, //Not implemented
		skUNDEF6, //Not implemented
		skCONNECTED, 
		skERROR 
	};
    struct sockaddr_in fromAddr;
	unsigned long fromAddr_len;

	//The following possibly should be private.
	string inBuffer;
	string outBuffer;

	int pUpdateWrite();
	int pUpdateRead();

	int pReadData(char * data);
	int pWriteData(const char * data, int dataSize);

	SockState state;

	int lastCode;	//Used for debugging purposes

	CString address;

	CString getIp();


private:

	//Only necessiary in windows, xbox
#if defined(_WINDOWS) || defined(_XBOX)
	WSADATA wsda;
#endif

	int MAXCON;
	int sock;
	struct sockaddr_in addr;


	//Used for Select() command
	fd_set  *scks;
	timeval *times;

	//Buffers
public:
	// Steam API related members
	bool InitializeSteamNetworking();
	void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);
	void InitStatusChanged();
	bool create(CString roomCode);
	bool connect(const string& roomCode);
	bool CheckUpdate(){return m_updated;}
	void ClearUpdate(){m_updated = false;}
	CSteamID GetLobbyId() { return m_lobbyID; }
	CSteamID GetSelfId() { return m_selfSteamID; }
	CSteamID GetHostId() {return m_hostSteamID;}
	void SetHostId(CSteamID id) {m_hostSteamID = id;}
	void SetSelfId(CSteamID id);
	void SetHandle(HSteamNetConnection conn) {m_conn = conn;}
	vector <HSteamNetConnection> m_conns;
private:
	void Reset();
	void SetupSteamCallbacks();

	STEAM_CALLBACK_MANUAL(EzSockets, OnLobbyCreated, LobbyCreated_t, m_LobbyCreatedCallback);
	STEAM_CALLBACK_MANUAL(EzSockets, OnLobbyMatchList, LobbyMatchList_t, m_LobbyMatchCallback);
	STEAM_CALLBACK_MANUAL(EzSockets, OnLobbyEnter, LobbyEnter_t, m_LobbyEnterCallback);
	STEAM_CALLBACK_MANUAL(EzSockets, OnLobbyChatUpdate, LobbyChatUpdate_t, m_LobbyChatUpdateCallback);
	bool m_useSteamNetworking;
	bool m_lobbyCreated;
	bool m_LobbyJoined;
	bool m_lobbySuccess;
	bool m_lobbyListReturned;
	bool m_lobbyFound;
	bool m_callbacksRegistered;
	bool m_updated;
	CSteamID m_lobbyID;
	std::string m_roomCode;
	std::string m_roomCodeTmp;
	CSteamID m_hostSteamID;
	CSteamID m_selfSteamID;
	RoleType m_roleType;
	HSteamListenSocket m_listenSock;
	HSteamNetConnection m_conn;
	HSteamNetConnection m_connHdl;
	bool m_connected;
	bool m_isInitialized;
};

istream& operator>>(istream& is, EzSockets& obj);
ostream& operator<<(ostream& os, EzSockets& obj);
bool SteamReady();

#endif

/*
 * (c) 2003-2004 Josh Allen, Charles Lohr, and Adam Lowman
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
