/* Host test for engine/platform/gamecube/hud_hires.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hud_hires.h"

#define BASE 0x4000u
#define SIZE ( 512u * 1024u )
static unsigned char fake_aram[SIZE];
static unsigned char arena[8 * 1024] __attribute__(( aligned( 32 )));
static unsigned char sheet[256 * 256];
static unsigned char pal[256 * 4];
static int idles;

static void fake_copy( uint32_t aram, void *buf, uint32_t len, int write )
{
	assert( aram >= BASE && aram + len <= BASE + SIZE );
	if( write )
		memcpy( fake_aram + ( aram - BASE ), buf, len );
	else
		memcpy( buf, fake_aram + ( aram - BASE ), len );
}

static void gpu_idle( void )
{
	idles++;
}

static uint16_t texel( const uint16_t *t, int tw, int x, int y )
{
	return t[(( y >> 2 ) * ( tw >> 2 ) + ( x >> 2 )) * 16 + ( y & 3 ) * 4 + ( x & 3 )];
}

int main( void )
{
	static gc_aram_heap_t heap;
	static gc_aram_cache_t cache;
	static gc_hudhi_t hh;
	const uint16_t *t, *t2;
	uint16_t tw, th;
	int x, y, fresh, h, i;

	/* Palette: index i is (i, 2i, 255-i) opaque, 0 is black, 255 a hole. */
	for( i = 0; i < 256; i++ )
	{
		pal[i * 4 + 0] = (unsigned char)i;
		pal[i * 4 + 1] = (unsigned char)( i * 2 );
		pal[i * 4 + 2] = (unsigned char)( 255 - i );
		pal[i * 4 + 3] = 255;
	}
	pal[0] = pal[1] = pal[2] = 0;
	pal[255 * 4 + 3] = 0;
	for( y = 0; y < 256; y++ )
		for( x = 0; x < 256; x++ )
			sheet[y * 256 + x] = (unsigned char)(( x + y ) % 255 ? ( x + y ) % 255 : 1 );
	/* A hole run at row 10, columns 20..23, between indices 19 and 24. */
	for( x = 20; x < 24; x++ )
		sheet[10 * 256 + x] = 255;
	sheet[10 * 256 + 19] = 100;
	sheet[10 * 256 + 24] = 200;

	assert( GC_HudHiPaletteEntry( 0, 0, 0, 255 ) == 0 );
	assert( GC_HudHiPaletteEntry( 8, 8, 8, 255 ) == 0 );          /* punched near-black */
	assert( GC_HudHiPaletteEntry( 255, 0, 0, 255 ) == 0xfc00 );
	assert( GC_HudHiPaletteEntry( 1, 2, 3, 0 ) == 0x0001 );       /* hole */
	assert( GC_HudHiPaletteEntry( 255, 255, 255, 128 ) == (( 4 << 12 ) | 0x0fff ));

	GC_AramHeapInit( &heap, BASE, SIZE );
	GC_AramCacheInit( &cache, &heap, fake_copy, SIZE );
	GC_HudHiInit( &hh, &cache, gpu_idle );

	h = GC_HudHiRegister( &hh, "#HUD/sprites/clip_clips(frame:00).spr", 256, 256, sheet, pal );
	assert( h >= 0 );
	assert( GC_HudHiRegister( &hh, "#HUD/sprites/clip_clips(frame:00).spr", 256, 256, sheet, pal ) == h );
	GC_HudHiSetTexnum( &hh, h, 77 );
	assert( GC_HudHiFind( &hh, 77, "#HUD/sprites/clip_clips(frame:00).spr" ) == h );
	assert( GC_HudHiFind( &hh, 77, "#HUD/sprites/other(frame:00).spr" ) < 0 );
	assert( GC_HudHiFind( &hh, 78, "#HUD/sprites/clip_clips(frame:00).spr" ) < 0 );

	/* No arena: nothing is built. */
	assert( !GC_HudHiCell( &hh, h, 0, 0, 8, 8, &tw, &th, &fresh ));
	GC_HudHiSetArena( &hh, arena, sizeof( arena ));
	GC_HudHiBeginFrame( &hh );

	/* A 57x48 cell (garand clip): 60x48 texture, texels match the palette,
	 * the padding columns repeat the last one. */
	t = GC_HudHiCell( &hh, h, 0, 183, 57, 48, &tw, &th, &fresh );
	assert( t && fresh && tw == 60 && th == 48 );
	for( y = 0; y < 48; y++ )
		for( x = 0; x < 60; x++ )
		{
			const int sx = x < 57 ? x : 56;
			const unsigned char idx = sheet[( 183 + y ) * 256 + sx];

			assert( texel( t, tw, x, y ) == GC_HudHiPaletteEntry( pal[idx * 4], pal[idx * 4 + 1],
				pal[idx * 4 + 2], pal[idx * 4 + 3] ));
		}
	/* Same cell again: cached. */
	t2 = GC_HudHiCell( &hh, h, 0, 183, 57, 48, &tw, &th, &fresh );
	assert( t2 == t && !fresh && hh.builds == 1 && hh.hits == 1 );

	/* Holes take the nearest opaque colour with alpha 0; rows past the cell
	 * repeat its last row. */
	t = GC_HudHiCell( &hh, h, 16, 10, 16, 1, &tw, &th, &fresh );
	assert( t && fresh && tw == 16 && th == 4 );
	assert( texel( t, tw, 4, 0 ) == 0x06c9 && texel( t, tw, 5, 0 ) == 0x06c9 );
	{
		const uint16_t v200 = GC_HudHiPaletteEntry( 200, 144, 55, 255 ); /* 2*200 wraps to 144 */
		const uint16_t c200 = (uint16_t)(((( v200 >> 11 ) & 15u ) << 8 ) | ((( v200 >> 6 ) & 15u ) << 4 ) | (( v200 >> 1 ) & 15u ));

		assert( texel( t, tw, 6, 0 ) == c200 && texel( t, tw, 7, 0 ) == c200 );
	}
	assert( texel( t, tw, 3, 3 ) == texel( t, tw, 3, 0 ));
	assert( hh.arena_used == 5760 + 128 && idles == 0 );

	/* Next frame, arena full: the GPU is waited on and old cells go. */
	GC_HudHiBeginFrame( &hh );
	t = GC_HudHiCell( &hh, h, 82, 125, 40, 35, &tw, &th, &fresh );
	assert( t && fresh && tw == 40 && th == 36 && idles == 1 && hh.arena_used == 2880 );
	/* The garand cell no longer fits beside it: this frame's cell is packed,
	 * still no room, so everything goes. */
	t = GC_HudHiCell( &hh, h, 0, 183, 57, 48, &tw, &th, &fresh );
	assert( t && fresh && idles == 3 && hh.arena_used == 5760 );
	/* Keep-current compaction moves a cell and keeps its texels. */
	GC_HudHiBeginFrame( &hh );
	t = GC_HudHiCell( &hh, h, 16, 10, 16, 1, &tw, &th, &fresh );   /* off 5760 */
	assert( t && hh.arena_used == 5760 + 128 );
	t = GC_HudHiCell( &hh, h, 0, 0, 40, 30, &tw, &th, &fresh );    /* 40x32: compacts */
	assert( t && idles == 4 && hh.arena_used == 128 + 2560 );
	t = GC_HudHiCell( &hh, h, 16, 10, 16, 1, &tw, &th, &fresh );
	assert( t == (const uint16_t *)arena && !fresh && texel( t, 16, 4, 0 ) == 0x06c9 );

	/* Too big for the arena, outside the sheet, or a bad handle: refused. */
	assert( !GC_HudHiCell( &hh, h, 0, 0, 128, 64, &tw, &th, &fresh ));
	assert( !GC_HudHiCell( &hh, h, 300, 0, 8, 8, &tw, &th, &fresh ));
	assert( !GC_HudHiCell( &hh, 5, 0, 0, 8, 8, &tw, &th, &fresh ));
	/* Clamped to the sheet edge. */
	t = GC_HudHiCell( &hh, h, 250, 250, 20, 20, &tw, &th, &fresh );
	assert( t && tw == 8 && th == 8 );

	/* Re-registering after the table was reset finds the ARAM copy. */
	{
		const uint32_t stores = cache.stores;
		static gc_hudhi_t hh2;

		GC_HudHiInit( &hh2, &cache, gpu_idle );
		assert( GC_HudHiRegister( &hh2, "#HUD/sprites/clip_clips(frame:00).spr", 256, 256, sheet, pal ) >= 0 );
		assert( cache.stores == stores );
	}
	GC_HudHiSetArena( &hh, NULL, 0 );
	assert( !GC_HudHiCell( &hh, h, 0, 0, 8, 8, &tw, &th, &fresh ));
	printf( "hud hires ok\n" );
	return 0;
}
