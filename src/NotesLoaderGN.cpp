#include "global.h"
#include "NotesLoaderGN.h"
#include "RageFile.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "RageUtil_CharConversions.h"
#include "NoteData.h"
#include "NoteTypes.h"
#include "GameManager.h"

/* GN 的 measurement 是 4 拍，SM 一拍 48 row，剛好對上 GN 一小節最細的 192 格。 */
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

/* GN 的 level 大約是 osu! 星數的五倍（工具端以 star×5 產生），實測看到 0~40 出頭。
 * SM 的 meter 只到 13，等比壓過去。 */
int GNLoader::GNLevelToMeter( int iLevel )
{
	if( iLevel <= 0 )
		return 0;	// 讓 Steps::TidyUpData 自己去猜
	return clamp( (int) roundf( iLevel * MAX_METER / 40.0f ), 1, MAX_METER );
}

int GNLoader::MeterToGNLevel( int iMeter )
{
	return clamp( (int) roundf( iMeter * 40.0f / MAX_METER ), 1, 99 );
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

	/* 基準 BPM 取表頭；表頭沒填就用第一個變速事件。 */
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

	/* type 1 的每個非空 slot 就是一次變速。 */
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
				continue;	// 不是合理的 BPM，多半是我們誤讀了 slot
			const float fBeat = max( 0.f, fr.GetSlotBeat(i) );
			/* 很多譜面會在開頭再標一次跟表頭一樣的 BPM；那不是變速，別留下多餘的 segment。 */
			if( fabsf(out.GetBPMAtBeat(fBeat) - fBPM) < 0.001f )
				continue;
			out.SetBPMAtBeat( fBeat, fBPM );
		}
	}

	/* 音樂的第 0 秒對到 type-10 那一拍；換算成 SM 的 beat 0 偏移。 */
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
				continue;	// 空格

			const int iRow = fr.iMeasurement * GN_ROWS_PER_MEASUREMENT +
				(int) roundf( GN_ROWS_PER_MEASUREMENT * i / (float) iInterval );
			if( iRow < 0 )
				continue;

			switch( sl.nt )
			{
			case GNFile::NOTE_HOLD_START:
				/* 前一個 hold 沒收尾就把它當成單顆音符。 */
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
					/* 長度為零的 hold，退化成單顆音符。 */
					out.SetTapNote( iCol, aiHoldStartRow[iCol], TAP_ORIGINAL_TAP );
					aiHoldStartRow[iCol] = -1;
				}
				else
				{
					/* 少數譜面有沒頭的 hold 結尾（原始資料就是這樣）。
					 * SM 沒有對應的表示法，當成單顆音符，總比整顆消失好。 */
					out.SetTapNote( iCol, iRow, TAP_ORIGINAL_TAP );
				}
				break;

			default:
				out.SetTapNote( iCol, iRow, TAP_ORIGINAL_TAP );
				break;
			}
		}
	}

	/* 沒有收尾的 hold 一律當單顆音符，免得整條音符消失。 */
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

	/* 一首歌常有成對的 xxxK.gn / xxxT.gn；K 是我們要的單人譜。 */
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

/* .gn 的表頭字串沒有標示編碼，實測多半是 GBK，少數台版是 Big5。 */
static CString GNStringToUTF8( const std::string &s )
{
	CString sOut = s.c_str();
	if( sOut.empty() )
		return sOut;
	TrimRight( sOut );
	ConvertString( sOut, "utf-8,chinese,big5,japanese,english" );
	return sOut;
}

/* 找出這首歌的音樂檔：先照表頭裡的檔名，再照 .gn 的檔名（去掉結尾的 K/T），
 * 最後退而求其次用資料夾裡任何一個音樂檔。 */
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

	/* 三個難度各自建 Steps，每個都帶著自己的 BPM 表。 */
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
			continue;	// 這個難度是空的（很多檔案只放了 Hard）

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

		/* Song 本身的 timing 用最難的那個難度；玩到哪個難度再換成它自己的。 */
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
