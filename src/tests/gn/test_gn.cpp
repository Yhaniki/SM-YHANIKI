/* GNFile 的獨立驗證：解密 → 解析 → 序列化 → 重新加密，逐步跟原檔比對。 */
#include "global.h"
#include "GNFile.h"
#include <stdio.h>

static bool ReadFile( const char *szPath, std::string &sOut )
{
	FILE *f = fopen( szPath, "rb" );
	if( !f )
		return false;
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	sOut.resize( n );
	size_t got = fread( &sOut[0], 1, n, f );
	fclose( f );
	return got == (size_t) n;
}

static const char *ContainerName( GNFile::Container c )
{
	switch( c )
	{
	case GNFile::CONTAINER_PLAIN:	return "plain";
	case GNFile::CONTAINER_DDRM:	return "ddrm";
	case GNFile::CONTAINER_SDOM:	return "sdom";
	case GNFile::CONTAINER_REWU:	return "rewu";
	default:						return "unknown";
	}
}

static int g_iFailures = 0;
static void Check( bool b, const char *szWhat )
{
	printf( "    [%s] %s\n", b ? " ok " : "FAIL", szWhat );
	if( !b )
		g_iFailures++;
}

int main( int argc, char **argv )
{
	for( int a = 1; a < argc; a++ )
	{
		printf( "===== %s\n", argv[a] );

		std::string sRaw;
		if( !ReadFile(argv[a], sRaw) )
		{
			printf( "  cannot read\n" );
			g_iFailures++;
			continue;
		}

		std::string sBody, sErr;
		GNFile::ContainerInfo info;
		if( !GNFile::Decrypt(sRaw, sBody, info, &sErr) )
		{
			printf( "  decrypt failed: %s\n", sErr.c_str() );
			g_iFailures++;
			continue;
		}
		printf( "  container=%s  raw=%u  body=%u  seed=0x%08X seed1=0x%08X seed2=0x%08X prefix=%u trailing=%u\n",
			ContainerName(info.type), (unsigned) sRaw.size(), (unsigned) sBody.size(),
			info.iSeed, info.iSeed1, info.iSeed2,
			(unsigned) info.sPrefix.size(), (unsigned) info.sTrailing.size() );

		GNFile::StepFile sf;
		if( !GNFile::Parse(sBody, sf, &sErr) )
		{
			printf( "  parse failed: %s\n", sErr.c_str() );
			g_iFailures++;
			continue;
		}

		printf( "  file_id=%d type=%s bpm=%.4f levels=%d/%d/%d notes=%d/%d/%d meas=%d/%d/%d extra52=%d/%d/%d dur=%d/%d/%d\n",
			sf.iFileId, sf.szFileType, sf.fBPM,
			sf.aiLevel[0], sf.aiLevel[1], sf.aiLevel[2],
			sf.aiNoteCount[0], sf.aiNoteCount[1], sf.aiNoteCount[2],
			sf.aiMeasurements[0], sf.aiMeasurements[1], sf.aiMeasurements[2],
			sf.aiExtra52[0], sf.aiExtra52[1], sf.aiExtra52[2],
			sf.aiDuration[0], sf.aiDuration[1], sf.aiDuration[2] );
		printf( "  addr=%u/%u/%u/%u  title=[%s] writer=[%s] file=[%s]\n",
			sf.aiAddress[0], sf.aiAddress[1], sf.aiAddress[2], sf.aiAddress[3],
			sf.GetTitle().c_str(), sf.GetWriter().c_str(), sf.GetFileName().c_str() );

		for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
		{
			int aiType[16];
			memset( aiType, 0, sizeof(aiType) );
			int iNotes = 0, iMaxM = 0;
			std::string sBPMs;
			for( unsigned f = 0; f < sf.avFrames[d].size(); f++ )
			{
				const GNFile::Frame &fr = sf.avFrames[d][f];
				if( fr.iType >= 0 && fr.iType < 16 )
					aiType[fr.iType]++;
				iMaxM = max( iMaxM, (int) fr.iMeasurement );
				for( unsigned i = 0; i < fr.vSlots.size(); i++ )
				{
					if( fr.vSlots[i].IsEmpty() )
						continue;
					if( fr.iType == GNFile::FRAME_BPM )
					{
						char buf[64];
						sprintf( buf, "(%.2f,%.3f) ", fr.GetSlotBeat(i), GNFile::SlotToBPM(fr.vSlots[i]) );
						sBPMs += buf;
					}
					else if( fr.iType >= 2 && fr.iType <= 5 )
						iNotes++;
				}
			}
			printf( "  diff%d: frames=%u notes=%d maxM=%d  types:", d, (unsigned) sf.avFrames[d].size(), iNotes, iMaxM );
			for( int t = 0; t < 16; t++ )
				if( aiType[t] )
					printf( " %d=%d", t, aiType[t] );
			printf( "\n    bpm events: %s\n", sBPMs.empty() ? "(none)" : sBPMs.c_str() );
		}

		/* 1. 解析後原樣序列化，應該跟解密出來的本文一模一樣。 */
		std::string sReBody;
		GNFile::Serialize( sf, sReBody );
		const std::string sBodyNoTrailing = sBody.substr( 0, sf.aiAddress[3] );
		Check( sReBody == sBodyNoTrailing, "serialize(parse(body)) == body" );

		/* 2. 原樣包回去，應該跟原檔一模一樣。 */
		std::string sReRaw;
		if( !GNFile::Encrypt(sReBody, info, true, sReRaw, &sErr) )
		{
			printf( "    [FAIL] encrypt(keepsize): %s\n", sErr.c_str() );
			g_iFailures++;
		}
		else
		{
			Check( sReRaw.size() == sRaw.size(), "encrypt(keepsize) keeps the file size" );
			Check( sReRaw == sRaw, "encrypt(serialize(parse(x))) == x" );
		}

		/* 3. 不保大小的路徑也要能被自己讀回來。 */
		std::string sGrowRaw;
		if( !GNFile::Encrypt(sReBody, info, false, sGrowRaw, &sErr) )
		{
			printf( "    [FAIL] encrypt(grow): %s\n", sErr.c_str() );
			g_iFailures++;
		}
		else
		{
			std::string sBody2;
			GNFile::ContainerInfo info2;
			GNFile::StepFile sf2;
			bool bOK = GNFile::Decrypt( sGrowRaw, sBody2, info2, &sErr ) &&
				GNFile::Parse( sBody2, sf2, &sErr );
			Check( bOK, "encrypt(grow) round-trips through Decrypt+Parse" );
			if( bOK )
			{
				bool bSame = true;
				for( int d = 0; d < GNFile::NUM_DIFFS; d++ )
					bSame = bSame && sf2.avFrames[d].size() == sf.avFrames[d].size();
				Check( bSame, "frame counts survive the grow path" );
				Check( info2.type == info.type, "container type survives the grow path" );
			}
		}
	}

	printf( "\n%s (%d failures)\n", g_iFailures ? "FAILED" : "ALL PASSED", g_iFailures );
	return g_iFailures ? 1 : 0;
}
