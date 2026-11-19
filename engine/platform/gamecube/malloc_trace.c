/*
malloc_trace.c - labelled boot malloc trace (debug builds only)

Built when XASH_GC_MALLOC_TRACE=1 is set for the GameCube build. The link wraps malloc,
calloc, memalign and realloc (-Wl,--wrap=...) and logs every request of GC_MALLOC_TRACE_MIN
bytes or more with the caller's return address and its caller's:

  Xash3D GameCube: malloc-trace malloc size=196608 ret=0x80123456 up=0x80234567 total=1.2 MiB

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

extern void *__real_malloc( size_t size );
extern void *__real_calloc( size_t nmemb, size_t size );
extern void *__real_memalign( size_t align, size_t size );
extern void *__real_realloc( void *ptr, size_t size );

static size_t gc_trace_total;
static int gc_trace_busy;

static int GC_TraceCodePtr( const void *p )
{
	const uintptr_t v = (uintptr_t)p;

	return v >= 0x80003100u && v < 0x81800000u;
}

/* The PPC back chain: word 0 of a frame points at the caller's frame, whose word 1 is its LR. */
static void *GC_TraceUpFrame( void )
{
	const uintptr_t *frame = (const uintptr_t *)__builtin_frame_address( 0 );
	uintptr_t up;

	if( !frame || !GC_TraceCodePtr( frame ))
		return NULL;
	frame = (const uintptr_t *)frame[0];	/* the caller of the wrapper */
	if( !frame || !GC_TraceCodePtr( frame ))
		return NULL;
	frame = (const uintptr_t *)frame[0];	/* its caller */
	if( !frame || !GC_TraceCodePtr( frame ))
		return NULL;
	up = frame[1];
	return GC_TraceCodePtr( (void *)up ) ? (void *)up : NULL;
}

static void GC_TraceLog( const char *what, size_t size, void *ret, void *got )
{
	if( gc_trace_busy || size < GC_MALLOC_TRACE_MIN )
		return;
	gc_trace_busy = 1;
	if( got )
		gc_trace_total += size;
	SYS_Report( "Xash3D GameCube: malloc-trace %s size=%u ret=%p up=%p %s total=%u\n",
		what, (unsigned)size, ret, GC_TraceUpFrame(), got ? "ok" : "FAILED", (unsigned)gc_trace_total );
	gc_trace_busy = 0;
}

void *__wrap_malloc( size_t size )
{
	void *p = __real_malloc( size );

	GC_TraceLog( "malloc", size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_calloc( size_t nmemb, size_t size )
{
	void *p = __real_calloc( nmemb, size );

	GC_TraceLog( "calloc", nmemb * size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_memalign( size_t align, size_t size )
{
	void *p = __real_memalign( align, size );

	GC_TraceLog( "memalign", size, __builtin_return_address( 0 ), p );
	return p;
}

void *__wrap_realloc( void *ptr, size_t size )
{
	void *p = __real_realloc( ptr, size );

	GC_TraceLog( "realloc", size, __builtin_return_address( 0 ), p );
	return p;
}
#endif
