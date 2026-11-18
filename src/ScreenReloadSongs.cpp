#include "global.h"
#include "ScreenReloadSongs.h"
#include "SongManager.h"
#include "UnlockSystem.h"
#include "ScreenManager.h"
#include "RageTimer.h"
#include "RageLog.h"
#include "ThemeManager.h"
#include "GameState.h"
#include "NetworkSyncManager.h"
#include "ProfileManager.h"
#include "Song.h"

#include "arch/LoadingWindow/LoadingWindow.h"

static const int DrawFrameRate = 20;
class ScreenReloadSongsLoadingWindow: public LoadingWindow
{
	RageTimer m_LastDraw;
	BitmapText &m_BitmapText;

public:
	ScreenReloadSongsLoadingWindow( BitmapText &bt ):
		m_BitmapText(bt)
	{
	}

	void SetText( CString str )
	{
		m_BitmapText.SetText( str );
		Paint();
	}

	void Paint()
	{
		/* We load songs much faster than we draw frames.  Cap the draw rate, so we don't
		 * slow down the reload. */
		if( m_LastDraw.PeekDeltaTime() < 1.0f/DrawFrameRate )
			return;
		m_LastDraw.GetDeltaTime();

		SCREENMAN->Draw();
	}
};

/* This could be cleaned up: show progress, for example.  Let's not use
 * this for the initial load, since we don't want to start up the display
 * until we finish loading songs; that way, people can continue to use their
 * computer while songs load. */
ScreenReloadSongs::ScreenReloadSongs( CString sClassName ): Screen(sClassName)
{
	m_iUpdates = 0;

	m_Loading.LoadFromFont( THEME->GetPathF("Common", "normal") );
	m_Loading.SetXY( CENTER_X, CENTER_Y );
	this->AddChild( &m_Loading );

	m_LoadingWindow = new ScreenReloadSongsLoadingWindow( m_Loading );
}

ScreenReloadSongs::~ScreenReloadSongs()
{
	delete m_LoadingWindow;
}


void ScreenReloadSongs::Update( float fDeltaTime )
{
	Screen::Update( fDeltaTime );
	/* Start the reload on the second update.  On the first, 0, SCREENMAN->Draw won't draw. */
	++m_iUpdates;
	if( m_iUpdates != 2 )
		return;
	ASSERT( !IsFirstUpdate() );

	/* Callers use these flags to tell this screen what to reload.  Read them and
	 * clear them right away, so whichever path we take below, they can't leak
	 * into a later reload. */
	const bool bOnlyConnect = GAMESTATE->m_bLoadPackConnect;
	const CString sOnlyGroup = bOnlyConnect? CString("connect") : GAMESTATE->m_pCurSongGroup;
	GAMESTATE->m_bLoadPackConnect = false;
	GAMESTATE->m_pCurSongGroup = "";

	if(GAMESTATE->m_bfastLoadInScreenSelectMusic)//fast load in ScreenSelectMusic
	{
		// GAMESTATE->Reset();
		GAMESTATE->m_pCurSong = NULL;
		GAMESTATE->m_pPreferredSong = NULL;
		FOREACH_PlayerNumber( p )
			GAMESTATE->m_pCurSteps[p] = NULL;
		GAMESTATE->m_pCurCourse = NULL;
		GAMESTATE->m_pPreferredCourse = NULL;
		FOREACH_PlayerNumber( p )
			GAMESTATE->m_pCurTrail[p] = NULL;
		if( sOnlyGroup.empty() )
		{
			LOG->Info("Fast reload all group");
			SONGMAN->FastReload(m_LoadingWindow);
		}
		else
		{
			LOG->Info("Fast reload group %s",sOnlyGroup.c_str());
			SONGMAN->ReloadGroup( sOnlyGroup, m_LoadingWindow );
		}
		UNLOCKMAN->UpdateSongs();
		SONGMAN->UpdateBest();
		SONGMAN->UpdateShuffled();
		//===========
		FOREACH_HumanPlayer( pn )
		{
			Profile* pProfile = PROFILEMAN->GetProfile(pn);
			if( GAMESTATE->m_pPreferredSong == NULL )
				GAMESTATE->m_pPreferredSong = pProfile->m_lastSong.ToSong();
			// if(GAMESTATE->m_pCurSongGroup!="" && GAMESTATE->m_pPreferredSong!=NULL)
			// {
			// 	GAMESTATE->m_pCurSongGroup = GAMESTATE->m_pPreferredSong->m_sGroupName;
			// }
			if( GAMESTATE->m_pPreferredCourse == NULL )
				GAMESTATE->m_pPreferredCourse = pProfile->m_lastCourse.ToCourse();
		}
		//===========
		GAMESTATE->PlayersFinalized();
		GAMESTATE->m_bfastLoadInScreenSelectMusic=false;
		SCREENMAN->SetNewScreen( "ScreenSelectMusic" );
		return;
	}
	else if(GAMESTATE->m_bEditing)//fast load
	{
		// fast reload
		if( !sOnlyGroup.empty() )
			SONGMAN->ReloadGroup( sOnlyGroup, m_LoadingWindow );
		else
		{
			/* A full reload deletes every song; GAMESTATE must not keep pointing at them. */
			GAMESTATE->m_pCurSong = NULL;
			GAMESTATE->m_pPreferredSong = NULL;
			FOREACH_PlayerNumber( p )
			{
				GAMESTATE->m_pCurSteps[p] = NULL;
				GAMESTATE->m_pCurTrail[p] = NULL;
			}
			GAMESTATE->m_pCurCourse = NULL;
			GAMESTATE->m_pPreferredCourse = NULL;
			SONGMAN->FastReload(m_LoadingWindow);
		}
		UNLOCKMAN->UpdateSongs();
		SONGMAN->UpdateBest();
		SONGMAN->UpdateShuffled();
		SCREENMAN->PopTopScreen();
	}
	else
	{
		SONGMAN->Reload( m_LoadingWindow );
		UNLOCKMAN->UpdateSongs();
		SCREENMAN->SetNewScreen( "ScreenTitleMenu" );
	}
}

/*
 * (c) 2003-2004 Glenn Maynard
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
