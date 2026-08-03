/* 驗證 NotesWriterGN::BuildFrames：
 * 拿真的 .gn 譜面 → 用跟 loader 一樣的方式攤成 NoteData/TimingData
 * → BuildFrames 重建 frames → 比對音符集合、BPM 事件與檔案結構。
 *
 * 這裡的「攤成 NoteData」是 loader 邏輯的複製品（loader 本身依賴太多引擎物件，
 * 沒辦法脫離遊戲編譯），所以測到的是 writer 這一側。 */
#include "global.h"
#include "GNFile.h"
#include "NotesWriterGN.h"
#include "NotesLoaderGN.h"
#include <set>
#include <map>

FakeLog g_Log;
FakeLog *LOG = &g_Log;

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

bool GNLoader::ReadGNFile( CString, GNFile::StepFile &, GNFile::ContainerInfo &, CString & )
{
	return false;	// 這個測試不走 Write()
}

static bool ReadWhole( const char *szPath, std::string &sOut )
{
	FILE *f = fopen( szPath, "rb" );
	if( !f )
		return false;
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	sOut.resize( n );
	size_t got = fread( &sOut[0], 1, n, f );
	fclose( f );
	return got == (size_t) n;
}

/* 一顆音符：(欄位, row, 種類)。用來比對往返前後是否一致。 */
typedef std::set< std::pair< std::pair<int,int>, int > > NoteSet;

static void CollectNotes( const vector<GNFile::Frame> &v, NoteSet &out )
{
	for( unsigned f = 0; f < v.size(); f++ )
	{
		const GNFile::Frame &fr = v[f];
		const int iCol = GNLoader::FrameTypeToCol( fr.iType );
		if( iCol < 0 )
			continue;
		const int iInterval = fr.GetInterval();
		for( int i = 0; i < iInterval; i++ )
		{
			if( fr.vSlots[i].u0 == 0 )
				continue;
			const int iRow = fr.iMeasurement * ROWS_PER_MEASURE +
				(int) ( ROWS_PER_MEASURE * i / (float) iInterval + 0.5f );
			out.insert( std::make_pair(std::make_pair(iCol, iRow), (int) fr.vSlots[i].nt) );
		}
	}
}

static void CollectBPMs( const vector<GNFile::Frame> &v, std::map<int,float> &out )
{
	for( unsigned f = 0; f < v.size(); f++ )
	{
		const GNFile::Frame &fr = v[f];
		if( fr.iType != GNFile::FRAME_BPM )
			continue;
		for( unsigned i = 0; i < fr.vSlots.size(); i++ )
		{
			if( fr.vSlots[i].IsEmpty() )
				continue;
			out[ BeatToNoteRow(fr.GetSlotBeat(i)) ] = GNFile::SlotToBPM( fr.vSlots[i] );
		}
	}
}

