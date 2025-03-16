#define LINE_THICKNESS                (0.2f)
#define BLOCK_SIZE                    (256)
#define ALPHA                         (0.6)
#define MAX_DISPLAY_SAMPLE_SIZE       (2000)

#include "global.h"
#include "WaveformDisplay.h"
#include "RageDisplay.h"
#include "RageLog.h"
#include "GameState.h"
#include "ArrowEffects.h"

WaveformDisplay::WaveformDisplay()
	: m_height(0),
	  m_bEnableFilter(true),
	  m_PlayerNumber(PLAYER_1),
	  m_fUserDurationRaw(0.f),
	  m_iBlockStart(0),
	  m_iBlockEnd(0),
	  m_totalBlocks(0),
	  m_fUserRequestedStart(0),
	  m_fUserRequestedEnd(0),
	  m_fClampedStart(0),
	  m_fClampedEnd(0),
	  m_fBaseY(0),
	  m_iDynamicBlockSize(BLOCK_SIZE),
	  m_bInit(false),
	  m_fYReverseOffsetPixels(720)
{
}

void WaveformDisplay::Initialize(Song *pSong)
{
	if(m_bInit) return;
	if(pSong)
	{
		m_pSong = pSong;
		m_Sound.Load(m_pSong->GetMusicPath());
		PrecomputeWaveform();
		m_bInit = true;
	}
}

void WaveformDisplay::SetPlayerNumber(PlayerNumber pn)
{
	m_PlayerNumber = pn;
}

int NextPowerOfTwo(int x)
{
	if (x < 1) return 1;
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return x + 1;
}

void WaveformDisplay::AdjustBlockSize(float duration)
{
	if (duration <= 0) return;

	int sampleRate = m_Sound.GetSampleRate();
	int totalSamples = static_cast<int>(duration * sampleRate);

	const int minBlockSize = 2;
	const int maxBlockSize = 16384;
	const int targetBlocks = 1000;

	int blockSize = totalSamples / targetBlocks;
	blockSize = std::max(minBlockSize, std::min(blockSize, maxBlockSize));

	int newBlockSize = NextPowerOfTwo(blockSize);

	if (m_iDynamicBlockSize != newBlockSize)
	{
		m_iDynamicBlockSize = newBlockSize;
		RebuildEnvelope();
	}
}

void WaveformDisplay::RebuildEnvelope()
{
	BuildEnvelope(m_LeftChannelFull, m_LeftEnvelopeFull);
	BuildEnvelope(m_RightChannelFull, m_RightEnvelopeFull);

	if (m_bEnableFilter)
	{
		BuildEnvelope(m_LeftChannelFiltered, m_LeftEnvelopeFiltered);
		BuildEnvelope(m_RightChannelFiltered, m_RightEnvelopeFiltered);
	}

	// LOG->Trace("Rebuilt envelope with BLOCK_SIZE = %d", m_iDynamicBlockSize);
}

void WaveformDisplay::ApplyHighPassFilter(const std::vector<int16_t> &inData,
										  std::vector<int16_t> &outData,
										  float strength)
{
	// Differential coefficient (simple high-pass), you can increase or decrease it as needed
	// Strength can be used to do "how much" feedback
	// For example out[n] = in[n] - α * in[n-1]
	// α roughly represents "cutoff frequency", the closer to 1 => the stronger Qualcomm
	float alpha = strength; // Between 0~1

	outData.clear();
	outData.resize(inData.size(), 0);

	if (inData.empty())
		return;

	outData[0] = inData[0];

	// Starting from i=1 do out[i] = in[i] - alpha * in[i-1]
	for (size_t i = 1; i < inData.size(); i++)
	{
		float sampleHP = (float)inData[i] - alpha * (float)inData[i - 1];
		if (sampleHP > 32767.f)
			sampleHP = 32767.f;
		if (sampleHP < -32768.f)
			sampleHP = -32768.f;
		outData[i] = (int16_t)sampleHP;
	}
}

