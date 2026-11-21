#include "global.h"

#include "SongCacheIndex.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "RageFileManager.h"
#include "RageFile.h"
#include "song.h"
#include <cstring>

/*
 * A quick explanation of song cache hashes: Each song has two hashes; a hash of the
 * song path, and a hash of the song directory.  The former is Song::GetCacheFilePath;
 * it stays the same if the contents of the directory change.  The latter is 
 * GetHashForDirectory(m_sSongDir), and changes on each modification.
 *
 * The file hash is used as the cache filename.  We don't want to use the directory
 * hash: if we do that, then we'll write a new cache file every time the song changes,
 * and they'll accumulate or we'll have to be careful to delete them.
 *
 * The directory hash is stored in here, indexed by the song path, and used to determine
 * if a song has changed.
 *
 * Another advantage of this system is that we can load songs from cache given only their
 * path; we don't have to actually look in the directory (to find out the directory hash)
 * in order to find the cache file.
 */
#define CACHE_DIR "Cache/"
#define CACHE_INDEX CACHE_DIR "index.cache"
#define SONG_META_FILE CACHE_DIR "songs.idx"

/* songs.idx layout (all numbers are little-endian 32-bit unsigned integers):
 *   "SMIX" | format version | FILE_CACHE_VERSION | entry count
 *   each entry: dir length | dir | uCacheHash | data length | data
 *   end marker
 * If any read fails, a length is unreasonable or a version differs, the whole
 * file is ignored and rebuilt later. */
static const char SONG_META_MAGIC[4] = { 'S', 'M', 'I', 'X' };
static const unsigned SONG_META_FORMAT = 1;
static const unsigned SONG_META_END = 0x58494D53;	/* "SMIX" */
static const unsigned SONG_META_MAX_DIR = 4096;
static const unsigned SONG_META_MAX_DATA = 16*1024*1024;

static void PutU32( CString &s, unsigned v )
{
	const char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
	s.append( b, 4 );
}

static void PutStr( CString &s, const CString &str )
{
	PutU32( s, str.size() );
	s.append( str.data(), str.size() );
}

static bool GetU32( const CString &s, unsigned &pos, unsigned &v )
{
	if( pos > s.size() || s.size() - pos < 4 )
		return false;
	const unsigned char *p = (const unsigned char *) s.data() + pos;
	v = unsigned(p[0]) | (unsigned(p[1]) << 8) | (unsigned(p[2]) << 16) | (unsigned(p[3]) << 24);
	pos += 4;
	return true;
}

static bool GetStr( const CString &s, unsigned &pos, CString &out, unsigned iMaxLen )
{
	unsigned len;
	if( !GetU32(s, pos, len) )
		return false;
	if( len > iMaxLen || s.size() - pos < len )
		return false;
	out.assign( s.data() + pos, len );
	pos += len;
	return true;
}


SongCacheIndex *SONGINDEX;

SongCacheIndex::SongCacheIndex():
	m_Mutex( "SongCacheIndex" )
{
	m_bSongMetaDirty = false;
	m_iBatchDepth = 0;
	m_bCacheIndexDirty = false;
	ReadCacheIndex();
	ReadSongMeta();
}

SongCacheIndex::~SongCacheIndex()
{

}

static void EmptyDir( CString dir )
{
	ASSERT(dir[dir.size()-1] == '/');

	CStringArray asCacheFileNames;
	GetDirListing( dir, asCacheFileNames );
	for( unsigned i=0; i<asCacheFileNames.size(); i++ )
	{
		if( !IsADirectory(dir + asCacheFileNames[i]) )
			FILEMAN->Remove( dir + asCacheFileNames[i] );
	}
}

