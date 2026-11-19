/*
mem_gamecube.c - GameCube main-memory telemetry and map-load staging
Copyright (C) 2026 xash3d-gc contributors
*/
#include "common.h"
#include "client.h"

#if XASH_GAMECUBE
#include "mem_gamecube.h"
#include "aram_core.h"
#include <stdlib.h>
#include <malloc.h>
#include <reent.h>
#include <ogc/system.h>
#include <ogc/aram.h>
#include <ogc/cache.h>

void *R_GCBorrowMapLoadStaticArena( size_t size, size_t *capacity );
qboolean R_GCReleaseMapLoadStaticArena( void *ptr );
qboolean SV_Active( void );
qboolean SV_GCPrivateOverflowReserved( void );
qboolean SV_GCPrivateOverflowPinsMapArena( void );
void GCube_AppendSwissEvidence( const char *fmt, ... );

static char gc_mem_map[MAX_QPATH] = "(none)";
static size_t gc_mem_last;
static size_t gc_mem_hwm;

/* Map-load memory pressure tracking */
static size_t gc_mapload_pressure_base;
static size_t gc_mapload_pressure_peak;
static size_t gc_mapload_pressure_delta;
static qboolean gc_mapload_pressure_active;

/* Contiguous staging buffer for maps/*.bsp. Borrowed after menu/client trim. */
static byte *gc_mapload_buf;
static size_t gc_mapload_buf_size;
static size_t gc_mapload_tail_highwater;
/* Highest end of any tail carve since this buffer was acquired. The
 * highwater is lowered again during a lean map load while earlier carves
 * (the world-texture low mips) stay live, so late users start above this. */
static size_t gc_mapload_tail_peak;
static qboolean gc_mapload_buf_in_use;
static int gc_mapload_memopt_depth;
static qboolean gc_mapload_memopt_session; /* stays on after playstart until cleared */
static qboolean gc_newgame_bootstrap_memopt = true;

#define GC_JOB_MAX 64
typedef struct { GC_JobFn fn; void *opaque; GC_JobPriority priority; } gc_job_t;
static gc_job_t gc_jobs[GC_JOB_MAX];
static unsigned gc_job_count;

qboolean GC_JobEnqueue( GC_JobFn fn, void *opaque, GC_JobPriority priority )
{
	unsigned i;
	if( !fn || gc_job_count >= GC_JOB_MAX ) return false;
	for( i = gc_job_count; i > 0 && gc_jobs[i - 1].priority > priority; i-- )
		gc_jobs[i] = gc_jobs[i - 1];
	gc_jobs[i].fn = fn;
	gc_jobs[i].opaque = opaque;
	gc_jobs[i].priority = priority;
	gc_job_count++;
	return true;
}

unsigned GC_JobPending( void ) { return gc_job_count; }

unsigned GC_JobRunBudget( unsigned budget_us )
{
	const double start = Sys_DoubleTime( );
	unsigned ran = 0;
	while( gc_job_count )
	{
		gc_job_t job = gc_jobs[0];
		memmove( gc_jobs, gc_jobs + 1, ( gc_job_count - 1 ) * sizeof( gc_jobs[0] ));
		gc_job_count--;
		job.fn( job.opaque );
		ran++;
		if( budget_us && ( Sys_DoubleTime( ) - start ) * 1000000.0 >= budget_us ) break;
	}
	return ran;
}

#define GC_BSP_BATCH_FACE_MAX 16384
static unsigned short gc_bsp_face_batch[GC_BSP_BATCH_FACE_MAX];
static int gc_bsp_batch_faces;
static unsigned int gc_bsp_batch_count;
static qboolean gc_bsp_batches_valid;
static int gc_bsp_lump_offset[15], gc_bsp_lump_size[15];
static qboolean gc_bsp_lump_hot[15];
static unsigned int gc_bsp_lump_aram[15];
static unsigned int gc_bsp_hot_aram, gc_bsp_hot_bytes;

unsigned int GC_MapFaceBatchKey( int face_index )
{
	if( !gc_bsp_batches_valid || face_index < 0 || face_index >= gc_bsp_batch_faces )
		return 0;
	return gc_bsp_face_batch[face_index];
}

void GC_MapBatchStats( unsigned int *batches, unsigned int *faces )
{
	if( batches ) *batches = gc_bsp_batches_valid ? gc_bsp_batch_count : 0;
	if( faces ) *faces = gc_bsp_batches_valid ? (unsigned int)gc_bsp_batch_faces : 0;
}

#define GC_PREFETCH_MAX_ENTRIES 32
#define GC_PREFETCH_MAX_BYTES (2u * 1024u * 1024u)
#define GC_PREFETCH_CHUNK 32768u
typedef struct { char path[MAX_QPATH]; unsigned int size, aram; qboolean ready; } gc_prefetch_entry_t;
static gc_prefetch_entry_t gc_prefetch[GC_PREFETCH_MAX_ENTRIES];
static unsigned int gc_prefetch_count, gc_prefetch_next, gc_prefetch_used, gc_prefetch_aram;
static unsigned int gc_prefetch_capacity;
static qboolean gc_prefetch_manifest_ready, gc_prefetch_initialized;
static byte gc_prefetch_bounce[GC_PREFETCH_CHUNK] __attribute__(( aligned( 32 )));

void GC_MemSetMap( const char *mapname )
{
	if( mapname && mapname[0] )
		Q_strncpy( gc_mem_map, mapname, sizeof( gc_mem_map ));
	else Q_strncpy( gc_mem_map, "(none)", sizeof( gc_mem_map ));
}

/* Zone pools only count Mem_Alloc traffic.  Direct malloc users (HLSDK DLLs,
 * XFBs, BSP pins, sprite/HUD sys-malloc) share the same newlib heap, so report
 * the real heap too: used/free inside the sbrk'd arena and what is still left
 * between the arena top and MEM1 end. */
static void GC_MemHeapStats( size_t *used, size_t *free_in_heap, size_t *unclaimed )
{
	struct mallinfo mi = mallinfo();
	uintptr_t lo = (uintptr_t)SYS_GetArena1Lo();
	uintptr_t hi = (uintptr_t)SYS_GetArena1Hi();

	*used = (size_t)mi.uordblks;
	*free_in_heap = (size_t)mi.fordblks;
	*unclaimed = hi > lo ? (size_t)( hi - lo ) : 0;
}

/*
 * Heap consistency check (newlib dlmalloc 2.6 layout). Walks every bin's
 * bk/fd chain and the contiguous chunk list from __malloc_sbrk_base to top,
 * and logs the first bad pointer or size. Used to bisect heap corruption:
 * the dod_charlie menu route faults in _malloc_r (DAR 0xC = NULL bin link)
 * on the first world DL frame.
 */
typedef struct gc_mchunk_s
{
	size_t prev_size;
	size_t size;
	struct gc_mchunk_s *fd;
	struct gc_mchunk_s *bk;
} gc_mchunk_t;

extern gc_mchunk_t *__malloc_av_[];
extern char *__malloc_sbrk_base __attribute__(( weak ));
void __malloc_lock( struct _reent *r );
void __malloc_unlock( struct _reent *r );

#define GC_HC_NAV	128
#define GC_HC_BIN( i )	((gc_mchunk_t *)((char *)&__malloc_av_[2 * ( i ) + 2] - 2 * sizeof( size_t )))

static qboolean GC_HeapCheckPtr( uintptr_t p, uintptr_t lo, uintptr_t hi )
{
	return p >= lo && p < hi && ( p & 7u ) == 0;
}

qboolean GC_HeapCheck( const char *tag )
{
	static int failures;
	gc_mchunk_t *top;
	uintptr_t lo, hi, p;
	int i, steps, chunks = 0;
	qboolean ok = true;
	char why[160];

	if( failures >= 8 )
		return false;
	why[0] = '\0';
	__malloc_lock( _REENT );
	top = GC_HC_BIN( 0 )->fd;
	lo = ( &__malloc_sbrk_base && __malloc_sbrk_base != (char *)-1 )
		? ( (uintptr_t)__malloc_sbrk_base + 7u ) & ~(uintptr_t)7u : 0x80000000u;
	hi = (uintptr_t)top + ( top ? ( top->size & ~(size_t)3 ) : 0 );
	if( !top || (uintptr_t)top < 0x80000000u || (uintptr_t)top >= 0x81800000u )
	{
		Q_snprintf( why, sizeof( why ), "top=%p outside MEM1", (void *)top );
		ok = false;
	}
	/* Bins 1..127: every link must be the bin header or a heap chunk. */
	for( i = 1; ok && i < GC_HC_NAV; i++ )
	{
		gc_mchunk_t *b = GC_HC_BIN( i );
		gc_mchunk_t *c = b->bk, *prev = b;

		for( steps = 0; c != b; steps++ )
		{
			if( steps > 8192 || !GC_HeapCheckPtr( (uintptr_t)c, lo, hi ))
			{
				Q_snprintf( why, sizeof( why ), "bin %d bk link %p after %p (step %d, heap %p..%p)",
					i, (void *)c, (void *)prev, steps, (void *)lo, (void *)hi );
				ok = false;
				break;
			}
			if( c->fd != prev )
			{
				Q_snprintf( why, sizeof( why ), "bin %d chunk %p fd=%p expected %p size=0x%lx",
					i, (void *)c, (void *)c->fd, (void *)prev, (unsigned long)c->size );
				ok = false;
				break;
			}
			prev = c;
			c = c->bk;
		}
	}
	/* Contiguous chunk walk up to top (stops at a foreign-sbrk fencepost). */
	for( p = lo; ok && lo != 0x80000000u && p < (uintptr_t)top; chunks++ )
	{
		size_t sz = ((gc_mchunk_t *)p)->size & ~(size_t)3;

		if( sz == 0 || ( sz & 7u ) || p + sz > (uintptr_t)top )
		{
			if( sz <= 2 * sizeof( size_t ))
				break; /* fencepost before a non-contiguous region */
			Q_snprintf( why, sizeof( why ), "chunk %p size=0x%lx (raw 0x%lx) past top %p after %d chunks",
				(void *)p, (unsigned long)sz, (unsigned long)((gc_mchunk_t *)p)->size, (void *)top, chunks );
			ok = false;
			break;
		}
		p += sz;
	}
	__malloc_unlock( _REENT );
	if( ok )
	{
		/* ~10 of these a frame: the first 40 and every 5000th are enough, and keep a long run under the host log cap. */
		static unsigned ok_logs;

		if( ok_logs++ < 40 || !( ok_logs % 5000 ))
			Con_Reportf( "Xash3D GameCube: heap check ok %s chunks=%d top=%p (#%u)\n", tag, chunks, (void *)top, ok_logs );
	}
	else
	{
		failures++;
		Con_Reportf( S_ERROR "Xash3D GameCube: heap check FAILED %s: %s\n", tag, why );
	}
	return ok;
}

/* Armed from gameplay active until the first world DL frame (or the first
 * failure): bisects the heap break between those two points frame by frame. */
static qboolean gc_heap_check_armed;

void GC_HeapCheckArm( qboolean on )
{
	gc_heap_check_armed = on;
}

void GC_HeapCheckArmed( const char *tag )
{
	if( gc_heap_check_armed && !GC_HeapCheck( tag ))
		gc_heap_check_armed = false;
}

/* Heap bytes still allocatable: free inside the arena plus unclaimed MEM1. */
size_t GC_MemHeapFreeBytes( void )
{
	size_t used, free_in_heap, unclaimed;

	GC_MemHeapStats( &used, &free_in_heap, &unclaimed );
	return free_in_heap + unclaimed;
}

/* Tip pressure for menu/client bring-up — unclaimed MEM1 above the arena can
 * look "fat" while fordblks is already < few KiB (DoD menu boot). */
size_t GC_MemArenaFreeBytes( void )
{
	size_t used, free_in_heap, unclaimed;

	GC_MemHeapStats( &used, &free_in_heap, &unclaimed );
	(void)used;
	(void)unclaimed;
	return free_in_heap;
}

