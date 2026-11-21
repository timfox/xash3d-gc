/*
mcol.h - per-map convex brush collision (.mcol) for converted MOHAA maps

Angled MOHAA detail costs too many clipnodes as BSP30 hull geometry, so the
converter (timfox/mohaa-gc tools/mesh_collision.py) writes it to
maps/<map>.mcol instead, and the engine sweeps player and monster boxes
against these brushes, LOD terrain heightfields and curved-patch triangles (thin
prisms) the way Quake 3 does (CM_TraceThroughBrush): every
plane is pushed out by the box, the brush bounds serve as its axial planes and
bevels (so the file stores only angled planes), and hits stop
MCOL_DIST_EPSILON short of the surface. A start less than MCOL_TOUCH_TOLERANCE
inside a plane counts as touching it, so float error can't wedge a box.

MCol_TraceBox must match mesh_collision.trace_box(). No engine dependencies
on purpose, so tests/mcol_test.c can build it on the host.
*/
#ifndef MCOL_H
#define MCOL_H

#include <stddef.h>
#include <stdint.h>

#define MCOL_VERSION		4
#define MCOL_DIST_EPSILON	0.03125f
#define MCOL_TOUCH_TOLERANCE	MCOL_DIST_EPSILON	// a start less than this inside one plane only touches it
#define MCOL_TOUCH_SLACK	( 1.0f / 1024.0f )
#define MCOL_TIE_FRACTION	1e-5f	// hits this close together count as one; the upward-facing normal wins
#define MCOL_MAX_STACK		128
#define MCOL_TERRAIN_SAMPLES	9
#define MCOL_TERRAIN_DEPTH	16.0f
#define MCOL_PATCH_THICKNESS	2.0f

typedef struct
{
	float	normal[3];
	float	dist;
} mcol_plane_t;

typedef struct
{
	int32_t	firstside;
	int32_t	numsides;
	float	mins[3];
	float	maxs[3];
} mcol_brush_t;

typedef struct
{
	float	mins[3];
	float	maxs[3];
	int32_t	a;	// b > 0: first brush of a leaf; b == 0: first of two adjacent children
	int32_t	b;	// brush count of a leaf, 0 for an inner node
} mcol_node_t;

// a 9 x 9 heightfield (MOHAA LOD terrain): sample (i, j) at origin + (i cell, j cell, zscale h)
typedef struct
{
	float	origin[3];
	float	cell;
	float	zscale;
	float	zmin, zmax;	// bounds of the heights
	uint8_t	h[MCOL_TERRAIN_SAMPLES * MCOL_TERRAIN_SAMPLES];
	uint8_t	pad[3];
} mcol_terrain_t;

typedef struct mcol_s
{
	const mcol_plane_t	*planes;
	const mcol_brush_t	*brushes;
	const int32_t		*sides;
	const mcol_node_t	*nodes;
	const mcol_terrain_t	*terrain;
	const mcol_node_t	*tnodes;
	const float		*verts;	// xyz
	const int32_t		*tris;	// three vertex indices each
	const mcol_node_t	*trinodes;
	int		numplanes, numbrushes, numsides, numnodes, numterrain, numtnodes;
	int		numverts, numtris, numtrinodes;
} mcol_t;

typedef struct
{
	float	fraction;
	float	normal[3];	// valid when fraction < 1 and !startsolid
	int	startsolid;
	int	allsolid;
} mcol_trace_t;

/*
Bytes the whole file takes, read from its header (at least 44 bytes of it), or 0 when
the header isn't a version this engine reads. Lets a loader size its buffer first.
*/
size_t MCol_Size( const void *header, size_t len );

/*
Swaps the little-endian file in data to host order in place and points m into
it. data must be 4-byte aligned and outlive m. Returns 1 on success, 0 when
the file is malformed (m is then empty).
*/
int MCol_Parse( mcol_t *m, void *data, size_t len );

/*
Sweeps the box (mins, maxs) from start to end. tr is filled from scratch:
fraction 1 and no flags when nothing is hit.
*/
void MCol_TraceBox( const mcol_t *m, const float start[3], const float mins[3], const float maxs[3], const float end[3], mcol_trace_t *tr );

#endif // MCOL_H
