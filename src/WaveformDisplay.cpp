#define LINE_THICKNESS                (0.2f)
#define BLOCK_SIZE                    (256)
#define ALPHA                         (0.6)

#include "global.h"
#include "WaveformDisplay.h"
#include "RageDisplay.h"
#include "RageLog.h"
#include "GameState.h"
#include "ArrowEffects.h"

WaveformDisplay::WaveformDisplay()
  : m_height(0),
    m_pSound(nullptr),
    m_bEnableFilter(true),
    m_PlayerNumber(PLAYER_INVALID),
    m_fUserStartRaw(0.f),
    m_fUserDurationRaw(0.f),
    m_iBlockStart(0),
    m_iBlockEnd(0),
    m_totalBlocks(0),
    m_fUserRequestedStart(0),
    m_fUserRequestedEnd(0),
    m_fClampedStart(0),
    m_fClampedEnd(0),
    m_fBaseY(0),
    m_fSegmentYStep(0),
    m_iDynamicBlockSize(BLOCK_SIZE),
    m_prevBlockSize(BLOCK_SIZE)
{
}

void WaveformDisplay::SetSound(RageSound *sound)
{
    m_pSound = sound;
    if (m_pSound) PrecomputeWaveform();
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
    if (!m_pSound || duration <= 0) return;

    int sampleRate = m_pSound->GetSampleRate();
    int totalSamples = static_cast<int>(duration * sampleRate);

    const int minBlockSize = 2;
    const int maxBlockSize = 1024;
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
    BuildEnvelope(m_LeftChannelFull,       m_LeftEnvelopeFull);
    BuildEnvelope(m_RightChannelFull,      m_RightEnvelopeFull);

    if(m_bEnableFilter)
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
    //Strength can be used to do "how much" feedback
    // For example out[n] = in[n] - α * in[n-1]
    // α roughly represents "cutoff frequency", the closer to 1 => the stronger Qualcomm
    float alpha = strength; // Between 0~1

    outData.clear();
    outData.resize(inData.size(), 0);

    if(inData.empty())
        return;

    outData[0] = inData[0];

    // Starting from i=1 do out[i] = in[i] - alpha * in[i-1]
    for(size_t i = 1; i < inData.size(); i++)
    {
        float sampleHP = (float)inData[i] - alpha * (float)inData[i - 1];
        if(sampleHP > 32767.f)  sampleHP = 32767.f;
        if(sampleHP < -32768.f) sampleHP = -32768.f;
        outData[i] = (int16_t)sampleHP;
    }
}

void WaveformDisplay::BuildEnvelope(const std::vector<int16_t>& source, std::vector<MinMax>& envelope)
{
    int totalSamples = (int)source.size();
    m_totalBlocks = (totalSamples + m_iDynamicBlockSize - 1) / m_iDynamicBlockSize;
    envelope.resize(m_totalBlocks);

    for(int blockIndex = 0; blockIndex < m_totalBlocks; blockIndex++)
    {
        int start = blockIndex * m_iDynamicBlockSize;
        int end   = std::min(start + m_iDynamicBlockSize, totalSamples);

        int16_t mn = 32767;
        int16_t mx = -32768;
        for(int i = start; i < end; i++)
        {
            int16_t val = source[i];
            if(val < mn) mn = val;
            if(val > mx) mx = val;
        }
        envelope[blockIndex].mn = mn;
        envelope[blockIndex].mx = mx;
    }
}

