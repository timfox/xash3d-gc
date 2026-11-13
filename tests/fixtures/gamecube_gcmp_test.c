/* Host test for engine/platform/gamecube/gcmp.h: decodes a sidecar written by
 * scripts/build-gamecube-disc.py (argv[1]) and compares against raw RGB565
 * reference texels (argv[2], little-endian u16, w*h) within a tolerance. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gcmp.h"

static unsigned char *slurp( const char *path, size_t *len )
{
	FILE *f = fopen( path, "rb" );
	unsigned char *p;

	assert( f );
	fseek( f, 0, SEEK_END );
	*len = (size_t)ftell( f );
	fseek( f, 0, SEEK_SET );
	p = (unsigned char *)malloc( *len );
	assert( fread( p, 1, *len, f ) == *len );
	fclose( f );
	return p;
}

int main( int argc, char **argv )
{
	size_t clen, rlen;
	unsigned char *cmp, *ref;
	gc_gcmp_header_t h;
	unsigned x, y;
	double err = 0;

	assert( argc == 3 );
	cmp = slurp( argv[1], &clen );
	ref = slurp( argv[2], &rlen );
	assert( GC_GcmpParseHeader( cmp, clen, &h ));
	assert( h.version == GC_GCMP_VERSION && h.mips == 1 && h.format == 0 );
	assert( clen == GC_GCMP_HEADER_BYTES + GC_GcmpBaseBytes( h.w, h.h ));
	assert( rlen == (size_t)h.w * h.h * 2 );
	/* bad headers are refused */
	cmp[0] = 'X';
	assert( !GC_GcmpParseHeader( cmp, clen, &h ));
	cmp[0] = 'G';
	assert( GC_GcmpParseHeader( cmp, clen, &h ));
	assert( !GC_GcmpParseHeader( cmp, 16, &h ));
	for( y = 0; y < h.h; y++ )
	{
		const unsigned char *row = cmp + GC_GCMP_HEADER_BYTES + ( y / 8 ) * GC_GcmpTileRowBytes( h.w );

		for( x = 0; x < h.w; x++ )
		{
			const uint16_t d = GC_GcmpTexel( row, x, y );
			const uint16_t r = (uint16_t)( ref[( y * h.w + x ) * 2] | ( ref[( y * h.w + x ) * 2 + 1] << 8 ));
			const int dr = (( d >> 11 ) & 31 ) - (( r >> 11 ) & 31 );
			const int dg = (( d >> 5 ) & 63 ) / 2 - (( r >> 5 ) & 63 ) / 2;
			const int db = ( d & 31 ) - ( r & 31 );

			err += abs( dr ) + abs( dg ) + abs( db );
		}
	}
	err /= (double)h.w * h.h * 3.0;
	printf( "mean abs error per channel (5-bit): %.2f\n", err );
	assert( err < 2.5 );
	puts( "gcmp ok" );
	return 0;
}
