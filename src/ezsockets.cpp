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
#include <iostream>
#include <locale>
#include <codecvt>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include <queue>
#include <mutex>
#include <unordered_map>

std::mutex g_clientToServerMutex;
std::mutex g_serverToClientMutex;

std::queue<SteamNetworkingMessage_t*> g_clientToServerQueue;
std::queue<SteamNetworkingMessage_t*> g_serverToClientQueue;
static std::unordered_map<HSteamNetConnection, EzSockets*> g_connToInstanceMap;
static std::mutex g_connMapMutex;
bool g_steamReady = false;
EzSockets* g_serverEzSocketsInstance = nullptr;

bool SendMessageWithLoopbackSupport(RoleType role,
									const SteamNetworkingIdentity &id,
									const void *data,
									uint32 size,
									HSteamNetConnection conn,
									int sendType = k_nSteamNetworkingSend_Reliable,
									int channel = 0)
{
	ISteamNetworkingMessages *net = SteamNetworkingMessages();
	if (!net) return false;

	CSteamID selfID = SteamUser()->GetSteamID();

	if (id.GetSteamID() == selfID)
	{
		// Create simulated message
		SteamNetworkingMessage_t *fakeMsg = SteamNetworkingUtils()->AllocateMessage(size);
		memcpy(fakeMsg->m_pData, data, size);
		fakeMsg->m_cbSize = size;
		fakeMsg->m_nChannel = channel;
		fakeMsg->m_identityPeer.SetSteamID(selfID);

		// Push into loopback queue
		// std::lock_guard<std::mutex> lock(g_loopbackMutex);
		// g_loopbackQueue.push(fakeMsg);
		if (role == ROLE_CLIENT)
		{
			std::lock_guard<std::mutex> lock(g_clientToServerMutex);
			g_clientToServerQueue.push(fakeMsg);
		}
		else if (role == ROLE_SERVER)
		{
			std::lock_guard<std::mutex> lock(g_serverToClientMutex);
			g_serverToClientQueue.push(fakeMsg);
		}
		else
		{
			// No role presets are set client -> server
			std::lock_guard<std::mutex> lock(g_clientToServerMutex);
			g_clientToServerQueue.push(fakeMsg);
		}

		return true;
	}

	SteamNetConnectionInfo_t info;
	if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info))
	{
		CSteamID remoteSteamId = info.m_identityRemote.GetSteamID();
		LOG->Info("Sending to SteamID: %llu\n", remoteSteamId.ConvertToUint64());
	}
	else
	{
		LOG->Info("Failed to get connection info for conn: %d\n", conn);
	}
	// return net->SendMessageToUser(id, data, size, sendType, channel);
	EResult result = SteamNetworkingSockets()->SendMessageToConnection(
		conn, data, size, sendType, nullptr);

	if (result == k_EResultOK)
	{
		return true;
	}
	else
	{
		// std::cerr << "[Error] SendMessageToConnection failed with code: " << result << "\n";
		LOG->Warn("[Error] SendMessageToConnection failed with code: %d",result);
		return false;
	}

	// const char* ping = "PING";
	// size_t len = strlen(ping);

	// SteamNetworkingMessage_t* msg = SteamNetworkingUtils()->AllocateMessage(len);
	// memcpy(msg->m_pData, ping, len);
	// msg->m_cbSize = len;
	// msg->m_conn = conn;
	// msg->m_nChannel = channel;
	// msg->m_nFlags = k_nSteamNetworkingSend_Unreliable;

	// SteamNetworkingSockets()->SendMessages(1, &msg, nullptr);
}

