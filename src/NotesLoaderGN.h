/* GNLoader - loads a song from an SDO-series .gn (StepFile). */

#ifndef NOTES_LOADER_GN_H
#define NOTES_LOADER_GN_H

#include "song.h"
#include "Steps.h"
#include "NotesLoader.h"
#include "GNFile.h"

class NoteData;

class GNLoader: public NotesLoader
{
public:
	void GetApplicableFiles( CString sPath, CStringArray &out );
	bool LoadFromDir( CString sDir, Song &out );

	/* If a folder has several .gn files, pick the main one (*K.gn preferred).
	 * Returns the file name relative to sDir. */
	static CString PickMainGNFile( CString sDir );

	/* Read + decrypt + parse. sPath is the full path. */
	static bool ReadGNFile( CString sPath, GNFile::StepFile &sfOut, GNFile::ContainerInfo &infoOut, CString &sErrOut );

	/* A single difficulty: GN frames -> SM NoteData / TimingData.
	 * fMusicStartBeatOut is the beat at which the type-10 frame (music start) sits. */
	static void FramesToNoteData( const std::vector<GNFile::Frame> &vFrames, NoteData &out );
	static void FramesToTimingData( const std::vector<GNFile::Frame> &vFrames, float fHeaderBPM, TimingData &out );

	/* Using the BPM table, compute how many seconds pass from beat 0 to fBeat
	 * (without any offset). */
	static float GetSecondsFromBeat( const TimingData &timing, float fBeat );

	/* Mapping between GN step_frame_type and dance-single columns. */
	static int FrameTypeToCol( int iFrameType );
	static int ColToFrameType( int iCol );

	/* Conversion between GN level (roughly osu! star rating x5) and SM meter. */
	static int GNLevelToMeter( int iLevel );
	static int MeterToGNLevel( int iMeter );
};

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
