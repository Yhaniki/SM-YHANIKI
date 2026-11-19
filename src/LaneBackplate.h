#ifndef LANE_BACKPLATE_H
#define LANE_BACKPLATE_H

#include "Actor.h"
#include "Sprite.h"
#include "Quad.h"
#include "PlayerNumber.h"

/*
 * LaneBackplate - a plate behind a player's lanes, so the notes stay readable over a
 * busy song background.
 *
 * It's either an image from the Backplates folder, stretched to the width of the
 * lanes, or plain black.  PlayerOptions::m_sBackplate picks which, and
 * PlayerOptions::m_fBackplateTransparency sets how much of the background shows
 * through.
 *
 * The actor goes where the note field is (same Y, zoom and tilt) and covers the
 * lanes from the top of the screen to the bottom; the image repeats above and below
 * the screen, so tilted lanes are covered all the way to the far end.  When MoveX<n>
 * moves lanes apart, the plate is cut between them and each piece moves with its
 * lanes.
 */
class LaneBackplate : public Actor
{
public:
	LaneBackplate();

	/* Load the backplate the player picked in GAMESTATE->m_PlayerOptions[pn]. */
	void Load( PlayerNumber pn );

	virtual void DrawPrimitives();

	/* PlayerOptions::m_sBackplate for a plain black plate. */
	static const CString SOLID;

	/* The names of the images in the Backplates folder (file names without the
	 * extension), for the options. */
	static void GetBackplateNames( CStringArray &asNamesOut );

private:
	static CString GetBackplatePath( const CString &sName );
	void DrawPiece( float fLeft, float fRight, float fPieceLeft, float fPieceRight,
					float fShift, float fCenterY, float fAlpha );

	PlayerNumber	m_PlayerNumber;
	bool			m_bShow;
	bool			m_bImage;		// m_sprImage, or else m_quadSolid
	Sprite			m_sprImage;
	Quad			m_quadSolid;
};

#endif
