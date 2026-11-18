#include "global.h"
#include "NotesLoaderGN.h"
#include "RageFile.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "RageUtil_CharConversions.h"
#include "NoteData.h"
#include "NoteTypes.h"
#include "GameManager.h"

/* A GN measurement is 4 beats and SM has 48 rows per beat, which lines up exactly with
 * GN's finest division of 192 slots per measure. */
static const int GN_ROWS_PER_MEASUREMENT = ROWS_PER_BEAT * GNFile::BEATS_PER_MEASUREMENT;

int GNLoader::FrameTypeToCol( int iFrameType )
{
	switch( iFrameType )
	{
	case GNFile::FRAME_LEFT:	return 0;
	case GNFile::FRAME_DOWN:	return 1;
	case GNFile::FRAME_UP:		return 2;
	case GNFile::FRAME_RIGHT:	return 3;
	default:					return -1;
	}
}

int GNLoader::ColToFrameType( int iCol )
{
	switch( iCol )
	{
	case 0:		return GNFile::FRAME_LEFT;
	case 1:		return GNFile::FRAME_DOWN;
	case 2:		return GNFile::FRAME_UP;
	case 3:		return GNFile::FRAME_RIGHT;
	default:	return -1;
	}
}

/* GN's level is the chart's difficulty level itself and maps one-to-one to SM's meter
 * (the tool that injects a .sm into a .gn also writes the meter straight into level; see
 * gn_master_core.py). SDO levels can reach 30 or 40, far beyond SM's MAX_METER; we do not
 * compress them here, otherwise the difficulty shown on screen would not match the
 * original chart. */
const int GN_LEVEL_MAX = 99;

int GNLoader::GNLevelToMeter( int iLevel )
{
	if( iLevel <= 0 )
		return 0;	// not set in the header; let Steps::TidyUpData guess
	return min( iLevel, GN_LEVEL_MAX );
}

int GNLoader::MeterToGNLevel( int iMeter )
{
	return clamp( iMeter, 0, GN_LEVEL_MAX );
}

float GNLoader::GetSecondsFromBeat( const TimingData &timing, float fBeat )
{
	if( fBeat <= 0 || timing.m_BPMSegments.empty() )
		return 0;

	float fSeconds = 0;
	for( unsigned i = 0; i < timing.m_BPMSegments.size(); i++ )
	{
		const float fBPM = timing.m_BPMSegments[i].m_fBPM;
		if( fBPM <= 0 )
			continue;
		const float fStart = timing.m_BPMSegments[i].m_fStartBeat;
		const bool bLast = ( i + 1 == timing.m_BPMSegments.size() );
		const float fNext = bLast ? fBeat : timing.m_BPMSegments[i+1].m_fStartBeat;
		if( fStart >= fBeat )
			break;
		const float fEnd = min( fNext, fBeat );
		fSeconds += (fEnd - fStart) * 60.0f / fBPM;
		if( fEnd >= fBeat )
			break;
	}
	return fSeconds;
}

