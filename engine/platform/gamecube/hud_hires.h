/*
hud_hires.h - full-resolution HUD sprite cells from ARAM (platform independent)
Copyright (C) 2026 xash3d-gc contributors

Big HUD sprite sheets (DoD's 256x256 hud_layout and clip sheets) do not fit
MEM1 at full resolution, so the engine uploads a shrunk copy. This module keeps
each sheet's full-resolution palette indices in the ARAM asset cache (pinned,
so later maps find them again without a disc read) and builds, on demand, a
small GX-tiled RGB5A3 texture of just the cell a HUD draw asks for. Only the
cells drawn recently live in MEM1, in an arena the caller provides.

A cell built this frame stays where it is until the next GC_HudHiBeginFrame;
when the arena is full the caller's gpu_idle hook runs before anything older
is moved or overwritten, because queued draws may still sample it.
No libogc or engine dependencies: tested on the host (test_gamecube_hud_hires.py).
*/
#ifndef HUD_HIRES_H
#define HUD_HIRES_H

#include "aram_core.h"

#define GC_HUDHI_SHEETS    32
#define GC_HUDHI_CELLS     128
#define GC_HUDHI_NAME_LEN  80
#define GC_HUDHI_MAX_DIM   512

typedef struct
{
	char     name[GC_HUDHI_NAME_LEN]; /* texture name, e.g. #HUD/sprites/x(frame:00).spr */
	uint16_t w, h;
	int      texnum;                  /* renderer texture of the shrunk upload, 0 if none */
	int      aram_index;              /* indexed pixels+table or direct RGBA pixels */
	uint8_t  used;
	uint8_t  ia8;                     /* grey sheet: IA8 texels (8-bit intensity and alpha) */
	uint8_t  rgba;                    /* direct 32-bit RGBA sheet */
} gc_hudhi_sheet_t;

typedef struct
{
	int      sheet;
	uint16_t x, y, w, h;              /* source rectangle */
	uint16_t tw, th;                  /* texture size (multiples of 4) */
	uint32_t off, bytes;              /* in the arena */
	uint32_t last_frame;
	uint8_t  used;
} gc_hudhi_cell_t;

typedef struct
{
	gc_aram_cache_t *cache;
	void   (*gpu_idle)( void );       /* waits until queued GPU draws are done */
	uint8_t *arena;
	uint32_t arena_bytes, arena_used;
	uint32_t frame;
	int      pal_sheet;               /* sheet whose table is in pal, -1 none */
	uint16_t pal[256];
	gc_hudhi_sheet_t sheets[GC_HUDHI_SHEETS];
	gc_hudhi_cell_t  cells[GC_HUDHI_CELLS];
	uint32_t builds, hits, compactions, refusals;
} gc_hudhi_t;

void GC_HudHiInit( gc_hudhi_t *s, gc_aram_cache_t *cache, void (*gpu_idle)( void ));

/* RGBA palette entry -> the HUD's RGB5A3 rules: alpha 0 is a hole, alpha 255
 * is opaque with near-black punched out (TransAlpha / additive HUD art), and
 * anything between uses RGB5A3's 3-bit alpha mode. */
uint16_t GC_HudHiPaletteEntry( uint8_t r, uint8_t g, uint8_t b, uint8_t a );
/* Same rules for a grey entry as an IA8 texel (alpha high byte, intensity
 * low): the HUD sheets are grey (the HUD colour is the draw's tint), so
 * their ramps keep all 256 levels instead of RGB5A3's 32. */
uint16_t GC_HudHiPaletteEntryIA8( uint8_t i, uint8_t a );
/* Nonzero when the sheet's cells are IA8 rather than RGB5A3. */
int  GC_HudHiSheetIA8( const gc_hudhi_t *s, int handle );

/* Store a sheet's full-resolution indices (w*h bytes) and its RGBA palette
 * (256 * 4 bytes). Registering a name again reuses its ARAM copy. Returns a
 * handle, or -1 when ARAM or the table is full. */
int  GC_HudHiRegister( gc_hudhi_t *s, const char *name, uint16_t w, uint16_t h,
	const uint8_t *indices, const uint8_t *rgba_palette );
/* Store a direct RGBA sprite sheet in ARAM for full-resolution HUD cells. */
int  GC_HudHiRegisterRGBA( gc_hudhi_t *s, const char *name, uint16_t w, uint16_t h,
	const uint8_t *rgba_pixels );
void GC_HudHiSetTexnum( gc_hudhi_t *s, int handle, int texnum );
/* Sheet for a renderer texture: texnum and name must both match. -1 if none. */
int  GC_HudHiFind( const gc_hudhi_t *s, int texnum, const char *name );
void GC_HudHiSheetSize( const gc_hudhi_t *s, int handle, uint16_t *w, uint16_t *h );

/* Give the cell arena (NULL to take it back; every cell is dropped). */
void GC_HudHiSetArena( gc_hudhi_t *s, void *arena, uint32_t bytes );
void GC_HudHiBeginFrame( gc_hudhi_t *s );

/* GX-tiled RGB5A3 texels of the source rectangle (clamped to the sheet), or
 * NULL when it cannot be built. tw, th receive the texture size; the cell's
 * texels start at its top-left corner. *fresh is set when the texels were
 * just written (the caller flushes the data cache and invalidates the GX
 * texture cache). */
const uint16_t *GC_HudHiCell( gc_hudhi_t *s, int handle, int x, int y, int w, int h,
	uint16_t *tw, uint16_t *th, int *fresh );

#endif /* HUD_HIRES_H */
