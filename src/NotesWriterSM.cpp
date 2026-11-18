#include "global.h"
#include "NotesWriterSM.h"
#include "Steps.h"
#include "RageUtil.h"
#include "GameManager.h"
#include "RageLog.h"
#include "RageFile.h"
#include "RageFileManager.h"
#include "song.h"
#include <cstring>
#include <cerrno>

/* Output is always built into a string: Write() writes it out in one go, and
 * the song index (GetCacheMetaText) uses the string directly.  The line ending
 * is "\r\n" like RageFile::PutLine, so written files are byte-for-byte the same
 * as when they were written line by line. */
static inline void PutLine( CString &f, const CString &str ) { f += str; f += "\r\n"; }

void NotesWriterSM::WriteGlobalTags( CString &f, const Song &out )
{
	PutLine( f, ssprintf( "#TITLE:%s;", out.m_sMainTitle.c_str() ) );
	PutLine( f, ssprintf( "#SUBTITLE:%s;", out.m_sSubTitle.c_str() ) );
	PutLine( f, ssprintf( "#ARTIST:%s;", out.m_sArtist.c_str() ) );
	PutLine( f, ssprintf( "#TITLETRANSLIT:%s;", out.m_sMainTitleTranslit.c_str() ) );
	PutLine( f, ssprintf( "#SUBTITLETRANSLIT:%s;", out.m_sSubTitleTranslit.c_str() ) );
	PutLine( f, ssprintf( "#ARTISTTRANSLIT:%s;", out.m_sArtistTranslit.c_str() ) );
	PutLine( f, ssprintf( "#CREDIT:%s;", out.m_sCredit.c_str() ) );
	PutLine( f, ssprintf( "#BANNER:%s;", out.m_sBannerFile.c_str() ) );
	PutLine( f, ssprintf( "#BACKGROUND:%s;", out.m_sBackgroundFile.c_str() ) );
	PutLine( f, ssprintf( "#LYRICSPATH:%s;", out.m_sLyricsFile.c_str() ) );
	PutLine( f, ssprintf( "#CDTITLE:%s;", out.m_sCDTitleFile.c_str() ) );
	PutLine( f, ssprintf( "#MUSIC:%s;", out.m_sMusicFile.c_str() ) );
	/* Remember which .gn this song came from, so it can still be written back later even
	 * when the song is loaded through the .sm / cache instead. */
	if( out.IsFromGN() )
		PutLine( f, ssprintf( "#GNFILE:%s;", out.m_sGNFileName.c_str() ) );
	PutLine( f, ssprintf( "#OFFSET:%.6f;", out.m_Timing.m_fBeat0OffsetInSeconds ) );
	PutLine( f, ssprintf( "#SAMPLESTART:%.3f;", out.m_fMusicSampleStartSeconds ) );
	PutLine( f, ssprintf( "#SAMPLELENGTH:%.3f;", out.m_fMusicSampleLengthSeconds ) );

	f += "#SELECTABLE:";
	switch(out.m_SelectionDisplay) {
	default: ASSERT(0);  /* fallthrough */
	case Song::SHOW_ALWAYS:
		f += "YES"; break;
	case Song::SHOW_NEVER:
		f += "NO"; break;
	case Song::SHOW_ROULETTE:
		f += "ROULETTE"; break;
	}
	PutLine( f, ";" );

	switch( out.m_DisplayBPMType )
	{
	case Song::DISPLAY_ACTUAL:
		// write nothing
		break;
	case Song::DISPLAY_SPECIFIED:
		if( out.m_fSpecifiedBPMMin == out.m_fSpecifiedBPMMax )
			PutLine( f, ssprintf( "#DISPLAYBPM:%.3f;", out.m_fSpecifiedBPMMin ) );
		else
			PutLine( f, ssprintf( "#DISPLAYBPM:%.3f:%.3f;", out.m_fSpecifiedBPMMin, out.m_fSpecifiedBPMMax ) );
		break;
	case Song::DISPLAY_RANDOM:
		PutLine( f, ssprintf( "#DISPLAYBPM:*;" ) );
		break;
	}


	f += "#BPMS:";
	unsigned i;
	for( i=0; i<out.m_Timing.m_BPMSegments.size(); i++ )
	{
		const BPMSegment &bs = out.m_Timing.m_BPMSegments[i];

		f += ssprintf( "%.3f=%.3f", bs.m_fStartBeat, bs.m_fBPM );
		if( i != out.m_Timing.m_BPMSegments.size()-1 )
			f += ",";
	}
	PutLine( f, ";" );

	f += "#STOPS:";
	for( i=0; i<out.m_Timing.m_StopSegments.size(); i++ )
	{
		const StopSegment &fs = out.m_Timing.m_StopSegments[i];

		// PutLine( f, ssprintf( "%.3f=%.3f", fs.m_fStartBeat, fs.m_fStopSeconds ) );
		//-----------------
		float tmp = fs.m_fStartBeat;
		if((int)(tmp*10000)%10>0)tmp = (tmp*1000/10*10+1)/1000;//exp:1.333->1.334
		// LOG->Info("%f", tmp);
		PutLine( f, ssprintf( "%.3f=%.3f", tmp, fs.m_fStopSeconds ) );
		//-----------------
		if( i != out.m_Timing.m_StopSegments.size()-1 )
			f += ",";
	}
	PutLine( f, ";" );

	f += "#BGCHANGES:";
	for( i=0; i<out.m_BackgroundChanges.size(); i++ )
	{
		const BackgroundChange &seg = out.m_BackgroundChanges[i];

		PutLine( f, ssprintf( "%.3f=%s=%.3f=%d=%d=%d,", seg.m_fStartBeat, seg.m_sBGName.c_str(), seg.m_fRate, seg.m_bFadeLast, seg.m_bRewindMovie, seg.m_bLoop ) );
	}
	/* If there's an animation plan at all, add a dummy "-nosongbg-" tag to indicate that
	 * this file doesn't want a song BG entry added at the end.  See SMLoader::TidyUpData.
	 * This tag will be removed on load.  Add it at a very high beat, so it won't cause
	 * problems if loaded in older versions. */
	if( !out.m_BackgroundChanges.empty() )
		PutLine( f, "99999=-nosongbg-=1.000=0=0=0 // don't automatically add -songbackground-" );
	PutLine( f, ";" );

	if( out.m_ForegroundChanges.size() )
	{
		f += "#FGCHANGES:";
		for( i=0; i<out.m_ForegroundChanges.size(); i++ )
		{
			const BackgroundChange &seg = out.m_ForegroundChanges[i];

			PutLine( f, ssprintf( "%.3f=%s=%.3f=%d=%d=%d", seg.m_fStartBeat, seg.m_sBGName.c_str(), seg.m_fRate, seg.m_bFadeLast, seg.m_bRewindMovie, seg.m_bLoop ) );
			if( i != out.m_ForegroundChanges.size()-1 )
				f += ",";
		}
		PutLine( f, ";" );
	}
}

