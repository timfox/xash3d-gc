/*
mcol.c - per-map convex brush and terrain collision (.mcol), see mcol.h

File layout, little-endian (timfox/mohaa-gc tools/mesh_collision.py):
  header  char magic[4] "MCOL"; int32 version (4); int32 numplanes, numbrushes, numsides,
          numnodes, numterrain, numtnodes, numverts, numtris, numtrinodes
  planes  { float normal[3]; float dist; }
  brushes { int32 firstside; int32 numsides; float mins[3]; float maxs[3]; }
          a brush is its six axial planes (its bounds, never stored) followed by
          its non-axial sides
  sides   { int32 planenum; }
  nodes   { float mins[3]; float maxs[3]; int32 a; int32 b; }  over brushes
  terrain { float origin[3]; float cell, zscale, zmin, zmax; uint8 h[81]; uint8 pad[3]; }
  tnodes  nodes over terrain patches
  verts   { float xyz[3]; }
  tris    { int32 v[3]; }  each the prism from abc back by MCOL_PATCH_THICKNESS
  trinodes nodes over triangles
Node 0 is the root of each tree, and children always come after their parent.
*/
#include "mcol.h"
#include <math.h>
#include <string.h>

#define MCOL_HEADER	44
#define MCOL_COUNTS	10

static uint32_t MCol_LE32( const unsigned char *p )
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8 ) | ((uint32_t)p[2] << 16 ) | ((uint32_t)p[3] << 24 );
}

static void MCol_Swap( unsigned char *p, size_t words )
{
	size_t i;

	for( i = 0; i < words; i++ )
	{
		uint32_t v = MCol_LE32( p + i * 4 );
		memcpy( p + i * 4, &v, 4 );
	}
}

static int MCol_CheckNodes( const mcol_node_t *nodes, int numnodes, int numitems )
{
	int i;

	for( i = 0; i < numnodes; i++ )
	{
		const mcol_node_t *n = &nodes[i];

		if( n->b < 0 || n->a < 0 )
			return 0;
		if( n->b > 0 && n->a > numitems - n->b )
			return 0;
		// children after the parent: no cycles, and the stack stays bounded
		if( n->b == 0 && ( n->a <= i || n->a + 1 >= numnodes ))
			return 0;
	}
	return 1;
}

size_t MCol_Size( const void *header, size_t len )
{
	const unsigned char *b = (const unsigned char *)header;
	int32_t hdr[MCOL_COUNTS];
	int i;

	if( !header || len < MCOL_HEADER || memcmp( b, "MCOL", 4 ))
		return 0;
	for( i = 0; i < MCOL_COUNTS; i++ )
		hdr[i] = (int32_t)MCol_LE32( b + 4 + i * 4 );
	if( hdr[0] != MCOL_VERSION )
		return 0;
	for( i = 1; i < MCOL_COUNTS; i++ )
	{
		if( hdr[i] < 0 || hdr[i] > 0x1000000 )
			return 0;
	}
	return MCOL_HEADER + (size_t)hdr[1] * sizeof( mcol_plane_t ) + (size_t)hdr[2] * sizeof( mcol_brush_t )
		+ (size_t)hdr[3] * sizeof( int32_t ) + (size_t)hdr[4] * sizeof( mcol_node_t )
		+ (size_t)hdr[5] * sizeof( mcol_terrain_t ) + (size_t)hdr[6] * sizeof( mcol_node_t )
		+ (size_t)hdr[7] * 3 * sizeof( float ) + (size_t)hdr[8] * 3 * sizeof( int32_t ) + (size_t)hdr[9] * sizeof( mcol_node_t );
}

