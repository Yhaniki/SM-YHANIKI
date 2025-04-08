/*******************************************************************\
| ezsockets.cpp: EzSockets Class Source                             |
|   Designed by Josh Allen, Charles Lohr and Adam Lowman.           |
|   Socket programming methods based on Charles Lohr's EZW progam.  |
|   Modified by Charles Lohr for use with Windows-Based OSes.       |
|   UDP/NON-TCP Support by Adam Lowman.                             |
\*******************************************************************/
#include "global.h"
#include "ezsockets.h"
#include "RageLog.h"

#if defined(_XBOX)
#elif defined(_WINDOWS) // We need the WinSock32 Library on Windows
#include"Winsock2.h"
#pragma comment(lib,"wsock32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

EzSockets::EzSockets()
{
	MAXCON = 5;
	memset (&addr,0,sizeof(addr)); //Clear the sockaddr_in structure
	
#if defined(_WINDOWS) || defined(_XBOX) // Windows REQUIRES WinSock Startup
	WSAStartup( MAKEWORD(1,1), &wsda );
#endif
	
	sock = -1;
	blocking = true;
	scks = new fd_set;
	times = new timeval;
	times->tv_sec = 0;
	times->tv_usec = 0;
	state = skDISCONNECTED;
}

EzSockets::~EzSockets()
{
	close();
	delete scks;
	delete times;
}


//Check to see if the socket has been created
bool EzSockets::check()
{
	return sock > 0;
}

bool EzSockets::create()
{
	return create(IPPROTO_TCP, SOCK_STREAM);
}

bool EzSockets::create(int Protocol)
{
	switch(Protocol)
	{
	case IPPROTO_TCP:
		return create(IPPROTO_TCP, SOCK_STREAM);
	case IPPROTO_UDP:
		return create(IPPROTO_UDP, SOCK_DGRAM);
	default:
		//XBOX does not support the raw socket.
		//So, since there's no need, we aren't
		//going to allow it on XBOX
#if defined(_XBOX)
		return false;
#else
		return create(Protocol, SOCK_RAW);
#endif
			
	}
}

bool EzSockets::create(int Protocol, int Type)
{
	state = skDISCONNECTED;
	sock = socket(AF_INET, Type, Protocol);
	lastCode = sock;

	return sock > 0;
}


bool EzSockets::bind(unsigned short port)
{
	if(!check())
		return false;
	
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(port);
	lastCode = ::bind(sock,(struct sockaddr*)&addr, sizeof(addr));
	return !lastCode;
}

bool EzSockets::listen()
{
	lastCode = ::listen(sock, MAXCON);
	if (lastCode)
		return false;
	
	state = skLISTENING;
	return true;
}

#if defined(WIN32)
typedef int socklen_t;
#endif

bool EzSockets::accept(EzSockets& socket)
{
	
	if (!blocking && !CanRead())
		return false;
		
	#if defined(HAVE_INET_NTOP)
		char buf[INET_ADDRSTRLEN];

		inet_ntop(AF_INET, &addr.sin_addr, buf, INET_ADDRSTRLEN);
		address = buf;
	#elif defined(HAVE_INET_NTOA)
		address = inet_ntoa(addr.sin_addr);
	#endif
	
	int length = sizeof(socket);
	// socket.address = address;
	socket.sock = ::accept(sock,(struct sockaddr*) &socket.addr, 
						   (socklen_t*) &length);

	CString strstrstr = socket.getIp();
	
	lastCode = socket.sock;

	if (socket.sock <= 0)
		return false;
	
	socket.state = skCONNECTED;
	return true;
}

void EzSockets::close()
{
	state = skDISCONNECTED;
	inBuffer = "";
	outBuffer = "";
	
#if defined(WIN32) // The close socket command is different in Windows
	::closesocket(sock);
#else
	::close(sock);
#endif
}

long EzSockets::uAddr()
{
	return addr.sin_addr.s_addr;
}


