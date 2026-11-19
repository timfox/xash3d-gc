/*
cine_path.c - keyframed camera paths for cinematic capture (see cine_path.h)
*/
#include "cine_path.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Cine_Err( char *err, size_t errlen, int line, const char *msg )
{
	if( err && errlen )
		snprintf( err, errlen, "line %d: %s", line, msg );
}

// next line of text into buf, without the comment; returns pointer past it or NULL at end
static const char *Cine_NextLine( const char *p, char *buf, size_t len )
{
	size_t n = 0;
	int comment = 0;

	if( !p || !*p )
		return NULL;
	while( *p && *p != '\n' )
	{
		if( *p == '#' )
			comment = 1;
		if( !comment && *p != '\r' && n + 1 < len )
			buf[n++] = *p;
		p++;
	}
	buf[n] = '\0';
	return *p ? p + 1 : p;
}

static float Cine_Unwrap( float prev, float cur )
{
	while( cur - prev > 180.0f )
		cur -= 360.0f;
	while( cur - prev < -180.0f )
		cur += 360.0f;
	return cur;
}

int Cine_ParsePath( cine_path_t *path, const char *text, char *err, size_t errlen )
{
	char line[512];
	const char *p = text;
	int lineno = 0, have_header = 0;

	memset( path, 0, sizeof( *path ));
	if( err && errlen )
		err[0] = '\0';

	while(( p = Cine_NextLine( p, line, sizeof( line ))) != NULL )
	{
		char word[16], mode[16];
		cine_key_t k;
		int n, version;

		lineno++;
		if( sscanf( line, "%15s", word ) != 1 )
			continue; // blank or comment

		if( !strcmp( word, "cam" ))
		{
			if( sscanf( line, "%*s %d", &version ) != 1 || version != 1 )
			{
				Cine_Err( err, errlen, lineno, "unsupported version (want \"cam 1\")" );
				return 0;
			}
			have_header = 1;
			continue;
		}

		if( strcmp( word, "key" ))
		{
			Cine_Err( err, errlen, lineno, "unknown statement" );
			return 0;
		}
		if( !have_header )
		{
			Cine_Err( err, errlen, lineno, "key before \"cam 1\" header" );
			return 0;
		}

		memset( &k, 0, sizeof( k ));
		mode[0] = '\0';
		n = sscanf( line, "%*s %f %f %f %f %f %f %f %f %15s", &k.t, &k.o[0], &k.o[1], &k.o[2],
			&k.a[0], &k.a[1], &k.a[2], &k.fov, mode );
		if( n < 8 )
		{
			Cine_Err( err, errlen, lineno, "key needs t x y z pitch yaw roll fov" );
			return 0;
		}
		if( n == 9 && mode[0] )
		{
			if( !strcmp( mode, "smooth" )) k.mode = CINE_SMOOTH;
			else if( !strcmp( mode, "linear" )) k.mode = CINE_LINEAR;
			else if( !strcmp( mode, "hold" ) || !strcmp( mode, "cut" )) k.mode = CINE_HOLD;
			else
			{
				Cine_Err( err, errlen, lineno, "mode must be smooth, linear or hold" );
				return 0;
			}
		}
		if( k.fov < 1.0f || k.fov > 170.0f )
		{
			Cine_Err( err, errlen, lineno, "fov out of range 1..170" );
			return 0;
		}
		if( path->count >= CINE_MAX_KEYS )
		{
			Cine_Err( err, errlen, lineno, "too many keys" );
			return 0;
		}
		if( path->count > 0 )
		{
			const cine_key_t *prev = &path->keys[path->count - 1];
			int i;

			if( k.t <= prev->t )
			{
				Cine_Err( err, errlen, lineno, "key times must increase" );
				return 0;
			}
			for( i = 0; i < 3; i++ )
				k.a[i] = Cine_Unwrap( prev->a[i], k.a[i] );
		}
		else if( k.t < 0.0f )
		{
			Cine_Err( err, errlen, lineno, "first key time is negative" );
			return 0;
		}
		path->keys[path->count++] = k;
	}

	if( !have_header )
	{
		Cine_Err( err, errlen, lineno, "missing \"cam 1\" header" );
		return 0;
	}
	if( path->count < 1 )
	{
		Cine_Err( err, errlen, lineno, "no keys" );
		return 0;
	}
	return 1;
}

float Cine_Duration( const cine_path_t *path )
{
	return path->count > 0 ? path->keys[path->count - 1].t : 0.0f;
}

