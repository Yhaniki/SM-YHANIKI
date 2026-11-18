#include "global.h"
#include "WaveformDisplay.h"
#include "RageDisplay.h"
#include "RageLog.h"
#include "RageFile.h"
#include "RageTimer.h"
#include "RageUtil.h"
#include "RageSoundReader_FileReader.h"
#include "GameState.h"
#include "ArrowEffects.h"
#include "song.h"

/* Look (same scheme as 65_remake's chart editor): RMS is the body, coloured
 * from dim to bright by loudness so accents stand out; the peak is a faint halo
 * around it that still shows where each hit is. */
static const float WAVE_WIDTH		= 240.f;	// full width of the waveform, NoteField units
static const float BG_HALF_WIDTH	= WAVE_WIDTH/2 + 10.f;
static const float BG_TOP			= -600.f;
static const float BG_BOTTOM		= 600.f;
static const float BG_ALPHA			= 0.6f;
static const float FULL_SCALE		= 32768.f;	// |sample| that's drawn full width (unless normalising)
static const float SILENCE			= 0.001f;	// rows whose scaled peak is below this aren't drawn
static const float MIN_RMS_WIDTH	= 0.5f;		// NoteField units
static const float RMS_BRIGHTNESS	= 1.4f;		// colour = lerp(lo, hi, clamp(rms * this))
static const RageColor COLOR_CENTER_LINE( 0x2b/255.f, 0x2b/255.f, 0x36/255.f, 1 );
static const RageColor COLOR_PEAK(        0x4a/255.f, 0x3f/255.f, 0x0d/255.f, 1 );
static const RageColor COLOR_RMS_LO(      0x8a/255.f, 0x77/255.f, 0x18/255.f, 1 );
static const RageColor COLOR_RMS_HI(      0xf0/255.f, 0xcf/255.f, 0x3a/255.f, 1 );
static const RageColor COLOR_NOW(         0, 0, 1, 1 );

/* Size of the smallest bin (RMS window), seconds. */
static const double BIN_SECONDS	= 0.002;

/* Cost.  ROW_HEIGHT is in NoteField units; the edit NoteField is zoomed 0.5,
 * so 2 units is about one screen pixel there. */
static const float ROW_HEIGHT		= 2.f;
static const int   MAX_ROWS			= 1500;
/* RageDisplay_D3D draws quads with 16-bit indices; keep each batch well under that. */
static const int   MAX_VERTS_PER_DRAW = 4096*4;

/*
 * Detect MP3s that are decoded ~one MPEG audio frame ahead of the actual music.
 *
 * StepMania's MP3 reader keeps a leading frame for files that carry a Xing/Info
 * header (to match DWI/BASS sync), but files WITHOUT such a header end up one
 * frame early in the decoded PCM we read for the waveform.  That makes the drawn
 * waveform sit ~26ms (1152 samples @ 44.1kHz) ahead of the note grid / music.
 * Return the duration of one MPEG frame in seconds for those files so we can
 * compensate; return 0 (no shift) for everything else.
 */
