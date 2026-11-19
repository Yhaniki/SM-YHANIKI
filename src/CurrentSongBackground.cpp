#include "global.h"
#include "CurrentSongBackground.h"
#include "GameState.h"
#include "song.h"
#include "RageUtil.h"
#include "ScreenDimensions.h"

/* How long the selection has to stay still on the same song before its background
 * is loaded.  Loading a background image stalls the game for a moment, so don't do
 * it for songs that are only passed over, e.g. while tapping through the wheel. */
static const float SETTLE_SECONDS = 0.25f;

static bool g_bSelectionMoving = false;

void CurrentSongBackground::SetSelectionMoving( bool bMoving )
{
	g_bSelectionMoving = bMoving;
}

/* How long a new background takes to fade in over the old one. */
static const float FADE_SECONDS = 0.3f;

CurrentSongBackground::CurrentSongBackground()
{
	m_size.x = SCREEN_WIDTH;
	m_size.y = SCREEN_HEIGHT;

	m_iFront = 0;
	m_fFrontAlpha = 1;
	m_fSecondsSinceSongChanged = 0;
	m_bWaitingToShow = false;

	/* Show the current song right away, without a fade; the layer's own commands
	 * decide how it comes on screen. */
	m_pSong = GAMESTATE->m_pCurSong;
	ShowImage( GetImagePath(m_pSong) );
	m_fFrontAlpha = 1;
}

CString CurrentSongBackground::GetImagePath( const Song *pSong )
{
	if( pSong == NULL || !pSong->HasBackground() )
		return "";

	/* A movie would have to be decoded while the player is browsing; skip it. */
	const CString sPath = pSong->GetBackgroundPath();
	CString sExt = GetExtension( sPath );
	sExt.MakeLower();
	if( sExt == "avi" || sExt == "mpg" || sExt == "mpeg" )
		return "";

	return sPath;
}

/* Start fading sPath in over what's shown now.  An empty path fades out to nothing. */
void CurrentSongBackground::ShowImage( const CString &sPath )
{
	if( sPath == m_sImagePath[m_iFront] )
		return;

	/* The old front image stays behind the new one until the fade is done.  The slot
	 * that was behind is reused; if it already holds sPath (we went back to the
	 * previous song), there's nothing to load. */
	const int iNew = 1 - m_iFront;
	if( sPath != m_sImagePath[iNew] )
	{
		m_sprImage[iNew].UnloadTexture();
		m_sImagePath[iNew] = "";

		if( !sPath.empty() )
		{
			/* The load happens on the main thread, so the game stalls while it runs.
			 * Building mipmaps (done on the CPU) is about half of that time, e.g.
			 * ~160ms -> ~80ms for a 1920x1080 PNG, and they buy little for an image
			 * that's drawn about screen-sized and never moves. */
			RageTextureID ID = Sprite::SongBGTexture( sPath );
			ID.bMipMaps = false;
			m_sprImage[iNew].Load( ID );

			/* Stretch to the whole actor, like song backgrounds in gameplay. */
			m_sprImage[iNew].SetWidth( m_size.x );
			m_sprImage[iNew].SetHeight( m_size.y );
			m_sImagePath[iNew] = sPath;
		}
	}

	m_iFront = iNew;
	m_fFrontAlpha = 0;
}

void CurrentSongBackground::Update( float fDeltaTime )
{
	Actor::Update( fDeltaTime );
	for( int i = 0; i < 2; ++i )
		m_sprImage[i].Update( fDeltaTime );

	const Song *pSong = GAMESTATE->m_pCurSong;
	if( pSong != m_pSong )
	{
		m_pSong = pSong;
		m_fSecondsSinceSongChanged = 0;
		m_bWaitingToShow = true;
	}
	else if( g_bSelectionMoving )
	{
		/* Holding a direction moves the wheel one song, waits a quarter second
		 * and then keeps spinning; a plain timer would load the first song right
		 * as the wheel starts spinning.  Only count time once it's still. */
		m_fSecondsSinceSongChanged = 0;
	}
	else
	{
		m_fSecondsSinceSongChanged += fDeltaTime;
	}

	if( m_bWaitingToShow && m_fSecondsSinceSongChanged >= SETTLE_SECONDS )
	{
		m_bWaitingToShow = false;
		ShowImage( GetImagePath(m_pSong) );
		return;	// this frame's delta doesn't count toward the fade
	}

	if( m_fFrontAlpha < 1 )
	{
		/* Loading the image makes the next frame's delta long; don't let that
		 * skip most of the fade. */
		const float fStep = min( fDeltaTime, 1/30.0f );
		m_fFrontAlpha = min( 1.0f, m_fFrontAlpha + fStep / FADE_SECONDS );

		if( m_fFrontAlpha >= 1 )
		{
			/* The image behind is now covered (or faded out); let its texture go. */
			const int iBack = 1 - m_iFront;
			m_sprImage[iBack].UnloadTexture();
			m_sImagePath[iBack] = "";
		}
	}
}

void CurrentSongBackground::DrawPrimitives()
{
	const int iBack = 1 - m_iFront;
	const bool bFrontLoaded = !m_sImagePath[m_iFront].empty();
	const bool bBackLoaded = !m_sImagePath[iBack].empty();
	const float t = m_fFrontAlpha;

	if( bBackLoaded && t < 1 )
	{
		RageColor diffuse[4];
		for( int i = 0; i < 4; ++i )
		{
			diffuse[i] = m_pTempState->diffuse[i];
			float a = diffuse[i].a;
			CLAMP( a, 0.0f, 1.0f );
			if( bFrontLoaded )
			{
				/* The front image goes over this one with alpha a*t.  Pick this one's
				 * alpha x so that together they still cover exactly a, with this one
				 * making up a*(1-t) of it: x*(1-a*t) = a*(1-t).  That keeps a
				 * translucent background from flashing brighter during the fade.
				 * a*t < 1 here, since t < 1. */
				diffuse[i].a = a*(1-t) / (1-a*t);
			}
			else
			{
				diffuse[i].a = a*(1-t);
			}
		}
		DrawImage( m_sprImage[iBack], diffuse, 1-t );
	}

	if( bFrontLoaded )
	{
		RageColor diffuse[4];
		for( int i = 0; i < 4; ++i )
		{
			diffuse[i] = m_pTempState->diffuse[i];
			diffuse[i].a *= t;
		}
		DrawImage( m_sprImage[m_iFront], diffuse, t );
	}
}

/* Draw one of the images with this actor's look, using the given diffuse colors. */
void CurrentSongBackground::DrawImage( Sprite &spr, const RageColor diffuse[4], float fGlowAlpha )
{
	for( int i = 0; i < 4; ++i )
		spr.SetDiffuses( i, diffuse[i] );

	RageColor glow = m_pTempState->glow;
	glow.a *= fGlowAlpha;
	spr.SetGlow( glow );
	spr.SetGlowMode( m_pTempState->glowmode );
	spr.SetBlendMode( m_BlendMode );

	spr.Draw();
}
