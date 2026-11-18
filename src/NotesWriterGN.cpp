#include "global.h"
#include "NotesWriterGN.h"
#include "NotesLoaderGN.h"
#include "song.h"
#include "Steps.h"
#include "NoteData.h"
#include "NoteTypes.h"
#include "TimingData.h"
#include "RageFile.h"
#include "RageLog.h"
#include "RageUtil.h"

#include <map>

static const int GN_ROWS_PER_MEASUREMENT = ROWS_PER_BEAT * GNFile::BEATS_PER_MEASUREMENT;

/* The intervals GN allows. Smaller means a smaller file, so pick the smallest one that
 * still fits all the notes. */
static const int g_aiIntervals[] = { 4, 8, 12, 16, 24, 32, 48, 64, 96, 192 };

/* Each (measurement, type) maps to one frame; slots are collected by row within the
 * measure first, and the interval is decided at the end. */
typedef std::map<int, GNFile::Slot> SlotMap;			// row within the measure (0..191) -> slot
typedef std::map<std::pair<int,int>, SlotMap> FrameMap;	// (measurement, type) -> slots

static void AddSlot( FrameMap &m, int iRow, int iType, const GNFile::Slot &slot )
{
	if( iRow < 0 )
		return;
	const int iMeasurement = iRow / GN_ROWS_PER_MEASUREMENT;
	const int iRowIn = iRow % GN_ROWS_PER_MEASUREMENT;
	m[std::make_pair(iMeasurement, iType)][iRowIn] = slot;
}

/* Find the smallest interval that can hold these rows. */
static int PickInterval( const SlotMap &slots )
{
	for( unsigned i = 0; i < RageARRAYSIZE(g_aiIntervals); i++ )
	{
		const int iInterval = g_aiIntervals[i];
		const int iStep = GN_ROWS_PER_MEASUREMENT / iInterval;
		bool bFits = true;
		for( SlotMap::const_iterator it = slots.begin(); it != slots.end(); ++it )
		{
			if( it->first % iStep != 0 )
			{
				bFits = false;
				break;
			}
		}
		if( bFits )
			return iInterval;
	}
	return GNFile::MAX_INTERVAL;
}

void NotesWriterGN::BuildFrames( const NoteData &nd, const TimingData &timing, float fHeaderBPM,
	const vector<GNFile::Frame> &vOriginal, vector<GNFile::Frame> &vOut )
{
	FrameMap mapFrames;

	// ---- Notes ----
	const int iLastRow = nd.GetLastRow();
	for( int t = 0; t < nd.GetNumTracks() && t < 4; t++ )
	{
		const int iType = GNLoader::ColToFrameType( t );
		for( int r = 0; r <= iLastRow; r++ )
		{
			const TapNote tn = nd.GetTapNote( t, r );
			if( tn.type == TapNote::empty || tn.type == TapNote::mine )
				continue;	// GN has no mines
			AddSlot( mapFrames, r, iType, GNFile::Slot(1, 0, GNFile::NOTE_ARROW) );
		}
	}

	for( int h = 0; h < nd.GetNumHoldNotes(); h++ )
	{
		const HoldNote &hn = nd.GetHoldNote( h );
		if( hn.iTrack < 0 || hn.iTrack >= 4 )
			continue;
		const int iType = GNLoader::ColToFrameType( hn.iTrack );
		AddSlot( mapFrames, hn.iStartRow, iType, GNFile::Slot(1, 0, GNFile::NOTE_HOLD_START) );
		AddSlot( mapFrames, hn.iEndRow,   iType, GNFile::Slot(1, 0, GNFile::NOTE_HOLD_END) );
	}

	// ---- BPM changes ----
	/* The BPM at beat 0 is given by the header; write a separate type 1 only when it
	 * differs from the header. */
	for( unsigned i = 0; i < timing.m_BPMSegments.size(); i++ )
	{
		const BPMSegment &seg = timing.m_BPMSegments[i];
		if( seg.m_fBPM <= 0 )
			continue;
		if( seg.m_fStartBeat <= 0 && fabsf(seg.m_fBPM - fHeaderBPM) < 0.001f )
			continue;
		const int iRow = BeatToNoteRow( max(0.f, seg.m_fStartBeat) );
		AddSlot( mapFrames, iRow, GNFile::FRAME_BPM, GNFile::BPMToSlot(seg.m_fBPM) );
	}

	// ---- Control frames (bar lines, music start/end, ...) are kept as-is ----
	int iMaxBarlineMeasurement = -1;
	int iBarlineStep = 2;		// spacing between two adjacent bar lines in the original file
	int iNextBarlineValue = -1;	// bar line numbers in the original file increase
	for( unsigned f = 0; f < vOriginal.size(); f++ )
	{
		const GNFile::Frame &fr = vOriginal[f];
		if( fr.iType == GNFile::FRAME_BPM || GNLoader::FrameTypeToCol(fr.iType) >= 0 )
			continue;	// notes and BPMs are rebuilt above

		vOut.push_back( fr );

		if( fr.iType == GNFile::FRAME_BARLINE )
		{
			if( iMaxBarlineMeasurement >= 0 )
				iBarlineStep = max( 1, fr.iMeasurement - iMaxBarlineMeasurement );
			iMaxBarlineMeasurement = fr.iMeasurement;
			for( unsigned i = 0; i < fr.vSlots.size(); i++ )
				if( !fr.vSlots[i].IsEmpty() )
					iNextBarlineValue = fr.vSlots[i].u0 + 1;
		}
	}

	/* If the chart was made longer, add bar lines at the original spacing. */
	int iMaxMeasurement = 0;
	for( FrameMap::const_iterator it = mapFrames.begin(); it != mapFrames.end(); ++it )
		iMaxMeasurement = max( iMaxMeasurement, it->first.first );

	if( iMaxBarlineMeasurement >= 0 && iNextBarlineValue > 0 )
	{
		for( int m = iMaxBarlineMeasurement + iBarlineStep; m <= iMaxMeasurement; m += iBarlineStep )
		{
			GNFile::Frame fr( m, GNFile::FRAME_BARLINE, 4 );
			fr.vSlots[0] = GNFile::Slot( (int16_t) iNextBarlineValue, 0, 0 );
			iNextBarlineValue++;
			vOut.push_back( fr );
		}
	}

	// ---- Turn the collected slots into frames ----
	for( FrameMap::const_iterator it = mapFrames.begin(); it != mapFrames.end(); ++it )
	{
		const int iMeasurement = it->first.first;
		const int iType = it->first.second;
		const SlotMap &slots = it->second;
		const int iInterval = PickInterval( slots );
		const int iStep = GN_ROWS_PER_MEASUREMENT / iInterval;

		GNFile::Frame fr( iMeasurement, (int16_t) iType, iInterval );
		for( SlotMap::const_iterator s = slots.begin(); s != slots.end(); ++s )
		{
			const int iIdx = s->first / iStep;
			if( iIdx >= 0 && iIdx < iInterval )
				fr.vSlots[iIdx] = s->second;
		}
		vOut.push_back( fr );
	}

	// ---- The original file is ordered by (measurement, type), ascending ----
	struct Sorter
	{
		static bool Less( const GNFile::Frame &a, const GNFile::Frame &b )
		{
			if( a.iMeasurement != b.iMeasurement )
				return a.iMeasurement < b.iMeasurement;
			return a.iType < b.iType;
		}
	};
	stable_sort( vOut.begin(), vOut.end(), Sorter::Less );
}

