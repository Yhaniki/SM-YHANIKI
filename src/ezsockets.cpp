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

static void ClearSteamLoopbackQueues()
{
	{
		std::lock_guard<std::mutex> lock(g_clientToServerMutex);
		while (!g_clientToServerQueue.empty())
		{
			g_clientToServerQueue.front()->Release();
			g_clientToServerQueue.pop();
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_serverToClientMutex);
		while (!g_serverToClientQueue.empty())
		{
			g_serverToClientQueue.front()->Release();
			g_serverToClientQueue.pop();
		}
	}
}

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

	// [FIX] Steam reliable send buffer 預設只有 512KB。如果上層連續送大量資料
	//       (例如分享歌 thread 一次塞幾十顆 60KB chunk)，會撞到 k_EResultLimitExceeded。
	//       原本只 log warning 就 return false → 資料直接被丟掉，這就是 receiver 大檔
	//       中間 chunk 大量遺失、檔變 sparse 的根因。
	//       修法：撞到 LimitExceeded 就 sleep 一下重試，最多等 5 秒。其它錯誤照舊 return false。
	const int kMaxRetries = 500;       // 500 * 10ms = 最多等 5 秒讓 buffer 騰空
	const int kRetrySleepMs = 10;
	for (int retry = 0; retry < kMaxRetries; ++retry)
	{
		EResult result = SteamNetworkingSockets()->SendMessageToConnection(
			conn, data, size, sendType, nullptr);

		if (result == k_EResultOK)
		{
			if (retry > 0)
				LOG->Info("[NETDBG] SendMessageToConnection OK after %d retries", retry);
			return true;
		}

		if (result != k_EResultLimitExceeded)
		{
			LOG->Warn("[Error] SendMessageToConnection failed code=%d (not LimitExceeded, give up)", result);
			return false;
		}

		// reliable buffer 滿了，sleep 一下讓 Steam 把 unacked 排出去再試
		if (retry == 0)
			LOG->Info("[NETDBG] SendMessageToConnection LimitExceeded, retrying...");

		Sleep(kRetrySleepMs);
	}
	LOG->Warn("[Error] SendMessageToConnection still LimitExceeded after 5s (data lost)");
	return false;

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
	LOG->Info("[NETDBG] EZ::close#1 this=%p useSteam=%d role=%d m_conn=%u m_conns.size=%u listenSock=%llu lobbyID=%llu",
		(void*)this, (int)m_useSteamNetworking, (int)m_roleType,
		(unsigned)m_conn, (unsigned)m_conns.size(),
		(unsigned long long)m_listenSock,
		m_lobbyID.ConvertToUint64());

	if (m_useSteamNetworking)
	{
		ISteamNetworkingSockets* pSockets = SteamNetworkingSockets();
		if (pSockets != nullptr)
		{
			for (HSteamNetConnection conn : m_conns)
			{
				if (conn != k_HSteamNetConnection_Invalid)
				{
					LOG->Info("[NETDBG] EZ::close#2 closing m_conns conn=%u", (unsigned)conn);
					pSockets->CloseConnection(conn, k_ESteamNetConnectionEnd_App_Generic, "Room closed", false);
					std::lock_guard<std::mutex> lock(g_connMapMutex);
					g_connToInstanceMap.erase(conn);
				}
			}
			m_conns.clear();

			if (m_conn != k_HSteamNetConnection_Invalid)
			{
				LOG->Info("[NETDBG] EZ::close#3 closing m_conn=%u", (unsigned)m_conn);
				pSockets->CloseConnection(m_conn, k_ESteamNetConnectionEnd_App_Generic, "Room closed", false);
				std::lock_guard<std::mutex> lock(g_connMapMutex);
				g_connToInstanceMap.erase(m_conn);
				m_conn = k_HSteamNetConnection_Invalid;
			}

			if (m_listenSock != k_HSteamListenSocket_Invalid)
			{
				LOG->Info("[NETDBG] EZ::close#4 closing listen socket=%llu", (unsigned long long)m_listenSock);
				pSockets->CloseListenSocket(m_listenSock);
				m_listenSock = k_HSteamListenSocket_Invalid;
			}

			SteamAPI_RunCallbacks();
			pSockets->RunCallbacks();
		}

		const bool wasServerInstance = (g_serverEzSocketsInstance == this);
		if (wasServerInstance)
		{
			LOG->Info("[NETDBG] EZ::close#5 clearing g_serverEzSocketsInstance");
			g_serverEzSocketsInstance = nullptr;
		}

		// [NETDBG] BUG FIX：只有真正的 server EzSockets 物件（持有 listen socket、g_serverEzSocketsInstance）
		// 才 clear loopback queue。否則 GameClient::clientSocket 被 delete 時會把 host 自己 self-host 的
		// loopback queue 全部清掉，造成 self-host 通訊中斷。
		if (wasServerInstance)
		{
			LOG->Info("[NETDBG] EZ::close#6 clearing loopback queues (real server)");
			ClearSteamLoopbackQueues();
		}

		if (m_lobbyID.IsValid())
		{
			ISteamMatchmaking* matchmaking = SteamMatchmaking();
			if (matchmaking != nullptr)
			{
				LOG->Info("[NETDBG] EZ::close#7 LeaveLobby=%llu", m_lobbyID.ConvertToUint64());
				matchmaking->LeaveLobby(m_lobbyID);
				m_lobbyID.Clear();
				m_LobbyJoined = false;
			}
			else
			{
				LOG->Warn("[NETDBG] EZ::close#8 SteamMatchmaking() returned null");
			}
		}

		m_connected = false;
		m_lobbyCreated = false;
		m_lobbySuccess = false;
		m_lobbyListReturned = false;
		m_lobbyFound = false;
		m_hostSteamID.Clear();
		m_selfSteamID.Clear();
		m_roleType = ROLE_UNKNOWN;
		m_roomCode.clear();
		m_roomCodeTmp.clear();
		m_lobbySearchAwaitingSerial = 0;
	}

	state = skDISCONNECTED;
	inBuffer = "";
	outBuffer = "";