int ReceiveMessageWithLoopbackSupport(RoleType role,
									  const SteamNetworkingIdentity &id,
									  int channel,
									  HSteamNetConnection conn,
									  SteamNetworkingMessage_t **ppOutMessage)
{
	CSteamID selfID = SteamUser()->GetSteamID();
	if (id.GetSteamID() == selfID)
	{
		std::queue<SteamNetworkingMessage_t *> *targetQueue = nullptr;
		std::mutex *targetMutex = nullptr;

		// Select queue according to role
		if (role == ROLE_SERVER)
		{
			targetQueue = &g_clientToServerQueue;
			targetMutex = &g_clientToServerMutex;
		}
		else if (role == ROLE_CLIENT)
		{
			targetQueue = &g_serverToClientQueue;
			targetMutex = &g_serverToClientMutex;
		}

		if (targetQueue && targetMutex)
		{
			std::lock_guard<std::mutex> lock(*targetMutex);
			if (!targetQueue->empty())
			{
				SteamNetworkingMessage_t *msg = targetQueue->front();
				if (msg->m_nChannel == channel)
				{
					targetQueue->pop();
					*ppOutMessage = msg;
					return 1;
				}
			}
		}
	}

	// // If it's not loopback, get it from Steam
	// ISteamNetworkingMessages *net = SteamNetworkingMessages();
	// if (!net)
	// 	return 0;
	
	// return net->ReceiveMessagesOnChannel(channel, ppOutMessage, 1);
	return SteamNetworkingSockets()->ReceiveMessagesOnConnection(conn, ppOutMessage, 1);
}

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
	InitializeSteamNetworking();
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
	if (m_lobbyID.IsValid())
	{
		ISteamMatchmaking* matchmaking = SteamMatchmaking();
		if (matchmaking != nullptr)
		{
			matchmaking->LeaveLobby(m_lobbyID);
			m_lobbyID.Clear();
			m_LobbyJoined = false;
			LOG->Info("Left Steam Lobby during close().");
		}
		else
		{
			LOG->Warn("SteamMatchmaking() returned null in EzSockets::close()");
		}
	}
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
	if (m_useSteamNetworking)
	{
		bool hasData = false;

		// Loop and receive multiple messages at once
		SteamNetworkingIdentity id;
		id.SetSteamID(m_hostSteamID);
		SteamNetworkingMessage_t* msg = nullptr;
		// int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, &msg, 1);
		int count = ReceiveMessageWithLoopbackSupport(m_roleType, id, m_conn, 0, &msg);
		if (count <= 0 || !msg) return false;

		// Append to input buffer
		inBuffer.append((const char*)msg->m_pData, msg->m_cbSize);
		msg->Release();
		hasData = true;

		// Also handle loopback to self
		return hasData || !inBuffer.empty();
	}
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1,scks,NULL,NULL,times) > 0;
}

bool EzSockets::IsError()
{
	if (m_useSteamNetworking)
	{
		// If Steam is not initialized or lobby not joined, treat as error
		// if (!m_LobbyJoined || !m_lobbyID.IsValid())
		// 	return true;
		return false;
	}
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
	if (m_useSteamNetworking)
	{
		// Steam networking messages are non-blocking, always writable
		return true;
	}
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1, NULL, scks, NULL, times) > 0;
}