void NotesWriterSM::WriteCacheTags( CString &f, const Song &out )
{
	PutLine( f, ssprintf( "// cache tags:" ) );
	PutLine( f, ssprintf( "#FIRSTBEAT:%.3f;", out.m_fFirstBeat ) );
	PutLine( f, ssprintf( "#LASTBEAT:%.3f;", out.m_fLastBeat ) );
	PutLine( f, ssprintf( "#SONGFILENAME:%s;", out.m_sSongFileName.c_str() ) );
	PutLine( f, ssprintf( "#HASMUSIC:%i;", out.m_bHasMusic ) );
	PutLine( f, ssprintf( "#HASBANNER:%i;", out.m_bHasBanner ) );
	PutLine( f, ssprintf( "#MUSICLENGTH:%.3f;", out.m_fMusicLengthSeconds ) );
	PutLine( f, ssprintf( "// end cache tags" ) );
}

static void WriteLineList( CString &f, vector<CString> &lines, bool SkipLeadingBlankLines, bool OmitLastNewline )
{
	for( unsigned i = 0; i < lines.size(); ++i )
	{
		TrimRight( lines[i] );
		if( SkipLeadingBlankLines )
		{
			if( lines.size() == 0 )
				continue;
			SkipLeadingBlankLines = false;
		}
		f += lines[i];

		if( !OmitLastNewline || i+1 < lines.size() )
			PutLine( f, "" ); /* newline */
	}
}

/* Each .gn difficulty can have its own BPM table, but a .sm has only one timing for
 * the whole song. So that it is not lost the next time the song is read back from the
 * .sm / cache, the difficulty's own timing is written here, before its #NOTES. Charts
 * in other formats do not have this tag, so their behavior is completely unchanged. */
static void WriteStepsTimingTag( const Steps &in, CString &f )
{
	const TimingData *pTiming = in.GetOwnTiming();
	if( pTiming == NULL )
		return;

	CStringArray asBPMs;
	for( unsigned i = 0; i < pTiming->m_BPMSegments.size(); i++ )
		asBPMs.push_back( ssprintf("%.3f=%.3f",
			pTiming->m_BPMSegments[i].m_fStartBeat, pTiming->m_BPMSegments[i].m_fBPM) );

	CStringArray asStops;
	for( unsigned i = 0; i < pTiming->m_StopSegments.size(); i++ )
		asStops.push_back( ssprintf("%.3f=%.3f",
			pTiming->m_StopSegments[i].m_fStartBeat, pTiming->m_StopSegments[i].m_fStopSeconds) );

	PutLine( f, "" );
	PutLine( f, ssprintf( "#STEPSTIMING:%s:%s:%.6f:%s:%s;",
		GameManager::StepsTypeToString(in.m_StepsType).c_str(),
		DifficultyToString(in.GetDifficulty()).c_str(),
		pTiming->m_fBeat0OffsetInSeconds,
		join(",",asBPMs).c_str(),
		join(",",asStops).c_str() ) );
}