void GNLoader::FramesToTimingData( const vector<GNFile::Frame> &vFrames, float fHeaderBPM, TimingData &out )
{
	out.m_BPMSegments.clear();
	out.m_StopSegments.clear();
	out.m_fBeat0OffsetInSeconds = 0;

	/* The base BPM comes from the header; if the header has none, use the first BPM change. */
	float fBaseBPM = fHeaderBPM;
	if( fBaseBPM <= 0 )
	{
		for( unsigned f = 0; f < vFrames.size() && fBaseBPM <= 0; f++ )
		{
			const GNFile::Frame &fr = vFrames[f];
			if( fr.iType != GNFile::FRAME_BPM )
				continue;
			for( unsigned i = 0; i < fr.vSlots.size(); i++ )
			{
				if( fr.vSlots[i].IsEmpty() )
					continue;
				const float fBPM = GNFile::SlotToBPM( fr.vSlots[i] );
				if( fBPM > 0 )
				{
					fBaseBPM = fBPM;
					break;
				}
			}
		}
	}
	if( fBaseBPM <= 0 )
		fBaseBPM = 120;

	out.AddBPMSegment( BPMSegment(0, fBaseBPM) );

	/* Every non-empty type 1 slot is a BPM change. */
	for( unsigned f = 0; f < vFrames.size(); f++ )
	{
		const GNFile::Frame &fr = vFrames[f];
		if( fr.iType != GNFile::FRAME_BPM )
			continue;
		for( unsigned i = 0; i < fr.vSlots.size(); i++ )
		{
			if( fr.vSlots[i].IsEmpty() )
				continue;
			const float fBPM = GNFile::SlotToBPM( fr.vSlots[i] );
			if( fBPM <= 0 || fBPM > 5000 )
				continue;	// not a plausible BPM; most likely we misread the slot
			const float fBeat = max( 0.f, fr.GetSlotBeat(i) );
			/* Many charts mark the header BPM again at the start; that is not a BPM change,
			 * so don't leave a redundant segment. */
			if( fabsf(out.GetBPMAtBeat(fBeat) - fBPM) < 0.001f )
				continue;
			out.SetBPMAtBeat( fBeat, fBPM );
		}
	}

	/* Second 0 of the music lines up with the type-10 beat; convert that into SM's
	 * beat 0 offset. */
	float fMusicStartBeat = -1;
	for( unsigned f = 0; f < vFrames.size(); f++ )
	{
		const GNFile::Frame &fr = vFrames[f];
		if( fr.iType != GNFile::FRAME_MUSIC )
			continue;
		int iSlot = 0;
		for( unsigned i = 0; i < fr.vSlots.size(); i++ )
		{
			if( !fr.vSlots[i].IsEmpty() )
			{
				iSlot = i;
				break;
			}
		}
		const float fBeat = fr.GetSlotBeat( iSlot );
		if( fMusicStartBeat < 0 || fBeat < fMusicStartBeat )
			fMusicStartBeat = fBeat;
	}
	if( fMusicStartBeat > 0 )
		out.m_fBeat0OffsetInSeconds = GetSecondsFromBeat( out, fMusicStartBeat );
}

void GNLoader::FramesToNoteData( const vector<GNFile::Frame> &vFrames, NoteData &out )
{
	out.ClearAll();
	out.SetNumTracks( 4 );

	int aiHoldStartRow[4];
	for( int i = 0; i < 4; i++ )
		aiHoldStartRow[i] = -1;

	vector<HoldNote> vHolds;

	for( unsigned f = 0; f < vFrames.size(); f++ )
	{
		const GNFile::Frame &fr = vFrames[f];
		const int iCol = FrameTypeToCol( fr.iType );
		if( iCol < 0 )
			continue;

		const int iInterval = fr.GetInterval();
		if( iInterval <= 0 )
			continue;

		for( int i = 0; i < iInterval; i++ )
		{
			const GNFile::Slot &sl = fr.vSlots[i];
			if( sl.u0 == 0 )
				continue;	// empty slot

			const int iRow = fr.iMeasurement * GN_ROWS_PER_MEASUREMENT +
				(int) roundf( GN_ROWS_PER_MEASUREMENT * i / (float) iInterval );
			if( iRow < 0 )
				continue;

			switch( sl.nt )
			{
			case GNFile::NOTE_HOLD_START:
				/* If the previous hold was never closed, treat it as a single tap. */
				if( aiHoldStartRow[iCol] != -1 )
					out.SetTapNote( iCol, aiHoldStartRow[iCol], TAP_ORIGINAL_TAP );
				aiHoldStartRow[iCol] = iRow;
				break;

			case GNFile::NOTE_HOLD_END:
				if( aiHoldStartRow[iCol] != -1 && iRow > aiHoldStartRow[iCol] )
				{
					vHolds.push_back( HoldNote(iCol, aiHoldStartRow[iCol], iRow) );
					aiHoldStartRow[iCol] = -1;
				}
				else if( aiHoldStartRow[iCol] != -1 )
				{
					/* Zero-length hold; degrade it to a single tap. */
					out.SetTapNote( iCol, aiHoldStartRow[iCol], TAP_ORIGINAL_TAP );
					aiHoldStartRow[iCol] = -1;
				}
				else
				{
					/* A few charts have hold ends with no start (that is how the original
					 * data is). SM has no way to represent this, so make it a single tap;
					 * better than having the note vanish entirely. */
					out.SetTapNote( iCol, iRow, TAP_ORIGINAL_TAP );
				}
				break;

			default:
				out.SetTapNote( iCol, iRow, TAP_ORIGINAL_TAP );
				break;
			}
		}
	}

	/* Holds that were never closed always become single taps, so the whole note does not
	 * disappear. */
	for( int c = 0; c < 4; c++ )
		if( aiHoldStartRow[c] != -1 )
			out.SetTapNote( c, aiHoldStartRow[c], TAP_ORIGINAL_TAP );

	for( unsigned i = 0; i < vHolds.size(); i++ )
		out.AddHoldNote( vHolds[i] );
}