void EzSockets::update()
{
	if (m_useSteamNetworking)
	{
		// No need to receive Steam messages here, handled by CanRead()

		// Steam mode still supports outBuffer for compatibility
		if (!outBuffer.empty())
		{
			SendData(outBuffer.c_str(), static_cast<unsigned int>(outBuffer.length()));
			outBuffer.clear();
		}
		return;
	}

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
	if (m_useSteamNetworking && m_hostSteamID.IsValid())
	{
		// Always assemble [header][payload] if outBuffer has pending header
		std::vector<char> fullPacket;

		if (outBuffer.length() >= 4)
		{
			fullPacket.insert(fullPacket.end(), outBuffer.begin(), outBuffer.begin() + 4);
			outBuffer = outBuffer.substr(4); // remove header
		}

		fullPacket.insert(fullPacket.end(), data, data + bytes);

		// Loopback to self
		// if (m_hostSteamID == SteamUser()->GetSteamID())
		// {
		// 	inBuffer.append(fullPacket.data(), fullPacket.size());
		// 	return;
		// }

		// Send full packet to target user
		SteamNetworkingIdentity id;
		id.SetSteamID(m_hostSteamID);

		// bool ok = SteamNetworkingMessages()->SendMessageToUser(
		// 	id,
		// 	fullPacket.data(),
		// 	static_cast<uint32>(fullPacket.size()),
		// 	k_nSteamNetworkingSend_Reliable,
		// 	0
		// );
		bool ok = SendMessageWithLoopbackSupport(
			m_roleType,
			id,
			fullPacket.data(),
			static_cast<uint32>(fullPacket.size()),
			m_conn,
			k_nSteamNetworkingSend_Reliable,
			0
		);

		if (!ok)
			LOG->Warn("SendMessageToUser failed in SendData");

		return;
	}
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
	if (m_useSteamNetworking)
	{
		// Steam messages are already pushed to inBuffer via update()
		// No need to manually receive here
	}
	else
	{
		if (blocking)
			while ((inBuffer.length()<bytes) && !IsError())
				pUpdateRead();
		else
			while (CanRead() && !IsError())
				if (pUpdateRead()<1)
					break;
	}

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
	if (m_useSteamNetworking)
	{
		// Steam data is already pushed to inBuffer via update()
		// No need to manually receive anything here
		CanRead();
	}
	else
	{
		// Legacy socket mode: pull new data if available
		if (CanRead())
			pUpdateRead();
	}
	
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
	if (m_useSteamNetworking)
	{
		// Steam: assume data already pushed by update()
	}
	else if (blocking)
	{
		while (t == -1 && !IsError())
		{
			pUpdateRead();
			t = inBuffer.find(delim, 0);
		}
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
	if (m_useSteamNetworking)
	{
		// Steam mode already fills inBuffer in update(), nothing to do
		return 0;
	}
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
	if (m_useSteamNetworking)
	{
		// Steam write should be done via SendData(), this function is unused
		// LOG->Warn("pWriteData() called in Steam mode, should not happen");
		return 0;
	}
	int bytes = pWriteData(outBuffer.c_str(), outBuffer.length());
	
	if (bytes > 0)
		outBuffer = outBuffer.substr(bytes);
	else if (bytes < 0)
		state = skERROR;
	return bytes;
}


int EzSockets::pReadData(char* data)
{
	if (m_useSteamNetworking)
	{
		// Steam mode: read is handled via update(), return 0
		return 0;
	}
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

CString EzSockets::getIp()
{
	struct sockaddr_in name;
	socklen_t namelen = sizeof(name);
	getsockname(sock, (struct sockaddr *)&name, &namelen);

	char* str = inet_ntoa(name.sin_addr);
	CString cstr = str;
	return cstr;	
}

static void OnSteamNetConnectionStatusChangedForwarder(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	std::lock_guard<std::mutex> lock(g_connMapMutex);

	// 如果這個連線尚未登記，就嘗試幫忙找對應 EzSockets
	auto it = g_connToInstanceMap.find(pInfo->m_hConn);
	if (it != g_connToInstanceMap.end()) {
		it->second->OnSteamNetConnectionStatusChanged(pInfo);
	}
	else {
		// 新連線進來但尚未註冊，這裡你可以手動指定 server 實例來接管，例如：
		if (g_serverEzSocketsInstance) {
			g_connToInstanceMap[pInfo->m_hConn] = g_serverEzSocketsInstance;
			g_serverEzSocketsInstance->OnSteamNetConnectionStatusChanged(pInfo);
		}
	}
}

const char* GetStateName(ESteamNetworkingConnectionState state) {
	switch (state) {
	case k_ESteamNetworkingConnectionState_None: return "None";
	case k_ESteamNetworkingConnectionState_Connecting: return "Connecting";
	case k_ESteamNetworkingConnectionState_FindingRoute: return "FindingRoute";
	case k_ESteamNetworkingConnectionState_Connected: return "Connected";
	case k_ESteamNetworkingConnectionState_ClosedByPeer: return "ClosedByPeer";
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "ProblemDetectedLocally";
	case k_ESteamNetworkingConnectionState_FinWait: return "FinWait";
	case k_ESteamNetworkingConnectionState_Linger: return "Linger";
	case k_ESteamNetworkingConnectionState_Dead: return "Dead";
	default: return "Unknown";
	}
}

void EzSockets::InitStatusChanged()
{
	// if (!m_isInitialized)
	// {
	// 	g_ezSocketsInstance = this;
	// 	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChangedForwarder);
	// 	m_isInitialized = true;
	// }
}

void EzSockets::Reset()
{
	m_lobbyCreated = false;
	m_LobbyJoined = false;
	m_lobbySuccess = false;
	m_lobbyListReturned = false;
	m_callbacksRegistered = true;
	m_updated = false;
	m_listenSock = k_HSteamListenSocket_Invalid;
	m_conn = k_HSteamNetConnection_Invalid;
	m_conns.clear();
}

// Initialize Steam network functions
bool EzSockets::InitializeSteamNetworking()
{
	Reset();
	m_useSteamNetworking = false;
	// Check if the Steam API is available
	if (!SteamAPI_Init())
	{
		LOG->Warn("Steam API not initialized. Falling back to standard sockets.");
		return false;
	}else
	{
		g_steamReady = true;
	}

	SteamNetworkingUtils()->InitRelayNetworkAccess();
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChangedForwarder);
	m_useSteamNetworking = true;

	SetupSteamCallbacks();
	LOG->Info("Steam networking initialized successfully.");
	return true;
}

void  EzSockets::SetSelfId(CSteamID id)
{ 
	state = skCONNECTED;
	m_selfSteamID = id;
	m_roleType = ROLE_SERVER;
}

void EzSockets::SetupSteamCallbacks()
{
	if (!m_callbacksRegistered) return;
	m_callbacksRegistered = true;
	m_LobbyCreatedCallback.Register(this, &EzSockets::OnLobbyCreated);
	m_LobbyMatchCallback.Register(this, &EzSockets::OnLobbyMatchList);
	m_LobbyEnterCallback.Register(this, &EzSockets::OnLobbyEnter);
	m_LobbyChatUpdateCallback.Register(this, &EzSockets::OnLobbyChatUpdate);
}

void EzSockets::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
	const auto &info = pInfo->m_info;

    LOG->Info(">>> OnSteamNetConnectionStatusChanged triggered!");
    LOG->Info("  Conn: %d", pInfo->m_hConn);
    LOG->Info("  New state: (%d) %s", (int)info.m_eState, GetStateName(info.m_eState));
    LOG->Info("  Reason: %d - %s", info.m_eEndReason, info.m_szEndDebug);

    switch (info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        if (m_roleType == ROLE_SERVER) {
            if (SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn) == k_EResultOK)
            {
                m_conns.push_back(pInfo->m_hConn);
                m_updated = true;
                LOG->Info("[Server] First client accepted.");
            }
            else
            {
                LOG->Warn("[Server] Failed to accept first client.");
            }
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        if (m_roleType == ROLE_CLIENT) {
            m_connected = true;
            m_conn = pInfo->m_hConn;
        }
        LOG->Info("[Server] Client fully connected.");
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        LOG->Info("[Server] Connection closed.");
        break;
    }

    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        std::lock_guard<std::mutex> lock(g_connMapMutex);
        g_connToInstanceMap.erase(pInfo->m_hConn);
    }
}

