/* GNFile - reads and writes SDO-series .gn (StepFile) chart files.
 *
 * This layer deals only with bytes and does not depend on Song / Steps / NoteData,
 * so it can be tested on its own.
 * Four containers are supported:
 *   PLAIN  unencrypted; the file starts with the StepFile
 *   DDRM   'ddrm' header (0x54 bytes) + LCG-encrypted body
 *   SDOM   resource file name table at the start of the file + embedded StepFile
 *          header (300 bytes, plaintext) + LCG-encrypted whole StepFile
 *   REWU   Dance Online; the whole file is LCG-encrypted
 * All three encrypted containers use the LCG multiplier 0x3D09; SDOM / REWU have
 * to recover the seed from known plaintext.
 *
 * See docs/GN_FORMAT.md for format details.
 */

#ifndef GN_FILE_H
#define GN_FILE_H

#include <stdint.h>
#include <string>
#include <vector>

namespace GNFile
{
	/* The StepFile header is always 300 bytes, and address_easy must be 300. */
	const int HEADER_SIZE = 300;

	/* One GN measurement = 4 beats, divided into at most 192 slots (same as SM's
	 * ROWS_PER_MEASURE). */
	const int BEATS_PER_MEASUREMENT = 4;
	const int MAX_INTERVAL = 192;

	/* Order in which the three difficulties are stored in the file. */
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
		FRAME_BPM     = 1,	// the slot's 4 bytes are a float BPM
		/* Numbering of the up and down lanes: the table in SM_GN_NOTE_FORMAT.md says
		 * 3=up, 4=down, but after saving back to the game up and down come out
		 * swapped; we go by testing and by COL_TO_FRAME_TYPE=(2,3,4,5)
		 * (left, down, up, right) in gn_master_core.py. */
		FRAME_LEFT    = 2,
		FRAME_DOWN    = 3,
		FRAME_UP      = 4,
		FRAME_RIGHT   = 5,
		FRAME_BARLINE = 9,	// bar line, placed in odd measurements
		FRAME_MUSIC   = 10	// music start/end marker
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
		int16_t u0;		// non-zero means this slot has a note
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
		std::vector<Slot>	vSlots;			// size is the interval

		int GetInterval() const { return (int) vSlots.size(); }
		/* Position of the slot in the whole song (in beats; beat 0 is the chart origin). */
		float GetSlotBeat( int iSlot ) const
		{
			const int iInterval = GetInterval();
			if( iInterval <= 0 )
				return iMeasurement * (float) BEATS_PER_MEASUREMENT;
			return iMeasurement * (float) BEATS_PER_MEASUREMENT +
				BEATS_PER_MEASUREMENT * iSlot / (float) iInterval;
		}
	};

	/* 300-byte header + the StepFrame lists of the three difficulties.
	 * Header fields we do not interpret are kept as-is and written back unchanged. */
	struct StepFile
	{
		StepFile();

		int32_t		iFileId;
		char		szFileType[8];			// usually "gn"
		uint8_t		aHeader8_15[8];			// not interpreted
		float		fBPM;					// base BPM from the header
		int16_t		aiLevel[NUM_DIFFS];
		uint8_t		aHeader26_39[14];		// not interpreted
		int32_t		aiNoteCount[NUM_DIFFS];	// counts only types 2/3/4/5
		int32_t		aiExtra52[NUM_DIFFS];	// meaning not settled yet (see NotesWriterGN.cpp); kept as-is
		int32_t		aiMeasurements[NUM_DIFFS];	// number of StepFrames
		uint8_t		aStrings[6][32];		// unknown0, title, unknown1, writer, producer, filename
		int32_t		iUnknown19;
		int32_t		aiDuration[NUM_DIFFS];	// seconds
		uint32_t	aiAddress[4];			// easy, normal, hard, end

		std::vector<Frame>	avFrames[NUM_DIFFS];

		/* Header strings (trailing \0 stripped). */
		std::string GetTitle() const		{ return GetString(1); }
		std::string GetWriter() const		{ return GetString(3); }
		std::string GetProducer() const		{ return GetString(4); }
		std::string GetFileName() const		{ return GetString(5); }
		std::string GetString( int i ) const;
		void SetString( int i, const std::string &s );
	};

	/* Container info recorded while decrypting; writing back must use the same key. */
	struct ContainerInfo
	{
		ContainerInfo();
		Container	type;
		uint32_t	iSeed;			// SDOM / REWU
		uint32_t	iSeed1, iSeed2;	// DDRM
		std::string	sPrefix;		// SDOM: the plaintext resource table at the start of the file
		std::string	sDDRMHeader;	// DDRM: the original 0x54-byte header (only length and CRC change on write-back)
		std::string	sDDRMBlock1;	// DDRM: plaintext of the 0x20-byte block inside the header
		size_t		iOrigFileSize;
		size_t		iOrigEncLen;	// SDOM: original length of the encrypted region
		uint32_t	iOrigCrc;		// SDOM: original CRC32 of the encrypted region
		std::string	sTrailing;		// leftover bytes after the end of the StepFile (rare; kept as-is)
	};

	/* Whether this looks like a .gn (checks only the container, does not parse the chart).
	 * Used to quickly filter out unrelated files. */
	bool LooksLikeGN( const std::string &sRaw );

	/* Decrypt / unwrap the container to get the plaintext StepFile body. On failure,
	 * psErrOut is filled with the reason.
	 * SDOM and REWU need the seed recovered (about 2^24 tries each), which takes a few
	 * tens of milliseconds per file. */
	bool Decrypt( const std::string &sRaw, std::string &sBodyOut, ContainerInfo &infoOut, std::string *psErrOut = NULL );

	/* Wrap the body back into its original container with the original key.
	 * bKeepFileSize: DDRM, SDOM and REWU pad the output to exactly the original file size,
	 *                and fail outright if the chart grew and no longer fits; false by
	 *                default, which lets the file grow. */
	bool Encrypt( const std::string &sBody, const ContainerInfo &info, bool bKeepFileSize,
		std::string &sRawOut, std::string *psErrOut = NULL );

	bool Parse( const std::string &sBody, StepFile &out, std::string *psErrOut = NULL );
	void Serialize( const StepFile &sf, std::string &sOut );

	/* The 4 bytes of a type 1 slot are simply a little-endian float. */
	float SlotToBPM( const Slot &s );
	Slot BPMToSlot( float fBPM );

	/* For tests / tools. */
	uint32_t CRC32( const void *pData, size_t iLen );
	void LCGTransform( uint32_t iSeed, std::string &sData, bool bEncrypt );
}

#endif

/*
 * (c) 2026 SM-YHANIKI
 * All rights reserved.
 */
