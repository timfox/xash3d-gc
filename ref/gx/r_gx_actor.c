/*
r_gx_actor.c - hardware-skinned studio characters on Flipper (GCac)

A character with a gc_studio/<name>.gca beside its MDL is drawn by the GX
transform unit instead of r_studio.c's CPU skin + immediate emit:

  * skinning: each display-list batch touches at most ten bones; their
    view x bone matrices go into PNMTX0..9 and every vertex carries its
    PNMTXIDX, so Flipper transforms positions and normals itself. The CPU
    only builds the bone matrices (R_StudioSetupBones, as before).
  * geometry: positions in bone space (s16), normals (s8) and UVs (s16) are
    indexed arrays; triangle strips live in prebuilt display lists, so a
    frame costs a handful of GX_CallDispList calls per character.
  * lighting: Flipper's lighting unit, from the map's own data baked per
    placement (tools/source_lightcache.py): the BSP leaf ambient cube as six
    lights with t^2 attenuation (exactly Source's n^2-weighted cube) and the
    light_environment sun when the sky is visible, its attenuation curve
    fitted so the lit side ramps like the PC's gamma-encoded output.
    $halflambert materials (face, eyes) use the half-Lambert curve.
  * LOD: the model's own Source LODs as baked (fisherman: LOD3, 5.2k tris,
    twice what the CPU path could afford), switched on its VTX switch
    points with Source's metric (100 / pixels per unit at the model).
  * memory: 8-bit indices into a 256-vertex array window per batch keep the
    file small; it loads beside the MDL into the studio arena (model.c
    calls R_GXActorPreloadModel) before the prop cache takes the rest.

Only Lost Coast bakes .gca files, so HL1 and DoD never take this path.
-gcnoactor turns it off.
*/

#include "r_local.h"

#if XASH_GAMECUBE

#include <gccore.h>
#include <ogc/gx.h>
#include <math.h>
#include <string.h>

#define GCA_MAGIC	0x47436163u /* 'GCac' */
#define GCA_VTXFMT	GX_VTXFMT5
#define GCA_MAX		8
#define GCA_HALFLAMBERT	0x01
#define GCA_MASKED	0x02
#define GCA_ADDITIVE	0x04
#define GCA_EQUIPMENT	0x08	/* a weapon merged onto the character: gone once the entity's body has bit 0 (he dropped it) */
#define GCA_HELD	0x10	/* a prop in his hand for a while (keys): drawn only while the entity's body has bit 1 */

typedef struct
{
	u32	magic;
	u16	version, nlod;
	u16	nbones, nplace;
	u32	file_size;
	f32	switchpt[4];
	u32	lod_ofs[4];
	u32	place_ofs;
	u32	pad[3];
} gca_header_t; /* 64 bytes */

typedef struct
{
	u16	nverts, nbatch;
	u8	pos_frac, tex_frac;
	u16	ntris;
	u32	pos_ofs, nrm_ofs, tex_ofs, batch_ofs;
	u8	tex_u8; /* texcoords u8 (stride 2) instead of s16 (stride 4) */
	u8	pad[7];
} gca_lod_t; /* 32 bytes */

typedef struct
{
	u16	mat;
	u8	nbones, flags;
	u8	bones[12];
	u32	dl_ofs, dl_size;
	u16	vbase; /* first vertex of this batch's array window */
	u16	pad[3];
} gca_batch_t; /* 32 bytes */

typedef struct
{
	f32	origin[3];
	u8	cube[6][4];
	u8	sun[4];
	f32	sundir[3];
	f32	lam[3];
	f32	half[3];
	u32	pad;
} gca_place_t; /* 80 bytes */

typedef struct
{
	const model_t	*mod;
	const void	*hdr;
	byte		*data;
	int		state; /* 1 ok, -1 failed */
} gca_slot_t;

extern byte *Mod_GCStudioBssAlloc( size_t size );
extern size_t Mod_GCStudioSkinUsed( void );
extern size_t Mod_GCStudioSkinRemaining( void );

static gca_slot_t gca_slots[GCA_MAX];

/* Studio arena reset: the cores point into the released arena. */
void R_GXActorSlotsReset( void )
{
	memset( gca_slots, 0, sizeof( gca_slots ));
}