void WaveformDisplay::BuildEnvelope(const std::vector<int16_t> &source, std::vector<MinMax> &envelope)
{
	int totalSamples = (int)source.size();
	m_totalBlocks = (totalSamples + m_iDynamicBlockSize - 1) / m_iDynamicBlockSize;
	envelope.resize(m_totalBlocks);

	for (int blockIndex = 0; blockIndex < m_totalBlocks; blockIndex++)
	{
		int start = blockIndex * m_iDynamicBlockSize;
		int end = std::min(start + m_iDynamicBlockSize, totalSamples);

		int16_t mn = 32767;
		int16_t mx = -32768;
		for (int i = start; i < end; i++)
		{
			int16_t val = source[i];
			if (val < mn) mn = val;
			if (val > mx) mx = val;
		}
		envelope[blockIndex].mn = mn;
		envelope[blockIndex].mx = mx;
	}
}

void WaveformDisplay::PrecomputeWaveform()
{
	float totalDuration = m_Sound.GetLengthSeconds();
	if (totalDuration <= 0)
	{
		LOG->Warn("Invalid sound length: %.2f", totalDuration);
		return;
	}

	// 1) First read out the entire audio file m_LeftChannelFull, m_RightChannelFull
	float startSecond = 0.f;
	float endSecond = totalDuration;

	if (!m_Sound.GetChannelWaveform(startSecond, endSecond,
									  m_LeftChannelFull, m_RightChannelFull))
	{
		LOG->Warn("GetChannelWaveform fail => fill silence");
		int sampleRate = m_Sound.GetSampleRate();
		int silentSamples = (int)(sampleRate * totalDuration);
		m_LeftChannelFull.assign(silentSamples, 0);
		m_RightChannelFull.assign(silentSamples, 0);
	}

	// 2) filter
	if (m_bEnableFilter)
	{
		m_LeftChannelFiltered.resize(m_LeftChannelFull.size());
		m_RightChannelFiltered.resize(m_RightChannelFull.size());

		ApplyHighPassFilter(m_LeftChannelFull, m_LeftChannelFiltered, 0.98f);
		ApplyHighPassFilter(m_RightChannelFull, m_RightChannelFiltered, 0.98f);
	}

	// 3) Create a "MinMax envelope" for both original and filtered data
	// Create an envelope first
	RebuildEnvelope();

	// LOG->Trace("Precompute done. total blocks = %d", m_totalBlocks);
	// By default, no range is displayed at first
	m_iBlockStart = 0;
	m_iBlockEnd = 0;
}

void WaveformDisplay::ExtractWaveformSegment(float firstBeat, float lastBeat, float startSecond, float duration)
{
	m_fFirstBeat = firstBeat;
	m_fLastBeat = lastBeat;

	m_fUserDurationRaw = duration;
	m_fUserRequestedStart = startSecond;
	m_fUserRequestedEnd = startSecond + duration;

	if (m_LeftChannelFull.empty()) return;

	float totalDuration = m_Sound.GetLengthSeconds();
	if (totalDuration <= 0) return;

	AdjustBlockSize(duration);

	float clampedStart = std::max(0.f, m_fUserRequestedStart);
	float clampedEnd = std::min(totalDuration, m_fUserRequestedEnd);

	m_fClampedStart = clampedStart;
	m_fClampedEnd = clampedEnd;
	int sampleRate = m_Sound.GetSampleRate();
	int totalSamples = (int)m_LeftChannelFull.size();

	int startIndex = (int)(clampedStart * sampleRate);
	int endIndex = (int)(clampedEnd * sampleRate);

	int totalBlocks = (totalSamples + m_iDynamicBlockSize - 1) / m_iDynamicBlockSize;
	m_iBlockStart = startIndex / m_iDynamicBlockSize;
	m_iBlockEnd = endIndex / m_iDynamicBlockSize;

	if (m_iBlockStart < 0) m_iBlockStart = 0;
	if (m_iBlockEnd < m_iBlockStart) m_iBlockEnd = m_iBlockStart;
	if (m_iBlockEnd > totalBlocks) m_iBlockEnd = totalBlocks;
	if (m_iBlockStart == m_iBlockEnd && m_iBlockEnd < totalBlocks) m_iBlockEnd++;
}

