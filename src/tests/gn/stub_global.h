/* 讓 NotesWriterGN.cpp 能脫離 StepMania 編譯的最小替身。
 * 只要能編譯並跑 BuildFrames 就好；Write() 用到的東西給空殼即可。 */
#ifndef GN_TEST_STUBS_H
#define GN_TEST_STUBS_H

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>

using std::min;
using std::max;
using std::vector;
using std::stable_sort;

typedef std::string CString;

#define RageARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- NoteTypes.h ---- */
const int BEATS_PER_MEASURE = 4;
const int ROWS_PER_BEAT = 48;
const int ROWS_PER_MEASURE = ROWS_PER_BEAT * BEATS_PER_MEASURE;

inline int BeatToNoteRow( float fBeatNum ) { return int( fBeatNum * ROWS_PER_BEAT + 0.5f ); }
inline float NoteRowToBeat( int i ) { return i / (float) ROWS_PER_BEAT; }

struct TapNote
{
	enum Type { empty, tap, hold_head, hold_tail, hold, mine, attack };
	unsigned type : 3;
	unsigned source : 2;
	unsigned attackIndex : 3;
};

inline TapNote MakeTap( TapNote::Type t )
{
	TapNote tn;
	tn.type = t;
	tn.source = 0;
	tn.attackIndex = 0;
	return tn;
}

struct HoldNote
{
	HoldNote( int t, int s, int e ) { iTrack = t; iStartRow = s; iEndRow = e; }
	int iStartRow, iEndRow, iTrack;
};

/* ---- NoteData.h ---- */
class NoteData
{
public:
	NoteData() : m_iNumTracks(4) { }
	int GetNumTracks() const { return m_iNumTracks; }
	void SetNumTracks( int n ) { m_iNumTracks = n; m_Taps.clear(); }
	void SetTapNote( int track, int row, TapNote tn )
	{
		if( (int) m_Taps.size() <= row )
			m_Taps.resize( row+1, std::vector<TapNote>(4, MakeTap(TapNote::empty)) );
		m_Taps[row][track] = tn;
	}
	TapNote GetTapNote( unsigned track, int row ) const
	{
		if( row < 0 || row >= (int) m_Taps.size() )
			return MakeTap( TapNote::empty );
		return m_Taps[row][track];
	}
	void AddHoldNote( HoldNote hn ) { m_HoldNotes.push_back( hn ); }
	int GetNumHoldNotes() const { return (int) m_HoldNotes.size(); }
	const HoldNote &GetHoldNote( int i ) const { return m_HoldNotes[i]; }
	int GetLastRow() const { return (int) m_Taps.size() - 1; }

	int m_iNumTracks;
	std::vector< std::vector<TapNote> > m_Taps;
	std::vector<HoldNote> m_HoldNotes;
};

/* ---- TimingData.h ---- */
struct BPMSegment
{
	BPMSegment() { m_fStartBeat = m_fBPM = -1; }
	BPMSegment( float s, float b ) { m_fStartBeat = s; m_fBPM = b; }
	float m_fStartBeat, m_fBPM;
};
struct StopSegment
{
	StopSegment() { m_fStartBeat = m_fStopSeconds = -1; }
	StopSegment( float s, float f ) { m_fStartBeat = s; m_fStopSeconds = f; }
	float m_fStartBeat, m_fStopSeconds;
};
class TimingData
{
public:
	TimingData() : m_fBeat0OffsetInSeconds(0) { }
	std::vector<BPMSegment> m_BPMSegments;
	std::vector<StopSegment> m_StopSegments;
	float m_fBeat0OffsetInSeconds;
};

/* ---- Steps / Song ---- */
enum Difficulty { DIFFICULTY_BEGINNER, DIFFICULTY_EASY, DIFFICULTY_MEDIUM, DIFFICULTY_HARD, DIFFICULTY_CHALLENGE, DIFFICULTY_EDIT, NUM_DIFFICULTIES, DIFFICULTY_INVALID };
enum StepsType { STEPS_TYPE_DANCE_SINGLE, STEPS_TYPE_INVALID };

class Steps
{
public:
	Steps() : m_pTiming(NULL) { }
	bool HasOwnTiming() const { return m_pTiming != NULL; }
	const TimingData *GetOwnTiming() const { return m_pTiming; }
	void GetNoteData( NoteData *p ) const { *p = m_NoteData; }
	NoteData m_NoteData;
	TimingData *m_pTiming;
};

class Song
{
public:
	Steps *GetStepsByDifficulty( StepsType, Difficulty d, bool ) const
	{
		return m_apSteps[d];
	}
	Song() { for( int i=0; i<NUM_DIFFICULTIES; i++ ) m_apSteps[i] = NULL; }
	Steps *m_apSteps[NUM_DIFFICULTIES];
	TimingData m_Timing;
};

/* ---- RageFile / RageLog ---- */
class RageFile
{
public:
	enum { READ = 1, WRITE = 2 };
	bool Open( CString, int = READ ) { return false; }
	int GetFileSize() const { return 0; }
	int Read( void *, size_t ) { return 0; }
	int Write( const void *, size_t n ) { return (int) n; }
	int Flush() { return 0; }
};

struct FakeLog
{
	void Trace( const char *, ... ) { }
	void Warn( const char *, ... ) { }
};
extern FakeLog *LOG;

#endif
