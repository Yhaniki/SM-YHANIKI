/* ServerStubs - the few game globals and helpers that the shared server code
 * (src/NetworkSyncServer.cpp, src/ezsockets.cpp, src/NetworkPacket.cpp)
 * expects from the rest of the game, plus the server log. */

#include "global.h"
#include "ServerLog.h"

#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

static RageLog g_Log;
static PrefsManager g_Prefs;
RageLog *LOG = &g_Log;
PrefsManager *PREFSMAN = &g_Prefs;

/* ---------------- ServerLog ---------------- */

namespace
{
	std::mutex g_LogMutex;
	FILE *g_pLogFile = NULL;
	std::deque<std::string> g_PendingLines;	// not yet collected by the window
	const size_t MAX_PENDING_LINES = 2000;
}

void ServerLog::Open( const std::wstring &sPath )
{
	std::lock_guard<std::mutex> lock( g_LogMutex );
	if( g_pLogFile )
		fclose( g_pLogFile );
	g_pLogFile = _wfopen( sPath.c_str(), L"wb" );
	if( g_pLogFile )
		fwrite( "\xEF\xBB\xBF", 1, 3, g_pLogFile );	// UTF-8 BOM, so Notepad shows names right
}

void ServerLog::Close()
{
	std::lock_guard<std::mutex> lock( g_LogMutex );
	if( g_pLogFile )
		fclose( g_pLogFile );
	g_pLogFile = NULL;
}

void ServerLog::Write( const std::string &sLine )
{
	time_t now = time( NULL );
	struct tm tmNow;
	localtime_s( &tmNow, &now );
	char sTime[16];
	strftime( sTime, sizeof(sTime), "%H:%M:%S", &tmNow );
	std::string sFull = std::string( "[" ) + sTime + "] " + sLine;

	std::lock_guard<std::mutex> lock( g_LogMutex );
	if( g_pLogFile )
	{
		fprintf( g_pLogFile, "%s\r\n", sFull.c_str() );
		fflush( g_pLogFile );
	}
	g_PendingLines.push_back( sFull );
	while( g_PendingLines.size() > MAX_PENDING_LINES )
		g_PendingLines.pop_front();
}

void ServerLog::Take( std::vector<std::string> &vOut )
{
	std::lock_guard<std::mutex> lock( g_LogMutex );
	vOut.insert( vOut.end(), g_PendingLines.begin(), g_PendingLines.end() );
	g_PendingLines.clear();
}

/* ---------------- RageLog ---------------- */

void RageLog::Info( const char *fmt, ... )
{
	va_list va;
	va_start( va, fmt );
	ServerLog::Write( (const char *)vssprintf( fmt, va ) );
	va_end( va );
}

void RageLog::Warn( const char *fmt, ... )
{
	va_list va;
	va_start( va, fmt );
	ServerLog::Write( "WARNING: " + std::string( (const char *)vssprintf( fmt, va ) ) );
	va_end( va );
}

void RageLog::Trace( const char *, ... )
{
	// The game writes these to log.txt only; the window doesn't need them.
}

/* ---------------- RageUtil ---------------- */

CString vssprintf( const char *fmt, va_list argList )
{
	va_list copy;
	va_copy( copy, argList );
	int iLen = _vscprintf( fmt, copy );
	va_end( copy );
	if( iLen <= 0 )
		return CString();
	std::vector<char> buf( iLen + 1 );
	vsnprintf( &buf[0], buf.size(), fmt, argList );
	return CString( &buf[0] );
}

CString ssprintf( const char *fmt, ... )
{
	va_list va;
	va_start( va, fmt );
	CString s = vssprintf( fmt, va );
	va_end( va );
	return s;
}

/* ---------------- crash ---------------- */

void sm_crash( const char *reason )
{
	ServerLog::Write( std::string( "CRASH: " ) + (reason ? reason : "(null)") );
	ServerLog::Close();
	MessageBoxA( NULL, reason ? reason : "Internal error", "StandAloneServer", MB_ICONERROR | MB_OK );
	std::abort();
}
