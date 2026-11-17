/*
in_osk.c - on-screen keyboard input
Copyright (C) 2016-2026 Alibek Omarov

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "input.h"
#include "client.h"

#if XASH_GAMECUBE
#define OSK_DEFAULT_ENABLE "1"
#else
#define OSK_DEFAULT_ENABLE "0"
#endif

CVAR_DEFINE_AUTO( osk_enable, OSK_DEFAULT_ENABLE, FCVAR_ARCHIVE | FCVAR_FILTERABLE, "enable built-in on-screen keyboard" );

/* On-screen keyboard, GameCube controller layout.
 *
 * Original implementation, modelled on the interaction of console-style
 * name-entry keyboards: three pages (abc, ABC, symbols), a 10 column grid
 * with a row of wide action keys below it.
 *
 *   D-Pad / stick  move the cursor (wraps at every edge)
 *   A              type the highlighted key
 *   B              delete the last character
 *   X              space
 *   Y, L, R        next page / previous page / next page
 *   Start          OK (Enter), closes the keyboard
 *   Z              cancel (Escape), closes the keyboard
 *
 * Text goes to the engine as character events, whatever is being edited (a menu
 * field, the console, chat). The header echoes what was typed since the keyboard
 * opened, because the panel can cover the field itself.
 */

#define OSK_COLS  10
#define OSK_ROWS  4 // character rows, plus one row of action keys
#define OSK_TEXT_MAX 96

enum { OSK_PAGE_LOWER, OSK_PAGE_UPPER, OSK_PAGE_SYMBOL, OSK_PAGES };
enum { OSK_ACT_PAGE, OSK_ACT_SPACE, OSK_ACT_DELETE, OSK_ACT_OK, OSK_ACTS };