/* loader 那側的等價轉換（複製自 NotesLoaderGN::FramesToNoteData 的行為）。 */
static void FramesToNoteData( const vector<GNFile::Frame> &vFrames, NoteData &out )
{
	out.SetNumTracks( 4 );
	int aiHoldStart[4] = { -1, -1, -1, -1 };
	vector<HoldNote> vHolds;

	for( unsigned f = 0; f < vFrames.size(); f++ )
	{
		const GNFile::Frame &fr = vFrames[f];
		const int iCol = GNLoader::FrameTypeToCol( fr.iType );
		if( iCol < 0 )
			continue;
		const int iInterval = fr.GetInterval();
		for( int i = 0; i < iInterval; i++ )
		{
			if( fr.vSlots[i].u0 == 0 )
				continue;
			const int iRow = fr.iMeasurement * ROWS_PER_MEASURE +
				(int) ( ROWS_PER_MEASURE * i / (float) iInterval + 0.5f );
			switch( fr.vSlots[i].nt )
			{
			case GNFile::NOTE_HOLD_START:
				if( aiHoldStart[iCol] != -1 )
					out.SetTapNote( iCol, aiHoldStart[iCol], MakeTap(TapNote::tap) );
				aiHoldStart[iCol] = iRow;
				break;
			case GNFile::NOTE_HOLD_END:
				if( aiHoldStart[iCol] != -1 && iRow > aiHoldStart[iCol] )
				{
					vHolds.push_back( HoldNote(iCol, aiHoldStart[iCol], iRow) );
					aiHoldStart[iCol] = -1;
				}
				else if( aiHoldStart[iCol] != -1 )
				{
					out.SetTapNote( iCol, aiHoldStart[iCol], MakeTap(TapNote::tap) );
					aiHoldStart[iCol] = -1;
				}
				else
				{
					/* 沒頭的 hold 結尾：當成單顆音符（與 loader 相同）。 */
					out.SetTapNote( iCol, iRow, MakeTap(TapNote::tap) );
				}
				break;
			default:
				out.SetTapNote( iCol, iRow, MakeTap(TapNote::tap) );
				break;
			}
		}
	}
	for( int c = 0; c < 4; c++ )
		if( aiHoldStart[c] != -1 )
			out.SetTapNote( c, aiHoldStart[c], MakeTap(TapNote::tap) );
	for( unsigned i = 0; i < vHolds.size(); i++ )
		out.AddHoldNote( vHolds[i] );
}

static int g_iFailures = 0;
static void Check( bool b, const char *szWhat )
{
	printf( "    [%s] %s\n", b ? " ok " : "FAIL", szWhat );
	if( !b )
		g_iFailures++;
}

