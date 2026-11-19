#include "global.h"
#include "ScreenTextEntry.h"
#include "PrefsManager.h"
#include "ScreenManager.h"
#include "GameSoundManager.h"
#include "ScreenTitleMenu.h"
#include "GameConstantsAndTypes.h"
#include "PrefsManager.h"
#include "ThemeManager.h"
#include "FontCharAliases.h"
#include "GameState.h"
#include "TextInput.h"

const float QUESTION_X	=	CENTER_X;
const float QUESTION_Y	=	CENTER_Y - 60;

const float ANSWER_X	=	CENTER_X;
const float ANSWER_Y	=	CENTER_Y + 120;
const float ANSWER_WIDTH	=	440;
const float ANSWER_HEIGHT	=	30;


CString ScreenTextEntry::s_sLastAnswer = "";
bool ScreenTextEntry::s_bCancelledLast = false;


/* XXX: Don't let the user use internal-use codepoints (those
 * that resolve to Unicode codepoints above 0xFFFF); those are
 * subject to change and shouldn't be written to .SMs.
 *
 * Handle UTF-8.  Right now, we need to at least be able to backspace
 * a whole UTF-8 character.  Better would be to operate in longchars.
 */
ScreenTextEntry::ScreenTextEntry( CString sClassName, CString sQuestion, CString sInitialAnswer, void(*OnOK)(CString sAnswer), void(*OnCancel)() ) :
  Screen( sClassName )
{
	m_bIsTransparent = true;	// draw screens below us

	m_pOnOK = OnOK;
	m_pOnCancel = OnCancel;

	m_sAnswer = CStringToWstring(sInitialAnswer);
	m_bCancelled = false;

	m_Background.LoadFromAniDir( THEME->GetPathToB("ScreenPrompt background") );
	this->AddChild( &m_Background );

	m_textQuestion.LoadFromFont( THEME->GetPathToF("Common normal") );
	m_textQuestion.SetText( sQuestion );
	m_textQuestion.SetXY( QUESTION_X, QUESTION_Y );
	this->AddChild( &m_textQuestion );

	m_rectAnswerBox.SetDiffuse( RageColor(0.5f,0.5f,1.0f,0.7f) );
	m_rectAnswerBox.SetXY( ANSWER_X, ANSWER_Y );
	m_rectAnswerBox.SetZoomX( ANSWER_WIDTH );
	m_rectAnswerBox.SetZoomY( ANSWER_HEIGHT );
	this->AddChild( &m_rectAnswerBox );

	m_textAnswer.LoadFromFont( THEME->GetPathToF("Common title") );
	m_textAnswer.SetXY( ANSWER_X, ANSWER_Y );
	UpdateText();
	this->AddChild( &m_textAnswer );

	m_In.Load( THEME->GetPathToB("ScreenPrompt in") );
	m_In.StartTransitioning();
	this->AddChild( &m_In );
	
	m_Out.Load( THEME->GetPathToB("ScreenPrompt out") );
	this->AddChild( &m_Out );
}

void ScreenTextEntry::UpdateText()
{
	CString txt = WStringToCString(m_sAnswer);
	FontCharAliases::ReplaceMarkers(txt);
	m_textAnswer.SetText( txt );

	/* The IME shows what's being composed right after the answer (it's centered). */
	TextInput::SetCompositionPos( ANSWER_X + m_textAnswer.GetZoomedWidth()/2, ANSWER_Y - m_textAnswer.GetZoomedHeight()/2 );
}

void ScreenTextEntry::Update( float fDeltaTime )
{
	Screen::Update( fDeltaTime );
}

void ScreenTextEntry::DrawPrimitives()
{
	Screen::DrawPrimitives();
}

void ScreenTextEntry::Input( const DeviceInput& DeviceI, const InputEventType type, const GameInput &GameI, const MenuInput &MenuI, const StyleInput &StyleI )
{
	if( m_In.IsTransitioning() || m_Out.IsTransitioning() )
		return;

	if( type != IET_FIRST_PRESS )
		return;

	/* While the IME is composing, keys are for it: Enter takes a word, Escape
	 * cancels it. */
	if( DeviceI.device == DEVICE_KEYBOARD && TextInput::IsComposing() )
		return;

	/* Characters and Backspace come in as typed text (HandleTextInput), which
	 * follows the keyboard layout and the IME. */
	switch( DeviceI.button )
	{
	case KEY_ESC:
		m_bCancelled = true;
		MenuStart(PLAYER_1);
		break;
	case KEY_ENTER:
	case KEY_KP_ENTER:
		MenuStart(PLAYER_1);
		break;
	}
}

bool ScreenTextEntry::WantsTextInput() const
{
	return true;
}

void ScreenTextEntry::HandleTextInput( const CString &sText )
{
	if( m_In.IsTransitioning() || m_Out.IsTransitioning() )
		return;

	/* Enter and Escape are handled as keys, in Input(). */
	const wstring sTyped = CStringToWstring( sText );
	for( unsigned i = 0; i < sTyped.size(); ++i )
	{
		const wchar_t c = sTyped[i];
		if( c == L'\b' )
		{
			if(!m_sAnswer.empty())
				m_sAnswer = m_sAnswer.erase( m_sAnswer.size()-1 );
			if(GAMESTATE->m_bClearText == true)
			{
				GAMESTATE->m_bClearText = false;
				m_sAnswer.clear();
			}
		}
		else if( c >= L' ' )
		{
			m_sAnswer += c;
		}
	}
	UpdateText();
}

void ScreenTextEntry::HandleScreenMessage( const ScreenMessage SM )
{
	switch( SM )
	{
	case SM_DoneClosingWipingLeft:
		break;
	case SM_DoneClosingWipingRight:
		break;
	case SM_DoneOpeningWipingLeft:
		break;
	case SM_DoneOpeningWipingRight:
		SCREENMAN->PopTopScreen();
		break;
	}
}

void ScreenTextEntry::MenuLeft( PlayerNumber pn )
{
}

void ScreenTextEntry::MenuRight( PlayerNumber pn )
{
}

void ScreenTextEntry::MenuStart( PlayerNumber pn )
{
	m_Out.StartTransitioning( SM_DoneOpeningWipingRight );

	m_textQuestion.BeginTweening( 0.2f );
	m_textQuestion.SetDiffuse( RageColor(1,1,1,0) );

	m_rectAnswerBox.BeginTweening( 0.2f );
	m_rectAnswerBox.SetDiffuse( RageColor(1,1,1,0) );

	m_textAnswer.SetEffectNone();

	m_textAnswer.BeginTweening( 0.2f );
	m_textAnswer.SetDiffuse( RageColor(1,1,1,0) );

	SCREENMAN->PlayStartSound();

	if( m_bCancelled )
	{
		if( m_pOnCancel ) 
			m_pOnCancel();

	} 
	else 
	{
		if( m_pOnOK )
		{
			CString ret = WStringToCString(m_sAnswer);
			FontCharAliases::ReplaceMarkers(ret);
			m_pOnOK( ret );
		}
	}

	s_bCancelledLast = m_bCancelled;
	s_sLastAnswer = m_bCancelled ? CString("") : WStringToCString(m_sAnswer);
}

void ScreenTextEntry::MenuBack( PlayerNumber pn )
{
	m_bCancelled = true;
	MenuStart(pn);
}

/*
 * (c) 2001-2004 Chris Danford
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
