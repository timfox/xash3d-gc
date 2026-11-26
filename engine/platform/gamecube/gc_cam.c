/*
gc_cam.c - camera paths (.gcam). See gc_cam.h.
*/
#include <math.h>
#include <string.h>
#include "gc_cam.h"

#define GCAM_HEADER 16
#define GCAM_CLIP 64
#define GCAM_TRACK 16
#define GCAM_FLAG_LOOP 1
#define GCAM_FLAG_FIXED 2

static uint32_t rd32( const uint8_t *p ) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16( const uint8_t *p ) { return (uint16_t)((p[0] << 8) | p[1]); }
static float rdf( const uint8_t *p )
{
	uint32_t u = rd32( p );
	float f;

	memcpy( &f, &u, 4 );
	return f;
}

int gc_cam_count( const uint8_t *blob, size_t len )
{
	uint32_t count;

	if( !blob || len < GCAM_HEADER || memcmp( blob, "GCAM", 4 ) || rd16( blob + 4 ) != 1 )
		return -1;
	count = rd16( blob + 6 );
	if( rd32( blob + 8 ) > len || GCAM_HEADER + (size_t)count * GCAM_CLIP > len )
		return -1;
	return (int)count;
}

int gc_cam_clip( const uint8_t *blob, size_t len, int i, gc_cam_clip_t *clip )
{
	const uint8_t *e, *body;
	uint32_t off, size, ntracks, k;
	int n = gc_cam_count( blob, len );

	if( n < 0 || i < 0 || i >= n || !clip )
		return -1;
	memset( clip, 0, sizeof( *clip ));
	e = blob + GCAM_HEADER + (size_t)i * GCAM_CLIP;
	memcpy( clip->name, e, GC_CAM_NAME - 1 );
	clip->name[GC_CAM_NAME - 1] = 0;
	off = rd32( e + 32 );
	size = rd32( e + 36 );
	clip->duration = rdf( e + 40 );
	clip->near_z = rdf( e + 44 );
	clip->far_z = rdf( e + 48 );
	clip->aspect = rdf( e + 52 );
	clip->tick_rate = rdf( e + 56 );
	clip->loop = (e[61] & GCAM_FLAG_LOOP) != 0;
	clip->fixed = (e[61] & GCAM_FLAG_FIXED) != 0;
	ntracks = e[62];
	if( off > len || size > len - off || (size_t)ntracks * GCAM_TRACK > size )
		return -1;
	if( clip->fixed && !(clip->tick_rate > 0.0f) )
		return -1;
	body = blob + off;
	for( k = 0; k < ntracks; k++ )
	{
		const uint8_t *t = body + k * GCAM_TRACK;
		uint32_t kind = t[0], comps = t[1], keys = rd16( t + 4 );
		uint32_t t_off = rd32( t + 8 ), v_off = rd32( t + 12 ), w = clip->fixed ? 2 : 4;
		gc_cam_track_t *tr;

		if( kind >= GC_CAM_TRACKS )
			continue;	/* a later version's track: skip it */
		if( !keys || comps != (kind == GC_CAM_POS ? 3u : kind == GC_CAM_ROT ? 4u : 1u) || t[2] > 15 )
			return -1;
		if( t_off > size || keys * w > size - t_off || v_off > size || keys * comps * w > size - v_off )
			return -1;
		tr = &clip->track[kind];
		tr->times = body + t_off;
		tr->values = body + v_off;
		tr->keys = (uint16_t)keys;
		tr->components = (uint8_t)comps;
		tr->frac = t[2];
		tr->smooth = t[3] == 1;
	}
	return 0;
}

static int lower( int c ) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int gc_cam_find( const uint8_t *blob, size_t len, const char *name )
{
	int n = gc_cam_count( blob, len ), i, k;

	for( i = 0; i < n; i++ )
	{
		const char *e = (const char *)blob + GCAM_HEADER + i * GCAM_CLIP;

		for( k = 0; k < GC_CAM_NAME - 1 && e[k] && lower( e[k] ) == lower( name[k] ); k++ )
			;
		if( k == GC_CAM_NAME - 1 || (!e[k] && !name[k]) )
			return i;
	}
	return -1;
}

/* --- sampling ----------------------------------------------------------------------- */

static float key_time( const gc_cam_clip_t *c, const gc_cam_track_t *tr, int i )
{
	if( c->fixed )
		return (float)rd16( tr->times + 2 * i ) / c->tick_rate;
	return rdf( tr->times + 4 * i );
}

