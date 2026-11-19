#include "global.h"
#include "LaneBackplate.h"
#include "GameState.h"
#include "PlayerOptions.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "ScreenDimensions.h"
#include "Style.h"
#include "GameConstantsAndTypes.h"

const CString LaneBackplate::SOLID = "solid";

/* Where the backplate images are, relative to the game's root (like Characters). */
static const CString BACKPLATES_DIR = "Backplates/";

static const char *const IMAGE_EXTENSIONS[] = { "png", "jpg", "jpeg", "bmp", "gif" };

static bool IsImageFile( const CString &sPath )
{
	CString sExt = GetExtension( sPath );
	sExt.MakeLower();
	for( unsigned i = 0; i < RageARRAYSIZE(IMAGE_EXTENSIONS); ++i )
		if( sExt == IMAGE_EXTENSIONS[i] )
			return true;
	return false;
}

void LaneBackplate::GetBackplateNames( CStringArray &asNamesOut )
{
	CStringArray asFiles;
	GetDirListing( BACKPLATES_DIR + "*", asFiles, false, false );
	SortCStringArray( asFiles );

	for( unsigned i = 0; i < asFiles.size(); ++i )
	{
		if( !IsImageFile(asFiles[i]) )
			continue;
		const CString sName = SetExtension( asFiles[i], "" );
		/* Two files with the same name and different extensions are one choice. */
		if( !asNamesOut.empty() && !asNamesOut.back().CompareNoCase(sName) )
			continue;
		asNamesOut.push_back( sName );
	}
}

/* The image file for a name from GetBackplateNames(), compared without case
 * (PlayerOptions keeps names in lower case); "" if there's none. */
CString LaneBackplate::GetBackplatePath( const CString &sName )
{
	CStringArray asFiles;
	GetDirListing( BACKPLATES_DIR + "*", asFiles, false, false );
	SortCStringArray( asFiles );

	for( unsigned i = 0; i < asFiles.size(); ++i )
	{
		if( IsImageFile(asFiles[i]) && !SetExtension(asFiles[i], "").CompareNoCase(sName) )
			return BACKPLATES_DIR + asFiles[i];
	}
	return "";
}

LaneBackplate::LaneBackplate()
{
	m_PlayerNumber = PLAYER_1;
	m_bShow = false;
	m_bImage = false;

	m_quadSolid.SetWidth( 1 );
	m_quadSolid.SetHeight( 1 );
}

void LaneBackplate::Load( PlayerNumber pn )
{
	m_PlayerNumber = pn;
	m_bShow = false;
	m_bImage = false;
	m_sprImage.UnloadTexture();

	const CString &sBackplate = GAMESTATE->m_PlayerOptions[pn].m_sBackplate;
	if( sBackplate.empty() )
		return;

	m_bShow = true;
	if( sBackplate == SOLID )
		return;

	const CString sPath = GetBackplatePath( sBackplate );
	if( sPath.empty() )
	{
		/* The image is gone (e.g. options saved on another machine); show the plain
		 * plate rather than nothing, since the player wants one. */
		LOG->Warn( "Backplate \"%s\" not found in %s", sBackplate.c_str(), BACKPLATES_DIR.c_str() );
		return;
	}

	m_sprImage.Load( sPath );
	m_bImage = true;
}

void LaneBackplate::DrawPrimitives()
{
	if( !m_bShow )
		return;

	const float fAlpha = (1 - GAMESTATE->m_PlayerOptions[m_PlayerNumber].m_fBackplateTransparency) * m_pTempState->diffuse[0].a;
	if( fAlpha <= 0 )
		return;

	/* The screen's center in our coordinates (unzoomed; Player puts us at the note
	 * field's Y).  Cover one screen height there and one more above and below, which
	 * is enough for Mini (half size) and for tilted lanes. */
	const float fCenterY = CENTER_Y - GetY();

	/* From the left edge of the leftmost lane to the right edge of the rightmost,
	 * as NoteField::GetWidth measures them. */
	float fMinX, fMaxX;
	GAMESTATE->GetCurrentStyle()->GetMinAndMaxColX( m_PlayerNumber, fMinX, fMaxX );
	const float fLeft = fMinX - ARROW_SIZE/2, fRight = fMaxX + ARROW_SIZE/2;

	if( !m_bImage )
	{
		m_quadSolid.StretchTo( RectF(fLeft, fCenterY - SCREEN_HEIGHT*1.5f, fRight, fCenterY + SCREEN_HEIGHT*1.5f) );
		m_quadSolid.SetDiffuse( RageColor(0,0,0,fAlpha) );
		m_quadSolid.Draw();
		return;
	}

	/* The image covers the lanes for one screen height; repeat it above and below. */
	m_sprImage.SetDiffuse( RageColor(1,1,1,fAlpha) );
	for( int i = -1; i <= 1; ++i )
	{
		const float fTop = fCenterY + (i-0.5f)*SCREEN_HEIGHT;
		m_sprImage.StretchTo( RectF(fLeft, fTop, fRight, fTop + SCREEN_HEIGHT) );
		m_sprImage.Draw();
	}
}