static gca_slot_t *R_GXActorSlot( const model_t *mod, const studiohdr_t *hdr )
{
	char base[64], path[MAX_QPATH];
	gca_slot_t *s = NULL;
	gca_header_t h;
	file_t *f;
	int i;

	for( i = 0; i < GCA_MAX; i++ )
	{
		gca_slot_t *t = &gca_slots[i];

		/* A new map resets the studio arena and reloads the model: a slot
		 * is live only while its header and its own data are untouched. */
		if( t->mod && ( t->state != 1 || ((const gca_header_t *)t->data)->magic != GCA_MAGIC ))
		{
			if( t->mod == mod && t->hdr == hdr && t->state != 1 )
				return t; /* failed for this load already */
			if( t->state == 1 )
				memset( t, 0, sizeof( *t ));
		}
		if( t->mod == mod )
		{
			if( t->hdr == hdr )
				return t;
			s = t; /* model reloaded: load again */
			break;
		}
		if( !s && !t->mod )
			s = t;
	}
	if( !s )
		return NULL;
	memset( s, 0, sizeof( *s ));
	s->mod = mod;
	s->hdr = hdr;
	s->state = -1;

	COM_FileBase( mod->name, base, sizeof( base ));
	Q_snprintf( path, sizeof( path ), "gc_studio/%s.gca", base );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: actor %s not on disc\n", path );
		return s;
	}
	if( gEngfuncs.fsapi->Read( f, &h, sizeof( h )) != (fs_offset_t)sizeof( h )
		|| h.magic != GCA_MAGIC || h.version != 2 || h.nlod == 0 || h.nlod > 4
		|| h.file_size < sizeof( h ) || h.file_size > 512u * 1024u
		|| h.nbones != hdr->numbones )
	{
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: actor %s bad header\n", path );
		gEngfuncs.fsapi->Close( f );
		return s;
	}
	s->data = Mod_GCStudioBssAlloc( h.file_size );
	if( !s->data )
	{
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: actor %s no room (%u Kb, skins %u Kb, free %u Kb)\n",
			path, h.file_size / 1024u, (unsigned)( Mod_GCStudioSkinUsed() / 1024u ),
			(unsigned)( Mod_GCStudioSkinRemaining() / 1024u ));
		gEngfuncs.fsapi->Close( f );
		return s;
	}
	/* Recycled arena memory: drop its cache lines before the file lands. */
	DCInvalidateRange( s->data, ( h.file_size + 31u ) & ~31u );
	memcpy( s->data, &h, sizeof( h ));
	if( gEngfuncs.fsapi->Read( f, s->data + sizeof( h ), h.file_size - sizeof( h ))
		!= (fs_offset_t)( h.file_size - sizeof( h )))
	{
		gEngfuncs.fsapi->Close( f );
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: actor %s short read\n", path );
		s->data = NULL;
		return s;
	}
	gEngfuncs.fsapi->Close( f );
	DCFlushRange( s->data, ( h.file_size + 31u ) & ~31u );
	s->state = 1;
	gEngfuncs.Con_Reportf( "Xash3D GameCube: actor %s %u Kb lods=%u places=%u\n",
		path, h.file_size / 1024u, h.nlod, h.nplace );
	return s;
}

/* Called by the engine right after a studio model loads (model.c), so the
 * display lists sit beside it before the prop cache claims the rest of the
 * studio arena. */
void R_GXActorPreloadModel( model_t *mod )
{
	if( mod && mod->type == mod_studio && mod->cache.data
		&& !Q_strnicmp( mod->name, "models/lc/", 10 )
		&& !gEngfuncs.Sys_CheckParm( "-gcnoactor" ))
		R_GXActorSlot( mod, (const studiohdr_t *)mod->cache.data );
}

static void R_GXActorView( Mtx out )
{
	vec3_t forward, right, up;
	const float *org = RI.rvp.vieworigin;

	AngleVectors( RI.rvp.viewangles, forward, right, up );
	out[0][0] = right[0];    out[0][1] = right[1];    out[0][2] = right[2];
	out[0][3] = -DotProduct( right, org );
	out[1][0] = up[0];       out[1][1] = up[1];       out[1][2] = up[2];
	out[1][3] = -DotProduct( up, org );
	out[2][0] = -forward[0]; out[2][1] = -forward[1]; out[2][2] = -forward[2];
	out[2][3] = DotProduct( forward, org );
}

static void R_GXActorRotate( Mtx view, const float *in, float *out )
{
	out[0] = view[0][0] * in[0] + view[0][1] * in[1] + view[0][2] * in[2];
	out[1] = view[1][0] * in[0] + view[1][1] * in[1] + view[1][2] * in[2];
	out[2] = view[2][0] * in[0] + view[2][1] * in[1] + view[2][2] * in[2];
}

/* A light whose attenuation is poly(N.dir) for N.dir >= 0 (Flipper's
 * specular attenuation with diffuse off): dir is world space. */
