/*
sv_lazy_logic.c - deferred spawn of map logic entities (platform independent)
Copyright (C) 2026 xash3d-gc contributors
*/
#include <string.h>
#include "sv_lazy_logic.h"

typedef struct
{
	uint32_t hash, ofs, len;
	uint8_t  spawned;
} lazy_entry_t;

static lazy_entry_t lazy_tab[LAZY_LOGIC_MAX];
static int lazy_count, lazy_pending, lazy_spawned;

uint32_t LazyLogic_Hash( const char *name )
{
	uint32_t h = 2166136261u;	/* FNV-1a */

	while( name && *name )
		h = ( h ^ (uint8_t)*name++ ) * 16777619u;
	return h ? h : 1u;
}

void LazyLogic_Reset( void )
{
	memset( lazy_tab, 0, sizeof( lazy_tab ));
	lazy_count = lazy_pending = lazy_spawned = 0;
}

int LazyLogic_Add( uint32_t name_hash, uint32_t ofs, uint32_t len )
{
	if( lazy_count >= LAZY_LOGIC_MAX || !name_hash || !len )
		return -1;
	lazy_tab[lazy_count].hash = name_hash;
	lazy_tab[lazy_count].ofs = ofs;
	lazy_tab[lazy_count].len = len;
	lazy_tab[lazy_count].spawned = 0;
	lazy_pending++;
	return lazy_count++;
}

int LazyLogic_Pending( void ) { return lazy_pending; }
int LazyLogic_Total( void ) { return lazy_count; }
int LazyLogic_Spawned( void ) { return lazy_spawned; }

int LazyLogic_Materialize( uint32_t name_hash, lazy_spawn_fn spawn, void *ctx )
{
	int i, n = 0;

	if( !lazy_pending || !name_hash || !spawn )
		return 0;
	for( i = 0; i < lazy_count; i++ )
	{
		lazy_entry_t *e = &lazy_tab[i];
		int r;

		if( e->spawned || e->hash != name_hash )
			continue;
		e->spawned = 1;			/* before the callback: a lookup made while it spawns must not re-enter */
		lazy_pending--;
		r = spawn( ctx, e->ofs, e->len );
		if( r > 0 )
		{
			lazy_spawned++;
			n++;
		}
		else
		{
			e->spawned = 0;		/* collision or no room right now: stays pending */
			lazy_pending++;
		}
	}
	return n;
}