float WaveformDisplay::BeatToYPosition(float beat)
{
	float fYOffset = ArrowGetYOffset(m_PlayerNumber, 0, beat);
	return ArrowGetYPos(m_PlayerNumber, 0, fYOffset, m_fYReverseOffsetPixels);
}

std::vector<BPMSegment> WaveformDisplay::GetRelevantBPMSegments(float &firstBeat, float &lastBeat)
{
	std::vector<BPMSegment> relevantSegments;
	const std::vector<BPMSegment> &bpmSegments = m_pSong->GetBPMSegment();

	float totalMusicTime = m_Sound.GetLengthSeconds();
	relevantSegments.push_back({firstBeat, m_pSong->GetBPMAtBeat(firstBeat)});

	for (const auto &segment : bpmSegments)
	{
		float segmentTime = m_pSong->GetElapsedTimeFromBeat(segment.m_fStartBeat);
		if (segmentTime >= 0 && segmentTime <= totalMusicTime &&
			segment.m_fStartBeat > firstBeat && segment.m_fStartBeat <= lastBeat)
		{
			relevantSegments.push_back(segment);
		}
	}
	relevantSegments.push_back({lastBeat, m_pSong->GetBPMAtBeat(lastBeat)});

	return relevantSegments;
}

void WaveformDisplay::DrawEnvelopeRange(const std::vector<MinMax> &envelope,
										float waveHeight,
										float waveWidth,
										float offsetX,
										RageColor lineColor)
{
	int blockCount = m_iBlockEnd - m_iBlockStart;
	if (blockCount < 1) return;

	

	float totalMusicTime = m_Sound.GetLengthSeconds();
	float firstBeat = m_fFirstBeat;
	float lastBeat = m_fLastBeat;
	float firstBeatTime = m_pSong->GetElapsedTimeFromBeat(firstBeat);
	float lastBeatTime = m_pSong->GetElapsedTimeFromBeat(lastBeat);
	if (firstBeatTime < 0 || firstBeatTime > totalMusicTime)
	{
		firstBeat = m_pSong->GetBeatFromElapsedTime(0);
	}
	if (lastBeatTime < 0 || lastBeatTime > totalMusicTime)
	{
		lastBeat = m_pSong->GetBeatFromElapsedTime(totalMusicTime);
	}

	vector<BPMSegment> relevantSegments = GetRelevantBPMSegments(firstBeat, lastBeat);

	float prevBeat = firstBeat;
	float prevTime = m_pSong->GetElapsedTimeFromBeat(prevBeat);
	float prevY = m_fBaseY;

	int blockStart = m_iBlockStart;
	float totalTime = m_fClampedEnd - m_fClampedStart;
	for (size_t segIdx = 1; segIdx < relevantSegments.size(); segIdx++)
	{
		float segmentStartBeat = prevBeat;
		float segmentEndBeat = relevantSegments[segIdx].m_fStartBeat;
		float segmentStartTime = std::max(0.0f, m_pSong->GetElapsedTimeFromBeat(segmentStartBeat));
		float segmentEndTime = std::min(totalMusicTime, m_pSong->GetElapsedTimeFromBeat(segmentEndBeat));
		float segmentDuration = segmentEndTime - segmentStartTime;
		float timeRatio = segmentDuration / totalTime;
		float segmentEndY = BeatToYPosition(segmentEndBeat);
		float segmentHeight = fabs(segmentEndY - BeatToYPosition(segmentStartBeat));

		float blockCountPerSegment = std::max(1.0f, (float)(blockCount * timeRatio));
		float sampleSpacingY = segmentHeight / blockCountPerSegment;

		for (int i = 0; i < blockCountPerSegment; i++)
		{
			if (i > MAX_DISPLAY_SAMPLE_SIZE) return;
			int blockIndex = blockStart + i;
			if (blockIndex < 0 || blockIndex >= (int)envelope.size())
				continue;

			int16_t mn = envelope[blockIndex].mn;
			int16_t mx = envelope[blockIndex].mx;

			float minAmp = (float)mn / 32768.f;
			float maxAmp = (float)mx / 32768.f;
			float peakAmp = std::max(std::fabs(minAmp), std::fabs(maxAmp));
			if (peakAmp < 0.01f) continue;
			float currentSegmentY = prevY + (i * sampleSpacingY);

			float xMin = offsetX + minAmp * (waveWidth / 2);
			float xMax = offsetX + maxAmp * (waveWidth / 2);

			RageSpriteVertex v[2];
			v[0].p = RageVector3(xMin, currentSegmentY, 0);
			v[0].c = lineColor;
			v[1].p = RageVector3(xMax, currentSegmentY, 0);
			v[1].c = lineColor;

			DISPLAY->DrawLineStrip(v, 2, LINE_THICKNESS);
		}
		blockStart += (blockCountPerSegment - 1);
		prevBeat = segmentEndBeat;
		prevTime = segmentEndTime;
		prevY = BeatToYPosition(segmentEndBeat);
	}
}

