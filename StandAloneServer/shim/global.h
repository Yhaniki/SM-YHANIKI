/* StandAloneServer/shim/global.h
 *
 * Stand-in for src/global.h, force-included (/FI) into every file of the
 * standalone server.  The server logic itself is the game's own code
 * (src/NetworkSyncServer.cpp, src/ezsockets.cpp, src/NetworkPacket.cpp);
 * these shims only replace the few game headers it touches, so the server
 * can be built without SDL, Lua, the renderer and so on.
 *
 * Every shim uses the include guard of the header it replaces, and this file
 * includes them all up front: when src/ code later does #include "RageLog.h",
 * the real header is already "included" and expands to nothing.
 *
 * If a build of StandAloneServer fails after a change to the server code, the
 * server started using another part of the game: add what it needs here (or
 * to ServerStubs.cpp) rather than copying server code. */
#ifndef GLOBAL_H
#define GLOBAL_H

#if defined(_MSC_VER)
#pragma once
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define ENDIAN_LITTLE
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#pragma warning (disable : 4018) // signed/unsigned mismatch
#pragma warning (disable : 4100) // unreferenced formal parameter
#pragma warning (disable : 4244) // conversion, possible loss of data
#pragma warning (disable : 4267) // size_t -> int
#pragma warning (disable : 4305) // truncation
#pragma warning (disable : 4996) // deprecated CRT / winsock functions

/* winsock2.h has to come before windows.h. */
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <stdint.h>

using namespace std;

#define PRINTF(a,b)
#define NORETURN __declspec(noreturn)

#include "StdString.h"

/* src/archutils/Win32/arch_setup.h maps usleep the same way. */
#ifndef usleep
#define usleep(us) Sleep((DWORD)((us) / 1000))
#endif

namespace Checkpoints
{
	inline void SetCheckpoint( const char *, int, const char * ) { }
}
#define CHECKPOINT (Checkpoints::SetCheckpoint(__FILE__, __LINE__, NULL))
#define CHECKPOINT_M(m) (Checkpoints::SetCheckpoint(__FILE__, __LINE__, m))

void NORETURN sm_crash( const char *reason = "Internal error" );
#define FAIL_M(MESSAGE) { sm_crash(MESSAGE); }
#define ASSERT_M(COND, MESSAGE) { if(!(COND)) { FAIL_M(MESSAGE); } }
#define ASSERT(COND) ASSERT_M((COND), "Assertion '" #COND "' failed")
#define DEBUG_ASSERT(x)

#define FOREACH_ENUM( e, max, var ) for( e var=(e)0; var<max; var=(e)(var+1) )

/* Pre-empt the real headers (same include guards). */
#include "PlayerNumber.h"
#include "Difficulty.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "PrefsManager.h"

#endif