static float key_value( const gc_cam_clip_t *c, const gc_cam_track_t *tr, int i, int k )
{
	int at = i * tr->components + k;

	if( c->fixed )
		return (float)(int16_t)rd16( tr->values + 2 * at ) / (float)(1 << tr->frac);
	return rdf( tr->values + 4 * at );
}

/* slope per second at key i: central difference, one-sided at the ends and next to a cut */
static float tangent( const gc_cam_clip_t *c, const gc_cam_track_t *tr, int i, int k )
{
	float ti = key_time( c, tr, i );
	int prev = i > 0 && key_time( c, tr, i - 1 ) < ti;
	int next = i + 1 < tr->keys && key_time( c, tr, i + 1 ) > ti;

	if( prev && next )
		return (key_value( c, tr, i + 1, k ) - key_value( c, tr, i - 1, k )) / (key_time( c, tr, i + 1 ) - key_time( c, tr, i - 1 ));
	if( next )
		return (key_value( c, tr, i + 1, k ) - key_value( c, tr, i, k )) / (key_time( c, tr, i + 1 ) - ti);
	if( prev )
		return (key_value( c, tr, i, k ) - key_value( c, tr, i - 1, k )) / (ti - key_time( c, tr, i - 1 ));
	return 0.0f;
}

static void eval_track( const gc_cam_clip_t *c, const gc_cam_track_t *tr, float t, float *out )
{
	int n = tr->keys, lo, hi, k;
	float t0, t1, dt, u;

	if( n == 1 || t < key_time( c, tr, 0 ))
	{
		for( k = 0; k < tr->components; k++ )
			out[k] = key_value( c, tr, 0, k );
		return;
	}
	if( t >= key_time( c, tr, n - 1 ))
	{
		for( k = 0; k < tr->components; k++ )
			out[k] = key_value( c, tr, n - 1, k );
		return;
	}
	/* the last key at or before t; at a cut that is the later of the two */
	lo = 0;
	hi = n - 1;
	while( hi - lo > 1 )
	{
		int mid = (lo + hi) / 2;

		if( key_time( c, tr, mid ) <= t )
			lo = mid;
		else
			hi = mid;
	}
	t0 = key_time( c, tr, lo );
	t1 = key_time( c, tr, lo + 1 );
	dt = t1 - t0;
	u = dt > 0.0f ? (t - t0) / dt : 0.0f;
	for( k = 0; k < tr->components; k++ )
	{
		float a = key_value( c, tr, lo, k ), b = key_value( c, tr, lo + 1, k );

		if( tr->smooth )
		{
			float u2 = u * u, u3 = u2 * u;

			out[k] = (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * dt * tangent( c, tr, lo, k )
				+ (-2 * u3 + 3 * u2) * b + (u3 - u2) * dt * tangent( c, tr, lo + 1, k );
		}
		else
			out[k] = a + (b - a) * u;
	}
}

void gc_cam_sample( const gc_cam_clip_t *clip, float t, gc_cam_pose_t *pose )
{
	float len2;

	if( clip->loop && clip->duration > 0.0f )
	{
		t = (float)fmod( t, clip->duration );
		if( t < 0.0f )
			t += clip->duration;
	}
	pose->origin[0] = pose->origin[1] = pose->origin[2] = 0.0f;
	pose->quat[0] = pose->quat[1] = pose->quat[2] = 0.0f;
	pose->quat[3] = 1.0f;
	pose->fov_y = 73.739795f;
	pose->near_z = clip->near_z;
	pose->far_z = clip->far_z;
	if( clip->track[GC_CAM_POS].keys )
		eval_track( clip, &clip->track[GC_CAM_POS], t, pose->origin );
	if( clip->track[GC_CAM_ROT].keys )
	{
		eval_track( clip, &clip->track[GC_CAM_ROT], t, pose->quat );
		len2 = pose->quat[0] * pose->quat[0] + pose->quat[1] * pose->quat[1] + pose->quat[2] * pose->quat[2] + pose->quat[3] * pose->quat[3];
		if( len2 > 1e-12f )
		{
			float inv = (float)(1.0 / sqrt( len2 ));

			pose->quat[0] *= inv; pose->quat[1] *= inv; pose->quat[2] *= inv; pose->quat[3] *= inv;
		}
	}
	if( clip->track[GC_CAM_FOV].keys )
		eval_track( clip, &clip->track[GC_CAM_FOV], t, &pose->fov_y );
}

/* --- what the renderer wants --------------------------------------------------------- */

void gc_cam_axes( const gc_cam_pose_t *pose, float f[3], float l[3], float u[3] )
{
	float x = pose->quat[0], y = pose->quat[1], z = pose->quat[2], w = pose->quat[3];

	f[0] = 1 - 2 * (y * y + z * z); f[1] = 2 * (x * y + z * w);     f[2] = 2 * (x * z - y * w);
	l[0] = 2 * (x * y - z * w);     l[1] = 1 - 2 * (x * x + z * z); l[2] = 2 * (y * z + x * w);
	u[0] = 2 * (x * z + y * w);     u[1] = 2 * (y * z - x * w);     u[2] = 1 - 2 * (x * x + y * y);
}

#define GC_CAM_DEG 57.29577951308232

void gc_cam_angles( const gc_cam_pose_t *pose, float angles[3] )
{
	float f[3], l[3], u[3];
	double s;

	gc_cam_axes( pose, f, l, u );
	s = f[2] > 1.0f ? 1.0 : f[2] < -1.0f ? -1.0 : f[2];
	angles[0] = (float)(-asin( s ) * GC_CAM_DEG);
	angles[1] = (float)(atan2( f[1], f[0] ) * GC_CAM_DEG);
	angles[2] = (float)(atan2( l[2], u[2] ) * GC_CAM_DEG);
}

float gc_cam_fov_x( float fov_y, float aspect )
{
	return (float)(2.0 * atan( tan( fov_y / GC_CAM_DEG * 0.5 ) * aspect ) * GC_CAM_DEG);
}

void gc_cam_view_mtx( const gc_cam_pose_t *pose, float m[3][4] )
{
	float f[3], l[3], u[3];
	int k;

	gc_cam_axes( pose, f, l, u );
	for( k = 0; k < 3; k++ )
	{
		m[0][k] = -l[k];
		m[1][k] = u[k];
		m[2][k] = -f[k];
	}
	for( k = 0; k < 3; k++ )
		m[k][3] = -(m[k][0] * pose->origin[0] + m[k][1] * pose->origin[1] + m[k][2] * pose->origin[2]);
}

void gc_cam_proj_mtx( const gc_cam_pose_t *pose, float aspect, float m[4][4] )
{
	float cot = (float)(1.0 / tan( pose->fov_y / GC_CAM_DEG * 0.5 ));
	float tmp = 1.0f / (pose->far_z - pose->near_z);

	memset( m, 0, sizeof( float ) * 16 );
	m[0][0] = cot / aspect;
	m[1][1] = cot;
	m[2][2] = -pose->near_z * tmp;
	m[2][3] = -(pose->far_z * pose->near_z) * tmp;
	m[3][2] = -1.0f;
}

int gc_cam_play( gc_cam_player_t *pl, const uint8_t *blob, size_t len, const char *name, double now )
{
	int i = gc_cam_find( blob, len, name );

	pl->active = 0;
	if( i < 0 || gc_cam_clip( blob, len, i, &pl->clip ))
		return -1;
	pl->blob = blob;
	pl->len = len;
	pl->start = now;
	pl->active = 1;
	return 0;
}

int gc_cam_update( gc_cam_player_t *pl, double now, float aspect, gc_cam_view_t *view )
{
	double t;

	if( !pl->active )
		return 0;
	t = now - pl->start;
	if( t < 0.0 )
		t = 0.0;
	if( !pl->clip.loop && t > (double)pl->clip.duration )
	{
		pl->active = 0;
		return 0;
	}
	if( pl->clip.loop && pl->clip.duration > 0.0f )	/* wrap in double, so hours of looping keep float precision */
		t -= (double)pl->clip.duration * (double)(long)( t / (double)pl->clip.duration );
	view->t = (float)t;
	gc_cam_sample( &pl->clip, view->t, &view->pose );
	view->vieworg[0] = view->pose.origin[0];
	view->vieworg[1] = view->pose.origin[1];
	view->vieworg[2] = view->pose.origin[2];
	gc_cam_angles( &view->pose, view->viewangles );
	view->fov_x = gc_cam_fov_x( view->pose.fov_y, pl->clip.aspect > 0.0f ? pl->clip.aspect : aspect );
	return 1;
}

void gc_cam_stop( gc_cam_player_t *pl )
{
	pl->active = 0;
}

#ifdef GC_CAM_GX
#include <ogc/gx.h>

void gc_cam_load( const gc_cam_pose_t *pose, float aspect, float view[3][4] )
{
	float p[4][4];

	gc_cam_proj_mtx( pose, aspect, p );
	GX_LoadProjectionMtx( p, GX_PERSPECTIVE );
	gc_cam_view_mtx( pose, view );
}
#endif