void WaveformDisplay::PrecomputeWaveform()
{
    if (!m_pSound) return;
    float totalDuration = m_pSound->GetLengthSeconds();
    if (totalDuration <= 0)
    {
        LOG->Warn("Invalid sound length: %.2f", totalDuration);
        return;
    }

    // 1) First read out the entire audio file m_LeftChannelFull, m_RightChannelFull
    float startSecond = 0.f;
    float endSecond = totalDuration;

    if (!m_pSound->GetChannelWaveform(startSecond, endSecond,
                                      m_LeftChannelFull, m_RightChannelFull))
    {
        LOG->Warn("GetChannelWaveform fail => fill silence");
        int sampleRate = m_pSound->GetSampleRate();
        int silentSamples = (int)(sampleRate * totalDuration);
        m_LeftChannelFull.assign(silentSamples, 0);
        m_RightChannelFull.assign(silentSamples, 0);
    }

    // 2) filter
    if(m_bEnableFilter)
    {
        m_LeftChannelFiltered.resize(m_LeftChannelFull.size());
        m_RightChannelFiltered.resize(m_RightChannelFull.size());

        ApplyHighPassFilter(m_LeftChannelFull,  m_LeftChannelFiltered, 0.98f);
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
    m_fUserStartRaw    = startSecond;
    m_fUserDurationRaw = duration;
    if (m_LeftChannelFull.empty()) return;

    float totalDuration = m_pSound->GetLengthSeconds();
    if (totalDuration <= 0) return;

    // Default remains as usual
    // ExtractWaveformSegment_Actual(startSecond, duration);
}

void WaveformDisplay::ExtractWaveformSegment_Actual(float startSecond, float duration)
{
    float totalDuration = (m_pSound)? m_pSound->GetLengthSeconds() : 0.f;
    if(totalDuration<=0.f) return;

    AdjustBlockSize(duration);

    m_fUserRequestedStart = startSecond;
    m_fUserRequestedEnd   = startSecond + duration;

    float clampedStart = std::max(0.f, m_fUserRequestedStart);
    float clampedEnd   = std::min(totalDuration, m_fUserRequestedEnd);

    m_fClampedStart = clampedStart;
    m_fClampedEnd   = clampedEnd;
    int sampleRate = m_pSound->GetSampleRate();
    int totalSamples = (int)m_LeftChannelFull.size();

    int startIndex = (int)(clampedStart * sampleRate);
    int endIndex   = (int)(clampedEnd   * sampleRate);

    int totalBlocks = (totalSamples + m_iDynamicBlockSize - 1) / m_iDynamicBlockSize;
    m_iBlockStart = startIndex / m_iDynamicBlockSize;
    m_iBlockEnd   = endIndex   / m_iDynamicBlockSize;

    if(m_iBlockStart<0)             m_iBlockStart=0;
    if(m_iBlockEnd<m_iBlockStart)   m_iBlockEnd=m_iBlockStart;
    if(m_iBlockEnd>totalBlocks)     m_iBlockEnd=totalBlocks;
    if(m_iBlockStart==m_iBlockEnd && m_iBlockEnd<totalBlocks)
        m_iBlockEnd++;
}

void WaveformDisplay::DrawEnvelopeRange(const std::vector<MinMax>& envelope,
                                        float waveHeight,
                                        float waveWidth,
                                        float offsetX,
                                        RageColor lineColor)
{
    int blockCount = m_iBlockEnd - m_iBlockStart;
    if (blockCount < 1) return;

    float yStep = m_fSegmentYStep; // 使用新計算的 yStep
    float baseY = m_fBaseY;

    for (int i = 0; i < blockCount; i++)
    {
        int blockIndex = m_iBlockStart + i;
        if (blockIndex < 0 || blockIndex >= (int)envelope.size())
            continue;

        int16_t mn = envelope[blockIndex].mn;
        int16_t mx = envelope[blockIndex].mx;

        float minAmp = (float)mn / 32768.f;
        float maxAmp = (float)mx / 32768.f;
        float peakAmp = std::max(std::fabs(minAmp), std::fabs(maxAmp));
        if (peakAmp < 0.01f) continue;

        float y = baseY + i * yStep;

        float xMin = offsetX + minAmp * (waveWidth / 2);
        float xMax = offsetX + maxAmp * (waveWidth / 2);
        if (xMin > xMax) std::swap(xMin, xMax);

        RageSpriteVertex v[2];
        v[0].p = RageVector3(xMin, y, 0);
        v[0].c = lineColor;
        v[1].p = RageVector3(xMax, y, 0);
        v[1].c = lineColor;

        DISPLAY->DrawLineStrip(v, 2, LINE_THICKNESS);
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
    // As a result, a certain toy artist reads scroll speed.
    if (m_PlayerNumber != PLAYER_INVALID)
    {
        float fScrollSpeed = GAMESTATE->m_CurrentPlayerOptions[m_PlayerNumber].m_fScrollSpeed;
        if (fScrollSpeed < 0.001f) // 0 Avoidance
            fScrollSpeed = 1.f;

        float finalDuration = m_fUserDurationRaw / fScrollSpeed;

        float startSec = std::max(0.f, m_fUserStartRaw);
        float endSec = startSec + std::max(0.f, finalDuration);

        // Recalculation blocks
        ExtractWaveformSegment_Actual(startSec, endSec - startSec);
    }
    else
    {
        ExtractWaveformSegment_Actual(m_fUserStartRaw, m_fUserDurationRaw);
    }
}

void WaveformDisplay::DrawBeatBar( const float fBeat )
{
	bool bIsMeasure = fmodf( fBeat, (float)BEATS_PER_MEASURE ) == 0;
	int iMeasureIndex = (int)fBeat / BEATS_PER_MEASURE;
	int iMeasureNoDisplay = iMeasureIndex+1;

	NoteType nt = BeatToNoteType( fBeat );
    float m_fYReverseOffsetPixels = 720;//350;//338;
 
	const float fYOffset	= ArrowGetYOffset( PLAYER_1, 0, fBeat );
	const float fYPos		= ArrowGetYPos(	PLAYER_1, 0, fYOffset, m_fYReverseOffsetPixels );

	float fAlpha;
	int iState;
	
	if( bIsMeasure )
	{
		fAlpha = 1;
		iState = 0;
	}
	else
	{
		float fScrollSpeed = GAMESTATE->m_CurrentPlayerOptions[PLAYER_1].m_fScrollSpeed;
		switch( nt )
		{
		default:	ASSERT(0);
		case NOTE_TYPE_4TH:	fAlpha = 1;										iState = 1;	break;
		case NOTE_TYPE_8TH:	fAlpha = SCALE(fScrollSpeed,1.f,2.f,0.f,1.f);	iState = 2;	break;
		case NOTE_TYPE_16TH:fAlpha = SCALE(fScrollSpeed,2.f,4.f,0.f,1.f);	iState = 3;	break;
		}
		CLAMP( fAlpha, 0, 1 );
	}

    float fWidth = 200;// GetWidth();
	float fFrameWidth = m_sprBars.GetUnzoomedWidth();

	// m_sprBars.SetX( 0 );
	// m_sprBars.SetY( fYPos );
	// m_sprBars.SetDiffuse( RageColor(1,0,0,fAlpha) );
	// m_sprBars.SetState( iState );
	// m_sprBars.SetCustomTextureRect( RectF(0,SCALE(iState,0.f,4.f,0.f,1.f), fWidth/fFrameWidth, SCALE(iState+1,0.f,4.f,0.f,1.f)) );
	// m_sprBars.SetZoomX( fWidth/m_sprBars.GetUnzoomedWidth() );
	// m_sprBars.Draw();
    //float fWidth = 200;//GetWidth(); // NoteField 的寬度
    float halfWidth = fWidth / 2.0f;

    if(!bIsMeasure)return;
    // 使用 DrawLineStrip 來畫線
    RageColor  lineColor = RageColor(0, 0, 1, 1);
    RageSpriteVertex line[2];
    line[0].p = RageVector3(-halfWidth, fYPos, 0); // 左側點
    line[0].c = lineColor;
    line[1].p = RageVector3(halfWidth, fYPos, 0); // 右側點
    line[1].c = lineColor;
    DISPLAY->DrawLineStrip(line, 2, 1.0f);
}

void WaveformDisplay::DrawPrimitives()
{
    if (m_LeftEnvelopeFull.empty() || m_iBlockStart >= m_iBlockEnd)
        return;

    Actor::SetRenderStates();
    DISPLAY->SetBlendMode(BLEND_NORMAL);
    DISPLAY->ClearAllTextures();
    
    float m_fYReverseOffsetPixels = 720;//350;//338;
 
	const float fYOffset	= ArrowGetYOffset( PLAYER_1, 0, m_fFirstBeat );
	const float fYPos		= ArrowGetYPos(	PLAYER_1, 0, fYOffset, m_fYReverseOffsetPixels );
    const float fYOffset2	= ArrowGetYOffset( PLAYER_1, 0, m_fLastBeat );
	const float fYPos2		= ArrowGetYPos(	PLAYER_1, 0, fYOffset2, m_fYReverseOffsetPixels );

    const float waveHeight = fabs(fYPos2-fYPos);
    const float waveWidth = 200.f;
    DrawBlackRectangle(-400.f, 400.f, -110.f, 110.f, ALPHA);

    float userTotalSec = m_fUserRequestedEnd - m_fUserRequestedStart;
    if (userTotalSec <= 0) userTotalSec = 0.001f;

    float realTotalSec = m_fClampedEnd - m_fClampedStart;
    if (realTotalSec < 0) realTotalSec = 0.f;
    float pixelsPerSec = waveHeight / userTotalSec;
    float negativePartDuration = std::max(0.0f, -m_fUserRequestedStart);
    float negativePartPixel = negativePartDuration * pixelsPerSec;

    // 計算音訊部分
    float realPartPixel = realTotalSec * pixelsPerSec;

    // 計算超出末端的空白
    float afterPartDuration = std::max(0.0f, m_fUserRequestedEnd - m_fClampedEnd);
    float afterPartPixel = afterPartDuration * pixelsPerSec;

    // 設定起始 Y 座標
    




    float currentY = fYPos;//-waveHeight / 2;
    // **第一部分：負數區間 (空白)**
    if (negativePartPixel > 0)
    {
        // DrawBlackRectangle(currentY, currentY + negativePartPixel, -110.f, 110.f, ALPHA);
        currentY += negativePartPixel;
    }

    // **第二部分：音訊區間**
    if (realPartPixel > 0)
    {
        int blockCount = m_iBlockEnd - m_iBlockStart;
        if (blockCount <= 1) blockCount = 2;

        // 計算這段的 yStep
        float yStepSegment = realPartPixel / (float)(blockCount - 1);
        m_fBaseY = currentY;
        m_fSegmentYStep = yStepSegment;

        DrawEnvelopeRange(m_LeftEnvelopeFull, waveHeight, waveWidth, 0.f, RageColor(0, 1, 0, 1));
        DrawEnvelopeRange(m_RightEnvelopeFull, waveHeight, waveWidth, 0.f, RageColor(1, 1, 0, 1));

        if (m_bEnableFilter)
        {
            DrawEnvelopeRange(m_LeftEnvelopeFiltered, waveHeight, waveWidth, 0.f, RageColor(1, 0, 0, 1));
            DrawEnvelopeRange(m_RightEnvelopeFiltered, waveHeight, waveWidth, 0.f, RageColor(1, 0, 0, 1));
        }

        currentY += realPartPixel;
    }

    // **第三部分：超出末端的空白**
    if (afterPartPixel > 0)
    {
        // DrawBlackRectangle(currentY, currentY + afterPartPixel, -110.f, 110.f, ALPHA);
        currentY += afterPartPixel;
    }

    // 計算每 8 分的高度
    int numLines = 8;
    float stepSize = waveHeight / numLines;

    // 畫橫線
    // for (int i = 0; i <= numLines; i++)
    // {
    //     float yPos = -waveHeight / 2 + i * stepSize;

    //     RageSpriteVertex line[2];
    //     line[0].p = RageVector3(-waveWidth / 2, yPos, 0);
    //     line[0].c = RageColor(1, 0,0, 1);  // 灰色線
    //     line[1].p = RageVector3(waveWidth / 2, yPos, 0);
    //     line[1].c = RageColor(1,0, 0, 1);

    //     DISPLAY->DrawLineStrip(line, 2, 0.1f);
    // }

    
    float fStartDrawingMeasureBars = max( 0, froundf(m_fFirstBeat-0.25f,0.25f) );
        for( float f=fStartDrawingMeasureBars; f<m_fLastBeat; f+=0.25f )
            DrawBeatBar( f );
}