void GC_HeapCensusReport( const char *stage );

void GC_MapLoadBufferWorldSettled( qboolean settled );
void GC_TexRegionReport( const char *stage );
#ifdef XASH_GC_MALLOC_TRACE
void GC_MallocTraceFlush( void );
#endif

void GC_MemSample( const char *stage )
{
#ifdef XASH_GC_MALLOC_TRACE
	GC_MallocTraceFlush();
#endif
	size_t total = Mem_TotalRealSize();
	size_t delta = (total >= gc_mem_last) ? (total - gc_mem_last) : 0;

	GC_MemWatch( stage );

	/* Per-map budget: spawning is done, so move map-lifetime heap blocks into
	 * the map's leftover memory (runs every map, unchanged totals or not).
	 * From here on, arena carves must avoid the world's arrays. */
	if( !Q_strcmp( stage, "bsp-buffer-release" ) || !Q_strcmp( stage, "entity spawn" ) || !Q_strcmp( stage, "pre-SV_Init" ))
		GC_HeapCensusReport( stage );
	if( !Q_strcmp( stage, "entity spawn" ))
	{
		extern void SV_GCParityReport( void );
		SV_GCParityReport();
	}
	if( !Q_strcmp( stage, "map active" ))
	{
		extern void Mod_GCMoveWorldEntitiesToArena( void );

		GC_MapLoadBufferWorldSettled( true );
		Mod_GCMoveWorldEntitiesToArena();
	}

	if( total > gc_mem_hwm )
		gc_mem_hwm = total;

	gc_mem_last = total;

	/* G72: unchanged totals are noise during map load; keep FAIL path chatty. */
	if( delta == 0 )
		return;

	{
		size_t heap_used, heap_free, heap_unclaimed;

		GC_MemHeapStats( &heap_used, &heap_free, &heap_unclaimed );
		Con_Reportf( "Xash3D GameCube: mem stage=%s total=%s delta=%s hwm=%s map=%s heap_used=%s heap_free=%s unclaimed=%s\n",
			stage, Q_memprint( total ), Q_memprint( delta ), Q_memprint( gc_mem_hwm ), gc_mem_map,
			Q_memprint( heap_used ), Q_memprint( heap_free ), Q_memprint( heap_unclaimed ));
	}
	if( !Q_strcmp( stage, "map active" ))
	{
		extern void GC_MemReportPools( const char *stage );

		GC_MemReportPools( stage );
		GC_HeapCensusReport( stage );
		GC_TexRegionReport( stage );
	}
}

/*
 * Boot arena: first-fit allocator over the renderer's vid.mapload_pad.
 *
 * Before the first map load that 1 MiB of BSS is idle, while the libc heap is
 * already short (two 640x480 XFBs plus the map-time static tables).  Large
 * allocations that the heap cannot satisfy during boot, the startup video and
 * the main menu are served here instead.  The first map load closes the arena
 * for the session; if anything is still live then, the renderer keeps the pad
 * out of the map-load arena rather than overwrite it.
 */
byte *R_GcmapBootPadRegion( size_t *capacity );

#define GC_BOOT_ARENA_ALIGN 32u
#define GC_BOOT_ARENA_MIN_ALLOC ( 8u * 1024u )

typedef struct gc_boot_blk_s
{
	uint32_t size;	/* whole block incl. header, multiple of GC_BOOT_ARENA_ALIGN */
	uint32_t used;
	uint32_t pad[6];	/* keep the payload 32-byte aligned */
} gc_boot_blk_t;

/* Heap-pressure fallback: small allocations the libc heap refuses are served
 * from a bump zone at the TOP of the pad, so whatever is still live at the
 * first map load pins only the top of the pad (the map-load arena ends at the
 * pad's end) instead of the whole of it. */
#define GC_BOOT_SMALL_ZONE ( 256u * 1024u )
#define GC_BOOT_SMALL_MAX ( 32u * 1024u )	/* largest block the zone serves */

static byte *gc_boot_base;
static size_t gc_boot_cap;
static size_t gc_boot_large_cap;	/* first-fit region: cap minus the small zone */
static byte *gc_boot_small_low, *gc_boot_small_top;
static unsigned int gc_boot_small_live;
static size_t gc_boot_small_live_bytes;
static qboolean gc_boot_ready, gc_boot_closed;
static unsigned int gc_boot_live;
static size_t gc_boot_live_bytes, gc_boot_peak_bytes;

static qboolean GC_BootArenaInit( void )
{
	gc_boot_blk_t *first;

	if( gc_boot_ready )
		return gc_boot_base != NULL;
	gc_boot_ready = true;
	gc_boot_base = R_GcmapBootPadRegion( &gc_boot_cap );
	gc_boot_cap &= ~(size_t)( GC_BOOT_ARENA_ALIGN - 1u );
	if( !gc_boot_base || gc_boot_cap < 2u * sizeof( gc_boot_blk_t ))
	{
		gc_boot_base = NULL;
		return false;
	}
	gc_boot_large_cap = gc_boot_cap;
	if( gc_boot_cap > 4u * GC_BOOT_SMALL_ZONE )
	{
		gc_boot_large_cap = gc_boot_cap - GC_BOOT_SMALL_ZONE;
		gc_boot_small_top = gc_boot_small_low = gc_boot_base + gc_boot_cap;
	}
	first = (gc_boot_blk_t *)gc_boot_base;
	first->size = (uint32_t)gc_boot_large_cap;
	first->used = 0;
	Con_Reportf( "Xash3D GameCube: boot arena ready capacity=%s\n", Q_memprint( gc_boot_cap ));
	return true;
}

qboolean GC_BootArenaOwns( const void *ptr )
{
	const byte *p = (const byte *)ptr;
	return gc_boot_base && p >= gc_boot_base && p < gc_boot_base + gc_boot_cap;
}

void *GC_BootArenaAlloc( size_t size )
{
	size_t need;
	byte *cursor;

	if( gc_boot_closed || size < GC_BOOT_ARENA_MIN_ALLOC || !GC_BootArenaInit() )
		return NULL;
	need = ( sizeof( gc_boot_blk_t ) + size + GC_BOOT_ARENA_ALIGN - 1u ) & ~(size_t)( GC_BOOT_ARENA_ALIGN - 1u );

	for( cursor = gc_boot_base; cursor < gc_boot_base + gc_boot_large_cap; cursor += ((gc_boot_blk_t *)cursor)->size )
	{
		gc_boot_blk_t *blk = (gc_boot_blk_t *)cursor;

		if( blk->used || blk->size < need )
			continue;
		if( blk->size - need >= 2u * sizeof( gc_boot_blk_t ))
		{
			gc_boot_blk_t *rest = (gc_boot_blk_t *)( cursor + need );
			rest->size = blk->size - (uint32_t)need;
			rest->used = 0;
			blk->size = (uint32_t)need;
		}
		blk->used = 1;
		gc_boot_live++;
		gc_boot_live_bytes += blk->size;
		if( gc_boot_live_bytes > gc_boot_peak_bytes )
			gc_boot_peak_bytes = gc_boot_live_bytes;
		return cursor + sizeof( gc_boot_blk_t );
	}
	Con_Reportf( "Xash3D GameCube: boot arena full need=%s live=%s\n",
		Q_memprint( need ), Q_memprint( gc_boot_live_bytes ));
	return NULL;
}

/* Last resort for a small allocation when the libc heap is exhausted. */
void *GC_BootArenaAllocSmall( size_t size )
{
	size_t need;
	gc_boot_blk_t *blk;

	if( size == 0 || size > GC_BOOT_SMALL_MAX || ( gc_boot_closed && !gc_boot_small_top )
		|| !GC_BootArenaInit() || !gc_boot_small_top )
		return NULL;
	need = ( sizeof( gc_boot_blk_t ) + size + GC_BOOT_ARENA_ALIGN - 1u ) & ~(size_t)( GC_BOOT_ARENA_ALIGN - 1u );
	/* Reuse a freed block first: realloc growth and short-lived allocations
	 * otherwise leave holes the downward bump never revisits, and the whole
	 * zone drains on churn with almost nothing live. */
#ifndef GC_BOOT_SMALL_NOREUSE
	{
		byte *cursor;

		for( cursor = gc_boot_small_low; cursor < gc_boot_small_top; cursor += ((gc_boot_blk_t *)cursor)->size )
		{
			gc_boot_blk_t *hole = (gc_boot_blk_t *)cursor;

			if( hole->used || hole->size < need )
				continue;
			if( hole->size - need >= 2u * GC_BOOT_ARENA_ALIGN + sizeof( gc_boot_blk_t ))
			{
				gc_boot_blk_t *rest = (gc_boot_blk_t *)( cursor + need );

				rest->size = hole->size - (uint32_t)need;
				rest->used = 0;
				hole->size = (uint32_t)need;
			}
			hole->used = 1;
			gc_boot_small_live++;
			gc_boot_small_live_bytes += hole->size;
			return (byte *)hole + sizeof( gc_boot_blk_t );
		}
	}
#endif
	/* After the map load the pad below small_low belongs to the map loader, so
	 * only the holes inside the pinned zone above may still be handed out. */
	if( gc_boot_closed )
		return NULL;
	if( (size_t)( gc_boot_small_low - ( gc_boot_base + gc_boot_large_cap )) < need )
		return NULL;
	gc_boot_small_low -= need;
	blk = (gc_boot_blk_t *)gc_boot_small_low;
	blk->size = (uint32_t)need;
	blk->used = 1;
	gc_boot_small_live++;
	gc_boot_small_live_bytes += need;
	return (byte *)blk + sizeof( gc_boot_blk_t );
}

/* Lowest address that must stay out of the map-load arena, or NULL. */
void *GC_BootArenaPinnedLow( void )
{
	if( gc_boot_live )
		return gc_boot_base;
	if( gc_boot_small_live )
		return gc_boot_small_low;
	return NULL;
}

qboolean GC_BootArenaFree( void *ptr )
{
	gc_boot_blk_t *blk;
	byte *cursor;

	if( !ptr || !GC_BootArenaOwns( ptr ))
		return false;
	blk = (gc_boot_blk_t *)((byte *)ptr - sizeof( gc_boot_blk_t ));
	if( !blk->used )
	{
		Con_Reportf( S_ERROR "Xash3D GameCube: boot arena double free %p\n", ptr );
		return true;
	}
	if( gc_boot_small_top && (byte *)blk >= gc_boot_base + gc_boot_large_cap )
	{
		blk->used = 0;
		gc_boot_small_live--;
		gc_boot_small_live_bytes -= blk->size;
#ifdef GC_BOOT_SMALL_POISON
		/* Stale users of freed small-zone memory read 0xDD and a later reuse
		 * of the block is easy to spot in dumps. */
		memset( (byte *)blk + sizeof( gc_boot_blk_t ), 0xDD, blk->size - sizeof( gc_boot_blk_t ));
#endif
#ifndef GC_BOOT_SMALL_NOREUSE
		/* Merge runs of free neighbours so holes can serve larger blocks. */
		{
			byte *cursor = gc_boot_small_low;

			while( cursor < gc_boot_small_top )
			{
				gc_boot_blk_t *cur = (gc_boot_blk_t *)cursor;
				byte *next = cursor + cur->size;

				if( !cur->used && next < gc_boot_small_top && !((gc_boot_blk_t *)next)->used )
					cur->size += ((gc_boot_blk_t *)next)->size;
				else
					cursor = next;
			}
		}
#endif
		/* The zone is contiguous from small_low to its top: retreat past
		 * freed blocks so a dead zone costs nothing at the map load. */
		while( gc_boot_small_low < gc_boot_small_top && !((gc_boot_blk_t *)gc_boot_small_low)->used )
			gc_boot_small_low += ((gc_boot_blk_t *)gc_boot_small_low)->size;
		return true;
	}
	blk->used = 0;
	gc_boot_live--;
	gc_boot_live_bytes -= blk->size;

	/* Coalesce every run of free neighbours (few blocks; a full walk is cheap). */
	for( cursor = gc_boot_base; cursor < gc_boot_base + gc_boot_large_cap; cursor += ((gc_boot_blk_t *)cursor)->size )
	{
		gc_boot_blk_t *cur = (gc_boot_blk_t *)cursor;

		while( !cur->used && cursor + cur->size < gc_boot_base + gc_boot_large_cap )
		{
			gc_boot_blk_t *next = (gc_boot_blk_t *)( cursor + cur->size );
			if( next->used )
				break;
			cur->size += next->size;
		}
	}
	return true;
}


