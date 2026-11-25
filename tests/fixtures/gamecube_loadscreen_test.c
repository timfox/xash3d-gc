/* Host test for engine/platform/gamecube/gc_loadscreen.h: parses a loading
 * screen file (argv[1]), checks the layout against argv[2..9] (bar x y w h,
 * plate tx ty tw th) and that the cut plate matches the screen's tiles. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_loadscreen.h"

int main( int argc, char **argv )
{
	FILE *f;
	unsigned char *buf, plate[GC_LOADSCREEN_RESIDENT_MAX];
	size_t len;
	gc_loadscreen_t ls;
	unsigned r;

	assert( argc == 10 );
	f = fopen( argv[1], "rb" );
	assert( f );
	fseek( f, 0, SEEK_END );
	len = (size_t)ftell( f );
	fseek( f, 0, SEEK_SET );
	buf = (unsigned char *)malloc( len );
	assert( fread( buf, 1, len, f ) == len );
	fclose( f );

	assert( GC_LoadScreenParse( buf, len, &ls ));
	assert( ls.bar_x == atoi( argv[2] ) && ls.bar_y == atoi( argv[3] ));
	assert( ls.bar_w == atoi( argv[4] ) && ls.bar_h == atoi( argv[5] ));
	assert( ls.plate_tx == atoi( argv[6] ) && ls.plate_ty == atoi( argv[7] ));
	assert( ls.plate_tw == atoi( argv[8] ) && ls.plate_th == atoi( argv[9] ));
	assert( len == GC_GCMP_HEADER_BYTES + GC_GcmpBaseBytes( GC_LOADSCREEN_W, GC_LOADSCREEN_H + ls.fill_h ));
	assert( ls.plate_bytes + ls.fill_bytes <= GC_LOADSCREEN_RESIDENT_MAX );

	GC_LoadScreenCutPlate( &ls, buf + GC_GCMP_HEADER_BYTES, plate );
	for( r = 0; r < ls.plate_th; r++ )
	{
		const unsigned char *src = buf + GC_GCMP_HEADER_BYTES
			+ (( ls.plate_ty + r ) * GC_LOADSCREEN_TILES_X + ls.plate_tx ) * 32u;

		assert( !memcmp( plate + r * ls.plate_tw * 32u, src, ls.plate_tw * 32u ));
	}

	/* refused: a plain 640x480 image, a plate missing the bar, a plate too big */
	{
		unsigned char h[GC_GCMP_HEADER_BYTES];

		memcpy( h, buf, sizeof( h ));
		h[6] = 480 >> 8; h[7] = 480 & 255;
		assert( !GC_LoadScreenParse( h, sizeof( h ), &ls ));
		memcpy( h, buf, sizeof( h ));
		h[29] = 1;
		assert( !GC_LoadScreenParse( h, sizeof( h ), &ls ));
		memcpy( h, buf, sizeof( h ));
		h[28] = 0; h[30] = 80;
		assert( !GC_LoadScreenParse( h, sizeof( h ), &ls ));
		assert( !GC_LoadScreenParse( buf, 16, &ls ));
	}
	puts( "loadscreen ok" );
	return 0;
}
