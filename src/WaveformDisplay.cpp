#include "global.h"
#include "WaveformDisplay.h"
#include "RageDisplay.h"
#include "RageLog.h"

WaveformDisplay::WaveformDisplay() : m_pSound(nullptr), m_MaxSamples(512) {}

void WaveformDisplay::SetSound(RageSound* sound) {
    m_pSound = sound;
     if (m_pSound) {
        PrecomputeWaveform();
    }
}
void WaveformDisplay::PrecomputeWaveform()
{
    if (!m_pSound)
        return;

    LOG->Trace("Precomputing waveform for: %s", m_pSound->GetLoadedFilePath().c_str());

    float totalDuration = m_pSound->GetLengthSeconds();
    if (totalDuration <= 0)
    {
        LOG->Warn("Invalid sound length: %.2f", totalDuration);
        return;
    }

    // 從 0 秒一路解到音檔結束
    float startSecond = 0.0f;
    float endSecond   = totalDuration;

    m_LeftChannelFull.clear();
    m_RightChannelFull.clear();

    // 直接調用剛剛在 RageSound 裡改寫的函式
    if (m_pSound->GetChannelWaveform(startSecond, endSecond,
                                     m_LeftChannelFull, m_RightChannelFull))
    {
        LOG->Trace("Successfully precomputed waveform: %d samples",
                   (int)m_LeftChannelFull.size());
    }
    else
    {
        // 解碼失敗就放一些空的零
        LOG->Warn("Failed to precompute waveform, filling with silence");
        int silentSamples = static_cast<int>(m_pSound->GetSampleRate() * totalDuration);
        m_LeftChannelFull.assign(silentSamples, 0);
        m_RightChannelFull.assign(silentSamples, 0);
    }
}

void WaveformDisplay::ExtractWaveformSegment(float startSecond, float duration) {
    if (m_LeftChannelFull.empty() || m_RightChannelFull.empty()) {
        LOG->Warn("Waveform data not available");
        return;
    }

    int sampleRate = m_pSound->GetSampleRate();
    int startIndex = static_cast<int>(startSecond * sampleRate);
    int endIndex = startIndex + static_cast<int>(duration * sampleRate);

    if (startIndex < 0) startIndex = 0;
    if (endIndex > (int)m_LeftChannelFull.size()) endIndex = (int)m_LeftChannelFull.size();

    m_LeftChannel.assign(m_LeftChannelFull.begin() + startIndex, m_LeftChannelFull.begin() + endIndex);
    m_RightChannel.assign(m_RightChannelFull.begin() + startIndex, m_RightChannelFull.begin() + endIndex);
}

void WaveformDisplay::Update(float deltaTime) {
    // if (m_pSound && m_pSound->IsPlaying()) {
    //     ProcessAudioData();
    // }
    if (m_pSound ) {
        ProcessAudioData();
    }
    Actor::Update(deltaTime);
}
// LOG->Trace("+++++++WaveformDisplay::DrawPrimitives() bytesRead %d\n ",bytesRead);
void WaveformDisplay::ProcessAudioData() {
     if (!m_pSound) return;

    float currentTime = m_pSound->GetPositionSeconds();
    if (currentTime < 0) currentTime = 0; // 確保時間合法

    float duration = 2.0f; // 取 5 秒內的波形
    ExtractWaveformSegment(currentTime, duration);
}
void FindMinMax(const std::vector<int16_t> &data, int start, int end, int16_t &outMin, int16_t &outMax)
{
    int16_t mn = 32767;
    int16_t mx = -32768;
    for(int i=start; i<end; i++)
    {
        int16_t val = data[i];
        if(val < mn) mn=val;
        if(val > mx) mx=val;
    }
    outMin = mn;
    outMax = mx;
}
static const float LINE_THICKNESS = 0.2f; 
void WaveformDisplay::DrawSingleChannelEnvelope(const std::vector<int16_t> &samples,
                                                float waveHeight,
                                                float waveWidth,
                                                float offsetX,
                                                RageColor lineColor)
{
    if(samples.empty())
        return;

    // 1) 每區段有多少樣本 => blockSize
    //   不需下採樣整個檔案，但繪圖時只畫 blocks
    static const int BLOCK_SIZE = 64; 
    int total = (int)samples.size();
    int numBlocks = (total + BLOCK_SIZE - 1) / BLOCK_SIZE; // ceil

    // 2) Y 軸設定
    float topY = -waveHeight/2;
    // numBlocks - 1 => 分成 (numBlocks-1) 段
    if(numBlocks < 2) 
        return;
    float yStep = waveHeight / (float)(numBlocks-1);

    // 3) 依序計算每個區段 [i*blockSize, (i+1)*blockSize) 的 min、max
    //   然後轉 float(-1..+1) => X_min, X_max
    for(int blockIndex=0; blockIndex<numBlocks; blockIndex++)
    {
        int start = blockIndex * BLOCK_SIZE;
        int end   = std::min(start + BLOCK_SIZE, total);

        // 取區段的 min, max
        int16_t mn, mx;
        FindMinMax(samples, start, end, mn, mx);

        // 若整段都接近 0，也可直接略過
        if(mn == 0 && mx == 0)
            continue; 

        // 轉 float
        float minAmp = (float)mn / 32768.f; // => -1..+1
        float maxAmp = (float)mx / 32768.f;

        // Y
        float y = topY + blockIndex*yStep;

        // X_min, X_max
        float xMin = offsetX + minAmp*(waveWidth/2);
        float xMax = offsetX + maxAmp*(waveWidth/2);

        float peakAmp = std::max(std::fabs(minAmp), std::fabs(maxAmp));
        if (peakAmp < 0.01f) // 門檻由你決定 0.01 => 1%
            continue; // 不畫
        // 這裡簡單畫一條 (xMin~xMax, y) 的橫線
        // 如果你想讓 min<0 max>0，就會從左到右
        // 也可以先做 if(xMin>xMax) swap
        if(xMin > xMax)
            std::swap(xMin, xMax);

        // 建兩頂點
        RageSpriteVertex v[2];
        v[0].p = RageVector3(xMin, y, 0);
        v[0].c = lineColor;
        v[1].p = RageVector3(xMax, y, 0);
        v[1].c = lineColor;

        DISPLAY->DrawLineStrip(v, 2, LINE_THICKNESS);
    }
}