/*
 * Heap census (diagnostic; disc config "heapcensus", set by probe builds only).
 *
 * The engine's tracked pools hold ~330 KiB of a ~1.2 MiB heap; the rest is
 * plain malloc from the game DLL, filesystem, renderer and libraries with no
 * owner on record.  With the census on, every heap block allocated from then
 * on is recorded (pointer -> caller, size) in a table placed at 0x81800000:
 * Dolphin backs 32 MiB for the 24 MiB MEM1 (next power of two), so the table
 * sits outside the retail layout and the heap is untouched -- no headers, no
 * extra heap bytes, the same OOM thresholds as a normal run.  Never enable it
 * on hardware: that address is not RAM there.
 * GC_HeapCensusReport prints the top call sites per "map active";
 * scripts/gamecube-heap-census.sh resolves them.
 */
#define GC_CENSUS_BASE    0x81800000u
#define GC_CENSUS_PTRS    ( 1u << 16 )	/* 64k live blocks, 12 bytes each */
#define GC_CENSUS_CALLERS 2048u

typedef struct { uint32_t ptr, caller, size; } gc_census_ent_t;
typedef struct { uint32_t caller, live, count, peak; } gc_census_slot_t;

static qboolean gc_census_on;
static gc_census_ent_t *gc_census_ptrs;
static gc_census_slot_t *gc_census_callers;
static uint32_t gc_census_dropped;

void GC_HeapCensusEnable( void )
{
	if( gc_census_on )
		return;
	gc_census_ptrs = (gc_census_ent_t *)GC_CENSUS_BASE;
	gc_census_callers = (gc_census_slot_t *)( GC_CENSUS_BASE + GC_CENSUS_PTRS * sizeof( gc_census_ent_t ));
	memset( gc_census_ptrs, 0, GC_CENSUS_PTRS * sizeof( gc_census_ent_t ));
	memset( gc_census_callers, 0, GC_CENSUS_CALLERS * sizeof( gc_census_slot_t ));
	gc_census_on = true;
}

static gc_census_slot_t *GC_CensusCaller( uint32_t caller )
{
	uint32_t i = ( caller >> 2 ) % GC_CENSUS_CALLERS, n;

	for( n = 0; n < GC_CENSUS_CALLERS; n++, i = ( i + 1 ) % GC_CENSUS_CALLERS )
	{
		if( gc_census_callers[i].caller == caller )
			return &gc_census_callers[i];
		if( !gc_census_callers[i].caller )
		{
			gc_census_callers[i].caller = caller;
			return &gc_census_callers[i];
		}
	}
	return NULL;
}

static uint32_t GC_CensusHash( uint32_t ptr )
{
	return (( ptr >> 3 ) * 2654435761u ) & ( GC_CENSUS_PTRS - 1 );
}

static void GC_CensusAdd( void *p, size_t size, uint32_t caller )
{
	uint32_t i, n;
	gc_census_slot_t *c;

#if XASH_GC_HEAP_CENSUS_BOOT
	/* Census builds (CFLAGS from SWEEP_HEAP_CENSUS) start at the first
	 * allocation, so boot-time blocks are on record too. */
	if( !gc_census_on )
		GC_HeapCensusEnable();
#endif
	if( !gc_census_on || !p )
		return;
	for( i = GC_CensusHash( (uint32_t)(uintptr_t)p ), n = 0; n < GC_CENSUS_PTRS; n++, i = ( i + 1 ) & ( GC_CENSUS_PTRS - 1 ))
	{
		if( !gc_census_ptrs[i].ptr || gc_census_ptrs[i].ptr == (uint32_t)(uintptr_t)p )
			break;
	}
	if( n == GC_CENSUS_PTRS )
	{
		gc_census_dropped++;
		return;
	}
	if( gc_census_ptrs[i].ptr && ( c = GC_CensusCaller( gc_census_ptrs[i].caller )))
	{
		/* Same block recorded again (realloc in place, nested wrapper). */
		c->live -= Q_min( c->live, gc_census_ptrs[i].size );
		if( c->count )
			c->count--;
	}
	gc_census_ptrs[i].ptr = (uint32_t)(uintptr_t)p;
	gc_census_ptrs[i].caller = caller;
	gc_census_ptrs[i].size = (uint32_t)size;
	if(( c = GC_CensusCaller( caller )))
	{
		c->live += (uint32_t)size;
		c->count++;
		if( c->live > c->peak )
			c->peak = c->live;
	}
}

static void GC_CensusRemove( void *p )
{
	uint32_t i, j, n;
	gc_census_slot_t *c;

	if( !gc_census_on || !p )
		return;
	for( i = GC_CensusHash( (uint32_t)(uintptr_t)p ), n = 0; n < GC_CENSUS_PTRS; n++, i = ( i + 1 ) & ( GC_CENSUS_PTRS - 1 ))
	{
		if( !gc_census_ptrs[i].ptr )
			return;	/* allocated before the census started */
		if( gc_census_ptrs[i].ptr == (uint32_t)(uintptr_t)p )
			break;
	}
	if( n == GC_CENSUS_PTRS )
		return;
	if(( c = GC_CensusCaller( gc_census_ptrs[i].caller )))
	{
		c->live -= Q_min( c->live, gc_census_ptrs[i].size );
		if( c->count )
			c->count--;
	}
	/* Backward-shift deletion keeps every probe chain unbroken. */
	gc_census_ptrs[i].ptr = 0;
	for( j = ( i + 1 ) & ( GC_CENSUS_PTRS - 1 ); gc_census_ptrs[j].ptr; j = ( j + 1 ) & ( GC_CENSUS_PTRS - 1 ))
	{
		const uint32_t home = GC_CensusHash( gc_census_ptrs[j].ptr );

		if((( j - home ) & ( GC_CENSUS_PTRS - 1 )) >= (( j - i ) & ( GC_CENSUS_PTRS - 1 )))
		{
			gc_census_ptrs[i] = gc_census_ptrs[j];
			gc_census_ptrs[j].ptr = 0;
			i = j;
		}
	}
}

void GC_HeapCensusReport( const char *stage )
{
	uint32_t total = 0, i, k;
	gc_census_slot_t top[16];

	if( !gc_census_on )
		return;
	memset( top, 0, sizeof( top ));
	for( i = 0; i < GC_CENSUS_CALLERS; i++ )
	{
		const gc_census_slot_t *c = &gc_census_callers[i];

		if( !c->caller || !c->live )
			continue;
		total += c->live;
		for( k = 0; k < 16; k++ )
		{
			if( c->live > top[k].live )
			{
				memmove( &top[k + 1], &top[k], sizeof( top[0] ) * ( 15 - k ));
				top[k] = *c;
				break;
			}
		}
	}
	Con_Reportf( "Xash3D GameCube: heap census at=%s live=%u dropped=%u\n", stage, (unsigned)total, (unsigned)gc_census_dropped );
	for( k = 0; k < 16 && top[k].caller; k++ )
		Con_Reportf( "Xash3D GameCube: heap census caller=0x%08x live=%u count=%u peak=%u\n",
			(unsigned)top[k].caller, (unsigned)top[k].live, (unsigned)top[k].count, (unsigned)top[k].peak );
}

/* Linked with -Wl,--wrap=free,--wrap=realloc: any boot-arena block can be
 * released or resized through the C library entry points. */
void __real_free( void *ptr );
void *__real_realloc( void *ptr, size_t size );

void __wrap_free( void *ptr )
{
	GC_CensusRemove( ptr );
	if( GC_BootArenaFree( ptr ))
		return;
	__real_free( ptr );
}

void *__real_malloc( size_t size );
void *__real_calloc( size_t nmemb, size_t size );

/* Every libc/library allocation: when the heap refuses a small block before
 * the first map load, serve it from the boot pad's small zone. */
void *__wrap_malloc( size_t size )
{
	void *p = __real_malloc( size );

	if( !p && size )
		p = GC_BootArenaAllocSmall( size );
	GC_CensusAdd( p, size, (uint32_t)(uintptr_t)__builtin_return_address( 0 ));
	return p;
}

void *__wrap_calloc( size_t nmemb, size_t size )
{
	void *p = __real_calloc( nmemb, size );

	if( !p && nmemb && size && nmemb <= GC_BOOT_SMALL_MAX / size )
	{
		p = GC_BootArenaAllocSmall( nmemb * size );
		if( p )
			memset( p, 0, nmemb * size );
	}
	GC_CensusAdd( p, nmemb * size, (uint32_t)(uintptr_t)__builtin_return_address( 0 ));
	return p;
}

void *__real_memalign( size_t align, size_t size );

void *__wrap_memalign( size_t align, size_t size )
{
	void *p = __real_memalign( align, size );

	GC_CensusAdd( p, size, (uint32_t)(uintptr_t)__builtin_return_address( 0 ));
	return p;
}

static void *GC_ReallocImpl( void *ptr, size_t size );

void *__wrap_realloc( void *ptr, size_t size )
{
	const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address( 0 );
	void *r = GC_ReallocImpl( ptr, size );

	/* A failed realloc leaves the old block live. */
	if( r || size == 0 )
		GC_CensusRemove( ptr );
	if( r )
		GC_CensusAdd( r, size, caller );
	return r;
}

static void *GC_ReallocImpl( void *ptr, size_t size )
{
	gc_boot_blk_t *blk;
	size_t have;
	void *neu;

	if( !ptr || !GC_BootArenaOwns( ptr ))
	{
		extern size_t malloc_usable_size( void *ptr );
		void *r = __real_realloc( ptr, size );

		/* Heap refused the grow: move the block into the boot pad's small
		 * zone (the old block stays valid when realloc fails). */
		if( !r && size )
		{
			if( !ptr )
				r = GC_BootArenaAllocSmall( size );
			else
			{
				size_t old = malloc_usable_size( ptr );

				r = GC_BootArenaAllocSmall( size );
				if( r )
				{
					memcpy( r, ptr, old < size ? old : size );
					__real_free( ptr );
				}
			}
		}
		return r;
	}
	if( size == 0 )
	{
		GC_BootArenaFree( ptr );
		return NULL;
	}
	blk = (gc_boot_blk_t *)((byte *)ptr - sizeof( gc_boot_blk_t ));
	have = blk->size - sizeof( gc_boot_blk_t );
	if( size <= have )
		return ptr;
	neu = malloc( size );
	if( !neu )
		neu = GC_BootArenaAlloc( size );
	if( !neu )
		return NULL;
	memcpy( neu, ptr, have );
	GC_BootArenaFree( ptr );
	return neu;
}

/* Menu/UI texture uploads may borrow the boot arena through the renderer's
 * zone pool; other ref_gx zone traffic (world/HUD/fonts that outlive the
 * menu) never does.  Set by the renderer around resource/ and gfx/shell/
 * uploads only. */
static qboolean gc_boot_menu_scope;
void GC_BootArenaSetMenuScope( qboolean on ) { gc_boot_menu_scope = on; }
qboolean GC_BootArenaMenuScope( void ) { return gc_boot_menu_scope && !gc_boot_closed; }
size_t GC_BootArenaLiveBytes( void ) { return ( gc_boot_live ? gc_boot_live_bytes : 0 ) + gc_boot_small_live_bytes; }

