/* GNFile - 讀寫 SDO 系列的 .gn (StepFile) 譜面檔。
 *
 * 這一層只處理位元組，不依賴 Song / Steps / NoteData，方便單獨測試。
 * 支援四種容器：
 *   PLAIN  未加密，開頭即 StepFile
 *   DDRM   'ddrm' 表頭 (0x54 bytes) + LCG 加密本文
 *   SDOM   檔首資源檔名表 + 內嵌 StepFile 表頭(明文 300) + LCG 加密全文
 *   REWU   熱舞 Online，整檔 LCG 加密
 * 三者的 LCG 乘數皆為 0x3D09；SDOM / REWU 需以已知明文還原 seed。
 *
 * 格式細節見 docs/GN_FORMAT.md。
 */

#ifndef GN_FILE_H
#define GN_FILE_H

#include <stdint.h>
#include <string>
#include <vector>

namespace GNFile
{
	/* StepFile 表頭固定 300 bytes，且 address_easy 必為 300。 */
	const int HEADER_SIZE = 300;

	/* GN 一個 measurement = 4 拍，最細切到 192 格（與 SM 的 ROWS_PER_MEASURE 相同）。 */
	const int BEATS_PER_MEASUREMENT = 4;
	const int MAX_INTERVAL = 192;

	/* 三個難度在檔案中的排列順序。 */
	enum Difficulty { DIFF_EASY = 0, DIFF_NORMAL = 1, DIFF_HARD = 2, NUM_DIFFS = 3 };

	enum Container
	{
		CONTAINER_UNKNOWN = 0,
		CONTAINER_PLAIN,
		CONTAINER_DDRM,
		CONTAINER_SDOM,
		CONTAINER_REWU
	};

	/* step_frame_type */
	enum FrameType
	{
		FRAME_BPM     = 1,	// slot 的 4 bytes 直接是 float BPM
		FRAME_LEFT    = 2,
		FRAME_UP      = 3,
		FRAME_DOWN    = 4,
		FRAME_RIGHT   = 5,
		FRAME_BARLINE = 9,	// 小節線，放在奇數 measurement
		FRAME_MUSIC   = 10	// 音樂起止標記
	};

	/* step_note_type */
	enum NoteKind
	{
		NOTE_ARROW      = 0,
		NOTE_HOLD_START = 2,
		NOTE_HOLD_END   = 3
	};

	struct Slot
	{
		Slot() : u0(0), u1(0), nt(0) { }
		Slot( int16_t a, uint8_t b, uint8_t c ) : u0(a), u1(b), nt(c) { }
		int16_t u0;		// 非 0 表示此格有音符
		uint8_t u1;
		uint8_t nt;		// NoteKind
		bool IsEmpty() const { return u0 == 0 && u1 == 0 && nt == 0; }
	};

	struct Frame
	{
		Frame() : iMeasurement(0), iType(0) { }
		Frame( int32_t m, int16_t t, int iInterval ) : iMeasurement(m), iType(t), vSlots(iInterval) { }
		int32_t				iMeasurement;
		int16_t				iType;			// FrameType
		std::vector<Slot>	vSlots;			// 大小即 interval

		int GetInterval() const { return (int) vSlots.size(); }
		/* slot 在整首歌中的位置（以拍為單位，第 0 拍為譜面原點）。 */
		float GetSlotBeat( int iSlot ) const
		{
			const int iInterval = GetInterval();
			if( iInterval <= 0 )
				return iMeasurement * (float) BEATS_PER_MEASUREMENT;
			return iMeasurement * (float) BEATS_PER_MEASUREMENT +
				BEATS_PER_MEASUREMENT * iSlot / (float) iInterval;
		}
	};

	/* 300 bytes 表頭 + 三難度的 StepFrame 串列。
	 * 表頭中我們不解讀的欄位原樣保留，寫回時照原樣送出。 */
	struct StepFile
	{
		StepFile();

		int32_t		iFileId;
		char		szFileType[8];			// 通常是 "gn"
		uint8_t		aHeader8_15[8];			// 未解讀
		float		fBPM;					// 表頭基準 BPM
		int16_t		aiLevel[NUM_DIFFS];
		uint8_t		aHeader26_39[14];		// 未解讀
		int32_t		aiNoteCount[NUM_DIFFS];	// 只計 type 2/3/4/5
		int32_t		aiExtra52[NUM_DIFFS];	// 實測為小節數，遊戲用來推有效 BPM
		int32_t		aiMeasurements[NUM_DIFFS];	// StepFrame 個數
		uint8_t		aStrings[6][32];		// unknown0, title, unknown1, writer, producer, filename
		int32_t		iUnknown19;
		int32_t		aiDuration[NUM_DIFFS];	// 秒
		uint32_t	aiAddress[4];			// easy, normal, hard, end

		std::vector<Frame>	avFrames[NUM_DIFFS];

		/* 表頭字串（去掉尾端 \0）。 */
		std::string GetTitle() const		{ return GetString(1); }
		std::string GetWriter() const		{ return GetString(3); }
		std::string GetProducer() const		{ return GetString(4); }
		std::string GetFileName() const		{ return GetString(5); }
		std::string GetString( int i ) const;
		void SetString( int i, const std::string &s );
	};

	/* 解密時記下的容器資訊，寫回時要用同一把鑰匙。 */
	struct ContainerInfo
	{
		ContainerInfo();
		Container	type;
		uint32_t	iSeed;			// SDOM / REWU
		uint32_t	iSeed1, iSeed2;	// DDRM
		std::string	sPrefix;		// SDOM 檔首那段明文資源表
		std::string	sDDRMHeader;	// DDRM 原本的 0x54 bytes 表頭（寫回時只改長度與 CRC）
		std::string	sDDRMBlock1;	// DDRM 表頭裡那段 0x20 bytes 區塊的明文
		size_t		iOrigFileSize;
		size_t		iOrigEncLen;	// SDOM：加密區原長度
		uint32_t	iOrigCrc;		// SDOM：加密區原 CRC32
		std::string	sTrailing;		// StepFile 結尾之後殘留的位元組（少見，原樣保留）
	};

	/* 看起來像不像 .gn（只檢查容器，不解析譜面）。用來快速篩掉不相干的檔案。 */
	bool LooksLikeGN( const std::string &sRaw );

	/* 解密／剝殼，取得 StepFile 明文本文。失敗時 psErrOut 會填入原因。
	 * SDOM 與 REWU 需要還原 seed（各約 2^24 次搜尋），單檔數十毫秒。 */
	bool Decrypt( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &infoOut, std::string *psErrOut = NULL );

	/* 用原本的鑰匙把本文包回原容器。
	 * bKeepFileSize：SDOM 與 REWU 會把輸出補到與原檔完全相同的大小，
	 *                譜面變大塞不下時直接失敗；預設 false，允許檔案變大。 */
	bool Encrypt( const std::string &sBody, const ContainerInfo &info, bool bKeepFileSize,
		std::string &sRawOut, std::string *psErrOut = NULL );

	bool Parse( const std::string &sBody, StepFile &out, std::string *psErrOut = NULL );
	void Serialize( const StepFile &sf, std::string &sOut );

	/* type 1 的 slot 4 bytes 就是一個 little-endian float。 */
	float SlotToBPM( const Slot &s );
	Slot BPMToSlot( float fBPM );

	/* 供測試／工具使用。 */
	uint32_t CRC32( const void *pData, size_t iLen );
	void LCGTransform( uint32_t iSeed, std::string &sData, bool bEncrypt );
}

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
