#ifndef CURRENT_SONG_BACKGROUND_H
#define CURRENT_SONG_BACKGROUND_H

#include "Actor.h"
#include "Sprite.h"

class Song;

/*
 * CurrentSongBackground - the background of GAMESTATE->m_pCurSong, following it
 * as the current song changes (e.g. while browsing ScreenSelectMusic).
 *
 * Load it from a BGAnimation.ini layer with "File=currentsongbackground".
 * Unlike "File=songbackground", which reads the current song once when the layer
 * is created, this one reloads itself whenever the current song changes: it waits
 * until the selection has stopped moving and the song has stayed the same for a
 * moment (so scrolling the wheel doesn't load every background on the way, since
 * loading stalls the game briefly) and then crossfades to the new image.
 * When there is no song, or the song has no background image, it fades out and
 * lets whatever is behind it show through.
 *
 * Its unzoomed size is the whole screen; position, zoom and diffuse come from the
 * layer's commands as usual.
 */
class CurrentSongBackground : public Actor
{
public:
	CurrentSongBackground();

	virtual void Update( float fDeltaTime );
	virtual void DrawPrimitives();

	/* Screens with a song selection report every frame whether it's still moving
	 * (ScreenSelectMusic: the music wheel hasn't settled, e.g. a direction is held
	 * and the wheel is about to keep spinning).  Nothing is loaded while it is. */
	static void SetSelectionMoving( bool bMoving );

private:
	static CString GetImagePath( const Song *pSong );
	void ShowImage( const CString &sPath );
	void DrawImage( Sprite &spr, const RageColor diffuse[4], float fGlowAlpha );

	/* m_sprImage[m_iFront] is the newest image and fades in over the other one. */
	Sprite		m_sprImage[2];
	CString		m_sImagePath[2];	// empty = nothing loaded
	int			m_iFront;
	float		m_fFrontAlpha;

	const Song	*m_pSong;			// the song we're showing, or waiting to show
	float		m_fSecondsSinceSongChanged;
	bool		m_bWaitingToShow;
};

#endif
