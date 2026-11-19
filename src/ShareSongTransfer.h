/* ShareSongTransfer - direct peer-to-peer song folder transfer over TCP.
 *
 * Replaces the old winsocket_server.exe / winsocket_client.exe / miniZip.exe
 * tool chain used by the /share commands.
 *
 * - The sender listens (port 1234, the same port the old tool used, so
 *   existing port forwarding keeps working) and the receiver connects to it.
 * - Files are streamed one by one, no zip step.
 * - The receiver decides which files it needs, so the same protocol covers
 *   "send the whole song" and "only send the missing video files".
 * - Every blocking step has a timeout.  When no data moves for
 *   STALL_TIMEOUT_MS the connection is dropped on both ends, partial files
 *   are removed and the caller gets RESULT_FAILED.
 *
 * The worker runs on its own thread.  It never touches game objects; the
 * caller polls the progress / result from the main thread.
 */

#ifndef SHARE_SONG_TRANSFER_H
#define SHARE_SONG_TRANSFER_H

#if !defined(WITHOUT_NETWORKING)

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class ShareSongTransfer
{
public:
	enum Role { ROLE_NONE, ROLE_SENDER, ROLE_RECEIVER };

	/* Values are sent over the network (NSSResult), keep them stable. */
	enum Result
	{
		RESULT_NONE = 0,
		RESULT_OK = 1,				// files transferred
		RESULT_NOTHING_NEEDED = 2,	// receiver already had every file it wanted
		RESULT_FAILED = 3,
		RESULT_CANCELLED = 4
	};

	static const unsigned short DEFAULT_PORT = 1234;

	ShareSongTransfer();
	~ShareSongTransfer();

	/* Send the song folder sSongDir (absolute path, UTF-8; so are the receiver's dirs).
	 * Video files are skipped unless bIncludeVideo. */
	bool StartSend( int iTransferId, const CString &sSongDir, bool bIncludeVideo );

	/* Receive from the sender.  vsHosts are tried in order.
	 * If sExistingSongDir is not empty the receiver already has this song:
	 * only video files missing from that folder are requested and written there.
	 * Otherwise the song is written to a new folder inside sConnectDir. */
	bool StartReceive( int iTransferId, const vector<CString> &vsHosts, unsigned short iPort,
		unsigned int iToken, const CString &sExistingSongDir, const CString &sConnectDir );

	/* Ask the worker to stop.  It exits within ~POLL_MS and reports RESULT_CANCELLED. */
	void Cancel();

	/* True from Start* until PollFinished() has collected the result. */
	bool IsBusy() const { return m_Role != ROLE_NONE; }

	/* Returns true once after the worker finished, then goes back to idle. */
	bool PollFinished( Result &result, CString &sMessage );

	Role GetRole() const { return m_Role; }
	int GetTransferId() const { return m_iTransferId; }

	/* Sender: listening socket is ready, GetListenPort()/GetToken() are valid. */
	bool IsListening() const { return m_bListening; }
	unsigned short GetListenPort() const { return m_iListenPort; }
	unsigned int GetToken() const { return m_iToken; }

	/* Bytes of the files that are actually transferred (valid once HasTotal()). */
	bool HasTotal() const { return m_bHaveTotal; }
	unsigned long long GetDoneBytes() const { return m_iDoneBytes; }
	unsigned long long GetTotalBytes() const { return m_iTotalBytes; }

	/* Receiver, new song: name of the folder created inside sConnectDir
	 * (may get a " (2)" suffix when the name was taken).  Empty otherwise. */
	CString GetReceivedFolder();

	static bool IsVideoFile( const CString &sFileName );

private:
	void SenderThread( CString sSongDir, bool bIncludeVideo );
	void ReceiverThread( vector<CString> vsHosts, unsigned short iPort, unsigned int iToken,
		CString sExistingSongDir, CString sConnectDir );
	void Finish( Result result, const CString &sMessage );
	void JoinWorker();

	Role m_Role;					// main thread only
	int m_iTransferId;				// main thread only
	std::thread m_Thread;

	std::atomic<bool> m_bCancel;
	std::atomic<bool> m_bFinished;
	std::atomic<bool> m_bListening;
	std::atomic<bool> m_bHaveTotal;
	std::atomic<unsigned short> m_iListenPort;
	std::atomic<unsigned int> m_iToken;
	std::atomic<unsigned long long> m_iDoneBytes;
	std::atomic<unsigned long long> m_iTotalBytes;

	std::mutex m_Mutex;				// protects the fields below
	Result m_Result;
	CString m_sMessage;
	CString m_sReceivedFolder;
};

#endif
#endif

