/*
malloc_trace.c - labelled boot malloc trace (debug builds only)

Built when XASH_GC_MALLOC_TRACE=1 is set for the GameCube build. The link wraps malloc,
calloc, memalign and realloc (-Wl,--wrap=...) and logs every request of GC_MALLOC_TRACE_MIN
bytes or more with the caller's return address and its caller's:

  Xash3D GameCube: malloc-trace malloc size=196608 ret=0x80123456 up=0x80234567 ok total=1228800

The allocator wrappers only RECORD into a static table: no I/O, no allocation, nothing that
can block this early in boot (a build that allocates 8 KiB before the SYS/report layer is up used
to hang here when the wrapper called SYS_Report itself). GC_MallocTraceFlush() prints the new
entries and is called from GC_MemSample, a stage marker that runs once the engine is up.

Resolve the addresses against the same ELF (powerpc-eabi-addr2line -f -e xash-dod ADDR) to
name the owner of each large block. Nothing is compiled or linked without the flag.
*/
#include <stddef.h>
#include <stdint.h>

#ifdef XASH_GC_MALLOC_TRACE
#include <ogc/system.h>

#ifndef GC_MALLOC_TRACE_MIN
#define GC_MALLOC_TRACE_MIN ( 8u * 1024u )
#endif
#define GC_MALLOC_TRACE_ENTRIES 512

extern void *__real_malloc( size_t size );
extern void *__real_calloc( size_t nmemb, size_t size );
extern void *__real_memalign( size_t align, size_t size );
extern void *__real_realloc( void *ptr, size_t size );

typedef struct
{
	const char *what;
	unsigned size;
	void *ret, *up;
	unsigned total;
	unsigned char ok;
} gc_trace_entry_t;

static gc_trace_entry_t gc_trace[GC_MALLOC_TRACE_ENTRIES];
static unsigned gc_trace_count, gc_trace_flushed, gc_trace_dropped, gc_trace_total;
static int gc_trace_busy;

static int GC_TraceCodePtr( const void *p )
{
	const uintptr_t v = (uintptr_t)p;

	return v >= 0x80003100u && v < 0x81800000u && ( v & 3u ) == 0;
}

/* The PPC back chain: word 0 of a frame points at the caller's frame (always higher on the stack),
 * and word 1 of that frame is its saved LR. Every step is range, alignment and direction checked so a
 * frame that is not set up yet (crt0, static init) ends the walk instead of faulting. */
static void *GC_TraceUpFrame( void )
{
	const uintptr_t *f0 = (const uintptr_t *)__builtin_frame_address( 0 );
	const uintptr_t *f1, *f2;
	uintptr_t up;

	if( !f0 || !GC_TraceCodePtr( f0 ))
		return NULL;
	f1 = (const uintptr_t *)f0[0];
	if( !f1 || f1 <= f0 || !GC_TraceCodePtr( f1 ))
		return NULL;
	f2 = (const uintptr_t *)f1[0];
	if( !f2 || f2 <= f1 || !GC_TraceCodePtr( f2 ))
		return NULL;
	up = f2[1];
	return GC_TraceCodePtr( (void *)up ) ? (void *)up : NULL;
}

static void GC_TraceRecord( const char *what, size_t size, void *ret, void *got )
{
	gc_trace_entry_t *e;

	if( gc_trace_busy || size < GC_MALLOC_TRACE_MIN )
		return;
	gc_trace_busy = 1;
	if( got )
		gc_trace_total += (unsigned)size;
	if( gc_trace_count < GC_MALLOC_TRACE_ENTRIES )
	{
		e = &gc_trace[gc_trace_count++];
		e->what = what;
		e->size = (unsigned)size;
		e->ret = ret;
		e->up = GC_TraceUpFrame();
		e->total = gc_trace_total;
		e->ok = got ? 1 : 0;
	}
	else
		gc_trace_dropped++;
	gc_trace_busy = 0;
}

/* Print the entries recorded since the last flush. Safe to call from any stage marker after boot. */
void GC_MallocTraceFlush( void )
{
	unsigned i;

	if( gc_trace_busy )
		return;
	gc_trace_busy = 1;	/* anything allocated while printing is not recorded */
	for( i = gc_trace_flushed; i < gc_trace_count; i++ )
	{
		const gc_trace_entry_t *e = &gc_trace[i];

		SYS_Report( "Xash3D GameCube: malloc-trace %s size=%u ret=%p up=%p %s total=%u\n",
			e->what, e->size, e->ret, e->up, e->ok ? "ok" : "FAILED", e->total );
	}
	gc_trace_flushed = gc_trace_count;
	if( gc_trace_dropped )
	{
		SYS_Report( "Xash3D GameCube: malloc-trace table full, %u requests not recorded\n", gc_trace_dropped );
		gc_trace_dropped = 0;
	}
	gc_trace_busy = 0;
}

void *__wrap_malloc( size_t size )
{
	void *p = __real_malloc( size );

	GC_TraceRecord( "malloc", size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_calloc( size_t nmemb, size_t size )
{
	void *p = __real_calloc( nmemb, size );

	GC_TraceRecord( "calloc", nmemb * size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_memalign( size_t align, size_t size )
{
	void *p = __real_memalign( align, size );

	GC_TraceRecord( "memalign", size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_realloc( void *ptr, size_t size )
{
	void *p = __real_realloc( ptr, size );

	GC_TraceRecord( "realloc", size, __builtin_return_address( 0 ), p );
	return p;
}
#endif