void WaveformDisplay::DrawSingleChannel(const std::vector<int16_t> &samples,
                                        float waveHeight,
                                        float waveWidth,
                                        float offsetX,
                                        RageColor lineColor)
{
    // 如果沒資料，直接不畫
    if(samples.empty()) 
        return;

    // 1) 下採樣
    static const int MAX_POINTS = 2000; 
    int total = (int)samples.size();
    int step = 1;
    if(total > MAX_POINTS)
        step = total / MAX_POINTS;

    // 2) 轉成 float(-1~+1)
    std::vector<float> norm;
    norm.reserve(total/step + 1);
    for(int i=0; i<total; i+=step)
    {
        float amp = (float)samples[i] / 32768.0f;
        norm.push_back(amp);
    }
    if(norm.size() < 2)
        return;

    // 3) 算 Y 步長
    float topY = -waveHeight / 2.0f;
    float yStep = waveHeight / (norm.size() - 1.0f);

    // 4) 線粗細
    static const float LINE_THICKNESS = 0.2f; 

    // 5) 畫
    for(size_t i=0; i<norm.size(); i++)
    {
        float amp = norm[i];
        // 允許一定閾值才畫線(避免 amp=0.0f 也畫)
        if(std::fabs(amp) < 0.01f) 
            continue;

        float y = topY + i*yStep;
        // x0 = offsetX (中心線)
        // x1 = offsetX + amp*(waveWidth/2)
        float x0 = offsetX;
        float x1 = offsetX + amp*(waveWidth/2.0f);

        RageSpriteVertex v[2];
        v[0].p = RageVector3(x0, y, 0);
        v[1].p = RageVector3(x1, y, 0);
        v[0].c = lineColor;
        v[1].c = lineColor;

        DISPLAY->DrawLineStrip(v, 2, LINE_THICKNESS);
    }
}


// 真正畫圖的函式：不要再用絕對螢幕座標
void WaveformDisplay::DrawPrimitives()
{
    if(m_LeftChannel.empty() && m_RightChannel.empty())
        return;

    Actor::SetRenderStates();
    DISPLAY->SetBlendMode(BLEND_NORMAL);
    DISPLAY->ClearAllTextures();

    // 假設 waveHeight=600, waveWidth=200
    float waveHeight = 600.f;
    float waveWidth  = 200.f;

    // 左聲道 offsetX=-100, 右聲道 offsetX=+100
    // 也可以只合併成單聲道
    if(!m_LeftChannel.empty())
        DrawSingleChannelEnvelope(m_LeftChannel, waveHeight, waveWidth, -100.f, RageColor(0,1,0,1));
    if(!m_RightChannel.empty())
        DrawSingleChannelEnvelope(m_RightChannel, waveHeight, waveWidth, +100.f, RageColor(1,1,0,1));
}