// =============================================================================
//  StandAloneServer/ServerStubs.cpp
//   Provides the small set of globals + helpers that NetworkSyncServer.cpp
//   and ezsockets.cpp would normally pull in from the rest of StepMania:
//      - RageLog* LOG               (prints to stdout)
//      - PrefsManager* PREFSMAN     (hard-coded defaults)
//      - sm_crash()                 (logs reason and aborts)
//      - PacketFunctions::*         (copied from src/NetworkSyncManager.cpp,
//                                    because we don't link that .cpp here)
//
//   This file is the only place that "knows" we're running headless: the rest
//   of the server source is reused verbatim from src/.
// =============================================================================
#include "global.h"
#include "RageLog.h"
#include "PrefsManager.h"
#include "NetworkSyncManager.h"   // for PacketFunctions, NETMAXBUFFERSIZE

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <winsock2.h>             // for ntohs/ntohl/htons/htonl

// ---------------------------------------------------------------------------
// Globals required by the reused .cpp files.
// ---------------------------------------------------------------------------
RageLog     *LOG      = nullptr;
PrefsManager*PREFSMAN = nullptr;

// ---------------------------------------------------------------------------
// RageLog implementation - prints to stdout with [INFO]/[WARN]/[TRACE] prefix
// and a timestamp, mirroring the look of StepMania's log.txt closely enough
// for diagnostic purposes.
// ---------------------------------------------------------------------------
static std::mutex g_logMutex;

static void WriteLogLine( const char *tag, const char *fmt, va_list ap )
{
	char buf[4096];
	int n = vsnprintf(buf, sizeof(buf)-1, fmt, ap);
	if (n < 0) n = 0;
	if (n >= (int)sizeof(buf)) n = sizeof(buf)-1;
	buf[n] = '\0';

	time_t t = time(NULL);
	struct tm tmv;
	localtime_s(&tmv, &t);
	char ts[32];
	strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

	std::lock_guard<std::mutex> lock(g_logMutex);
	fprintf(stdout, "[%s] %s %s\n", ts, tag, buf);
	fflush(stdout);
}

RageLog::RageLog()  {}
RageLog::~RageLog() {}

void RageLog::Info(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	WriteLogLine("INFO", fmt, ap);
	va_end(ap);
}

void RageLog::Warn(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	WriteLogLine("WARN", fmt, ap);
	va_end(ap);
}

void RageLog::Trace(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	WriteLogLine("TRCE", fmt, ap);
	va_end(ap);
}

void RageLog::Flush() { fflush(stdout); }

// ---------------------------------------------------------------------------
// sm_crash - the StepMania-side version drops into a crash handler with stack
// dumps.  Here we just log and abort so the server exits with non-zero status.
// ---------------------------------------------------------------------------
void sm_crash(const char *reason)
{
	if (LOG) LOG->Warn("sm_crash: %s", reason ? reason : "(null)");
	else     fprintf(stderr, "sm_crash: %s\n", reason ? reason : "(null)");
	std::abort();
}

// ---------------------------------------------------------------------------
// PacketFunctions implementation (verbatim copy from src/NetworkSyncManager.cpp)
// We can't link the real .cpp because it depends on the whole StepMania.
// ---------------------------------------------------------------------------
uint8_t PacketFunctions::Read1()
{
	if (Position >= NETMAXBUFFERSIZE)
		return 0;
	return Data[Position++];
}

uint16_t PacketFunctions::Read2()
{
	if (Position >= NETMAXBUFFERSIZE-1)
		return 0;
	uint16_t Temp;
	memcpy(&Temp, Data + Position, 2);
	Position += 2;
	return ntohs(Temp);
}

uint32_t PacketFunctions::Read4()
{
	if (Position >= NETMAXBUFFERSIZE-3)
		return 0;
	uint32_t Temp;
	memcpy(&Temp, Data + Position, 4);
	Position += 4;
	return ntohl(Temp);
}

CString PacketFunctions::ReadNT()
{
	CString TempStr;
	while ((Position < NETMAXBUFFERSIZE) && (((char*)Data)[Position] != 0))
		TempStr = TempStr + (char)Data[Position++];
	++Position;
	return TempStr;
}

void PacketFunctions::Write1(uint8_t data)
{
	if (Position >= NETMAXBUFFERSIZE) return;
	memcpy(&Data[Position], &data, 1);
	++Position;
}

void PacketFunctions::Write2(uint16_t data)
{
	if (Position >= NETMAXBUFFERSIZE-1) return;
	data = htons(data);
	memcpy(&Data[Position], &data, 2);
	Position += 2;
}

void PacketFunctions::Write4(uint32_t data)
{
	if (Position >= NETMAXBUFFERSIZE-3) return;
	data = htonl(data);
	memcpy(&Data[Position], &data, 4);
	Position += 4;
}

void PacketFunctions::WriteNT(const CString& data)
{
	int index = 0;
	while ((Position < NETMAXBUFFERSIZE) && (index < data.GetLength()))
		Data[Position++] = (unsigned char)(data.c_str()[index++]);
	Data[Position++] = 0;
}

void PacketFunctions::WriteBytes(const char *src, int bytes)
{
	if (bytes <= 0) return;
	int room = NETMAXBUFFERSIZE - Position;
	if (room <= 0) return;
	int n = bytes < room ? bytes : room;
	memcpy(&Data[Position], src, n);
	Position += n;
}

int PacketFunctions::ReadBytes(char *out, int bytes)
{
	if (bytes <= 0) return 0;
	int room = NETMAXBUFFERSIZE - Position;
	if (room <= 0) return 0;
	int n = bytes < room ? bytes : room;
	memcpy(out, &Data[Position], n);
	Position += n;
	return n;
}

void PacketFunctions::ClearPacket()
{
	memset((void*)(&Data), 0, NETMAXBUFFERSIZE);
	Position = 0;
	PayloadLength = 0;
}
