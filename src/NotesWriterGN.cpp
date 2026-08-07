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

/* GN 允許的 interval。愈小檔案愈小，所以挑「放得下所有音符」的最小值。 */
static const int g_aiIntervals[] = { 4, 8, 12, 16, 24, 32, 48, 64, 96, 192 };

/* 一個 (measurement, type) 對應一個 frame；先按小節內的 row 收集，最後再決定 interval。 */
typedef std::map<int, GNFile::Slot> SlotMap;			// 小節內 row (0..191) -> slot
typedef std::map<std::pair<int,int>, SlotMap> FrameMap;	// (measurement, type) -> slots

static void AddSlot( FrameMap &m, int iRow, int iType, const GNFile::Slot &slot )
{
	if( iRow < 0 )
		return;
	const int iMeasurement = iRow / GN_ROWS_PER_MEASUREMENT;
	const int iRowIn = iRow % GN_ROWS_PER_MEASUREMENT;
	m[std::make_pair(iMeasurement, iType)][iRowIn] = slot;
}

/* 找出放得下這些 row 的最小 interval。 */
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

	// ---- 音符 ----
	const int iLastRow = nd.GetLastRow();
	for( int t = 0; t < nd.GetNumTracks() && t < 4; t++ )
	{
		const int iType = GNLoader::ColToFrameType( t );
		for( int r = 0; r <= iLastRow; r++ )
		{
			const TapNote tn = nd.GetTapNote( t, r );
			if( tn.type == TapNote::empty || tn.type == TapNote::mine )
				continue;	// GN 沒有地雷
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

	// ---- BPM 變化 ----
	/* 第 0 拍的 BPM 由表頭表達；只有跟表頭不同時才另外寫一個 type 1。 */
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

	// ---- 控制用 frame（小節線、音樂起止…）原樣保留 ----
	int iMaxBarlineMeasurement = -1;
	int iBarlineStep = 2;		// 原檔相鄰兩條小節線的間隔
	int iNextBarlineValue = -1;	// 原檔小節線的編號是遞增的
	for( unsigned f = 0; f < vOriginal.size(); f++ )
	{
		const GNFile::Frame &fr = vOriginal[f];
		if( fr.iType == GNFile::FRAME_BPM || GNLoader::FrameTypeToCol(fr.iType) >= 0 )
			continue;	// 音符與 BPM 由上面重建

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

	/* 譜面被改長了就照原本的間隔把小節線補上去。 */
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

	// ---- 把收集到的 slot 變成 frame ----
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

	// ---- 原檔的順序是 (measurement, type) 由小到大 ----
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
	return f.Flush() == 0;
}

bool NotesWriterGN::Write( CString sPath, const Song &song, bool bKeepFileSize, CString &sErrOut )
{
	/* 以原檔當骨架：表頭沒解讀的欄位、控制 frame 都留著。 */
	GNFile::StepFile sf;
	GNFile::ContainerInfo info;
	if( !GNLoader::ReadGNFile( sPath, sf, info, sErrOut ) )
		return false;

	static const Difficulty aDiffs[GNFile::NUM_DIFFS] =
	{
		DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD
	};

	/* 表頭 BPM 沿用最難那個難度的起始 BPM。 */
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
			continue;	// 這個難度沒載進來（.gn 裡是空的），原樣保留

		NoteData nd;
		pSteps->GetNoteData( &nd );

		const TimingData *pTiming = pSteps->HasOwnTiming() ? pSteps->GetOwnTiming() : &song.m_Timing;

		vector<GNFile::Frame> vNew;
		BuildFrames( nd, *pTiming, sf.fBPM, sf.avFrames[d], vNew );
		sf.avFrames[d] = vNew;

		sf.aiNoteCount[d] = CountNotesInFrames( vNew );
		sf.aiMeasurements[d] = (int) vNew.size();

		/* 等級：GN 的 level 跟 SM 的 meter 是同一個數字，在編輯器改過就寫回去。
		 * 原本表頭沒填（0）的就別動，免得把 Steps::TidyUpData 猜出來的數字寫進檔案。 */
		if( sf.aiLevel[d] > 0 && pSteps->GetMeter() > 0 )
			sf.aiLevel[d] = (int16_t) GNLoader::MeterToGNLevel( pSteps->GetMeter() );

		/* aiExtra52 / aiDuration 的確切語意還沒完全弄清楚（實測 extra52 既不是
		 * 最大小節也不是小節數），亂寫可能讓原版遊戲算錯速度，所以原樣保留。 */
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
