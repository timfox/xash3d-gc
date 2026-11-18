/*
hitext_gamecube.c - hardware resolution menu text for GameCube

The soft 2D buffer is 320x240 and every glyph drawn into it is doubled on the way to the 640x480 XFB. The
glyphs queued here are drawn as GX quads at 640x480 over the presented soft frame instead, from a glyph
atlas baked by scripts/bake-hitext.py (Noto Sans, SIL OFL 1.1) and loaded from gfx/gc_hitext.bin.

Only mainui glyphs (pfnDrawCharacter with the resident credits font) use it. When the atlas is missing, the
queue is full, or the on-screen keyboard is up, the caller falls back to the soft path (cvar gc_hitext 0
switches it off).
*/
#include "common.h"
#include "client.h"

#if XASH_GAMECUBE
#include <gccore.h>
#include <malloc.h>
#include <string.h>

#define HT_MAX_QUEUE 900

typedef struct
{
	unsigned short x, y, w, h;
	short xoff, yoff;
	unsigned short adv;
} ht_glyph_t;

static struct
{
	qboolean   tried, ready;
	int        tex_w, tex_h, line_h, first, count;
	ht_glyph_t glyph[96];
	byte      *tex;
	GXTexObj   obj;
} ht;

static struct
{
	float x, y, h;     // 640x480 space: the cell's top left and its height
	float w;           // cell width
	unsigned short ch; // index into ht.glyph
	unsigned int rgba; // 0xRRGGBBAA
} ht_queue[HT_MAX_QUEUE];
static int ht_count;

static CVAR_DEFINE_AUTO( gc_hitext, "1", FCVAR_ARCHIVE, "draw menu text at 640x480 with GX quads (0 = soft 320x240 text)" );
// set by the main menu while it draws: its labels keep the original text
static CVAR_DEFINE_AUTO( gc_hitext_pause, "0", FCVAR_GAMEUIDLL, "main menu draws its own text" );

static unsigned short HT_U16( const byte *p ) { return (unsigned short)(( p[0] << 8 ) | p[1] ); }
static short HT_S16( const byte *p ) { return (short)HT_U16( p ); }

void GC_HiTextInit( void )
{
	Cvar_RegisterVariable( &gc_hitext );
	Cvar_RegisterVariable( &gc_hitext_pause );
}

static qboolean HT_Load( void )
{
	fs_offset_t len = 0;
	byte *file;
	int i, tex_bytes;
	const byte *p;

	ht.tried = true;
	file = FS_LoadFile( "gfx/gc_hitext.bin", &len, false );
	if( !file || len < 16 || memcmp( file, "HTXT", 4 ))
	{
		Con_Reportf( "Xash3D GameCube: hitext atlas missing (gfx/gc_hitext.bin), soft text\n" );
		if( file ) Mem_Free( file );
		return false;
	}

	ht.tex_w = HT_U16( file + 6 );
	ht.tex_h = HT_U16( file + 8 );
	ht.line_h = HT_U16( file + 10 );
	ht.first = HT_U16( file + 12 );
	ht.count = HT_U16( file + 14 );
	tex_bytes = ht.tex_w * ht.tex_h / 2;
	if( ht.count < 1 || ht.count > 96 || len < 16 + ht.count * 14 + tex_bytes )
	{
		Con_Reportf( "Xash3D GameCube: hitext atlas malformed\n" );
		Mem_Free( file );
		return false;
	}
	p = file + 16;
	for( i = 0; i < ht.count; i++, p += 14 )
	{
		ht.glyph[i].x = HT_U16( p );
		ht.glyph[i].y = HT_U16( p + 2 );
		ht.glyph[i].w = HT_U16( p + 4 );
		ht.glyph[i].h = HT_U16( p + 6 );
		ht.glyph[i].xoff = HT_S16( p + 8 );
		ht.glyph[i].yoff = HT_S16( p + 10 );
		ht.glyph[i].adv = HT_U16( p + 12 );
	}
	ht.tex = (byte *)memalign( 32, tex_bytes );
	if( !ht.tex )
	{
		Con_Reportf( "Xash3D GameCube: hitext atlas alloc %d bytes failed, soft text\n", tex_bytes );
		Mem_Free( file );
		return false;
	}
	memcpy( ht.tex, p, tex_bytes );
	DCFlushRange( ht.tex, tex_bytes );
	GX_InitTexObj( &ht.obj, ht.tex, (u16)ht.tex_w, (u16)ht.tex_h, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE );
	GX_InitTexObjFilterMode( &ht.obj, GX_LINEAR, GX_LINEAR );
	Mem_Free( file );
	ht.ready = true;
	Con_Reportf( "Xash3D GameCube: hitext atlas ready %dx%d glyphs=%d bytes=%d\n", ht.tex_w, ht.tex_h, ht.count, tex_bytes );
	return true;
}

// Called when a map loads: give the atlas back, the match uses the soft path.
void GC_HiTextRelease( void )
{
	if( ht.tex )
	{
		free( ht.tex );
		ht.tex = NULL;
	}
	ht.ready = false;
	ht.tried = false;
	ht_count = 0;
}

qboolean GC_HiTextActive( void )
{
	if( cls.state == ca_active )
	{
		if( ht.tex )
			GC_HiTextRelease();
		return false;
	}
	if( !gc_hitext.value || gc_hitext_pause.value )
		return false;
	if( !ht.tried && !HT_Load( ))
		return false;
	return ht.ready;
}