#ifdef XASH_GC_MEMWATCH_DEBUG
/* Where is the pad small zone and the sbrk break?  Compare with a corrupt block address. */
extern void *sbrk( ptrdiff_t incr );
void GC_BootDiagLog( const char *stage )
{
	struct mallinfo mi = mallinfo();
	Con_Printf( "Xash3D GameCube: bootdiag %s pad=[%p,%p) large_end=%p small=[%p,%p) sbrk=%p arena1=[%p,%p) heap_used=%d heap_free=%d\n",
		stage, (void *)gc_boot_base, (void *)( gc_boot_base + gc_boot_cap ),
		(void *)( gc_boot_base + gc_boot_large_cap ), (void *)gc_boot_small_low, (void *)gc_boot_small_top,
		sbrk( 0 ), SYS_GetArena1Lo(), SYS_GetArena1Hi(), mi.uordblks, mi.fordblks );
}
#endif

/* First map load: no new boot allocations.  Returns the bytes still live. */
size_t GC_BootArenaClose( void )
{
	if( !gc_boot_closed )
	{
		gc_boot_closed = true;
#ifdef XASH_GC_MEMWATCH_DEBUG
		GC_BootDiagLog( "boot arena closed" );
#endif
		if( gc_boot_base )
			Con_Reportf( "Xash3D GameCube: boot arena closed live=%u bytes=%s peak=%s\n",
				gc_boot_live, Q_memprint( gc_boot_live_bytes ), Q_memprint( gc_boot_peak_bytes ));
	}
	return ( gc_boot_live ? gc_boot_live_bytes : 0 ) + gc_boot_small_live_bytes;
}

/* Unconditional heap mark at fixed boot stages (MEM1 headroom telemetry). */
void GC_MemHeapMark( const char *stage )
{
	size_t heap_used, heap_free, heap_unclaimed;
	GC_MemHeapStats( &heap_used, &heap_free, &heap_unclaimed );
	Con_Reportf( "Xash3D GameCube: heap mark stage=%s used=%u free=%u unclaimed=%u avail=%u\n",
		stage, (unsigned)heap_used, (unsigned)heap_free, (unsigned)heap_unclaimed,
		(unsigned)( heap_free + heap_unclaimed ));
}

void GC_MemBreadcrumb( const char *stage )
{
	size_t heap_used, heap_free, heap_unclaimed;

	if( !stage || !stage[0] ) return;
	GC_WatchdogMark( stage );
	GC_MemHeapStats( &heap_used, &heap_free, &heap_unclaimed );
	/* This is deliberately written even when the memory total is unchanged:
	 * the last completed stage is the useful artifact after a hard reset. */
	GCube_AppendSwissEvidence( "breadcrumb stage=%s map=%s total=%u hwm=%u heap_used=%u heap_free=%u unclaimed=%u",
		stage, gc_mem_map, (unsigned)Mem_TotalRealSize(), (unsigned)gc_mem_hwm,
		(unsigned)heap_used, (unsigned)heap_free, (unsigned)heap_unclaimed );
}

void GC_MemFail( const char *subsystem, size_t size, const char *file, int line )
{
	size_t heap_used, heap_free, heap_unclaimed;

	GC_MemHeapStats( &heap_used, &heap_free, &heap_unclaimed );
	Con_Reportf( "Xash3D GameCube: mem FAIL subsystem=%s size=%s map=%s at=%s:%i total=%s hwm=%s heap_used=%s heap_free=%s unclaimed=%s\n",
		subsystem ? subsystem : "unknown", Q_memprint( size ), gc_mem_map, file, line,
		Q_memprint( Mem_TotalRealSize() ), Q_memprint( gc_mem_hwm ),
		Q_memprint( heap_used ), Q_memprint( heap_free ), Q_memprint( heap_unclaimed ));
	GC_MemReportPoolPressure( subsystem, size );
}

/* Runtime memory arena telemetry */
void GC_MemArena_GetStats( GC_MemArenaStats *stats )
{
	size_t total = Mem_TotalRealSize();
	
	if( !stats )
		return;
	
	stats->total = total;
	stats->hwm = gc_mem_hwm;
	stats->budget = GC_MEMORY_BUDGET_BYTES;
	stats->budget_used = total;
	stats->budget_free = (total < GC_MEMORY_BUDGET_BYTES) ? (GC_MEMORY_BUDGET_BYTES - total) : 0;
	stats->budget_exceeded = (total > GC_MEMORY_BUDGET_BYTES);
}

qboolean GC_MemBudgetCheck( void )
{
	size_t total = Mem_TotalRealSize();
	return (total <= GC_MEMORY_BUDGET_BYTES);
}

void GC_MemBudgetWarn( const char *stage )
{
	size_t total = Mem_TotalRealSize();
	size_t used_percent = (total * 100) / GC_MEMORY_BUDGET_BYTES;
	
	if( total >= GC_MEMORY_CRITICAL_95_PERCENT )
	{
		Con_Reportf( S_ERROR "Xash3D GameCube: mem CRITICAL %d%% budget used (%s/%s) at %s\n",
			used_percent, Q_memprint( total ), Q_memprint( GC_MEMORY_BUDGET_BYTES ), stage );
	}
	else if( total >= GC_MEMORY_WARNING_90_PERCENT )
	{
		Con_Reportf( S_WARN "Xash3D GameCube: mem WARNING %d%% budget used (%s/%s) at %s\n",
			used_percent, Q_memprint( total ), Q_memprint( GC_MEMORY_BUDGET_BYTES ), stage );
	}
	else if( total >= GC_MEMORY_WARNING_80_PERCENT )
	{
		Con_Reportf( "Xash3D GameCube: mem INFO %d%% budget used (%s/%s) at %s\n",
			used_percent, Q_memprint( total ), Q_memprint( GC_MEMORY_BUDGET_BYTES ), stage );
	}
}

qboolean GC_MemBudgetEnforce( size_t requested, const char *subsystem )
{
	size_t total = Mem_TotalRealSize();
	size_t new_total = total + requested;
	
	if( new_total > GC_MEMORY_BUDGET_BYTES )
	{
		Con_Reportf( S_ERROR "Xash3D GameCube: mem BUDGET EXCEEDED requested=%s total=%s new=%s budget=%s subsystem=%s\n",
			Q_memprint( requested ), Q_memprint( total ), Q_memprint( new_total ),
			Q_memprint( GC_MEMORY_BUDGET_BYTES ), subsystem ? subsystem : "unknown" );
		return false;
	}
	
	return true;
}

/* Memory budget telemetry */
size_t GC_MemBudgetTotal( void )
{
	return Mem_TotalRealSize();
}

size_t GC_MemBudgetUsed( void )
{
	return Mem_TotalRealSize();
}

size_t GC_MemBudgetFree( void )
{
	size_t total = Mem_TotalRealSize();
	return (total < GC_MEMORY_BUDGET_BYTES) ? (GC_MEMORY_BUDGET_BYTES - total) : 0;
}

qboolean GC_MemBudgetExceeded( void )
{
	return (Mem_TotalRealSize() > GC_MEMORY_BUDGET_BYTES);
}

/* Entity memory estimation */
size_t GC_EntityEstimateSize( void )
{
	/* Estimate edict_t + typical private data (model strings, etc.)
	 * edict_t is ~2KB on GameCube, private data varies by entity type.
	 * Use a conservative average estimate for budgeting.
	 * Note: This is an estimate based on typical entity sizes.
	 * The actual size depends on the progdefs and entity type. */
	return 2048 + 512; /* ~2.5KB per entity average */
}

/* Map-load memory pressure measurement */
void GC_MapLoadPressureBegin( void )
{
	gc_mapload_pressure_base = Mem_TotalRealSize();
	gc_mapload_pressure_peak = gc_mapload_pressure_base;
	gc_mapload_pressure_delta = 0;
	gc_mapload_pressure_active = true;
}

void GC_MapLoadPressureEnd( void )
{
	gc_mapload_pressure_active = false;
}

void GC_MapLoadPressureSample( const char *stage )
{
	size_t total;
	
	if( !gc_mapload_pressure_active )
		return;
	
	total = Mem_TotalRealSize();
	
	if( total > gc_mapload_pressure_peak )
	{
		gc_mapload_pressure_peak = total;
		gc_mapload_pressure_delta = gc_mapload_pressure_peak - gc_mapload_pressure_base;
		Con_Reportf( "Xash3D GameCube: map-load pressure stage=%s peak=%s delta=%s base=%s\n",
			stage, Q_memprint( gc_mapload_pressure_peak ), Q_memprint( gc_mapload_pressure_delta ),
			Q_memprint( gc_mapload_pressure_base ) );
	}
}

size_t GC_MapLoadPressurePeak( void )
{
	return gc_mapload_pressure_peak;
}

size_t GC_MapLoadPressureDelta( void )
{
	return gc_mapload_pressure_delta;
}

void GC_InitMapLoadBuffer( void )
{
	/* Prepared on demand in GC_PrepareMapLoadBuffer after client/menu trim. */
}

void GC_PrepareMapLoadBuffer( size_t size )
{
	void *buf;

	if( size == 0 )
		size = GC_MAPLOAD_BUFFER_DEFAULT;

	buf = GC_BorrowMapLoadBuffer( size );
	if( buf )
		GC_ReleaseMapLoadBuffer( buf );
}

void GC_PrepareMapLoadBufferForMap( const char *mapname )
{
	if( !mapname || !mapname[0] )
	{
		GC_PrepareMapLoadBuffer( GC_MAPLOAD_BUFFER_DEFAULT );
		return;
	}
	GC_LoadMapMetadata( mapname );
	/* Do not stat the BSP here: map-load trim can leave MEM1 with only a few
	 * hundred bytes free, and a failed directory-cache allocation in the ISO
	 * filesystem otherwise DSI's before Mod_LoadModel can run. The fixed arena
	 * is also the compact-load capacity for oversized BSPs. */
	GC_PrepareMapLoadBuffer( GC_MAPLOAD_BUFFER_DEFAULT );
}

