#include "global.h"
#include "TextInput.h"
#include "RageUtil.h"
#include "ScreenDimensions.h"

#if defined(_WINDOWS)
#include <windows.h>
#include <imm.h>
#include "StepMania.h"	// for g_hWndMain
#pragma comment(lib, "imm32.lib")

static HWND		g_hWnd = NULL;			// the window whose messages we see
static WNDPROC	g_OldWndProc = NULL;	// its window procedure before us
static bool		g_bWantText = false;
static bool		g_bComposing = false;
static bool		g_bCandidateOpen = false;
static wstring	g_sTyped;
static bool		g_bHaveCompositionPos = false;
static float	g_fCompositionX = 0, g_fCompositionY = 0;
static DWORD	g_iCompositionEndedAt = 0;	// GetTickCount() when the IME last finished

/* How long after the IME finishes that keys still count as the IME's.  The key that
 * ends a composition (Enter, Escape, ...) also reaches the game through DirectInput,
 * which can be handled a frame after the window message. */
static const DWORD COMPOSITION_GRACE_MS = 200;

static void AddChar( wchar_t c )
{
	/* Characters outside the BMP come as surrogate pairs; wstring is UTF-16 here,
	 * but the rest of the game treats each wchar_t as a character, and the fonts
	 * couldn't show them anyway. */
	if( c >= 0xD800 && c <= 0xDFFF )
		return;
	if( c < ' ' && c != '\b' && c != '\r' && c != 0x1B )
		return;	// Ctrl+letter, Tab, ...
	if( c == 0x7F )
		return;	// Ctrl+Backspace
	g_sTyped += c;
}

/* Put the IME's composition and candidate windows at the requested spot. */
static void PositionImeWindows( HWND hWnd )
{
	if( !g_bHaveCompositionPos )
		return;

	HIMC hImc = ImmGetContext( hWnd );
	if( hImc == NULL )
		return;

	RECT rc;
	GetClientRect( hWnd, &rc );
	POINT pt;
	pt.x = (LONG) (g_fCompositionX * (rc.right - rc.left) / SCREEN_WIDTH);
	pt.y = (LONG) (g_fCompositionY * (rc.bottom - rc.top) / SCREEN_HEIGHT);

	COMPOSITIONFORM cf;
	ZERO( cf );
	cf.dwStyle = CFS_POINT;
	cf.ptCurrentPos = pt;
	ImmSetCompositionWindow( hImc, &cf );

	CANDIDATEFORM cand;
	ZERO( cand );
	cand.dwIndex = 0;
	cand.dwStyle = CFS_CANDIDATEPOS;
	cand.ptCurrentPos = pt;
	ImmSetCandidateWindow( hImc, &cand );

	ImmReleaseContext( hWnd, hImc );
}

/* Read the text the IME just finished composing. */
static void ReadCompositionResult( HWND hWnd )
{
	HIMC hImc = ImmGetContext( hWnd );
	if( hImc == NULL )
		return;

	const LONG iBytes = ImmGetCompositionStringW( hImc, GCS_RESULTSTR, NULL, 0 );
	if( iBytes > 0 )
	{
		vector<wchar_t> buf( iBytes/sizeof(wchar_t) + 1, 0 );
		ImmGetCompositionStringW( hImc, GCS_RESULTSTR, &buf[0], iBytes );
		for( unsigned i = 0; i < iBytes/sizeof(wchar_t); ++i )
			AddChar( buf[i] );
	}

	ImmReleaseContext( hWnd, hImc );
}