void NotesWriterSM::WriteSMNotesTag( const Steps &in, CString &f, bool bSavingCache, bool bOmitNoteData )
{
	WriteStepsTimingTag( in, f );

	/* The song index has no note data; store the hash of the cache file's notes
	 * instead.  Steps::Decompress reads the notes back from the cache file when
	 * they're needed, and EDIT steps are matched by this hash. */
	if( bOmitNoteData )
		PutLine( f, ssprintf( "#STEPSHASH:%u;", in.GetHash() ) );

	PutLine( f, "" );
	if( !bOmitNoteData )
		PutLine( f, ssprintf( "//---------------%s - %s----------------",
			GameManager::StepsTypeToString(in.m_StepsType).c_str(), in.GetDescription().c_str() ) );
	PutLine( f, "#NOTES:" );
	PutLine( f, ssprintf( "     %s:", GameManager::StepsTypeToString(in.m_StepsType).c_str() ) );
	PutLine( f, ssprintf( "     %s:", in.GetDescription().c_str() ) );
	PutLine( f, ssprintf( "     %s:", DifficultyToString(in.GetDifficulty()).c_str() ) );
	PutLine( f, ssprintf( "     %d:", in.GetMeter() ) );

	int MaxRadar = bSavingCache? NUM_RADAR_CATEGORIES:5;
	CStringArray asRadarValues;
	for( int r=0; r < MaxRadar; r++ )
		asRadarValues.push_back( ssprintf("%.3f", in.GetRadarValues()[r]) );
	/* Don't append a newline here; it's added in NoteDataUtil::GetSMNoteDataString.
	 * If we add it here, then every time we write unmodified data we'll add an extra
	 * newline and they'll accumulate. */
	f += ssprintf( "     %s:", join(",",asRadarValues).c_str() );

	if( bOmitNoteData )
	{
		/* Leave an empty note data field so #NOTES has enough fields when read back. */
		PutLine( f, ";" );
		return;
	}

	CString sNoteData;
	CString sAttackData;
	in.GetSMNoteData( sNoteData, sAttackData );

	vector<CString> lines;

	split( sNoteData, "\n", lines, false );
	WriteLineList( f, lines, true, true );

	if( sAttackData.empty() )
		PutLine( f, ";" );
	else
	{
		PutLine( f, ":" );

		lines.clear();
		split( sAttackData, "\n", lines, false );
		WriteLineList( f, lines, true, true );

		PutLine( f, ";" );
	}
}

void NotesWriterSM::WriteAllSteps( CString &f, const Song &out, bool bSavingCache, bool bOmitNoteData )
{
	//
	// Save all Steps for this file
	//
	const vector<Steps*>& vpSteps = out.GetAllSteps();
	for( unsigned i=0; i<vpSteps.size(); i++ )
	{
		const Steps* pSteps = vpSteps[i];
		if( pSteps->IsAutogen() )
			continue; /* don't write autogen notes */

		/* Only save steps that weren't loaded from a profile. */
		if( pSteps->WasLoadedFromProfile() )
			continue;

		WriteSMNotesTag( *pSteps, f, bSavingCache, bOmitNoteData );
	}
}

bool NotesWriterSM::Write(CString sPath, const Song &out, bool bSavingCache)
{
	/* Flush dir cache when writing steps, so the old size isn't cached. */
	FILEMAN->FlushDirCache( Dirname(sPath) );

	int flags = RageFile::WRITE;

	/* If we're not saving cache, we're saving real data, so enable SLOW_FLUSH
	 * to prevent data loss.  If we're saving cache, this will slow things down
	 * too much. */
	if( !bSavingCache )
		flags |= RageFile::SLOW_FLUSH;

	RageFile f;
	if( !f.Open( sPath, flags ) )
	{
		LOG->Warn( "Error opening song file '%s' for writing: %s", sPath.c_str(), f.GetError().c_str() );
		return false;
	}

	CString sOut;
	WriteGlobalTags( sOut, out );
	if( bSavingCache )
		WriteCacheTags( sOut, out );
	WriteAllSteps( sOut, out, bSavingCache, false );

	if( f.Write( sOut ) == -1 )
	{
		LOG->Warn( "Error writing song file '%s': %s", sPath.c_str(), f.GetError().c_str() );
		return false;
	}

	return true;
}

CString NotesWriterSM::GetCacheMetaText( const Song &out )
{
	NotesWriterSM writer;
	CString sOut;
	writer.WriteGlobalTags( sOut, out );
	writer.WriteCacheTags( sOut, out );
	writer.WriteAllSteps( sOut, out, true, true );
	return sOut;
}

/*
 * (c) 2001-2004 Chris Danford, Glenn Maynard
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