qboolean GC_LoadMapMetadata( const char *mapname )
{
	char path[MAX_QPATH];
	char base[MAX_QPATH];
	/* The metadata is only needed while parsing. Do not reserve 64 KiB of
	 * scarce MEM1 permanently for a buffer that is idle during gameplay. */
	char *text;
	file_t *f;
	int n, rows = 0, hot = 0;
	fs_offset_t text_size;
	qboolean valid = false;
	unsigned file_size = 0;
	memset( gc_bsp_face_batch, 0, sizeof( gc_bsp_face_batch ));
	gc_bsp_batch_faces = 0;
	gc_bsp_batch_count = 0;
	gc_bsp_batches_valid = false;
	memset( gc_bsp_lump_offset, 0, sizeof( gc_bsp_lump_offset ));
	memset( gc_bsp_lump_size, 0, sizeof( gc_bsp_lump_size ));
	memset( gc_bsp_lump_hot, 0, sizeof( gc_bsp_lump_hot ));
	memset( gc_bsp_lump_aram, 0, sizeof( gc_bsp_lump_aram ));
	gc_bsp_hot_aram = gc_bsp_hot_bytes = 0;
	if( !mapname || !mapname[0] ) return false;
	COM_FileBase( mapname, base, sizeof( base ));
	/* The disc builder only ships .gcmeta files for the c0a0 tram chain.
	 * Looking up an absent metadata file walks the ISO9660 directory tree and
	 * duplicates its path; map-load MEM1 can be too tight for that optional
	 * allocation (DSI in entry_from_path when strdup returns NULL). */
	if( Q_stricmp( base, "c0a0" ) && Q_stricmp( base, "c0a0a" )
		&& Q_stricmp( base, "c0a0b" ) && Q_stricmp( base, "c0a0b1" )
		&& Q_stricmp( base, "c0a0b2" ) && Q_stricmp( base, "c0a0c" )
		&& Q_stricmp( base, "c0a0d" ) && Q_stricmp( base, "c0a0e" ))
		return false;
	Q_snprintf( path, sizeof( path ), "media/%s.gcmeta", base );
	f = FS_Open( path, "rb", false );
	if( !f ) return false;
	text_size = FS_FileLength( f );
	if( text_size <= 0 || text_size >= 65536 )
	{
		FS_Close( f );
		return false;
	}
	text = malloc( (size_t)text_size + 1 );
	if( !text )
	{
		FS_Close( f );
		return false;
	}
	n = FS_Read( f, text, (size_t)text_size );
	FS_Close( f );
	if( n != text_size ) goto done;
	text[n] = '\0';
	{
		char *cursor = text;
		char *end;
	unsigned version, crc;
	fs_offset_t actual_size;
		if( sscanf( cursor, "GCMETA 1 %u %x %u", &version, &crc, &file_size ) != 3
			|| version != 30 || !crc || !file_size ) goto done;
		actual_size = FS_FileSize( va( "maps/%s.bsp", base ), false );
		if( actual_size <= 0 || (unsigned)actual_size != file_size ) goto done;
		end = strchr( cursor, '\n' );
		if( !end ) goto done;
		cursor = end + 1;
		while( cursor && *cursor )
		{
			int index, offset, size, align;
			char name[32], tier[8];
			end = strchr( cursor, '\n' );
			if( end ) *end = '\0';
			if( !Q_strnicmp( cursor, "BATCHES ", 8 ))
			{
				int batch_count, face_count, batch_no = 0;
				if( sscanf( cursor, "BATCHES %d %d", &batch_count, &face_count ) != 2
					|| batch_count < 0 || batch_count > 4096 || face_count < 0
					|| face_count > GC_BSP_BATCH_FACE_MAX ) goto done;
				cursor = end ? end + 1 : NULL;
				while( cursor && *cursor && batch_no < batch_count )
				{
					int miptex, flags, count, firstface, consumed = 0;
					char *p;
					end = strchr( cursor, '\n' );
					if( end ) *end = '\0';
					if( sscanf( cursor, "BATCH %d %d %d %d %n", &miptex, &flags,
						&count, &firstface, &consumed ) != 4 || count < 0 || firstface < 0
						|| firstface + count > face_count ) goto done;
					p = cursor + consumed;
					while( count-- > 0 )
					{
						char *next;
						long face = strtol( p, &next, 10 );
						if( next == p || face < 0 || face >= GC_BSP_BATCH_FACE_MAX ) goto done;
						gc_bsp_face_batch[face] = (unsigned short)( batch_no + 1 );
						if( face + 1 > gc_bsp_batch_faces ) gc_bsp_batch_faces = (int)face + 1;
						p = next;
						if( *p == ',' ) p++;
					}
					batch_no++;
					if( !end ) break;
					cursor = end + 1;
				}
				if( batch_no != batch_count ) goto done;
				gc_bsp_batch_count = (unsigned int)batch_count;
				gc_bsp_batches_valid = true;
				break;
			}
			if( sscanf( cursor, "%d %31s %7s %d %d %d", &index, name, tier,
				&offset, &size, &align ) != 6 || index != rows || index < 0 || index >= 15
				|| offset < 0 || size < 0 || align != ( offset & 31 )
				|| ( Q_stricmp( tier, "HOT" ) && Q_stricmp( tier, "COLD" )))
				goto done;
			gc_bsp_lump_offset[index] = offset;
			gc_bsp_lump_size[index] = size;
			gc_bsp_lump_hot[index] = !Q_stricmp( tier, "HOT" );
			if( !Q_stricmp( tier, "HOT" )) hot++;
			rows++;
			if( !end ) break;
			cursor = end + 1;
		}
	}
	if( rows != 15 ) goto done;
	Con_Reportf( "Xash3D GameCube: validated BSP metadata %s rows=%d hot=%d size=%u\n",
				path, rows, hot, file_size );
	valid = true;
done:
	free( text );
	if( !valid )
	{
		gc_bsp_batches_valid = false;
		memset( gc_bsp_lump_aram, 0, sizeof( gc_bsp_lump_aram ));
	}
	return valid;
}

/* Set at "map active", cleared when the next load borrows the arena: while
 * set, every tail carve must lie in a gap the world-array map says is free
 * (see GC_MapLoadBufferTailAlloc). */
static qboolean gc_mapload_world_settled;

void GC_MapLoadBufferWorldSettled( qboolean settled )
{
	gc_mapload_world_settled = settled;
}

qboolean GC_MapLoadBufferIsWorldSettled( void )
{
	return gc_mapload_world_settled;
}

void *GC_BorrowMapLoadBuffer( size_t size )
{
	if( size == 0 )
		return NULL;
	gc_mapload_world_settled = false;
	/* Entity private objects may still point into this map's arena. */
	if( SV_GCPrivateOverflowPinsMapArena() )
		return NULL;

	if( gc_mapload_buf && gc_mapload_buf_in_use )
		return NULL;

	if( gc_mapload_buf && size > gc_mapload_buf_size )
	{
		if( gc_mapload_buf_in_use )
			return NULL;
		if( !R_GCReleaseMapLoadStaticArena( gc_mapload_buf ))
			free( gc_mapload_buf );
		gc_mapload_buf = NULL;
		gc_mapload_buf_size = 0;
	}

	if( !gc_mapload_buf )
	{
		size_t alloc_size = ( size + 4095u ) & ~4095u;
		size_t static_capacity = 0;

		gc_mapload_buf = (byte *)R_GCBorrowMapLoadStaticArena( alloc_size, &static_capacity );
		if( gc_mapload_buf )
		{
			extern size_t GC_TexRegionNoteArena( byte *base, size_t capacity );
			const size_t usable = GC_TexRegionNoteArena( gc_mapload_buf, static_capacity );

			if( alloc_size > usable )
			{
				/* The persistent texture region holds the top of the arena:
				 * this map takes its lean load paths instead (logged). */
				Con_Reportf( "Xash3D GameCube: budget map-load arena short need=%s usable=%s (texture region %s); lean load\n",
					Q_memprint( alloc_size ), Q_memprint( usable ), Q_memprint( static_capacity - usable ));
				R_GCReleaseMapLoadStaticArena( gc_mapload_buf );
				gc_mapload_buf = NULL;
				gc_mapload_buf_size = 0;
				return NULL;
			}
			gc_mapload_buf_size = usable;
			gc_mapload_tail_highwater = 0;
			gc_mapload_tail_peak = 0;
			gc_mapload_buf_in_use = true;
			return gc_mapload_buf;
		}

		if( GC_MemHeapFreeBytes() < alloc_size )
		{
			Con_Reportf( S_ERROR "Xash3D GameCube: map-load buffer rejected by heap budget need=%s free=%s\n",
				Q_memprint( alloc_size ), Q_memprint( GC_MemHeapFreeBytes() ));
			return NULL;
		}
		gc_mapload_buf = (byte *)memalign( 32, alloc_size );
		if( !gc_mapload_buf )
		{
			/* One more shot after releasing decode scratch; keeps mid-size BSPs
			 * loading when the renderer arena is still a few hundred KB short. */
			Image_GCPurgeDecodeScratch();
			if( GC_MemHeapFreeBytes() >= alloc_size )
				gc_mapload_buf = (byte *)memalign( 32, alloc_size );
		}
		if( !gc_mapload_buf )
		{
			Con_Reportf( S_ERROR "Xash3D GameCube: map-load buffer alloc failed (%s)\n",
				Q_memprint( alloc_size ));
			return NULL;
		}
		gc_mapload_buf_size = alloc_size;
		gc_mapload_tail_highwater = 0;
		gc_mapload_tail_peak = 0;
		Con_Reportf( "Xash3D GameCube: map-load buffer ready %s\n", Q_memprint( alloc_size ));
	}

	/* A released staging buffer starts a fresh sub-arena carve session. */
	gc_mapload_tail_highwater = 0;
	gc_mapload_tail_peak = 0;
	gc_mapload_buf_in_use = true;
	return gc_mapload_buf;
}

qboolean GC_ReleaseMapLoadBuffer( void *ptr )
{
	if( !ptr || ptr != gc_mapload_buf )
		return false;

	gc_mapload_buf_in_use = false;
	return true;
}

void GC_RetainMapLoadBufferBorrow( void )
{
	if( gc_mapload_buf )
		gc_mapload_buf_in_use = true;
}

qboolean GC_IsMapLoadBuffer( const void *ptr )
{
	return ptr != NULL && ptr == gc_mapload_buf;
}

const byte *GC_MapLoadBufferBase( void )
{
	return gc_mapload_buf;
}

/*
 * Persistent texture region (per-map budget, texture slots).
 *
 * The renderer's mip storage (HUD, sprites, skins: GX reads it in place) was
 * ~86 KiB of heap from the client's connect on, leaving the heap empty during
 * play and too short at every level change (entities skipped as tip-starved).
 * Every map measured leaves >= 240 KiB at the top of the map-load arena
 * unused, so mips live in a region growing down from the arena top instead:
 * sized on demand by the textures actually allocated, first-fit with
 * coalescing, shrinking when its lowest block is freed.  The arena's usable
 * size (gc_mapload_buf_size, which every map-load user honours) is the
 * region's low edge.  It only grows into space the world-array gap map says
 * is free, while a map is settled; otherwise callers keep using the heap.
 */
typedef struct gc_texreg_blk_s
{
	uint32_t size;	/* whole block, header included, multiple of 32 */
	uint32_t used;
	uint32_t pad[6];
} gc_texreg_blk_t;

static byte  *gc_texreg_base;	/* arena base (static renderer arena) */
static size_t gc_texreg_cap;	/* full arena capacity */
static size_t gc_texreg_bytes;	/* region size, from the top */
static size_t gc_texreg_live;

/* Called when the static arena is borrowed: remember its geometry and
 * return the size a map load may use. */
size_t GC_TexRegionNoteArena( byte *base, size_t capacity )
{
	gc_texreg_base = base;
	gc_texreg_cap = capacity;
	if( gc_texreg_bytes > capacity )
		gc_texreg_bytes = 0;
	return capacity - gc_texreg_bytes;
}

qboolean GC_TexRegionOwns( const void *p )
{
	const byte *b = (const byte *)p;

	return p && gc_texreg_bytes && b >= gc_texreg_base + gc_texreg_cap - gc_texreg_bytes
		&& b < gc_texreg_base + gc_texreg_cap;
}

void *GC_TexRegionAlloc( size_t bytes )
{
	extern size_t Mod_GCArenaGap( size_t from, size_t *gap_end );
	const size_t need = (( bytes + 31u ) & ~(size_t)31u ) + sizeof( gc_texreg_blk_t );
	byte *low, *top;
	gc_texreg_blk_t *h;

	if( !gc_texreg_base || !bytes || need > 1024u * 1024u )
		return NULL;
	top = gc_texreg_base + gc_texreg_cap;
	low = top - gc_texreg_bytes;
	/* First fit among freed blocks. */
	for( h = (gc_texreg_blk_t *)low; (byte *)h < top; h = (gc_texreg_blk_t *)((byte *)h + h->size ))
	{
		if( h->used || h->size < need )
			continue;
		if( h->size - need >= sizeof( gc_texreg_blk_t ) + 32u )
		{
			gc_texreg_blk_t *rest = (gc_texreg_blk_t *)((byte *)h + need );

			rest->size = h->size - (uint32_t)need;
			rest->used = 0;
			h->size = (uint32_t)need;
		}
		h->used = 1;
		gc_texreg_live += h->size;
		memset( h + 1, 0, h->size - sizeof( *h ));
		return h + 1;
	}
	/* Grow down into free arena space below the region. */
	{
		const size_t new_low = gc_texreg_cap - gc_texreg_bytes - need;
		size_t gap_end = 0;

		if( gc_texreg_cap - gc_texreg_bytes < need || !gc_mapload_world_settled
			|| gc_mapload_buf != gc_texreg_base
			|| new_low < Q_max( gc_mapload_tail_highwater, gc_mapload_tail_peak )
			|| Mod_GCArenaGap( new_low, &gap_end ) != new_low
			|| gap_end < gc_texreg_cap - gc_texreg_bytes )
			return NULL;
		gc_texreg_bytes += need;
		gc_mapload_buf_size = gc_texreg_cap - gc_texreg_bytes;
		h = (gc_texreg_blk_t *)( gc_texreg_base + new_low );
		h->size = (uint32_t)need;
		h->used = 1;
		gc_texreg_live += need;
		memset( h + 1, 0, need - sizeof( *h ));
		return h + 1;
	}
}

