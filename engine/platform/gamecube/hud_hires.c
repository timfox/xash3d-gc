/*
hud_hires.c - full-resolution HUD sprite cells from ARAM (platform independent)
Copyright (C) 2026 xash3d-gc contributors
*/
#include <stdio.h>
#include <string.h>
#include "hud_hires.h"

#define HH_ALIGN 32u
/* A palette hole (alpha 0 in the sprite palette, index 255 of alphatest
 * sheets): transparent like 0, but its colour is replaced by the nearest
 * opaque texel's so bilinear filtering does not darken the cell's edges.
 * Near-black punched out of opaque art stays 0 and keeps its black. */
#define HH_HOLE  0x0001u

static uint32_t HH_Align( uint32_t v )
{
	return ( v + HH_ALIGN - 1u ) & ~( HH_ALIGN - 1u );
}

void GC_HudHiInit( gc_hudhi_t *s, gc_aram_cache_t *cache, void (*gpu_idle)( void ))
{
	memset( s, 0, sizeof( *s ));
	s->cache = cache;
	s->gpu_idle = gpu_idle;
	s->pal_sheet = -1;
}

uint16_t GC_HudHiPaletteEntry( uint8_t r, uint8_t g, uint8_t b, uint8_t a )
{
	const unsigned r5 = r >> 3, g5 = g >> 3, b5 = b >> 3;
	unsigned a3;

	if( a == 0 )
		return HH_HOLE;
	a3 = a >> 5;
	if( a == 255 )
	{
		if( r5 <= 1u && g5 <= 1u && b5 <= 1u )
			return 0;
		return (uint16_t)( 0x8000u | ( r5 << 10 ) | ( g5 << 5 ) | b5 );
	}
	if( a3 == 0 )
		return 0;
	if( a3 >= 7 )
		return (uint16_t)( 0x8000u | ( r5 << 10 ) | ( g5 << 5 ) | b5 );
	return (uint16_t)(( a3 << 12 ) | (( r >> 4 ) << 8 ) | (( g >> 4 ) << 4 ) | ( b >> 4 ));
}

uint16_t GC_HudHiPaletteEntryIA8( uint8_t i, uint8_t a )
{
	if( a == 0 )
		return HH_HOLE;
	if( a == 255 && ( i >> 3 ) <= 1u )
		return 0;
	return (uint16_t)(( a << 8 ) | i );
}

static void HH_Key( char *out, size_t size, const char *name, uint16_t w, uint16_t h, int ia8 )
{
	uint32_t hash = 2166136261u;
	const unsigned char *p = (const unsigned char *)name;

	while( *p )
	{
		hash ^= *p++;
		hash *= 16777619u;
	}
	snprintf( out, size, "hudhi:%08x:%ux%u%s", (unsigned)hash, (unsigned)w, (unsigned)h, ia8 ? ":ia" : "" );
}

int GC_HudHiRegister( gc_hudhi_t *s, const char *name, uint16_t w, uint16_t h,
	const uint8_t *indices, const uint8_t *rgba_palette )
{
	char key[GC_ARAM_NAME_LEN];
	uint16_t table[256];
	uint8_t present[256];
	int i, slot = -1, index, ia8 = 1;
	const uint32_t idx_bytes = HH_Align( (uint32_t)w * h );

	if( !s->cache || !name || !name[0] || !indices || !rgba_palette
		|| w < 1 || h < 1 || w > GC_HUDHI_MAX_DIM || h > GC_HUDHI_MAX_DIM
		|| strlen( name ) >= GC_HUDHI_NAME_LEN )
		return -1;
	for( i = 0; i < GC_HUDHI_SHEETS; i++ )
	{
		gc_hudhi_sheet_t *sh = &s->sheets[i];

		if( sh->used && !strcmp( sh->name, name ) && sh->w == w && sh->h == h )
			return i;
		if( !sh->used && slot < 0 )
			slot = i;
	}
	if( slot < 0 )
	{
		s->refusals++;
		return -1;
	}

	/* Grey when every colour the sheet uses has r == g == b. */
	memset( present, 0, sizeof( present ));
	for( i = 0; i < (int)w * h; i++ )
		present[indices[i]] = 1;
	for( i = 0; i < 256 && ia8; i++ )
		if( present[i] && rgba_palette[i * 4 + 3] != 0
			&& ( rgba_palette[i * 4 + 0] != rgba_palette[i * 4 + 1]
			|| rgba_palette[i * 4 + 0] != rgba_palette[i * 4 + 2] ))
			ia8 = 0;

	HH_Key( key, sizeof( key ), name, w, h, ia8 );
	index = GC_AramCacheFind( s->cache, key );
	if( index < 0 )
	{
		for( i = 0; i < 256; i++ )
			table[i] = ia8 ? GC_HudHiPaletteEntryIA8( rgba_palette[i * 4 + 0], rgba_palette[i * 4 + 3] )
				: GC_HudHiPaletteEntry( rgba_palette[i * 4 + 0], rgba_palette[i * 4 + 1],
				rgba_palette[i * 4 + 2], rgba_palette[i * 4 + 3] );
		index = GC_AramCacheBegin( s->cache, key, idx_bytes + sizeof( table ));
		if( index < 0 )
		{
			s->refusals++;
			return -1;
		}
		if( !GC_AramCacheWrite( s->cache, index, 0, indices, (uint32_t)w * h )
			|| !GC_AramCacheWrite( s->cache, index, idx_bytes, table, sizeof( table ))
			|| !GC_AramCacheCommit( s->cache, index ))
		{
			GC_AramCacheAbort( s->cache, index );
			s->refusals++;
			return -1;
		}
	}
	GC_AramCachePin( s->cache, index );

	memset( &s->sheets[slot], 0, sizeof( s->sheets[slot] ));
	strcpy( s->sheets[slot].name, name );
	s->sheets[slot].w = w;
	s->sheets[slot].h = h;
	s->sheets[slot].aram_index = index;
	s->sheets[slot].ia8 = (uint8_t)ia8;
	s->sheets[slot].used = 1;
	return slot;
}