void SongCacheIndex::ReadCacheIndex()
{
	LockMut( m_Mutex );
	m_bCacheIndexDirty = false;
	CacheIndex.ReadFile( CACHE_INDEX );	// don't care if this fails

	int iCacheVersion = -1;
	CacheIndex.GetValue( "Cache", "CacheVersion", iCacheVersion );
	if( iCacheVersion == FILE_CACHE_VERSION )
		return; /* OK */

	LOG->Trace( "Cache format is out of date.  Deleting all cache files." );
	EmptyDir( CACHE_DIR );
	EmptyDir( CACHE_DIR "Banners/" );
	EmptyDir( CACHE_DIR "Songs/" );

	CacheIndex.Reset();

	/* songs.idx was deleted above as well; it described the cache files we just removed. */
	m_SongMeta.clear();
	m_bSongMetaDirty = false;
}

void SongCacheIndex::AddCacheIndex(const CString &path, unsigned hash)
{
	if( hash == 0 )
		++hash; /* no 0 hash values */
	LockMut( m_Mutex );
	CacheIndex.SetValue( "Cache", "CacheVersion", FILE_CACHE_VERSION );
	CacheIndex.SetValue( "Cache", MangleName(path), hash );
	m_bCacheIndexDirty = true;

	/* Rewriting the whole index for every song is O(n^2) when thousands of songs
	 * are cached at once; inside a batch, write it when the batch ends. */
	if( m_iBatchDepth == 0 )
		WriteCacheIndex();
}

void SongCacheIndex::WriteCacheIndex()
{
	ASSERT( m_Mutex.IsLockedByThisThread() );
	if( !m_bCacheIndexDirty )
		return;
	CacheIndex.WriteFile( CACHE_INDEX );
	m_bCacheIndexDirty = false;
}

void SongCacheIndex::BeginBatch()
{
	LockMut( m_Mutex );
	++m_iBatchDepth;
}

void SongCacheIndex::EndBatch()
{
	LockMut( m_Mutex );
	ASSERT( m_iBatchDepth > 0 );
	if( --m_iBatchDepth == 0 )
		WriteCacheIndex();
}

void SongCacheIndex::FlushCacheIndex()
{
	LockMut( m_Mutex );
	WriteCacheIndex();
}

unsigned SongCacheIndex::GetCacheHash( const CString &path ) const
{
	LockMut( m_Mutex );
	unsigned iDirHash;
	if( !CacheIndex.GetValue( "Cache", MangleName(path), iDirHash ) )
		return 0;
	if( iDirHash == 0 )
		++iDirHash; /* no 0 hash values */
	return iDirHash;
}

CString SongCacheIndex::MangleName( const CString &Name )
{
	/* We store paths in an INI.  We can't store '='. */
	CString ret = Name;
	ret.Replace( "=", "");
	return ret;
}

void SongCacheIndex::ReadSongMeta()
{
	LockMut( m_Mutex );
	m_SongMeta.clear();
	m_bSongMetaDirty = false;

	RageFile f;
	if( !f.Open( SONG_META_FILE, RageFile::READ ) )
		return;	/* no index yet; it's built on the first run */

	CString sBuf;
	if( f.Read( sBuf ) == -1 )
	{
		LOG->Warn( "Couldn't read song index \"%s\": %s", SONG_META_FILE, f.GetError().c_str() );
		return;
	}
	f.Close();

	unsigned pos = 0;
	unsigned uFormat, uCacheVersion, uCount;
	if( sBuf.size() < sizeof(SONG_META_MAGIC) || memcmp( sBuf.data(), SONG_META_MAGIC, sizeof(SONG_META_MAGIC) ) )
	{
		LOG->Warn( "Song index \"%s\" isn't a song index; rebuilding it.", SONG_META_FILE );
		return;
	}
	pos += sizeof(SONG_META_MAGIC);

	if( !GetU32(sBuf, pos, uFormat) || !GetU32(sBuf, pos, uCacheVersion) || !GetU32(sBuf, pos, uCount) )
	{
		LOG->Warn( "Song index \"%s\" is truncated; rebuilding it.", SONG_META_FILE );
		return;
	}
	if( uFormat != SONG_META_FORMAT || uCacheVersion != (unsigned) FILE_CACHE_VERSION )
	{
		LOG->Trace( "Song index \"%s\" is out of date (format %u, cache %u); rebuilding it.", SONG_META_FILE, uFormat, uCacheVersion );
		return;
	}

	map<CString, SongMeta> Loaded;
	for( unsigned i = 0; i < uCount; ++i )
	{
		CString sDir;
		SongMeta meta;
		if( !GetStr(sBuf, pos, sDir, SONG_META_MAX_DIR) ||
			!GetU32(sBuf, pos, meta.uCacheHash) ||
			!GetStr(sBuf, pos, meta.sData, SONG_META_MAX_DATA) )
		{
			LOG->Warn( "Song index \"%s\" is damaged at entry %u; rebuilding it.", SONG_META_FILE, i );
			return;
		}
		Loaded[sDir] = meta;
	}

	unsigned uEnd;
	if( !GetU32(sBuf, pos, uEnd) || uEnd != SONG_META_END )
	{
		LOG->Warn( "Song index \"%s\" is missing its end marker; rebuilding it.", SONG_META_FILE );
		return;
	}

	m_SongMeta.swap( Loaded );
	LOG->Trace( "Read %u entries from song index \"%s\".", (unsigned) m_SongMeta.size(), SONG_META_FILE );
}