static float DetectMp3FrameAlignSeconds( const CString &sPath )
{
	/* Only MP3s have the problem.  Scanning other formats for an MPEG frame sync
	 * can find a false one in compressed data and shift them by mistake. */
	CString sExt = GetExtension( sPath );
	sExt.MakeLower();
	if( sExt != "mp3" )
		return 0.f;

	RageFile file;
	if( !file.Open( sPath ) )
		return 0.f;

	unsigned char buf[16384];
	int got = file.Read( buf, sizeof(buf) );
	if( got < 8 )
		return 0.f;

	int i = 0;
	// Skip an ID3v2 tag if present (syncsafe 28-bit size).
	if( got > 10 && buf[0]=='I' && buf[1]=='D' && buf[2]=='3' )
		i = 10 + ( ((buf[6]&0x7f)<<21) | ((buf[7]&0x7f)<<14) | ((buf[8]&0x7f)<<7) | (buf[9]&0x7f) );

	// Find the first MPEG audio frame sync (0xFF 0xEx/0xFx).
	int ver = -1, srIdx = -1;
	for( ; i < got-4; ++i )
	{
		if( buf[i]==0xFF && (buf[i+1]&0xE0)==0xE0 )
		{
			ver          = (buf[i+1]>>3)&3;	// 0=MPEG2.5, 2=MPEG2, 3=MPEG1
			int layer    = (buf[i+1]>>1)&3;
			srIdx        = (buf[i+2]>>2)&3;
			if( ver!=1 && layer!=0 && srIdx!=3 )
				break;
		}
	}
	if( ver < 0 || i >= got-4 )
		return 0.f;	// not a recognisable MP3 -> no shift

	// If this first frame carries a Xing/Info header, the file is already aligned.
	int end = min( got-4, i+200 );
	for( int j = i; j < end; ++j )
	{
		if( (buf[j]=='X'&&buf[j+1]=='i'&&buf[j+2]=='n'&&buf[j+3]=='g') ||
		    (buf[j]=='I'&&buf[j+1]=='n'&&buf[j+2]=='f'&&buf[j+3]=='o') )
			return 0.f;
	}

	// No Xing/Info header: compute one MPEG frame's duration.
	static const int rate1[4] = { 44100, 48000, 32000, 0 };
	int rate = rate1[srIdx];
	if( ver==2 )      rate /= 2;	// MPEG2
	else if( ver==0 ) rate /= 4;	// MPEG2.5
	if( rate <= 0 )
		return 0.f;
	int samplesPerFrame = (ver==3) ? 1152 : 576;	// MPEG1 vs MPEG2/2.5 (Layer III)
	return (float)samplesPerFrame / (float)rate;
}

static inline void MergeBin( WaveformData::Bin &into, const WaveformData::Bin &b )
{
	if( b.peak > into.peak ) into.peak = b.peak;
	if( b.rms > into.rms ) into.rms = b.rms;
}

//
// WaveformData
//

WaveformData *WaveformData::s_pShared = NULL;

void WaveformData::UpdateShared( const CString &sMusicPath, float fMaxSeconds )
{
	if( s_pShared != NULL && s_pShared->m_sPath != sMusicPath )
		FreeShared();

	if( s_pShared == NULL )
	{
		if( sMusicPath.empty() )
			return;
		s_pShared = new WaveformData( sMusicPath );
	}

	if( !s_pShared->m_bDone )
		s_pShared->Decode( fMaxSeconds );
}

void WaveformData::FreeShared()
{
	delete s_pShared;
	s_pShared = NULL;
}

WaveformData::WaveformData( const CString &sPath ):
	m_sPath( sPath ),
	m_pReader( NULL ),
	m_bDone( false ),
	m_iChannels( 2 ),
	m_iBinFrames( 1 ),
	m_fBinsPerSecond( 0 ),
	m_fAlignSeconds( 0 ),
	m_fLengthSeconds( 0 ),
	m_fDecodeSeconds( 0 ),
	m_iDecodeCalls( 0 ),
	m_iCurPeak( 0 ),
	m_fCurSumSquares( 0 ),
	m_iCurFrames( 0 ),
	m_iMaxPeak( 0 ),
	m_iMaxRms( 0 )
{
}

WaveformData::~WaveformData()
{
	delete m_pReader;
}

bool WaveformData::Open()
{
	CString sError;
	m_pReader = SoundReader_FileReader::OpenFile( m_sPath, sError );
	if( m_pReader == NULL )
	{
		LOG->Warn( "Waveform: couldn't open \"%s\": %s", m_sPath.c_str(), sError.c_str() );
		return false;
	}

	const int iRate = m_pReader->GetSampleRate();
	m_iChannels = m_pReader->GetNumChannels();
	if( iRate <= 0 || (m_iChannels != 1 && m_iChannels != 2) )
	{
		LOG->Warn( "Waveform: \"%s\": unsupported format (%d Hz, %d channels)",
			m_sPath.c_str(), iRate, m_iChannels );
		return false;
	}

	/* A bin is a whole number of frames (~2ms; RMS over a much shorter window
	 * is little more than the peak).  Work out bins per second from that whole
	 * number, not from the 2ms asked for, or the waveform slowly drifts away
	 * from the music over the length of the song. */
	m_iBinFrames = max( 1, (int) floor(iRate * BIN_SECONDS + 0.5) );
	m_fBinsPerSecond = double(iRate) / m_iBinFrames;
	m_fAlignSeconds = DetectMp3FrameAlignSeconds( m_sPath );

	/* Reserve for the whole song up front so the vectors don't keep growing. */
	const int iLengthMs = m_pReader->GetLength_Fast();
	if( iLengthMs > 0 )
	{
		m_fLengthSeconds = iLengthMs / 1000.f;
		size_t iBins = size_t( double(iLengthMs) / 1000 * m_fBinsPerSecond ) + 16;
		for( int l = 0; l < MAX_LEVELS && iBins > 0; ++l, iBins /= 2 )
			m_Levels[l].reserve( iBins + 1 );
	}

	m_Buf.resize( 4096 * sizeof(int16_t) * m_iChannels );
	return true;
}