#if defined(WIN32) // The close socket command is different in Windows
	if (sock > 0)
		::closesocket(sock);
#else
	if (sock > 0)
		::close(sock);
#endif
	sock = -1;
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
		// [NETDBG] BUG FIX：原本呼叫順序錯誤 (m_roleType, id, m_conn, 0, &msg)
		// signature 是 (role, id, channel, conn, ppOutMessage)
		// 等於把 m_conn 當成 channel，把 0 (invalid handle) 當成 conn
		// self-host 時 m_conn==0 巧合下走 loopback queue 看起來正常
		// 但 remote client (m_conn=3608401646) 會走 ReceiveMessagesOnConnection(conn=0)
		// → invalid handle 永遠收不到資料！這就是 server 端收不到 remote client 訊息的真正原因
		int count = ReceiveMessageWithLoopbackSupport(m_roleType, id, 0, m_conn, &msg);
		if (count <= 0 || !msg) return false;

		// [FPS] 原本是 LOG->Info，每收一個 Steam 訊息就會 fsync 一次 log.txt，
		// 大檔分享 (~85k chunks) 時直接把 FPS 從 100+ 砸到個位數。改用 Trace
		// (一樣寫 log.txt 但不 flush)，保留診斷價值又不卡 frame。
		LOG->Trace("[NETDBG] EZ::CanRead#1 got %d bytes role=%d host=%llu conn=%u",
			(int)msg->m_cbSize, (int)m_roleType,
			m_hostSteamID.ConvertToUint64(), (unsigned)m_conn);

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
		// [FPS] 同 CanRead#1 — 每送一個 chunk 就 fsync 會殺 FPS。降為 Trace。
		LOG->Trace("[NETDBG] EZ::SendData#1 steam mode role=%d host=%llu self=%llu bytes=%u conn=%u",
			(int)m_roleType,
			m_hostSteamID.ConvertToUint64(),
			m_selfSteamID.ConvertToUint64(),
			bytes,
			(unsigned)m_conn);

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
		// 之前嘗試加 k_nSteamNetworkingSend_NoNagle 想去掉 ~5ms 批次延遲，
		// 但實測 client 在收到一定量後會閃退 (可能與 receiver 主執行緒在 ProcessInput 內
		// 高速 drain 大量訊息使 render 來不及 → Windows TDR 有關)。
		// 退回單純的 Reliable 模式，行為與使用者已驗證能完整傳完的版本一致。
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
			LOG->Warn("[NETDBG] EZ::SendData#2 SendMessageToUser failed");
		else
			LOG->Trace("[NETDBG] EZ::SendData#3 sent OK"); // [FPS] 改 Trace 不 flush

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

	// [FIX] 防 buffer overrun：原本 (tBuff.substr(0, max)) 的回傳值沒被接住，
	// 等於沒截斷；若 size > max，memcpy 會直接寫爆 caller 的 data buffer。
	// 而且 size 是從遠端 4 bytes 讀來的，若 outBuffer 流亂掉或被雜訊汙染，
	// size 可能變成天文數字 (例如 0xFFFFFFFF)，size+4 還會 wrap 成很小，
	// 讓底下的 wait 直接通過，後面 substr 把整個 inBuffer 都讀去 memcpy。
	// 一旦 size > max 我們無法把它放進 caller 的 buffer，
	// 同時這也代表 stream 已不可信任 -- 整個 inBuffer 清掉強制重同步。
	if (size > max)
	{
		inBuffer.clear();
		return -1;
	}
	
	if (blocking)
		while (inBuffer.length()<(size+4) && !IsError())
			pUpdateRead();
	else
		if (inBuffer.length()<(size+4) || inBuffer.length()<=4)
			return -1;
	
	if (IsError())
		return -1; 
	//What if we get disconnected while waiting for data?

	// size <= max 已驗過，inBuffer 至少有 size+4 bytes，可以直接 memcpy
	memcpy(data, inBuffer.data() + 4, size);
	return (int)size;
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
	LOG->Info("[NETDBG] EZ::Forwarder#1 enter conn=%u state=%d",
		(unsigned)pInfo->m_hConn, (int)pInfo->m_info.m_eState);

	// [NETDBG] BUG FIX：原本在持有 g_connMapMutex 的情況下直接 dispatch 到
	// OnSteamNetConnectionStatusChanged，而後者內部又會嘗試 lock 同一個 mutex
	// → Windows std::mutex 不是 recursive，重複 lock 會 throw std::system_error
	// "device or resource busy"（EBUSY），這就是兩邊閃退的真正原因！
	// 修法：先在 lock 內查/補 map，記下要 dispatch 的物件，釋放 lock 後再 dispatch。
	EzSockets* targetInstance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_connMapMutex);

		auto it = g_connToInstanceMap.find(pInfo->m_hConn);
		if (it != g_connToInstanceMap.end()) {
			LOG->Info("[NETDBG] EZ::Forwarder#2 conn=%u mapped to instance=%p, dispatching",
				(unsigned)pInfo->m_hConn, (void*)it->second);
			if (it->second == nullptr)
			{
				LOG->Warn("[NETDBG] EZ::Forwarder#3 mapped instance is NULL! Skipping dispatch");
				return;
			}
			targetInstance = it->second;
		}
		else {
			LOG->Info("[NETDBG] EZ::Forwarder#4 conn=%u not mapped, server instance=%p",
				(unsigned)pInfo->m_hConn, (void*)g_serverEzSocketsInstance);
			if (g_serverEzSocketsInstance) {
				g_connToInstanceMap[pInfo->m_hConn] = g_serverEzSocketsInstance;
				targetInstance = g_serverEzSocketsInstance;
			}
			else
			{
				LOG->Warn("[NETDBG] EZ::Forwarder#5 no server instance to receive conn=%u",
					(unsigned)pInfo->m_hConn);
			}
		}
	}

	if (targetInstance)
	{
		targetInstance->OnSteamNetConnectionStatusChanged(pInfo);
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
	m_lobbySearchSerial = 0;
	m_lobbySearchAwaitingSerial = 0;
	m_targetLobbyID.Clear();
}

