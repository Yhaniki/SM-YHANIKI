#ifndef SONG_CACHE_INDEX_H
#define SONG_CACHE_INDEX_H

#include "IniFile.h"
#include "RageThreads.h"
#include <map>

/* All members are safe to call from several threads at once (songs are loaded
 * by worker threads, see SongManager::LoadStepManiaSongDir). */
class SongCacheIndex
{
	mutable RageMutex m_Mutex;

	IniFile CacheIndex;
	static CString MangleName( const CString &Name );

	/* While a batch is open, AddCacheIndex only marks index.cache dirty instead
	 * of rewriting the whole file for every song; EndBatch writes it once. */
	int m_iBatchDepth;
	bool m_bCacheIndexDirty;
	void WriteCacheIndex();

	/* The song index (Cache/songs.idx): for each song, its cache file contents
	 * with the note data stripped (NotesWriterSM::GetCacheMetaText).  At startup
	 * complete Song and Steps objects are built from it, without opening and
	 * parsing every cache file under Cache/Songs/; notes are read from the cache
	 * file by Steps::Decompress only when they're needed.
	 *
	 * uCacheHash is GetCacheHash(song dir) at the time the data was generated.
	 * Whenever the cache file is rewritten (Song::SaveToCacheFile), GetCacheHash
	 * changes too, and the entry is ignored from then on. */
	struct SongMeta
	{
		unsigned uCacheHash;
		CString sData;
	};
	map<CString, SongMeta> m_SongMeta;
	bool m_bSongMetaDirty;

	void ReadSongMeta();

public:
	SongCacheIndex();
	~SongCacheIndex();

	void ReadCacheIndex();
	void AddCacheIndex( const CString &path, unsigned hash );
	unsigned GetCacheHash( const CString &path ) const;

	/* Batches nest.  FlushCacheIndex writes pending changes without ending the
	 * batch, so a crash during a long load loses only the latest entries. */
	void BeginBatch();
	void EndBatch();
	void FlushCacheIndex();

	/* Returns true only if the entry exists and its cache hash matches. */
	bool GetSongMeta( const CString &sSongDir, unsigned uCacheHash, CString &sDataOut ) const;
	void SetSongMeta( const CString &sSongDir, unsigned uCacheHash, const CString &sData );
	/* Keep only the songs in vsSongDirs; write the file only if something changed. */
	void SaveSongMeta( const vector<CString> &vsSongDirs );
};

extern SongCacheIndex *SONGINDEX;	// global and accessable from anywhere in our program

#endif

/*
 * (c) 2002-2003 Glenn Maynard
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
