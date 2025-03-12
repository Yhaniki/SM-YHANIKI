#ifndef WAVEFORM_DISPLAY_H
#define WAVEFORM_DISPLAY_H

#include "Actor.h"
#include "RageSound.h"
#include "PlayerNumber.h"
#include <vector>
#include "Sprite.h"
#include "Quad.h"
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

    // 指定要讀取哪個音檔
    void SetSound(RageSound* sound);

    // 指定要對應哪個玩家，用以讀取 scroll speed
    void SetPlayerNumber(PlayerNumber pn);

    // Actor 內建的更新/繪圖
    void Update(float deltaTime) override;
    void DrawPrimitives() override;

    // 設定要顯示哪個秒數區段(最原始、尚未套 scroll speed 的)
    void ExtractWaveformSegment(float firstBeat, float lastBeat, float startSecond, float duration);
    void SetHeight(float height) {m_height = height;};
    void SetDisplayRange(float startSec, float endSec, float visTopY, float visBottomY);
private:
    // 是否啟用高通濾波後的資料(示範)
    float m_height;
    bool m_bEnableFilter;
    float m_fFirstBeat;
    float m_fLastBeat;
    // 外部設定：用來取得 scroll speed
    PlayerNumber m_PlayerNumber;

    // 用來記錄呼叫 ExtractWaveformSegment 時，使用者給的「原始」起迄
    float m_fUserStartRaw;
    float m_fUserDurationRaw;

    // 音檔
    RageSound* m_pSound;

    // 整首音樂的「原始」完整樣本
    std::vector<int16_t> m_LeftChannelFull;
    std::vector<int16_t> m_RightChannelFull;

    // 整首音樂的「高通濾波」完整樣本
    std::vector<int16_t> m_LeftChannelFiltered;
    std::vector<int16_t> m_RightChannelFiltered;

    // 預先計算好的「原始波形包絡 (min/max)」
    std::vector<MinMax> m_LeftEnvelopeFull;
    std::vector<MinMax> m_RightEnvelopeFull;

    // 預先計算好的「濾波後波形包絡 (min/max)」
    std::vector<MinMax> m_LeftEnvelopeFiltered;
    std::vector<MinMax> m_RightEnvelopeFiltered;

    // 給波形繪製用的區段 (block 範圍)
    int m_iBlockStart;
    int m_iBlockEnd;
    int m_totalBlocks; // 全部樣本換成多少個 block

    // 使用者原本要求顯示 (start, end)，但超出範圍會 clamp
    float m_fUserRequestedStart;
    float m_fUserRequestedEnd;

    // clamp 後的可用範圍
    float m_fClampedStart;
    float m_fClampedEnd;

    // 繪製時用到的座標，包含起始 Y 與每個 block 之間的距離
    float m_fBaseY;
    float m_fSegmentYStep;

    // 動態調整的 block size
    int m_iDynamicBlockSize;
    int m_prevBlockSize;

    // 前置計算整首音檔的樣本(含包絡)
    void PrecomputeWaveform();
    // 重建包絡 (如改變 block size)
    void RebuildEnvelope();

    // 幫整首 m_LeftChannelFull, m_RightChannelFull 建立包絡
    void BuildEnvelope(const std::vector<int16_t>& source, std::vector<MinMax>& envelope);

    // 簡單高通
    void ApplyHighPassFilter(const std::vector<int16_t> &inData, std::vector<int16_t> &outData, float strength);

    // 動態調整 block size
    void AdjustBlockSize(float duration);

    // 依照目前 clamp 後的秒數區段，計算 m_iBlockStart / m_iBlockEnd
    void ExtractWaveformSegment_Actual(float startSecond, float duration);

    // 幫 DrawPrimitives() 的包絡繪製
    void DrawEnvelopeRange(const std::vector<MinMax>& envelope,
                           float waveHeight,
                           float waveWidth,
                           float offsetX,
                           RageColor lineColor);

    // 幫 DrawPrimitives() 畫灰底
    void DrawBlackRectangle(float bottomY, float topY, float leftX, float rightX, float alpha);
    Quad			m_sprBars;
    void DrawBeatBar( const float fBeat );
};

#endif