static void R_GXActorLight( int i, Mtx view, const float *dir, const u8 *rgb, f32 a0, f32 a1, f32 a2 )
{
	GXLightObj lo;
	GXColor c;
	float v[3];

	R_GXActorRotate( view, dir, v );
	c.r = rgb[0]; c.g = rgb[1]; c.b = rgb[2]; c.a = 255;
	GX_InitLightColor( &lo, c );
	GX_InitSpecularDirHA( &lo, -v[0], -v[1], -v[2], v[0], v[1], v[2] );
	GX_InitLightAttn( &lo, a0, a1, a2, 1.0f, 0.0f, 0.0f );
	GX_LoadLightObj( &lo, GX_LIGHT0 << i );
}

static const gca_place_t *R_GXActorPlace( const byte *data, const cl_entity_t *e )
{
	const gca_header_t *h = (const gca_header_t *)data;
	const gca_place_t *p = (const gca_place_t *)( data + h->place_ofs );
	const gca_place_t *best = NULL;
	float bd = 1e30f;
	int i;

	for( i = 0; i < h->nplace; i++, p++ )
	{
		float dx = p->origin[0] - e->origin[0];
		float dy = p->origin[1] - e->origin[1];
		float dz = p->origin[2] - e->origin[2];
		float d = dx * dx + dy * dy + dz * dz;

		if( d < bd )
		{
			bd = d;
			best = p;
		}
	}
	return best;
}

static void R_GXActorLights( Mtx view, const gca_place_t *pl, qboolean halflambert )
{
	static const float axes[6][3] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
	};
	static const u8 grey[4] = { 96, 96, 96, 255 };
	int i;

	for( i = 0; i < 6; i++ )
		R_GXActorLight( i, view, axes[i], pl ? pl->cube[i] : grey, 0.0f, 0.0f, 1.0f );
	if( pl && ( pl->sun[0] | pl->sun[1] | pl->sun[2] ))
	{
		const f32 *k = halflambert ? pl->half : pl->lam;

		R_GXActorLight( 6, view, pl->sundir, pl->sun, k[0], k[1], k[2] );
	}
	else
	{
		static const u8 black[4] = { 0, 0, 0, 255 };
		static const float up[3] = { 0, 0, 1 };

		R_GXActorLight( 6, view, up, black, 0.0f, 0.0f, 0.0f );
	}
}

static void R_GXActorState( void )
{
	GXColor black = { 0, 0, 0, 255 }, white = { 255, 255, 255, 255 };

	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_PTNMTXIDX, GX_DIRECT );
	/* 8-bit indices into each batch's own 256-vertex window */
	GX_SetVtxDesc( GX_VA_POS, GX_INDEX8 );
	GX_SetVtxDesc( GX_VA_NRM, GX_INDEX8 );
	GX_SetVtxDesc( GX_VA_TEX0, GX_INDEX8 );
	GX_SetVtxAttrFmt( GCA_VTXFMT, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6 );

	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0 | GX_LIGHT1 | GX_LIGHT2 | GX_LIGHT3 | GX_LIGHT4 | GX_LIGHT5 | GX_LIGHT6,
		GX_DF_NONE, GX_AF_SPEC );
	GX_SetChanCtrl( GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL,
		GX_DF_NONE, GX_AF_NONE );
	GX_SetChanAmbColor( GX_COLOR0A0, black );
	GX_SetChanMatColor( GX_COLOR0A0, white );

	GX_SetNumTexGens( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetNumTevStages( 1 );
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	/* skin x light x 2: lights are stored halved, as the world lightmaps */
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetCullMode( GX_CULL_BACK );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
}

