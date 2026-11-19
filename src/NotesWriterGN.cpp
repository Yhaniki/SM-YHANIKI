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
#include <cmath>

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

/* How the game (sdo_stand_alone.exe) uses the per-difficulty header fields, from
 * CNewNote::LoadNote and the gameplay screen (see GN_FORMAT.md):
 *   note count  the size of the per-note state; the official files count non-empty
 *               note slots (u0 != 0) of frame types 2..8.
 *   extra52     the song's last measurement number.  The note lookup tables hold
 *               extra52+1 measurements and are filled by each frame's measurement with
 *               no bounds check, so it must be at least the highest frame measurement;
 *               and the chart isn't loaded at all unless it is below 501.
 *   duration    the song length in seconds.  The dancers' beat is
 *               duration * 1000 / (extra52 * beats per measure), and at the start two
 *               random times are drawn with "rand() % duration" until they reach 21 and
 *               41, so it has to be above 41 or the game hangs. */
static const int GN_MAX_EXTRA52 = 500;
static const int GN_MIN_DURATION = 42;

/* The header note count, counted the way the official files are.  This engine only plays
 * types 2..5, but 6-key charts also use 6 and 7; those frames are kept as they are. */
static int CountNotesInFrames( const vector<GNFile::Frame> &v )
{
	int iCount = 0;
	for( unsigned f = 0; f < v.size(); f++ )
	{
		if( v[f].iType < 2 || v[f].iType > 8 )
			continue;
		for( unsigned i = 0; i < v[f].vSlots.size(); i++ )
			if( v[f].vSlots[i].u0 != 0 )
				iCount++;
	}
	return iCount;
}

/* The highest measurement of any frame, or -1 if there are none. */
static int MaxMeasurement( const vector<GNFile::Frame> &v )
{
	int iMax = -1;
	for( unsigned f = 0; f < v.size(); f++ )
		iMax = max( iMax, (int) v[f].iMeasurement );
	return iMax;
}

/* Seconds from beat 0 to fBeat, from the header BPM and the type-1 frames: the timing
 * the game itself reads from the file (no offset, no stops). */
static double SecondsAtBeat( const vector<GNFile::Frame> &v, float fHeaderBPM, double fBeat )
{
	struct Change
	{
		double fBeat, fBPM;
		static bool Less( const Change &a, const Change &b ) { return a.fBeat < b.fBeat; }
	};
	vector<Change> vChanges;
	Change first = { 0.0, fHeaderBPM > 0 ? fHeaderBPM : 120.0 };
	vChanges.push_back( first );
	for( unsigned f = 0; f < v.size(); f++ )
	{
		if( v[f].iType != GNFile::FRAME_BPM )
			continue;
		for( unsigned i = 0; i < v[f].vSlots.size(); i++ )
		{
			if( v[f].vSlots[i].IsEmpty() )
				continue;
			const float fBPM = GNFile::SlotToBPM( v[f].vSlots[i] );
			if( fBPM <= 0 || fBPM > 5000 )
				continue;	// same plausibility check as the loader
			Change c = { max( 0.0, (double) v[f].GetSlotBeat(i) ), fBPM };
			vChanges.push_back( c );
		}
	}
	/* Stable, so of two changes on the same beat the later one in the file wins. */
	stable_sort( vChanges.begin(), vChanges.end(), Change::Less );

	double fSeconds = 0;
	for( unsigned i = 0; i < vChanges.size() && vChanges[i].fBeat < fBeat; i++ )
	{
		const double fEnd = (i + 1 < vChanges.size()) ? min( vChanges[i+1].fBeat, fBeat ) : fBeat;
		fSeconds += (fEnd - vChanges[i].fBeat) * 60.0 / vChanges[i].fBPM;
	}
	return fSeconds;
}

/* Whole seconds from beat 0 to the end of the last measurement of these frames, the way
 * the official files store the song length; 0 if there are no frames. */