int main( int argc, char **argv )
{
	for( int a = 1; a < argc; a++ )
	{
		std::string sRaw, sBody, sErr;
		GNFile::ContainerInfo info;
		GNFile::StepFile sf;
		if( !ReadWhole(argv[a], sRaw) ||
			!GNFile::Decrypt(sRaw, sBody, info, &sErr) ||
			!GNFile::Parse(sBody, sf, &sErr) )
		{
			printf( "===== %s\n  skip: %s\n", argv[a], sErr.c_str() );
			continue;
		}
		printf( "===== %s\n", argv[a] );

		for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
		{
			const vector<GNFile::Frame> &vOrig = sf.avFrames[d];
			NoteSet setOrig;
			CollectNotes( vOrig, setOrig );
			if( setOrig.empty() )
				continue;

			std::map<int,float> mapOrigBPM;
			CollectBPMs( vOrig, mapOrigBPM );

			NoteData nd;
			FramesToNoteData( vOrig, nd );

			/* timing：表頭 BPM 當基準，加上這個難度自己的變速事件。 */
			TimingData timing;
			timing.m_BPMSegments.push_back( BPMSegment(0, sf.fBPM > 0 ? sf.fBPM : 120) );
			for( std::map<int,float>::const_iterator it = mapOrigBPM.begin(); it != mapOrigBPM.end(); ++it )
			{
				if( it->first == 0 )
					continue;
				timing.m_BPMSegments.push_back( BPMSegment(NoteRowToBeat(it->first), it->second) );
			}

			vector<GNFile::Frame> vNew;
			NotesWriterGN::BuildFrames( nd, timing, sf.fBPM, vOrig, vNew );

			NoteSet setNew;
			CollectNotes( vNew, setNew );

			char buf[192];
			/* 位置集合必須一模一樣；種類可能因為原始資料有壞掉的 hold 而退化成單顆音符。 */
			std::set< std::pair<int,int> > posOrig, posNew;
			int iKindDiff = 0;
			for( NoteSet::const_iterator it = setOrig.begin(); it != setOrig.end(); ++it )
			{
				posOrig.insert( it->first );
				if( setNew.find(*it) == setNew.end() )
					iKindDiff++;
			}
			for( NoteSet::const_iterator it = setNew.begin(); it != setNew.end(); ++it )
				posNew.insert( it->first );

			sprintf( buf, "diff%d: %u note positions survive the round trip (%d kind changes)",
				d, (unsigned) posOrig.size(), iKindDiff );
			Check( posNew == posOrig, buf );
			if( posNew != posOrig )
			{
				printf( "        orig=%u new=%u\n", (unsigned) setOrig.size(), (unsigned) setNew.size() );
				NoteSet::const_iterator i1 = setOrig.begin(), i2 = setNew.begin();
				int iShown = 0;
				while( (i1 != setOrig.end() || i2 != setNew.end()) && iShown < 5 )
				{
					if( i2 == setNew.end() || (i1 != setOrig.end() && *i1 < *i2) )
					{
						printf( "        only in orig: col=%d row=%d nt=%d\n", i1->first.first, i1->first.second, i1->second );
						++i1; iShown++;
					}
					else if( i1 == setOrig.end() || *i2 < *i1 )
					{
						printf( "        only in new : col=%d row=%d nt=%d\n", i2->first.first, i2->first.second, i2->second );
						++i2; iShown++;
					}
					else { ++i1; ++i2; }
				}
			}

			/* BPM 事件也要留住（表頭那一個除外，它由 fBPM 表達）。 */
			std::map<int,float> mapNewBPM;
			CollectBPMs( vNew, mapNewBPM );
			bool bBPMOK = true;
			for( std::map<int,float>::const_iterator it = mapOrigBPM.begin(); it != mapOrigBPM.end(); ++it )
			{
				if( it->first == 0 )
					continue;
				std::map<int,float>::const_iterator f = mapNewBPM.find( it->first );
				if( f == mapNewBPM.end() || fabsf(f->second - it->second) > 0.001f )
					bBPMOK = false;
			}
			sprintf( buf, "diff%d: %u bpm events survive", d, (unsigned) mapOrigBPM.size() );
			Check( bBPMOK, buf );

			/* 小節線之類的控制 frame 一個都不能少。 */
			int iCtrlOrig = 0, iCtrlNew = 0;
			for( unsigned f = 0; f < vOrig.size(); f++ )
				if( vOrig[f].iType != GNFile::FRAME_BPM && GNLoader::FrameTypeToCol(vOrig[f].iType) < 0 )
					iCtrlOrig++;
			for( unsigned f = 0; f < vNew.size(); f++ )
				if( vNew[f].iType != GNFile::FRAME_BPM && GNLoader::FrameTypeToCol(vNew[f].iType) < 0 )
					iCtrlNew++;
			sprintf( buf, "diff%d: control frames kept (%d -> %d)", d, iCtrlOrig, iCtrlNew );
			Check( iCtrlNew >= iCtrlOrig, buf );

			/* frame 必須照 (measurement, type) 排好，原檔就是這個順序。 */
			bool bSorted = true;
			for( unsigned f = 1; f < vNew.size(); f++ )
			{
				const GNFile::Frame &a = vNew[f-1], &b = vNew[f];
				if( a.iMeasurement > b.iMeasurement ||
					(a.iMeasurement == b.iMeasurement && a.iType > b.iType) )
					bSorted = false;
			}
			sprintf( buf, "diff%d: frames sorted by (measurement, type)", d );
			Check( bSorted, buf );

			/* 重建出來的檔案要能再被解析回來。 */
			GNFile::StepFile sf2 = sf;
			sf2.avFrames[d] = vNew;
			std::string sBody2;
			GNFile::Serialize( sf2, sBody2 );
			GNFile::StepFile sf3;
			sprintf( buf, "diff%d: rebuilt file parses again", d );
			Check( GNFile::Parse(sBody2, sf3, &sErr) && sf3.avFrames[d].size() == vNew.size(), buf );

			printf( "      frames %u -> %u, size %u -> %u bytes\n",
				(unsigned) vOrig.size(), (unsigned) vNew.size(),
				(unsigned)( sf.aiAddress[3] - sf.aiAddress[0] ), (unsigned)( sf3.aiAddress[3] - sf3.aiAddress[0] ) );
		}
	}

	printf( "\n%s (%d failures)\n", g_iFailures ? "FAILED" : "ALL PASSED", g_iFailures );
	return g_iFailures ? 1 : 0;
}