bool EzSockets::connect(const std::string& host, unsigned short port)
{
	if(!check())
		return false;
	
#if defined(_XBOX)
	// FIXME: Xbox doesn't have gethostbyname or any way to get a hostent.  
	// Investigate the samples and figure out how this is supposed to work.
	return false;
#else
	struct hostent* phe;
	phe = gethostbyname(host.c_str());
	if (phe == NULL)
		return false;
	memcpy(&addr.sin_addr, phe->h_addr, sizeof(struct in_addr));
#endif 
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
	
	if(::connect(sock, (struct sockaddr*)&addr, sizeof(addr)))
		return false;
	
	state = skCONNECTED;
	return true;
}

bool EzSockets::CanRead()
{
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1,scks,NULL,NULL,times) > 0;
}

bool EzSockets::IsError()
{
	if (state == skERROR)
		return true;
	
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	if (select(sock+1, NULL, NULL, scks, times) >=0 )
		return false;
	
	state = skERROR;
	return true;
}

bool EzSockets::CanWrite()
{
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1, NULL, scks, NULL, times) > 0;
}

void EzSockets::update()
{
	if (IsError()) //If socket is in error, don't bother.
		return;
	
	while (CanRead() && !IsError()) //Check for Reading
		if (pUpdateRead() < 1)
			break;
	
	if (CanWrite() && (outBuffer.length()>0))
		pUpdateWrite();
}


/*********************\
|   Raw Data System   |
\*********************/
void EzSockets::SendData(const string& outData)
{
	outBuffer.append(outData);
	if(blocking)
		while ((outBuffer.length()>0) && !IsError())
			pUpdateWrite();
	else
		update();
}

void EzSockets::SendData(const char *data, unsigned int bytes)
{
	outBuffer.append(data, bytes);
	if(blocking)
		while ((outBuffer.length()>0) && !IsError())
			pUpdateWrite();
	else
		update();
}

int EzSockets::ReadData(char *data, unsigned int bytes)
{	
	int bytesRead = PeekData(data,bytes);
	inBuffer = inBuffer.substr(bytesRead);
	return bytesRead;
}

int EzSockets::PeekData(char *data, unsigned int bytes)
{
	if (blocking)
		while ((inBuffer.length()<bytes) && !IsError())
			pUpdateRead();
	else
		while (CanRead() && !IsError())
			if (pUpdateRead()<1)
				break;
	
	int bytesRead = bytes;
	if (inBuffer.length()<bytes)
		bytesRead = inBuffer.length();
	memcpy(data,inBuffer.c_str(), bytesRead);
	
	return bytesRead;
}


/**********************************\
|   Packet/Structure Data System   |
\**********************************/
void EzSockets::SendPack(const char *data, unsigned int bytes)
{
	unsigned int SendSize = htonl(bytes);
	outBuffer.append( (const char *) & SendSize, 4);	//Add size to buffer, but don't send yet.
	SendData(data, bytes);
}

int EzSockets::ReadPack(char *data, unsigned int max)
{
	int size = PeekPack(data, max);
	
	if (size != -1)
		inBuffer = inBuffer.substr(size+4);
	
	return size;
}

int EzSockets::PeekPack(char *data, unsigned int max)
{
	if (CanRead())
		pUpdateRead();
	
	if (blocking)
	{
		while ((inBuffer.length()<4) && !IsError())
			pUpdateRead();
		
		if (IsError())
			return -1;
	}
	
	if (inBuffer.length()<4)
		return -1;
	
	unsigned int size;
	PeekData((char*)&size, 4);
	size = ntohl(size);
	
	if (blocking)
		while (inBuffer.length()<(size+4) && !IsError())
			pUpdateRead();
	else
		if (inBuffer.length()<(size+4) || inBuffer.length()<=4)
			return -1;
	
	if (IsError())
		return -1; 
	//What if we get disconnected while waiting for data?
	
	string tBuff(inBuffer.substr(4, size));
	if (tBuff.length() > max)
		tBuff.substr(0, max);
	
	memcpy (data, tBuff.c_str(),tBuff.length());
	return size;
}