static int SongSeconds( const vector<GNFile::Frame> &v, float fHeaderBPM )
{
	const int iMax = MaxMeasurement( v );
	if( iMax < 0 )
		return 0;
	const double fEndBeat = (iMax + 1) * (double) GNFile::BEATS_PER_MEASUREMENT;
	return (int) floor( SecondsAtBeat( v, fHeaderBPM, fEndBeat ) );
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
	 * frames are kept, and the per-difficulty header values follow the new frames. */
	GNFile::StepFile sf;
	GNFile::ContainerInfo info;
	if( !GNLoader::ReadGNFile( sPath, sf, info, sErrOut ) )
		return false;

	static const Difficulty aDiffs[GNFile::NUM_DIFFS] =
	{
		DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD
	};

	/* Every difficulty the song has is written.  One whose slot in the original .gn has no
	 * notes (many files only contain Hard) is new: its slot holds no usable control frames
	 * or header values, so both come from another difficulty below. */
	bool abWrite[GNFile::NUM_DIFFS], abNew[GNFile::NUM_DIFFS];
	for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
	{
		const Steps *pSteps = song.GetStepsByDifficulty( STEPS_TYPE_DANCE_SINGLE, aDiffs[d], false );
		abWrite[d] = pSteps != NULL;
		abNew[d] = CountNotesInFrames( sf.avFrames[d] ) == 0;

		/* A new chart that has no notes yet leaves the empty slot as it was, rather than
		 * turning it into a selectable but empty difficulty. */
		if( abWrite[d] && abNew[d] )
		{
			NoteData nd;
			pSteps->GetNoteData( &nd );
			const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;
			vector<GNFile::Frame> vNoSkeleton, vNotes;
			BuildFrames( nd, *pTiming, sf.fBPM, vNoSkeleton, vNotes );
			abWrite[d] = CountNotesInFrames( vNotes ) > 0;
		}
	}

	/* A new difficulty takes its control frames (bar lines, music markers, ...) from the
	 * hardest difficulty that has a chart in the original.  Its own slot may hold some,
	 * but those are placeholders (e.g. the music end marker right at the start); only if
	 * no difficulty has a chart are they used. */
	int iDonor = -1;
	for( int d = GNFile::NUM_DIFFS - 1; d >= 0 && iDonor < 0; d-- )
		if( !abNew[d] )
			iDonor = d;

	/* The original frames and header values, for the header values below (the loop
	 * changes sf as it goes, and a new difficulty reads another one's originals). */
	vector<GNFile::Frame> avOriginal[GNFile::NUM_DIFFS];
	int aiOriginalExtra52[GNFile::NUM_DIFFS], aiOriginalDuration[GNFile::NUM_DIFFS];
	for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
	{
		avOriginal[d] = sf.avFrames[d];
		aiOriginalExtra52[d] = sf.aiExtra52[d];
		aiOriginalDuration[d] = sf.aiDuration[d];
	}

	/* The header BPM takes the starting BPM of the hardest difficulty written. */
	for( int d = GNFile::NUM_DIFFS - 1; d >= 0; d-- )
	{
		if( !abWrite[d] )
			continue;
		const Steps *pSteps = song.GetStepsByDifficulty( STEPS_TYPE_DANCE_SINGLE, aDiffs[d], false );
		const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;
		if( !pTiming->m_BPMSegments.empty() )
		{
			sf.fBPM = pTiming->m_BPMSegments[0].m_fBPM;
			break;
		}
	}

	for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
	{
		if( !abWrite[d] )
			continue;	// not in the song (never there, or deleted in the editor), or new and empty: keep it as-is
		const Steps *pSteps = song.GetStepsByDifficulty( STEPS_TYPE_DANCE_SINGLE, aDiffs[d], false );

		NoteData nd;
		pSteps->GetNoteData( &nd );

		const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;

		const vector<GNFile::Frame> *pSkeleton = &avOriginal[d];
		if( abNew[d] && iDonor >= 0 )
			pSkeleton = &avOriginal[iDonor];

		vector<GNFile::Frame> vNew;
		BuildFrames( nd, *pTiming, sf.fBPM, *pSkeleton, vNew );

		const int iMaxMeasurement = MaxMeasurement( vNew );
		if( iMaxMeasurement > GN_MAX_EXTRA52 )
		{
			static const char *aszNames[GNFile::NUM_DIFFS] = { "Easy", "Normal", "Hard" };
			char szErr[160];
			snprintf( szErr, sizeof(szErr), "%s goes up to measure %d; the game only loads charts up to measure %d",
				aszNames[d], iMaxMeasurement, GN_MAX_EXTRA52 );
			sErrOut = szErr;
			return false;
		}

		sf.avFrames[d] = vNew;
		sf.aiNoteCount[d] = CountNotesInFrames( vNew );
		sf.aiMeasurements[d] = (int) vNew.size();

		/* extra52 and duration describe the song (where it ends, how fast the dancers
		 * move), so they start from the original values: this difficulty's own, or for a
		 * new one those of the difficulty its control frames came from, which keeps the
		 * dancers exactly as on that difficulty.  An unchanged chart keeps both as they were. */
		const int iBase = abNew[d] ? iDonor : d;
		const int iBaseExtra52 = iBase >= 0 ? aiOriginalExtra52[iBase] : 0;

		/* extra52 moves by as much as the chart reaches further or less far, and must
		 * cover every frame (some files made by other tools don't). */
		int iExtra52 = iMaxMeasurement;
		if( iBase >= 0 )
			iExtra52 = iBaseExtra52 + iMaxMeasurement - MaxMeasurement( avOriginal[iBase] );
		iExtra52 = max( iExtra52, iMaxMeasurement );
		sf.aiExtra52[d] = iExtra52;

		/* The dancers' beat is duration / extra52, so the duration scales with extra52 and
		 * their speed stays exactly the same.  With nothing to start from, use the song
		 * length the way the official files store it. */
		int iDuration;
		if( iBase >= 0 && iBaseExtra52 > 0 )
			iDuration = (int) floor( (double) aiOriginalDuration[iBase] * iExtra52 / iBaseExtra52 + 0.5 );
		else
			iDuration = SongSeconds( vNew, sf.fBPM );
		sf.aiDuration[d] = max( iDuration, GN_MIN_DURATION );

		/* Level: GN's level and SM's meter are the same number.  For a difficulty that
		 * was already there, leave it alone if the header had none (0), so that a number
		 * guessed by Steps::TidyUpData does not end up in the file. */
		if( pSteps->GetMeter() > 0 && (abNew[d] || sf.aiLevel[d] > 0) )
			sf.aiLevel[d] = (int16_t) GNLoader::MeterToGNLevel( pSteps->GetMeter() );
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
