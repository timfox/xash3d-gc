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
 *   Start          OK (Enter)
 *   Z              cancel (Escape)
 */

#define OSK_COLS  10
#define OSK_ROWS  4 // character rows, plus one row of action keys

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

static void OSK_SendChar( int ch )
{
	// do not pass UTF-8 sequence into the engine, convert it here
	if( !cls.accept_utf8 )
		ch = Con_UtfProcessCharForce( ch );

	if( ch )
		CL_CharEvent( ch );
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
		case OSK_ACT_DELETE: OSK_SendKey( K_BACKSPACE ); break;
		case OSK_ACT_OK:     OSK_SendKey( K_ENTER ); break;
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
	case K_B_BUTTON:   OSK_SendKey( K_BACKSPACE ); break;
	case K_X_BUTTON:   OSK_SendChar( ' ' ); break;
	case K_Y_BUTTON:
	case K_R1_BUTTON:  OSK_ChangePage( 1 ); break;
	case K_L1_BUTTON:  OSK_ChangePage( -1 ); break;
	case K_START_BUTTON: OSK_SendKey( K_ENTER ); break;
	case K_Z_BUTTON:   OSK_SendKey( K_ESCAPE ); break;
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
		osk.page = OSK_PAGE_LOWER;
		osk.x = osk.y = 0;
	}
}

// layout in the 640x480 virtual screen
#define OSK_CELL    36
#define OSK_GAP     4
#define OSK_PAD     14
#define OSK_HEADER  26
#define OSK_FOOTER  22

static void OSK_Fill( float sx, float sy, int x, int y, int w, int h, byte r, byte g, byte b, byte a )
{
	ref.dllFuncs.FillRGBA( kRenderTransTexture, x * sx, y * sy, w * sx, h * sy, r, g, b, a );
}

static void OSK_Text( float sx, float sy, int x, int y, const char *s, const rgba_t color )
{
	Con_DrawString( x * sx, y * sy, s, color );
}

static void OSK_TextCentered( float sx, float sy, int x, int y, int w, int h, const char *s, const rgba_t color )
{
	int len = 0, hgt = 0;

	Con_DrawStringLen( s, &len, &hgt );
	Con_DrawString( x * sx + ( w * sx - len ) * 0.5f, y * sy + ( h * sy - hgt ) * 0.5f, s, color );
}

static void OSK_DrawKey( float sx, float sy, int x, int y, int w, int h, qboolean selected, const char *label )
{
	static const rgba_t normal = { 230, 224, 200, 255 };
	static const rgba_t dark = { 20, 24, 12, 255 };

	if( selected )
		OSK_Fill( sx, sy, x - 2, y - 2, w + 4, h + 4, 255, 200, 40, 255 );
	OSK_Fill( sx, sy, x, y, w, h, selected ? 250 : 58, selected ? 240 : 66, selected ? 200 : 40, 255 );
	OSK_TextCentered( sx, sy, x, y, w, h, label, selected ? dark : normal );
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
	const float sx = refState.width / 640.0f, sy = refState.height / 480.0f;
	const int width = OSK_COLS * ( OSK_CELL + OSK_GAP ) - OSK_GAP + OSK_PAD * 2;
	const int rows = OSK_ROWS + 1;
	const int height = OSK_HEADER + rows * ( OSK_CELL + OSK_GAP ) - OSK_GAP + OSK_FOOTER + OSK_PAD * 2;
	const int left = ( 640 - width ) / 2, top = 480 - height - 16;
	char label[2] = { 0, 0 };
	int i, j, p;

	if( !osk.enable || !osk_enable.value )
		return;

	// panel, olive drab like the retail menus
	OSK_Fill( sx, sy, left - 2, top - 2, width + 4, height + 4, 20, 22, 10, 230 );
	OSK_Fill( sx, sy, left, top, width, height, 74, 80, 48, 245 );

	// page tabs
	for( p = 0; p < OSK_PAGES; p++ )
		OSK_Text( sx, sy, left + OSK_PAD + p * 60, top + 6, osk_page_names[p], p == osk.page ? gold : dim );

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

	OSK_Text( sx, sy, left + OSK_PAD, top + height - OSK_FOOTER - 2,
		"A type  B delete  X space  Y page  Start OK  Z cancel", dim );
}

void OSK_Init( void )
{
	Cvar_RegisterVariable( &osk_enable );
}
