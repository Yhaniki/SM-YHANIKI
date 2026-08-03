/* NotesWriterGN - 把一首 Song 寫回原本的 .gn 檔。 */

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
	/* 以 sPath 這個既有的 .gn 當骨架，把 song 的音符與 BPM 寫回去，覆蓋原檔。
	 * 表頭裡我們沒解讀的欄位、type 9/10 這類控制 frame 都原樣保留。
	 *
	 * bKeepFileSize：輸出必須與原檔一模一樣大（SDOM 容器本來就靠補零維持大小；
	 *                譜面塞不下時直接失敗）。預設 false，也就是不管大小直接寫回。
	 *
	 * 失敗時回傳 false 並填入 sErrOut，原檔不會被動到。 */
	static bool Write( CString sPath, const Song &song, bool bKeepFileSize, CString &sErrOut );

	/* 把單一難度的音符與 timing 套進骨架的 frame 串列（保留控制用 frame）。 */
	static void BuildFrames( const NoteData &nd, const TimingData &timing, float fHeaderBPM,
		const vector<GNFile::Frame> &vOriginal, vector<GNFile::Frame> &vOut );
};

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