static std::string TrimRoomCode(const std::string& code)
{
	const char* ws = " \t\r\n";
	const size_t start = code.find_first_not_of(ws);
	if (start == std::string::npos)
		return "";
	const size_t end = code.find_last_not_of(ws);
	return code.substr(start, end - start + 1);
}

bool EzSockets::establishP2PConnection()
{
	LOG->Info("[NETDBG] EZ::P2P#1 begin host=%llu valid=%d",
		m_hostSteamID.ConvertToUint64(), (int)m_hostSteamID.IsValid());
	if (!m_hostSteamID.IsValid())
	{
		LOG->Warn("[NETDBG] EZ::P2P#2 hostSteamID invalid, abort");
		return false;
	}

	m_connected = false;
	m_conn = k_HSteamNetConnection_Invalid;

	SteamNetworkingIdentity id;
	id.SetSteamID(m_hostSteamID);
	HSteamNetConnection conn = SteamNetworkingSockets()->ConnectP2P(id, 0, 0, nullptr);
	LOG->Info("[NETDBG] EZ::P2P#3 ConnectP2P returned conn=%u", (unsigned)conn);
	if (conn != k_HSteamNetConnection_Invalid)
	{
		std::lock_guard<std::mutex> lock(g_connMapMutex);
		g_connToInstanceMap[conn] = this;
		LOG->Info("[NETDBG] EZ::P2P#4 mapped conn->this");
	}

	const int waitTimeoutMs = 15000;
	int waitMs = 0;
	while (m_conn == k_HSteamNetConnection_Invalid && waitMs < waitTimeoutMs)
	{
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		waitMs += 100;
		if ((waitMs % 2000) == 0)
			LOG->Info("[NETDBG] EZ::P2P#5 waiting connection... %dms (LobbyJoined=%d connected=%d)",
				waitMs, (int)m_LobbyJoined, (int)m_connected);
	}

	LOG->Info("[NETDBG] EZ::P2P#6 wait done LobbyJoined=%d connected=%d m_conn=%u (waited %dms)",
		(int)m_LobbyJoined, (int)m_connected, (unsigned)m_conn, waitMs);

	if (m_LobbyJoined && m_connected)
		state = skCONNECTED;
	return m_LobbyJoined && m_connected;
}

