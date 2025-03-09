#ifndef WAVEFORM_DISPLAY_H
#define WAVEFORM_DISPLAY_H

#include "Actor.h"
#include "RageSound.h"
#include <vector>

class WaveformDisplay : public Actor {
public:
    WaveformDisplay();
    ~WaveformDisplay() override {}

    void SetSound(RageSound* sound);
    void Update(float deltaTime) override;
    void DrawPrimitives() override;

private:
    RageSound* m_pSound;
    std::vector<int16_t> m_LeftChannel;
    std::vector<int16_t> m_RightChannel;
    std::vector<int16_t> m_LeftChannelFull;  // 存整首歌的左聲道
    std::vector<int16_t> m_RightChannelFull; // 存整首歌的右聲道
    int m_MaxSamples;

    void ProcessAudioData();
    void PrecomputeWaveform();  // 預先計算整首歌的波形
    void ExtractWaveformSegment(float startSecond, float duration);
    void DrawSingleChannel(const std::vector<int16_t> &samples,
                                        float waveHeight,
                                        float waveWidth,
                                        float offsetX,
                                        RageColor lineColor);
    void DrawSingleChannelEnvelope(const std::vector<int16_t> &samples,
                                                float waveHeight,
                                                float waveWidth,
                                                float offsetX,
                                                RageColor lineColor);
};

#endif