static const char *osk_pages[OSK_PAGES][OSK_ROWS] =
{
	{ "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm.:/" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM.:/" },
	{ "!@#$%^&*()", "-_=+[]{}\\|", ";:\"',.<>/?", "~`<>#@&;$%" },
};
static const char *osk_page_names[OSK_PAGES] = { "abc", "ABC", "#+=" };
static const char *osk_act_names[OSK_ACTS] = { NULL, "Space", "Delete", "OK" };
// first column and width of each action key
static const int osk_act_col[OSK_ACTS] = { 0, 2, 6, 8 };
static const int osk_act_width[OSK_ACTS] = { 2, 4, 2, 2 };

static struct osk_s
{
	qboolean enable;
	qboolean passthru;
	int      page;
	int      x, y; // y == OSK_ROWS is the action row, x is then the column
	char     text[OSK_TEXT_MAX]; // what was typed since the keyboard opened, echoed in the header
} osk;

static int OSK_ActionAt( int x )
{
	int i;

	for( i = OSK_ACTS - 1; i > 0; i-- )
		if( x >= osk_act_col[i] )
			return i;
	return 0;
}

static void OSK_SendKey( int key )
{
	osk.passthru = true;
	Key_Event( key, true );
	Key_Event( key, false );
	osk.passthru = false;
}

static void OSK_TextAdd( int ch )
{
	size_t len = Q_strlen( osk.text );

	if( ch >= 32 && ch < 127 && len + 1 < sizeof( osk.text ))
	{
		osk.text[len] = ch;
		osk.text[len + 1] = 0;
	}
}

static void OSK_TextDelete( void )
{
	size_t len = Q_strlen( osk.text );

	if( len > 0 )
		osk.text[len - 1] = 0;
}

// OK and cancel end the edit. The menu does not always tell the engine that the
// field lost focus (a page closed by Escape never does), so do not wait for it,
// otherwise the keyboard stays up and swallows the pad on a page without fields.
static void OSK_Close( int key )
{
	OSK_SendKey( key );
	osk.enable = false;
	osk.text[0] = 0;
}

static void OSK_SendChar( int ch )
{
	// do not pass UTF-8 sequence into the engine, convert it here
	if( !cls.accept_utf8 )
		ch = Con_UtfProcessCharForce( ch );

	if( ch )
	{
		CL_CharEvent( ch );
		OSK_TextAdd( ch );
	}
}

static void OSK_ChangePage( int dir )
{
	osk.page = ( osk.page + dir + OSK_PAGES ) % OSK_PAGES;
}

static void OSK_Move( int dx, int dy )
{
	if( dy )
	{
		int act = osk.y == OSK_ROWS ? OSK_ActionAt( osk.x ) : -1;

		osk.y = ( osk.y + dy + OSK_ROWS + 1 ) % ( OSK_ROWS + 1 );

		// the action row is coarser than the grid, keep the cursor over the same key
		if( act >= 0 && osk.y != OSK_ROWS )
			osk.x = osk_act_col[act] + osk_act_width[act] / 2;
	}

	if( dx )
	{
		if( osk.y == OSK_ROWS )
		{
			int act = ( OSK_ActionAt( osk.x ) + dx + OSK_ACTS ) % OSK_ACTS;
			osk.x = osk_act_col[act];
		}
		else osk.x = ( osk.x + dx + OSK_COLS ) % OSK_COLS;
	}
}

static void OSK_Activate( void )
{
	if( osk.y == OSK_ROWS )
	{
		switch( OSK_ActionAt( osk.x ))
		{
		case OSK_ACT_PAGE:   OSK_ChangePage( 1 ); break;
		case OSK_ACT_SPACE:  OSK_SendChar( ' ' ); break;
		case OSK_ACT_DELETE: OSK_SendKey( K_BACKSPACE ); OSK_TextDelete(); break;
		case OSK_ACT_OK:     OSK_Close( K_ENTER ); break;
		}
		return;
	}

	OSK_SendChar( (byte)osk_pages[osk.page][osk.y][osk.x] );
}

qboolean OSK_KeyEvent( int key, int down )
{
	qboolean ours;

	if( !osk.enable || !osk_enable.value || osk.passthru )
		return false;

	// physical keyboards keep working, only the pad and arrows drive the OSK
	ours = ( key >= K_AUX1 && key <= K_AUX32 ) || key == K_UPARROW || key == K_DOWNARROW
		|| key == K_LEFTARROW || key == K_RIGHTARROW || key == K_LTRIGGER || key == K_RTRIGGER;

	if( !ours )
		return false;

	if( !down )
		return true;

	switch( key )
	{
	case K_UPARROW:
	case K_DPAD_UP:    OSK_Move( 0, -1 ); break;
	case K_DOWNARROW:
	case K_DPAD_DOWN:  OSK_Move( 0, 1 ); break;
	case K_LEFTARROW:
	case K_DPAD_LEFT:  OSK_Move( -1, 0 ); break;
	case K_RIGHTARROW:
	case K_DPAD_RIGHT: OSK_Move( 1, 0 ); break;
	case K_A_BUTTON:   OSK_Activate(); break;
	case K_B_BUTTON:   OSK_SendKey( K_BACKSPACE ); OSK_TextDelete(); break;
	case K_X_BUTTON:   OSK_SendChar( ' ' ); break;
	case K_Y_BUTTON:
	case K_R1_BUTTON:  OSK_ChangePage( 1 ); break;
	case K_L1_BUTTON:  OSK_ChangePage( -1 ); break;
	case K_START_BUTTON: OSK_Close( K_ENTER ); break;
	case K_Z_BUTTON:   OSK_Close( K_ESCAPE ); break;
	default: break;
	}

	return true;
}

/*
=============
OSK_EnableTextInput

Enables built-in IME
=============
*/
void OSK_EnableTextInput( qboolean enable, qboolean force )
{
	qboolean old = osk.enable;

	osk.enable = enable;

	if( osk.enable && ( !old || force ))
	{
		const char *initial = Key_GetTextInputBuffer(); // set by menus that pass the field text

		osk.page = OSK_PAGE_LOWER;
		osk.x = osk.y = 0;
		Q_strncpy( osk.text, initial ? initial : "", sizeof( osk.text ));
	}
	else if( !osk.enable )
		osk.text[0] = 0;
}

// layout in the 640x480 virtual screen
#define OSK_CELL    36
#define OSK_GAP     4
#define OSK_PAD     14
#define OSK_HEADER  26
#define OSK_FOOTER  38

// The keyboard draws its own text from this 5x8 bitmap font, as filled rectangles. The console font
// needs its texture resident, and the in-game renderer gives that texture away during a map load.
// Glyphs are rows of 5 bits, ASCII 32..126, from the X11 misc-fixed 5x8 font (public domain).
#define OSK_GLYPH_W   5
#define OSK_GLYPH_H   8
#define OSK_GLYPH_ADV 6
#define OSK_FP        2 // virtual pixels per font pixel: one or two real pixels at 320 or 640 wide

static const byte osk_font[95][OSK_GLYPH_H] =
{
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // ' '
	{ 0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x00 }, // '!'
	{ 0x00,0x0a,0x0a,0x0a,0x00,0x00,0x00,0x00 }, // '"'
	{ 0x0a,0x0a,0x1f,0x0a,0x1f,0x0a,0x0a,0x00 }, // '#'
	{ 0x04,0x0e,0x14,0x0e,0x05,0x0e,0x04,0x00 }, // '$'
	{ 0x00,0x08,0x0a,0x04,0x0a,0x02,0x00,0x00 }, // '%'
	{ 0x08,0x14,0x14,0x08,0x14,0x14,0x0a,0x00 }, // '&'
	{ 0x00,0x04,0x04,0x04,0x00,0x00,0x00,0x00 }, // '''
	{ 0x00,0x04,0x08,0x08,0x08,0x08,0x04,0x00 }, // '('
	{ 0x00,0x08,0x04,0x04,0x04,0x04,0x08,0x00 }, // ')'
	{ 0x00,0x00,0x12,0x0c,0x1e,0x0c,0x12,0x00 }, // '*'
	{ 0x00,0x00,0x04,0x04,0x1f,0x04,0x04,0x00 }, // '+'
	{ 0x00,0x00,0x00,0x00,0x00,0x06,0x04,0x08 }, // ','
	{ 0x00,0x00,0x00,0x00,0x1e,0x00,0x00,0x00 }, // '-'
	{ 0x00,0x00,0x00,0x00,0x00,0x0c,0x0c,0x00 }, // '.' (the font's own is a plus sign)
	{ 0x00,0x02,0x02,0x04,0x08,0x10,0x10,0x00 }, // '/'
	{ 0x00,0x04,0x0a,0x0a,0x0a,0x0a,0x04,0x00 }, // '0'
	{ 0x00,0x04,0x0c,0x04,0x04,0x04,0x0e,0x00 }, // '1'
	{ 0x00,0x0c,0x12,0x02,0x0c,0x10,0x1e,0x00 }, // '2'
	{ 0x00,0x1e,0x04,0x0c,0x02,0x12,0x0c,0x00 }, // '3'
	{ 0x00,0x04,0x0c,0x14,0x1e,0x04,0x04,0x00 }, // '4'
	{ 0x00,0x1e,0x10,0x1c,0x02,0x12,0x0c,0x00 }, // '5'
	{ 0x00,0x0c,0x10,0x1c,0x12,0x12,0x0c,0x00 }, // '6'
	{ 0x00,0x1e,0x02,0x04,0x04,0x08,0x08,0x00 }, // '7'
	{ 0x00,0x0c,0x12,0x0c,0x12,0x12,0x0c,0x00 }, // '8'
	{ 0x00,0x0c,0x12,0x12,0x0e,0x02,0x0c,0x00 }, // '9'
	{ 0x00,0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00 }, // ':'
	{ 0x00,0x00,0x06,0x06,0x00,0x06,0x04,0x08 }, // ';'
	{ 0x00,0x02,0x04,0x08,0x08,0x04,0x02,0x00 }, // '<'
	{ 0x00,0x00,0x00,0x1e,0x00,0x1e,0x00,0x00 }, // '='
	{ 0x00,0x08,0x04,0x02,0x02,0x04,0x08,0x00 }, // '>'
	{ 0x00,0x04,0x0a,0x02,0x04,0x00,0x04,0x00 }, // '?'
	{ 0x06,0x09,0x13,0x15,0x15,0x12,0x08,0x06 }, // '@'
	{ 0x00,0x0c,0x12,0x12,0x1e,0x12,0x12,0x00 }, // 'A'
	{ 0x00,0x1c,0x12,0x1c,0x12,0x12,0x1c,0x00 }, // 'B'
	{ 0x00,0x0c,0x12,0x10,0x10,0x12,0x0c,0x00 }, // 'C'
	{ 0x00,0x1c,0x12,0x12,0x12,0x12,0x1c,0x00 }, // 'D'
	{ 0x00,0x1e,0x10,0x1c,0x10,0x10,0x1e,0x00 }, // 'E'
	{ 0x00,0x1e,0x10,0x1c,0x10,0x10,0x10,0x00 }, // 'F'
	{ 0x00,0x0c,0x12,0x10,0x16,0x12,0x0c,0x00 }, // 'G'
	{ 0x00,0x12,0x12,0x1e,0x12,0x12,0x12,0x00 }, // 'H'
	{ 0x00,0x0e,0x04,0x04,0x04,0x04,0x0e,0x00 }, // 'I'
	{ 0x00,0x0e,0x04,0x04,0x04,0x14,0x08,0x00 }, // 'J'
	{ 0x00,0x12,0x14,0x18,0x14,0x14,0x12,0x00 }, // 'K'
	{ 0x00,0x10,0x10,0x10,0x10,0x10,0x1e,0x00 }, // 'L'
	{ 0x00,0x12,0x1e,0x1e,0x12,0x12,0x12,0x00 }, // 'M'
	{ 0x00,0x12,0x1a,0x1e,0x16,0x16,0x12,0x00 }, // 'N'
	{ 0x00,0x0c,0x12,0x12,0x12,0x12,0x0c,0x00 }, // 'O'
	{ 0x00,0x1c,0x12,0x12,0x1c,0x10,0x10,0x00 }, // 'P'
	{ 0x00,0x0c,0x12,0x12,0x1a,0x16,0x0c,0x02 }, // 'Q'
	{ 0x00,0x1c,0x12,0x12,0x1c,0x12,0x12,0x00 }, // 'R'
	{ 0x00,0x0c,0x12,0x08,0x04,0x12,0x0c,0x00 }, // 'S'
	{ 0x00,0x0e,0x04,0x04,0x04,0x04,0x04,0x00 }, // 'T'
	{ 0x00,0x12,0x12,0x12,0x12,0x12,0x0c,0x00 }, // 'U'
	{ 0x00,0x12,0x12,0x12,0x12,0x0c,0x0c,0x00 }, // 'V'
	{ 0x00,0x12,0x12,0x12,0x1e,0x1e,0x12,0x00 }, // 'W'
	{ 0x00,0x12,0x12,0x0c,0x0c,0x12,0x12,0x00 }, // 'X'
	{ 0x00,0x11,0x11,0x0a,0x04,0x04,0x04,0x00 }, // 'Y'
	{ 0x00,0x1e,0x02,0x04,0x08,0x10,0x1e,0x00 }, // 'Z'
	{ 0x00,0x0e,0x08,0x08,0x08,0x08,0x0e,0x00 }, // '['
	{ 0x00,0x10,0x10,0x08,0x04,0x02,0x02,0x00 }, // '\\'
	{ 0x00,0x0e,0x02,0x02,0x02,0x02,0x0e,0x00 }, // ']'
	{ 0x00,0x04,0x0a,0x00,0x00,0x00,0x00,0x00 }, // '^'
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1e }, // '_'
	{ 0x00,0x08,0x04,0x00,0x00,0x00,0x00,0x00 }, // '`'
	{ 0x00,0x00,0x00,0x0e,0x12,0x12,0x0e,0x00 }, // 'a'
	{ 0x00,0x10,0x10,0x1c,0x12,0x12,0x1c,0x00 }, // 'b'
	{ 0x00,0x00,0x00,0x06,0x08,0x08,0x06,0x00 }, // 'c'
	{ 0x00,0x02,0x02,0x0e,0x12,0x12,0x0e,0x00 }, // 'd'
	{ 0x00,0x00,0x00,0x0c,0x16,0x18,0x0c,0x00 }, // 'e'
	{ 0x00,0x04,0x0a,0x08,0x1c,0x08,0x08,0x00 }, // 'f'
	{ 0x00,0x00,0x00,0x0c,0x12,0x0e,0x02,0x0c }, // 'g'
	{ 0x00,0x10,0x10,0x1c,0x12,0x12,0x12,0x00 }, // 'h'
	{ 0x00,0x04,0x00,0x0c,0x04,0x04,0x0e,0x00 }, // 'i'
	{ 0x00,0x02,0x00,0x02,0x02,0x02,0x0a,0x04 }, // 'j'
	{ 0x00,0x10,0x10,0x12,0x1c,0x12,0x12,0x00 }, // 'k'
	{ 0x00,0x0c,0x04,0x04,0x04,0x04,0x0e,0x00 }, // 'l'
	{ 0x00,0x00,0x00,0x1a,0x15,0x15,0x15,0x00 }, // 'm'
	{ 0x00,0x00,0x00,0x1c,0x12,0x12,0x12,0x00 }, // 'n'
	{ 0x00,0x00,0x00,0x0c,0x12,0x12,0x0c,0x00 }, // 'o'
	{ 0x00,0x00,0x00,0x1c,0x12,0x1c,0x10,0x10 }, // 'p'
	{ 0x00,0x00,0x00,0x0e,0x12,0x0e,0x02,0x02 }, // 'q'
	{ 0x00,0x00,0x00,0x14,0x1a,0x10,0x10,0x00 }, // 'r'
	{ 0x00,0x00,0x00,0x06,0x0c,0x02,0x0c,0x00 }, // 's'
	{ 0x00,0x08,0x08,0x1c,0x08,0x0a,0x04,0x00 }, // 't'
	{ 0x00,0x00,0x00,0x12,0x12,0x12,0x0e,0x00 }, // 'u'
	{ 0x00,0x00,0x00,0x0a,0x0a,0x0a,0x04,0x00 }, // 'v'
	{ 0x00,0x00,0x00,0x11,0x15,0x15,0x0a,0x00 }, // 'w'
	{ 0x00,0x00,0x00,0x12,0x0c,0x0c,0x12,0x00 }, // 'x'
	{ 0x00,0x00,0x00,0x12,0x12,0x0e,0x12,0x0c }, // 'y'
	{ 0x00,0x00,0x00,0x1e,0x04,0x08,0x1e,0x00 }, // 'z'
	{ 0x06,0x08,0x04,0x18,0x04,0x08,0x06,0x00 }, // '{'
	{ 0x00,0x04,0x04,0x04,0x04,0x04,0x04,0x00 }, // '|'
	{ 0x18,0x04,0x08,0x06,0x08,0x04,0x18,0x00 }, // '}'
	{ 0x00,0x0a,0x14,0x00,0x00,0x00,0x00,0x00 }, // '~'
};