static void R_GXActorRestore( void )
{
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZCompLoc( GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetCurrentMtx( GX_PNMTX0 );
}

/* What he was carrying, once he lets go of it (the fisherman's harpoon as he
 * reaches for his keys: the map kills his weapon and spawns a physics
 * harpoon in his hand). There is no rigid-body solver here, so the mesh that
 * was in his hand is carried on as one rigid rod from where his hand left
 * it: it drops under gravity until its lower end meets the ground he stands
 * on, tips over about that end as a rod does (angular acceleration
 * 3g/2L cos(angle)), and lies where it lands. No bounce, no slide. */
#define GCA_DROP_GRAVITY	600.0f	/* sv_gravity */

static struct
{
	int	ent;		/* 0 = nothing held or dropped */
	int	state;		/* 0 in his hand, 1 falling, 2 tipping, 3 at rest */
	float	time;		/* of the last step */
	Mtx	held;		/* bone to world, as last drawn in his hand */
	Mtx	now;		/* as it is drawn while dropped */
	float	centre[3], axis[3];	/* in the bone's space: the rod's middle and its long way */
	float	half, ground;
	float	c[3], a[3];	/* world: middle, long way (pointing up) */
	float	vz, tilt, spin;	/* fall speed; angle off the ground and its rate */
} gca_drop;

/* The rod of a batch: the bounds of the vertices its display list names. */
static void R_GXActorDropMeasure( const byte *data, const gca_lod_t *lod, const gca_batch_t *b )
{
	const byte *p = data + b->dl_ofs, *end = p + b->dl_size;
	const s16 *pos = (const s16 *)( data + lod->pos_ofs + 6u * b->vbase );
	const float scale = 1.0f / (float)( 1 << lod->pos_frac );
	float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
	int k, n, axis = 0;

	while( p + 3 <= end && *p )
	{
		n = ( p[1] << 8 ) | p[2];
		p += 3;
		for( ; n > 0 && p + 4 <= end; n--, p += 4 )
		{
			for( k = 0; k < 3; k++ )
			{
				const float v = (float)pos[p[1] * 3 + k] * scale;

				if( v < lo[k] ) lo[k] = v;
				if( v > hi[k] ) hi[k] = v;
			}
		}
	}
	for( k = 1; k < 3; k++ )
		if( hi[k] - lo[k] > hi[axis] - lo[axis] )
			axis = k;
	for( k = 0; k < 3; k++ )
	{
		gca_drop.centre[k] = 0.5f * ( lo[k] + hi[k] );
		gca_drop.axis[k] = k == axis ? 1.0f : 0.0f;
	}
	gca_drop.half = 0.5f * ( hi[axis] - lo[axis] );
}

/* The bone-to-world matrix of the dropped thing at this moment. */
static void R_GXActorDropStep( const cl_entity_t *e, float time )
{
	float dt = time - gca_drop.time, u[3], n[3], len, cs, sn;
	Mtx r;
	int k;

	if( gca_drop.state == 0 )
	{
		/* let go: where the rod is, in the world */
		for( k = 0; k < 3; k++ )
		{
			gca_drop.c[k] = gca_drop.held[k][0] * gca_drop.centre[0] + gca_drop.held[k][1] * gca_drop.centre[1]
				+ gca_drop.held[k][2] * gca_drop.centre[2] + gca_drop.held[k][3];
			gca_drop.a[k] = gca_drop.held[k][0] * gca_drop.axis[0] + gca_drop.held[k][1] * gca_drop.axis[1]
				+ gca_drop.held[k][2] * gca_drop.axis[2];
		}
		len = sqrtf( DotProduct( gca_drop.a, gca_drop.a ));
		if( len < 0.001f )
			len = 1.0f;
		for( k = 0; k < 3; k++ )
			gca_drop.a[k] *= ( gca_drop.a[2] < 0.0f ? -1.0f : 1.0f ) / len;
		gca_drop.ground = e->origin[2] + 1.0f;
		gca_drop.vz = 0.0f;
		gca_drop.state = 1;
		dt = 0.0f;
	}
	if( dt < 0.0f || dt > 0.25f )
		dt = dt < 0.0f ? 0.0f : 0.25f;
	gca_drop.time = time;
	if( gca_drop.state == 1 )
	{
		const float low = gca_drop.c[2] - gca_drop.a[2] * gca_drop.half;

		gca_drop.vz += GCA_DROP_GRAVITY * dt;
		if( low - gca_drop.vz * dt <= gca_drop.ground )
		{
			gca_drop.c[2] -= low - gca_drop.ground;
			gca_drop.tilt = asinf( bound( 0.0f, gca_drop.a[2], 1.0f ));
			gca_drop.spin = 0.0f;
			gca_drop.state = 2;
		}
		else
			gca_drop.c[2] -= gca_drop.vz * dt;
	}
	else if( gca_drop.state == 2 )
	{
		/* about its lower end, which stays where it landed */
		float end[3], flat;

		for( k = 0; k < 3; k++ )
			end[k] = gca_drop.c[k] - gca_drop.a[k] * gca_drop.half;
		/* (a rod stood dead upright still goes over: the least lean) */
		gca_drop.spin += 1.5f * GCA_DROP_GRAVITY / ( 2.0f * gca_drop.half ) * Q_max( cosf( gca_drop.tilt ), 0.02f ) * dt;
		gca_drop.tilt -= gca_drop.spin * dt;
		if( gca_drop.tilt <= 0.0f )
		{
			gca_drop.tilt = 0.0f;
			gca_drop.state = 3;
		}
		flat = sqrtf( gca_drop.a[0] * gca_drop.a[0] + gca_drop.a[1] * gca_drop.a[1] );
		if( flat < 0.001f )
		{
			/* which way it goes over: the way the entity faces */
			gca_drop.a[0] = cosf( e->angles[1] * ( M_PI_F / 180.0f ));
			gca_drop.a[1] = sinf( e->angles[1] * ( M_PI_F / 180.0f ));
			flat = 1.0f;
		}
		cs = cosf( gca_drop.tilt );
		sn = sinf( gca_drop.tilt );
		gca_drop.a[0] *= cs / flat;
		gca_drop.a[1] *= cs / flat;
		gca_drop.a[2] = sn;
		for( k = 0; k < 3; k++ )
			gca_drop.c[k] = end[k] + gca_drop.a[k] * gca_drop.half;
	}
	/* The turn from how it left his hand to how it lies now: about the axis
	 * square to both directions of the rod. */
	for( k = 0; k < 3; k++ )
		u[k] = gca_drop.held[k][0] * gca_drop.axis[0] + gca_drop.held[k][1] * gca_drop.axis[1] + gca_drop.held[k][2] * gca_drop.axis[2];
	len = sqrtf( DotProduct( u, u ));
	for( k = 0; k < 3; k++ )
		u[k] = len > 0.001f ? u[k] / len : gca_drop.a[k];
	if( u[2] < 0.0f )
		VectorNegate( u, u ); /* (as a was turned to point up when he let go) */
	CrossProduct( u, gca_drop.a, n );
	sn = sqrtf( DotProduct( n, n ));
	cs = DotProduct( u, gca_drop.a );
	if( sn > 0.0001f )
	{
		guVector ax = { n[0] / sn, n[1] / sn, n[2] / sn };

		guMtxRotAxisRad( r, &ax, atan2f( sn, cs ));
	}
	else
		guMtxIdentity( r );
	r[0][3] = r[1][3] = r[2][3] = 0.0f;
	guMtxConcat( r, gca_drop.held, gca_drop.now );
	/* and the rod's middle where the fall has it */
	for( k = 0; k < 3; k++ )
	{
		gca_drop.now[k][3] = 0.0f;
		gca_drop.now[k][3] = gca_drop.c[k] - ( gca_drop.now[k][0] * gca_drop.centre[0]
			+ gca_drop.now[k][1] * gca_drop.centre[1] + gca_drop.now[k][2] * gca_drop.centre[2] );
	}
}

/*
=============
R_GXActorDraw

Draw the current studio entity from its .gca. bones are the world-space
bone matrices R_StudioSetupBones just built. Returns true when handled.
=============
*/
qboolean R_GXActorDraw( cl_entity_t *e, model_t *mod, studiohdr_t *hdr, matrix3x4 *bones )
{
	static int disabled = -1;
	static int logged;
	gca_slot_t *s;
	const gca_header_t *h;
	const gca_lod_t *lod;
	const gca_batch_t *b;
	const gca_place_t *pl;
	mstudiotexture_t *ptexture;
	short *pskinref;
	Mtx view, m;
	float dist, ppu, metric;
	int li, bi, k, lit_mode = -1;
	byte *data;

	if( disabled < 0 )
		disabled = gEngfuncs.Sys_CheckParm( "-gcnoactor" ) ? 1 : 0;
	if( disabled || !e || !mod || !hdr || !bones || e == tr.viewent )
		return false;
	/* Only Lost Coast's baked cast (models/lc/) ships .gca files: other
	 * games' models never probe the disc for one. */
	if( Q_strnicmp( mod->name, "models/lc/", 10 ))
		return false;
	s = R_GXActorSlot( mod, hdr );
	if( !s || s->state != 1 )
		return false;
	data = s->data;
	h = (const gca_header_t *)data;

	if( hdr->numtextures <= 0 || hdr->textureindex <= 0 || hdr->skinindex <= 0 )
		return true;
	ptexture = (mstudiotexture_t *)((byte *)hdr + hdr->textureindex );
	pskinref = (short *)((byte *)hdr + hdr->skinindex );
	if( e->curstate.skin > 0 && e->curstate.skin < hdr->numskinfamilies )
		pskinref += e->curstate.skin * hdr->numskinref;

	/* Source's LOD metric: 100 / (pixels a unit covers at the model). */
	{
		vec3_t d;

		VectorSubtract( e->origin, RI.rvp.vieworigin, d );
		dist = sqrtf( DotProduct( d, d ));
		ppu = ( 0.5f * (float)gpGlobals->height )
			/ tanf(( RI.rvp.fov_y > 1.0f ? RI.rvp.fov_y : 74.0f ) * (float)M_PI_F / 360.0f );
		metric = ppu > 0.0f ? 100.0f * dist / ppu : 0.0f;
	}
	for( li = h->nlod - 1; li > 0 && metric < h->switchpt[li]; li-- )
		;
	lod = (const gca_lod_t *)( data + h->lod_ofs[li] );
	if( li == 0 )
	{
		/* lip sync: the mouth vertices follow the phoneme being spoken */
		extern void R_GXFaceApply( const cl_entity_t *e, const model_t *mod, s16 *pos, int nverts );

		extern void R_GXFaceWarm( const model_t *mod );
		extern void R_GXGestureWarm( const model_t *mod, const studiohdr_t *hdr );

		/* (both read their files once, on the level's first frame) */
		R_GXFaceWarm( mod );
		R_GXGestureWarm( mod, hdr );
		R_GXFaceApply( e, mod, (s16 *)( data + lod->pos_ofs ), lod->nverts );
	}

	R_GXStudioForceBegin( false );
	R_GXActorView( view );
	R_GXActorState();
	GX_SetVtxAttrFmt( GCA_VTXFMT, GX_VA_POS, GX_POS_XYZ, GX_S16, lod->pos_frac );
	GX_SetVtxAttrFmt( GCA_VTXFMT, GX_VA_TEX0, GX_TEX_ST, lod->tex_u8 ? GX_U8 : GX_S16, lod->tex_frac );
	pl = R_GXActorPlace( data, e );

	b = (const gca_batch_t *)( data + lod->batch_ofs );
	for( bi = 0; bi < lod->nbatch; bi++, b++ )
	{
		int tex = b->mat < hdr->numskinref ? pskinref[b->mat] : b->mat;
		int texnum = ( tex >= 0 && tex < hdr->numtextures ) ? ptexture[tex].index : 0;
		const int hl = ( b->flags & GCA_HALFLAMBERT ) ? 1 : 0;

		if( texnum <= 0 || texnum >= MAX_TEXTURES )
			continue; /* skins not uploaded yet */
		if( b->flags & GCA_EQUIPMENT )
		{
			const float now = gp_cl ? (float)gp_cl->time : 0.0f;

			if( gca_drop.ent != e->index || now + 1.0f < gca_drop.time )
			{
				memset( &gca_drop, 0, sizeof( gca_drop ));
				gca_drop.ent = e->index;
				R_GXActorDropMeasure( data, lod, b );
			}
			if( !( e->curstate.body & 1 ))
			{
				/* in his hand: remember where, for when he lets go */
				memcpy( gca_drop.held, bones[b->bones[0] < hdr->numbones ? b->bones[0] : 0], sizeof( Mtx ));
				gca_drop.state = 0;
				gca_drop.time = now;
			}
			else if( gca_drop.half <= 0.0f )
				continue;
			else if( gca_drop.state != 3 || gca_drop.time == 0.0f )
				R_GXActorDropStep( e, now );
		}
		if(( b->flags & GCA_HELD ) && !( e->curstate.body & 2 ))
			continue;
		if( hl != lit_mode )
		{
			R_GXActorLights( view, pl, hl );
			lit_mode = hl;
		}
		for( k = 0; k < b->nbones; k++ )
		{
			int bone = b->bones[k] < hdr->numbones ? b->bones[k] : 0;

			if(( b->flags & GCA_EQUIPMENT ) && ( e->curstate.body & 1 ))
				guMtxConcat( view, gca_drop.now, m );
			else
				guMtxConcat( view, bones[bone], m );
			GX_LoadPosMtxImm( m, GX_PNMTX0 + 3 * k );
			GX_LoadNrmMtxImm( m, GX_PNMTX0 + 3 * k );
		}
		if( b->flags & GCA_MASKED )
		{
			GX_SetAlphaCompare( GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetZCompLoc( GX_FALSE );
		}
		else
		{
			GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetZCompLoc( GX_TRUE );
		}
		if( b->flags & GCA_ADDITIVE )
		{
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		}
		else
		{
			GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		}
		{
			const u32 ts = lod->tex_u8 ? 2u : 4u;

			GX_SetArray( GX_VA_POS, data + lod->pos_ofs + 6u * b->vbase, 6 );
			GX_SetArray( GX_VA_NRM, data + lod->nrm_ofs + 3u * b->vbase, 3 );
			GX_SetArray( GX_VA_TEX0, data + lod->tex_ofs + ts * b->vbase, ts );
			GX_InvVtxCache();
		}
		R_GXStudioBindTexnum( (unsigned)texnum );
		GX_CallDispList( data + b->dl_ofs, b->dl_size );
	}

	R_GXActorRestore();
	R_GXStudioEnd();
	R_GXStudioInvalidateBindings();
	R_GXInvalidateTriApiLayout();
	r_stats.c_studio_polys += lod->ntris;
	if( logged++ < 2 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: actor draw %s lod=%d/%d metric=%.0f batches=%u tris=%u place=%s\n",
			mod->name, li, h->nlod, metric, lod->nbatch, lod->ntris, pl ? "baked" : "none" );
	return true;
}

/*
 * GoldSrc body parts (Day of Defeat soldiers) from display lists.
 *
 * dod-gc's split_studio_mdl.py appends a "GCGX" section to each streamed body part's blob: batches of at most ten
 * bones, each a display list of strips, fans and triangles whose vertices carry a matrix slot, 16-bit indices
 * into the blob's own vertex and normal arrays (bone space, big-endian floats) and the texel coordinates of the
 * triangle commands. The CPU keeps R_StudioSetupBones; Flipper transforms, lights and draws.
 *
 * Light is GoldSrc's (R_StudioLighting): colour x ( ambient + shade x w ), w = clamp(( 1 + N.L ) / 1.5, 0, 1 ) with
 * L pointing at the light. One signed diffuse light of 2/3 on an ambient of 2/3 gives w in the raster colour;
 * the first TEV stage turns it into amb + w x shade and the second multiplies the skin in.
 */
#define GCGX_MAGIC	0x47434758u /* 'GCGX' */
#define GCGX_VTXFMT	GX_VTXFMT5

typedef struct
{
	u32	magic;
	u16	version, nbatch;
	u32	size;
	u32	pad;
} gcgx_header_t; /* 16 bytes */

typedef struct
{
	u16	mesh;
	u8	nbones, pad0;
	u8	bones[10];
	u16	pad1;
	u32	dl_ofs, dl_size;
} gcgx_batch_t; /* 24 bytes */

/* view x bone per bone, valid while the caller's serial (one per body drawn) is unchanged */
#define GCGX_MV_MAX 64 /* a DoD soldier has 34 bones; 128 entries would be 6 KB of BSS */
static Mtx gcgx_mv[GCGX_MV_MAX];
static unsigned gcgx_mv_serial[GCGX_MV_MAX];

static u8 R_GXBodyByte( float v )
{
	const float x = v * 255.0f + 0.5f;

	return x <= 0.0f ? 0 : x >= 255.0f ? 255 : (u8)x;
}

qboolean R_GXActorDrawBodyLists( const byte *base, const mstudiomodel_t *sub, const studiohdr_t *hdr, matrix3x4 *bones,
	const mstudiomesh_t *pmesh, const mstudiotexture_t *ptexture, const short *pskinref, const float *lightvec,
	const float *amb, const float *shade, void (*bind)( int texture ), unsigned serial )
{
	static int logged;
	const gcgx_header_t *h;
	const gcgx_batch_t *b;
	const byte *gx;
	GXColor white = { 255, 255, 255, 255 }, twothirds = { 170, 170, 170, 255 }, c;
	GXLightObj lo;
	Mtx view, texm;
	float toward[3], v[3];
	u32 gx_ofs, gx_len;
	int bi, k, last_tex = -1, tris = 0;

	if( !base || !sub || !hdr || !bones || !bind || memcmp( base, "GCMB", 4 ) || hdr->numbones > GCGX_MV_MAX )
		return false;
	memcpy( &gx_ofs, base + 4, sizeof( gx_ofs ));
	memcpy( &gx_len, base + 8, sizeof( gx_len ));
	if( gx_ofs < 32u || ( gx_ofs & 31u ) || gx_len < sizeof( *h ) || gx_len > 256u * 1024u )
		return false;
	gx = base + gx_ofs;
	h = (const gcgx_header_t *)gx;
	if( h->magic != GCGX_MAGIC || h->version != 1 || h->nbatch == 0 || h->size != gx_len
		|| sizeof( *h ) + (u32)h->nbatch * sizeof( *b ) > gx_len || ((uintptr_t)gx & 31u ))
		return false;
	b = (const gcgx_batch_t *)( gx + sizeof( *h ));
	for( bi = 0; bi < h->nbatch; bi++ )
	{
		if( b[bi].mesh >= sub->nummesh || b[bi].nbones == 0 || b[bi].nbones > 10 || ( b[bi].dl_ofs & 31u )
			|| ( b[bi].dl_size & 31u ) || b[bi].dl_ofs + b[bi].dl_size > gx_len )
			return false;
	}

	R_GXStudioForceBegin( false );
	R_GXActorView( view );

	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_PTNMTXIDX, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_POS, GX_INDEX16 );
	GX_SetVtxDesc( GX_VA_NRM, GX_INDEX16 );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GCGX_VTXFMT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GCGX_VTXFMT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GCGX_VTXFMT, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0 );
	GX_SetArray( GX_VA_POS, (void *)( base + sub->vertindex ), 12 );
	GX_SetArray( GX_VA_NRM, (void *)( base + sub->normindex ), 12 );
	GX_InvVtxCache();

	/* w = 2/3 + 2/3 N.L, clamped to 0..1 by the channel */
	toward[0] = -lightvec[0]; toward[1] = -lightvec[1]; toward[2] = -lightvec[2];
	R_GXActorRotate( view, toward, v );
	GX_InitLightColor( &lo, twothirds );
	GX_InitLightPos( &lo, v[0] * 1048576.0f, v[1] * 1048576.0f, v[2] * 1048576.0f );
	GX_InitLightAttn( &lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f );
	GX_LoadLightObj( &lo, GX_LIGHT0 );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_SIGNED, GX_AF_NONE );
	GX_SetChanCtrl( GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_SetChanAmbColor( GX_COLOR0A0, twothirds );
	GX_SetChanMatColor( GX_COLOR0A0, white );

	GX_SetNumTexGens( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0 );
	GX_SetNumTevStages( 2 );
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevDirect( GX_TEVSTAGE1 );
	c.r = R_GXBodyByte( amb[0] ); c.g = R_GXBodyByte( amb[1] ); c.b = R_GXBodyByte( amb[2] ); c.a = 255;
	GX_SetTevColor( GX_TEVREG0, c );
	c.r = R_GXBodyByte( shade[0] ); c.g = R_GXBodyByte( shade[1] ); c.b = R_GXBodyByte( shade[2] ); c.a = 255;
	GX_SetTevKColor( GX_KCOLOR0, c );
	GX_SetTevKColorSel( GX_TEVSTAGE0, GX_TEV_KCSEL_K0 );
	/* stage 0: amb + w x shade; alpha is the skin's */
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_KONST, GX_CC_C0 );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	/* stage 1: x skin */
	GX_SetTevOrder( GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
	GX_SetTevAlphaOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetCullMode( GX_CULL_BACK );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );

	for( bi = 0; bi < h->nbatch; bi++, b++ )
	{
		const int tex = pskinref[pmesh[b->mesh].skinref];

		if( tex != last_tex )
		{
			last_tex = tex;
			bind( tex );
			/* triangle commands carry texels: scale them to 0..1 like the CPU path's 1/width, 1/height */
			memset( texm, 0, sizeof( texm ));
			texm[0][0] = 1.0f / (float)ptexture[tex].width;
			texm[1][1] = 1.0f / (float)ptexture[tex].height;
			texm[2][2] = 1.0f;
			GX_LoadTexMtxImm( texm, GX_TEXMTX0, GX_MTX2x4 );
			if( ptexture[tex].flags & STUDIO_NF_MASKED )
			{
				GX_SetAlphaCompare( GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0 );
				GX_SetZCompLoc( GX_FALSE );
			}
			else
			{
				GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
				GX_SetZCompLoc( GX_TRUE );
			}
		}
		for( k = 0; k < b->nbones; k++ )
		{
			const int bone = ( b->bones[k] < hdr->numbones && b->bones[k] < GCGX_MV_MAX ) ? b->bones[k] : 0;

			/* view x bone once per body: its parts and batches share bones */
			if( gcgx_mv_serial[bone] != serial )
			{
				guMtxConcat( view, bones[bone], gcgx_mv[bone] );
				gcgx_mv_serial[bone] = serial;
			}
			GX_LoadPosMtxImm( gcgx_mv[bone], GX_PNMTX0 + 3 * k );
			GX_LoadNrmMtxImm( gcgx_mv[bone], GX_PNMTX0 + 3 * k );
		}
		GX_CallDispList( (void *)( gx + b->dl_ofs ), b->dl_size );
	}
	for( k = 0; k < sub->nummesh; k++ )
		tris += pmesh[k].numtris;

	GX_SetNumTevStages( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	R_GXActorRestore();
	R_GXStudioEnd();
	R_GXStudioInvalidateBindings();
	R_GXInvalidateTriApiLayout();
	r_stats.c_studio_polys += tris;
	if( logged++ < 4 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: body lists '%s' batches=%u bytes=%u verts=%d norms=%d\n",
			sub->name, h->nbatch, gx_len, sub->numverts, sub->numnorms );
	return true;
}

#else /* !XASH_GAMECUBE */

qboolean R_GXActorDrawBodyLists( const byte *base, const mstudiomodel_t *sub, const studiohdr_t *hdr, matrix3x4 *bones,
	const mstudiomesh_t *pmesh, const mstudiotexture_t *ptexture, const short *pskinref, const float *lightvec,
	const float *amb, const float *shade, void (*bind)( int texture ), unsigned serial )
{
	(void)serial;
	(void)base; (void)sub; (void)hdr; (void)bones; (void)pmesh; (void)ptexture; (void)pskinref; (void)lightvec;
	(void)amb; (void)shade; (void)bind;
	return false;
}

void R_GXActorPreloadModel( model_t *mod ) { (void)mod; }

qboolean R_GXActorDraw( cl_entity_t *e, model_t *mod, studiohdr_t *hdr, matrix3x4 *bones )
{
	(void)e; (void)mod; (void)hdr; (void)bones;
	return false;
}

#endif