qboolean GC_TexRegionFree( void *p )
{
	gc_texreg_blk_t *h, *n;
	byte *top;

	if( !GC_TexRegionOwns( p ))
		return false;
	top = gc_texreg_base + gc_texreg_cap;
	h = (gc_texreg_blk_t *)p - 1;
	h->used = 0;
	gc_texreg_live -= Q_min( gc_texreg_live, (size_t)h->size );
	/* Coalesce every free run, then give a free bottom back to the maps. */
	for( h = (gc_texreg_blk_t *)( top - gc_texreg_bytes ); (byte *)h < top; h = (gc_texreg_blk_t *)((byte *)h + h->size ))
	{
		while( !h->used && (byte *)h + h->size < top
			&& !( n = (gc_texreg_blk_t *)((byte *)h + h->size ))->used )
			h->size += n->size;
	}
	h = (gc_texreg_blk_t *)( top - gc_texreg_bytes );
	if( gc_texreg_bytes && !h->used )
	{
		gc_texreg_bytes -= h->size;
		if( gc_mapload_buf == gc_texreg_base )
			gc_mapload_buf_size = gc_texreg_cap - gc_texreg_bytes;
	}
	return true;
}

void GC_TexRegionReport( const char *stage )
{
	if( gc_texreg_base )
		Con_Reportf( "Xash3D GameCube: budget texture region at=%s bytes=%s live=%s arena_usable=%s\n",
			stage, Q_memprint( gc_texreg_bytes ), Q_memprint( gc_texreg_live ),
			Q_memprint( gc_texreg_cap - gc_texreg_bytes ));
}

/* Offset of a free arena gap that fits `bytes` for a post-load claim (entity
 * overflow, packet rings, client entity table), or (size_t)-1. A lean load
 * lowers the highwater to 0 and leaves world arrays both near the bottom
 * (in-place planes) and in a high slot near the top (leafs/nodes); the gap
 * finder in mod_bmodel.c knows every one of them. */
size_t GC_MapArenaFind( size_t bytes )
{
	extern size_t Mod_GCArenaGap( size_t from, size_t *gap_end );
	size_t from = gc_mapload_tail_highwater;

	if( from < gc_mapload_tail_peak )
		from = gc_mapload_tail_peak;
	while( gc_mapload_buf && from < gc_mapload_buf_size )
	{
		size_t end;
		const size_t start = ( Mod_GCArenaGap( from, &end ) + 31u ) & ~(size_t)31u;

		if( start + bytes <= end )
			return start;
		if( end <= from )
			break;
		from = end;
	}
	return (size_t)-1;
}

/* Bytes in the largest free gap left above the tail, for budget logs. */
size_t GC_MapArenaLargestGap( void )
{
	extern size_t Mod_GCArenaGap( size_t from, size_t *gap_end );
	size_t from = Q_max( gc_mapload_tail_highwater, gc_mapload_tail_peak ), best = 0;

	while( gc_mapload_buf && from < gc_mapload_buf_size )
	{
		size_t end;
		const size_t start = Mod_GCArenaGap( from, &end );

		if( end > start && end - start > best )
			best = end - start;
		if( end <= from )
			break;
		from = end;
	}
	return best;
}

qboolean GC_IsMapLoadBufferAddress( const void *ptr )
{
	const uintptr_t address = (uintptr_t)ptr;
	const uintptr_t base = (uintptr_t)gc_mapload_buf;

	return ptr != NULL && gc_mapload_buf != NULL
		&& address >= base && address - base < gc_mapload_buf_size;
}

size_t GC_MapLoadBufferCapacity( void )
{
	return gc_mapload_buf_size;
}

/* End of the last tail carve: the next carve must start at or above it. */
size_t GC_MapLoadBufferTailHighwater( void )
{
	return gc_mapload_tail_highwater;
}

/* Address the next tail carve would start at (before its 32-byte alignment). */
void *GC_MapLoadBufferTailEnd( void )
{
	return gc_mapload_buf ? gc_mapload_buf + gc_mapload_tail_highwater : NULL;
}

size_t GC_MapLoadBufferTailPeak( void )
{
	return gc_mapload_tail_peak;
}

void GC_MapLoadBufferLowerTailHighwater( size_t highwater )
{
	if( highwater < gc_mapload_tail_highwater )
		gc_mapload_tail_highwater = highwater;
}

void *GC_MapLoadBufferTailAlloc( size_t after_offset, size_t need )
{
	size_t off;
	size_t remaining;
	uintptr_t address, aligned, base;

	/* A live entity overflow reservation pins this map's arena after the BSP
	 * staging borrow ends. Its high-water mark also protects all earlier tail
	 * allocations, so remaining tail space is still safe for world arrays. */
	if( !gc_mapload_buf || ( !gc_mapload_buf_in_use && !SV_GCPrivateOverflowPinsMapArena() ) || need == 0 )
		return NULL;

	base = (uintptr_t)gc_mapload_buf;
	address = base + after_offset;
	if( address < base || address > UINTPTR_MAX - 31u )
		return NULL;
	aligned = ( address + 31u ) & ~(uintptr_t)31u;
	off = (size_t)( aligned - base );
	if( off + need < off )
		return NULL;
	if( off < gc_mapload_tail_highwater )
	{
		Con_Reportf( "Xash3D GameCube: mem arena tail=OVERLAP after_bytes=%lu offset_bytes=%lu highwater_bytes=%lu\n",
			(unsigned long)after_offset, (unsigned long)off,
			(unsigned long)gc_mapload_tail_highwater );
		return NULL;
	}
	/* After load, a lean world keeps arrays above the highwater (leafs/nodes
	 * high slot, planes, clipnodes), and gap-placed claims (entity text,
	 * studio slices, packet ring) can raise the highwater into the middle of
	 * them.  A carve made "at the highwater" then landed on the world (c1a4k:
	 * a 768 KiB carve after the entity text move hit planes, nodes and
	 * clipnodes).  Every post-load carve goes to the first gap at or after
	 * the requested offset that fits, or fails cleanly. */
	if( gc_mapload_world_settled )
	{
		extern size_t Mod_GCArenaGap( size_t from, size_t *gap_end );
		size_t from = off;

		for( ;; )
		{
			size_t gap_end = 0;
			const size_t start = ( Mod_GCArenaGap( from, &gap_end ) + 31u ) & ~(size_t)31u;

			if( start + need <= gap_end && start + need <= gc_mapload_buf_size )
			{
				if( start != off )
					Con_Reportf( "Xash3D GameCube: mem arena tail=MOVED offset_bytes=%lu -> %lu need_bytes=%lu (world data in the way)\n",
						(unsigned long)off, (unsigned long)start, (unsigned long)need );
				off = start;
				break;
			}
			if( gap_end <= from || gap_end >= gc_mapload_buf_size )
			{
				Con_Reportf( "Xash3D GameCube: mem arena tail=FAIL after_bytes=%lu need_bytes=%lu (no free gap after the world)\n",
					(unsigned long)after_offset, (unsigned long)need );
				return NULL;
			}
			from = gap_end;
		}
	}
	if( off + need > gc_mapload_buf_size )
	{
		Con_Reportf( "Xash3D GameCube: mem arena tail=FAIL after_bytes=%lu need_bytes=%lu capacity_bytes=%lu\n",
			(unsigned long)after_offset, (unsigned long)need, (unsigned long)gc_mapload_buf_size );
		return NULL;
	}

	remaining = gc_mapload_buf_size - ( off + need );
	gc_mapload_tail_highwater = off + need;
	if( gc_mapload_tail_peak < off + need )
		gc_mapload_tail_peak = off + need;
	Con_Reportf( "Xash3D GameCube: mem arena tail=PASS after_bytes=%lu carve_bytes=%lu remaining_bytes=%lu capacity_bytes=%lu\n",
		(unsigned long)after_offset, (unsigned long)need, (unsigned long)remaining,
		(unsigned long)gc_mapload_buf_size );

	return gc_mapload_buf + off;
}

void GC_DiscardMapLoadBuffer( void )
{
	if( SV_GCPrivateOverflowPinsMapArena() )
	{
		Con_Reportf( "Xash3D GameCube: retaining map arena for entity private data\n" );
		return;
	}
	if( gc_mapload_buf_in_use )
		return;

	if( gc_mapload_buf )
	{
		if( R_GCReleaseMapLoadStaticArena( gc_mapload_buf ))
		{
			gc_mapload_buf = NULL;
			gc_mapload_buf_size = 0;
			return;
		}

		Con_Reportf( "Xash3D GameCube: map-load buffer discarded %s\n",
			Q_memprint( gc_mapload_buf_size ));
		free( gc_mapload_buf );
		gc_mapload_buf = NULL;
		gc_mapload_buf_size = 0;
	}
}

qboolean Mod_GCEnsureStaticReserves( void );
qboolean SV_GCEnsureStringArena( void );

void GC_BeginMapLoadMemoryOpt( void )
{
	/* First map load: take the session's world reserves before BSP staging. */
	Mod_GCEnsureStaticReserves();
	SV_GCEnsureStringArena();
	gc_mapload_memopt_depth++;
	gc_mapload_memopt_session = true;
	gc_newgame_bootstrap_memopt = false;
}

void GC_EndMapLoadMemoryOpt( void )
{
	if( gc_mapload_memopt_depth > 0 )
		gc_mapload_memopt_depth--;
}

void GC_ClearMapLoadMemoryOpt( void )
{
	gc_mapload_memopt_depth = 0;
	gc_mapload_memopt_session = false;
	gc_newgame_bootstrap_memopt = false;
	/* Jobs may retain pointers into the outgoing map arena.  Drop them before
	 * releasing/resetting map-owned storage; future streaming producers can
	 * safely enqueue work again on the next map. */
	gc_job_count = 0;
	GC_AramMapReset();
	GC_PrefetchResetForMenu();
}

/* A local reconnect (same-map changelevel on the listen route) goes through
 * CL_Disconnect, which clears the session flag; the reload must keep the lean
 * map route it had. */
void GC_SetMapLoadMemoryOptSession( qboolean on )
{
	gc_mapload_memopt_session = on;
}

qboolean GC_MapLoadMemoryOpt( void )
{
	return gc_mapload_memopt_session
		|| gc_mapload_memopt_depth > 0
		|| Sys_CheckParm( "-gcmap" ) != 0
		|| ( Sys_CheckParm( "-gcnewgame" ) != 0 && gc_newgame_bootstrap_memopt );
}

/*
==============================================================================

ARAM storage helpers

The 16 MiB of auxiliary RAM is DMA-only. These move arbitrary byte ranges
through a 32-byte aligned MEM1 bounce buffer, so callers (the transition
save bank) never need a MEM1 copy of a whole file.

==============================================================================
*/
#define GC_ARAM_BOUNCE 4096
static u8 gc_aram_bounce[GC_ARAM_BOUNCE] __attribute__(( aligned( 32 )));
/* libogc's AR stack is only used once, to hand the whole pool to the
 * allocator in aram_core.c; every engine owner allocates from that. */