void WaveformData::Decode( float fMaxSeconds )
{
	RageTimer timer;
	DecodeSome( timer, fMaxSeconds );
	m_fDecodeSeconds += timer.Ago();
	++m_iDecodeCalls;

	if( m_bDone )
		LOG->Trace( "Waveform: loaded \"%s\": %u bins, max peak %d, max rms %d, align %.4fs; "
			"decoding took %.3fs over %d frames",
			m_sPath.c_str(), (unsigned) m_Levels[0].size(), m_iMaxPeak, m_iMaxRms, m_fAlignSeconds,
			m_fDecodeSeconds, m_iDecodeCalls );
}

float WaveformData::GetLoadedFraction() const
{
	if( m_bDone )
		return 1;
	if( m_fLengthSeconds <= 0 || m_fBinsPerSecond <= 0 )
		return 0;
	const float fDecoded = float( m_Levels[0].size() / m_fBinsPerSecond );
	return clamp( fDecoded / m_fLengthSeconds, 0.f, 1.f );
}

void WaveformData::DecodeSome( const RageTimer &timer, float fMaxSeconds )
{
	if( m_pReader == NULL && !Open() )
	{
		FinishLoading();
		return;
	}

	const int iFrameBytes = sizeof(int16_t) * m_iChannels;
	do
	{
		const int iGot = m_pReader->Read( &m_Buf[0], m_Buf.size() );
		if( iGot <= 0 )
		{
			if( iGot < 0 )
				LOG->Warn( "Waveform: error decoding \"%s\": %s", m_sPath.c_str(), m_pReader->GetError().c_str() );
			FinishLoading();
			return;
		}

		/* Both channels go into the same bin (the waveform is mono). */
		const int16_t *pSample = (const int16_t *) &m_Buf[0];
		const int iFrames = iGot / iFrameBytes;
		for( int f = 0; f < iFrames; ++f )
		{
			for( int c = 0; c < m_iChannels; ++c, ++pSample )
			{
				const int s = *pSample;
				const int a = s < 0? -s:s;
				if( a > m_iCurPeak )
					m_iCurPeak = a;
				m_fCurSumSquares += double(s) * s;
			}

			if( ++m_iCurFrames == m_iBinFrames )
				FlushBin();
		}
	} while( timer.Ago() < fMaxSeconds );
}

void WaveformData::FlushBin()
{
	if( m_iCurFrames == 0 )
		return;

	Bin bin;
	bin.peak = (uint16_t) min( m_iCurPeak, 32768 );
	bin.rms = (uint16_t) min( (int) sqrt( m_fCurSumSquares / (m_iCurFrames * m_iChannels) ), 32768 );
	AddBin( bin );

	m_iMaxPeak = max( m_iMaxPeak, (int) bin.peak );
	m_iMaxRms = max( m_iMaxRms, (int) bin.rms );

	m_iCurPeak = 0;
	m_fCurSumSquares = 0;
	m_iCurFrames = 0;
}

void WaveformData::AddBin( const Bin &bin )
{
	m_Levels[0].push_back( bin );

	/* Every time a level gets an even number of bins, its last pair becomes one
	 * bin of the next level. */
	for( int l = 0; l+1 < MAX_LEVELS; ++l )
	{
		const size_t n = m_Levels[l].size();
		if( n % 2 != 0 )
			break;
		Bin merged = m_Levels[l][n-2];
		MergeBin( merged, m_Levels[l][n-1] );
		m_Levels[l+1].push_back( merged );
	}
}

void WaveformData::FinishLoading()
{
	FlushBin();

	delete m_pReader;
	m_pReader = NULL;
	m_Buf.clear();
	m_bDone = true;
}

