/* Host checks for engine/client/cine_path.c. */
#include "cine_path.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK( cond ) do { if( !( cond )) { printf( "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond ); failures++; } } while( 0 )
#define NEAR( a, b, eps ) ( fabsf(( a ) - ( b )) <= ( eps ))

static const char *dolly =
	"# a test dolly\n"
	"cam 1\n"
	"key 0   0   0   0   0  350 0 90\n"
	"key 2 100   0   0  10   10 0 90 linear   # 20 degrees through north\n"
	"key 4 100 100   0   0   20 0 60 hold\n"
	"key 6 200 200  50   0   30 0 60\n"
	"key 8 300 200  50   0   40 0 60\n"
	"key 10 400 200 50   0   50 0 60\n";

int main( void )
{
	cine_path_t path;
	char err[128], line[160];
	float o[3], a[3], fov;
	int i, f;

	CHECK( Cine_ParsePath( &path, dolly, err, sizeof( err )));
	CHECK( path.count == 6 );
	CHECK( NEAR( Cine_Duration( &path ), 10.0f, 1e-4f ));

	/* ends: clamp to the first and last key */
	CHECK( Cine_Sample( &path, -1.0f, o, a, &fov ));
	CHECK( NEAR( o[0], 0.0f, 1e-3f ) && NEAR( fov, 90.0f, 1e-3f ));
	CHECK( Cine_Sample( &path, 99.0f, o, a, &fov ));
	CHECK( NEAR( o[0], 400.0f, 1e-3f ) && NEAR( a[1], 50.0f, 1e-3f ));

	/* smooth from a path start eases in: at a quarter of the segment the camera is short of a quarter */
	Cine_Sample( &path, 0.5f, o, a, &fov );
	CHECK( o[0] > 0.0f && o[0] < 25.0f );
	/* yaw 350 -> 10 turns through 0, not back through 180 */
	Cine_Sample( &path, 1.0f, o, a, &fov );
	CHECK( NEAR( a[1], 0.0f, 0.01f ));
	CHECK( NEAR( o[0], 50.0f, 0.01f ));	/* symmetric ease: midpoint at half time */

	/* linear segment: constant speed */
	Cine_Sample( &path, 2.5f, o, a, &fov );
	CHECK( NEAR( o[1], 25.0f, 0.01f ) && NEAR( fov, 82.5f, 0.01f ));

	/* hold segment: the camera stays on key 2 until key 3, then cuts */
	Cine_Sample( &path, 5.99f, o, a, &fov );
	CHECK( NEAR( o[0], 100.0f, 1e-3f ) && NEAR( o[1], 100.0f, 1e-3f ));
	Cine_Sample( &path, 6.0f, o, a, &fov );
	CHECK( NEAR( o[0], 200.0f, 1e-3f ));

	/* a run of smooth keys passes through its middle key without stopping */
	{
		float before[3], after[3];

		Cine_Sample( &path, 7.95f, before, a, &fov );
		Cine_Sample( &path, 8.05f, after, a, &fov );
		CHECK( after[0] - before[0] > 4.0f );	/* ~50 units/s through the key */
		Cine_Sample( &path, 8.0f, o, a, &fov );
		CHECK( NEAR( o[0], 300.0f, 1e-3f ));
	}

	/* continuity: no jumps inside smooth/linear spans */
	for( i = 0; i < 1000; i++ )
	{
		float t = i * 0.01f, p[3], q[3];

		if( t >= 3.9f && t <= 6.1f )
			continue;	/* hold + cut */
		Cine_Sample( &path, t, p, a, &fov );
		Cine_Sample( &path, t + 0.01f, q, a, &fov );
		CHECK( fabsf( q[0] - p[0] ) < 2.0f && fabsf( q[1] - p[1] ) < 2.0f );
	}

	/* format and parse back */
	{
		const float ko[3] = { 12.5f, -40.0f, 300.0f }, ka[3] = { -5.0f, 181.0f, 0.0f };
		char text[256];

		Cine_FormatKey( line, sizeof( line ), 1.25f, ko, ka, 75.0f, CINE_LINEAR );
		snprintf( text, sizeof( text ), "cam 1\n%s\n", line );
		CHECK( Cine_ParsePath( &path, text, err, sizeof( err )));
		CHECK( path.count == 1 && path.keys[0].mode == CINE_LINEAR && NEAR( path.keys[0].o[2], 300.0f, 0.05f ));
	}

	/* errors name the line */
	CHECK( !Cine_ParsePath( &path, "key 0 0 0 0 0 0 0 90\n", err, sizeof( err )));
	CHECK( strstr( err, "line 1" ) != NULL );
	CHECK( !Cine_ParsePath( &path, "cam 1\nkey 1 0 0 0 0 0 0 90\nkey 1 0 0 0 0 0 0 90\n", err, sizeof( err )));
	CHECK( strstr( err, "line 3" ) && strstr( err, "increase" ));
	CHECK( !Cine_ParsePath( &path, "cam 1\nkey 0 0 0 0 0 0 0 90 wobble\n", err, sizeof( err )));
	CHECK( !Cine_ParsePath( &path, "cam 2\n", err, sizeof( err )));
	CHECK( !Cine_ParsePath( &path, "cam 1\n", err, sizeof( err )));

	/* frame stamps round-trip and reject damaged words */
	for( f = 0; f < 140000; f += 7 )
	{
		unsigned int w = Cine_StampWord( (unsigned int)f );

		CHECK( Cine_StampFrame( w ) == ( f & (( 1 << ( CINE_STAMP_BITS - 3 )) - 1 )));
		CHECK( Cine_StampFrame( w ^ 4u ) == -1 );	/* one flipped data cell */
	}
	CHECK( Cine_StampFrame( 0 ) == -1 );
	CHECK( Cine_StampFrame( 0xfffffu ) == -1 );

	if( failures )
		return 1;
	printf( "cine ok\n" );
	return 0;
}