CString GNLoader::PickMainGNFile( CString sDir )
{
	CStringArray as;
	GetDirListing( sDir + CString("*.gn"), as );
	if( as.empty() )
		return "";

	SortCStringArray( as );

	/* A song often comes as a pair, xxxK.gn / xxxT.gn; K is the single-player chart we want. */
	for( unsigned i = 0; i < as.size(); i++ )
	{
		CString sStem = SetExtension( as[i], "" );
		if( sStem.empty() )
			continue;
		const char c = sStem[sStem.size()-1];
		if( c == 'k' || c == 'K' )
			return as[i];
	}
	return as[0];
}

void GNLoader::GetApplicableFiles( CString sPath, CStringArray &out )
{
	GetDirListing( sPath + CString("*.gn"), out );
}

static bool ReadWholeFile( CString sPath, std::string &sOut )
{
	RageFile f;
	if( !f.Open( sPath ) )
		return false;
	const int iSize = f.GetFileSize();
	if( iSize <= 0 )
		return false;
	sOut.resize( (size_t) iSize );
	return f.Read( &sOut[0], (size_t) iSize ) == iSize;
}

bool GNLoader::ReadGNFile( CString sPath, GNFile::StepFile &sfOut, GNFile::ContainerInfo &infoOut, CString &sErrOut )
{
	std::string sRaw;
	if( !ReadWholeFile( sPath, sRaw ) )
	{
		sErrOut = "could not read the file";
		return false;
	}

	std::string sBody, sErr;
	if( !GNFile::Decrypt( sRaw, sBody, infoOut, &sErr ) )
	{
		sErrOut = sErr.c_str();
		return false;
	}
	if( !GNFile::Parse( sBody, sfOut, &sErr ) )
	{
		sErrOut = sErr.c_str();
		return false;
	}
	return true;
}

/* .gn header strings do not declare their encoding; in practice they are mostly GBK, and
 * a few Taiwanese releases use Big5. */
static CString GNStringToUTF8( const std::string &s )
{
	CString sOut = s.c_str();
	if( sOut.empty() )
		return sOut;
	TrimRight( sOut );
	ConvertString( sOut, "utf-8,chinese,big5,japanese,english" );
	return sOut;
}

/* Find this song's music file: first by the file name in the header, then by the .gn
 * file name (with the trailing K/T removed), and as a last resort any music file in the
 * folder. */
static CString FindMusicFile( CString sDir, CString sGNFileName, const GNFile::StepFile &sf )
{
	CStringArray asStems;

	CString sHeaderName = sf.GetFileName().c_str();
	if( !sHeaderName.empty() )
		asStems.push_back( SetExtension(sHeaderName, "") );
	asStems.push_back( SetExtension(sGNFileName, "") );

	/* sdom2956k -> sdom2956 */
	for( int i = (int) asStems.size() - 1; i >= 0; i-- )
	{
		CString s = asStems[i];
		if( s.empty() )
			continue;
		const char c = s[s.size()-1];
		if( c == 'k' || c == 'K' || c == 't' || c == 'T' )
			asStems.push_back( s.Left(s.size()-1) );
	}

	const char *aszExt[] = { "ogg", "mp3", "wav" };
	for( unsigned e = 0; e < RageARRAYSIZE(aszExt); e++ )
	{
		for( unsigned i = 0; i < asStems.size(); i++ )
		{
			CStringArray as;
			GetDirListing( sDir + asStems[i] + "." + aszExt[e], as );
			if( !as.empty() )
				return as[0];
		}
	}

	for( unsigned e = 0; e < RageARRAYSIZE(aszExt); e++ )
	{
		CStringArray as;
		GetDirListing( sDir + CString("*.") + aszExt[e], as );
		if( !as.empty() )
			return as[0];
	}
	return "";
}

