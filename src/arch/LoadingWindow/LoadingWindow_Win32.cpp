#include "../../global.h"
#include "../../RageUtil.h"

#include "LoadingWindow_Win32.h"
#include "RageFileManager.h"
#include "archutils/win32/WindowsResources.h"
#include <windows.h>
#include "RageSurface_Load.h"
#include "RageSurface.h"
#include "RageSurfaceUtils.h"

static HBITMAP g_hBitmap = NULL;

/*
 * The dialog template is laid out in dialog units, and DLUs are derived from the
 * dialog font ("MS Sans Serif").  When the system locale is set to a DBCS locale
 * such as Japanese, that font is substituted with a CJK font whose average
 * character width is larger, so the whole dialog grows while the splash bitmap
 * stays the same size, leaving empty space on the right and between the image
 * and the text.
 *
 * To keep the window the same size under every locale, ignore the template's
 * geometry and lay the dialog out in pixels in WM_INITDIALOG:
 *  - the client area is always SPLASH_WIDTH pixels wide (the width the dialog
 *    has on an English system, and the width of the stock splash image);
 *  - the splash image is scaled to that width, keeping its aspect ratio, so
 *    replacement images of any size fit;
 *  - the three text lines are stacked below the image, sized from the actual
 *    dialog font (which may still be the substituted CJK font, so that song
 *    names in the local code page display correctly).
 */
static const int SPLASH_WIDTH = 468;
static const int TEXT_TOP_MARGIN = 4;
static const int TEXT_LINE_PADDING = 6;
static const int BOTTOM_MARGIN = 8;

/* Load an image file into a GDI bitmap, scaled to iWidth pixels wide with the
 * aspect ratio preserved.  The scaled height is returned in iHeightOut. */
static HBITMAP LoadWin32Surface( CString fn, int iWidth, int &iHeightOut )
{
	CString error;
	RageSurface *s = RageSurfaceUtils::LoadFile( fn, error );
	if( s == NULL )
		return NULL;

	if( s->w <= 0 || s->h <= 0 )
	{
		delete s;
		return NULL;
	}

	/* 32-bit BI_RGB DIB layout: each pixel is a little-endian 0x00RRGGBB. */
	RageSurfaceUtils::ConvertSurface( s, s->w, s->h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0 );

	const int iHeight = max( 1, (s->h * iWidth + s->w / 2) / s->w );

	BITMAPINFO bmi;
	ZERO( bmi );
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = s->pitch / 4;
	bmi.bmiHeader.biHeight = -s->h;	/* top-down */
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	HDC hScreenDC = GetDC( NULL );
	HBITMAP bitmap = CreateCompatibleBitmap( hScreenDC, iWidth, iHeight );
	ASSERT( bitmap );

	HDC hBitmapDC = CreateCompatibleDC( hScreenDC );
	HGDIOBJ hOldBitmap = SelectObject( hBitmapDC, bitmap );

	SetStretchBltMode( hBitmapDC, HALFTONE );
	SetBrushOrgEx( hBitmapDC, 0, 0, NULL );
	StretchDIBits( hBitmapDC,
		0, 0, iWidth, iHeight,
		0, 0, s->w, s->h,
		s->pixels, &bmi, DIB_RGB_COLORS, SRCCOPY );

	SelectObject( hBitmapDC, hOldBitmap );
	DeleteDC( hBitmapDC );
	ReleaseDC( NULL, hScreenDC );

	delete s;

	iHeightOut = iHeight;
	return bitmap;
}

