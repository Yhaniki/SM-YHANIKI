/* Shim for src/RageUtil.h (see shim/global.h): the server only formats strings. */
#ifndef RAGEUTIL_H
#define RAGEUTIL_H

CString ssprintf( const char *fmt, ... );
CString vssprintf( const char *fmt, va_list argList );

#endif