bool EzSockets::create(CString roomCode)
{
	Reset();
	const int timeoutMs = 5000;
	if (!m_useSteamNetworking && !InitializeSteamNetworking())
		return false;
	InitStatusChanged();
	g_serverEzSocketsInstance = this;

	// Storage room number (optional)
	m_roomCode = std::string(roomCode);

	m_lobbyCreated = false;
	m_lobbySuccess = false;
	m_lobbyFound = false;
	m_roleType = ROLE_SERVER;
	// Create lobby
	SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 4);

	// Wait for callback to return or timeout
	int waited = 0;
	while (!m_lobbyCreated && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks();
		Sleep(100);
		waited += 100;
	}

	m_listenSock = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);
	if (m_listenSock == k_HSteamListenSocket_Invalid)
	{
		std::cerr << "[Server] Failed to create listen socket.\n";
		m_lobbySuccess = false;
		return m_lobbySuccess;
	}
	state = skCONNECTED;
	return m_lobbySuccess;
}

bool EzSockets::connect(const string& roomCode)
{
	Reset();
	if (!m_useSteamNetworking) 
		InitializeSteamNetworking();
	InitStatusChanged();
	m_roomCodeTmp = roomCode;
	m_lobbyFound = false;
	m_roleType = ROLE_CLIENT;
	m_connected = false;
	// If you are already in the correct room, return true directly
	if (m_lobbyID.IsValid())
	{
		std::string currentCode = SteamMatchmaking()->GetLobbyData(m_lobbyID, "room_code");
		if (currentCode == m_roomCodeTmp)
		{
			LOG->Info("Already in target lobby with room code: %s", m_roomCodeTmp.c_str());
			state = skCONNECTED;
			return true;
		}
	}
	SteamMatchmaking()->AddRequestLobbyListStringFilter("room_code", m_roomCodeTmp.c_str(), k_ELobbyComparisonEqual);
	SteamMatchmaking()->RequestLobbyList();

	// Wait for OnLobbyMatchList callback to return
	const int timeoutMs = 5000;
	int waited = 0;
	while (!m_lobbyListReturned && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks();
		Sleep(100);
		waited += 100;
	}
	if (!m_lobbyListReturned || !m_lobbyFound) return false;
	// If a lobby is found and successfully joined, OnLobbyMatchList will trigger JoinLobby
	// Now continue to wait for OnLobbyEnter to successfully enter the room
	waited = 0;
	while (!m_LobbyJoined && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks();
		Sleep(100);
		waited += 100;
	}

	if(m_LobbyJoined)
	{
		const int waitTimeoutMs = 15000;
		SteamNetworkingIdentity id;
		id.SetSteamID(m_hostSteamID);
		HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(id, 0, 0, nullptr);
		if (conn != k_HSteamNetConnection_Invalid) {
			{
				std::lock_guard<std::mutex> lock(g_connMapMutex);
				g_connToInstanceMap[conn] = this;  // 註冊本 EzSockets 實例
			}
			// m_conns.push_back(conn);  // 可選：紀錄起來方便管理
		}
		int waitMs = 0;
		if (!m_useSteamNetworking) return false;
		while (m_conn == k_HSteamNetConnection_Invalid &&
			   waitMs < waitTimeoutMs)
		{
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			waitMs += 100;
		}
	}
	
	if(m_LobbyJoined && m_connected) state = skCONNECTED;
	return m_LobbyJoined && m_connected;
}

