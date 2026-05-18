// Minimal RageLog.h for StandAloneServer build.
// Mirrors the public interface used by NetworkSyncServer.cpp / ezsockets.cpp
// so they can be compiled unmodified.  Implementation lives in ServerStubs.cpp.
#ifndef RAGELOG_H
#define RAGELOG_H

class RageLog
{
public:
	RageLog();
	~RageLog();

	void Trace( const char *fmt, ...) PRINTF(2,3);
	void Warn ( const char *fmt, ...) PRINTF(2,3);
	void Info ( const char *fmt, ...) PRINTF(2,3);
	void Flush();

	void MapLog( const CString &/*key*/, const char* /*fmt*/, ... ) {}
	void UnmapLog( const CString &/*key*/ ) {}
};

extern RageLog *LOG;

#endif