bool WaveformData::GetBin( float fStartSec, float fEndSec, Bin &out ) const
{
	const int iNumBins = (int) m_Levels[0].size();
	if( iNumBins == 0 )
		return false;

	if( fEndSec < fStartSec )
		swap( fStartSec, fEndSec );

	/* Shift the look-up later by m_fAlignSeconds; see DetectMp3FrameAlignSeconds(). */
	int iStart = (int) floor( (fStartSec - m_fAlignSeconds) * m_fBinsPerSecond );
	int iEnd = (int) ceil( (fEndSec - m_fAlignSeconds) * m_fBinsPerSecond );
	if( iEnd <= iStart )
		iEnd = iStart + 1;
	if( iEnd <= 0 || iStart >= iNumBins )
		return false;
	iStart = max( iStart, 0 );
	iEnd = min( iEnd, iNumBins );

	/* Cover [iStart, iEnd) exactly with the largest aligned bins available. */
	out.peak = out.rms = 0;
	while( iStart < iEnd )
	{
		int l = 0;
		while( l+1 < MAX_LEVELS )
		{
			const int iSpan = 1 << (l+1);
			if( (iStart & (iSpan-1)) != 0 || iStart + iSpan > iEnd )
				break;
			if( (iStart >> (l+1)) >= (int) m_Levels[l+1].size() )
				break;	// not built yet (still loading)
			++l;
		}
		MergeBin( out, m_Levels[l][iStart >> l] );
		iStart += 1 << l;
	}
	return true;
}

//
// WaveformDisplay
//

WaveformDisplay::WaveformDisplay():
	m_PlayerNumber( PLAYER_1 ),
	m_fFirstBeat( 0 ),
	m_fLastBeat( 0 ),
	m_fYReverseOffsetPixels( 0 )
{
}

void WaveformDisplay::SetDrawRange( PlayerNumber pn, float fFirstBeat, float fLastBeat, float fYReverseOffsetPixels )
{
	m_PlayerNumber = pn;
	m_fFirstBeat = fFirstBeat;
	m_fLastBeat = fLastBeat;
	m_fYReverseOffsetPixels = fYReverseOffsetPixels;
}

float WaveformDisplay::BeatToYPos( float fBeat ) const
{
	const float fYOffset = ArrowGetYOffset( m_PlayerNumber, 0, fBeat );
	return ArrowGetYPos( m_PlayerNumber, 0, fYOffset, m_fYReverseOffsetPixels );
}

void WaveformDisplay::AddQuad( float fLeft, float fTop, float fRight, float fBottom, const RageColor &c )
{
	/* Corners in order around the quad, as both GL_QUADS and the D3D index
	 * buffer (0,1,2 / 2,3,0) expect. */
	RageSpriteVertex v;
	v.c = c;
	v.p = RageVector3( fLeft,  fTop,    0 );	m_Verts.push_back( v );
	v.p = RageVector3( fRight, fTop,    0 );	m_Verts.push_back( v );
	v.p = RageVector3( fRight, fBottom, 0 );	m_Verts.push_back( v );
	v.p = RageVector3( fLeft,  fBottom, 0 );	m_Verts.push_back( v );
}