// tangent (units per second) of channel c at key i, for smooth runs
static float Cine_Tangent( const cine_path_t *path, int i, int c )
{
	const cine_key_t *k = path->keys;
	float v0, v1;

	// ends of the path, and keys bordering a non-smooth segment, ease to a stop
	if( i <= 0 || i >= path->count - 1 )
		return 0.0f;
	if( k[i - 1].mode != CINE_SMOOTH || k[i].mode != CINE_SMOOTH )
		return 0.0f;

	if( c < 3 ) { v0 = k[i - 1].o[c]; v1 = k[i + 1].o[c]; }
	else if( c < 6 ) { v0 = k[i - 1].a[c - 3]; v1 = k[i + 1].a[c - 3]; }
	else { v0 = k[i - 1].fov; v1 = k[i + 1].fov; }

	return ( v1 - v0 ) / ( k[i + 1].t - k[i - 1].t );
}

static float Cine_Channel( const cine_key_t *k, int c )
{
	if( c < 3 ) return k->o[c];
	if( c < 6 ) return k->a[c - 3];
	return k->fov;
}

static float Cine_AngleNorm( float a )
{
	a = fmodf( a, 360.0f );
	if( a > 180.0f ) a -= 360.0f;
	if( a <= -180.0f ) a += 360.0f;
	return a;
}

int Cine_Sample( const cine_path_t *path, float t, float o[3], float a[3], float *fov )
{
	const cine_key_t *k0, *k1;
	float out[7];
	int i, c;

	if( !path || path->count < 1 )
		return 0;

	if( path->count == 1 || t <= path->keys[0].t )
	{
		k0 = &path->keys[0];
		for( c = 0; c < 7; c++ ) out[c] = Cine_Channel( k0, c );
	}
	else if( t >= path->keys[path->count - 1].t )
	{
		k0 = &path->keys[path->count - 1];
		for( c = 0; c < 7; c++ ) out[c] = Cine_Channel( k0, c );
	}
	else
	{
		float dt, s;

		for( i = 0; i < path->count - 2; i++ )
			if( t < path->keys[i + 1].t )
				break;
		k0 = &path->keys[i];
		k1 = &path->keys[i + 1];
		dt = k1->t - k0->t;
		s = ( t - k0->t ) / dt;

		for( c = 0; c < 7; c++ )
		{
			float p0 = Cine_Channel( k0, c ), p1 = Cine_Channel( k1, c );

			if( k0->mode == CINE_HOLD )
				out[c] = p0;
			else if( k0->mode == CINE_LINEAR )
				out[c] = p0 + ( p1 - p0 ) * s;
			else
			{
				float m0 = Cine_Tangent( path, i, c ) * dt;
				float m1 = Cine_Tangent( path, i + 1, c ) * dt;
				float s2 = s * s, s3 = s2 * s;

				out[c] = ( 2.0f * s3 - 3.0f * s2 + 1.0f ) * p0
					+ ( s3 - 2.0f * s2 + s ) * m0
					+ ( -2.0f * s3 + 3.0f * s2 ) * p1
					+ ( s3 - s2 ) * m1;
			}
		}
	}

	for( c = 0; c < 3; c++ )
	{
		if( o ) o[c] = out[c];
		if( a ) a[c] = Cine_AngleNorm( out[3 + c] );
	}
	if( fov ) *fov = out[6];
	return 1;
}

int Cine_FormatKey( char *buf, size_t len, float t, const float o[3], const float a[3], float fov, int mode )
{
	static const char *names[] = { "smooth", "linear", "hold" };

	if( mode < CINE_SMOOTH || mode > CINE_HOLD )
		mode = CINE_SMOOTH;
	return snprintf( buf, len, "key %.3f %.1f %.1f %.1f %.2f %.2f %.2f %.1f %s",
		t, o[0], o[1], o[2], a[0], a[1], a[2], fov, names[mode] );
}

#define CINE_STAMP_DATA	( CINE_STAMP_BITS - 3 )	// cells 1..17 carry the frame number
#define CINE_STAMP_MASK	(( 1u << CINE_STAMP_DATA ) - 1 )

static unsigned int Cine_Parity( unsigned int v )
{
	unsigned int p = 0;

	while( v )
	{
		p ^= v & 1u;
		v >>= 1;
	}
	return p;
}

unsigned int Cine_StampWord( unsigned int frame )
{
	unsigned int data = frame & CINE_STAMP_MASK;

	// cell 0 white, cells 1..17 data (LSB first), cell 18 parity, cell 19 black
	return 1u | ( data << 1 ) | ( Cine_Parity( data ) << ( CINE_STAMP_BITS - 2 ));
}

int Cine_StampFrame( unsigned int word )
{
	unsigned int data;

	if( !( word & 1u ) || ( word & ( 1u << ( CINE_STAMP_BITS - 1 ))))
		return -1;
	data = ( word >> 1 ) & CINE_STAMP_MASK;
	if((( word >> ( CINE_STAMP_BITS - 2 )) & 1u ) != Cine_Parity( data ))
		return -1;
	return (int)data;
}
