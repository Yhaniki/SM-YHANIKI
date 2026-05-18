// =============================================================================
//  StandAloneServer/shim/global.h
//   Minimal replacement for src/global.h, used ONLY by StandAloneServer build.
//   Provides: CString (CStdString), basic types, NORETURN, FOREACH_ENUM,
//             Checkpoints stubs, FAIL_M/ASSERT_M, etc., WITHOUT pulling in
//             arch_setup.h / Crash / RageException / SDL / DirectX / boost.
// =============================================================================
#ifndef GLOBAL_H
#define GLOBAL_H

#if defined(_MSC_VER) && _MSC_VER >= 1000
#pragma once
#endif

// Avoid winsock.h being pulled in by windows.h conflicting with winsock2.h
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#define __STDC__ 0

// Endian
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

// Disable noisy MSVC warnings (mirror StepMania.vcxproj options)
#pragma warning (disable : 4100) // unreferenced formal parameter
#pragma warning (disable : 4244) // conversion, possible loss of data
#pragma warning (disable : 4267) // size_t -> int
#pragma warning (disable : 4305) // truncation
#pragma warning (disable : 4996) // deprecated
#pragma warning (disable : 4018) // signed/unsigned mismatch

#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

using namespace std;

// PRINTF macro used in RageLog.h declarations
#if defined(__GNUC__)
#define PRINTF(a,b) __attribute__((format(__printf__,a,b)))
#else
#define PRINTF(a,b)
#endif

// NORETURN
#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#define NORETURN __attribute__((__noreturn__))
#else
#define NORETURN
#endif

// CString / CStdString - reuse the real StdString.h from ../src/
#include "StdString.h"

typedef const CString& CCStringRef;

// usleep -> Sleep().  arch_setup.h does this same trick with my_usleep, but we
// don't pull in arch_setup.h.  Defining this here means NetworkSyncServer.cpp
// (which uses usleep) keeps compiling on MSVC without modification.
#include <windows.h>
#ifndef usleep
#define usleep(us) Sleep((us) / 1000)
#endif

// === Force-include all shim headers so the real src/ versions get short-
// circuited by their identical header guards.  This is the only reliable way
// to override headers when re-compiling cpp files that live in src/, since
// `#include "Foo.h"` from src/Foo.cpp always finds src/Foo.h first.
#include "PlayerNumber.h"
#include "Difficulty.h"
#include "RageLog.h"
#include "PrefsManager.h"

// Checkpoints stubs (real one is in arch crash handler). Standalone server
// just no-ops them; if a checkpoint fires we don't have a crash handler to
// pretty-print backtraces, but we don't need one for a headless server.
namespace Checkpoints
{
	inline void SetCheckpoint( const char* /*file*/, int /*line*/, const char* /*message*/ ) {}
}
#define CHECKPOINT (Checkpoints::SetCheckpoint(__FILE__, __LINE__, NULL))
#define CHECKPOINT_M(m) (Checkpoints::SetCheckpoint(__FILE__, __LINE__, m))

// Crash / asserts -> just abort. Server logs are enough for debug.
void NORETURN sm_crash( const char* reason = "Internal error" );
#define FAIL_M(MESSAGE) { sm_crash(MESSAGE); }
#define ASSERT_M(COND, MESSAGE) { if(!(COND)) { FAIL_M(MESSAGE); } }
#define ASSERT(COND) ASSERT_M((COND), "Assertion '" #COND "' failed")
#ifdef DEBUG
#define DEBUG_ASSERT(x) ASSERT(x)
#else
#define DEBUG_ASSERT(x)
#endif

// FOREACH_ENUM is referenced by NetworkSyncManager.h indirectly via the FOREACH_NSScoreBoardColumn macro;
// the server code itself doesn't call it but the macro must still parse.
#define FOREACH_ENUM( e, max, var ) for( e var=(e)0; var<max; var=(e)(var+1) )

#endif // GLOBAL_H
