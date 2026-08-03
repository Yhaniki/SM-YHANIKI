#include "global.h"
#include "GNFile.h"

#include <string.h>
#include <unordered_map>

namespace GNFile
{

// =============================================================================
// 小工具：little-endian 存取（GN 全檔皆為小端序）
// =============================================================================

static inline uint32_t GetU32( const std::string &s, size_t o )
{
	return (uint32_t)(uint8_t)s[o] | ((uint32_t)(uint8_t)s[o+1] << 8) |
		((uint32_t)(uint8_t)s[o+2] << 16) | ((uint32_t)(uint8_t)s[o+3] << 24);
}

static inline int32_t GetI32( const std::string &s, size_t o )
{
	return (int32_t) GetU32( s, o );
}

static inline int16_t GetI16( const std::string &s, size_t o )
{
	return (int16_t)( (uint16_t)(uint8_t)s[o] | ((uint16_t)(uint8_t)s[o+1] << 8) );
}

static inline float GetF32( const std::string &s, size_t o )
{
	uint32_t i = GetU32( s, o );
	float f;
	memcpy( &f, &i, 4 );
	return f;
}

static inline void PutU32( std::string &s, size_t o, uint32_t v )
{
	s[o+0] = (char)(uint8_t)( v & 0xFF );
	s[o+1] = (char)(uint8_t)( (v >> 8) & 0xFF );
	s[o+2] = (char)(uint8_t)( (v >> 16) & 0xFF );
	s[o+3] = (char)(uint8_t)( (v >> 24) & 0xFF );
}

static inline void PutI32( std::string &s, size_t o, int32_t v )
{
	PutU32( s, o, (uint32_t) v );
}

static inline void PutI16( std::string &s, size_t o, int16_t v )
{
	s[o+0] = (char)(uint8_t)( (uint16_t) v & 0xFF );
	s[o+1] = (char)(uint8_t)( ((uint16_t) v >> 8) & 0xFF );
}

static inline void PutF32( std::string &s, size_t o, float f )
{
	uint32_t i;
	memcpy( &i, &f, 4 );
	PutU32( s, o, i );
}

static void SetErr( std::string *psErrOut, const char *sFmt, ... )
{
	if( psErrOut == NULL )
		return;
	char buf[512];
	va_list va;
	va_start( va, sFmt );
	vsnprintf( buf, sizeof(buf), sFmt, va );
	va_end( va );
	buf[sizeof(buf)-1] = 0;
	*psErrOut = buf;
}

// =============================================================================
// CRC32 (IEEE) 與「改最後 4 bytes 湊出指定 CRC」
// =============================================================================

static uint32_t g_aCrcTable[256];
static bool g_bCrcTableInited = false;

static void InitCrcTable()
{
	if( g_bCrcTableInited )
		return;
	for( int i = 0; i < 256; i++ )
	{
		uint32_t c = (uint32_t) i;
		for( int k = 0; k < 8; k++ )
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		g_aCrcTable[i] = c;
	}
	g_bCrcTableInited = true;
}

uint32_t CRC32( const void *pData, size_t iLen )
{
	InitCrcTable();
	const uint8_t *p = (const uint8_t *) pData;
	uint32_t crc = 0xFFFFFFFFu;
	for( size_t i = 0; i < iLen; i++ )
		crc = g_aCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

static uint32_t CrcInternal( const uint8_t *p, size_t iLen )
{
	InitCrcTable();
	uint32_t crc = 0xFFFFFFFFu;
	for( size_t i = 0; i < iLen; i++ )
		crc = g_aCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc;	// 不做最終 XOR
}

/* 已知某 byte 與該步之後的狀態，反推該步之前的狀態。找不到回 false。 */
static bool ReverseCrcStep( uint32_t iStateAfter, uint8_t iByte, uint32_t &iStateBeforeOut )
{
	InitCrcTable();
	for( int idx = 0; idx < 256; idx++ )
	{
		uint32_t shifted = iStateAfter ^ g_aCrcTable[idx];
		if( (shifted >> 24) != 0 )
			continue;
		uint32_t sb = (shifted << 8) | (uint32_t)( (uint8_t) idx ^ iByte );
		if( (g_aCrcTable[(sb ^ iByte) & 0xFF] ^ (sb >> 8)) == iStateAfter )
		{
			iStateBeforeOut = sb;
			return true;
		}
	}
	return false;
}

/* 改寫 sData 的最後 4 bytes，使 CRC32(sData) == iTargetCrc。 */
static bool Crc32FixLast4( std::string &sData, uint32_t iTargetCrc )
{
	if( sData.size() < 4 )
		return false;
	InitCrcTable();

	const uint32_t iPrefix = CrcInternal( (const uint8_t *) sData.data(), sData.size() - 4 );
	const uint32_t iTargetInternal = iTargetCrc ^ 0xFFFFFFFFu;

	/* 前半：列舉最後四個 byte 中的前兩個，記下走完兩步後的狀態。 */
	std::unordered_map<uint32_t, uint32_t> mapForward;	// state -> (b0 << 8 | b1)
	mapForward.reserve( 70000 );
	for( int b0 = 0; b0 < 256; b0++ )
	{
		const uint32_t s1 = g_aCrcTable[(iPrefix ^ (uint32_t) b0) & 0xFF] ^ (iPrefix >> 8);
		for( int b1 = 0; b1 < 256; b1++ )
		{
			const uint32_t s2 = g_aCrcTable[(s1 ^ (uint32_t) b1) & 0xFF] ^ (s1 >> 8);
			mapForward[s2] = (uint32_t)( (b0 << 8) | b1 );
		}
	}

	/* 後半：從目標狀態往回推兩步，看看能不能接上前半。 */
	for( int b3 = 0; b3 < 256; b3++ )
	{
		uint32_t s3;
		if( !ReverseCrcStep( iTargetInternal, (uint8_t) b3, s3 ) )
			continue;
		for( int b2 = 0; b2 < 256; b2++ )
		{
			uint32_t s2;
			if( !ReverseCrcStep( s3, (uint8_t) b2, s2 ) )
				continue;
			std::unordered_map<uint32_t, uint32_t>::const_iterator it = mapForward.find( s2 );
			if( it == mapForward.end() )
				continue;
			const size_t n = sData.size();
			sData[n-4] = (char)(uint8_t)( (it->second >> 8) & 0xFF );
			sData[n-3] = (char)(uint8_t)( it->second & 0xFF );
			sData[n-2] = (char)(uint8_t) b2;
			sData[n-1] = (char)(uint8_t) b3;
			return true;
		}
	}
	return false;
}

// =============================================================================
// LCG 串流加解密
// =============================================================================

const uint32_t MULTIPLIER = 0x3D09;

void LCGTransform( uint32_t iSeed, std::string &sData, bool bEncrypt )
{
	uint32_t state = iSeed;
	const size_t n = sData.size();
	for( size_t i = 0; i < n; i++ )
	{
		state *= MULTIPLIER;
		const uint8_t k = (uint8_t)( (state >> 16) & 0xFF );
		const uint8_t c = (uint8_t) sData[i];
		sData[i] = (char)(uint8_t)( bEncrypt ? (uint8_t)(c + k) : (uint8_t)(c - k) );
	}
}

/* 0x3D09 在 mod 2^32 下的乘法反元素，用來把狀態往回推。 */
static uint32_t ModInverseMultiplier()
{
	/* 牛頓迭代：x_{n+1} = x_n * (2 - a * x_n) */
	uint32_t x = 1;
	for( int i = 0; i < 6; i++ )
		x *= 2u - MULTIPLIER * x;
	return x;
}

/* 已知明文攻擊：iKs 是 iNumKs 個連續的 keystream byte（密文 - 明文），
 * 還原出產生第一個 byte 之前的 seed。搜尋空間 2^24。 */
static bool RecoverSeed( const uint8_t *pKs, int iNumKs, uint32_t &iSeedOut )
{
	if( iNumKs < 4 )
		return false;
	const uint32_t iInv = ModInverseMultiplier();
	for( uint32_t hi = 0; hi < 256; hi++ )
	{
		const uint32_t base = (hi << 24) | ((uint32_t) pKs[0] << 16);
		for( uint32_t lo = 0; lo < 65536; lo++ )
		{
			/* state 即產生 pKs[0] 的那一步狀態，其 bit16-23 必等於 pKs[0]。 */
			const uint32_t state = base | lo;
			uint32_t s = state * MULTIPLIER;
			if( (uint8_t)((s >> 16) & 0xFF) != pKs[1] )
				continue;
			s *= MULTIPLIER;
			if( (uint8_t)((s >> 16) & 0xFF) != pKs[2] )
				continue;
			s *= MULTIPLIER;
			if( (uint8_t)((s >> 16) & 0xFF) != pKs[3] )
				continue;

			uint32_t t = state;
			bool bOK = true;
			for( int i = 1; i < iNumKs; i++ )
			{
				t *= MULTIPLIER;
				if( (uint8_t)((t >> 16) & 0xFF) != pKs[i] )
				{
					bOK = false;
					break;
				}
			}
			if( !bOK )
				continue;
			iSeedOut = state * iInv;	// 回推一步，得到 LCGTransform 要吃的 seed
			return true;
		}
	}
	return false;
}

// =============================================================================
// 容器辨識與解密
// =============================================================================

const uint32_t DDRM_MAGIC = 0x6D726464;	// 'ddrm'
const int DDRM_HEADER_SIZE = 0x54;
const int DDRM_BLOCK1_LEN = 0x20;
const int DDRM_OFF_SEED1 = 0x0C;
const int DDRM_OFF_CRC1 = 0x10;
const int DDRM_OFF_BLOCK1 = 0x20;
const int DDRM_B1_OFF_SEED2 = 0x04;
const int DDRM_B1_OFF_CRC2 = 0x08;

const int OFF_FILE_TYPE = 4;
const int OFF_ADDRESS_EASY = 284;

/* 表頭看起來是不是一份合法的 StepFile 表頭。 */
static bool HeaderLooksValid( const std::string &s, size_t off, size_t iAvail )
{
	if( off + HEADER_SIZE > s.size() )
		return false;
	if( s.compare( off + OFF_FILE_TYPE, 2, "gn" ) != 0 && s.compare( off + OFF_FILE_TYPE, 2, "GN" ) != 0 )
		return false;
	if( s[off+OFF_FILE_TYPE+2] != 0 || s[off+OFF_FILE_TYPE+3] != 0 )
		return false;
	const uint32_t ae = GetU32( s, off + OFF_ADDRESS_EASY );
	if( ae != (uint32_t) HEADER_SIZE )
		return false;
	const uint32_t an = GetU32( s, off + OFF_ADDRESS_EASY + 4 );
	const uint32_t ah = GetU32( s, off + OFF_ADDRESS_EASY + 8 );
	const uint32_t aend = GetU32( s, off + OFF_ADDRESS_EASY + 12 );
	if( !((uint32_t) HEADER_SIZE <= an && an <= ah && ah <= aend && aend <= iAvail) )
		return false;
	return true;
}

/* SDOM：檔首是資源檔名表，往後掃描第一個像 StepFile 表頭的位置（常見為 0x1C8）。 */
static int FindSdomInnerOffset( const std::string &sRaw )
{
	const int iScanMax = 0x4000;
	if( sRaw.size() < (size_t) HEADER_SIZE + 8 )
		return -1;
	const size_t iLimit = min( (size_t) iScanMax, sRaw.size() - HEADER_SIZE );
	for( size_t off = 0; off < iLimit; off++ )
	{
		if( sRaw.compare( off + OFF_FILE_TYPE, 2, "gn" ) != 0 )
			continue;
		if( HeaderLooksValid( sRaw, off, sRaw.size() - off ) )
			return (int) off;
	}
	return -1;
}

static bool DecryptDDRM( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &info, std::string *psErrOut )
{
	if( sRaw.size() < (size_t) DDRM_HEADER_SIZE )
	{
		SetErr( psErrOut, "ddrm header is truncated" );
		return false;
	}
	const uint32_t s1 = GetU32( sRaw, DDRM_OFF_SEED1 );
	std::string sBlock1 = sRaw.substr( DDRM_OFF_BLOCK1, DDRM_BLOCK1_LEN );
	LCGTransform( s1, sBlock1, false );
	const uint32_t s2 = GetU32( sBlock1, DDRM_B1_OFF_SEED2 );

	sBodyOut = sRaw.substr( DDRM_HEADER_SIZE );
	LCGTransform( s2, sBodyOut, false );

	info.type = CONTAINER_DDRM;
	info.iSeed1 = s1;
	info.iSeed2 = s2;
	/* 表頭與 block1 裡我們沒解讀的欄位原樣留著，寫回時只改長度與 CRC。 */
	info.sDDRMHeader = sRaw.substr( 0, DDRM_HEADER_SIZE );
	info.sDDRMBlock1 = sBlock1;
	return true;
}

static bool DecryptSDOM( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &info, std::string *psErrOut )
{
	const int iInnerOff = FindSdomInnerOffset( sRaw );
	if( iInnerOff < 0 )
		return false;	// 不是 SDOM，交給下一種容器試

	const std::string sInner = sRaw.substr( iInnerOff );
	if( sInner.size() < (size_t) HEADER_SIZE + 16 )
	{
		SetErr( psErrOut, "SDOM: encrypted region is too short" );
		return false;
	}
	/* 加密區的明文開頭就是同一份 300 bytes 表頭，拿來當已知明文。 */
	const std::string sHeader = sInner.substr( 0, HEADER_SIZE );
	const std::string sEnc = sInner.substr( HEADER_SIZE );

	uint8_t aKs[16];
	for( int i = 0; i < 16; i++ )
		aKs[i] = (uint8_t)( (uint8_t) sEnc[i] - (uint8_t) sHeader[i] );

	uint32_t iSeed;
	if( !RecoverSeed( aKs, 16, iSeed ) )
	{
		SetErr( psErrOut, "SDOM: could not recover the LCG seed" );
		return false;
	}

	std::string sDec = sEnc;
	LCGTransform( iSeed, sDec, false );
	if( sDec.compare( 0, HEADER_SIZE, sHeader ) != 0 )
	{
		SetErr( psErrOut, "SDOM: decrypted header does not match the plaintext header" );
		return false;
	}

	sBodyOut = sDec;
	info.type = CONTAINER_SDOM;
	info.iSeed = iSeed;
	info.sPrefix = sRaw.substr( 0, iInnerOff );
	info.iOrigEncLen = sEnc.size();
	info.iOrigCrc = CRC32( sEnc.data(), sEnc.size() );
	return true;
}

static bool DecryptREWU( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &info, std::string *psErrOut )
{
	if( sRaw.size() < (size_t) HEADER_SIZE )
	{
		SetErr( psErrOut, "file is shorter than 300 bytes" );
		return false;
	}
	/* 整檔加密，offset 4-7 的明文一定是 "gn\0\0" 或 "GN\0\0"。 */
	const char *aExpected[2] = { "gn\0\0", "GN\0\0" };
	const uint32_t iInv = ModInverseMultiplier();

	for( int e = 0; e < 2; e++ )
	{
		uint8_t aKs[4];
		for( int i = 0; i < 4; i++ )
			aKs[i] = (uint8_t)( (uint8_t) sRaw[4+i] - (uint8_t) aExpected[e][i] );

		for( uint32_t hi = 0; hi < 256; hi++ )
		{
			const uint32_t base = (hi << 24) | ((uint32_t) aKs[0] << 16);
			for( uint32_t lo = 0; lo < 65536; lo++ )
			{
				const uint32_t state = base | lo;
				uint32_t s = state * MULTIPLIER;
				if( (uint8_t)((s >> 16) & 0xFF) != aKs[1] )
					continue;
				s *= MULTIPLIER;
				if( (uint8_t)((s >> 16) & 0xFF) != aKs[2] )
					continue;
				s *= MULTIPLIER;
				if( (uint8_t)((s >> 16) & 0xFF) != aKs[3] )
					continue;

				/* state 是產生 offset 4 那個 byte 的狀態，往回推 5 步得到檔首的 seed。 */
				uint32_t iSeed = state;
				for( int k = 0; k < 5; k++ )
					iSeed *= iInv;

				std::string sHead = sRaw.substr( 0, HEADER_SIZE );
				LCGTransform( iSeed, sHead, false );
				if( sHead.compare( 4, 4, aExpected[e], 4 ) != 0 )
					continue;
				if( GetU32( sHead, OFF_ADDRESS_EASY ) != (uint32_t) HEADER_SIZE )
					continue;

				sBodyOut = sRaw;
				LCGTransform( iSeed, sBodyOut, false );
				info.type = CONTAINER_REWU;
				info.iSeed = iSeed;
				return true;
			}
		}
	}
	SetErr( psErrOut, "unrecognized .gn (not ddrm, SDOM, Dance Online or plain)" );
	return false;
}

ContainerInfo::ContainerInfo()
{
	type = CONTAINER_UNKNOWN;
	iSeed = iSeed1 = iSeed2 = 0;
	iOrigFileSize = 0;
	iOrigEncLen = 0;
	iOrigCrc = 0;
}

bool LooksLikeGN( const std::string &sRaw )
{
	if( sRaw.size() < (size_t) HEADER_SIZE )
		return false;
	if( sRaw.size() >= (size_t) DDRM_HEADER_SIZE && GetU32( sRaw, 0 ) == DDRM_MAGIC )
		return true;
	if( HeaderLooksValid( sRaw, 0, sRaw.size() ) )
		return true;
	if( FindSdomInnerOffset( sRaw ) >= 0 )
		return true;
	/* 熱舞版整檔加密，看不出特徵；副檔名對就當作候選，真正判斷留給 Decrypt。 */
	return true;
}

bool Decrypt( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &infoOut, std::string *psErrOut )
{
	infoOut = ContainerInfo();
	infoOut.iOrigFileSize = sRaw.size();

	bool bOK = false;
	if( sRaw.size() >= (size_t) DDRM_HEADER_SIZE && GetU32( sRaw, 0 ) == DDRM_MAGIC )
	{
		bOK = DecryptDDRM( sRaw, sBodyOut, infoOut, psErrOut );
	}
	else if( HeaderLooksValid( sRaw, 0, sRaw.size() ) )
	{
		sBodyOut = sRaw;
		infoOut.type = CONTAINER_PLAIN;
		bOK = true;
	}
	else
	{
		bOK = DecryptSDOM( sRaw, sBodyOut, infoOut, psErrOut );
		if( !bOK && infoOut.type == CONTAINER_UNKNOWN )
			bOK = DecryptREWU( sRaw, sBodyOut, infoOut, psErrOut );
	}

	if( !bOK )
		return false;

	if( !HeaderLooksValid( sBodyOut, 0, sBodyOut.size() ) )
	{
		SetErr( psErrOut, "decrypted data is not a valid StepFile header" );
		return false;
	}

	/* StepFile 之後若還有殘留位元組，原樣留著，寫回時接上。 */
	const uint32_t aend = GetU32( sBodyOut, OFF_ADDRESS_EASY + 12 );
	if( sBodyOut.size() > aend )
		infoOut.sTrailing = sBodyOut.substr( aend );

	return true;
}

// =============================================================================
// 包回原容器
// =============================================================================

/* SDOM 的檔首多半是資源檔名表，但少數檔案在那裡放的是另一份表頭；
 * 只有後者才需要把統計欄位同步過去。 */
static void SyncSdomPrefix( std::string &sPrefix, const std::string &sBody )
{
	if( sPrefix.size() < (size_t) HEADER_SIZE )
		return;
	if( !HeaderLooksValid( sPrefix, 0, sPrefix.size() ) )
		return;
	memcpy( &sPrefix[16], &sBody[16], 4 );		// bpm
	memcpy( &sPrefix[20], &sBody[20], 6 );		// level
	memcpy( &sPrefix[40], &sBody[40], 12 );		// note_count
	memcpy( &sPrefix[52], &sBody[52], 12 );		// extra52
	memcpy( &sPrefix[64], &sBody[64], 12 );		// measurements
	memcpy( &sPrefix[272], &sBody[272], 12 );	// duration
}

bool Encrypt( const std::string &sBodyIn, const ContainerInfo &info, bool bKeepFileSize,
	std::string &sRawOut, std::string *psErrOut )
{
	const std::string sBody = sBodyIn + info.sTrailing;

	switch( info.type )
	{
	case CONTAINER_PLAIN:
		sRawOut = sBody;
		return true;

	case CONTAINER_DDRM:
	{
		std::string sBodyCt = sBody;
		if( bKeepFileSize )
		{
			const size_t iOrigBody = info.iOrigFileSize > (size_t) DDRM_HEADER_SIZE ?
				info.iOrigFileSize - DDRM_HEADER_SIZE : 0;
			if( sBodyCt.size() > iOrigBody )
			{
				SetErr( psErrOut, "chart grew (%u > original %u bytes); cannot keep the original file size",
					(unsigned) sBodyCt.size(), (unsigned) iOrigBody );
				return false;
			}
			sBodyCt.append( iOrigBody - sBodyCt.size(), '\0' );
		}
		LCGTransform( info.iSeed2, sBodyCt, true );
		const uint32_t iCrc2 = CRC32( sBodyCt.data(), sBodyCt.size() );

		/* 原檔的 block1 與表頭裡還有我們沒解讀的位元組，留著別動。 */
		std::string sBlock1 = info.sDDRMBlock1;
		sBlock1.resize( DDRM_BLOCK1_LEN, '\0' );
		PutU32( sBlock1, DDRM_B1_OFF_SEED2, info.iSeed2 );
		PutU32( sBlock1, DDRM_B1_OFF_CRC2, iCrc2 );
		LCGTransform( info.iSeed1, sBlock1, true );
		const uint32_t iCrc1 = CRC32( sBlock1.data(), sBlock1.size() );

		std::string sHdr = info.sDDRMHeader;
		sHdr.resize( DDRM_HEADER_SIZE, '\0' );
		PutU32( sHdr, 0, DDRM_MAGIC );
		if( info.sDDRMHeader.empty() )
			PutU32( sHdr, 4, 1 );	// version
		PutU32( sHdr, 8, (uint32_t)( DDRM_HEADER_SIZE + sBodyCt.size() ) );
		PutU32( sHdr, DDRM_OFF_SEED1, info.iSeed1 );
		PutU32( sHdr, DDRM_OFF_CRC1, iCrc1 );
		memcpy( &sHdr[DDRM_OFF_BLOCK1], sBlock1.data(), DDRM_BLOCK1_LEN );

		sRawOut = sHdr + sBodyCt;
		return true;
	}

	case CONTAINER_REWU:
	{
		std::string sPlain = sBody;
		if( bKeepFileSize )
		{
			if( sPlain.size() > info.iOrigFileSize )
			{
				SetErr( psErrOut, "chart grew (%u > original %u bytes); cannot keep the original file size",
					(unsigned) sPlain.size(), (unsigned) info.iOrigFileSize );
				return false;
			}
			sPlain.append( info.iOrigFileSize - sPlain.size(), '\0' );
		}
		LCGTransform( info.iSeed, sPlain, true );
		sRawOut = sPlain;
		return true;
	}

	case CONTAINER_SDOM:
	{
		/* 加密區的明文＝完整 StepFile（含表頭），檔案裡另外再放一份明文表頭。
		 * 比原本短就補零補回去；長了就只能讓檔案變大。 */
		std::string sPadded = sBody;
		if( sPadded.size() > info.iOrigEncLen )
		{
			if( bKeepFileSize )
			{
				SetErr( psErrOut, "chart grew (%u > encrypted region %u bytes); cannot keep the original file size",
					(unsigned) sPadded.size(), (unsigned) info.iOrigEncLen );
				return false;
			}
			sPadded.append( 4, '\0' );	// 留 4 bytes 給 CRC 修補
		}
		else
		{
			sPadded.append( info.iOrigEncLen - sPadded.size(), '\0' );
		}

		std::string sEnc = sPadded;
		LCGTransform( info.iSeed, sEnc, true );
		/* 遊戲會檢查加密區的 CRC32，所以要把它湊回原本的值。
		 * 剛好已經對上（例如原封不動存回）時就別再動尾端 4 bytes。 */
		if( CRC32(sEnc.data(), sEnc.size()) != info.iOrigCrc &&
			!Crc32FixLast4( sEnc, info.iOrigCrc ) )
		{
			SetErr( psErrOut, "SDOM: CRC32 patching failed" );
			return false;
		}

		std::string sPrefix = info.sPrefix;
		SyncSdomPrefix( sPrefix, sPadded );

		sRawOut = sPrefix + sPadded.substr( 0, HEADER_SIZE ) + sEnc;
		return true;
	}

	default:
		SetErr( psErrOut, "unknown container type; cannot write back" );
		return false;
	}
}

// =============================================================================
// StepFile 本體
// =============================================================================

StepFile::StepFile()
{
	iFileId = 0;
	strcpy( szFileType, "gn" );
	memset( aHeader8_15, 0, sizeof(aHeader8_15) );
	fBPM = 0;
	memset( aHeader26_39, 0, sizeof(aHeader26_39) );
	memset( aStrings, 0, sizeof(aStrings) );
	iUnknown19 = 0;
	for( int i = 0; i < NUM_DIFFS; i++ )
	{
		aiLevel[i] = 0;
		aiNoteCount[i] = 0;
		aiExtra52[i] = 0;
		aiMeasurements[i] = 0;
		aiDuration[i] = 0;
	}
	aiAddress[0] = HEADER_SIZE;
	aiAddress[1] = aiAddress[2] = aiAddress[3] = HEADER_SIZE;
}

std::string StepFile::GetString( int i ) const
{
	const char *p = (const char *) aStrings[i];
	size_t n = 0;
	while( n < 32 && p[n] != '\0' )
		n++;
	return std::string( p, n );
}

void StepFile::SetString( int i, const std::string &s )
{
	memset( aStrings[i], 0, 32 );
	memcpy( aStrings[i], s.data(), min( s.size(), (size_t) 32 ) );
}

float SlotToBPM( const Slot &s )
{
	uint8_t b[4];
	b[0] = (uint8_t)( (uint16_t) s.u0 & 0xFF );
	b[1] = (uint8_t)( ((uint16_t) s.u0 >> 8) & 0xFF );
	b[2] = s.u1;
	b[3] = s.nt;
	float f;
	memcpy( &f, b, 4 );
	return f;
}

Slot BPMToSlot( float fBPM )
{
	uint8_t b[4];
	memcpy( b, &fBPM, 4 );
	Slot s;
	s.u0 = (int16_t)( (uint16_t) b[0] | ((uint16_t) b[1] << 8) );
	s.u1 = b[2];
	s.nt = b[3];
	return s;
}

static bool ReadFrames( const std::string &s, size_t iBegin, size_t iEnd, std::vector<Frame> &out, std::string *psErrOut )
{
	size_t p = iBegin;
	while( p < iEnd )
	{
		if( p + 8 > s.size() )
		{
			SetErr( psErrOut, "StepFrame header runs past end of file (offset %u)", (unsigned) p );
			return false;
		}
		const int32_t m = GetI32( s, p );
		const int16_t t = GetI16( s, p + 4 );
		const int iInterval = (int)(uint16_t)GetI16( s, p + 6 );
		p += 8;

		if( p + (size_t) iInterval * 4 > s.size() )
		{
			SetErr( psErrOut, "StepFrame data runs past end of file (offset %u, interval %d)", (unsigned) p, iInterval );
			return false;
		}

		out.push_back( Frame( m, t, iInterval ) );
		Frame &fr = out.back();
		for( int i = 0; i < iInterval; i++ )
		{
			fr.vSlots[i].u0 = GetI16( s, p );
			fr.vSlots[i].u1 = (uint8_t) s[p+2];
			fr.vSlots[i].nt = (uint8_t) s[p+3];
			p += 4;
		}
	}
	return true;
}

bool Parse( const std::string &s, StepFile &out, std::string *psErrOut )
{
	if( s.size() < (size_t) HEADER_SIZE )
	{
		SetErr( psErrOut, "StepFile is too short (%u < 300 bytes)", (unsigned) s.size() );
		return false;
	}

	out = StepFile();
	out.iFileId = GetI32( s, 0 );
	memset( out.szFileType, 0, sizeof(out.szFileType) );
	memcpy( out.szFileType, &s[4], 4 );
	memcpy( out.aHeader8_15, &s[8], 8 );
	out.fBPM = GetF32( s, 16 );
	for( int i = 0; i < NUM_DIFFS; i++ )
		out.aiLevel[i] = GetI16( s, 20 + i*2 );
	memcpy( out.aHeader26_39, &s[26], 14 );
	for( int i = 0; i < NUM_DIFFS; i++ )
	{
		out.aiNoteCount[i]    = GetI32( s, 40 + i*4 );
		out.aiExtra52[i]      = GetI32( s, 52 + i*4 );
		out.aiMeasurements[i] = GetI32( s, 64 + i*4 );
	}
	for( int i = 0; i < 6; i++ )
		memcpy( out.aStrings[i], &s[76 + i*32], 32 );
	out.iUnknown19 = GetI32( s, 268 );
	for( int i = 0; i < NUM_DIFFS; i++ )
		out.aiDuration[i] = GetI32( s, 272 + i*4 );
	for( int i = 0; i < 4; i++ )
		out.aiAddress[i] = GetU32( s, 284 + i*4 );

	if( out.aiAddress[0] != (uint32_t) HEADER_SIZE )
	{
		SetErr( psErrOut, "address_easy should be 300, got %u", out.aiAddress[0] );
		return false;
	}
	if( !(out.aiAddress[0] <= out.aiAddress[1] && out.aiAddress[1] <= out.aiAddress[2] &&
		out.aiAddress[2] <= out.aiAddress[3] && out.aiAddress[3] <= s.size()) )
	{
		SetErr( psErrOut, "difficulty addresses are out of range (%u/%u/%u/%u, file size %u)",
			out.aiAddress[0], out.aiAddress[1], out.aiAddress[2], out.aiAddress[3], (unsigned) s.size() );
		return false;
	}

	for( int d = 0; d < NUM_DIFFS; d++ )
	{
		if( !ReadFrames( s, out.aiAddress[d], out.aiAddress[d+1], out.avFrames[d], psErrOut ) )
			return false;
	}
	return true;
}

static void WriteFrames( const std::vector<Frame> &v, std::string &sOut )
{
	for( unsigned f = 0; f < v.size(); f++ )
	{
		const Frame &fr = v[f];
		const size_t iBase = sOut.size();
		sOut.append( 8 + fr.vSlots.size() * 4, '\0' );
		PutI32( sOut, iBase, fr.iMeasurement );
		PutI16( sOut, iBase + 4, fr.iType );
		PutI16( sOut, iBase + 6, (int16_t)(uint16_t) fr.vSlots.size() );
		size_t p = iBase + 8;
		for( unsigned i = 0; i < fr.vSlots.size(); i++, p += 4 )
		{
			PutI16( sOut, p, fr.vSlots[i].u0 );
			sOut[p+2] = (char) fr.vSlots[i].u1;
			sOut[p+3] = (char) fr.vSlots[i].nt;
		}
	}
}

void Serialize( const StepFile &sf, std::string &sOut )
{
	std::string sBody;
	uint32_t aiAddress[4];
	aiAddress[0] = HEADER_SIZE;
	for( int d = 0; d < NUM_DIFFS; d++ )
	{
		WriteFrames( sf.avFrames[d], sBody );
		aiAddress[d+1] = (uint32_t)( HEADER_SIZE + sBody.size() );
	}

	std::string s( HEADER_SIZE, '\0' );
	PutI32( s, 0, sf.iFileId );
	memcpy( &s[4], sf.szFileType, 4 );
	memcpy( &s[8], sf.aHeader8_15, 8 );
	PutF32( s, 16, sf.fBPM );
	for( int i = 0; i < NUM_DIFFS; i++ )
		PutI16( s, 20 + i*2, sf.aiLevel[i] );
	memcpy( &s[26], sf.aHeader26_39, 14 );
	for( int i = 0; i < NUM_DIFFS; i++ )
	{
		PutI32( s, 40 + i*4, sf.aiNoteCount[i] );
		PutI32( s, 52 + i*4, sf.aiExtra52[i] );
		PutI32( s, 64 + i*4, sf.aiMeasurements[i] );
	}
	for( int i = 0; i < 6; i++ )
		memcpy( &s[76 + i*32], sf.aStrings[i], 32 );
	PutI32( s, 268, sf.iUnknown19 );
	for( int i = 0; i < NUM_DIFFS; i++ )
		PutI32( s, 272 + i*4, sf.aiDuration[i] );
	for( int i = 0; i < 4; i++ )
		PutU32( s, 284 + i*4, aiAddress[i] );

	sOut = s + sBody;
}

}	// namespace GNFile

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