static int CountNotes( const vector<GNFile::Frame> &vFrames )
{
	int iCount = 0;
	for( unsigned f = 0; f < vFrames.size(); f++ )
	{
		const GNFile::Frame &fr = vFrames[f];
		if( GNLoader::FrameTypeToCol(fr.iType) < 0 )
			continue;
		for( unsigned i = 0; i < fr.vSlots.size(); i++ )
			if( fr.vSlots[i].u0 != 0 )
				iCount++;
	}
	return iCount;
}

bool GNLoader::LoadFromDir( CString sDir, Song &out )
{
	LOG->Trace( "Song::LoadFromGNDir(%s)", sDir.c_str() );

	const CString sGNFileName = PickMainGNFile( sDir );
	if( sGNFileName.empty() )
	{
		LOG->Warn( "No .gn file found in '%s'", sDir.c_str() );
		return false;
	}

	GNFile::StepFile sf;
	GNFile::ContainerInfo info;
	CString sErr;
	if( !ReadGNFile( sDir + sGNFileName, sf, info, sErr ) )
	{
		LOG->Warn( "Couldn't load '%s%s': %s", sDir.c_str(), sGNFileName.c_str(), sErr.c_str() );
		return false;
	}

	out.m_sGNFileName = sGNFileName;

	CString sTitle = GNStringToUTF8( sf.GetTitle() );
	if( sTitle.empty() )
		sTitle = SetExtension( sGNFileName, "" );
	NotesLoader::GetMainAndSubTitlesFromFullTitle( sTitle, out.m_sMainTitle, out.m_sSubTitle );
	out.m_sArtist = GNStringToUTF8( sf.GetWriter() );
	out.m_sCredit = GNStringToUTF8( sf.GetProducer() );

	out.m_sMusicFile = FindMusicFile( sDir, sGNFileName, sf );

	/* Build a Steps for each of the three difficulties, each carrying its own BPM table. */
	static const Difficulty aDiffs[GNFile::NUM_DIFFS] =
	{
		DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD
	};
	static const char *aszDiffNames[GNFile::NUM_DIFFS] = { "Easy", "Normal", "Hard" };

	int iStepsLoaded = 0;
	int iSongTimingFrom = -1;
	for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
	{
		const vector<GNFile::Frame> &vFrames = sf.avFrames[d];
		if( CountNotes(vFrames) == 0 )
			continue;	// this difficulty is empty (many files only contain Hard)

		TimingData timing;
		FramesToTimingData( vFrames, sf.fBPM, timing );

		NoteData nd;
		FramesToNoteData( vFrames, nd );

		Steps *pSteps = new Steps;
		pSteps->m_StepsType = STEPS_TYPE_DANCE_SINGLE;
		pSteps->SetDifficulty( aDiffs[d] );
		pSteps->SetMeter( GNLevelToMeter(sf.aiLevel[d]) );
		pSteps->SetDescription( aszDiffNames[d] );
		pSteps->SetNoteData( &nd );
		pSteps->SetOwnTiming( timing );
		pSteps->TidyUpData();
		out.AddSteps( pSteps );

		/* The Song's own timing comes from the hardest difficulty; whichever difficulty is
		 * played then switches to its own. */
		if( iSongTimingFrom < d )
		{
			out.m_Timing = timing;
			iSongTimingFrom = d;
		}
		iStepsLoaded++;
	}

	if( iStepsLoaded == 0 )
	{
		LOG->Warn( "'%s%s' has no notes in any difficulty", sDir.c_str(), sGNFileName.c_str() );
		return false;
	}

	if( out.m_Timing.m_BPMSegments.empty() )
		out.m_Timing.AddBPMSegment( BPMSegment(0, sf.fBPM > 0 ? sf.fBPM : 120) );

	return true;
}

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