int GC_HudHiSheetIA8( const gc_hudhi_t *s, int handle )
{
	return handle >= 0 && handle < GC_HUDHI_SHEETS && s->sheets[handle].used && s->sheets[handle].ia8;
}

void GC_HudHiSetTexnum( gc_hudhi_t *s, int handle, int texnum )
{
	if( handle >= 0 && handle < GC_HUDHI_SHEETS && s->sheets[handle].used )
		s->sheets[handle].texnum = texnum;
}

int GC_HudHiFind( const gc_hudhi_t *s, int texnum, const char *name )
{
	int i;

	if( texnum <= 0 )
		return -1;
	for( i = 0; i < GC_HUDHI_SHEETS; i++ )
	{
		const gc_hudhi_sheet_t *sh = &s->sheets[i];

		if( sh->used && sh->texnum == texnum )
			return ( name && !strcmp( sh->name, name )) ? i : -1;
	}
	return -1;
}

void GC_HudHiSheetSize( const gc_hudhi_t *s, int handle, uint16_t *w, uint16_t *h )
{
	*w = *h = 0;
	if( handle >= 0 && handle < GC_HUDHI_SHEETS && s->sheets[handle].used )
	{
		*w = s->sheets[handle].w;
		*h = s->sheets[handle].h;
	}
}

void GC_HudHiSetArena( gc_hudhi_t *s, void *arena, uint32_t bytes )
{
	memset( s->cells, 0, sizeof( s->cells ));
	s->arena = (uint8_t *)arena;
	s->arena_bytes = arena ? bytes & ~( HH_ALIGN - 1u ) : 0;
	s->arena_used = 0;
}

void GC_HudHiBeginFrame( gc_hudhi_t *s )
{
	s->frame++;
}

/* Arena full: once the GPU is idle nothing queued samples the arena, so keep
 * this frame's cells (packed to the front, in order) and drop the rest. */
static void HH_Compact( gc_hudhi_t *s, int keep_current )
{
	int order[GC_HUDHI_CELLS];
	uint32_t cursor = 0;
	int i, j, n = 0;

	if( s->gpu_idle )
		s->gpu_idle();
	s->compactions++;
	for( i = 0; i < GC_HUDHI_CELLS; i++ )
	{
		if( !s->cells[i].used )
			continue;
		/* Insertion sort by arena offset. */
		for( j = n; j > 0 && s->cells[order[j - 1]].off > s->cells[i].off; j-- )
			order[j] = order[j - 1];
		order[j] = i;
		n++;
	}
	for( i = 0; i < n; i++ )
	{
		gc_hudhi_cell_t *c = &s->cells[order[i]];

		if( keep_current && c->last_frame == s->frame )
		{
			if( c->off != cursor )
				memmove( s->arena + cursor, s->arena + c->off, c->bytes );
			c->off = cursor;
			cursor += c->bytes;
		}
		else
			c->used = 0;
	}
	s->arena_used = cursor;
}

static int HH_Alloc( gc_hudhi_t *s, uint32_t bytes, uint32_t *off )
{
	if( bytes > s->arena_bytes )
		return 0;
	if( s->arena_used + bytes > s->arena_bytes )
		HH_Compact( s, 1 );
	if( s->arena_used + bytes > s->arena_bytes )
		HH_Compact( s, 0 );
	*off = s->arena_used;
	s->arena_used += bytes;
	return 1;
}

static uint16_t HH_Rgb12( uint16_t v )
{
	if( v & 0x8000u )
		return (uint16_t)(((( v >> 11 ) & 15u ) << 8 ) | ((( v >> 6 ) & 15u ) << 4 ) | (( v >> 1 ) & 15u ));
	return (uint16_t)( v & 0x0fffu );
}

/* Holes take the colour of the nearest texel that is not a hole, alpha 0. */
static uint16_t HH_Clear( uint16_t v, int ia8 )
{
	return ia8 ? (uint16_t)( v & 0x00ffu ) : HH_Rgb12( v );
}

