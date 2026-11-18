#ifndef RAGEUTIL_CHAR_CONVERSIONS_H
#define RAGEUTIL_CHAR_CONVERSIONS_H

/* Convert a string to UTF-8 from the first possible encoding in the given comma-
 * separated list of encodings.  Valid strings are "utf-8", "english", "japanese",
 * "korean", "chinese" (GBK) and "big5".
 * Return true if the conversion was successful (or a no-op).  Return false and
 * leave the string unchanged if the conversion was unsuccessful. */
bool ConvertString(CString &str, const CString &encodings);

#endif
