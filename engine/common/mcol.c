/*
mcol.c - per-map convex brush collision (.mcol), see mcol.h

File layout, little-endian:
  header  char magic[4] "MCOL"; int32 version; int32 numplanes, numbrushes, numsides, numnodes
  planes  { float normal[3]; float dist; }
  brushes { int32 firstside; int32 numsides; float mins[3]; float maxs[3]; }
  sides   { int32 planenum; }
  nodes   { float mins[3]; float maxs[3]; int32 a; int32 b; }
Node 0 is the root, and children always come after their parent.
*/
#include "mcol.h"
#include <string.h>

static uint32_t MCol_LE32( const unsigned char *p )
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8 ) | ((uint32_t)p[2] << 16 ) | ((uint32_t)p[3] << 24 );
}

int MCol_Parse( mcol_t *m, void *data, size_t len )
{
	unsigned char *b = (unsigned char *)data;
	size_t words, need;
	int32_t hdr[5];
	int i;

	memset( m, 0, sizeof( *m ));

	if( !data || len < 24 || ((uintptr_t)data & 3 ) || memcmp( b, "MCOL", 4 ))
		return 0;

	for( i = 0; i < 5; i++ )
		hdr[i] = (int32_t)MCol_LE32( b + 4 + i * 4 );

	if( hdr[0] != MCOL_VERSION )
		return 0;
	for( i = 1; i < 5; i++ )
	{
		if( hdr[i] < 0 || hdr[i] > 0x1000000 )
			return 0;
	}

	need = 24 + (size_t)hdr[1] * sizeof( mcol_plane_t ) + (size_t)hdr[2] * sizeof( mcol_brush_t )
		+ (size_t)hdr[3] * sizeof( int32_t ) + (size_t)hdr[4] * sizeof( mcol_node_t );
	if( need > len )
		return 0;

	// every field is 32 bits wide, so one pass swaps the whole file
	words = need / 4;
	for( i = 6; (size_t)i < words; i++ )
	{
		uint32_t v = MCol_LE32( b + i * 4 );
		memcpy( b + i * 4, &v, 4 );
	}

	m->numplanes = hdr[1];
	m->numbrushes = hdr[2];
	m->numsides = hdr[3];
	m->numnodes = hdr[4];
	m->planes = (const mcol_plane_t *)( b + 24 );
	m->brushes = (const mcol_brush_t *)( m->planes + m->numplanes );
	m->sides = (const int32_t *)( m->brushes + m->numbrushes );
	m->nodes = (const mcol_node_t *)( m->sides + m->numsides );

	for( i = 0; i < m->numsides; i++ )
	{
		if( m->sides[i] < 0 || m->sides[i] >= m->numplanes )
			goto bad;
	}

	for( i = 0; i < m->numbrushes; i++ )
	{
		const mcol_brush_t *br = &m->brushes[i];

		if( br->firstside < 0 || br->numsides < 0 || br->firstside > m->numsides - br->numsides )
			goto bad;
	}

	for( i = 0; i < m->numnodes; i++ )
	{
		const mcol_node_t *n = &m->nodes[i];

		if( n->b < 0 || n->a < 0 )
			goto bad;
		if( n->b > 0 && n->a > m->numbrushes - n->b )
			goto bad;
		// children after the parent: no cycles, and the stack stays bounded
		if( n->b == 0 && ( n->a <= i || n->a + 1 >= m->numnodes ))
			goto bad;
	}

	return 1;
bad:
	memset( m, 0, sizeof( *m ));
	return 0;
}

static float MCol_Dot( const float *a, const float *b )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static int MCol_Overlaps( const float *lo, const float *hi, const float *bmin, const float *bmax )
{
	return lo[0] <= bmax[0] && hi[0] >= bmin[0]
		&& lo[1] <= bmax[1] && hi[1] >= bmin[1]
		&& lo[2] <= bmax[2] && hi[2] >= bmin[2];
}

static void MCol_TraceBrush( const mcol_t *m, const mcol_brush_t *br, const float *start, const float *mins, const float *maxs, const float *end, mcol_trace_t *tr )
{
	float enter = -1.0f, leave = 1.0f;
	const float *clipn = NULL;
	int startout = 0, getsout = 0;
	int i, k;

	for( i = 0; i < br->numsides; i++ )
	{
		const mcol_plane_t *p = &m->planes[m->sides[br->firstside + i]];
		float off[3], dist, d1, d2, f;

		for( k = 0; k < 3; k++ )
			off[k] = p->normal[k] < 0.0f ? maxs[k] : mins[k];

		dist = p->dist - MCol_Dot( off, p->normal );
		d1 = MCol_Dot( start, p->normal ) - dist;
		d2 = MCol_Dot( end, p->normal ) - dist;

		if( d2 > 0.0f )
			getsout = 1;
		if( d1 > 0.0f )
			startout = 1;

		// completely in front of this face, no intersection with the brush
		if( d1 > 0.0f && ( d2 >= MCOL_DIST_EPSILON || d2 >= d1 ))
			return;

		// completely behind this face, it doesn't clip
		if( d1 <= 0.0f && d2 <= 0.0f )
			continue;

		if( d1 > d2 )
		{
			// entering
			f = ( d1 - MCOL_DIST_EPSILON ) / ( d1 - d2 );
			if( f < 0.0f )
				f = 0.0f;
			if( f > enter )
			{
				enter = f;
				clipn = p->normal;
			}
		}
		else
		{
			// leaving
			f = ( d1 + MCOL_DIST_EPSILON ) / ( d1 - d2 );
			if( f > 1.0f )
				f = 1.0f;
			if( f < leave )
				leave = f;
		}
	}

	if( !startout )
	{
		tr->startsolid = 1;
		if( !getsout )
		{
			tr->allsolid = 1;
			tr->fraction = 0.0f;
		}
		return;
	}

	if( enter < leave && enter > -1.0f && enter < tr->fraction && clipn )
	{
		tr->fraction = enter > 0.0f ? enter : 0.0f;
		tr->normal[0] = clipn[0];
		tr->normal[1] = clipn[1];
		tr->normal[2] = clipn[2];
	}
}

void MCol_TraceBox( const mcol_t *m, const float start[3], const float mins[3], const float maxs[3], const float end[3], mcol_trace_t *tr )
{
	int stack[MCOL_MAX_STACK];
	float bmin[3], bmax[3];
	int sp = 0, k;

	memset( tr, 0, sizeof( *tr ));
	tr->fraction = 1.0f;

	if( !m || m->numnodes <= 0 )
		return;

	for( k = 0; k < 3; k++ )
	{
		bmin[k] = ( start[k] < end[k] ? start[k] : end[k] ) + mins[k];
		bmax[k] = ( start[k] > end[k] ? start[k] : end[k] ) + maxs[k];
	}

	stack[sp++] = 0;
	while( sp > 0 )
	{
		const mcol_node_t *n = &m->nodes[stack[--sp]];
		int i;

		if( !MCol_Overlaps( n->mins, n->maxs, bmin, bmax ))
			continue;

		if( n->b > 0 )
		{
			for( i = n->a; i < n->a + n->b; i++ )
			{
				const mcol_brush_t *br = &m->brushes[i];

				if( MCol_Overlaps( br->mins, br->maxs, bmin, bmax ))
					MCol_TraceBrush( m, br, start, mins, maxs, end, tr );
				if( tr->allsolid )
					return;
			}
		}
		else if( sp + 2 <= MCOL_MAX_STACK )
		{
			stack[sp++] = n->a;
			stack[sp++] = n->a + 1;
		}
	}
}
