/*
mcol_test.c - host driver for engine/common/mcol.c

usage: mcol_test <file.mcol>
Reads traces from stdin, one per line: start[3] end[3] mins[3] maxs[3].
Prints one line per trace: fraction normal[3] startsolid allsolid.
Exits 2 when the file does not parse.
*/
#include "../engine/common/mcol.h"
#include <stdio.h>
#include <stdlib.h>

int main( int argc, char **argv )
{
	FILE *f;
	long len;
	void *data;
	mcol_t m;
	float v[12];

	if( argc != 2 || !( f = fopen( argv[1], "rb" )))
		return 1;
	fseek( f, 0, SEEK_END );
	len = ftell( f );
	fseek( f, 0, SEEK_SET );
	data = malloc( len > 0 ? (size_t)len : 1 );
	if( !data || fread( data, 1, (size_t)len, f ) != (size_t)len )
		return 1;
	fclose( f );

	if( !MCol_Parse( &m, data, (size_t)len ))
	{
		free( data );
		return 2;
	}

	while( scanf( "%f %f %f %f %f %f %f %f %f %f %f %f",
		&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11] ) == 12 )
	{
		mcol_trace_t tr;

		MCol_TraceBox( &m, v, v + 6, v + 9, v + 3, &tr );
		printf( "%.6f %.6f %.6f %.6f %d %d\n", tr.fraction, tr.normal[0], tr.normal[1], tr.normal[2], tr.startsolid, tr.allsolid );
	}

	free( data );
	return 0;
}
