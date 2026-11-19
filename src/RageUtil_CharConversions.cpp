#include "global.h"
#include "RageUtil_CharConversions.h"

#include "RageUtil.h"
#include "RageLog.h"

#if defined(_WINDOWS)

#include "windows.h"

/* Convert from the given codepage to UTF-8.  Return true if successful. */
static bool CodePageConvert(CString &txt, int cp)
{
	if( txt.size() == 0 )
		return true;

	int size = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, txt.data(), txt.size(), NULL, 0);
	if( size == 0 )
	{
		LOG->Trace("%s\n", werr_ssprintf(GetLastError(), "err: ").c_str());
		return false; /* error */
	}

	wstring out;
	out.append(size, ' ');
	/* Nonportable: */
	size = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, txt.data(), txt.size(), (wchar_t *) out.data(), size);
	ASSERT( size != 0 );

	txt = WStringToCString(out);
	return true;
}

static bool AttemptEnglishConversion( CString &txt ) { return CodePageConvert( txt, 1252 ); }
static bool AttemptKoreanConversion( CString &txt ) { return CodePageConvert( txt, 949 ); }
static bool AttemptJapaneseConversion( CString &txt ) { return CodePageConvert( txt, 932 ); }
static bool AttemptChineseConversion( CString &txt ) { return CodePageConvert( txt, 936 ); }	// GBK
static bool AttemptBig5Conversion( CString &txt ) { return CodePageConvert( txt, 950 ); }

#elif defined(HAVE_ICONV)
#include <iconv.h>

TODO

#else

/* No converters are available, so all fail--we only accept UTF-8. */
static bool AttemptEnglishConversion( CString &txt ) { return false; }
static bool AttemptKoreanConversion( CString &txt ) { return false; }
static bool AttemptJapaneseConversion( CString &txt ) { return false; }
static bool AttemptChineseConversion( CString &txt ) { return false; }
static bool AttemptBig5Conversion( CString &txt ) { return false; }

#endif

bool ConvertString(CString &str, const CString &encodings)
{
	CStringArray lst;
	split(encodings, ",", lst);

	for(unsigned i = 0; i < lst.size(); ++i)
	{
		if( lst[i] == "utf-8" )
		{
			/* Is the string already valid utf-8? */
			if( utf8_is_valid(str) )
				return true;
			continue;
		}
		if( lst[i] == "english" )
		{
			if(AttemptEnglishConversion(str))
				return true;
			continue;
		}

		if( lst[i] == "japanese" )
		{
			if(AttemptJapaneseConversion(str))
				return true;
			continue;
		}

		if( lst[i] == "korean" )
		{
			if(AttemptKoreanConversion(str))
				return true;
			continue;
		}

		/* .gn header strings are mostly GBK; a few Taiwanese releases use Big5. */
		if( lst[i] == "chinese" )
		{
			if(AttemptChineseConversion(str))
				return true;
			continue;
		}

		if( lst[i] == "big5" )
		{
			if(AttemptBig5Conversion(str))
				return true;
			continue;
		}

		RageException::Throw( "Unexpected conversion string \"%s\" (string \"%s\")",
						lst[i].c_str(), str.c_str() );
	}

	return false;
}

#if defined(_WINDOWS)
wstring Utf8ToWide( const CString &s )
{
	if( s.empty() )
		return wstring();
	const int iLen = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int) s.size(), NULL, 0 );
	if( iLen <= 0 )
		return wstring();
	wstring ret( iLen, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), (int) s.size(), &ret[0], iLen );
	return ret;
}

CString WideToUtf8( const wstring &s )
{
	if( s.empty() )
		return CString();
	const int iLen = WideCharToMultiByte( CP_UTF8, 0, s.data(), (int) s.size(), NULL, 0, NULL, NULL );
	if( iLen <= 0 )
		return CString();
	string ret( iLen, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, s.data(), (int) s.size(), &ret[0], iLen, NULL, NULL );
	return CString( ret.data(), iLen );
}
#endif

CString LegacyPathToUtf8( const CString &s )
{
	if( utf8_is_valid(s) )
		return s;
#if defined(_WINDOWS)
	/* Not CodePageConvert: that logs, and this runs before LOG exists. */
	const int iLen = MultiByteToWideChar( CP_ACP, MB_ERR_INVALID_CHARS, s.data(), (int) s.size(), NULL, 0 );
	if( iLen > 0 )
	{
		wstring w( iLen, L'\0' );
		MultiByteToWideChar( CP_ACP, MB_ERR_INVALID_CHARS, s.data(), (int) s.size(), &w[0], iLen );
		return WideToUtf8( w );
	}
#endif
	return s;
}

/* Written by Glenn Maynard.  In the public domain; there are so many
 * simple conversion interfaces that restricting them is silly. */