static void OSK_Fill( float sx, float sy, int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	const int rx = (int)( x * sx + 0.5f ), ry = (int)( y * sy + 0.5f );
	const int rw = (int)(( x + w ) * sx + 0.5f ) - rx, rh = (int)(( y + h ) * sy + 0.5f ) - ry;

	if( rw > 0 && rh > 0 )
		ref.dllFuncs.FillRGBA( kRenderTransTexture, rx, ry, rw, rh, r, g, b, a );
}

static int OSK_TextWidth( const char *s )
{
	return (int)Q_strlen( s ) * OSK_GLYPH_ADV * OSK_FP;
}

static void OSK_Text( float sx, float sy, int x, int y, const char *s, const rgba_t color )
{
	for( ; *s; s++, x += OSK_GLYPH_ADV * OSK_FP )
	{
		const byte *glyph;
		int row, col, run;

		if( *s < 32 || *s > 126 )
			continue;

		glyph = osk_font[*s - 32];
		for( row = 0; row < OSK_GLYPH_H; row++ )
		{
			for( col = 0; col < OSK_GLYPH_W; col += run )
			{
				run = 1;
				if( !( glyph[row] & ( 0x10 >> col )))
					continue;

				while( col + run < OSK_GLYPH_W && ( glyph[row] & ( 0x10 >> ( col + run ))))
					run++;
				OSK_Fill( sx, sy, x + col * OSK_FP, y + row * OSK_FP, run * OSK_FP, OSK_FP, color[0], color[1], color[2], color[3] );
			}
		}
	}
}