static LRESULT CALLBACK TextInputWndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch( msg )
	{
	case WM_KEYDOWN:
		/* SDL dispatches messages without TranslateMessage, so there would be no
		 * WM_CHAR, and the IME couldn't compose.  Translate key presses ourselves
		 * while a screen wants text. */
		if( g_bWantText )
		{
			MSG m;
			m.hwnd = hWnd;
			m.message = msg;
			m.wParam = wParam;
			m.lParam = lParam;
			m.time = GetMessageTime();
			const DWORD pos = GetMessagePos();
			m.pt.x = (short) LOWORD( pos );
			m.pt.y = (short) HIWORD( pos );
			TranslateMessage( &m );
		}
		break;

	case WM_CHAR:
		if( g_bWantText )
			AddChar( (wchar_t) wParam );
		break;

	case WM_IME_STARTCOMPOSITION:
		g_bComposing = true;
		PositionImeWindows( hWnd );
		break;

	case WM_IME_ENDCOMPOSITION:
		g_bComposing = false;
		g_iCompositionEndedAt = GetTickCount();
		break;

	case WM_IME_NOTIFY:
		if( wParam == IMN_OPENCANDIDATE )
		{
			g_bCandidateOpen = true;
			PositionImeWindows( hWnd );
		}
		else if( wParam == IMN_CLOSECANDIDATE )
		{
			g_bCandidateOpen = false;
			g_iCompositionEndedAt = GetTickCount();
		}
		break;

	case WM_IME_COMPOSITION:
		if( lParam & GCS_RESULTSTR )
		{
			if( g_bWantText )
				ReadCompositionResult( hWnd );

			/* We have the result; don't let the default handling send it again as
			 * WM_IME_CHAR and WM_CHAR.  The rest (the composition string the IME
			 * shows) still goes through. */
			lParam &= ~(GCS_RESULTSTR | GCS_RESULTCLAUSE | GCS_RESULTREADSTR | GCS_RESULTREADCLAUSE);
		}
		break;

	case WM_IME_CHAR:
		return 0;	// text from the IME is read from WM_IME_COMPOSITION
	}

	return CallWindowProcW( g_OldWndProc, hWnd, msg, wParam, lParam );
}

/* Turn the IME on for the window, or off (and drop anything half-composed). */
static void EnableIme( HWND hWnd, bool bEnable )
{
	if( bEnable )
	{
		ImmAssociateContextEx( hWnd, NULL, IACE_DEFAULT );
		return;
	}

	HIMC hImc = ImmGetContext( hWnd );
	if( hImc != NULL )
	{
		ImmNotifyIME( hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0 );
		ImmReleaseContext( hWnd, hImc );
	}
	ImmAssociateContext( hWnd, NULL );
	g_bComposing = false;
	g_bCandidateOpen = false;
}

void TextInput::Update( bool bWantText )
{
	/* The window can be recreated, e.g. when changing renderers. */
	if( g_hWndMain != g_hWnd )
	{
		g_hWnd = g_hWndMain;
		g_OldWndProc = NULL;
		g_bComposing = false;
		g_bCandidateOpen = false;
		if( g_hWnd == NULL )
			return;

		/* SetWindowLongPtrW makes the window a Unicode one, so WM_CHAR comes as
		 * UTF-16; CallWindowProcW converts it back for SDL's procedure. */
		g_OldWndProc = (WNDPROC) SetWindowLongPtrW( g_hWnd, GWLP_WNDPROC, (LONG_PTR) TextInputWndProc );
		g_bWantText = bWantText;
		EnableIme( g_hWnd, bWantText );
	}

	if( g_hWnd == NULL )
		return;

	if( bWantText != g_bWantText )
	{
		g_bWantText = bWantText;
		EnableIme( g_hWnd, bWantText );
		g_sTyped = L"";
	}
}

CString TextInput::GetTypedText()
{
	if( g_sTyped.empty() )
		return CString();

	CString sRet = WStringToCString( g_sTyped );
	g_sTyped = L"";
	return sRet;
}

bool TextInput::IsComposing()
{
	if( g_bComposing || g_bCandidateOpen )
		return true;
	return g_iCompositionEndedAt != 0 && GetTickCount() - g_iCompositionEndedAt < COMPOSITION_GRACE_MS;
}

void TextInput::SetCompositionPos( float fX, float fY )
{
	if( g_bHaveCompositionPos && fX == g_fCompositionX && fY == g_fCompositionY )
		return;

	g_bHaveCompositionPos = true;
	g_fCompositionX = fX;
	g_fCompositionY = fY;
	if( g_hWnd != NULL && g_bWantText )
		PositionImeWindows( g_hWnd );
}

#else

void TextInput::Update( bool bWantText ) { }
CString TextInput::GetTypedText() { return CString(); }
bool TextInput::IsComposing() { return false; }
void TextInput::SetCompositionPos( float fX, float fY ) { }

#endif