/*****************************************\
|   Null Terminating String Data System   |
\*****************************************/
void EzSockets::SendStr(const string& data, char delim)
{
	char tDr[1];
	tDr[0] = delim;
	SendData(data.c_str(), data.length());
	SendData(tDr, 1);
}

int EzSockets::ReadStr(string& data, char delim)
{
	int t = PeekStr(data, delim);
	if (t >= 0)
		inBuffer = inBuffer.substr(t+1);
	return t;
}

int EzSockets::PeekStr(string& data, char delim)
{
	int t = inBuffer.find(delim,0);
	if (blocking)
	{
		while (t == -1 && !IsError())
		{
			pUpdateRead();
			t = inBuffer.find(delim, 0);
		}
		data = inBuffer.substr(0, t);
	}
	
	if(t >= 0)
		data = inBuffer.substr(0, t);
	return t;
}


/************************\
|   Stream Data System   |
\************************/
istream& operator>>(istream &is, EzSockets& obj)
{
	string writeString;
	obj.SendStr(writeString);
	is >> writeString;
	return is;
}

ostream& operator<<(ostream &os, EzSockets &obj)
{
	string readString;
	obj.ReadStr(readString);
	os << readString;
	return os;
}


/**************************\
|   Internal Data System   |
\**************************/
int EzSockets::pUpdateRead()
{
	char tempData[1024];
	int bytes = pReadData(tempData);
	
	if (bytes > 0)
		inBuffer.append(tempData, bytes);
	else if (bytes <= 0)
		/* To get her I think CanRead was called at least once.
		So if length equals 0 and can read says there is data than 
		the socket was closed.*/
		state = skERROR;
	return bytes;
}

int EzSockets::pUpdateWrite()
{
	int bytes = pWriteData(outBuffer.c_str(), outBuffer.length());
	
	if (bytes > 0)
		outBuffer = outBuffer.substr(bytes);
	else if (bytes < 0)
		state = skERROR;
	return bytes;
}


int EzSockets::pReadData(char* data)
{
	if(state == skCONNECTED || state == skLISTENING)
		return recv(sock, data, 1024, 0);
	
	fromAddr_len = sizeof(sockaddr_in);
	return recvfrom(sock, data, 1024, 0, (sockaddr*)&fromAddr,
					(socklen_t*)&fromAddr_len);
}

int EzSockets::pWriteData(const char* data, int dataSize)
{
	return send(sock, data, dataSize, 0);
}

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


CString EzSockets::getIp()
{
	struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    getsockname(sock, (struct sockaddr*)&name, &namelen);
	
	char* str = inet_ntoa(name.sin_addr);
	CString cstr = str;
	return cstr;	
}

#include <iostream>
#include <string>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include "steam/steam_api.h"

CSteamID gLobbyID;
std::string gRoomCode;
std::atomic<bool> gLobbyJoined{ false };
std::mutex gInputMutex;

class SteamLobbyExample {
public:
	SteamLobbyExample()
		: m_LobbyCreated(this, &SteamLobbyExample::OnLobbyCreated),
		m_LobbyEnter(this, &SteamLobbyExample::OnLobbyEnter),
		m_LobbyMatchList(this, &SteamLobbyExample::OnLobbyMatchList),
		m_LobbyChatUpdate(this, &SteamLobbyExample::OnLobbyChatUpdate),
		m_LobbyChatMsg(this, &SteamLobbyExample::OnLobbyChatMsg) {}