void EzSockets::tryMarkAlreadyInLobby(CSteamID lobbyID)
{
	if (!lobbyID.IsValid() || m_LobbyJoined)
		return;

	const int count = SteamMatchmaking()->GetNumLobbyMembers(lobbyID);
	const CSteamID self = SteamUser()->GetSteamID();
	for (int i = 0; i < count; ++i)
	{
		if (SteamMatchmaking()->GetLobbyMemberByIndex(lobbyID, i) == self)
		{
			m_lobbyID = lobbyID;
			m_LobbyJoined = true;
			m_hostSteamID = SteamMatchmaking()->GetLobbyOwner(lobbyID);
			m_selfSteamID = self;
			if (m_roomCode.empty())
				m_roomCode = SteamMatchmaking()->GetLobbyData(lobbyID, "room_code");
			LOG->Info("Already in lobby, skipping JoinLobby.");
			return;
		}
	}
}

bool EzSockets::attachToLobby(CSteamID lobbyID)
{
	LOG->Info("[NETDBG] EZ::attachToLobby#1 lobbyID=%llu valid=%d",
		lobbyID.ConvertToUint64(), (int)lobbyID.IsValid());
	if (!lobbyID.IsValid())
		return false;
	if (!m_useSteamNetworking && !InitializeSteamNetworking())
	{
		LOG->Warn("[NETDBG] EZ::attachToLobby#2 Steam not ready");
		return false;
	}

	InitStatusChanged();
	ClearSteamLoopbackQueues();

	m_roleType = ROLE_CLIENT;
	m_connected = false;
	m_conn = k_HSteamNetConnection_Invalid;
	m_lobbyID = lobbyID;
	m_LobbyJoined = true;
	m_hostSteamID = SteamMatchmaking()->GetLobbyOwner(lobbyID);
	m_selfSteamID = SteamUser()->GetSteamID();
	m_roomCode = SteamMatchmaking()->GetLobbyData(lobbyID, "room_code");
	m_roomCodeTmp = m_roomCode;
	m_targetLobbyID = lobbyID;

	LOG->Info("[NETDBG] EZ::attachToLobby#3 host=%llu self=%llu room_code=%s",
		m_hostSteamID.ConvertToUint64(), m_selfSteamID.ConvertToUint64(), m_roomCode.c_str());

	// Self-host: data flows through loopback queues, no real Steam P2P needed.
	if (m_hostSteamID == m_selfSteamID)
	{
		LOG->Info("[NETDBG] EZ::attachToLobby#4 self-host loopback path");
		m_connected = true;
		state = skCONNECTED;
		// Drive a few callback ticks so the server side sees the lobby member
		for (int i = 0; i < 4; ++i)
		{
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
			Sleep(25);
		}
		LOG->Info("[NETDBG] EZ::attachToLobby#5 self-host done");
		return true;
	}

	LOG->Info("[NETDBG] EZ::attachToLobby#6 remote host -> establishP2PConnection");
	bool ok = establishP2PConnection();
	LOG->Info("[NETDBG] EZ::attachToLobby#7 P2P result=%d", (int)ok);
	return ok;
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

    LOG->Info("[NETDBG] EZ::ConnStatus#1 conn=%u role=%d newState=(%d)%s reason=%d debug=%s remoteID=%llu",
        (unsigned)pInfo->m_hConn,
        (int)m_roleType,
        (int)info.m_eState,
        GetStateName(info.m_eState),
        info.m_eEndReason,
        info.m_szEndDebug,
        (unsigned long long)info.m_identityRemote.GetSteamID().ConvertToUint64());

    switch (info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        if (m_roleType == ROLE_SERVER) {
            EResult ar = SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);
            LOG->Info("[NETDBG] EZ::ConnStatus#2 server AcceptConnection result=%d", (int)ar);
            if (ar == k_EResultOK)
            {
                m_conns.push_back(pInfo->m_hConn);
                m_updated = true;
                LOG->Info("[NETDBG] EZ::ConnStatus#3 server accepted, m_conns.size=%u",
                    (unsigned)m_conns.size());
            }
            else
            {
                LOG->Warn("[NETDBG] EZ::ConnStatus#4 server FAILED to accept");
            }
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
        if (m_roleType == ROLE_CLIENT) {
            m_connected = true;
            m_conn = pInfo->m_hConn;
            LOG->Info("[NETDBG] EZ::ConnStatus#5 client connected, m_conn=%u", (unsigned)m_conn);
        }
        else
        {
            LOG->Info("[NETDBG] EZ::ConnStatus#6 server side, peer connected conn=%u", (unsigned)pInfo->m_hConn);
        }
        break;

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        LOG->Info("[NETDBG] EZ::ConnStatus#7 connection closed/problem conn=%u", (unsigned)pInfo->m_hConn);
        break;
    }

    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
    {
        std::lock_guard<std::mutex> lock(g_connMapMutex);
        g_connToInstanceMap.erase(pInfo->m_hConn);
        LOG->Info("[NETDBG] EZ::ConnStatus#8 erased conn=%u from g_connToInstanceMap", (unsigned)pInfo->m_hConn);
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
	LOG->Info("[NETDBG] EZ::connect#1 begin roomCode='%s'", roomCode.c_str());
	Reset();
	if (!m_useSteamNetworking) 
		InitializeSteamNetworking();
	InitStatusChanged();
	m_roomCodeTmp = TrimRoomCode(roomCode);
	if (m_roomCodeTmp.empty())
	{
		LOG->Warn("[NETDBG] EZ::connect#2 empty room code");
		return false;
	}
	m_lobbyFound = false;
	m_roleType = ROLE_CLIENT;
	m_connected = false;
	m_lobbyListReturned = false;
	m_lobbySearchAwaitingSerial = 0;
	m_targetLobbyID.Clear();

	// Drain stale Steam lobby callbacks from a previous session
	for (int i = 0; i < 10; ++i)
	{
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		Sleep(25);
	}

	m_lobbySearchSerial++;
	m_lobbyListReturned = false;
	m_lobbyFound = false;
	m_lobbySearchAwaitingSerial = m_lobbySearchSerial;

	LOG->Info("Searching Steam lobby with room_code=%s", m_roomCodeTmp.c_str());
	SteamMatchmaking()->AddRequestLobbyListStringFilter("room_code", m_roomCodeTmp.c_str(), k_ELobbyComparisonEqual);
	SteamMatchmaking()->RequestLobbyList();

	const int timeoutMs = 10000;
	int waited = 0;
	while (!m_lobbyListReturned && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		Sleep(100);
		waited += 100;
	}
	m_lobbySearchAwaitingSerial = 0;

	if (!m_lobbyListReturned || !m_lobbyFound)
	{
		LOG->Warn("[NETDBG] EZ::connect#3 Lobby search failed (returned=%d found=%d)",
			m_lobbyListReturned ? 1 : 0, m_lobbyFound ? 1 : 0);
		return false;
	}
	LOG->Info("[NETDBG] EZ::connect#4 lobby found, waiting JoinLobby callback");

	waited = 0;
	while (!m_LobbyJoined && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
		Sleep(100);
		waited += 100;
	}

	if (!m_LobbyJoined && m_targetLobbyID.IsValid())
	{
		LOG->Info("[NETDBG] EZ::connect#5 tryMarkAlreadyInLobby");
		tryMarkAlreadyInLobby(m_targetLobbyID);
	}

	if (!m_LobbyJoined)
	{
		LOG->Warn("[NETDBG] EZ::connect#6 Failed to enter lobby after match.");
		return false;
	}
	LOG->Info("[NETDBG] EZ::connect#7 lobby joined, host=%llu",
		m_hostSteamID.ConvertToUint64());

	if (!establishP2PConnection())
	{
		LOG->Warn("[NETDBG] EZ::connect#8 Lobby joined but P2P connection failed.");
		return false;
	}
	LOG->Info("[NETDBG] EZ::connect#9 done OK");

	return true;
}

void EzSockets::OnLobbyCreated(LobbyCreated_t* pCallback)
{
	m_lobbyCreated = true;

	if (pCallback->m_eResult == k_EResultOK)
	{
		m_lobbySuccess = true;
		m_lobbyID = pCallback->m_ulSteamIDLobby;

		if (!m_roomCode.empty())
			SteamMatchmaking()->SetLobbyData(m_lobbyID, "room_code", m_roomCode.c_str());

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
	LOG->Info("[NETDBG] EZ::OnLobbyMatchList#1 role=%d roomCodeTmp='%s' matches=%u awaitingSerial=%u",
		(int)m_roleType, m_roomCodeTmp.c_str(),
		(unsigned)pCallback->m_nLobbiesMatching, (unsigned)m_lobbySearchAwaitingSerial);
	if (m_roleType != ROLE_CLIENT || m_roomCodeTmp.empty())
		return;
	if (m_lobbySearchAwaitingSerial == 0)
		return;

	m_lobbyListReturned = true;
	int matches = pCallback->m_nLobbiesMatching;
	for (int i = 0; i < matches; ++i) {
		CSteamID lobbyID = SteamMatchmaking()->GetLobbyByIndex(i);
		std::string lobbyCode = SteamMatchmaking()->GetLobbyData(lobbyID, "room_code");
		LOG->Info("[NETDBG] EZ::OnLobbyMatchList#2 candidate lobby=%llu code='%s'",
			lobbyID.ConvertToUint64(), lobbyCode.c_str());
		if (lobbyCode == m_roomCodeTmp) {
			m_lobbyFound = true;
			m_roomCode = lobbyCode;
			m_targetLobbyID = lobbyID;
			if (m_lobbyID.IsValid() && m_lobbyID == lobbyID)
			{
				LOG->Info("[NETDBG] EZ::OnLobbyMatchList#3 already in this lobby, tryMarkAlreadyInLobby");
				tryMarkAlreadyInLobby(lobbyID);
				return;
			}
			LOG->Info("[NETDBG] EZ::OnLobbyMatchList#4 calling JoinLobby");
			SteamMatchmaking()->JoinLobby(lobbyID);
			return;
		}
	}
	LOG->Info("[NETDBG] EZ::OnLobbyMatchList#5 no matching code");
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
	LOG->Info("[NETDBG] EZ::OnLobbyEnter#1 lobby=%llu response=%d",
		m_lobbyID.ConvertToUint64(), (int)pCallback->m_EChatRoomEnterResponse);

	if (m_roomCode.empty())
		m_roomCode = SteamMatchmaking()->GetLobbyData(m_lobbyID, "room_code");

	if (SteamMatchmaking()->GetLobbyOwner(m_lobbyID) == SteamUser()->GetSteamID())
	{
		if (!m_roomCode.empty())
			SteamMatchmaking()->SetLobbyData(m_lobbyID, "room_code", m_roomCode.c_str());
	}
	CSteamID self = SteamUser()->GetSteamID();
	std::string name = SteamFriends()->GetFriendPersonaName(self);
	std::wcout << L"[JOIN] You (" << Utf8ToWide(name) << L") joined lobby: " << m_lobbyID.ConvertToUint64() << std::endl;
	CSteamID hostID = SteamMatchmaking()->GetLobbyOwner(m_lobbyID);
	m_hostSteamID = hostID;  // All clients send data to the host
	m_selfSteamID = self;
	m_updated = true;
	LOG->Info("[NETDBG] EZ::OnLobbyEnter#2 host=%llu self=%llu role=%d",
		m_hostSteamID.ConvertToUint64(), m_selfSteamID.ConvertToUint64(), (int)m_roleType);
	// UpdateLobbyMembers();
}

void EzSockets::OnLobbyChatUpdate(LobbyChatUpdate_t* pCallback)
{
	LOG->Info("[NETDBG] EZ::OnLobbyChatUpdate#1 changedUser=%llu makingChange=%llu stateChange=%u",
		(unsigned long long)pCallback->m_ulSteamIDUserChanged,
		(unsigned long long)pCallback->m_ulSteamIDMakingChange,
		(unsigned)pCallback->m_rgfChatMemberStateChange);
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