static u32 gc_aram_reserve_blocks[4];
static gc_aram_heap_t gc_aram_heap;
static gc_aram_cache_t gc_aram_cache;
static qboolean gc_aram_manager_tried;
static u32 gc_aram_audio_base;
static u32 gc_aram_audio_capacity;
static u32 gc_aram_video_base;
static u32 gc_aram_video_capacity;
static u32 gc_aram_map_base;
static u32 gc_aram_map_capacity;

static void GC_AramDMA( u32 type, u32 aram, u32 len )
{
	if( type == AR_MRAMTOARAM )
		DCFlushRange( gc_aram_bounce, len );
	else
		DCInvalidateRange( gc_aram_bounce, len );
	while( AR_GetDMAStatus( ))
		;
	AR_StartDMA( type, (u32)gc_aram_bounce, aram, len );
	while( AR_GetDMAStatus( ))
		;
	if( type == AR_ARAMTOMRAM )
		DCInvalidateRange( gc_aram_bounce, len );
}

qboolean GC_StageMapHotLumps( const void *bsp, size_t bsp_size )
{
	unsigned int total = 0, reserve, cursor = 0, base;
	int i;

	if( !bsp || !bsp_size || !gc_bsp_batches_valid ) return false;
	for( i = 0; i < 15; i++ )
	{
		if( !gc_bsp_lump_hot[i] || gc_bsp_lump_size[i] <= 0 ) continue;
		total = ( total + 31u ) & ~31u;
		total += (unsigned int)gc_bsp_lump_size[i];
	}
	/* Reserve headroom on the first map so the same session window can hold
	 * the next tram-chain BSP's slightly larger hot-lump set without asking
	 * the non-growing ARAM allocator for a second block mid-transition. */
	reserve = total + 128u * 1024u;
	if( reserve < total ) return false;
	if( reserve < 1024u * 1024u ) reserve = 1024u * 1024u;
	base = GC_AramMapReserve( reserve );
	if( !base || total > GC_AramMapCapacity() ) return false;
	for( i = 0; i < 15; i++ )
	{
		unsigned int size;
		if( !gc_bsp_lump_hot[i] || gc_bsp_lump_size[i] <= 0 ) continue;
		if( gc_bsp_lump_offset[i] < 0
			|| (size_t)gc_bsp_lump_offset[i] + (size_t)gc_bsp_lump_size[i] > bsp_size )
			return false;
		size = (unsigned int)gc_bsp_lump_size[i];
		GC_AramCopy( base + cursor, (byte *)bsp + gc_bsp_lump_offset[i], size, true );
		gc_bsp_lump_aram[i] = base + cursor;
		cursor = ( cursor + size + 31u ) & ~31u;
	}
	gc_bsp_hot_aram = base;
	gc_bsp_hot_bytes = cursor;
	Con_Reportf( "Xash3D GameCube: staged BSP HOT bytes=%s aram=0x%08x\n",
		Q_memprint( cursor ), base );
	return true;
}

qboolean GC_MapHotLumpRead( int lump, size_t offset, void *dst, size_t size )
{
	if( lump < 0 || lump >= 15 || !dst || !size || !gc_bsp_lump_aram[lump]
		|| !gc_bsp_lump_hot[lump] || offset > (size_t)gc_bsp_lump_size[lump]
		|| size > (size_t)gc_bsp_lump_size[lump] - offset )
		return false;
	GC_AramCopy( gc_bsp_lump_aram[lump] + (unsigned int)offset, dst,
		(unsigned int)size, false );
	return true;
}

qboolean GC_MapHotLumpReadPointer( const void *source, void *dst, size_t size )
{
	uintptr_t p = (uintptr_t)source, base = (uintptr_t)gc_mapload_buf;
	int i;
	if( !source || !dst || !size || !base || p < base || p >= base + gc_mapload_buf_size )
		return false;
	for( i = 0; i < 15; i++ )
	{
		uintptr_t start;
		if( !gc_bsp_lump_hot[i] || !gc_bsp_lump_aram[i] ) continue;
		start = base + (uintptr_t)gc_bsp_lump_offset[i];
		if( p >= start && p - start <= (uintptr_t)gc_bsp_lump_size[i]
			&& size <= (size_t)gc_bsp_lump_size[i] - (size_t)( p - start ))
			return GC_MapHotLumpRead( i, (size_t)( p - start ), dst, size );
	}
	return false;
}

static void GC_AramCacheMove( uint32_t aram, void *buf, uint32_t len, int write )
{
	GC_AramCopy( aram, buf, len, write ? true : false );
}

/* Take every byte of ARAM above whatever the AR stack already holds (the DSP
 * reserves the first 16 KiB) and manage it here. */
static qboolean GC_AramManagerInit( void )
{
	u32 base, top, pool;

	if( gc_aram_heap.count )
		return true;
	if( gc_aram_manager_tried )
		return false;
	gc_aram_manager_tried = true;
	if( !AR_CheckInit( ))
		AR_Init( gc_aram_reserve_blocks, sizeof( gc_aram_reserve_blocks ) / sizeof( gc_aram_reserve_blocks[0] ));
	top = AR_GetSize( );
	/* AR_Alloc returns the current stack pointer; probe it without keeping
	 * the block, then claim everything above it in one block. */
	base = AR_Alloc( GC_ARAM_ALIGN );
	AR_Free( NULL );
	base = ( base + GC_ARAM_ALIGN - 1u ) & ~( GC_ARAM_ALIGN - 1u );
	if( !top || base >= top )
	{
		Con_Reportf( S_ERROR "Xash3D GameCube: ARAM manager has no pool base=0x%08x top=0x%08x\n", base, top );
		return false;
	}
	pool = AR_Alloc( top - base );
	GC_AramHeapInit( &gc_aram_heap, pool, top - base );
	/* The asset cache only borrows free stream space: session owners evict it
	 * on demand (GC_AramAllocTagged), so its budget is a ceiling, not a claim. */
	GC_AramCacheInit( &gc_aram_cache, &gc_aram_heap, GC_AramCacheMove, GC_ARAM_ASSET_CACHE_BYTES );
	Con_Reportf( "Xash3D GameCube: ARAM manager pool %s at 0x%08x (cache ceiling %s)\n",
		Q_memprint( top - base ), pool, Q_memprint( GC_ARAM_ASSET_CACHE_BYTES ));
	return gc_aram_heap.count > 0;
}

unsigned int GC_AramAllocTagged( unsigned int size, qboolean stream, const char *tag )
{
	u32 base;

	if( !size || !GC_AramManagerInit( ))
		return 0;
	for( ;; )
	{
		base = GC_AramHeapAlloc( &gc_aram_heap, size, stream ? GC_ARAM_STREAM : GC_ARAM_SESSION, tag );
		if( base )
			break;
		/* Cached assets are always re-readable from disc: give their space to
		 * an owner that cannot be. */
		if( !GC_AramCacheEvictLRU( &gc_aram_cache ))
			break;
	}
	if( base )
		Con_Reportf( "Xash3D GameCube: ARAM %s %s %s at 0x%08x free=%s\n",
			stream ? "stream" : "reserve", tag ? tag : "?", Q_memprint( size ), base,
			Q_memprint( GC_AramHeapFreeBytes( &gc_aram_heap )));
	else
	{
		int i;

		Con_Reportf( S_WARN "Xash3D GameCube: ARAM %s refused %s need=%s free=%s largest=%s segs=%d/%d\n",
			tag ? tag : "?", stream ? "stream" : "reserve", Q_memprint( size ),
			Q_memprint( GC_AramHeapFreeBytes( &gc_aram_heap )),
			Q_memprint( GC_AramHeapLargestFree( &gc_aram_heap )), (int)gc_aram_heap.count, GC_ARAM_MAX_SEGS );
		/* Say what fragments it: the whole segment table, once per refusal. */
		for( i = 0; i < gc_aram_heap.count; i++ )
			Con_Reportf( "Xash3D GameCube: ARAM map 0x%08x %s %s%s\n", gc_aram_heap.segs[i].offset,
				Q_memprint( gc_aram_heap.segs[i].size ), gc_aram_heap.segs[i].used ? gc_aram_heap.segs[i].tag : "FREE",
				gc_aram_heap.segs[i].used ? ( gc_aram_heap.segs[i].cls == GC_ARAM_STREAM ? " (stream)" : " (session)" ) : "" );
	}
	return base;
}

qboolean GC_AramFree( unsigned int base )
{
	u32 size;

	if( !base || !gc_aram_heap.count )
		return false;
	if( base == gc_aram_audio_base || base == gc_aram_map_base || base == gc_aram_video_base )
	{
		Con_Reportf( S_WARN "Xash3D GameCube: ARAM free of owned window 0x%08x refused\n", base );
		return false;
	}
	/* A DMA may still be reading or writing the range. */
	while( AR_GetDMAStatus( ))
		;
	size = GC_AramHeapBlockSize( &gc_aram_heap, base );
	if( !GC_AramHeapFree( &gc_aram_heap, base ))
		return false;
	Con_Reportf( "Xash3D GameCube: ARAM free %s at 0x%08x\n", Q_memprint( size ), base );
	return true;
}

unsigned int GC_AramReserveTagged( unsigned int size, const char *tag )
{
	return GC_AramAllocTagged( size, false, tag );
}

unsigned int GC_AramReserve( unsigned int size )
{
	return GC_AramAllocTagged( size, false, "legacy" );
}

unsigned int GC_AramAudioReserve( unsigned int size )
{
	u32 aligned;

	if( gc_aram_audio_base )
		return gc_aram_audio_capacity >= size ? gc_aram_audio_base : 0;
	aligned = ( size + 31u ) & ~31u;
	if( !aligned )
		return 0;
	gc_aram_audio_base = GC_AramAllocTagged( aligned, false, "audio" );
	if( !gc_aram_audio_base )
		return 0;
	gc_aram_audio_capacity = aligned;
	return gc_aram_audio_base;
}

unsigned int GC_AramAudioBase( void )
{
	return gc_aram_audio_base;
}

unsigned int GC_AramAudioCapacity( void )
{
	return gc_aram_audio_capacity;
}

unsigned int GC_AramVideoReserve( unsigned int size )
{
	u32 aligned = ( size + 31u ) & ~31u;
	if( !aligned ) return 0;
	if( gc_aram_video_base )
		return gc_aram_video_capacity >= aligned ? gc_aram_video_base : 0;
	gc_aram_video_base = GC_AramAllocTagged( aligned, true, "video" );
	if( gc_aram_video_base ) gc_aram_video_capacity = aligned;
	return gc_aram_video_base;
}

void GC_AramVideoRelease( void )
{
	u32 base = gc_aram_video_base;

	if( !base ) return;
	/* The video window is a stream block now: it can be returned whatever was
	 * allocated after it, instead of having to be the AR stack's top block. */
	gc_aram_video_base = 0;
	gc_aram_video_capacity = 0;
	GC_AramFree( base );
}

/* World DL packs get their own growable stream block.  They used to share
 * the map window with the BSP hot lumps: on the retail disc the hot lumps
 * claimed it first (1 MiB), the 1.4 MiB c0a0 pack fell back to the disc
 * path and the first frame stalled on a GX fence. */
static u32 gc_aram_worlddl_base, gc_aram_worlddl_capacity;

unsigned int GC_AramWorldDLReserve( unsigned int size )
{
	u32 aligned = ( size + 31u ) & ~31u;

	if( !aligned ) return 0;
	if( gc_aram_worlddl_base && aligned <= gc_aram_worlddl_capacity )
		return gc_aram_worlddl_base;
	if( gc_aram_worlddl_base )
	{
		GC_AramFree( gc_aram_worlddl_base );
		gc_aram_worlddl_base = 0;
		gc_aram_worlddl_capacity = 0;
	}
	/* Round up so the tram chain's slightly larger packs reuse the block.
	 * Lost Coast is one map: keep the 32-byte DMA align only, so a 2.3 MB
	 * pack does not pay another ~200–700 KiB of empty ARAM (stream=3700 Kb
	 * on an older 3.5 MB bake; today's file is 2.36 MB). */
#if !( defined(XASH_GC_LOSTCOAST) && XASH_GC_LOSTCOAST )
	aligned = ( aligned + 256u * 1024u - 1u ) & ~( 256u * 1024u - 1u );
#endif
	gc_aram_worlddl_base = GC_AramAllocTagged( aligned, true, "worlddl" );
	if( gc_aram_worlddl_base ) gc_aram_worlddl_capacity = aligned;
	return gc_aram_worlddl_base;
}