static void HH_CarryHoles( uint16_t *line, int n, int ia8 )
{
	int dist[GC_HUDHI_MAX_DIM];
	uint16_t col[GC_HUDHI_MAX_DIM];
	int i, d = -1;
	uint16_t c = HH_HOLE;

	for( i = 0; i < n; i++ )
	{
		if( line[i] != HH_HOLE )
		{
			c = HH_Clear( line[i], ia8 );
			d = 0;
		}
		else if( d >= 0 )
			d++;
		dist[i] = d;
		col[i] = c;
	}
	d = -1;
	for( i = n - 1; i >= 0; i-- )
	{
		if( line[i] != HH_HOLE )
		{
			c = HH_Clear( line[i], ia8 );
			d = 0;
			continue;
		}
		if( d >= 0 )
			d++;
		if( d >= 0 && ( dist[i] < 0 || d < dist[i] ))
			col[i] = c;
		line[i] = dist[i] < 0 && d < 0 ? (uint16_t)HH_HOLE : col[i];
	}
}

const uint16_t *GC_HudHiCell( gc_hudhi_t *s, int handle, int x, int y, int w, int h,
	uint16_t *tw, uint16_t *th, int *fresh )
{
	const gc_hudhi_sheet_t *sh;
	uint8_t row[GC_HUDHI_MAX_DIM];
	uint16_t line[GC_HUDHI_MAX_DIM];
	gc_hudhi_cell_t *cell;
	uint16_t *dst;
	uint32_t bytes, off;
	int i, r, c, slot = -1, oldest = -1, tws, ths;

	*fresh = 0;
	*tw = *th = 0;
	if( handle < 0 || handle >= GC_HUDHI_SHEETS || !s->sheets[handle].used || !s->arena )
		return NULL;
	sh = &s->sheets[handle];
	if( x < 0 ) { w += x; x = 0; }
	if( y < 0 ) { h += y; y = 0; }
	if( x + w > sh->w ) w = sh->w - x;
	if( y + h > sh->h ) h = sh->h - y;
	if( w < 1 || h < 1 )
		return NULL;

	for( i = 0; i < GC_HUDHI_CELLS; i++ )
	{
		cell = &s->cells[i];
		if( !cell->used )
		{
			if( slot < 0 )
				slot = i;
			continue;
		}
		if( cell->sheet == handle && cell->x == x && cell->y == y && cell->w == w && cell->h == h )
		{
			cell->last_frame = s->frame;
			s->hits++;
			*tw = cell->tw;
			*th = cell->th;
			return (const uint16_t *)( s->arena + cell->off );
		}
		if( cell->last_frame != s->frame && ( oldest < 0 || cell->last_frame < s->cells[oldest].last_frame ))
			oldest = i;
	}

	tws = ( w + 3 ) & ~3;
	ths = ( h + 3 ) & ~3;
	bytes = (uint32_t)tws * (uint32_t)ths * 2u;
	if( slot < 0 )
	{
		/* Table full: reuse the oldest cell not drawn this frame. Its bytes
		 * stay allocated until the next compaction. */
		if( oldest < 0 )
		{
			s->refusals++;
			return NULL;
		}
		s->cells[oldest].used = 0;
		slot = oldest;
	}
	if( !HH_Alloc( s, bytes, &off ))
	{
		s->refusals++;
		return NULL;
	}

	if( s->pal_sheet != handle )
	{
		if( !GC_AramCacheRead( s->cache, sh->aram_index, HH_Align( (uint32_t)sh->w * sh->h ),
			s->pal, sizeof( s->pal )))
		{
			s->pal_sheet = -1;
			s->arena_used = off;
			s->refusals++;
			return NULL;
		}
		s->pal_sheet = handle;
	}

	dst = (uint16_t *)( s->arena + off );
	for( r = 0; r < ths; r++ )
	{
		/* Rows and columns past the cell repeat its edge, so filtering at
		 * the border samples the cell itself. */
		if( r < h )
		{
			if( !GC_AramCacheRead( s->cache, sh->aram_index,
				(uint32_t)( y + r ) * sh->w + (uint32_t)x, row, (uint32_t)w ))
			{
				s->arena_used = off;
				s->refusals++;
				return NULL;
			}
			for( c = 0; c < w; c++ )
				line[c] = s->pal[row[c]];
			HH_CarryHoles( line, w, sh->ia8 );
			for( c = w; c < tws; c++ )
				line[c] = line[w - 1];
		}
		for( c = 0; c < tws; c++ )
			dst[(( r >> 2 ) * ( tws >> 2 ) + ( c >> 2 )) * 16 + ( r & 3 ) * 4 + ( c & 3 )] = line[c];
	}

	cell = &s->cells[slot];
	cell->sheet = handle;
	cell->x = (uint16_t)x;
	cell->y = (uint16_t)y;
	cell->w = (uint16_t)w;
	cell->h = (uint16_t)h;
	cell->tw = (uint16_t)tws;
	cell->th = (uint16_t)ths;
	cell->off = off;
	cell->bytes = bytes;
	cell->last_frame = s->frame;
	cell->used = 1;
	s->builds++;
	*fresh = 1;
	*tw = cell->tw;
	*th = cell->th;
	return dst;
}
