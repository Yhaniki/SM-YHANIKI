/* Shim for src/RageLog.h (see shim/global.h).  Same interface as the game's
 * logger; ServerStubs.cpp sends the lines to the server window and log file. */
#ifndef RAGELOG_H
#define RAGELOG_H

class RageLog
{
public:
	void Trace( const char *fmt, ... );
	void Warn( const char *fmt, ... );
	void Info( const char *fmt, ... );
	void Flush() { }
	void MapLog( const CString &, const char *, ... ) { }
	void UnmapLog( const CString & ) { }
};

extern RageLog *LOG;

#endif
