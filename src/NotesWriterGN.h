/* NotesWriterGN - writes a Song back to its original .gn file. */

#ifndef NOTES_WRITER_GN_H
#define NOTES_WRITER_GN_H

#include "GNFile.h"

class Song;
class Steps;
class TimingData;
class NoteData;

class NotesWriterGN
{
public:
	/* Use the existing .gn at sPath as a skeleton, write the song's notes and BPMs back
	 * into it, and overwrite the original file. Header fields we do not interpret and
	 * control frames such as type 9/10 are kept as-is.
	 *
	 * bKeepFileSize: the output must be exactly the same size as the original file (the
	 *                SDOM container already relies on zero padding to keep its size;
	 *                fails outright if the chart does not fit). False by default, i.e.
	 *                write back regardless of size.
	 *
	 * On failure, returns false and fills in sErrOut; the original file is not touched. */
	static bool Write( CString sPath, const Song &song, bool bKeepFileSize, CString &sErrOut );

	/* Fit a single difficulty's notes and timing into the skeleton's frame list
	 * (control frames are kept). */
	static void BuildFrames( const NoteData &nd, const TimingData &timing, float fHeaderBPM,
		const vector<GNFile::Frame> &vOriginal, vector<GNFile::Frame> &vOut );
};

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
