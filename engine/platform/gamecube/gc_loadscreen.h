/*
gc_loadscreen.h - full-screen loading screens ("GCMP" + bar layout), platform independent
Copyright (C) 2026 xash3d-gc contributors

A game can bring its own loading screen as resource/gc_loading/<map>.cmp, with
resource/gc_loading/default.cmp for maps without one (MOHAA's converter writes
these from the retail ui/loading_*.urc menus). The file is a GCMP image (see
gcmp.h), 640 wide and 480 + fill rows high:

  rows   0..479   the 640x480 screen with the empty progress bar already drawn in
  rows 480..      the bar's fill texture, at x 0, fill_w x fill_h (multiples of 8)

The bar layout sits in the 12 reserved GCMP header bytes (big-endian):

  20 u16 bar x, 22 u16 bar y, 24 u16 bar w, 26 u16 bar h   fill rect, 640x480 units
  28 u8  plate tile x, 29 u8 plate tile y                   8x8 tiles of the screen
  30 u8  plate tiles wide, 31 u8 plate tiles high           that hold the bar art

The engine draws the screen once, then while the map loads it redraws only the
plate (the screen tiles under the bar) and the fill up to the load fraction.
Plate and fill stay resident and must fit GC_LOADSCREEN_RESIDENT_MAX bytes.
Header only, no libc beyond stdint/stddef, so it can be tested on the host.
*/
#ifndef GC_LOADSCREEN_H_INCLUDED
#define GC_LOADSCREEN_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include "gcmp.h"

#define GC_LOADSCREEN_W            640u
#define GC_LOADSCREEN_H            480u
#define GC_LOADSCREEN_TILES_X      ( GC_LOADSCREEN_W / 8u )
#define GC_LOADSCREEN_TILES_Y      ( GC_LOADSCREEN_H / 8u )
#define GC_LOADSCREEN_SCREEN_BYTES ( GC_LOADSCREEN_TILES_X * GC_LOADSCREEN_TILES_Y * 32u )
#define GC_LOADSCREEN_RESIDENT_MAX 4096u

typedef struct
{
	uint16_t bar_x, bar_y, bar_w, bar_h;      /* fill rect in 640x480 */
	uint8_t  plate_tx, plate_ty, plate_tw, plate_th; /* tiles of the screen under the bar */
	uint16_t fill_w, fill_h;                   /* fill texture size */
	uint32_t plate_bytes, fill_bytes;          /* resident CMPR bytes of each */
} gc_loadscreen_t;

/* 1 and *out filled if head (a GCMP header, len bytes) is a usable loading screen. */
static inline int GC_LoadScreenParse( const uint8_t *head, size_t len, gc_loadscreen_t *out )
{
	gc_gcmp_header_t h;

	if( !GC_GcmpParseHeader( head, len, &h ) || h.w != GC_LOADSCREEN_W || h.h <= GC_LOADSCREEN_H
		|| h.h - GC_LOADSCREEN_H > 64u )
		return 0;
	out->bar_x = GC_GcmpBE16( head + 20 );
	out->bar_y = GC_GcmpBE16( head + 22 );
	out->bar_w = GC_GcmpBE16( head + 24 );
	out->bar_h = GC_GcmpBE16( head + 26 );
	out->plate_tx = head[28];
	out->plate_ty = head[29];
	out->plate_tw = head[30];
	out->plate_th = head[31];
	out->fill_w = (uint16_t)(( out->bar_w + 7u ) & ~7u );
	out->fill_h = (uint16_t)( h.h - GC_LOADSCREEN_H );
	out->plate_bytes = (uint32_t)out->plate_tw * out->plate_th * 32u;
	out->fill_bytes = (uint32_t)( out->fill_w / 8u ) * ( out->fill_h / 8u ) * 32u;
	if( !out->bar_w || !out->bar_h || out->bar_h > out->fill_h
		|| (uint32_t)out->bar_x + out->bar_w > GC_LOADSCREEN_W
		|| (uint32_t)out->bar_y + out->bar_h > GC_LOADSCREEN_H )
		return 0;
	if( !out->plate_tw || !out->plate_th
		|| (uint32_t)out->plate_tx + out->plate_tw > GC_LOADSCREEN_TILES_X
		|| (uint32_t)out->plate_ty + out->plate_th > GC_LOADSCREEN_TILES_Y )
		return 0;
	/* the plate must cover the bar, so redrawing it wipes the last fill */
	if( out->plate_tx * 8u > out->bar_x || out->plate_ty * 8u > out->bar_y
		|| ( out->plate_tx + out->plate_tw ) * 8u < (uint32_t)out->bar_x + out->bar_w
		|| ( out->plate_ty + out->plate_th ) * 8u < (uint32_t)out->bar_y + out->bar_h )
		return 0;
	if( out->plate_bytes + out->fill_bytes > GC_LOADSCREEN_RESIDENT_MAX )
		return 0;
	return 1;
}

/* Copy the plate's tiles out of the 640x480 screen (GC_LOADSCREEN_SCREEN_BYTES of
 * CMPR tiles) into dst as a plate_tw*8 x plate_th*8 CMPR texture. */
static inline void GC_LoadScreenCutPlate( const gc_loadscreen_t *ls, const uint8_t *screen, uint8_t *dst )
{
	unsigned r, i;
	const unsigned row = ls->plate_tw * 32u;

	for( r = 0; r < ls->plate_th; r++ )
	{
		const uint8_t *src = screen + (( ls->plate_ty + r ) * GC_LOADSCREEN_TILES_X + ls->plate_tx ) * 32u;

		for( i = 0; i < row; i++ )
			dst[r * row + i] = src[i];
	}
}

#endif /* GC_LOADSCREEN_H_INCLUDED */