static int CountNotesInFrames( const vector<GNFile::Frame> &v )
{
	int iCount = 0;
	for( unsigned f = 0; f < v.size(); f++ )
	{
		if( GNLoader::FrameTypeToCol(v[f].iType) < 0 )
			continue;
		for( unsigned i = 0; i < v[f].vSlots.size(); i++ )
			if( v[f].vSlots[i].u0 != 0 )
				iCount++;
	}
	return iCount;
}

static bool WriteWholeFile( CString sPath, const std::string &s )
{
	RageFile f;
	if( !f.Open( sPath, RageFile::WRITE ) )
		return false;
	if( f.Write( s.data(), s.size() ) != (int) s.size() )
		return false;
	/* Flush() returns the number of bytes it wrote (0 only if the buffer was
	 * already empty) and -1 on error.  Checking for == 0 reported almost every
	 * successful write as "could not write the file". */
	return f.Flush() != -1;
}

bool NotesWriterGN::Write( CString sPath, const Song &song, bool bKeepFileSize, CString &sErrOut )
{
	/* Use the original file as the skeleton: uninterpreted header fields and control
	 * frames are kept. */
	GNFile::StepFile sf;
	GNFile::ContainerInfo info;
	if( !GNLoader::ReadGNFile( sPath, sf, info, sErrOut ) )
		return false;

	static const Difficulty aDiffs[GNFile::NUM_DIFFS] =
	{
		DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD
	};

	/* The header BPM takes the starting BPM of the hardest difficulty. */
	for( int d = GNFile::NUM_DIFFS - 1; d >= 0; d-- )
	{
		const Steps *pSteps = song.GetStepsByDifficulty( STEPS_TYPE_DANCE_SINGLE, aDiffs[d], false );
		if( pSteps == NULL )
			continue;
		const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;
		if( !pTiming->m_BPMSegments.empty() )
		{
			sf.fBPM = pTiming->m_BPMSegments[0].m_fBPM;
			break;
		}
	}

	for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
	{
		const Steps *pSteps = song.GetStepsByDifficulty( STEPS_TYPE_DANCE_SINGLE, aDiffs[d], false );
		if( pSteps == NULL )
			continue;	// this difficulty was not loaded (it is empty in the .gn); keep it as-is

		NoteData nd;
		pSteps->GetNoteData( &nd );

		const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;

		vector<GNFile::Frame> vNew;
		BuildFrames( nd, *pTiming, sf.fBPM, sf.avFrames[d], vNew );
		sf.avFrames[d] = vNew;

		sf.aiNoteCount[d] = CountNotesInFrames( vNew );
		sf.aiMeasurements[d] = (int) vNew.size();

		/* Level: GN's level and SM's meter are the same number, so write it back if it was
		 * changed in the editor. Leave it alone if the header originally had none (0), so
		 * that a number guessed by Steps::TidyUpData does not end up in the file. */
		if( sf.aiLevel[d] > 0 && pSteps->GetMeter() > 0 )
			sf.aiLevel[d] = (int16_t) GNLoader::MeterToGNLevel( pSteps->GetMeter() );

		/* The exact meaning of aiExtra52 / aiDuration is not fully understood yet (testing
		 * shows extra52 is neither the highest measure nor the measure count); writing
		 * arbitrary values could make the original game compute the wrong speed, so they
		 * are kept as-is. */
	}

	std::string sBody;
	GNFile::Serialize( sf, sBody );

	std::string sRaw, sErr;
	if( !GNFile::Encrypt( sBody, info, bKeepFileSize, sRaw, &sErr ) )
	{
		sErrOut = sErr.c_str();
		return false;
	}

	if( !WriteWholeFile( sPath, sRaw ) )
	{
		sErrOut = "could not write the file";
		return false;
	}

	LOG->Trace( "Wrote .gn: %s (%u bytes, original was %u bytes)",
		sPath.c_str(), (unsigned) sRaw.size(), (unsigned) info.iOrigFileSize );
	return true;
}

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