/* x, y, w, h: the cell in screen pixels (refState size), ulRGBA as pfnDrawCharacter gets it (ARGB). */
qboolean GC_HiTextGlyph( int ch, float x, float y, float w, float h, int ulRGBA )
{
	int i;

	ch &= 255;
	i = ch - ht.first;
	if( i < 0 || i >= ht.count || ht_count >= HT_MAX_QUEUE )
		return false;

	ht_queue[ht_count].x = x;
	ht_queue[ht_count].y = y;
	ht_queue[ht_count].w = w;
	ht_queue[ht_count].h = h;
	ht_queue[ht_count].ch = (unsigned short)i;
	ht_queue[ht_count].rgba = ( (unsigned)( ulRGBA & 0x00FFFFFF ) << 8 ) | (unsigned)(( ulRGBA >> 24 ) & 0xFF );
	ht_count++;
	return true;
}

/* A filled box drawn after some text covers it: drop the glyphs under it (dialogs over a page). */
void GC_HiTextOcclude( int x, int y, int w, int h, int alpha )
{
	const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	int i, out = 0;

	if( !ht_count || alpha < 160 )
		return;
	for( i = 0; i < ht_count; i++ )
	{
		const float cx = ht_queue[i].x + ht_queue[i].w * 0.5f, cy = ht_queue[i].y + ht_queue[i].h * 0.5f;

		if( cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1 )
			continue;
		ht_queue[out++] = ht_queue[i];
	}
	ht_count = out;
}

/* At the end of a frame, before CopyDisp: set the whole GX pipe for a 640x480 ortho quad stream (the renderer
 * before us left its own state in it) and draw the queued glyphs over the finished frame. */
qboolean GC_HiTextFlush( float fb_w, float fb_h )
{
	int i;
	Mtx44 proj;
	Mtx modelview;
	const float inv_w = 1.0f / (float)ht.tex_w, inv_h = 1.0f / (float)ht.tex_h;
	// the EFB is fb_w x fb_h (320x480 on this video mode, 640x480 on the preferred one); the menu lays out in refState
	const float sx = fb_w / (float)refState.width, sy = fb_h / (float)refState.height;

	if( !ht_count )
		return false;
	if( !ht.ready )
	{
		ht_count = 0;
		return false;
	}

	GX_SetViewport( 0.0f, 0.0f, fb_w, fb_h, 0.0f, 1.0f );
	GX_SetScissor( 0, 0, (u32)fb_w, (u32)fb_h );
	guOrtho( proj, 0.0f, fb_h, 0.0f, fb_w, 0.0f, 1.0f );
	GX_LoadProjectionMtx( proj, GX_ORTHOGRAPHIC );
	guMtxIdentity( modelview );
	GX_LoadPosMtxImm( modelview, GX_PNMTX0 );

	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_SetNumTexGens( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetNumTevStages( 1 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	// colour from the vertex, alpha = glyph coverage * vertex alpha
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaUpdate( GX_FALSE );
	GX_SetCullMode( GX_CULL_NONE );

	{
		extern void R_GXInvalidateTriApiLayout( void );

		R_GXInvalidateTriApiLayout();
	}
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );
	GX_InvVtxCache();
	GX_InvalidateTexAll();
	GX_LoadTexObj( &ht.obj, GX_TEXMAP0 );

	for( i = 0; i < ht_count; i++ )
	{
		const ht_glyph_t *g = &ht.glyph[ht_queue[i].ch];
		// height sets the size; a pixel is sx/sy as wide as it is tall, so the glyph is that much narrower
		const float scale_y = ht_queue[i].h * sy / (float)ht.line_h, scale_x = scale_y * sx / sy;
		const float gw = g->w * scale_x, gh = g->h * scale_y;
		// centre the natural glyph in the cell the menu laid out
		const float gx = ht_queue[i].x * sx + ( ht_queue[i].w * sx - g->adv * scale_x ) * 0.5f + g->xoff * scale_x;
		const float gy = ht_queue[i].y * sy + g->yoff * scale_y;
		const float s0 = g->x * inv_w, s1 = ( g->x + g->w ) * inv_w;
		const float t0 = g->y * inv_h, t1 = ( g->y + g->h ) * inv_h;
		const unsigned int c = ht_queue[i].rgba;

		if( !g->w || !g->h )
			continue;
		GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
		GX_Position3f32( gx, gy, 0.0f );
		GX_Color1u32( c );
		GX_TexCoord2f32( s0, t0 );
		GX_Position3f32( gx + gw, gy, 0.0f );
		GX_Color1u32( c );
		GX_TexCoord2f32( s1, t0 );
		GX_Position3f32( gx + gw, gy + gh, 0.0f );
		GX_Color1u32( c );
		GX_TexCoord2f32( s1, t1 );
		GX_Position3f32( gx, gy + gh, 0.0f );
		GX_Color1u32( c );
		GX_TexCoord2f32( s0, t1 );
		GX_End();
	}
	ht_count = 0;
	return true;
}

#else
void GC_HiTextInit( void ) {}
void GC_HiTextRelease( void ) {}
qboolean GC_HiTextActive( void ) { return false; }
qboolean GC_HiTextGlyph( int ch, float x, float y, float w, float h, int ulRGBA ) { return false; }
void GC_HiTextOcclude( int x, int y, int w, int h, int alpha ) {}
qboolean GC_HiTextFlush( float fb_w, float fb_h ) { return false; }
#endif