void EzSockets::OnLobbyCreated(LobbyCreated_t* pCallback)
{
	m_lobbyCreated = true;

	if (pCallback->m_eResult == k_EResultOK)
	{
		m_lobbySuccess = true;
		m_lobbyID = pCallback->m_ulSteamIDLobby;

		// Set custom lobby data for room code
		// SteamMatchmaking()->SetLobbyData(m_lobbyID, "room_code", m_roomCode.c_str());

		LOG->Info("Lobby created successfully: %llu", m_lobbyID.ConvertToUint64());
	}
	else
	{
		m_lobbySuccess = false;
		LOG->Warn("Failed to create lobby. Result: %d", pCallback->m_eResult);
	}
}

void EzSockets::OnLobbyMatchList(LobbyMatchList_t* pCallback)
{
	m_lobbyListReturned = true;
	int matches = pCallback->m_nLobbiesMatching;
	for (int i = 0; i < matches; ++i) {
		CSteamID lobbyID = SteamMatchmaking()->GetLobbyByIndex(i);
		std::string lobbyCode = SteamMatchmaking()->GetLobbyData(lobbyID, "room_code");
		if (lobbyCode == m_roomCodeTmp) {
			m_lobbyFound = true;
			m_roomCode = lobbyCode;
			// If you are already in the same lobby, don't join
			if (m_lobbyID.IsValid() && m_lobbyID == lobbyID)
				return;
			SteamMatchmaking()->JoinLobby(lobbyID);
			return;
		}
	}
}

std::wstring Utf8ToWide(const std::string& str)
{
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	return converter.from_bytes(str);
}

void EzSockets::OnLobbyEnter(LobbyEnter_t* pCallback)
{
	m_lobbyID = pCallback->m_ulSteamIDLobby;
	m_LobbyJoined = true;

	if (SteamMatchmaking()->GetLobbyOwner(m_lobbyID) == SteamUser()->GetSteamID())
	{
		SteamMatchmaking()->SetLobbyData(m_lobbyID, "room_code", m_roomCode.c_str());
	}
	CSteamID self = SteamUser()->GetSteamID();
	std::string name = SteamFriends()->GetFriendPersonaName(self);
	std::wcout << L"[JOIN] You (" << Utf8ToWide(name) << L") joined lobby: " << m_lobbyID.ConvertToUint64() << std::endl;
	CSteamID hostID = SteamMatchmaking()->GetLobbyOwner(m_lobbyID);
	m_hostSteamID = hostID;  // All clients send data to the host
	m_selfSteamID = self;
	m_updated = true;
	// UpdateLobbyMembers();
}

void EzSockets::OnLobbyChatUpdate(LobbyChatUpdate_t* pCallback)
{
	m_updated = true;
	// UpdateLobbyMembers();
}

bool SteamReady()
{
	return g_steamReady;
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