static void OSK_TextCentered( float sx, float sy, int x, int y, int w, int h, const char *s, const rgba_t color )
{
	OSK_Text( sx, sy, x + ( w - OSK_TextWidth( s ) + OSK_FP ) / 2, y + ( h - OSK_GLYPH_H * OSK_FP ) / 2, s, color );
}

static void OSK_DrawKey( float sx, float sy, int x, int y, int w, int h, qboolean selected, const char *label )
{
	static const rgba_t normal = { 230, 224, 200, 255 };
	static const rgba_t gold = { 255, 210, 80, 255 };

	if( selected )
		OSK_Fill( sx, sy, x - 3, y - 3, w + 6, h + 6, 255, 200, 40, 255 );
	OSK_Fill( sx, sy, x, y, w, h, 30, 34, 20, 255 );
	OSK_TextCentered( sx, sy, x, y, w, h, label, selected ? gold : normal );
}

/*
=============
OSK_Draw

Draw on screen keyboard, if enabled
=============
*/
void OSK_Draw( void )
{
	static const rgba_t gold = { 255, 210, 80, 255 };
	static const rgba_t dim = { 190, 190, 170, 255 };
	static const rgba_t white = { 255, 255, 255, 255 };
	const float sx = refState.width / 640.0f, sy = refState.height / 480.0f;
	const int width = OSK_COLS * ( OSK_CELL + OSK_GAP ) - OSK_GAP + OSK_PAD * 2;
	const int rows = OSK_ROWS + 1;
	const int height = OSK_HEADER + rows * ( OSK_CELL + OSK_GAP ) - OSK_GAP + OSK_FOOTER + OSK_PAD * 2;
	const int left = ( 640 - width ) / 2, top = 480 - height - 16;
	const int tabw = 4 * OSK_GLYPH_ADV * OSK_FP;
	char label[2] = { 0, 0 };
	int i, j, p;

	if( !osk.enable || !osk_enable.value )
		return;

	// panel, olive drab like the retail menus
	OSK_Fill( sx, sy, left - 2, top - 2, width + 4, height + 4, 20, 22, 10, 230 );
	OSK_Fill( sx, sy, left, top, width, height, 74, 80, 48, 245 );

	// page tabs
	for( p = 0; p < OSK_PAGES; p++ )
		OSK_Text( sx, sy, left + OSK_PAD + p * tabw, top + 6, osk_page_names[p], p == osk.page ? gold : dim );

	// what was typed, the panel usually covers the field itself
	{
		const int tx = left + OSK_PAD + OSK_PAGES * tabw + 6, tw = left + width - OSK_PAD - tx;
		const char *tail = osk.text;
		char shown[OSK_TEXT_MAX + 2];

		OSK_Fill( sx, sy, tx, top + 4, tw, OSK_HEADER - 4, 28, 32, 16, 255 );
		Q_snprintf( shown, sizeof( shown ), "%s_", tail );
		while( OSK_TextWidth( shown ) > tw - 8 && *tail )
			Q_snprintf( shown, sizeof( shown ), "%s_", ++tail ); // keep the end of a long text visible
		OSK_Text( sx, sy, tx + 4, top + 6, shown, white );
	}

	for( j = 0; j < OSK_ROWS; j++ )
	{
		for( i = 0; i < OSK_COLS; i++ )
		{
			int x = left + OSK_PAD + i * ( OSK_CELL + OSK_GAP );
			int y = top + OSK_HEADER + OSK_PAD + j * ( OSK_CELL + OSK_GAP );

			label[0] = osk_pages[osk.page][j][i];
			OSK_DrawKey( sx, sy, x, y, OSK_CELL, OSK_CELL, osk.y == j && osk.x == i, label );
		}
	}

	j = OSK_ROWS;
	for( i = 0; i < OSK_ACTS; i++ )
	{
		int x = left + OSK_PAD + osk_act_col[i] * ( OSK_CELL + OSK_GAP );
		int y = top + OSK_HEADER + OSK_PAD + j * ( OSK_CELL + OSK_GAP );
		int w = osk_act_width[i] * ( OSK_CELL + OSK_GAP ) - OSK_GAP;
		const char *name = i == OSK_ACT_PAGE ? osk_page_names[( osk.page + 1 ) % OSK_PAGES] : osk_act_names[i];

		OSK_DrawKey( sx, sy, x, y, w, OSK_CELL, osk.y == OSK_ROWS && OSK_ActionAt( osk.x ) == i, name );
	}

	OSK_Text( sx, sy, left + OSK_PAD, top + height - OSK_FOOTER - OSK_PAD + 6, "A type  B delete  X space", dim );
	OSK_Text( sx, sy, left + OSK_PAD, top + height - OSK_FOOTER - OSK_PAD + 6 + OSK_GLYPH_H * OSK_FP + 2,
		"Y page  Start OK  Z cancel", dim );
}

void OSK_Init( void )
{
	Cvar_RegisterVariable( &osk_enable );
}
