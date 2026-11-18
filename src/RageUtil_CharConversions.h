#ifndef RAGEUTIL_CHAR_CONVERSIONS_H
#define RAGEUTIL_CHAR_CONVERSIONS_H

/* Convert a string to UTF-8 from the first possible encoding in the given comma-
 * separated list of encodings.  Valid strings are "utf-8", "english", "japanese",
 * "korean", "chinese" (GBK) and "big5".
 * Return true if the conversion was successful (or a no-op).  Return false and
 * leave the string unchanged if the conversion was unsuccessful. */
bool ConvertString(CString &str, const CString &encodings);

#if defined(_WINDOWS)
/* File paths are kept in UTF-8 inside the engine; the wide Win32 APIs take
 * UTF-16.  Unlike CStringToWstring, these handle characters outside the BMP. */
wstring Utf8ToWide( const CString &s );
CString WideToUtf8( const wstring &s );
#endif

/* Older builds kept file paths in the system code page.  If s isn't valid
 * UTF-8, assume it was saved by such a build and convert it; otherwise return
 * it unchanged.  Use this on paths read back from files written by the game. */
CString LegacyPathToUtf8( const CString &s );

#endif