bool SongCacheIndex::GetSongMeta( const CString &sSongDir, unsigned uCacheHash, CString &sDataOut ) const
{
	LockMut( m_Mutex );
	map<CString, SongMeta>::const_iterator it = m_SongMeta.find( sSongDir );
	if( it == m_SongMeta.end() || it->second.uCacheHash != uCacheHash )
		return false;
	sDataOut = it->second.sData;
	return true;
}

void SongCacheIndex::SetSongMeta( const CString &sSongDir, unsigned uCacheHash, const CString &sData )
{
	LockMut( m_Mutex );
	SongMeta &meta = m_SongMeta[sSongDir];
	meta.uCacheHash = uCacheHash;
	meta.sData = sData;
	m_bSongMetaDirty = true;
}

void SongCacheIndex::SaveSongMeta( const vector<CString> &vsSongDirs )
{
	LockMut( m_Mutex );

	/* Drop songs that are no longer loaded (deleted or renamed). */
	map<CString, SongMeta> Kept;
	for( unsigned i = 0; i < vsSongDirs.size(); ++i )
	{
		map<CString, SongMeta>::iterator it = m_SongMeta.find( vsSongDirs[i] );
		if( it != m_SongMeta.end() )
			Kept[it->first] = it->second;
	}
	if( Kept.size() != m_SongMeta.size() )
		m_bSongMetaDirty = true;
	m_SongMeta.swap( Kept );

	if( !m_bSongMetaDirty )
		return;

	CString sBuf;
	sBuf.append( SONG_META_MAGIC, sizeof(SONG_META_MAGIC) );
	PutU32( sBuf, SONG_META_FORMAT );
	PutU32( sBuf, (unsigned) FILE_CACHE_VERSION );
	PutU32( sBuf, m_SongMeta.size() );
	for( map<CString, SongMeta>::const_iterator it = m_SongMeta.begin(); it != m_SongMeta.end(); ++it )
	{
		PutStr( sBuf, it->first );
		PutU32( sBuf, it->second.uCacheHash );
		PutStr( sBuf, it->second.sData );
	}
	PutU32( sBuf, SONG_META_END );

	/* RageFile::WRITE writes to a temporary file and swaps it in on close, so a
	 * crash halfway through never leaves a partial index behind. */
	RageFile f;
	if( !f.Open( SONG_META_FILE, RageFile::WRITE ) )
	{
		LOG->Warn( "Couldn't open song index \"%s\" for writing: %s", SONG_META_FILE, f.GetError().c_str() );
		return;
	}
	if( f.Write( sBuf ) == -1 )
	{
		LOG->Warn( "Couldn't write song index \"%s\": %s", SONG_META_FILE, f.GetError().c_str() );
		return;
	}
	f.Close();

	m_bSongMetaDirty = false;
	LOG->Trace( "Wrote %u entries to song index \"%s\".", (unsigned) m_SongMeta.size(), SONG_META_FILE );
}

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