void WaveformDisplay::DrawPrimitives()
{
	const WaveformData *pData = WaveformData::Get();
	const Song *pSong = GAMESTATE->m_pCurSong;
	if( pData == NULL || pSong == NULL || m_fLastBeat <= m_fFirstBeat )
		return;

	m_Verts.clear();

	AddQuad( -BG_HALF_WIDTH, BG_TOP, BG_HALF_WIDTH, BG_BOTTOM, RageColor(0,0,0,BG_ALPHA) );
	AddQuad( -0.5f, BG_TOP, 0.5f, BG_BOTTOM, COLOR_CENTER_LINE );

	const float fHalfWidth = WAVE_WIDTH / 2;
	const float fNowY = BeatToYPos( GAMESTATE->m_fSongBeat );

	if( !GAMESTATE->m_bWaveNormalize )
	{
		/* Default: fixed scale (full scale = full width).  A row's width depends
		 * only on its own audio, so nothing already drawn changes as the rest of
		 * the song loads: draw while loading. */
		AddRows( *pData, *pSong, 1.f / FULL_SCALE, 1.f / FULL_SCALE );
	}
	else if( pData->IsLoaded() )
	{
		/* Normalised (Shift+F3, for testing): the loudest part of the song is
		 * stretched to full width, so quiet songs are as wide as loud ones.  The
		 * loudest part isn't known until the whole song is decoded, and drawing
		 * before that would keep rescaling (shrinking) the waveform, so wait. */
		const float fPeakScale = pData->GetMaxPeak() > 0? 1.f / pData->GetMaxPeak() : 0.f;
		const float fRmsScale = pData->GetMaxRms() > 0? 1.f / pData->GetMaxRms() : 0.f;
		AddRows( *pData, *pSong, fPeakScale, fRmsScale );
	}

	/* Loading progress. */
	if( !pData->IsLoaded() )
	{
		const float fRight = -fHalfWidth + WAVE_WIDTH * pData->GetLoadedFraction();
		AddQuad( -fHalfWidth, fNowY - 2.f, fRight, fNowY + 2.f, COLOR_RMS_LO );
	}

	/* Current song position. */
	AddQuad( -fHalfWidth, fNowY - 0.5f, fHalfWidth, fNowY + 0.5f, COLOR_NOW );

	FlushQuads();
}

void WaveformDisplay::AddRows( const WaveformData &data, const Song &song, float fPeakScale, float fRmsScale )
{
	const float fHalfWidth = WAVE_WIDTH / 2;

	/* Split the visible beats into rows about ROW_HEIGHT tall.  Each row shows
	 * the music between its two beats (the max over it, so zooming out never
	 * hides a hit), and BPM changes and stops are followed exactly (a stop lands
	 * inside the row that contains its beat). */
	const float fFirstY = BeatToYPos( m_fFirstBeat );
	const float fLastY = BeatToYPos( m_fLastBeat );
	int iRows = (int) ( fabsf(fLastY - fFirstY) / ROW_HEIGHT );
	CLAMP( iRows, 1, MAX_ROWS );
	const float fBeatsPerRow = (m_fLastBeat - m_fFirstBeat) / iRows;

	float fPrevY = fFirstY;
	float fPrevSec = song.GetElapsedTimeFromBeat( m_fFirstBeat );
	for( int r = 1; r <= iRows; ++r )
	{
		const float fBeat = m_fFirstBeat + r * fBeatsPerRow;
		const float fY = BeatToYPos( fBeat );
		const float fSec = song.GetElapsedTimeFromBeat( fBeat );

		WaveformData::Bin bin;
		if( data.GetBin(fPrevSec, fSec, bin) )
		{
			const float fPeak = min( bin.peak * fPeakScale, 1.f );
			const float fRms = min( bin.rms * fRmsScale, 1.f );
			if( fPeak > SILENCE )
			{
				const float fTop = min( fPrevY, fY );
				const float fBottom = max( fPrevY, fY );

				/* Peak: faint halo that marks where the hits are. */
				const float fPeakWidth = max( MIN_RMS_WIDTH, fPeak * fHalfWidth );
				AddQuad( -fPeakWidth, fTop, fPeakWidth, fBottom, COLOR_PEAK );

				/* RMS: the body, brighter where it's louder. */
				const float fRmsWidth = fRms * fHalfWidth;
				if( fRmsWidth >= MIN_RMS_WIDTH )
				{
					const float fBright = clamp( fRms * RMS_BRIGHTNESS, 0.f, 1.f );
					const RageColor c = COLOR_RMS_LO + (COLOR_RMS_HI - COLOR_RMS_LO) * fBright;
					AddQuad( -fRmsWidth, fTop, fRmsWidth, fBottom, c );
				}
			}
		}

		fPrevY = fY;
		fPrevSec = fSec;
	}
}

void WaveformDisplay::FlushQuads()
{
	Actor::SetRenderStates();
	DISPLAY->SetBlendMode( BLEND_NORMAL );
	DISPLAY->ClearAllTextures();

	for( size_t i = 0; i < m_Verts.size(); i += MAX_VERTS_PER_DRAW )
	{
		const int iNum = (int) min( m_Verts.size() - i, (size_t) MAX_VERTS_PER_DRAW );
		DISPLAY->DrawQuads( &m_Verts[i], iNum );
	}
}