int MCol_Parse( mcol_t *m, void *data, size_t len )
{
	unsigned char *b = (unsigned char *)data;
	unsigned char *p;
	size_t need;
	int32_t hdr[MCOL_COUNTS];
	int i;

	memset( m, 0, sizeof( *m ));

	if( !data || ((uintptr_t)data & 3 ))
		return 0;
	need = MCol_Size( data, len );
	if( !need || need > len )
		return 0;
	for( i = 0; i < MCOL_COUNTS; i++ )
		hdr[i] = (int32_t)MCol_LE32( b + 4 + i * 4 );

	m->numplanes = hdr[1];
	m->numbrushes = hdr[2];
	m->numsides = hdr[3];
	m->numnodes = hdr[4];
	m->numterrain = hdr[5];
	m->numtnodes = hdr[6];
	m->numverts = hdr[7];
	m->numtris = hdr[8];
	m->numtrinodes = hdr[9];

	// planes, brushes, sides and nodes are all 32-bit fields
	p = b + MCOL_HEADER;
	MCol_Swap( p, ( (size_t)m->numplanes * sizeof( mcol_plane_t ) + (size_t)m->numbrushes * sizeof( mcol_brush_t )
		+ (size_t)m->numsides * sizeof( int32_t ) + (size_t)m->numnodes * sizeof( mcol_node_t )) / 4 );
	m->planes = (const mcol_plane_t *)p;
	m->brushes = (const mcol_brush_t *)( m->planes + m->numplanes );
	m->sides = (const int32_t *)( m->brushes + m->numbrushes );
	m->nodes = (const mcol_node_t *)( m->sides + m->numsides );
	m->terrain = (const mcol_terrain_t *)( m->nodes + m->numnodes );
	m->tnodes = (const mcol_node_t *)( m->terrain + m->numterrain );

	// terrain: seven floats, then bytes that stay as they are
	for( i = 0; i < m->numterrain; i++ )
		MCol_Swap( (unsigned char *)&m->terrain[i], 7 );
	// tnodes, verts, tris and trinodes are 32-bit fields again
	MCol_Swap( (unsigned char *)m->tnodes, ( (size_t)m->numtnodes * sizeof( mcol_node_t )
		+ (size_t)m->numverts * 3 * sizeof( float ) + (size_t)m->numtris * 3 * sizeof( int32_t )
		+ (size_t)m->numtrinodes * sizeof( mcol_node_t )) / 4 );
	m->verts = (const float *)( m->tnodes + m->numtnodes );
	m->tris = (const int32_t *)( m->verts + 3 * (size_t)m->numverts );
	m->trinodes = (const mcol_node_t *)( m->tris + 3 * (size_t)m->numtris );

	for( i = 0; i < m->numtris * 3; i++ )
	{
		if( m->tris[i] < 0 || m->tris[i] >= m->numverts )
			goto bad;
	}

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

	for( i = 0; i < m->numterrain; i++ )
	{
		const mcol_terrain_t *t = &m->terrain[i];

		if( !( t->cell > 0.0f ) || !isfinite( t->cell ) || !isfinite( t->zscale )
			|| !isfinite( t->origin[0] ) || !isfinite( t->origin[1] ) || !isfinite( t->origin[2] ))
			goto bad;
	}

	if( !MCol_CheckNodes( m->nodes, m->numnodes, m->numbrushes ) || !MCol_CheckNodes( m->tnodes, m->numtnodes, m->numterrain )
		|| !MCol_CheckNodes( m->trinodes, m->numtrinodes, m->numtris ))
		goto bad;

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

typedef struct
{
	const float	*start, *end, *mins, *maxs;
	float	bmin[3], bmax[3];
	mcol_trace_t	*tr;
} mcol_work_t;

/*
Quake 3 CM_TraceThroughBrush over the brush bounds (its axial planes, in the order
-x, +x, -y, +y, -z, +z as mesh_collision.bound_planes()) and then its extra planes:
either num indices into m->planes, or num planes given directly.
*/
static void MCol_TraceBrush( const mcol_t *m, mcol_work_t *w, const float *bmins, const float *bmaxs,
	const int32_t *sides, const mcol_plane_t *direct, int num )
{
	mcol_trace_t *tr = w->tr;
	float enter = -1.0f, leave = 1.0f;
	float clipn[3] = { 0.0f, 0.0f, 0.0f };
	int clipped = 0;
	int startout = 0, getsout = 0;
	int i, k;

	for( i = -6; i < num; i++ )
	{
		mcol_plane_t axial;
		const mcol_plane_t *p;
		float off[3], dist, d1, d2, f;

		if( i < 0 )
		{
			int ax = ( i + 6 ) >> 1;

			memset( &axial, 0, sizeof( axial ));
			if(( i + 6 ) & 1 )
			{
				axial.normal[ax] = 1.0f;
				axial.dist = bmaxs[ax];
			}
			else
			{
				axial.normal[ax] = -1.0f;
				axial.dist = -bmins[ax];
			}
			p = &axial;
		}
		else p = direct ? &direct[i] : &m->planes[sides[i]];

		for( k = 0; k < 3; k++ )
			off[k] = p->normal[k] < 0.0f ? w->maxs[k] : w->mins[k];

		dist = p->dist - MCol_Dot( off, p->normal );
		d1 = MCol_Dot( w->start, p->normal ) - dist;
		d2 = MCol_Dot( w->end, p->normal ) - dist;

		// a start this close is touching, not inside: float error in a move that
		// ended on the surface (or a move by another collision path) left it there,
		// and calling it solid would wedge the box for good
		if( d1 > -MCOL_TOUCH_TOLERANCE && d1 <= 0.0f )
		{
			const float shift = MCOL_TOUCH_SLACK - d1;

			d1 += shift;
			d2 += shift;
		}

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
				clipn[0] = p->normal[0];
				clipn[1] = p->normal[1];
				clipn[2] = p->normal[2];
				clipped = 1;
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

	if( enter < leave && enter > -1.0f && enter < tr->fraction && clipped )
	{
		tr->fraction = enter > 0.0f ? enter : 0.0f;
		tr->normal[0] = clipn[0];
		tr->normal[1] = clipn[1];
		tr->normal[2] = clipn[2];
	}
}

static void MCol_Normalize( float *v )
{
	float len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );

	if( len > 0.0f )
	{
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

static void MCol_TerrainPoint( const mcol_terrain_t *t, int i, int j, float *out )
{
	out[0] = t->origin[0] + i * t->cell;
	out[1] = t->origin[1] + j * t->cell;
	out[2] = t->origin[2] + t->zscale * t->h[j * MCOL_TERRAIN_SAMPLES + i];
}

// the two wedges of terrain cell (i, j), as mesh_collision.terrain_wedges()
static void MCol_TraceTerrainCell( const mcol_t *m, mcol_work_t *w, const mcol_terrain_t *t, int i, int j )
{
	float p00[3], p10[3], p01[3], p11[3];
	const float *tris[2][3];
	int k;

	MCol_TerrainPoint( t, i, j, p00 );
	MCol_TerrainPoint( t, i + 1, j, p10 );
	MCol_TerrainPoint( t, i, j + 1, p01 );
	MCol_TerrainPoint( t, i + 1, j + 1, p11 );
	tris[0][0] = p00; tris[0][1] = p10; tris[0][2] = p01;
	tris[1][0] = p11; tris[1][1] = p01; tris[1][2] = p10;

	for( k = 0; k < 2; k++ )
	{
		const float *a = tris[k][0], *b = tris[k][1], *c = tris[k][2];
		float u[3], v[3], e[2], zlo, zhi;
		float wmins[3], wmaxs[3];
		mcol_plane_t planes[2];
		float *n = planes[0].normal, *sn = planes[1].normal;

		zlo = a[2] < b[2] ? a[2] : b[2];
		zlo = c[2] < zlo ? c[2] : zlo;
		zhi = a[2] > b[2] ? a[2] : b[2];
		zhi = c[2] > zhi ? c[2] : zhi;
		wmins[0] = p00[0]; wmins[1] = p00[1]; wmins[2] = zlo - MCOL_TERRAIN_DEPTH;
		wmaxs[0] = p11[0]; wmaxs[1] = p11[1]; wmaxs[2] = zhi;
		if( !MCol_Overlaps( wmins, wmaxs, w->bmin, w->bmax ))
			continue;

		u[0] = b[0] - a[0]; u[1] = b[1] - a[1]; u[2] = b[2] - a[2];
		v[0] = c[0] - a[0]; v[1] = c[1] - a[1]; v[2] = c[2] - a[2];
		n[0] = u[1] * v[2] - u[2] * v[1];
		n[1] = u[2] * v[0] - u[0] * v[2];
		n[2] = u[0] * v[1] - u[1] * v[0];
		MCol_Normalize( n );
		if( n[2] < 0.0f )
		{
			n[0] = -n[0];
			n[1] = -n[1];
			n[2] = -n[2];
		}
		planes[0].dist = MCol_Dot( n, a );

		// diagonal b -> c, facing away from a
		e[0] = c[0] - b[0];
		e[1] = c[1] - b[1];
		sn[0] = e[1];
		sn[1] = -e[0];
		sn[2] = 0.0f;
		MCol_Normalize( sn );
		if( sn[0] * ( a[0] - b[0] ) + sn[1] * ( a[1] - b[1] ) > 0.0f )
		{
			sn[0] = -sn[0];
			sn[1] = -sn[1];
		}
		planes[1].dist = MCol_Dot( sn, b );

		MCol_TraceBrush( m, w, wmins, wmaxs, NULL, planes, 2 );
		if( w->tr->allsolid )
			return;
	}
}

static int MCol_CellIndex( float v, float origin, float cell )
{
	const int last = MCOL_TERRAIN_SAMPLES - 2;
	float f = floorf(( v - origin ) / cell );

	if( f < 0.0f )
		return 0;
	if( f > (float)last )
		return last;
	return (int)f;
}

static void MCol_TraceTerrain( const mcol_t *m, mcol_work_t *w, const mcol_terrain_t *t )
{
	const float span = t->cell * ( MCOL_TERRAIN_SAMPLES - 1 );
	float lo[3], hi[3];
	int i, j, i0, i1, j0, j1;

	lo[0] = t->origin[0]; lo[1] = t->origin[1]; lo[2] = t->zmin - MCOL_TERRAIN_DEPTH;
	hi[0] = t->origin[0] + span; hi[1] = t->origin[1] + span; hi[2] = t->zmax;
	if( !MCol_Overlaps( lo, hi, w->bmin, w->bmax ))
		return;

	i0 = MCol_CellIndex( w->bmin[0], t->origin[0], t->cell );
	i1 = MCol_CellIndex( w->bmax[0], t->origin[0], t->cell );
	j0 = MCol_CellIndex( w->bmin[1], t->origin[1], t->cell );
	j1 = MCol_CellIndex( w->bmax[1], t->origin[1], t->cell );
	for( j = j0; j <= j1; j++ )
	{
		for( i = i0; i <= i1; i++ )
		{
			MCol_TraceTerrainCell( m, w, t, i, j );
			if( w->tr->allsolid )
				return;
		}
	}
}

// patch triangle abc as the prism behind it, as mesh_collision.triangle_prism()
static void MCol_TraceTriangle( const mcol_t *m, mcol_work_t *w, const int32_t *tri )
{
	const float *p[3];
	float u[3], v[3], n[3], pmins[3], pmaxs[3];
	mcol_plane_t planes[5];
	int e, k;

	for( k = 0; k < 3; k++ )
		p[k] = &m->verts[3 * tri[k]];

	for( k = 0; k < 3; k++ )
	{
		u[k] = p[1][k] - p[0][k];
		v[k] = p[2][k] - p[0][k];
	}
	n[0] = u[1] * v[2] - u[2] * v[1];
	n[1] = u[2] * v[0] - u[0] * v[2];
	n[2] = u[0] * v[1] - u[1] * v[0];
	if( MCol_Dot( n, n ) < 1e-8f )
		return;
	MCol_Normalize( n );

	for( k = 0; k < 3; k++ )
	{
		float lo = p[0][k], hi = p[0][k];
		int c;

		for( c = 0; c < 3; c++ )
		{
			const float a = p[c][k], b = p[c][k] - n[k] * MCOL_PATCH_THICKNESS;

			lo = a < lo ? a : lo;
			lo = b < lo ? b : lo;
			hi = a > hi ? a : hi;
			hi = b > hi ? b : hi;
		}
		pmins[k] = lo;
		pmaxs[k] = hi;
	}
	if( !MCol_Overlaps( pmins, pmaxs, w->bmin, w->bmax ))
		return;

	for( k = 0; k < 3; k++ )
	{
		planes[0].normal[k] = n[k];
		planes[1].normal[k] = -n[k];
	}
	planes[0].dist = MCol_Dot( n, p[0] );
	planes[1].dist = -planes[0].dist + MCOL_PATCH_THICKNESS;

	for( e = 0; e < 3; e++ )
	{
		const float *a = p[e], *b = p[( e + 1 ) % 3], *c = p[( e + 2 ) % 3];
		float *en = planes[2 + e].normal;
		float ab[3], ac[3];

		for( k = 0; k < 3; k++ )
		{
			ab[k] = b[k] - a[k];
			ac[k] = c[k] - a[k];
		}
		en[0] = ab[1] * n[2] - ab[2] * n[1];
		en[1] = ab[2] * n[0] - ab[0] * n[2];
		en[2] = ab[0] * n[1] - ab[1] * n[0];
		MCol_Normalize( en );
		if( MCol_Dot( en, ac ) > 0.0f )
		{
			en[0] = -en[0];
			en[1] = -en[1];
			en[2] = -en[2];
		}
		planes[2 + e].dist = MCol_Dot( en, a );
	}

	MCol_TraceBrush( m, w, pmins, pmaxs, NULL, planes, 5 );
}

enum { MCOL_TREE_BRUSHES, MCOL_TREE_TERRAIN, MCOL_TREE_TRIS };

static void MCol_Walk( const mcol_t *m, mcol_work_t *w, int tree )
{
	const mcol_node_t *nodes = tree == MCOL_TREE_TERRAIN ? m->tnodes : tree == MCOL_TREE_TRIS ? m->trinodes : m->nodes;
	const int numnodes = tree == MCOL_TREE_TERRAIN ? m->numtnodes : tree == MCOL_TREE_TRIS ? m->numtrinodes : m->numnodes;
	int stack[MCOL_MAX_STACK];
	int sp = 0;

	if( numnodes <= 0 )
		return;

	stack[sp++] = 0;
	while( sp > 0 )
	{
		const mcol_node_t *n = &nodes[stack[--sp]];
		int i;

		if( !MCol_Overlaps( n->mins, n->maxs, w->bmin, w->bmax ))
			continue;

		if( n->b > 0 )
		{
			for( i = n->a; i < n->a + n->b; i++ )
			{
				if( tree == MCOL_TREE_TERRAIN )
					MCol_TraceTerrain( m, w, &m->terrain[i] );
				else if( tree == MCOL_TREE_TRIS )
					MCol_TraceTriangle( m, w, &m->tris[3 * i] );
				else
				{
					const mcol_brush_t *br = &m->brushes[i];

					if( MCol_Overlaps( br->mins, br->maxs, w->bmin, w->bmax ))
						MCol_TraceBrush( m, w, br->mins, br->maxs, &m->sides[br->firstside], NULL, br->numsides );
				}
				if( w->tr->allsolid )
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

void MCol_TraceBox( const mcol_t *m, const float start[3], const float mins[3], const float maxs[3], const float end[3], mcol_trace_t *tr )
{
	mcol_work_t w;
	int k;

	memset( tr, 0, sizeof( *tr ));
	tr->fraction = 1.0f;

	if( !m )
		return;

	w.start = start;
	w.end = end;
	w.mins = mins;
	w.maxs = maxs;
	w.tr = tr;
	for( k = 0; k < 3; k++ )
	{
		w.bmin[k] = ( start[k] < end[k] ? start[k] : end[k] ) + mins[k];
		w.bmax[k] = ( start[k] > end[k] ? start[k] : end[k] ) + maxs[k];
	}

	MCol_Walk( m, &w, MCOL_TREE_BRUSHES );
	if( !tr->allsolid )
		MCol_Walk( m, &w, MCOL_TREE_TERRAIN );
	if( !tr->allsolid )
		MCol_Walk( m, &w, MCOL_TREE_TRIS );
}
