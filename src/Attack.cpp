#include "global.h"
#include "Attack.h"
#include "GameState.h"
#include "RageUtil.h"
#include "song.h"
#include "Foreach.h"

void Attack::GetAttackBeats( const Song *song, PlayerNumber pn, float &fStartBeat, float &fEndBeat ) const
{
	ASSERT( song );

	if( fStartSecond >= 0 )
	{
		CHECKPOINT;
		fStartBeat = song->GetBeatFromElapsedTime( fStartSecond );
		fEndBeat = song->GetBeatFromElapsedTime( fStartSecond+fSecsRemaining );
	} else {
		CHECKPOINT;
		/* If fStartSecond < 0, then the attack starts right off the screen; this requires
		 * that a song actually be playing.  Pre-queued course attacks must always have 
		 * fStartSecond >= 0. */
		ASSERT( GAMESTATE->m_pCurSong );
		
		/* We're setting this effect on the fly.  If it's an arrow-changing effect
		 * (transform or note skin), apply it in the future, after what's currently on
		 * screen, so new arrows will scroll on screen with this effect. */
		GAMESTATE->GetUndisplayedBeats( pn, fSecsRemaining, fStartBeat, fEndBeat );
	}

	ASSERT_M( fEndBeat >= fStartBeat, ssprintf("%f >= %f", fEndBeat, fStartBeat) );
}

bool Attack::operator== ( const Attack &rhs ) const
{
#define EQUAL(a) (a==rhs.a)
	return 
		EQUAL(level) &&
		EQUAL(fStartSecond) &&
		EQUAL(fSecsRemaining) &&
		EQUAL(sModifier) &&
		EQUAL(bOn) &&
		EQUAL(bGlobal) &&
		EQUAL(bFromSong);
}

bool Attack::ContainsTransformOrTurn() const
{
	PlayerOptions po;
	po.FromString( sModifier );
	return po.ContainsTransformOrTurn();
}

bool AttackArray::ContainsTransformOrTurn() const
{
	FOREACH_CONST( Attack, *this, a )
	{
		if( a->ContainsTransformOrTurn() )
			return true;
	}
	return false;
}

void ParseAttackParams( const vector<CString> &asParams, Attack attack, AttackArray &out )
{
	float end = -9999;
	for( unsigned j = 1; j < asParams.size(); ++j )
	{
		CStringArray sBits;
		split( asParams[j], "=", sBits, false );
		if( sBits.size() < 2 )
			continue;

		TrimLeft( sBits[0] );
		TrimRight( sBits[0] );
		if( !sBits[0].CompareNoCase("TIME") )
			attack.fStartSecond = strtof( sBits[1], NULL );
		else if( !sBits[0].CompareNoCase("LEN") )
			attack.fSecsRemaining = strtof( sBits[1], NULL );
		else if( !sBits[0].CompareNoCase("END") )
			end = strtof( sBits[1], NULL );
		else if( !sBits[0].CompareNoCase("MODS") )
		{
			if( end != -9999 )
			{
				attack.fSecsRemaining = end - attack.fStartSecond;
				end = -9999;
			}
			/* Drop the line break before the next ":TIME=", or it is written back
			 * as an extra blank line every time the file is saved. */
			attack.sModifier = sBits[1];
			TrimLeft( attack.sModifier );
			TrimRight( attack.sModifier );
			out.push_back( attack );
		}
	}
}

/*
 * (c) 2003-2004 Chris Danford
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
