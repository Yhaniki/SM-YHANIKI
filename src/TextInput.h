#ifndef TEXT_INPUT_H
#define TEXT_INPUT_H

/*
 * TextInput - text typed on the keyboard, as characters: what the keyboard layout
 * types (shifted keys, non-US layouts) and what an IME produces (Chinese and so on).
 *
 * Game input comes from DirectInput as key presses, which can't tell any of that.
 * On Windows this reads the window's character and IME messages instead.  SDL
 * dispatches window messages without translating them into characters, so key
 * presses are translated here, and only while a screen wants text: the IME is
 * turned off otherwise, so it doesn't pop up during play.
 *
 * Elsewhere there's no text; screens fall back to nothing.
 */
namespace TextInput
{
	/* Call once per frame.  bWantText: the top screen takes typed text
	 * (Screen::WantsTextInput); turns the IME on or off to match. */
	void Update( bool bWantText );

	/* The text typed since the last call, as UTF-8.  Besides printable characters
	 * it can hold '\b' for Backspace, '\r' for Enter and '\x1b' for Escape (none of
	 * them when the IME used the key); other control characters are left out. */
	CString GetTypedText();

	/* True while the IME is composing (or showing candidates), and for a moment
	 * after.  Keys then belong to the IME: a screen shouldn't also treat them as
	 * menu keys, or e.g. the Escape that cancels a composition would also leave
	 * the screen. */
	bool IsComposing();

	/* Where the IME should show what's being composed and its candidates, in screen
	 * coordinates (SCREEN_WIDTH x SCREEN_HEIGHT), e.g. at a text box. */
	void SetCompositionPos( float fX, float fY );
}

#endif