unsigned int GC_AramMapReserve( unsigned int size )
{
	u32 aligned = ( size + 31u ) & ~31u;
	if( !aligned ) return 0;
	if( gc_aram_map_base && aligned <= gc_aram_map_capacity )
		return gc_aram_map_base;
	if( gc_aram_map_base )
	{
		Con_Reportf( S_WARN "Xash3D GameCube: map ARAM window too small need=%s have=%s\n",
			Q_memprint( aligned ), Q_memprint( gc_aram_map_capacity ));
		return 0;
	}
	gc_aram_map_base = GC_AramAllocTagged( aligned, false, "mapwindow" );
	if( gc_aram_map_base ) gc_aram_map_capacity = aligned;
	return gc_aram_map_base;
}

void GC_AramMapReset( void )
{
	/* Ownership is retained; callers can overwrite the window for the next
	 * map without asking for another block. */
}

unsigned int GC_AramMapCapacity( void )
{
	return gc_aram_map_capacity;
}

unsigned int GC_AramMapBase( void )
{
	return gc_aram_map_base;
}

/*
==============================================================================

ARAM asset cache: whole files (studio models, sequence groups) kept in free
stream space so a reload after an arena reset, a team pick or a changelevel
is a DMA instead of a disc read.

==============================================================================
*/
gc_aram_cache_t *GC_AramAssetCacheGet( void )
{
	return GC_AramManagerInit() ? &gc_aram_cache : NULL;
}

qboolean GC_AssetCacheLookup( const char *path, unsigned int *size )
{
	int index;

	if( size )
		*size = 0;
	if( !path || !GC_AramManagerInit( ))
		return false;
	index = GC_AramCacheFind( &gc_aram_cache, path );
	if( index < 0 )
		return false;
	if( size )
		*size = GC_AramCacheSize( &gc_aram_cache, index );
	return true;
}

qboolean GC_AssetCacheRead( const char *path, void *dst, unsigned int size )
{
	int index;

	if( !path || !dst || !size || !GC_AramManagerInit( ))
		return false;
	index = GC_AramCacheFind( &gc_aram_cache, path );
	if( index < 0 || GC_AramCacheSize( &gc_aram_cache, index ) != size )
		return false;
	return GC_AramCacheRead( &gc_aram_cache, index, 0, dst, size ) ? true : false;
}

qboolean GC_AssetCacheStore( const char *path, const void *data, unsigned int size )
{
	if( !path || !data || !size || !GC_AramManagerInit( ))
		return false;
	if( GC_AramCacheStore( &gc_aram_cache, path, data, size ) < 0 )
	{
		Con_Reportf( "Xash3D GameCube: asset cache refused '%s' (%s)\n", path, Q_memprint( size ));
		return false;
	}
	return true;
}

void GC_AramReport( const char *stage )
{
	if( !gc_aram_heap.count )
		return;
	Con_Reportf( "Xash3D GameCube: ARAM %s session=%s stream=%s free=%s largest=%s peak=%s refused=%u"
		" cache=%s/%s hits=%u misses=%u stores=%u evictions=%u\n",
		stage ? stage : "-", Q_memprint( gc_aram_heap.session_bytes ),
		Q_memprint( gc_aram_heap.stream_bytes ), Q_memprint( GC_AramHeapFreeBytes( &gc_aram_heap )),
		Q_memprint( GC_AramHeapLargestFree( &gc_aram_heap )), Q_memprint( gc_aram_heap.peak_bytes ),
		gc_aram_heap.failures, Q_memprint( gc_aram_cache.bytes ), Q_memprint( gc_aram_cache.budget ),
		gc_aram_cache.hits, gc_aram_cache.misses, gc_aram_cache.stores, gc_aram_cache.evictions );
}

/* Whole 32-byte aligned blocks, straight to or from the buffer. */
void GC_AramTransfer( unsigned int aram, void *buf, unsigned int len, qboolean write )
{
	if( write )
		DCFlushRange( buf, len );
	else
		DCInvalidateRange( buf, len );
	while( AR_GetDMAStatus( ))
		;
	AR_StartDMA( write ? AR_MRAMTOARAM : AR_ARAMTOMRAM, (u32)buf, aram, len );
	while( AR_GetDMAStatus( ))
		;
	if( !write )
		DCInvalidateRange( buf, len );
}

void GC_AramCopy( unsigned int aram, void *buf, unsigned int len, qboolean write )
{
	byte *p = (byte *)buf;

	while( len > 0 )
	{
		const u32 a0 = aram & ~31u;
		const u32 off = aram - a0;
		const u32 chunk = Q_min( len, GC_ARAM_BOUNCE - off );
		const u32 span = ( off + chunk + 31u ) & ~31u;

		if( write )
		{
			/* Partial 32-byte blocks keep their neighbouring bytes. */
			if( off || ( chunk & 31u ))
				GC_AramDMA( AR_ARAMTOMRAM, a0, span );
			memcpy( gc_aram_bounce + off, p, chunk );
			GC_AramDMA( AR_MRAMTOARAM, a0, span );
		}
		else
		{
			GC_AramDMA( AR_ARAMTOMRAM, a0, span );
			memcpy( p, gc_aram_bounce + off, chunk );
		}
		aram += chunk;
		p += chunk;
		len -= chunk;
	}
}

void GC_PrefetchMenuIdle( void )
{
	file_t *f;
	char manifest[8192];
	int n;
	if( gc_prefetch_manifest_ready || cls.key_dest != key_menu || cls.state == ca_cinematic
		|| SV_Active() || Sys_CheckParm( "-gcmap" )) return;
	if( !gc_prefetch_initialized )
	{
		gc_prefetch_initialized = true;
		f = FS_Open( "media/c0a0.prefetch", "rb", false );
		if( !f ) { gc_prefetch_manifest_ready = true; return; }
			n = FS_Read( f, manifest, sizeof( manifest ) - 1 ); FS_Close( f );
		if( n <= 0 ) { gc_prefetch_manifest_ready = true; return; }
		manifest[n] = '\0';
		{
			char *cursor = manifest;
			while( gc_prefetch_count < GC_PREFETCH_MAX_ENTRIES && cursor && *cursor )
			{
				char *end = strchr( cursor, '\n' );
				if( end ) *end = '\0';
				if( cursor[0] != '#' && cursor[0] != '\0'
					&& sscanf( cursor, "%127s %u", gc_prefetch[gc_prefetch_count].path,
						&gc_prefetch[gc_prefetch_count].size ) == 2 ) gc_prefetch_count++;
				if( !end ) break; cursor = end + 1;
			}
		}
		if( !gc_prefetch_count ) { gc_prefetch_manifest_ready = true; return; }
		{
			unsigned int reserve = 0;
			unsigned int i;
			for( i = 0; i < gc_prefetch_count; i++ )
			{
				if( gc_prefetch[i].size <= 512u * 1024u
					&& gc_prefetch[i].size <= GC_PREFETCH_MAX_BYTES - reserve )
					reserve += ( gc_prefetch[i].size + 31u ) & ~31u;
			}
			if( !reserve ) { gc_prefetch_manifest_ready = true; return; }
			if( reserve > GC_PREFETCH_MAX_BYTES ) reserve = GC_PREFETCH_MAX_BYTES;
			if( !gc_prefetch_aram )
			{
				gc_prefetch_aram = GC_AramReserveTagged( reserve, "prefetch" );
				if( gc_prefetch_aram ) gc_prefetch_capacity = reserve;
			}
			if( !gc_prefetch_aram ) { gc_prefetch_manifest_ready = true; return; }
		}
	}
	if( gc_prefetch_next >= gc_prefetch_count ) { gc_prefetch_manifest_ready = true; return; }
	{
		gc_prefetch_entry_t *entry = &gc_prefetch[gc_prefetch_next++];
		unsigned int left, offset = 0;
		if( !entry->size || entry->size > 512u * 1024u
			|| gc_prefetch_used > gc_prefetch_capacity
			|| (( entry->size + 31u ) & ~31u ) > gc_prefetch_capacity - gc_prefetch_used ) return;
		f = FS_Open( entry->path, "rb", false ); if( !f ) return;
		entry->aram = gc_prefetch_aram + gc_prefetch_used; left = entry->size;
		while( left )
		{
			unsigned int chunk = left > GC_PREFETCH_CHUNK ? GC_PREFETCH_CHUNK : left;
			if( FS_Read( f, gc_prefetch_bounce, chunk ) != (int)chunk ) break;
			/* File tails are commonly not DMA-sized; AramCopy preserves the
			 * neighbouring bytes in the final 32-byte block. */
			GC_AramCopy( entry->aram + offset, gc_prefetch_bounce, chunk, true );
			offset += chunk; left -= chunk;
		}
		FS_Close( f );
		if( !left )
		{
			entry->ready = true;
			gc_prefetch_used += ( entry->size + 31u ) & ~31u;
		}
	}
}

void GC_PrefetchResetForMenu( void )
{
	/* AR_Free is intentionally not used here: other subsystems may have
	 * allocated a block after the prefetch window. The window is a permanent
	 * session owner and is reset in place, which gives transitions deterministic
	 * reuse without ARAM fragmentation or stale reads. */
	memset( gc_prefetch, 0, sizeof( gc_prefetch ));
	gc_prefetch_count = 0;
	gc_prefetch_next = 0;
	gc_prefetch_used = 0;
	gc_prefetch_manifest_ready = false;
	gc_prefetch_initialized = false;
}

/* The menu-idle first-map prefetch window is dead weight once the map is loaded (1 MiB of ARAM
 * in a DoD match). Drop its entries first so no reader can see freed ARAM, then free the block;
 * the next menu idle reserves a fresh one (GC_PrefetchResetForMenu clears the init flag). */
void GC_PrefetchReleaseForMatch( void )
{
	unsigned int base = gc_prefetch_aram;

	if( !base )
		return;
	memset( gc_prefetch, 0, sizeof( gc_prefetch ));
	gc_prefetch_count = 0;
	gc_prefetch_next = 0;
	gc_prefetch_used = 0;
	gc_prefetch_capacity = 0;
	gc_prefetch_manifest_ready = true;
	gc_prefetch_aram = 0;
	GC_AramFree( base );
}

void GC_PrefetchStats( unsigned int *used, unsigned int *capacity, unsigned int *entries )
{
	if( used ) *used = gc_prefetch_used;
	if( capacity ) *capacity = gc_prefetch_capacity;
	if( entries ) *entries = gc_prefetch_next;
}

qboolean GC_PrefetchRead( const char *path, void *buffer, unsigned int size )
{
	unsigned int i;
	if( !path || !buffer || !size ) return false;
	for( i = 0; i < gc_prefetch_count; i++ )
	{
		gc_prefetch_entry_t *entry = &gc_prefetch[i];
		if( entry->ready && entry->size == size && !Q_stricmp( entry->path, path ))
		{
			GC_AramCopy( entry->aram, buffer, size, false );
			return true;
		}
	}
	return false;
}

qboolean GC_PrefetchContains( const char *path, unsigned int size )
{
	unsigned int i;
	if( !path || !size ) return false;
	for( i = 0; i < gc_prefetch_count; i++ )
	{
		const gc_prefetch_entry_t *entry = &gc_prefetch[i];
		if( entry->ready && entry->size == size && !Q_stricmp( entry->path, path ))
			return true;
	}
	return false;
}
#endif
