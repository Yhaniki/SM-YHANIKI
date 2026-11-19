/* ServerLog - the standalone server's event log.
 * Lines are UTF-8.  They are written to StandAloneServer.log and kept for the
 * window, which collects them with Take().  Safe to call from any thread. */
#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include <string>
#include <vector>

namespace ServerLog
{
	void Open( const std::wstring &sPath );	// starts a new file
	void Close();
	void Write( const std::string &sLine );	// adds the time
	void Take( std::vector<std::string> &vOut );
}

#endif