void WaveformDisplay::DrawBlackRectangle(float bottomY, float topY, float leftX, float rightX, float alpha)
{
	RageSpriteVertex v[4];
	v[0].p = RageVector3(leftX, bottomY, 0);
	v[0].c = RageColor(0, 0, 0, alpha);
	v[1].p = RageVector3(rightX, bottomY, 0);
	v[1].c = RageColor(0, 0, 0, alpha);
	v[2].p = RageVector3(rightX, topY, 0);
	v[2].c = RageColor(0, 0, 0, alpha);
	v[3].p = RageVector3(leftX, topY, 0);
	v[3].c = RageColor(0, 0, 0, alpha);

	DISPLAY->DrawQuad(v);
}

void WaveformDisplay::Update(float deltaTime)
{
	Actor::Update(deltaTime);
}

void WaveformDisplay::DrawPrimitives()
{
	if (!m_bInit) return;
	if (m_LeftEnvelopeFull.empty() || m_iBlockStart >= m_iBlockEnd)
		return;

	Actor::SetRenderStates();
	DISPLAY->SetBlendMode(BLEND_NORMAL);
	DISPLAY->ClearAllTextures();

	// float m_fYReverseOffsetPixels = 720; // 350;

	const float fYPos = BeatToYPosition(m_fFirstBeat);
	const float fYPos2 = BeatToYPosition(m_fLastBeat);
	const float waveHeight = fabs(fYPos2 - fYPos);
	const float waveWidth = 240.f;
	const float halfRect = waveWidth / 2.0f + 10.0f;
	DrawBlackRectangle(-600.f, 600.f, -halfRect, halfRect, ALPHA);

	float userTotalSec = std::max(0.001f, m_fUserRequestedEnd - m_fUserRequestedStart);
	float realTotalSec = std::max(0.0f, m_fClampedEnd - m_fClampedStart);
	float pixelsPerSec = waveHeight / userTotalSec;
	float negativePartDuration = std::max(0.0f, -m_fUserRequestedStart);
	float negativePartPixel = negativePartDuration * pixelsPerSec;
	float realPartPixel = realTotalSec * pixelsPerSec;

	m_fBaseY = fYPos + std::max(0.0f, negativePartPixel);

	// part 2: Audio Interpretation
	if (realPartPixel > 0)
	{
		DrawEnvelopeRange(m_LeftEnvelopeFull, waveHeight, waveWidth, 0.f, RageColor(0, 1, 0, 1));
		DrawEnvelopeRange(m_RightEnvelopeFull, waveHeight, waveWidth, 0.f, RageColor(1, 1, 0, 1));

		if (m_bEnableFilter)
		{
			DrawEnvelopeRange(m_LeftEnvelopeFiltered, waveHeight, waveWidth, 0.f, RageColor(1, 0, 0, 1));
			DrawEnvelopeRange(m_RightEnvelopeFiltered, waveHeight, waveWidth, 0.f, RageColor(1, 0, 0, 1));
		}
	}

	const float fYPos3 = BeatToYPosition(GAMESTATE->m_fSongBeat);
	RageSpriteVertex v[2];
	v[0].p = RageVector3(-waveWidth/2, fYPos3, 0);
	v[0].c = RageColor(0, 0, 1, 1);
	v[1].p = RageVector3(waveWidth/2, fYPos3, 0);
	v[1].c = RageColor(0, 0, 1, 1);
	DISPLAY->DrawLineStrip(v, 2, 1.0f);
}
