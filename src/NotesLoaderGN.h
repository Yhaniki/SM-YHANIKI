/* GNLoader - 從 SDO 系列的 .gn (StepFile) 讀出一首歌。 */

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

	/* 一個資料夾裡若有多個 .gn，選出主檔（優先 *K.gn）。回傳相對於 sDir 的檔名。 */
	static CString PickMainGNFile( CString sDir );

	/* 讀檔＋解密＋解析。sPath 是完整路徑。 */
	static bool ReadGNFile( CString sPath, GNFile::StepFile &sfOut, GNFile::ContainerInfo &infoOut, CString &sErrOut );

	/* 單一難度：GN frames → SM 的 NoteData / TimingData。
	 * fMusicStartBeatOut 是 type-10（音樂起點）所在的拍數。 */
	static void FramesToNoteData( const std::vector<GNFile::Frame> &vFrames, NoteData &out );
	static void FramesToTimingData( const std::vector<GNFile::Frame> &vFrames, float fHeaderBPM, TimingData &out );

	/* 依 BPM 表，算出從第 0 拍到 fBeat 經過幾秒（不含任何 offset）。 */
	static float GetSecondsFromBeat( const TimingData &timing, float fBeat );

	/* GN 的 step_frame_type 與 dance-single 欄位的對應。 */
	static int FrameTypeToCol( int iFrameType );
	static int ColToFrameType( int iCol );

	/* GN 的 level（約為 osu! 星數 ×5）與 SM meter 的換算。 */
	static int GNLevelToMeter( int iLevel );
	static int MeterToGNLevel( int iMeter );
};

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
