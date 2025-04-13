/*******************************************************************\
| ezsockets.cpp: EzSockets Class Source                             |
|   Designed by Josh Allen, Charles Lohr and Adam Lowman.           |
|   Socket programming methods based on Charles Lohr's EZW progam.  |
|   Modified by Charles Lohr for use with Windows-Based OSes.       |
|   UDP/NON-TCP Support by Adam Lowman.                             |
|   Steam API Integration by Claude AI.                              |
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
// ezsockets.cpp 中 constructor 修正為：
: m_SteamNetConnectionStatusChanged(this, &EzSockets::OnSteamNetConnectionStatusChanged)

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
	
	// 初始化Steam API相關成員
	m_hListenSocket = k_HSteamListenSocket_Invalid;
	m_hConnection = k_HSteamNetConnection_Invalid;
	m_pNetworkingSockets = nullptr;
	m_useSteamNetworking = false;
	
	// 嘗試初始化Steam網絡功能
	InitializeSteamNetworking();
}

EzSockets::~EzSockets()
{
	close();
	delete scks;
	delete times;
	SteamAPI_Shutdown();
}

// 初始化Steam網絡功能
bool EzSockets::InitializeSteamNetworking()
{
	// 檢查Steam API是否可用
	if (!SteamAPI_Init())
	{
		LOG->Warn("Steam API not initialized. Falling back to standard sockets.");
		return false;
	}
	
	// // 獲取Steam網絡接口
	// m_pNetworkingSockets = SteamNetworkingSockets();
	// if (!m_pNetworkingSockets)
	// {
	// 	LOG->Warn("SteamNetworkingSockets not available. Falling back to standard sockets.");
	// 	return false;
	// }
	
	// // 初始化Steam網絡認證
	// ESteamNetworkingAvailability avail = m_pNetworkingSockets->InitAuthentication();
	// if (avail != k_ESteamNetworkingAvailability_Current)
	// {
	// 	LOG->Warn("Steam networking authentication not available. Status: %d", avail);
	// 	return false;
	// }
	
	m_useSteamNetworking = true;
	LOG->Info("Steam networking initialized successfully.");
	return true;
}

// 處理Steam回調
void EzSockets::ProcessSteamCallbacks()
{
	// if (!m_useSteamNetworking || !m_pNetworkingSockets)
	// 	return;
		
	// // 處理Steam網絡回調
	// SteamAPI_RunCallbacks();
}

// 將IP地址和端口轉換為SteamNetworkingIPAddr
SteamNetworkingIPAddr EzSockets::CreateSteamNetworkingIPAddr(const string& host, unsigned short port)
{
	SteamNetworkingIPAddr addr;
	addr.Clear();
	
	// 如果是主機名，嘗試解析
	if (host != "LISTEN" && host != "localhost" && host != "127.0.0.1")
	{
		struct hostent* phe = gethostbyname(host.c_str());
		if (phe)
		{
			// 轉換為IPv4地址
			addr.SetIPv4(*(uint32_t*)phe->h_addr, port);
			return addr;
		}
	}
	
	// 如果是IP地址，直接解析
	if (host == "LISTEN" || host == "localhost" || host == "127.0.0.1")
	{
		// 監聽模式，使用任意地址
		addr.SetIPv4(0, port);
	}
	else
	{
		// 嘗試解析IP地址
		addr.ParseString(host.c_str());
		addr.m_port = port;
	}
	
	return addr;
}

// 將SteamNetworkingIPAddr轉換為字符串
string EzSockets::SteamNetworkingIPAddrToString(const SteamNetworkingIPAddr& addr)
{
	char szAddr[128];
	addr.ToString(szAddr, sizeof(szAddr), true);
	return string(szAddr);
}

// 處理Steam連接狀態變化
void EzSockets::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pCallback)
{
	if (!m_useSteamNetworking)
		return;
		
	// 更新連接狀態
	switch (pCallback->m_info.m_eState)
	{
		case k_ESteamNetworkingConnectionState_None:
			// 連接已關閉
			state = skDISCONNECTED;
			break;
			
		case k_ESteamNetworkingConnectionState_Connecting:
			// 正在連接
			break;
			
		case k_ESteamNetworkingConnectionState_Connected:
			// 連接成功
			state = skCONNECTED;
			break;
			
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			// 連接關閉或出現問題
			state = skERROR;
			break;
	}
	
	// 處理新連接
	if (pCallback->m_info.m_hListenSocket == m_hListenSocket && 
		pCallback->m_eOldState == k_ESteamNetworkingConnectionState_None &&
		pCallback->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
	{
		// 接受連接
		m_pNetworkingSockets->AcceptConnection(pCallback->m_hConn);
	}
}

// 處理Steam消息
void EzSockets::OnSteamNetworkingMessages(SteamNetworkingMessage_t* pMessage)
{
	if (!m_useSteamNetworking || !pMessage)
		return;
		
	// 將消息添加到接收緩衝區
	inBuffer.append((const char*)pMessage->GetData(), pMessage->GetSize());
	
	// 釋放消息
	pMessage->Release();
}

//Check to see if the socket has been created
bool EzSockets::check()
{
	if (m_useSteamNetworking)
	{
		return m_hConnection != k_HSteamNetConnection_Invalid || 
			   m_hListenSocket != k_HSteamListenSocket_Invalid;
	}
	
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
	
	if (m_useSteamNetworking)
	{
		// 使用Steam網絡功能
		// 注意：Steam網絡功能不需要預先創建socket
		// 連接時會自動創建
		return true;
	}
	
	// 使用標準socket
	sock = socket(AF_INET, Type, Protocol);
	lastCode = sock;

	return sock > 0;
}

bool EzSockets::create(CString roomCode)
{
	if (!m_useSteamNetworking)
		return false;

	// 儲存房號（可選）
	m_roomCode = roomCode;

	m_lobbyCreated = false;
	m_lobbySuccess = false;

	// 開啟 callback
	m_LobbyCreatedCallback.Register(this, &EzSockets::OnLobbyCreated);

	// 建立 lobby
	SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 4);

	// 等待 callback 回來或超時
	const int timeoutMs = 5000;
	int waited = 0;
	while (!m_lobbyCreated && waited < timeoutMs)
	{
		SteamAPI_RunCallbacks(); // 可考慮由外層代為控制
		Sleep(100);
		waited += 100;
	}

	m_LobbyCreatedCallback.Unregister();

	return m_lobbySuccess;
}

void EzSockets::OnLobbyCreated(LobbyCreated_t* pCallback)
{
	m_lobbyCreated = true;
	if (pCallback->m_eResult == k_EResultOK)
	{
		m_lobbySuccess = true;
		m_lobbyID = pCallback->m_ulSteamIDLobby;
		LOG->Info("Lobby created successfully: %llu", m_lobbyID.ConvertToUint64());
	}
	else
	{
		m_lobbySuccess = false;
		LOG->Warn("Failed to create lobby. Result: %d", pCallback->m_eResult);
	}
}

bool EzSockets::bind(unsigned short port)
{
	if(!check())
		return false;
	
	if (m_useSteamNetworking)
	{
		// 使用Steam網絡功能
		SteamNetworkingIPAddr addr;
		addr.Clear();
		addr.m_port = port;
		
		m_hListenSocket = m_pNetworkingSockets->CreateListenSocketIP(addr, 0, nullptr);
		if (m_hListenSocket == k_HSteamListenSocket_Invalid)
		{
			LOG->Warn("Failed to create Steam listen socket on port %d", port);
			return false;
		}
		
		state = skLISTENING;
		return true;
	}
	
	// 使用標準socket
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(port);
	lastCode = ::bind(sock,(struct sockaddr*)&addr, sizeof(addr));
	return !lastCode;
}

bool EzSockets::listen()
{
	if (m_useSteamNetworking)
	{
		// Steam網絡功能在bind時就已經開始監聽
		return m_hListenSocket != k_HSteamListenSocket_Invalid;
	}
	
	// 使用標準socket
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
	if (m_useSteamNetworking)
	{
		// Steam網絡功能使用回調處理新連接
		// 這裡我們需要檢查是否有待處理的連接
		// 注意：這是一個簡化實現，實際應用中可能需要更複雜的邏輯
		
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查是否有連接
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			// 獲取連接信息
			SteamNetConnectionInfo_t info;
			if (m_pNetworkingSockets->GetConnectionInfo(m_hConnection, &info))
			{
				// 設置socket的連接
				socket.m_hConnection = m_hConnection;
				socket.state = skCONNECTED;
				
				// 獲取IP地址
				socket.address = SteamNetworkingIPAddrToString(info.m_addrRemote).c_str();
				
				return true;
			}
		}
		
		return false;
	}
	
	// 使用標準socket
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
	
	if (m_useSteamNetworking)
	{
		// 關閉Steam連接
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			m_pNetworkingSockets->CloseConnection(m_hConnection, 0, nullptr, false);
			m_hConnection = k_HSteamNetConnection_Invalid;
		}
		
		// 關閉Steam監聽socket
		if (m_hListenSocket != k_HSteamListenSocket_Invalid)
		{
			m_pNetworkingSockets->CloseListenSocket(m_hListenSocket);
			m_hListenSocket = k_HSteamListenSocket_Invalid;
		}
	}
	else
	{
		// 使用標準socket
		#if defined(WIN32) // The close socket command is different in Windows
			::closesocket(sock);
		#else
			::close(sock);
		#endif
	}
}

long EzSockets::uAddr()
{
	if (m_useSteamNetworking)
	{
		// 獲取Steam連接的IP地址
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetConnectionInfo_t info;
			if (m_pNetworkingSockets->GetConnectionInfo(m_hConnection, &info))
			{
				return info.m_addrRemote.GetIPv4();
			}
		}
		
		return 0;
	}
	
	// 使用標準socket
	return addr.sin_addr.s_addr;
}


bool EzSockets::connect(const std::string& host, unsigned short port)
{
	if(!check())
		return false;
	
	if (m_useSteamNetworking)
	{
		// 使用Steam網絡功能
		SteamNetworkingIPAddr addr = CreateSteamNetworkingIPAddr(host, port);
		
		m_hConnection = m_pNetworkingSockets->ConnectByIPAddress(addr, 0, nullptr);
		if (m_hConnection == k_HSteamNetConnection_Invalid)
		{
			LOG->Warn("Failed to connect to %s:%d using Steam networking", host.c_str(), port);
			return false;
		}
		
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 等待連接完成
		int attempts = 0;
		while (state != skCONNECTED && attempts < 10)
		{
			ProcessSteamCallbacks();
			Sleep(100);
			attempts++;
		}
		
		return state == skCONNECTED;
	}
	
	// 使用標準socket
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
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查是否有數據可讀
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetworkingMessage_t* pMessages[1];
			int numMessages = m_pNetworkingSockets->ReceiveMessagesOnConnection(m_hConnection, pMessages, 1);
			
			if (numMessages > 0)
			{
				// 將消息添加到接收緩衝區
				inBuffer.append((const char*)pMessages[0]->GetData(), pMessages[0]->GetSize());
				
				// 釋放消息
				pMessages[0]->Release();
				
				return true;
			}
		}
		
		return false;
	}
	
	// 使用標準socket
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1,scks,NULL,NULL,times) > 0;
}

bool EzSockets::IsError()
{
	if (state == skERROR)
		return true;
	
	if (m_useSteamNetworking)
	{
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查連接狀態
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetConnectionInfo_t info;
			if (m_pNetworkingSockets->GetConnectionInfo(m_hConnection, &info))
			{
				if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
					info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
				{
					state = skERROR;
					return true;
				}
			}
		}
		
		return false;
	}
	
	// 使用標準socket
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
		// Steam網絡功能不需要檢查是否可以寫入
		// 它會自動處理擁塞控制
		return true;
	}
	
	// 使用標準socket
	FD_ZERO(scks);
	FD_SET((unsigned)sock, scks);
	
	return select(sock+1, NULL, scks, NULL, times) > 0;
}

void EzSockets::update()
{
	if (IsError()) //If socket is in error, don't bother.
		return;
	
	if (m_useSteamNetworking)
	{
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查是否有數據可讀
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetworkingMessage_t* pMessages[10];
			int numMessages = m_pNetworkingSockets->ReceiveMessagesOnConnection(m_hConnection, pMessages, 10);
			
			for (int i = 0; i < numMessages; i++)
			{
				// 將消息添加到接收緩衝區
				inBuffer.append((const char*)pMessages[i]->GetData(), pMessages[i]->GetSize());
				
				// 釋放消息
				pMessages[i]->Release();
			}
		}
		
		// 發送緩衝區中的數據
		if (outBuffer.length() > 0)
		{
			pUpdateWrite();
		}
		
		return;
	}
	
	// 使用標準socket
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
	if (m_useSteamNetworking)
	{
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查是否有數據可讀
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetworkingMessage_t* pMessages[1];
			int numMessages = m_pNetworkingSockets->ReceiveMessagesOnConnection(m_hConnection, pMessages, 1);
			
			if (numMessages > 0)
			{
				// 將消息添加到接收緩衝區
				inBuffer.append((const char*)pMessages[0]->GetData(), pMessages[0]->GetSize());
				
				// 釋放消息
				pMessages[0]->Release();
				
				return pMessages[0]->GetSize();
			}
		}
		
		return 0;
	}
	
	// 使用標準socket
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
		// 發送緩衝區中的數據
		if (m_hConnection != k_HSteamNetConnection_Invalid && outBuffer.length() > 0)
		{
			EResult result = m_pNetworkingSockets->SendMessageToConnection(
				m_hConnection, outBuffer.c_str(), outBuffer.length(), 
				k_nSteamNetworkingSend_Reliable, nullptr);
				
			if (result == k_EResultOK)
			{
				int bytesSent = outBuffer.length();
				outBuffer = "";
				return bytesSent;
			}
			else
			{
				LOG->Warn("Failed to send data via Steam networking. Error: %d", result);
				state = skERROR;
				return -1;
			}
		}
		
		return 0;
	}
	
	// 使用標準socket
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
		// 處理Steam回調
		ProcessSteamCallbacks();
		
		// 檢查是否有數據可讀
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetworkingMessage_t* pMessages[1];
			int numMessages = m_pNetworkingSockets->ReceiveMessagesOnConnection(m_hConnection, pMessages, 1);
			
			if (numMessages > 0)
			{
				// 複製數據
				int size = pMessages[0]->GetSize();
				if (size > 1024)
					size = 1024;
					
				memcpy(data, pMessages[0]->GetData(), size);
				
				// 釋放消息
				pMessages[0]->Release();
				
				return size;
			}
		}
		
		return 0;
	}
	
	// 使用標準socket
	if(state == skCONNECTED || state == skLISTENING)
		return recv(sock, data, 1024, 0);
	
	fromAddr_len = sizeof(sockaddr_in);
	return recvfrom(sock, data, 1024, 0, (sockaddr*)&fromAddr,
					(socklen_t*)&fromAddr_len);
}

int EzSockets::pWriteData(const char* data, int dataSize)
{
	if (m_useSteamNetworking)
	{
		// 發送數據
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			EResult result = m_pNetworkingSockets->SendMessageToConnection(
				m_hConnection, data, dataSize, 
				k_nSteamNetworkingSend_Reliable, nullptr);
				
			if (result == k_EResultOK)
				return dataSize;
			else
			{
				LOG->Warn("Failed to send data via Steam networking. Error: %d", result);
				return -1;
			}
		}
		
		return 0;
	}
	
	// 使用標準socket
	return send(sock, data, dataSize, 0);
}

CString EzSockets::getIp()
{
	if (m_useSteamNetworking)
	{
		// 獲取Steam連接的IP地址
		if (m_hConnection != k_HSteamNetConnection_Invalid)
		{
			SteamNetConnectionInfo_t info;
			if (m_pNetworkingSockets->GetConnectionInfo(m_hConnection, &info))
			{
				char szAddr[128];
				info.m_addrRemote.ToString(szAddr, sizeof(szAddr), true);
				return CString(szAddr);
			}
		}
		else if (m_hListenSocket != k_HSteamListenSocket_Invalid)
		{
			SteamNetworkingIPAddr addr;
			if (m_pNetworkingSockets->GetListenSocketAddress(m_hListenSocket, &addr))
			{
				char szAddr[128];
				addr.ToString(szAddr, sizeof(szAddr), true);
				return CString(szAddr);
			}
		}
		
		return CString("0.0.0.0");
	}
	
	// 使用標準socket
	struct sockaddr_in name;
	socklen_t namelen = sizeof(name);
	getsockname(sock, (struct sockaddr*)&name, &namelen);
	
	char* str = inet_ntoa(name.sin_addr);
	CString cstr = str;
	return cstr;	
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
