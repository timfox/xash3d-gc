/*
mcol.h - per-map convex brush collision (.mcol) for converted MOHAA maps

Angled MOHAA detail costs too many clipnodes as BSP30 hull geometry, so the
converter (timfox/mohaa-gc tools/mesh_collision.py) writes it to
maps/<map>.mcol instead, and the engine sweeps player and monster boxes
against these brushes the way Quake 3 does (CM_TraceThroughBrush): every
plane is pushed out by the box, the brush bounds serve as its axial planes and
bevels (so the file stores only angled planes), and hits stop
MCOL_DIST_EPSILON short of the surface.

MCol_TraceBox must match mesh_collision.trace_box(). No engine dependencies
on purpose, so tests/mcol_test.c can build it on the host.
*/
#ifndef MCOL_H
#define MCOL_H

#include <stddef.h>
#include <stdint.h>

#define MCOL_VERSION		2
#define MCOL_DIST_EPSILON	0.03125f
#define MCOL_MAX_STACK		128

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

typedef struct mcol_s
{
	const mcol_plane_t	*planes;
	const mcol_brush_t	*brushes;
	const int32_t		*sides;
	const mcol_node_t	*nodes;
	int		numplanes, numbrushes, numsides, numnodes;
} mcol_t;

typedef struct
{
	float	fraction;
	float	normal[3];	// valid when fraction < 1 and !startsolid
	int	startsolid;
	int	allsolid;
} mcol_trace_t;

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
