#ifndef WAVEFORM_DISPLAY_H
#define WAVEFORM_DISPLAY_H

#include "Actor.h"
#include "RageSound.h"
#include "PlayerNumber.h"
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

	float m_fUserStartRaw;
	float m_fUserDurationRaw;
	bool m_bInit;
	float m_fYReverseOffsetPixels;
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
	float m_fUserRequestedStart;
	float m_fUserRequestedEnd;
	float m_fClampedStart;
	float m_fClampedEnd;

	float m_fBaseY;
	float m_fSegmentYStep;

	int m_iDynamicBlockSize;
	int m_prevBlockSize;

	void PrecomputeWaveform();
	void RebuildEnvelope();
	void BuildEnvelope(const std::vector<int16_t> &source, std::vector<MinMax> &envelope);
	void ApplyHighPassFilter(const std::vector<int16_t> &inData, std::vector<int16_t> &outData, float strength);
	void AdjustBlockSize(float duration);
	void ExtractWaveformSegment_Actual(float startSecond, float duration);
	void DrawEnvelopeRange(const std::vector<MinMax> &envelope,
						   float waveHeight,
						   float waveWidth,
						   float offsetX,
						   RageColor lineColor);
	void DrawBlackRectangle(float bottomY, float topY, float leftX, float rightX, float alpha);
};

#endif
