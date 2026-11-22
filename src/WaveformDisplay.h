#ifndef WAVEFORM_DISPLAY_H
#define WAVEFORM_DISPLAY_H

#include "Actor.h"
#include "PlayerNumber.h"
#include <vector>

class SoundReader;
class RageTimer;
class Song;

/*
 * WaveformData - peak/RMS pyramid of one music file, shared by every NoteField
 * in the editor (edit / record / play all draw the same data).
 *
 * - The file is decoded once, sequentially, for at most a few milliseconds per
 *   frame on the main thread (see UpdateShared()).  Turning the waveform on
 *   never freezes the editor.  (Decoding stays on the main thread on purpose:
 *   some sound readers, e.g. the WAV reader, use static scratch buffers.)
 *   The waveform is drawn while it loads, from the start of the song down.
 * - Raw PCM is never kept.  Every ~2ms of frames (both channels mixed) is
 *   reduced to one bin right away, and coarser levels (each halving the previous
 *   one, taking the max) are built incrementally, so any time range is answered
 *   in O(log n) at any zoom.  A 3 minute song takes ~1MB.
 * - Each bin keeps two values, both needed:
 *   - RMS (loudness envelope) is the body of the waveform.  Peaks alone are
 *     useless for mastered/compressed music: almost every bin peaks near full
 *     scale, so the waveform turns into a solid bar with no visible beats.
 *   - Peak (max |sample|) keeps the transients that RMS averages away; it's
 *     drawn as a faint halo around the body.
 * - By default both are drawn on a fixed scale (full scale = full width), so
 *   nothing drawn changes as the rest of the song loads.  Normalising (each
 *   value stretched by the song's own maximum, so quiet songs are as wide as
 *   loud ones) is kept as a test option (GAMESTATE->m_bWaveNormalize, Shift+F3
 *   in the editor); since the maximum is only known once the whole song is
 *   decoded, the waveform is then drawn only after loading finishes.
 */
class WaveformData
{
public:
	struct Bin
	{
		uint16_t peak;	// max |sample|, 0..32768
		uint16_t rms;	// RMS of the samples, 0..32768
	};

	/* The shared instance, or NULL if none was requested. */
	static const WaveformData *Get() { return s_pShared; }
	/* Make sure the shared instance is for sMusicPath and keep decoding it for at
	 * most fMaxSeconds.  Cheap once the file is fully loaded. */
	static void UpdateShared( const CString &sMusicPath, float fMaxSeconds );
	static void FreeShared();

	/* Bin n covers music time [n, n+1) / GetBinsPerSecond(); 0 until the file
	 * is opened. */
	double GetBinsPerSecond() const { return m_fBinsPerSecond; }
	/* Bins decoded so far. */
	int GetNumBins() const { return (int) m_Levels[0].size(); }
	/* Max peak / RMS over bins [iStart, iEnd); one look-up when the range is a
	 * power of two starting on a multiple of it.  Returns false if none of
	 * those bins is decoded (yet). */
	bool GetBins( int iStart, int iEnd, Bin &out ) const;

	/* Largest values decoded so far (the whole song's once IsLoaded()), for
	 * normalising. */
	int GetMaxPeak() const { return m_iMaxPeak; }
	int GetMaxRms() const { return m_iMaxRms; }

	bool IsLoaded() const { return m_bDone; }
	/* 0..1, for a progress bar while loading. */
	float GetLoadedFraction() const;

private:
	WaveformData( const CString &sPath );
	~WaveformData();

	bool Open();
	void Decode( float fMaxSeconds );
	void DecodeSome( const RageTimer &timer, float fMaxSeconds );
	void FlushBin();
	void AddBin( const Bin &bin );
	void FinishLoading();

	static WaveformData *s_pShared;

	enum { MAX_LEVELS = 24 };

	CString		m_sPath;
	SoundReader	*m_pReader;
	bool		m_bDone;			// fully decoded (or gave up); reader is closed
	int			m_iChannels;
	int			m_iBinFrames;		// frames per level-0 bin (~2ms)
	double		m_fBinsPerSecond;
	float		m_fLengthSeconds;	// as reported by the reader; only for progress

	/* How long loading took (logged when done, to tune the per-frame budget). */
	float		m_fDecodeSeconds;
	int			m_iDecodeCalls;

	/* The bin being accumulated. */
	int			m_iCurPeak;
	double		m_fCurSumSquares;
	int			m_iCurFrames;

	int			m_iMaxPeak;
	int			m_iMaxRms;

	/* m_Levels[0] has one bin per m_iBinFrames frames; m_Levels[n+1][i] is the
	 * max of m_Levels[n][2i] and m_Levels[n][2i+1]. */
	std::vector<Bin> m_Levels[MAX_LEVELS];
	std::vector<char> m_Buf;
};

/*
 * WaveformDisplay - draws the shared WaveformData behind a NoteField in the
 * editor.  The visible range is drawn in rows about the same height (so the
 * cost depends on the screen height, not on the zoom or song length).  Rows are
 * cut on the music's time grid, so the offset, BPMs and stops move the
 * waveform without reshaping it, and every row is mapped music time ->
 * beat through the song's timing data (so BPM changes and stops line up).
 * Everything is sent to the renderer as a single batch of quads.
 */
class WaveformDisplay : public Actor
{
public:
	WaveformDisplay();

	/* Called by the NoteField right before Draw(). */
	void SetDrawRange( PlayerNumber pn, float fFirstBeat, float fLastBeat, float fYReverseOffsetPixels );
	virtual void DrawPrimitives();

private:
	float BeatToYPos( float fBeat ) const;
	float BinToYPos( const Song &song, int iBin, double fBinsPerSecond ) const;
	/* One quad pair (peak halo + RMS body) per visible row; widths are the
	 * values times fPeakScale / fRmsScale (clamped to 1), times half the width. */
	void AddRows( const WaveformData &data, const Song &song, float fPeakScale, float fRmsScale );
	void AddQuad( float fLeft, float fTop, float fRight, float fBottom, const RageColor &c );
	void FlushQuads();	// draw everything added with AddQuad()

	PlayerNumber	m_PlayerNumber;
	float			m_fFirstBeat;
	float			m_fLastBeat;
	float			m_fYReverseOffsetPixels;
	float			m_fBrightness;		// AddQuad() multiplies colours by this; set each frame
	std::vector<RageSpriteVertex> m_Verts;	// reused every frame
};

#endif