	void CreateLobby() {
		int code = 1000 + std::rand() % 90000;
		gRoomCode = std::to_string(code);
		SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 4);
	}

	void SearchLobby(const std::string& code) {
		gRoomCode = code;
		SteamMatchmaking()->AddRequestLobbyListStringFilter("room_code", code.c_str(), k_ELobbyComparisonEqual);
		SteamMatchmaking()->RequestLobbyList();
	}

	void SendChatMessage(const std::string& message) {
		if (!gLobbyJoined) return;
		SteamMatchmaking()->SendLobbyChatMsg(gLobbyID, message.c_str(), static_cast<int>(message.size()) + 1);
	}

private:
	CCallback<SteamLobbyExample, LobbyCreated_t> m_LobbyCreated;
	CCallback<SteamLobbyExample, LobbyEnter_t> m_LobbyEnter;
	CCallback<SteamLobbyExample, LobbyMatchList_t> m_LobbyMatchList;
	CCallback<SteamLobbyExample, LobbyChatUpdate_t> m_LobbyChatUpdate;
	CCallback<SteamLobbyExample, LobbyChatMsg_t> m_LobbyChatMsg;

	void OnLobbyCreated(LobbyCreated_t* pCallback) {
		if (pCallback->m_eResult == k_EResultOK) {
			gLobbyID = pCallback->m_ulSteamIDLobby;
			SteamMatchmaking()->SetLobbyData(gLobbyID, "room_code", gRoomCode.c_str());
			std::cout << "[HOST] Lobby created. Room code: " << gRoomCode << std::endl;

			// std::string status = "Room " + gRoomCode;
			// SteamFriends()->SetRichPresence("status", status.c_str());
			// SteamFriends()->SetRichPresence("steam_display", "#Status_InRoom");
		}
		else {
			std::cout << "[HOST] Failed to create lobby. Error code: " << pCallback->m_eResult << std::endl;
		}
	}

	void OnLobbyEnter(LobbyEnter_t* pCallback) {
		gLobbyID = pCallback->m_ulSteamIDLobby;
		CSteamID self = SteamUser()->GetSteamID();
		std::string name = SteamFriends()->GetFriendPersonaName(self);
		std::cout << "[JOIN] You (" << name << ") have entered lobby ID: " << gLobbyID.ConvertToUint64() << std::endl;
		gLobbyJoined = true;
	}

	void OnLobbyMatchList(LobbyMatchList_t* pCallback) {
		int matches = pCallback->m_nLobbiesMatching;
		std::cout << "[CLIENT] Found " << matches << " matching lobbies." << std::endl;

		for (int i = 0; i < matches; ++i) {
			CSteamID lobbyID = SteamMatchmaking()->GetLobbyByIndex(i);
			std::string code = SteamMatchmaking()->GetLobbyData(lobbyID, "room_code");
			if (code == gRoomCode) {
				std::cout << "[CLIENT] Joining lobby with room code: " << code << std::endl;
				SteamMatchmaking()->JoinLobby(lobbyID);
				return;
			}
		}

		std::cout << "[CLIENT] No lobby with matching code found." << std::endl;
	}

	void OnLobbyChatUpdate(LobbyChatUpdate_t* pCallback) {
		CSteamID userChanged = pCallback->m_ulSteamIDUserChanged;
		std::string name = SteamFriends()->GetFriendPersonaName(userChanged);

		if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) {
			std::cout << "[LOBBY] " << name << " joined the lobby." << std::endl;
		}
		else if (pCallback->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft) {
			std::cout << "[LOBBY] " << name << " left the lobby." << std::endl;
		}
	}

	void OnLobbyChatMsg(LobbyChatMsg_t* pCallback) {
		char buffer[4096] = {};
		EChatEntryType chatType;
		CSteamID sender;
		int len = SteamMatchmaking()->GetLobbyChatEntry(pCallback->m_ulSteamIDLobby,
			pCallback->m_iChatID, &sender, buffer, sizeof(buffer), &chatType);
		if (len > 0) {
			std::string name = SteamFriends()->GetFriendPersonaName(sender);
			std::cout << name << ": " << buffer << std::endl;
		}
	}
};
