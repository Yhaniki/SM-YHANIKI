#ifndef WAVEFORM_DISPLAY_H
#define WAVEFORM_DISPLAY_H

#include "Actor.h"
#include "RageSound.h"
#include "PlayerNumber.h"
#include "TimingData.h"
#include "Song.h"
#include <vector>

typedef struct
{
	int16_t mn;
	int16_t mx;
} MinMax;

class WaveformDisplay : public Actor
{
public:
	WaveformDisplay();
	~WaveformDisplay() override {}

	void Initialize(Song *pSong);
	void SetPlayerNumber(PlayerNumber pn);
	void Update(float deltaTime) override;
	void DrawPrimitives() override;
	void ExtractWaveformSegment(float firstBeat, float lastBeat, float startSecond, float duration);
	void SetHeight(float height) { m_height = height; };
	void SetDisplayRange(float startSec, float endSec, float visTopY, float visBottomY);
	void SetYReverseOffsetPixels(float pixels) {m_fYReverseOffsetPixels = pixels;};
private:
	float m_height;
	bool m_bEnableFilter;
	float m_fFirstBeat;
	float m_fLastBeat;
	PlayerNumber m_PlayerNumber;

	float m_fUserDurationRaw;
	float m_fUserRequestedStart;
	float m_fUserRequestedEnd;
	float m_fClampedStart;
	float m_fClampedEnd;
	bool m_bInit;
	float m_fYReverseOffsetPixels;
	Song* m_pSong;
	RageSound m_Sound;

	std::vector<int16_t> m_LeftChannelFull;
	std::vector<int16_t> m_RightChannelFull;
	std::vector<int16_t> m_LeftChannelFiltered;
	std::vector<int16_t> m_RightChannelFiltered;
	std::vector<MinMax> m_LeftEnvelopeFull;
	std::vector<MinMax> m_RightEnvelopeFull;
	std::vector<MinMax> m_LeftEnvelopeFiltered;
	std::vector<MinMax> m_RightEnvelopeFiltered;

	int m_iBlockStart;
	int m_iBlockEnd;
	int m_totalBlocks;
	float m_fBaseY;
	float m_fSegmentYStep;
	int m_iDynamicBlockSize;

	// Seconds to shift the PCM look-up by so the waveform lines up with the real
	// music.  MP3s without a Xing/Info header decode ~one MPEG frame early, which
	// draws the waveform ahead of the note grid; this is that one-frame duration
	// for such files and 0 for everything else.  See Initialize().
	float m_fWaveAlignSeconds;

	void PrecomputeWaveform();
	void RebuildEnvelope();
	void BuildEnvelope(const std::vector<int16_t> &source, std::vector<MinMax> &envelope);
	void ApplyHighPassFilter(const std::vector<int16_t> &inData, std::vector<int16_t> &outData, float strength);
	void AdjustBlockSize(float duration);
	void DrawEnvelopeRange(const std::vector<MinMax> &envelope,
						   float waveHeight,
						   float waveWidth,
						   float offsetX,
						   RageColor lineColor);
	void DrawBlackRectangle(float bottomY, float topY, float leftX, float rightX, float alpha);
	float BeatToYPosition(float beat);
	std::vector<BPMSegment> GetRelevantBPMSegments(float firstBeat, float lastBeat);
	std::vector<StopSegment> GetRelevantStopSegments(float firstBeat, float lastBeat);
};

#endif