/* Lay out the dialog in pixels; see the comment above SPLASH_WIDTH. */
static void LayoutDialog( HWND hWnd, int iSplashHeight )
{
	const int iWidth = SPLASH_WIDTH;

	int iLineHeight = 13 + TEXT_LINE_PADDING;
	HDC hDC = GetDC( hWnd );
	if( hDC != NULL )
	{
		HFONT hFont = (HFONT) SendMessage( hWnd, WM_GETFONT, 0, 0 );
		HGDIOBJ hOldFont = hFont != NULL? SelectObject( hDC, hFont ):NULL;
		TEXTMETRIC tm;
		if( GetTextMetrics(hDC, &tm) )
			iLineHeight = tm.tmHeight + TEXT_LINE_PADDING;
		if( hOldFont != NULL )
			SelectObject( hDC, hOldFont );
		ReleaseDC( hWnd, hDC );
	}

	SetWindowPos( GetDlgItem(hWnd, IDC_SPLASH), NULL, 0, 0, iWidth, iSplashHeight, SWP_NOZORDER | SWP_NOACTIVATE );

	int y = iSplashHeight + TEXT_TOP_MARGIN;
	const int msgs[] = { IDC_STATIC_MESSAGE1, IDC_STATIC_MESSAGE2, IDC_STATIC_MESSAGE3 };
	for( unsigned i = 0; i < 3; ++i )
	{
		SetWindowPos( GetDlgItem(hWnd, msgs[i]), NULL, 0, y, iWidth, iLineHeight, SWP_NOZORDER | SWP_NOACTIVATE );
		y += iLineHeight;
	}
	const int iClientHeight = y + BOTTOM_MARGIN;

	/* Convert the client size to a window size, and keep the window centered
	 * where DS_CENTER originally placed it. */
	RECT rcOld;
	GetWindowRect( hWnd, &rcOld );

	RECT rcNew = { 0, 0, iWidth, iClientHeight };
	AdjustWindowRectEx( &rcNew, GetWindowLong(hWnd, GWL_STYLE), FALSE, GetWindowLong(hWnd, GWL_EXSTYLE) );
	const int cx = rcNew.right - rcNew.left;
	const int cy = rcNew.bottom - rcNew.top;
	const int x = (rcOld.left + rcOld.right - cx) / 2;
	const int top = (rcOld.top + rcOld.bottom - cy) / 2;
	SetWindowPos( hWnd, NULL, x, top, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE );
}

BOOL CALLBACK LoadingWindow_Win32::WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch( msg )
	{
	case WM_INITDIALOG:
	{
		int iSplashHeight = 0;
		g_hBitmap = LoadWin32Surface( "Data/splash.png", SPLASH_WIDTH, iSplashHeight );
		if( g_hBitmap == NULL )
			g_hBitmap = LoadWin32Surface( "Data/splash.bmp", SPLASH_WIDTH, iSplashHeight );
		SendMessage( 
			GetDlgItem(hWnd,IDC_SPLASH), 
			STM_SETIMAGE, 
			(WPARAM) IMAGE_BITMAP, 
			(LPARAM) (HANDLE) g_hBitmap );
		LayoutDialog( hWnd, g_hBitmap != NULL? iSplashHeight:0 );
		break;
	}

	case WM_DESTROY:
		DeleteObject( g_hBitmap );
		g_hBitmap = NULL;
		break;
	}

	return FALSE;
}

LoadingWindow_Win32::LoadingWindow_Win32()
{
	hwnd = CreateDialog(handle.Get(), MAKEINTRESOURCE(IDD_LOADING_DIALOG), NULL, WndProc);
	for( unsigned i = 0; i < 3; ++i )
		text[i] = "XXX"; /* always set on first call */
	SetText("Initializing hardware...");
	Paint();
}

LoadingWindow_Win32::~LoadingWindow_Win32()
{
	if(hwnd)
		DestroyWindow( hwnd );
}

void LoadingWindow_Win32::Paint()
{
	SendMessage( hwnd, WM_PAINT, 0, 0 );

	/* Process all queued messages since the last paint.  This allows the window to
	 * come back if it loses focus during load. */
	MSG msg;
	while( PeekMessage( &msg, hwnd, 0, 0, PM_NOREMOVE ) ) {
		GetMessage(&msg, hwnd, 0, 0 );
		DispatchMessage( &msg );
	}
}

void LoadingWindow_Win32::SetText(CString str)
{
	CStringArray asMessageLines;
	split( str, "\n", asMessageLines, false );
	while( asMessageLines.size() < 3 )
		asMessageLines.push_back( "" );
	
	const int msgs[] = { IDC_STATIC_MESSAGE1, IDC_STATIC_MESSAGE2, IDC_STATIC_MESSAGE3 };
	for( unsigned i = 0; i < 3; ++i )
	{
		if( text[i] == asMessageLines[i] )
			continue;
		text[i] = asMessageLines[i];

		SendDlgItemMessage( hwnd, msgs[i], WM_SETTEXT, 0, 
			(LPARAM)asMessageLines[i].c_str());
	}
}

/*
 * (c) 2001-2004 Chris Danford, Glenn Maynard
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
