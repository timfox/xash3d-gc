/*
r_gx_worlddl.c - full GoldSrc BSP world as baked GX display lists + real PVS

Same GCwd format as lostcoast-gc. Host bake (tools/bake_worlddl_goldsrc.py)
emits media/wd_<map>.gcd: every drawable world face as GX display lists
grouped by (PVS cluster, material), the BSP node tree for point-in-leaf, and
Quake RLE PVS rows.

MEM1 is tight on DoD FULL BSP maps, so the whole .gcd lives in ARAM. Spill
only holds the small per-chunk cache state + a chunk bounce ring. Tree /
chunkdir / PVS rows are fetched from ARAM on demand.

-gcnobspdl disables the path (CapFaces / live emit fallback).
*/

#include "r_local.h"

void R_GXPropDrawHorizon( const float *org );

#if XASH_GAMECUBE

#include <gccore.h>
#include <ogc/gx.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ogc/lwp_watchdog.h>
#include "../../engine/platform/gamecube/mem_gamecube.h"

#define GC_WD_FLAG_LIGHT_X1	0x0001	/* GoldSrc lighting: lightmap pages at x1 */
#define GC_WD_MAGIC		0x47437764u /* 'GCwd' */
#define GC_WD_MAT_WATER	0x01	/* SURF_WARP face */
#define GC_WD_MAT_ENVMAP	0x04
#define GC_WD_MAT_BASEALPHA	0x08	/* $basealphaenvmapmask — no sand IndTex */
#define GC_WD_MAT_ENVNOMASK	0x10	/* $envmap without a mask */
#define GC_WD_MAX_MATS		512
#define GC_WD_MAX_CHUNKS	6144
#define GC_WD_ST_FRAC		9
/* Per-frame ARAM→spill fetch. 256 KiB left early frames sparse; 512 fills
 * the visible PVS faster while still lazy-loading distant chunks. */
#define GC_WD_LOAD_BUDGET	( 512u * 1024u )
#define GC_WD_WARMUP_BUDGET	( 1024u * 1024u )
/* 192 full 32×32 RGB565 pages (384 KiB). Paid for by shrinking gx_fifo
 * 512→128 KiB in vid_gamecube.c (non-network). Enough for a whole view. */
#if defined( XASH_DODGC_NETWORK ) && XASH_DODGC_NETWORK
/* The DoD profile keeps the 256 KiB gx_fifo (vid_gamecube.c) because the
 * 384 KiB below is not paid for there: it overflowed MEM1 at cvar init. v1
 * world packs have no lightmap pages, so a handful of slots is enough. */
#define GC_WD_LM_SLOTS		8
#else
#define GC_WD_LM_SLOTS		192
#endif
#define GC_WD_LM_PAGE_BYTES	2048u

typedef struct
{
	u32	magic;
	u16	version, flags;
	u32	n_clusters, n_chunks, n_mats, n_nodes;
	u32	n_planes, ofs_mats, ofs_nodes, ofs_planes;
	u32	ofs_chunkdir, ofs_pvsdir, ofs_pvs, ofs_chunks;
	u32	file_size, max_chunk;
} gc_wd_header_t;

typedef struct
{
	u32	tex_hash;
	u32	hash2;		/* version 2 only; 0 on version 1 */
	u8	tevmat, flags;
	u16	pad;
} gc_wd_mat_t;

typedef struct
{
	u32	ofs, bytes;
	s16	mins[3], maxs[3];
	u16	cluster, pad;
} gc_wd_chunkdir_t;

typedef struct
{
	u16	mat, pad;
	u32	dl_ofs, dl_bytes;
} gc_wd_batch_t;

typedef struct
{
	s32	cache_ofs;
	s32	used_frame;
} gc_wd_slot_t;

extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );
extern size_t Mod_GCWorldArraySpillUsed( void );
extern int GC_WorldTexnumByHash( unsigned hash );
extern int GC_GetWaterFaceCount( void );
extern size_t Mod_GCBspScratchFloor( void );

/* Bytes of the map-load arena the live BSP scratch still occupies (master's
 * fix for DoD maps). On Lost Coast the stub BSP's scratch is the whole 3.7 MB
 * arena, which pushed the chunk ring to 234 KB and the texture slots out and
 * took the harbor from 40 to 20 fps; its surface table is small and already
 * under the highwater, so the floor is not applied there. */
static size_t R_GXWorldDLBspFloor( void )
{
	return Q_stricmp( XASH_GAMEDIR, "lostcoast" ) ? Mod_GCBspScratchFloor() : 0;
}
extern size_t Mod_GCWorldArraySpillFree( void );
extern size_t Mod_GCWorldArraySpillFreeLasting( void );
/* Spill kept for the team bodies' animation and streamed-mesh ring. */
#define GC_WD_BODY_RING_RESERVE ( 128u * 1024u )
extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );
extern void R_GXCmprBounceAdopt( byte *base, int nslots, unsigned slot_bytes, int far_n, unsigned far_bytes );
extern void R_GXCmprSetFar( qboolean far_pool );
extern void R_GXCmprBigAdopt( byte *base, int big_n, unsigned big_cap );
extern void R_GXCmprBigForProps( qboolean on );
extern qboolean _Sys_GetParmFromCmdLine( const char *parm, char *out, size_t size );
extern int GC_WorldTexCount( void );
extern void R_GXCmprStats( unsigned *hits, unsigned *misses );
extern qboolean R_GXWorldDLBindTex( unsigned texnum );
/* Mirror-pass clip stage: see r_gx_world.c. */
extern qboolean r_gx_mirror_clip;
extern void R_GXWrapNumTevStages( u8 n );
extern void R_GXWrapNumTexGens( u8 n );
extern void R_GXWrapAlphaCompare( u8 c0, u8 r0, u8 op, u8 c1, u8 r1 );
extern void R_GXWrapBlendMode( u8 type, u8 src, u8 dst, u8 op );
extern void R_GXWrapZCompLoc( u8 before_tex );
extern void R_GXMirrorClipSet( qboolean on );
#define GX_SetNumTevStages	R_GXWrapNumTevStages
#define GX_SetNumTexGens	R_GXWrapNumTexGens
#define GX_SetAlphaCompare	R_GXWrapAlphaCompare
#define GX_SetBlendMode		R_GXWrapBlendMode
#define GX_SetZCompLoc		R_GXWrapZCompLoc

/* Bump relief and detail live in media/tevmat.gcm (I8 mod2x tiles). Sand
 * uses the real normal map: IndTex warps the sun disc, then the alpha mask
 * punches the wet glints (retail $normalmapalphaenvmapmask). */
#define GC_TEV_MAX		96
#define GC_TEV_DETAIL		5440
#define GC_TEV_ENVMASK		2048
#define GC_TEV_MAX_ENVMASK	8
#define GC_TEV_F_ENVMASK	0x04
static u8 gc_tev_rec[GC_TEV_MAX][8];
static u8 gc_tev_nmat, gc_tev_ndet, gc_tev_nmask;
static s8 gc_tev_bound = -1;
/* Detail tiles stay in MEM1. The map ARAM window is the world pack; a
 * second reserve returns that same base and would wipe the BSP tree. */
static u8 gc_tev_store[8 * GC_TEV_DETAIL] __attribute__(( aligned( 32 )));
static GXTexObj gc_tev_obj;
static u8 gc_tev_envmask[GC_TEV_MAX_ENVMASK * GC_TEV_ENVMASK] __attribute__(( aligned( 32 )));
static GXTexObj gc_tev_envmask_obj;
static s8 gc_tev_envmask_bound = -1;
static byte *gc_wd_force;
static u32 gc_wd_force_size;
static int gc_wd_tev_gens = 1;
/* Lightmap pages: copied once from ARAM, kept at full 32×32. TEXMAP2. */
static byte gc_wd_lm_store[GC_WD_LM_SLOTS * GC_WD_LM_PAGE_BYTES] __attribute__(( aligned( 32 )));
static u32 gc_wd_lm_stream;	/* file offset of page 0 */
static s32 gc_wd_lm_slot_page[GC_WD_LM_SLOTS];
static s32 gc_wd_lm_slot_frame[GC_WD_LM_SLOTS];
static int gc_wd_lm_miss, gc_wd_lm_miss_total;
static int gc_wd_lm_binds;
static int gc_wd_chunk_lm;	/* current chunk uses TEXMAP2 lightmap */
static GXTexObj gc_wd_lm_obj;
/* Declared early — LmPage / SetupLit sit above the rest of the WorldDL state. */
static gc_wd_header_t gc_wd_hdr;
static int gc_wd_frame;
static char gc_wd_mapbase[40];
static void R_GXWorldDLFog( f32 z_near, f32 z_far );

static qboolean R_GXWorldDLFetch( u32 file_ofs, void *dst, u32 len );

static void R_GXTevBindDetail( u8 idx )
{
	if( idx >= gc_tev_ndet )
		return;
	if( gc_tev_bound != (s8)idx )
	{
		GX_InitTexObj( &gc_tev_obj, gc_tev_store + (u32)idx * GC_TEV_DETAIL,
			64, 64, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_TRUE );
		GX_InitTexObjLOD( &gc_tev_obj, GX_LIN_MIP_LIN, GX_LINEAR,
			0.0f, 3.0f, 0.0f, GX_FALSE, GX_TRUE, GX_ANISO_1 );
		gc_tev_bound = (s8)idx;
	}
	GX_LoadTexObj( &gc_tev_obj, GX_TEXMAP1 );
}

static void R_GXTevBindEnvMask( u8 idx )
{
	if( idx >= gc_tev_nmask )
		return;
	if( gc_tev_envmask_bound != (s8)idx )
	{
		GX_InitTexObj( &gc_tev_envmask_obj,
			gc_tev_envmask + (u32)idx * GC_TEV_ENVMASK,
			64, 64, GX_TF_I4, GX_REPEAT, GX_REPEAT, GX_FALSE );
		GX_InitTexObjFilterMode( &gc_tev_envmask_obj, GX_LINEAR, GX_LINEAR );
		gc_tev_envmask_bound = (s8)idx;
	}
	GX_LoadTexObj( &gc_tev_envmask_obj, GX_TEXMAP5 );
}

static int R_GXTevMatFlags( int id )
{
	if( id < 1 || id > (int)gc_tev_nmat )
		return 0;
	return gc_tev_rec[id - 1][0];
}

static int R_GXTevMatEnvMask( int id )
{
	if( id < 1 || id > (int)gc_tev_nmat )
		return -1;
	if( !( gc_tev_rec[id - 1][0] & GC_TEV_F_ENVMASK ))
		return -1;
	if( gc_tev_rec[id - 1][5] >= gc_tev_nmask )
		return -1;
	return gc_tev_rec[id - 1][5];
}

/*
 * One copy of each page, full 32×32. A hit returns the cached bytes and
 * does not touch ARAM. A miss copies the page once into a slot the GPU
 * is done with (the frame starts with DrawDone). Slots already used this
 * frame are left alone.
 */
static byte *R_GXWorldDLLmPage( u32 page )
{
	static u8 hint[2048];	/* page & 2047 -> slot it was last found in */
	static int full_frame = -1;	/* frame in which every slot was already in use */
	int i, victim = -1;
	s32 oldest = 0x7fffffff;

	i = hint[page & 2047u];
	if( i < GC_WD_LM_SLOTS && gc_wd_lm_slot_page[i] == (s32)page )
	{
		gc_wd_lm_slot_frame[i] = gc_wd_frame;
		return gc_wd_lm_store + (u32)i * GC_WD_LM_PAGE_BYTES;
	}
	/* A wide view asks for more pages than there are slots (500 chunks in
	 * the Lost Coast harbor, 192 slots). Once the cache is full for this
	 * frame every further request walked all the slots to learn that again:
	 * up to 2.5 ms a world pass. With at most 2048 pages the hint is exact,
	 * so a hint miss on a full frame means the page is not resident. */
	if( full_frame == gc_wd_frame && gc_wd_hdr.n_chunks <= 2048u )
		return NULL;
	for( i = 0; i < GC_WD_LM_SLOTS; i++ )
	{
		if( gc_wd_lm_slot_page[i] == (s32)page )
		{
			gc_wd_lm_slot_frame[i] = gc_wd_frame;
			hint[page & 2047u] = (u8)i;
			return gc_wd_lm_store + (u32)i * GC_WD_LM_PAGE_BYTES;
		}
		/* A page used by the previous world pass stays: with the water
		 * mirror there are two passes a frame, and when the two together
		 * want more pages than there are slots, the far chunks of one pass
		 * evicted the near pages of the other and each pass refetched about
		 * 50 pages from ARAM (2 ms a pass in the Lost Coast harbor). */
		if( gc_wd_lm_slot_frame[i] < gc_wd_frame - 1 && gc_wd_lm_slot_frame[i] < oldest )
		{
			oldest = gc_wd_lm_slot_frame[i];
			victim = i;
		}
	}
	if( victim < 0 )
	{
		full_frame = gc_wd_frame;
		return NULL;
	}
	if( !R_GXWorldDLFetch( gc_wd_lm_stream + page * GC_WD_LM_PAGE_BYTES,
		gc_wd_lm_store + (u32)victim * GC_WD_LM_PAGE_BYTES, GC_WD_LM_PAGE_BYTES ))
		return NULL;
	if( gc_wd_lm_slot_page[victim] >= 0 )
		GX_InvalidateTexAll();
	gc_wd_lm_slot_page[victim] = (s32)page;
	gc_wd_lm_slot_frame[victim] = gc_wd_frame;
	gc_wd_lm_miss++;
	gc_wd_lm_miss_total++;
	hint[page & 2047u] = (u8)victim;
	return gc_wd_lm_store + (u32)victim * GC_WD_LM_PAGE_BYTES;
}

/* Diffuse × lightmap page (TEXMAP2), or diffuse × vertex colour when no page. */
/* The TEV recipe R_GXTevMatAppend last left in place: ( id << 1 ) | lightmap,
 * or -1 when anything else touched the stages. Most materials share recipe
 * 0, and in the harbor nearly every batch changes material: skipping the
 * identical setup saved 1.3 ms per world pass (two passes with the mirror). */
static int gc_wd_tev_key = -1;

static void R_GXWorldDLSetupLit( int lightmap )
{
	gc_wd_tev_key = -1;
	GX_SetNumIndStages( 0 );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZCompLoc( GX_TRUE );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	if( gc_wd_hdr.version == 2 )
		GX_SetTexCoordGen( GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY );
	if( lightmap )
	{
		GX_SetNumTexGens( 2 );
		GX_SetNumTevStages( 2 );
		GX_SetTevDirect( GX_TEVSTAGE0 );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL );
		GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
		GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
		GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevDirect( GX_TEVSTAGE1 );
		GX_SetTevOrder( GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL );
		GX_SetTevColorIn( GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
		/* ×2 on the LM multiply restores cliff/tower mass now that SpecOn
		 * is needle-tower only (wide plates no longer get the white fill).
		 * Grey-pad luxels stay mid-grey so unset UVs don't fullbright. */
		GX_SetTevColorOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, ( gc_wd_hdr.flags & GC_WD_FLAG_LIGHT_X1 ) ? GX_CS_SCALE_1 : GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV ); /* GoldSrc packs light at x1 */
		GX_SetTevAlphaIn( GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		return;
	}
	GX_SetNumTexGens( gc_wd_tev_gens );
	GX_SetNumTevStages( 1 );
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
		gc_wd_hdr.version == 2 ? GX_CS_SCALE_1 : GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
}

/* id 0 restores the lit diffuse stage. A detail id adds the sun-lit
 * bump relief (or authored detail) as mod2x on top. */
static void R_GXTevMatAppend( int id, int a, int b )
{
	u8 flags, didx, aref;
	u16 scale_fp;
	float scale;
	Mtx tm;
	int base_stages;

	(void)a;
	(void)b;
	if( id < 1 || id > (int)gc_tev_nmat )
		id = 0;
	if( gc_wd_tev_key == (( id << 1 ) | ( gc_wd_chunk_lm ? 1 : 0 )))
		return;
	R_GXWorldDLSetupLit( gc_wd_chunk_lm );
	gc_wd_tev_key = ( id << 1 ) | ( gc_wd_chunk_lm ? 1 : 0 );
	base_stages = gc_wd_chunk_lm ? 2 : 1;
	if( !id )
		return;
	flags = gc_tev_rec[id - 1][0];
	didx = gc_tev_rec[id - 1][1];
	scale_fp = (u16)( ( gc_tev_rec[id - 1][2] << 8 ) | gc_tev_rec[id - 1][3] );
	aref = gc_tev_rec[id - 1][4];
	/* Detail mod2x on top of Source LM punched white voids into harbor
	 * cliffs. Keep relief only when there is no lightmap page. */
	if( !gc_wd_chunk_lm && ( flags & 0x01 ) && scale_fp && didx < gc_tev_ndet )
	{
		scale = (float)scale_fp / 256.0f;
		R_GXTevBindDetail( didx );
		tm[0][0] = scale; tm[0][1] = 0.0f; tm[0][2] = 0.0f; tm[0][3] = 0.0f;
		tm[1][0] = 0.0f; tm[1][1] = scale; tm[1][2] = 0.0f; tm[1][3] = 0.0f;
		tm[2][0] = 0.0f; tm[2][1] = 0.0f; tm[2][2] = 1.0f; tm[2][3] = 0.0f;
		GX_LoadTexMtxImm( tm, GX_TEXMTX0, GX_MTX2x4 );
		GX_SetNumTexGens( gc_wd_tev_gens > 1 ? 3 : 2 );
		GX_SetTexCoordGen( GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0 );
		GX_SetNumTevStages( base_stages + 1 );
		GX_SetTevOrder( GX_TEVSTAGE0 + base_stages, GX_TEXCOORD2, GX_TEXMAP1, GX_COLORNULL );
		GX_SetTevColorIn( GX_TEVSTAGE0 + base_stages, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
		GX_SetTevColorOp( GX_TEVSTAGE0 + base_stages, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0 + base_stages, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( GX_TEVSTAGE0 + base_stages, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	/* Alpha-test on LM-lit cliff/brush mats cut white sky holes through
	 * solid plates. Keep punch-through only for unlit foliage-style mats. */
	/* Decals (flag 0x08, Source $decal): the alpha is the decal's shape, so
	 * they cut out on lightmapped walls too. Without it the infodecals and
	 * overlays on lit walls drew as opaque rectangles. */
	if(( flags & 0x02 ) && ( !gc_wd_chunk_lm || ( flags & 0x08 )))
	{
		GX_SetAlphaCompare( GX_GEQUAL, aref ? aref : 128, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetZCompLoc( GX_FALSE );
	}
}
static void R_GXTevMatEndAlphaTest( void ) {}
/* Real Flipper fog lives in r_gx_bloom.c (d2_lostcoast env_fog_controller). */
extern void R_GXFogBegin( void );
extern void R_GXFogEnd( void );

static int gc_wd_state;	/* 0 untried, 1 ready, -1 absent */
static char gc_wd_map[64];
static size_t gc_wd_spill_mark;
static gc_wd_mat_t gc_wd_mats_local[GC_WD_MAX_MATS];
static const gc_wd_mat_t *gc_wd_mats = gc_wd_mats_local;
static gc_wd_slot_t *gc_wd_slots;
static s16 *gc_wd_texnum;
/* $basetexture2 texnum per material: 0 unresolved, -1 missing. */
#define GC_WD_BLEND_MATS 512
static s16 gc_wd_texnum2[GC_WD_BLEND_MATS];
static int blend_draws;
static u32 *gc_wd_chunk_bytes;	/* aligned size per chunk — avoids O(n²) ARAM dir fetches */
static u16 *gc_wd_chunk_cluster;
static s16 *gc_wd_chunk_mins;	/* n_chunks * 3 */
static s16 *gc_wd_chunk_maxs;
static u32 *gc_wd_chunk_ofs;
static byte *gc_wd_cache;
static qboolean gc_wd_cache_in_arena;
static qboolean cmpr_adopted;	/* the large-world branch adopted the CMPR slots this map */
/* More distinct world textures than full-size slots: distant chunks then
 * bind from the small-mip pool. Lost Coast has 82 after the bake shares one
 * texture across cubemap patch variants, so all of them stay at 128x128. */
static qboolean gc_wd_tex_overflow;
/* Flythrough profile of the draw loop (-gcvst), logged every 120 frames. */
static u64 gc_wd_pf[10], gc_wd_pf_t0, gc_wd_pf_t1, gc_wd_pf_enter;
static int gc_wd_pf_lm_miss;
static u32 gc_wd_pf_cnt[10];
static int gc_wd_pf_on = -1, gc_wd_pf_frames;
static u32 gc_wd_pf_bytes, gc_wd_pf_bytes_max;	/* chunk bytes drawn this frame / peak */
#define WD_PF( k, stmt ) do { if( gc_wd_pf_on > 0 ) { u64 pf_t = gettime(); stmt; gc_wd_pf[k] += gettime() - pf_t; gc_wd_pf_cnt[k]++; } else { stmt; } } while( 0 )
static u32 gc_wd_cache_size, gc_wd_cache_head;
static u32 gc_wd_aram;		/* entire file in ARAM */
static u32 gc_wd_aram_prefix, gc_wd_aram_lm_start, gc_wd_aram_lm_bytes;
static u32 gc_wd_disc_reads, gc_wd_disc_bytes;
static file_t *gc_wd_file;
static byte *gc_wd_pvs;
static int gc_wd_pvs_cluster = -2;
static byte gc_wd_scratch[1024] __attribute__(( aligned( 32 )));
/* 64 bytes: 6-byte nodes / 16-byte planes can straddle a 32-byte ARAM line. */
static byte gc_wd_node_tmp[64] __attribute__(( aligned( 32 )));
static byte gc_wd_plane_tmp[64] __attribute__(( aligned( 32 )));

/* Bytes [ofs, ofs+len) of the .gcd → dst (32-aligned). */
static qboolean R_GXWorldDLFetch( u32 file_ofs, void *dst, u32 len )
{
	len = ( len + 31u ) & ~31u;
	if( gc_wd_aram && file_ofs <= gc_wd_aram_prefix
		&& len <= gc_wd_aram_prefix - file_ofs )
	{
		DCInvalidateRange( dst, len );
		GC_AramTransfer( gc_wd_aram + file_ofs, dst, len, false );
		return true;
	}
	if( gc_wd_aram && file_ofs >= gc_wd_aram_lm_start
		&& file_ofs - gc_wd_aram_lm_start <= gc_wd_aram_lm_bytes
		&& len <= gc_wd_aram_lm_bytes - ( file_ofs - gc_wd_aram_lm_start ))
	{
		DCInvalidateRange( dst, len );
		GC_AramTransfer( gc_wd_aram + gc_wd_aram_prefix + file_ofs - gc_wd_aram_lm_start, dst, len, false );
		return true;
	}
	if( !gc_wd_file )
		return false;
	gEngfuncs.fsapi->Seek( gc_wd_file, (fs_offset_t)file_ofs, SEEK_SET );
	if( gEngfuncs.fsapi->Read( gc_wd_file, dst, len ) != (fs_offset_t)len )
		return false;
	gc_wd_disc_reads++;
	gc_wd_disc_bytes += len;
	DCFlushRange( dst, len );
	return true;
}

static void R_GXWorldDLUnload( void )
{
	if( gc_wd_file )
		gEngfuncs.fsapi->Close( gc_wd_file );
	gc_wd_file = NULL;
	gc_wd_cache = NULL;
	gc_wd_slots = NULL;
	gc_wd_texnum = NULL;
	gc_wd_pvs = NULL;
	gc_wd_aram = 0;
	gc_wd_aram_prefix = gc_wd_aram_lm_start = gc_wd_aram_lm_bytes = 0;
	gc_wd_disc_reads = gc_wd_disc_bytes = 0;
	gc_wd_pvs_cluster = -2;
}

static void R_GXMatPackLoad( void )
{
	file_t *f;
	byte hdr[8];
	u32 bytes, nmat;
	static qboolean logged;

	gc_tev_nmat = 0;
	gc_tev_ndet = 0;
	gc_tev_nmask = 0;
	gc_tev_bound = -1;
	gc_tev_envmask_bound = -1;
	f = gEngfuncs.fsapi->Open( "media/tevmat.gcm", "rb", false );
	if( f && gEngfuncs.fsapi->Read( f, hdr, 8 ) == 8
		&& hdr[0] == 'G' && hdr[1] == 'C' && hdr[2] == 'm' && hdr[3] == 't'
		&& hdr[4] == 0 && hdr[5] == 1 )
	{
		nmat = hdr[6];
		gc_tev_ndet = hdr[7];
		if( nmat > 0 && nmat <= GC_TEV_MAX && gc_tev_ndet <= 8
			&& gEngfuncs.fsapi->Read( f, gc_tev_rec, nmat * 8u ) == (fs_offset_t)( nmat * 8u ))
		{
			u32 ofs = ( 8u + nmat * 8u + 31u ) & ~31u;
			byte trail[8];

			bytes = (u32)gc_tev_ndet * GC_TEV_DETAIL;
			gEngfuncs.fsapi->Seek( f, (fs_offset_t)ofs, SEEK_SET );
			if( gc_tev_ndet == 0 || gEngfuncs.fsapi->Read( f, gc_tev_store, bytes ) == (fs_offset_t)bytes )
			{
				if( gc_tev_ndet )
					DCFlushRange( gc_tev_store, bytes );
				gc_tev_nmat = (u8)nmat;
				/* Optional GCem trailer: $basealphaenvmapmask I4 tiles. */
				if( gEngfuncs.fsapi->Read( f, trail, 8 ) == 8
					&& trail[0] == 'G' && trail[1] == 'C'
					&& trail[2] == 'e' && trail[3] == 'm'
					&& trail[4] > 0 && trail[4] <= GC_TEV_MAX_ENVMASK )
				{
					u32 mbytes = (u32)trail[4] * GC_TEV_ENVMASK;

					if( gEngfuncs.fsapi->Read( f, gc_tev_envmask, mbytes ) == (fs_offset_t)mbytes )
					{
						DCFlushRange( gc_tev_envmask, mbytes );
						gc_tev_nmask = trail[4];
					}
				}
			}
			else
				gc_tev_ndet = 0;
		}
		gEngfuncs.fsapi->Close( f );
	}
	else if( f )
		gEngfuncs.fsapi->Close( f );
	if( !logged )
	{
		logged = true;
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: tevmat mats=%u details=%u envmasks=%u\n",
			(unsigned)gc_tev_nmat, (unsigned)gc_tev_ndet, (unsigned)gc_tev_nmask );
	}
}

static void R_GXWorldDLLoad( const char *mapbase )
{
	char path[96];
	file_t *f;
	u32 tables, try_sz, i, done;
	byte *tbl;
	byte *bounce;

	gc_wd_state = -1;
	R_GXWorldDLUnload();
	if( gEngfuncs.Sys_CheckParm( "-gcnobspdl" ))
		return;
	Q_strncpy( gc_wd_mapbase, mapbase, sizeof( gc_wd_mapbase ));
	Q_snprintf( path, sizeof( path ), "media/wd_%s.gcd", mapbase );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL open %s\n", path );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL missing %s\n", path );
		return;
	}
	if( gEngfuncs.fsapi->Read( f, &gc_wd_hdr, sizeof( gc_wd_hdr )) != sizeof( gc_wd_hdr )
		|| gc_wd_hdr.magic != GC_WD_MAGIC
		|| ( gc_wd_hdr.version != 1 && gc_wd_hdr.version != 2 )
		|| gc_wd_hdr.n_mats > GC_WD_MAX_MATS || gc_wd_hdr.n_chunks > GC_WD_MAX_CHUNKS )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL pack reject %s\n", path );
		gEngfuncs.fsapi->Close( f );
		return;
	}

	/* slots + texnum + pvs + compact chunkdir mirror (bytes/cluster/mins/maxs/ofs) */
	tables = gc_wd_hdr.n_chunks * sizeof( gc_wd_slot_t )
		+ gc_wd_hdr.n_mats * sizeof( s16 )
		+ (( gc_wd_hdr.n_clusters + 7 ) >> 3 ) + 64
		+ gc_wd_hdr.n_chunks * ( sizeof( u32 ) * 2 + sizeof( u16 ) + sizeof( s16 ) * 6 );
	tbl = (byte *)Mod_GCWorldArraySpillTryAlloc( tables, "world DL tables" );
	if( !tbl )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL no spill for tables %u Kb\n",
			tables / 1024u );
		gEngfuncs.fsapi->Close( f );
		return;
	}
	gc_wd_slots = (gc_wd_slot_t *)tbl;
	gc_wd_texnum = (s16 *)( gc_wd_slots + gc_wd_hdr.n_chunks );
	gc_wd_pvs = (byte *)( gc_wd_texnum + gc_wd_hdr.n_mats );
	{
		byte *p = (byte *)( gc_wd_pvs + (( gc_wd_hdr.n_clusters + 7 ) >> 3 ) + 64 );

		p = (byte *)(((uintptr_t)p + 3u ) & ~3u );
		gc_wd_chunk_bytes = (u32 *)p;
		p += gc_wd_hdr.n_chunks * sizeof( u32 );
		gc_wd_chunk_ofs = (u32 *)p;
		p += gc_wd_hdr.n_chunks * sizeof( u32 );
		gc_wd_chunk_cluster = (u16 *)p;
		p += gc_wd_hdr.n_chunks * sizeof( u16 );
		gc_wd_chunk_mins = (s16 *)p;
		p += gc_wd_hdr.n_chunks * 3 * sizeof( s16 );
		gc_wd_chunk_maxs = (s16 *)p;
	}
	for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
	{
		gc_wd_slots[i].cache_ofs = -1;
		gc_wd_slots[i].used_frame = -1000;
	}
	memset( gc_wd_texnum2, 0, sizeof( gc_wd_texnum2 ));
	for( i = 0; i < gc_wd_hdr.n_mats; i++ )
		gc_wd_texnum[i] = -1;

	/* Material table + chunkdir compact mirror from the open file (pre-ARAM).
	 * Version 1 records are 8 bytes. Version 2 (Lost Coast) records are
	 * hash, hash2, tev, flags, pad — 12 bytes. */
	gEngfuncs.fsapi->Seek( f, (fs_offset_t)gc_wd_hdr.ofs_mats, SEEK_SET );
	if( gc_wd_hdr.version == 2 )
		gEngfuncs.fsapi->Read( f, gc_wd_mats_local, gc_wd_hdr.n_mats * sizeof( gc_wd_mat_t ));
	else
	{
		for( i = 0; i < gc_wd_hdr.n_mats; i++ )
		{
			u8 rec[8];

			if( gEngfuncs.fsapi->Read( f, rec, 8 ) != 8 )
				break;
			gc_wd_mats_local[i].tex_hash = *(u32 *)rec;
			gc_wd_mats_local[i].hash2 = 0;
			gc_wd_mats_local[i].tevmat = rec[4];
			gc_wd_mats_local[i].flags = rec[5];
			gc_wd_mats_local[i].pad = *(u16 *)( rec + 6 );
		}
	}
	gEngfuncs.fsapi->Seek( f, (fs_offset_t)gc_wd_hdr.ofs_chunkdir, SEEK_SET );
	for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
	{
		gc_wd_chunkdir_t d;

		if( gEngfuncs.fsapi->Read( f, &d, sizeof( d )) != (fs_offset_t)sizeof( d ))
			break;
		gc_wd_chunk_ofs[i] = d.ofs;
		gc_wd_chunk_bytes[i] = ( d.bytes + 31u ) & ~31u;
		gc_wd_chunk_cluster[i] = d.cluster;
		gc_wd_chunk_mins[i * 3 + 0] = d.mins[0];
		gc_wd_chunk_mins[i * 3 + 1] = d.mins[1];
		gc_wd_chunk_mins[i * 3 + 2] = d.mins[2];
		gc_wd_chunk_maxs[i * 3 + 0] = d.maxs[0];
		gc_wd_chunk_maxs[i * 3 + 1] = d.maxs[1];
		gc_wd_chunk_maxs[i * 3 + 2] = d.maxs[2];
	}

	/* Prefer a larger ring so PVS-visible chunks stay resident while the eye
	 * turns — still lazy (spill), never omit faces for a densify cheat.
	 * CapFaces listen map props (lean sherman/jagd/…) need ~256 KiB of the
	 * same spill after WorldDL; leave headroom (ava-prop-2 tip≈6 KiB heap). */
	gc_wd_cache = NULL;
	try_sz = 0;
	gc_wd_cache_in_arena = false;
	cmpr_adopted = false;
	/* Large worlds (Lost Coast, 3.5 MB of lists): the renderer's static
	 * map-load arena is idle once the BSP is in, with about 3 MB above its
	 * tail highwater. Keeping the whole visible set resident there ends
	 * the per-frame ARAM refetch of 800+ chunks that held the harbor at
	 * 3 fps. 256 KB stays free above for entity overflow carves. */
	if( gc_wd_hdr.file_size >= 1536u * 1024u && !gEngfuncs.Sys_CheckParm( "-gcworlddlsmallcache" ))
	{
		/* Start above every carve this map made, not above the highwater:
		 * a lean load lowers the highwater to 0 while the world-texture low
		 * mips it carved stay live (starting at 0 bleached them white). */
		const size_t live = Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak());
		/* The BSP surface table stays live inside the arena for the whole map and
		 * the highwater does not cover it: slots carved there were overwritten by
		 * its per-frame marks (and overwrote its records). */
		const size_t hw = ( Q_max( live, R_GXWorldDLBspFloor()) + 31u ) & ~(size_t)31u;
		const size_t cap = GC_MapLoadBufferCapacity();
		/* Left above the chunk ring: water table 34 KB, prop texture slots
		 * 96 KB, prop far LODs about 360 KB (284 KB with normals, pack v7), the fire atlas 33 KB, the water sun 2 KB, the resident skyline 42 KB, and entity
		 * overflow. The ring peaks near 915 KB in the Lost Coast harbor. */
		const size_t keep = 532u * 1024u;

		if( cap > hw + keep + 512u * 1024u )
		{
			size_t hw2 = hw;
			size_t want;

			/* Texture slots first: a near and a far pool; the near one holds a 128x128 CMPR chain
			 * (a 256x256 drops its top mip) for every texture in view. */
			GC_RetainMapLoadBufferBorrow();
			if( cap > hw + keep + 2048u * 1024u )
			{
				/* 90 full slots (128x128 chain, 10944 bytes): every world
				 * texture of Lost Coast at once. 128 slots one mip down
				 * (64x64 chain, 2752 bytes) for props. 1.31 MB. */
				/* 82 full slots, not one for each of the 90 world textures: a
				 * view does not bind all of them, and the eight slots (88 KB)
				 * make two big ones, a whole 256x256 chain each (43680 bytes),
				 * for the two coast rock textures the props put in front of
				 * the eye. The far pool stays at 128: the props fill it (96
				 * slots refetched 60 textures a frame). */
				const unsigned slot_bytes = 10944u, far_bytes = 2752u, big_bytes = 43680u;
				const int nslots = 82, far_n = 128, big_n = 2;
				byte *tex = (byte *)GC_MapLoadBufferTailAlloc( hw,
					(size_t)( nslots + 8 ) * slot_bytes + (size_t)far_n * far_bytes );

				if( tex )
				{
					R_GXCmprBounceAdopt( tex, nslots, slot_bytes, far_n, far_bytes );
					R_GXCmprBigAdopt( tex + (size_t)nslots * slot_bytes + (size_t)far_n * far_bytes, big_n, big_bytes );
					R_GXCmprBigForProps( true );
					cmpr_adopted = true;
					/* Not the far-pool mode for distant world chunks: that pool is
					 * the props'. The near slots turn over by least recent use. */
					gc_wd_tex_overflow = GC_WorldTexCount() > nslots + 8;
					hw2 = ( GC_MapLoadBufferTailHighwater() + 31u ) & ~(size_t)31u;
				}
			}
			want = ( cap - hw2 - keep ) & ~(size_t)31u;
			const size_t all = (( gc_wd_hdr.file_size + gc_wd_hdr.max_chunk + 63u ) & ~(size_t)31u );

			if( want > all )
				want = all;
			gc_wd_cache = (byte *)GC_MapLoadBufferTailAlloc( hw2, want );
			if( gc_wd_cache )
			{
				try_sz = (u32)want;
				gc_wd_cache_in_arena = true;
			}
		}
	}
	/* Worlds whose lists are under 1.5 MB (the DoD maps) skipped the adoption
	 * above, so the texture cache stayed at one heap slot: every packed bind
	 * missed and fence-waited (about 200 binds, 45 ms, on the Avalanche
	 * street). Their chunk cache comes from the spill, leaving the idle map
	 * arena above its carves unused. Lend the texture cache a share of that
	 * remainder, scaled to what is there; give the cache up to the old
	 * full set only when the arena is large. */
	/* A large world whose arena is too full for the branch above (dod_charlie:
	 * 2.4 MB of lists, 742 KB of arena left) took neither the chunk cache nor
	 * the slots there and fell back to one 24 KB heap slot: every packed bind
	 * missed and fence-waited, 60-120 ms of a 95-150 ms frame. It gets the same
	 * scaled share as a small world, provided nothing was carved above. */
	if(( gc_wd_hdr.file_size < 1536u * 1024u || ( !cmpr_adopted && !gc_wd_cache_in_arena ))
		&& !gEngfuncs.Sys_CheckParm( "-gcworlddlsmallcache" )
		&& !gEngfuncs.Sys_CheckParm( "-gcnocmprarena" ))
	{
		const size_t live = Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak());
		/* The BSP surface table stays live inside the arena for the whole map and
		 * the highwater does not cover it: slots carved there were overwritten by
		 * its per-frame marks (and overwrote its records). */
		const size_t hw = ( Q_max( live, R_GXWorldDLBspFloor()) + 31u ) & ~(size_t)31u;
		const size_t cap = GC_MapLoadBufferCapacity();
		const size_t keep = 352u * 1024u;	/* water table, prop slots, entity overflow */
		const size_t keep_tight = 64u * 1024u;	/* floor-limited arena: carves left above the slots */
		const unsigned slot_bytes = 10944u, far_bytes = 2752u;
		const size_t room = cap > hw ? cap - hw : 0;
		size_t budget = 0;
		int nslots, far_n = 0;
		byte *tex;

		if( room > keep + 192u * 1024u )
		{
			budget = ( room - keep ) / 2u;	/* leave half for later carves */
			if( budget > 1190u * 1024u )
				budget = 1190u * 1024u;
		}
		else if( room > keep_tight )
			budget = room - keep_tight;	/* live BSP scratch leaves only a tail */
		if( budget >= 760u * 1024u )
		{
			nslots = 64;
			far_n = (int)(( budget - (size_t)nslots * slot_bytes ) / far_bytes );
			if( far_n > 176 )
				far_n = 176;
		}
		else
		{
			nslots = (int)( budget / slot_bytes );
			if( nslots > 64 )
				nslots = 64;
		}
		if( nslots >= 8 )
		{
			GC_RetainMapLoadBufferBorrow();
			tex = (byte *)GC_MapLoadBufferTailAlloc( hw,
				(size_t)nslots * slot_bytes + (size_t)far_n * far_bytes );
			if( tex )
				R_GXCmprBounceAdopt( tex, nslots, slot_bytes, far_n, far_bytes );
			gEngfuncs.Con_Reportf( "Xash3D GameCube: CMPR slots %s: %d near + %d far, floor/hw %u cap %u room %u\n",
				tex ? "adopted" : "carve FAILED", nslots, far_n, (unsigned)hw, (unsigned)cap, (unsigned)room );
		}
		else
		{
			gEngfuncs.Con_Reportf( "Xash3D GameCube: CMPR slots skipped: budget %u < 8 slots, hw %u cap %u room %u\n",
				(unsigned)budget, (unsigned)hw, (unsigned)cap, (unsigned)room );
			/* The floor above is the whole BSP scratch, but the world arrays
			 * in it leave gaps (the post-load claims for entity overflow and
			 * client entities find them the same way, GC_MapArenaFind). Take
			 * full 24 KB slots there, the bounce's size, so no texture drops a
			 * mip: about 25 of them hold every texture a charlie frame binds,
			 * and a resident bind never waits for the GP. */
			if( !gEngfuncs.Sys_CheckParm( "-gcnoarenagap" ))
			{
				/* Plus a few whole-chain slots for 256x256 textures (about
				 * 43 KB with L0): in a 24 KB slot they lose L0 and the beach
				 * and trench ground goes soft. -gccmprbig N adds N (default none:
				 * on charlie fewer 24 KB slots cost more misses than they save). */
				const size_t small = 24u * 1024u;
				const size_t big = 44u * 1024u;
				const size_t keep_gap = 160u * 1024u;	/* later claims (overflow, entity tables) */
				const size_t gap = GC_MapArenaLargestGap();
				const size_t avail = gap > keep_gap ? gap - keep_gap : 0;
				int bn = 0, gn;
				byte *gt = NULL;
				char nb[8];

				if( _Sys_GetParmFromCmdLine( "-gccmprbig", nb, sizeof( nb )))
					bn = Q_atoi( nb );
				bn = bound( 0, bn, 16 );
				while( bn > 0 && avail < (size_t)bn * big + 8u * small )
					bn--;
				gn = (int)(( avail - (size_t)bn * big ) / small );
				if( gn > 32 )
					gn = 32;
				for( ; gn >= 8 && !gt; gn-- )
				{
					const size_t bytes = (size_t)gn * small + (size_t)bn * big;
					const size_t at = GC_MapArenaFind( bytes );

					if( at == (size_t)-1 )
						continue;
					GC_RetainMapLoadBufferBorrow();
					gt = (byte *)GC_MapLoadBufferTailAlloc( at, bytes );
					if( gt )
					{
						R_GXCmprBounceAdopt( gt, gn, (unsigned)small, 0, 0 );
						if( bn )
							R_GXCmprBigAdopt( gt + (size_t)gn * small, bn, (unsigned)big );
						break;
					}
				}
				gEngfuncs.Con_Reportf( "Xash3D GameCube: CMPR slots %s from arena gap: %d x %u + %d x %u whole-chain (largest gap %u)\n",
					gt ? "adopted" : "skipped", gt ? gn : 0, (unsigned)small, gt ? bn : 0, (unsigned)big, (unsigned)gap );
			}
			/* Experimental (-gcslotsspill): the arena is full, so take the slots
			 * from the world-array spill, leaving 48 KiB for props. */
			if( gEngfuncs.Sys_CheckParm( "-gcslotsspill" ))
			{
				/* What stays free after the CMPR bounce borrow, which comes
			 * later and takes the top 32 KiB of the spill. */
			const size_t sfree = Mod_GCWorldArraySpillFreeLasting();
				const size_t keep_props = 48u * 1024u;
				const size_t sb = sfree > keep_props ? sfree - keep_props : 0;
				const int sn = (int)Q_min( sb / slot_bytes, 64u );

				if( sn >= 8 )
				{
					byte *st = (byte *)Mod_GCWorldArraySpillTryAlloc( (size_t)sn * slot_bytes, "cmpr-slots" );

					if( st )
					{
						Mod_GCWorldArraySpillPinBase( st, (size_t)sn * slot_bytes );
						R_GXCmprBounceAdopt( st, sn, slot_bytes, 0, far_bytes );
					}
					gEngfuncs.Con_Reportf( "Xash3D GameCube: CMPR slots %s from spill: %d near (spill free %u)\n",
						st ? "adopted" : "carve FAILED", sn, (unsigned)sfree );
				}
			}
		}
	}
	if( !gc_wd_cache )
	{
		const unsigned cache_hi =
			( gEngfuncs.Sys_CheckParm( "-gcworldrender" )
				&& gEngfuncs.Sys_CheckParm( "-gcplayerinput" ))
			? ( 192u * 1024u ) : ( 512u * 1024u );
		unsigned cache_cap = cache_hi;
		unsigned cache_lo = 96u * 1024u;

		/* Day of Defeat team bodies (gc_studio/<team>.mdl, about 260 KiB each)
		 * load into the spill when a player of that team shows up. Sizing the
		 * chunk cache to all free spill left a bot's axis-inf core 29 KiB
		 * short (ceres f152a6b: 229.78 of 259.16 Kb). Leave room for both
		 * cores, or at least one if the cache would drop below its floor. */
		if( cache_lo < gc_wd_hdr.max_chunk * 2u )
			cache_lo = ( gc_wd_hdr.max_chunk * 2u + 32767u ) & ~32767u;
		{
			static const char *const team_cores[] = { "gc_studio/us-inf.mdl", "gc_studio/axis-inf.mdl" };
			/* What stays free after the CMPR bounce borrow, which comes
			 * later and takes the top 32 KiB of the spill. */
			const size_t sfree = Mod_GCWorldArraySpillFreeLasting();
			size_t core[2] = { 0, 0 };
			int i, keep;

			for( i = 0; i < 2; i++ )
			{
				fs_offset_t sz = gEngfuncs.fsapi->FileSize( team_cores[i], false );

				if( sz > 0 )
					core[i] = ALIGN( (size_t)sz, 32 ) + 4096u;
			}
			for( keep = 2; keep >= 1; keep-- )
			{
				/* One body: whichever team core is larger. */
				size_t reserve = keep == 2 ? core[0] + core[1] : Q_max( core[0], core[1] );
				size_t room;

				if( !reserve )
					break;
				/* Plus the animation/mesh ring (Mod_GCAnimPoolReady), which
				 * falls back to the spill when the studio arena is full. */
				reserve += GC_WD_BODY_RING_RESERVE;
				room = sfree > reserve ? ( sfree - reserve ) & ~(size_t)32767u : 0;
				if( room >= cache_lo || keep == 1 )
				{
					if( room < cache_cap )
						cache_cap = (unsigned)Q_max( room, (size_t)cache_lo );
					gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL chunk cache cap %u KiB, keeping %u KiB spill for %d team bod%s (spill free %u KiB)\n",
						cache_cap / 1024u, (unsigned)( reserve / 1024u ), keep, keep == 1 ? "y" : "ies",
						(unsigned)( sfree / 1024u ));
					break;
				}
			}
		}

		for( try_sz = cache_cap; try_sz >= 96u * 1024u; try_sz -= 32u * 1024u )
		{
			gc_wd_cache = (byte *)Mod_GCWorldArraySpillTryAlloc( try_sz, "world DL chunk cache" );
			if( gc_wd_cache )
				break;
		}
	}
	if( !gc_wd_cache || try_sz < gc_wd_hdr.max_chunk * 2u )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL no spill for chunk cache\n" );
		gEngfuncs.fsapi->Close( f );
		return;
	}
	gc_wd_cache_size = try_sz;
	gc_wd_cache_head = 0;
	if( gc_wd_cache_in_arena )
		Mod_GCWorldArraySpillPinBase( tbl, tables );
	else
		Mod_GCWorldArraySpillPinBase( tbl, (size_t)(( gc_wd_cache + try_sz ) - tbl ));
	gc_wd_spill_mark = Mod_GCWorldArraySpillUsed();

	/* Lazy policy (Codex's pvs-lazy work), opt in with -gclazyworld: keep
	 * tree/PVS and lightmaps in ARAM and pull the geometry chunks from the
	 * open world pack on the disc, which frees about 1.4 MB of ARAM. Off by
	 * default: in the Lost Coast harbor it costs about 16 fps (40 -> 24 in a
	 * Dolphin Null-backend ride) because every cache miss reads the disc. */
	gc_wd_aram_prefix = gc_wd_hdr.file_size;
	gc_wd_aram_lm_start = gc_wd_hdr.file_size;
	gc_wd_aram_lm_bytes = 0;
	if( gc_wd_hdr.version == 2 && !Q_stricmp( mapbase, "d2_lostcoast" )
		&& gEngfuncs.Sys_CheckParm( "-gclazyworld" ))
	{
		u32 last_end = 0;
		for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
		{
			u32 end = gc_wd_chunk_ofs[i] + gc_wd_chunk_bytes[i];
			if( end > last_end ) last_end = end;
		}
		gc_wd_aram_lm_start = gc_wd_hdr.ofs_chunks + (( last_end + 31u ) & ~31u );
		if( gc_wd_aram_lm_start <= gc_wd_hdr.file_size )
		{
			gc_wd_aram_prefix = gc_wd_hdr.ofs_chunks;
			gc_wd_aram_lm_bytes = gc_wd_hdr.file_size - gc_wd_aram_lm_start;
		}
	}
	gc_wd_aram = GC_AramWorldDLReserve( gc_wd_aram_prefix + gc_wd_aram_lm_bytes );
	bounce = gc_wd_cache;
	done = 0;
	if( gc_wd_aram )
	{
		gEngfuncs.fsapi->Seek( f, 0, SEEK_SET );
		while( done < gc_wd_aram_prefix )
		{
			u32 n = gc_wd_aram_prefix - done;

			if( n > 64u * 1024u )
				n = 64u * 1024u;
			if( n > gc_wd_cache_size )
				n = gc_wd_cache_size;
			if( gEngfuncs.fsapi->Read( f, bounce, n ) != (fs_offset_t)n )
				break;
			DCFlushRange( bounce, ( n + 31u ) & ~31u );
			GC_AramTransfer( gc_wd_aram + done, bounce, ( n + 31u ) & ~31u, true );
			done += n;
		}
		if( done < gc_wd_aram_prefix )
			gc_wd_aram = 0;
		if( gc_wd_aram && gc_wd_aram_lm_bytes )
		{
			gEngfuncs.fsapi->Seek( f, (fs_offset_t)gc_wd_aram_lm_start, SEEK_SET );
			done = 0;
			while( done < gc_wd_aram_lm_bytes )
			{
				u32 n = Q_min( gc_wd_aram_lm_bytes - done, 64u * 1024u );
				if( n > gc_wd_cache_size ) n = gc_wd_cache_size;
				if( gEngfuncs.fsapi->Read( f, bounce, n ) != (fs_offset_t)n ) break;
				DCFlushRange( bounce, ( n + 31u ) & ~31u );
				GC_AramTransfer( gc_wd_aram + gc_wd_aram_prefix + done, bounce, ( n + 31u ) & ~31u, true );
				done += n;
			}
			if( done < gc_wd_aram_lm_bytes ) gc_wd_aram = 0;
		}
	}
	if( gc_wd_aram && gc_wd_aram_prefix == gc_wd_hdr.file_size )
		gEngfuncs.fsapi->Close( f );
	else
		gc_wd_file = f;
	if( gc_wd_aram && gc_wd_file )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL lazy disc chunks prefix=%u lightmaps=%u deferred=%u bytes\n",
			gc_wd_aram_prefix, gc_wd_aram_lm_bytes,
			gc_wd_aram_lm_start - gc_wd_aram_prefix );

	/* Lightmap page stream sits after the last padded chunk blob. */
	gc_wd_lm_stream = 0;
	gc_wd_chunk_lm = 0;
	if( gc_wd_hdr.version == 2 && gc_wd_hdr.n_chunks > 0 )
	{
		u32 last_end = 0;

		for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
		{
			u32 end = gc_wd_chunk_ofs[i] + gc_wd_chunk_bytes[i];

			if( end > last_end )
				last_end = end;
		}
		gc_wd_lm_stream = gc_wd_hdr.ofs_chunks + (( last_end + 31u ) & ~31u );
		for( i = 0; i < GC_WD_LM_SLOTS; i++ )
		{
			gc_wd_lm_slot_page[i] = -1;
			gc_wd_lm_slot_frame[i] = -1000;
		}
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: world DL lightmap pages stream@%u slots=%d (%u Kb)\n",
			(unsigned)gc_wd_lm_stream, GC_WD_LM_SLOTS,
			(unsigned)( GC_WD_LM_SLOTS * GC_WD_LM_PAGE_BYTES / 1024u ));
	}

	gc_wd_tev_gens = gc_wd_hdr.version == 2 ? 2 : 1;
	gc_wd_pvs_cluster = -2;
	/* The tail of the chunk cache is the overflow bounce. A frustum chunk
	 * that does not fit the shore budget still draws from here. */
	{
		u32 tail = ( gc_wd_hdr.max_chunk + 31u ) & ~31u;

		gc_wd_force = NULL;
		gc_wd_force_size = 0;
		if( tail >= 4096u && gc_wd_cache_size > tail + 96u * 1024u )
		{
			gc_wd_cache_size -= tail;
			gc_wd_force = gc_wd_cache + gc_wd_cache_size;
			gc_wd_force_size = tail;
		}
	}
	GC_HeapCheck( "world DL before mat pack" );
	R_GXMatPackLoad();
	gc_wd_state = 1;
	gEngfuncs.Con_Reportf(
		"Xash3D GameCube: world DL %s v%u clusters=%u chunks=%u mats=%u cache=%u Kb file=%u Kb (%s)\n",
		path, gc_wd_hdr.version, gc_wd_hdr.n_clusters, gc_wd_hdr.n_chunks, gc_wd_hdr.n_mats,
		gc_wd_cache_size / 1024u, gc_wd_hdr.file_size / 1024u,
		gc_wd_aram ? ( gc_wd_file ? "ARAM+disc" : "ARAM" ) : "disc" );
}

/* Light points without a live msurface table: media/wd_<map>_lf.gcd holds
 * each face's light mapping (texinfo vecs, texturemins, extents, flags,
 * lightofs, styles) and the BSP lighting lump. It lives in ARAM; the light
 * point walk in ref_light.c reads one node's faces and a few samples. */
#define GC_LF_REC 56u
static u32 gc_lf_aram, gc_lf_cap, gc_lf_faces, gc_lf_rec_ofs, gc_lf_lit_ofs, gc_lf_lit_bytes;

static void R_GXLightFacesLoad( const char *mapbase )
{
	char path[96];
	file_t *f;
	u32 hdr[8] __attribute__(( aligned( 32 )));
	u32 size, done;

	gc_lf_faces = 0;
	if( !gc_wd_cache || gc_wd_cache_size < 32u || gEngfuncs.Sys_CheckParm( "-gcnolightpoints" ))
		return;
	Q_snprintf( path, sizeof( path ), "media/wd_%s_lf.gcd", mapbase );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: light faces missing %s\n", path );
		return;
	}
	if( gEngfuncs.fsapi->Read( f, hdr, 32 ) != 32 || memcmp( hdr, "GClf", 4 ) || hdr[1] != 1u
		|| hdr[3] != 32u || hdr[4] < 32u + hdr[2] * GC_LF_REC )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: light faces bad header %s\n", path );
		gEngfuncs.fsapi->Close( f );
		return;
	}
	size = ( hdr[4] + hdr[5] + 31u ) & ~31u;
	if( !gc_lf_aram || gc_lf_cap < size )
	{
		if( gc_lf_aram )
			GC_AramFree( gc_lf_aram );
		gc_lf_aram = GC_AramAllocTagged( size, true, "lightfaces" );
		gc_lf_cap = gc_lf_aram ? size : 0;
	}
	if( !gc_lf_aram )
	{
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: light faces: no ARAM for %u bytes\n", size );
		gEngfuncs.fsapi->Close( f );
		return;
	}
	gEngfuncs.fsapi->Seek( f, 0, SEEK_SET );
	for( done = 0; done < size; )
	{
		u32 n = Q_min( size - done, 64u * 1024u );
		fs_offset_t got;

		if( n > gc_wd_cache_size )
			n = gc_wd_cache_size & ~31u;
		got = gEngfuncs.fsapi->Read( f, gc_wd_cache, n );
		if( got <= 0 )
			break;
		GC_AramCopy( gc_lf_aram + done, gc_wd_cache, (u32)got, true );
		done += (u32)got;
		if( (u32)got < n )
			break;
	}
	gEngfuncs.fsapi->Close( f );
	if( done < hdr[4] + hdr[5] )
	{
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: light faces short read %u/%u\n", done, hdr[4] + hdr[5] );
		return;
	}
	gc_lf_rec_ofs = hdr[3];
	gc_lf_lit_ofs = hdr[4];
	gc_lf_lit_bytes = hdr[5];
	gc_lf_faces = hdr[2];
	gEngfuncs.Con_Reportf( "Xash3D GameCube: light faces %s faces=%u lighting=%u bytes (ARAM %u)\n",
		path, gc_lf_faces, gc_lf_lit_bytes, size );
}

int R_GXLightFacesCount( void )
{
	return gc_lf_aram ? (int)gc_lf_faces : 0;
}

qboolean R_GXLightFacesRead( int first, int num, void *out )
{
	if( !gc_lf_faces || first < 0 || num <= 0 || (u32)( first + num ) > gc_lf_faces )
		return false;
	GC_AramCopy( gc_lf_aram + gc_lf_rec_ofs + (u32)first * GC_LF_REC, out, (u32)num * GC_LF_REC, false );
	return true;
}

qboolean R_GXLightSamplesRead( u32 ofs, void *out, u32 len )
{
	if( !gc_lf_faces || ofs >= gc_lf_lit_bytes || len > gc_lf_lit_bytes - ofs )
		return false;
	GC_AramCopy( gc_lf_aram + gc_lf_lit_ofs + ofs, out, len, false );
	return true;
}

qboolean R_GXWorldDLActive( void )
{
	model_t *world = WORLDMODEL;
	char base[64];

	if( !world || !world->name[0] )
		return false;
	COM_FileBase( world->name, base, sizeof( base ));
	if( gc_wd_state != 0 && ( Q_stricmp( base, gc_wd_map )
		|| ( gc_wd_state == 1 && Mod_GCWorldArraySpillUsed() < gc_wd_spill_mark )))
		gc_wd_state = 0;
	if( gc_wd_state == 0 )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL Active load map=%s\n", base );
		GC_HeapCheck( "world DL load begin" );
		Q_strncpy( gc_wd_map, base, sizeof( gc_wd_map ));
		R_GXWorldDLLoad( base );
		R_GXLightFacesLoad( base );
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL Active done state=%d\n", gc_wd_state );
		GC_HeapCheck( "world DL load done" );
	}
	return gc_wd_state == 1;
}

static int R_GXWorldDLEyeCluster( const float *org )
{
	int idx = 0;
	u32 guard = 0;

	while( idx >= 0 )
	{
		u32 nofs, pofs;
		u16 plane_i;
		s16 c0, c1;
		float d;

		if( (u32)idx >= gc_wd_hdr.n_nodes || ++guard > gc_wd_hdr.n_nodes + 8u )
			return -1;
		nofs = gc_wd_hdr.ofs_nodes + (u32)idx * 6u;
		if( !R_GXWorldDLFetch( nofs & ~31u, gc_wd_node_tmp, 64 ))
			return -1;
		plane_i = *(const u16 *)( gc_wd_node_tmp + ( nofs & 31u ));
		c0 = *(const s16 *)( gc_wd_node_tmp + ( nofs & 31u ) + 2 );
		c1 = *(const s16 *)( gc_wd_node_tmp + ( nofs & 31u ) + 4 );
		if( plane_i >= gc_wd_hdr.n_planes )
			return -1;
		pofs = gc_wd_hdr.ofs_planes + (u32)plane_i * 16u;
		if( !R_GXWorldDLFetch( pofs & ~31u, gc_wd_plane_tmp, 64 ))
			return -1;
		{
			const float *p = (const float *)( gc_wd_plane_tmp + ( pofs & 31u ));

			d = p[0] * org[0] + p[1] * org[1] + p[2] * org[2] - p[3];
		}
		idx = ( d >= 0.0f ) ? c0 : c1;
	}
	return idx == -1 ? -1 : -idx - 2;
}

static void R_GXWorldDLLoadPVS( int cluster )
{
	const u32 visbytes = ( gc_wd_hdr.n_clusters + 7 ) >> 3;
	u32 dir_ofs, row, out = 0, in, base;

	gc_wd_pvs_cluster = cluster;
	memset( gc_wd_pvs, 0xff, visbytes );
	if( cluster < 0 )
		return;
	dir_ofs = gc_wd_hdr.ofs_pvsdir + (u32)cluster * 4u;
	base = dir_ofs & ~31u;
	if( !R_GXWorldDLFetch( base, gc_wd_scratch, 32 ))
		return;
	row = *(const u32 *)( gc_wd_scratch + ( dir_ofs - base ));
	if( row == 0xffffffffu )
		return;
	row += gc_wd_hdr.ofs_pvs;
	base = row & ~31u;
	if( !R_GXWorldDLFetch( base, gc_wd_scratch, sizeof( gc_wd_scratch )))
		return;
	in = row - base;
	memset( gc_wd_pvs, 0, visbytes );
	while( out < visbytes )
	{
		if( in + 2 > sizeof( gc_wd_scratch ))
		{
			row = base + in;
			base = row & ~31u;
			if( !R_GXWorldDLFetch( base, gc_wd_scratch, sizeof( gc_wd_scratch )))
				break;
			in = row - base;
		}
		if( gc_wd_scratch[in] )
			gc_wd_pvs[out++] = gc_wd_scratch[in++];
		else
		{
			out += gc_wd_scratch[in + 1];
			in += 2;
		}
	}
}

static const float *gc_wd_sort_org;

static float R_GXWorldDLChunkD2( u32 i )
{
	float d = 0.0f;
	int a;

	for( a = 0; a < 3; a++ )
	{
		const float c = gc_wd_sort_org[a];
		const float mn = (float)gc_wd_chunk_mins[i * 3 + a];
		const float mx = (float)gc_wd_chunk_maxs[i * 3 + a];
		const float t = c < mn ? mn : ( c > mx ? mx : c );
		const float e = c - t;

		d += e * e;
	}
	/* Nearby ground stays first. Chunks whose center is behind the eye
	 * would otherwise fill the cache with the bank we are standing past
	 * and leave the harbor shore out. */
	if( d > ( 400.0f * 400.0f ))
	{
		const float cx = ( (float)gc_wd_chunk_mins[i * 3 + 0] + (float)gc_wd_chunk_maxs[i * 3 + 0] ) * 0.5f;
		const float cy = ( (float)gc_wd_chunk_mins[i * 3 + 1] + (float)gc_wd_chunk_maxs[i * 3 + 1] ) * 0.5f;
		const float fwd = ( cx - gc_wd_sort_org[0] ) * RI.cull_vforward[0]
			+ ( cy - gc_wd_sort_org[1] ) * RI.cull_vforward[1];

		if( fwd < 0.0f )
			d += 10000000000.0f;
		else
		{
			const float hz = (float)gc_wd_chunk_maxs[i * 3 + 2]
				- (float)gc_wd_chunk_mins[i * 3 + 2];
			const float cz = ( (float)gc_wd_chunk_mins[i * 3 + 2]
				+ (float)gc_wd_chunk_maxs[i * 3 + 2] ) * 0.5f;

			/* Prefer harbor stacks / cliff faces in the sort. */
			if( hz > 480.0f && cz > 0.0f && d > ( 600.0f * 600.0f ))
			{
				const float dist = sqrtf( d );
				const float pulled = dist - 1000.0f;

				d = pulled > 1.0f ? pulled * pulled : 1.0f;
			}
		}
	}
	return d;
}

/*
 * PVS test for an entity box. The world DL now streams its BSP nodes and planes
 * from ARAM instead of keeping them resident, so walking the tree per entity
 * would cost a DMA per node. Until a resident node copy exists every entity
 * counts as visible here; brush entities are still frustum culled by the caller.
 */
qboolean R_GXWorldDLEntityBoxVisible( const vec3_t mins, const vec3_t maxs )
{
	(void)mins;
	(void)maxs;
	return true;
}

/* Chunks resident in the cache, sorted by offset, and this frame's smallest
 * request that found no room (anything as large fails without a search). */
#define GC_WD_RES_MAX	2048
static u16 gc_wd_res[GC_WD_RES_MAX];
static u32 gc_wd_res_n, gc_wd_res_fail;
static int gc_wd_res_fail_frame = -1;

static byte *R_GXWorldDLResident( u32 i, u32 *budget )
{
	const u32 bytes = gc_wd_chunk_bytes[i];
	u32 a, b, k, n, best_a = 0, best_b = 0, best_ofs = 0;
	s32 best_age = 0x7fffffff;

	if( gc_wd_slots[i].cache_ofs >= 0 )
		return gc_wd_cache + gc_wd_slots[i].cache_ofs;
	if( bytes > *budget || bytes > gc_wd_cache_size || bytes == 0 || gc_wd_hdr.n_chunks > GC_WD_RES_MAX )
		return NULL;
	if( gc_wd_res_fail_frame == gc_wd_frame && bytes >= gc_wd_res_fail )
		return NULL;
	/* The cache was emptied behind the list's back (map load). */
	if( gc_wd_cache_head == 0 )
		gc_wd_res_n = 0;
	gc_wd_cache_head = 1;
	n = gc_wd_res_n;
	/* Every run of neighbours that would make room: take a free gap when
	 * there is one, else the run whose most recent use is oldest. Chunks
	 * drawn this frame stay (the GP may still be reading them; DrawDone at
	 * the start of the frame retired last frame's lists). A ring head used
	 * to land on this frame's chunks, so dozens were re-read from ARAM every
	 * frame even with room to spare. */
	for( a = 0; a <= n; a++ )
	{
		const u32 start = a ? (u32)gc_wd_slots[gc_wd_res[a - 1]].cache_ofs + gc_wd_chunk_bytes[gc_wd_res[a - 1]] : 0u;
		s32 age = -0x7fffffff;

		for( b = a; ; b++ )
		{
			const u32 end = b < n ? (u32)gc_wd_slots[gc_wd_res[b]].cache_ofs : gc_wd_cache_size;

			if( end >= start + bytes )
			{
				if( age < best_age )
				{
					best_age = age; best_a = a; best_b = b; best_ofs = start;
				}
				break;
			}
			if( b >= n )
				break;
			if( gc_wd_slots[gc_wd_res[b]].used_frame == gc_wd_frame )
			{
				a = b;	/* no run crosses a chunk in use */
				break;
			}
			if( gc_wd_slots[gc_wd_res[b]].used_frame > age )
				age = gc_wd_slots[gc_wd_res[b]].used_frame;
		}
		if( best_age == -0x7fffffff )
			break;
	}
	if( best_age == 0x7fffffff )
	{
		if( gc_wd_res_fail_frame != gc_wd_frame || bytes < gc_wd_res_fail )
			gc_wd_res_fail = bytes;
		gc_wd_res_fail_frame = gc_wd_frame;
		return NULL;
	}
	if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_chunks + gc_wd_chunk_ofs[i], gc_wd_cache + best_ofs, bytes ))
		return NULL;
	for( k = best_a; k < best_b; k++ )
		gc_wd_slots[gc_wd_res[k]].cache_ofs = -1;
	memmove( &gc_wd_res[best_a + 1], &gc_wd_res[best_b], ( n - best_b ) * sizeof( gc_wd_res[0] ));
	gc_wd_res[best_a] = (u16)i;
	gc_wd_res_n = n - ( best_b - best_a ) + 1u;
	gc_wd_slots[i].cache_ofs = (s32)best_ofs;
	*budget -= bytes;
	return gc_wd_cache + best_ofs;
}

/* Cache miss. The chunk is inside the frustum, so draw it from the tail
 * bounce instead of skipping it. Caller retires the GP before reuse. */
static byte *R_GXWorldDLForce( u32 i )
{
	const u32 bytes = gc_wd_chunk_bytes[i];

	if( !gc_wd_force || bytes == 0 || bytes > gc_wd_force_size )
		return NULL;
	GX_DrawDone();
	if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_chunks + gc_wd_chunk_ofs[i], gc_wd_force, bytes ))
		return NULL;
	return gc_wd_force;
}

static void R_GXWorldDLFrustum( vec3_t normals[4] )
{
	vec3_t fwd, right, up;
	const float fx = ( RI.rvp.fov_x > 1.0f ? RI.rvp.fov_x : 90.0f ) * (float)M_PI_F / 360.0f;
	const float fy = ( RI.rvp.fov_y > 1.0f ? RI.rvp.fov_y : 90.0f ) * (float)M_PI_F / 360.0f;
	const float sx = sinf( fx ), cx = cosf( fx ), sy = sinf( fy ), cy = cosf( fy );
	int i;

	AngleVectors( RI.rvp.viewangles, fwd, right, up );
	for( i = 0; i < 3; i++ )
	{
		normals[0][i] = fwd[i] * sx + right[i] * cx;
		normals[1][i] = fwd[i] * sx - right[i] * cx;
		normals[2][i] = fwd[i] * sy + up[i] * cy;
		normals[3][i] = fwd[i] * sy - up[i] * cy;
	}
}

static qboolean R_GXWorldDLBoxVisible( u32 i, vec3_t normals[4], const float *org )
{
	int p, a;
	const s16 *mins = &gc_wd_chunk_mins[i * 3];
	const s16 *maxs = &gc_wd_chunk_maxs[i * 3];

	for( p = 0; p < 4; p++ )
	{
		float dist = 0.0f;

		for( a = 0; a < 3; a++ )
			dist += normals[p][a] * (( normals[p][a] >= 0.0f ? (float)maxs[a] : (float)mins[a] ) - org[a] );
		if( dist < -8.0f )
			return false;
	}
	return true;
}

/* Entity visibility for the client's draw list in a world DL map: false when
 * the box sits in a cluster the eye cannot see (the map's own PVS row) or
 * well outside the view. The row and the view are last frame's, so the
 * frustum test keeps a margin of 15 % of the distance. Without this every
 * character in the map was animated and skinned every frame (30 ms in the
 * Lost Coast harbor). `key` identifies the entity for the cluster memo. */
qboolean R_GXWorldDLEntityVisible( int key, const float *mins, const float *maxs )
{
	static struct { int key; float org[3]; int cluster; } memo[32];
	static vec3_t normals[4];
	static int normals_frame = -1;
	const float *eye = RI.rvp.vieworigin;
	vec3_t c;
	float r, d;
	int p, cl;

	if( gc_wd_state != 1 || !gc_wd_pvs || gc_wd_pvs_cluster == -2 || gc_wd_frame < 2 )
		return true;
	c[0] = 0.5f * ( mins[0] + maxs[0] );
	c[1] = 0.5f * ( mins[1] + maxs[1] );
	c[2] = 0.5f * ( mins[2] + maxs[2] );
	r = 0.5f * ( fabsf( maxs[0] - mins[0] ) + fabsf( maxs[1] - mins[1] ) + fabsf( maxs[2] - mins[2] )) + 32.0f;
	if( normals_frame != gc_wd_frame )
	{
		R_GXWorldDLFrustum( normals );
		normals_frame = gc_wd_frame;
	}
	d = fabsf( c[0] - eye[0] ) + fabsf( c[1] - eye[1] ) + fabsf( c[2] - eye[2] );
	for( p = 0; p < 4; p++ )
	{
		if( normals[p][0] * ( c[0] - eye[0] ) + normals[p][1] * ( c[1] - eye[1] )
			+ normals[p][2] * ( c[2] - eye[2] ) < -( r + 0.15f * d ))
			return false;
	}
	p = key & 31;
	if( memo[p].key != key + 1 || fabsf( memo[p].org[0] - c[0] ) + fabsf( memo[p].org[1] - c[1] )
		+ fabsf( memo[p].org[2] - c[2] ) > 16.0f )
	{
		memo[p].key = key + 1;
		VectorCopy( c, memo[p].org );
		memo[p].cluster = R_GXWorldDLEyeCluster( c );
	}
	cl = memo[p].cluster;
	if( cl < 0 || (u32)cl >= gc_wd_hdr.n_clusters )
		return true;
	return ( gc_wd_pvs[cl >> 3] & ( 1 << ( cl & 7 ))) != 0;
}

/* Harbor industrial stacks: tall and narrow. Wide cliff plates must stay on
 * the shore pass — SpecOn fill turns them into a white sheet. */
static qboolean R_GXWorldDLIsTower( u32 i )
{
	const float hx = (float)gc_wd_chunk_maxs[i * 3 + 0]
		- (float)gc_wd_chunk_mins[i * 3 + 0];
	const float hy = (float)gc_wd_chunk_maxs[i * 3 + 1]
		- (float)gc_wd_chunk_mins[i * 3 + 1];
	const float hz = (float)gc_wd_chunk_maxs[i * 3 + 2]
		- (float)gc_wd_chunk_mins[i * 3 + 2];
	const float cz = ( (float)gc_wd_chunk_mins[i * 3 + 2]
		+ (float)gc_wd_chunk_maxs[i * 3 + 2] ) * 0.5f;
	const float horiz = hx > hy ? hx : hy;
	float dx, dy, dz;

	/* Needle industrial stacks only. horiz < 0.80·hz matched 1k-wide cliff
	 * volumes (SpecOn → white vertical wash on the harbor wall). */
	if( hz <= 480.0f || cz <= 32.0f || !gc_wd_sort_org )
		return false;
	if( horiz > 280.0f || horiz > hz * 0.40f )
		return false;
	dx = ( (float)gc_wd_chunk_mins[i * 3 + 0] + (float)gc_wd_chunk_maxs[i * 3 + 0] ) * 0.5f
		- gc_wd_sort_org[0];
	dy = ( (float)gc_wd_chunk_mins[i * 3 + 1] + (float)gc_wd_chunk_maxs[i * 3 + 1] ) * 0.5f
		- gc_wd_sort_org[1];
	dz = cz - gc_wd_sort_org[2];
	return ( dx * dx + dy * dy + dz * dz ) > ( 520.0f * 520.0f );
}

/* Lost Coast sky_camera is at (0, 0, -5108) with scale 16. The tower row
 * lives in that volume. Drawing it here, scaled onto the horizon, is the
 * distant land. The sky cube above stays the existing pass.
 * One bounce is not enough: the GP reads the position array when it
 * reaches the display list, and the next ARAM fetch overwrites it first.
 * That tore the distant chunks into a sheet over the sky. Four slots,
 * retired with DrawDone before a slot is reused. */
#define GC_WD_SKY_RING	4
static byte gc_wd_sky_ring[GC_WD_SKY_RING][4096] __attribute__(( aligned( 32 )));
static int gc_wd_sky_ring_i;
static int gc_wd_sky_ring_n;

static byte *R_GXWorldDLSkySlot( void )
{
	byte *slot;

	if( gc_wd_sky_ring_n >= GC_WD_SKY_RING )
	{
		GX_DrawDone();
		gc_wd_sky_ring_n = 0;
	}
	slot = gc_wd_sky_ring[gc_wd_sky_ring_i & ( GC_WD_SKY_RING - 1 )];
	gc_wd_sky_ring_i++;
	gc_wd_sky_ring_n++;
	return slot;
}

extern qboolean r_gx_mirror;
extern float r_gx_mirror_z;
extern float r_gx_mirror_real_org[3];

static void R_GXWorldDLLoadView( Mtx mv, Mtx44 proj, const float *org, f32 z_near, f32 z_far )
{
	vec3_t forward, right, up;
	f32 fov_y = RI.rvp.fov_y > 1.0f ? RI.rvp.fov_y : 90.0f;
	f32 fov_x = RI.rvp.fov_x > 1.0f ? RI.rvp.fov_x : 90.0f;
	f32 y_max, x_max;

	AngleVectors( RI.rvp.viewangles, forward, right, up );
	mv[0][0] = right[0];
	mv[0][1] = right[1];
	mv[0][2] = right[2];
	mv[0][3] = -DotProduct( right, org );
	mv[1][0] = up[0];
	mv[1][1] = up[1];
	mv[1][2] = up[2];
	mv[1][3] = -DotProduct( up, org );
	mv[2][0] = -forward[0];
	mv[2][1] = -forward[1];
	mv[2][2] = -forward[2];
	mv[2][3] = DotProduct( forward, org );
	y_max = z_near * tanf( fov_y * 3.14159265f / 360.0f );
	x_max = z_near * tanf( fov_x * 3.14159265f / 360.0f );
	/* Water mirror pass: the reflected camera's image is upside down. */
	if( r_gx_mirror )
		guFrustum( proj, -y_max, y_max, -x_max, x_max, z_near, z_far );
	else
		guFrustum( proj, y_max, -y_max, -x_max, x_max, z_near, z_far );
}

/*
 * $envmap reflections (wet sand, rock, roofs, plaster, metal).
 *
 * LightmappedGeneric adds ( cubemap x mask x $envmaptint )^2 in linear light
 * ($envmapcontrast 1), the cubemap being the env_cubemap sample VBSP patched
 * into the material. The bake carries all of it: the material record names
 * its cubemap and tint, the tevmat table has the authored mask, and each
 * reflective batch carries its surface normal n.
 *
 * GX has no cubemaps and cannot reflect a vector per vertex, but for a flat
 * surface the mirror direction of the view ray through P is linear in P:
 * R = H ( P - eye ), H = I - 2 n n^T. media/cm_<map>.gcm holds the HDR
 * cubemaps resampled on planes a . d = 1 about a few axes a (linear, square
 * root); with the plane's frame ( t, b ) the lookup ( R.t, R.b ) / R.a is one
 * projective texgen from the vertex position. The maps sit in ARAM; a few
 * stay resident.
 */
#define GC_WD_ENV_SLOTS_UP	16	/* ground views, side x side */
#define GC_WD_ENV_SLOTS	48	/* then 32 small ones for walls, roofs, rock */
#define GC_WD_ENV_MAXKEYS	320
#define GC_WD_ENV_AXES	17
#define GC_WD_ENVQ_MAX	224
static int gc_wd_env_state;	/* 0 untried, 1 ready, -1 absent */
static u32 gc_wd_env_aram, gc_wd_env_bytes, gc_wd_env_n;
static u32 gc_wd_env_bytes_s, gc_wd_env_n_up;	/* small view bytes; views about the up axis come first */
static u16 gc_wd_env_side_s;
static float gc_wd_env_k, gc_wd_env_k_up;	/* plane half-width: sides, up */
static u16 gc_wd_env_side;
static byte *gc_wd_env_mem;
static u16 gc_wd_env_keys[GC_WD_ENV_MAXKEYS];	/* axis * 256 + cubemap, sorted */
static s16 gc_wd_env_cube[GC_WD_ENV_SLOTS];
static int gc_wd_env_used[GC_WD_ENV_SLOTS];
static GXTexObj gc_wd_env_obj[GC_WD_ENV_SLOTS];
static u8 gc_wd_env_tint[64];
static float gc_wd_env_axis[GC_WD_ENV_AXES][3];
/* Reflective batches of resident chunks, drawn after the opaque world. */
static struct { const byte *chunk; const gc_wd_batch_t *b; } gc_wd_envq[GC_WD_ENVQ_MAX];
static int gc_wd_envq_n;
static u8 gc_wd_env_white[32] __attribute__(( aligned( 32 )));
static u32 gc_wd_env_stat[3];	/* view loads, views refused (slots busy), batches */
/* The slot each axis last bound this frame: when every slot is busy a
 * surface borrows its axis's last view (a neighbouring cubemap) instead of
 * losing its reflection, which showed as hard edges between sand patches. */
static s8 gc_wd_env_axis_slot[GC_WD_ENV_AXES];
static int gc_wd_env_axis_frame[GC_WD_ENV_AXES];
static GXTexObj gc_wd_env_white_obj;
/* $bumpmap X/Y behind the reflection (media/en_<map>.bin), by mask index. */
static u8 gc_wd_env_nrm_of[8];	/* slot + 1, 0 = flat */
static GXTexObj gc_wd_env_nrm_obj[3];

static void R_GXWorldDLEnvNormals( void )
{
	char path[96];
	byte head[32], *mem;
	file_t *f;
	u32 i, n;

	Q_snprintf( path, sizeof( path ), "media/en_%s.bin", gc_wd_mapbase );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return;
	if( gEngfuncs.fsapi->Read( f, head, 32 ) == 32 && head[0] == 'G' && head[3] == 'n'
		&& ( n = head[4] ) >= 1 && n <= 3
		&& ( mem = (byte *)GC_MapLoadBufferTailAlloc(
			Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()), n * 8192u )) != NULL
		&& gEngfuncs.fsapi->Read( f, mem, n * 8192u ) == (fs_offset_t)( n * 8192u ))
	{
		DCFlushRange( mem, n * 8192u );
		for( i = 0; i < n; i++ )
		{
			GX_InitTexObj( &gc_wd_env_nrm_obj[i], mem + i * 8192u, 64, 64, GX_TF_IA8,
				GX_REPEAT, GX_REPEAT, GX_FALSE );
			GX_InitTexObjFilterMode( &gc_wd_env_nrm_obj[i], GX_LINEAR, GX_LINEAR );
			gc_wd_env_nrm_of[head[5 + i] & 7] = (u8)( i + 1 );
		}
	}
	gEngfuncs.fsapi->Close( f );
}

static qboolean R_GXWorldDLEnvReady( void )
{
	char path[96];
	file_t *f;
	u32 hdr[8], i, tab;

	if( gc_wd_env_state )
		return gc_wd_env_state > 0;
	if( GC_MapLoadBufferTailPeak() == 0 )
		return false;
	gc_wd_env_state = -1;
	Q_snprintf( path, sizeof( path ), "media/et_%s.bin", gc_wd_mapbase );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	i = (u32)gEngfuncs.fsapi->Read( f, gc_wd_env_tint, 64 );
	gEngfuncs.fsapi->Close( f );
	Q_snprintf( path, sizeof( path ), "media/cm_%s.gcm", gc_wd_mapbase );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f || i != 64 )
		return false;
	if( gEngfuncs.fsapi->Read( f, hdr, 32 ) == 32 && hdr[0] == 0x4743434du && ( hdr[1] >> 16 ) == 5
		&& ( hdr[1] & 0xffffu ) <= GC_WD_ENV_MAXKEYS )
	{
		gc_wd_env_n = hdr[1] & 0xffffu;
		gc_wd_env_side = (u16)( hdr[2] >> 16 );
		gc_wd_env_k = (float)( hdr[2] & 0xffffu ) * ( 1.0f / 256.0f );
		gc_wd_env_k_up = (float)( hdr[3] >> 16 ) * ( 1.0f / 256.0f );
		gc_wd_env_side_s = (u16)( hdr[3] & 0xffffu );
		gc_wd_env_n_up = hdr[4] >> 16;
		gc_wd_env_bytes = (u32)gc_wd_env_side * gc_wd_env_side / 2u;
		gc_wd_env_bytes_s = (u32)gc_wd_env_side_s * gc_wd_env_side_s / 2u;
		tab = ( gc_wd_env_n * 2u + 31u ) & ~31u;
		gc_wd_env_mem = (byte *)GC_MapLoadBufferTailAlloc(
			Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()),
			GC_WD_ENV_SLOTS_UP * gc_wd_env_bytes + ( GC_WD_ENV_SLOTS - GC_WD_ENV_SLOTS_UP ) * gc_wd_env_bytes_s );
		if( gc_wd_env_mem && gc_wd_env_k > 0.0f && gc_wd_env_k_up > 0.0f && gc_wd_env_n_up <= gc_wd_env_n
			&& gc_wd_env_bytes && !( gc_wd_env_bytes & 31u ) && gc_wd_env_bytes_s && !( gc_wd_env_bytes_s & 31u )
			&& gc_wd_env_bytes_s <= gc_wd_env_bytes
			&& gEngfuncs.fsapi->Read( f, gc_wd_env_keys, gc_wd_env_n * 2u ) == (fs_offset_t)( gc_wd_env_n * 2u )
			&& gEngfuncs.fsapi->Seek( f, 32 + tab, SEEK_SET ) == 0
			&& ( gc_wd_env_aram = GC_AramReserveTagged( gc_wd_env_n_up * gc_wd_env_bytes
				+ ( gc_wd_env_n - gc_wd_env_n_up ) * gc_wd_env_bytes_s, "cubemaps" )) != 0 )
		{
			/* Slot 0 is the bounce while the pack goes up to ARAM. */
			u32 at = 0;

			for( i = 0; i < gc_wd_env_n; i++ )
			{
				const u32 nb = i < gc_wd_env_n_up ? gc_wd_env_bytes : gc_wd_env_bytes_s;

				if( gEngfuncs.fsapi->Read( f, gc_wd_env_mem, nb ) != (fs_offset_t)nb )
					break;
				GC_AramTransfer( gc_wd_env_aram + at, gc_wd_env_mem, nb, true );
				at += nb;
			}
			if( i == gc_wd_env_n )
			{
				for( i = 0; i < GC_WD_ENV_SLOTS; i++ )
					gc_wd_env_cube[i] = -1;
				/* Same list as tools/bake_lostcoast_cubemaps.py: up, eight
				 * level directions, eight raised 45 degrees. */
				gc_wd_env_axis[0][2] = 1.0f;
				for( i = 0; i < 16; i++ )
				{
					const float az = (float)( i & 7u ) * ( M_PI_F / 4.0f );
					const float ce = i < 8 ? 1.0f : 0.70710678f;

					gc_wd_env_axis[1 + i][0] = cosf( az ) * ce;
					gc_wd_env_axis[1 + i][1] = sinf( az ) * ce;
					gc_wd_env_axis[1 + i][2] = i < 8 ? 0.0f : 0.70710678f;
				}
				memset( gc_wd_env_white, 0xff, sizeof( gc_wd_env_white ));
				DCFlushRange( gc_wd_env_white, sizeof( gc_wd_env_white ));
				GX_InitTexObj( &gc_wd_env_white_obj, gc_wd_env_white, 8, 8, GX_TF_I4,
					GX_REPEAT, GX_REPEAT, GX_FALSE );
				gc_wd_env_state = 1;
				R_GXWorldDLEnvNormals();
			}
		}
	}
	gEngfuncs.fsapi->Close( f );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: env cubemaps %s: %u views %ux%u\n",
		gc_wd_env_state > 0 ? "ready" : "FAILED", (unsigned)gc_wd_env_n,
		(unsigned)gc_wd_env_side, (unsigned)gc_wd_env_side );
	return gc_wd_env_state > 0;
}

/* Bind cubemap view `view` on TEXMAP6. False when every slot of its size is
 * in use this frame. */
static qboolean R_GXWorldDLEnvBind( int view, int axis )
{
	int i, victim = -1, s0, s1;
	qboolean up;

	if( view < 0 || (u32)view >= gc_wd_env_n )
		return false;
	up = (u32)view < gc_wd_env_n_up;
	s0 = up ? 0 : GC_WD_ENV_SLOTS_UP;
	s1 = up ? GC_WD_ENV_SLOTS_UP : GC_WD_ENV_SLOTS;
	for( i = s0; i < s1; i++ )
	{
		if( gc_wd_env_cube[i] == view )
			break;
		if( gc_wd_env_used[i] != gc_wd_frame
			&& ( victim < 0 || gc_wd_env_used[i] < gc_wd_env_used[victim] ))
			victim = i;
	}
	if( i == s1 )
	{
		const u32 nb = up ? gc_wd_env_bytes : gc_wd_env_bytes_s;
		const u16 side = up ? gc_wd_env_side : gc_wd_env_side_s;
		byte *mem;

		if( victim < 0 )
		{
			gc_wd_env_stat[1]++;
			if( gc_wd_env_axis_frame[axis] != gc_wd_frame )
				return false;
			GX_LoadTexObj( &gc_wd_env_obj[gc_wd_env_axis_slot[axis]], GX_TEXMAP6 );
			return true;
		}
		gc_wd_env_stat[0]++;
		i = victim;
		mem = up ? gc_wd_env_mem + (u32)i * gc_wd_env_bytes
			: gc_wd_env_mem + GC_WD_ENV_SLOTS_UP * gc_wd_env_bytes + (u32)( i - GC_WD_ENV_SLOTS_UP ) * gc_wd_env_bytes_s;
		GC_AramTransfer( gc_wd_env_aram + ( up ? (u32)view * gc_wd_env_bytes
			: gc_wd_env_n_up * gc_wd_env_bytes + ( (u32)view - gc_wd_env_n_up ) * gc_wd_env_bytes_s ),
			mem, nb, false );
		GX_InitTexObj( &gc_wd_env_obj[i], mem, side, side, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE );
		GX_InitTexObjFilterMode( &gc_wd_env_obj[i], GX_LINEAR, GX_LINEAR );
		GX_InvalidateTexAll();
		gc_wd_env_cube[i] = (s16)view;
	}
	gc_wd_env_used[i] = gc_wd_frame;
	gc_wd_env_axis_slot[axis] = (s8)i;
	gc_wd_env_axis_frame[axis] = gc_wd_frame;
	GX_LoadTexObj( &gc_wd_env_obj[i], GX_TEXMAP6 );
	return true;
}

/* State shared by every reflective batch. */
static void R_GXWorldDLEnvBegin( void )
{
	gc_wd_tev_key = -1;
	const GXColor black = { 0, 0, 0, 255 };
	f32 zfar = RI.farClip > 1.0f ? RI.farClip : 4096.0f;
	int st;

	GX_SetNumIndStages( 0 );
	GX_SetNumTexGens( 2 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX8 );
	GX_SetTexCoordGen( GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetNumTevStages( 4 );
	/* The cubemap texel is sqrt( cube / 2 ), linear HDR. Stage 0 squares it
	 * ( cube / 2 ), stage 1 squares that x2 ( cube^2 / 2 ), stage 2 multiplies
	 * by the constant ( tint^2 x exposure / 2 ) x4, stage 3 by mask^2 x2:
	 * 2 x exposure x ( cube x tint x mask )^2, clamped. */
	for( st = 0; st < 4; st++ )
	{
		GX_SetTevDirect( GX_TEVSTAGE0 + st );
		GX_SetTevOrder( GX_TEVSTAGE0 + st, st == 3 ? GX_TEXCOORD1 : GX_TEXCOORD0,
			st == 3 ? GX_TEXMAP5 : GX_TEXMAP6, GX_COLORNULL );
		GX_SetTevKColorSel( GX_TEVSTAGE0 + st, GX_TEV_KCSEL_K0 );
		GX_SetTevColorIn( GX_TEVSTAGE0 + st, GX_CC_ZERO,
			st == 0 ? GX_CC_TEXC : GX_CC_CPREV,
			st == 1 ? GX_CC_CPREV : st == 2 ? GX_CC_KONST : GX_CC_TEXC, GX_CC_ZERO );
		GX_SetTevColorOp( GX_TEVSTAGE0 + st, GX_TEV_ADD, GX_TB_ZERO,
			st == 0 ? GX_CS_SCALE_1 : st == 2 ? GX_CS_SCALE_4 : GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0 + st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, st ? GX_CA_APREV : GX_CA_KONST );
		GX_SetTevAlphaOp( GX_TEVSTAGE0 + st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	/* The frame buffer is display gamma d; the sum belongs in linear light.
	 * d( ( b + s )^( 1 / 2.2 ) ) is close to 2 s ( 1 - d ) over the mid range,
	 * so the pass carries 2 s and blends src x ( 1 - dst ) + dst. */
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_INVDSTCLR, GX_BL_ONE, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	/* Added light is fogged toward black, not toward the fog colour. */
	if( zfar < 16384.0f )
		zfar = 16384.0f;
	GX_SetFog( GX_FOG_PERSP_LIN, 1600.0f, 50000.0f, 4.0f, zfar, black );
}

/* Cubemap view, lookup matrix, tint and mask for one material on a surface
 * with the batch word `pad` (0x8000 | texture layout << 12 | octahedral
 * normal). False: no pass. */
static qboolean R_GXWorldDLEnvSetup( const gc_wd_mat_t *m, u16 pad )
{
	const float *eye = RI.rvp.vieworigin;
	float ik, kk;
	const u8 *tn = &gc_wd_env_tint[( m->pad & 15u ) * 4u];
	const int cube = (int)( m->pad >> 8 ) - 1;
	int mask = -1, lo, hi, axis = 0, i, view = -1;
	vec3_t n, a, t, b, ht, hb, ha;
	float best = -2.0f, d;
	GXColor tint;
	Mtx tm;

	if( cube < 0 )
		return false;
	if( !( m->flags & GC_WD_MAT_ENVNOMASK ))
	{
		/* No authored mask in the table: no reflection, rather than an
		 * unmasked one. */
		mask = R_GXTevMatEnvMask( m->tevmat );
		if( mask < 0 )
			return false;
	}
	/* The batch normal. */
	n[0] = (float)(( pad >> 6 ) & 63u ) * ( 2.0f / 63.0f ) - 1.0f;
	n[1] = (float)( pad & 63u ) * ( 2.0f / 63.0f ) - 1.0f;
	n[2] = 1.0f - fabsf( n[0] ) - fabsf( n[1] );
	if( n[2] < 0.0f )
	{
		const float x = n[0];

		n[0] = ( 1.0f - fabsf( n[1] )) * ( x >= 0.0f ? 1.0f : -1.0f );
		n[1] = ( 1.0f - fabsf( x )) * ( n[1] >= 0.0f ? 1.0f : -1.0f );
	}
	VectorNormalize( n );
	for( i = 0; i < GC_WD_ENV_AXES; i++ )
	{
		d = DotProduct( n, gc_wd_env_axis[i] );
		if( d > best )
		{
			best = d;
			axis = i;
		}
	}
	lo = 0; hi = (int)gc_wd_env_n - 1;
	while( lo <= hi )
	{
		const int mid = ( lo + hi ) >> 1;
		const int key = gc_wd_env_keys[mid];

		if( key == axis * 256 + cube )
		{
			view = mid;
			break;
		}
		if( key < axis * 256 + cube )
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	if( view < 0 || !R_GXWorldDLEnvBind( view, axis ))
		return false;
	if( mask >= 0 )
		R_GXTevBindEnvMask( (u8)mask );
	else
		GX_LoadTexObj( &gc_wd_env_white_obj, GX_TEXMAP5 );
	kk = axis == 0 ? gc_wd_env_k_up : gc_wd_env_k;
	ik = 0.5f / kk;
	/* The plane's frame, then each vector through H = I - 2 n n^T. */
	VectorCopy( gc_wd_env_axis[axis], a );
	if( axis == 0 )
	{
		VectorSet( t, 1.0f, 0.0f, 0.0f );
		VectorSet( b, 0.0f, 1.0f, 0.0f );
	}
	else
	{
		VectorSet( t, -a[1], a[0], 0.0f );
		VectorNormalize( t );
		CrossProduct( a, t, b );
	}
	d = 2.0f * DotProduct( t, n ); VectorMA( t, -d, n, ht );
	d = 2.0f * DotProduct( b, n ); VectorMA( b, -d, n, hb );
	d = 2.0f * DotProduct( a, n ); VectorMA( a, -d, n, ha );
	for( i = 0; i < 3; i++ )
	{
		tm[0][i] = ik * ht[i] + 0.5f * ha[i];
		tm[1][i] = ik * hb[i] + 0.5f * ha[i];
		tm[2][i] = ha[i];
	}
	for( i = 0; i < 3; i++ )
		tm[i][3] = -( tm[i][0] * eye[0] + tm[i][1] * eye[1] + tm[i][2] * eye[2] );
	GX_LoadTexMtxImm( tm, GX_TEXMTX8, GX_MTX3x4 );
	tint.r = tn[0]; tint.g = tn[1]; tint.b = tn[2]; tint.a = 255;
	GX_SetTevKColor( GX_KCOLOR0, tint );
	/* Reflect about the per-pixel normal: with the normal map's ( nx, ny )
	 * the lookup p = ( R.t, R.b ) / R.a moves by 2 ( I + p p^T ) ( nx, ny ) to
	 * first order. An indirect matrix is constant over the draw, so p is
	 * taken at the centre of the view. Offset texels = m x ( n x 127.5 ) x
	 * 2^( exp + 3 ). */
	if( mask >= 0 && gc_wd_env_nrm_of[mask & 7] )
	{
		vec3_t fwd;
		f32 im[2][3], pa, pb;
		const f32 c = (f32)( axis == 0 ? gc_wd_env_side : gc_wd_env_side_s ) / ( kk * 510.0f );

		AngleVectors( RI.rvp.viewangles, fwd, NULL, NULL );
		d = DotProduct( fwd, ha );
		if( d < 0.25f )
			d = 0.25f;
		pa = DotProduct( fwd, ht ) / d; pb = DotProduct( fwd, hb ) / d;
		{
			/* The normal map's X runs along the surface's texture S and its
			 * Y along T. Bits 12-14 of the batch word say how S and T lie in
			 * ( t, b ): bit 0 swapped, bit 1 / 2 the sign of each row. */
			const u32 o = ( pad >> 12 ) & 7u;
			const f32 sx = ( o & 2u ) ? -1.0f : 1.0f, sy = ( o & 4u ) ? -1.0f : 1.0f;
			const f32 m00 = c * ( 1.0f + pa * pa ), m01 = c * pa * pb, m11 = c * ( 1.0f + pb * pb );
			int r, q;

			if( o & 1u )
			{
				im[0][0] = m01 * sy; im[0][1] = m00 * sx;
				im[1][0] = m11 * sy; im[1][1] = m01 * sx;
			}
			else
			{
				im[0][0] = m00 * sx; im[0][1] = m01 * sy;
				im[1][0] = m01 * sx; im[1][1] = m11 * sy;
			}
			im[0][2] = im[1][2] = 0.0f;
			for( r = 0; r < 2; r++ )
				for( q = 0; q < 2; q++ )
					im[r][q] = im[r][q] > 0.99f ? 0.99f : im[r][q] < -0.99f ? -0.99f : im[r][q];
		}
		GX_SetIndTexMatrix( GX_ITM_0, im, -1 );
		GX_LoadTexObj( &gc_wd_env_nrm_obj[gc_wd_env_nrm_of[mask & 7] - 1], GX_TEXMAP4 );
		GX_SetNumIndStages( 1 );
		GX_SetIndTexOrder( GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP4 );
		GX_SetIndTexCoordScale( GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1 );
		GX_SetTevIndirect( GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_ST, GX_ITM_0,
			GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF );
	}
	else
	{
		GX_SetNumIndStages( 0 );
		GX_SetTevDirect( GX_TEVSTAGE0 );
	}
	return true;
}

static void R_GXWorldDLEnvOff( void )
{
	f32 zfar = RI.farClip > 1.0f ? RI.farClip : 4096.0f;

	GX_SetTevDirect( GX_TEVSTAGE0 );

	if( zfar < 16384.0f )
		zfar = 16384.0f;
	R_GXWorldDLFog( 4.0f, zfar );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	R_GXWorldDLSetupLit( gc_wd_chunk_lm );
	if( gc_wd_chunk_lm )
		GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
}

/* Draw the queued reflective batches over the finished opaque world. */
static void R_GXWorldDLEnvFlush( void )
{
	const byte *chunk = NULL;
	u32 last = 0xffffffffu;
	qboolean ok = false;
	int q;

	if( !gc_wd_envq_n )
		return;
	R_GXWorldDLEnvBegin();
	for( q = 0; q < gc_wd_envq_n; q++ )
	{
		const gc_wd_batch_t *b = gc_wd_envq[q].b;
		const u32 key = ( (u32)b->mat << 16 ) | b->pad;

		if( gc_wd_envq[q].chunk != chunk )
		{
			chunk = gc_wd_envq[q].chunk;
			GX_SetArray( GX_VA_POS, (void *)( chunk + *(const u32 *)( chunk + 4 )), 6 );
		}
		if( key != last )
		{
			ok = R_GXWorldDLEnvSetup( &gc_wd_mats[b->mat], b->pad );
			last = key;
		}
		if( ok )
			GX_CallDispList( (void *)( chunk + b->dl_ofs ), b->dl_bytes );
	}
	gc_wd_env_stat[2] += (u32)gc_wd_envq_n;
	if( gc_wd_pf_on > 0 && ( gc_wd_frame & 127 ) == 0 )
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: env per frame batches=%u view loads=%u refused=%u\n",
			gc_wd_env_stat[2] / 128u, gc_wd_env_stat[0] / 128u, gc_wd_env_stat[1] / 128u );
		gc_wd_env_stat[0] = gc_wd_env_stat[1] = gc_wd_env_stat[2] = 0;
	}
	gc_wd_envq_n = 0;
	R_GXWorldDLEnvOff();
}

static void R_GXWorldDLEmitChunk( byte *chunk, int *cur_mat )
{
	const gc_wd_batch_t *b;
	u32 nb, k;

	nb = *(const u16 *)( chunk + 2 );
	GX_SetArray( GX_VA_POS, chunk + *(const u32 *)( chunk + 4 ), 6 );
	b = (const gc_wd_batch_t *)( chunk + 16 );
	for( k = 0; k < nb; k++, b++ )
	{
		const gc_wd_mat_t *m;

		if( b->mat >= gc_wd_hdr.n_mats )
			continue;
		m = &gc_wd_mats[b->mat];
		if( (int)b->mat != *cur_mat )
		{
			if( gc_wd_texnum[b->mat] < 0 )
				gc_wd_texnum[b->mat] = (s16)GC_WorldTexnumByHash( m->tex_hash );
			if( gc_wd_texnum[b->mat] <= 0 )
				continue;
			if( !R_GXWorldDLBindTex( (unsigned)gc_wd_texnum[b->mat] ))
				continue;
			*cur_mat = (int)b->mat;
		}
		GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
	}
}

static void R_GXWorldDLClearDepth( void )
{
	GXRModeObj *rmode = (GXRModeObj *)GC_GetGxVideoMode();
	Mtx44 ortho;
	Mtx ident;
	f32 fb_w, fb_h;

	if( !rmode )
		return;
	fb_w = (f32)rmode->fbWidth;
	fb_h = (f32)rmode->efbHeight;
	GX_SetColorUpdate( GX_FALSE );
	GX_SetZMode( GX_TRUE, GX_ALWAYS, GX_TRUE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetNumChans( 0 );
	GX_SetNumTexGens( 0 );
	GX_SetNumTevStages( 1 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	guOrtho( ortho, 0.0f, fb_h, 0.0f, fb_w, 0.0f, 1.0f );
	GX_LoadProjectionMtx( ortho, GX_ORTHOGRAPHIC );
	guMtxIdentity( ident );
	GX_LoadPosMtxImm( ident, GX_PNMTX0 );
	GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
	GX_Position3f32( 0.0f, 0.0f, -1.0f );
	GX_Position3f32( fb_w, 0.0f, -1.0f );
	GX_Position3f32( fb_w, fb_h, -1.0f );
	GX_Position3f32( 0.0f, fb_h, -1.0f );
	GX_End();
	GX_SetColorUpdate( GX_TRUE );
}

/* Map fog (env_fog_controller and sky_camera agree: 151 166 181,
 * 1600..50000). GX fog is derived from the projection's near/far, so it is
 * re-armed with each projection: the backdrop runs to 131072, the harbor
 * to 16384+. With one shared range the backdrop had no haze at all. */
static void R_GXWorldDLFog( f32 z_near, f32 z_far )
{
	const GXColor fog = { 151, 166, 181, 255 };

	GX_SetFog( GX_FOG_PERSP_LIN, 1600.0f, 50000.0f, z_near, z_far, fog );
}

static void R_GXWorldDLRestoreHarbor( Mtx mv, Mtx44 proj, const float *org )
{
	/* zmax*1.73 stops near 7 km. The refinery brushes sit past that and were
	 * clipped to nothing, leaving the prop detail as the only tower. */
	{
		f32 zfar = RI.farClip > 1.0f ? RI.farClip : 4096.0f;

		if( zfar < 16384.0f )
			zfar = 16384.0f;
		R_GXWorldDLLoadView( mv, proj, org, 4.0f, zfar );
		R_GXWorldDLFog( 4.0f, zfar );
	}
	GX_LoadProjectionMtx( proj, GX_PERSPECTIVE );
	GX_LoadPosMtxImm( mv, GX_PNMTX0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_INDEX16 );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_POS, GX_POS_XYZ, GX_S16, 0 );
	GX_SetVtxDesc( GX_VA_NRM, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX1, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6 );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_TEX0, GX_TEX_ST, GX_S16, GC_WD_ST_FRAC );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_TEX1, GX_TEX_ST, GX_U8, 8 );
	GX_SetNumTexGens( 2 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetTexCoordGen( GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY );
	GX_SetNumTevStages( 1 );
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
}

#define GC_WD_SKY_MAX	32
static byte *gc_wd_sky_mem;
static u16 gc_wd_sky_idx[GC_WD_SKY_MAX], gc_wd_sky_at[GC_WD_SKY_MAX];
static int gc_wd_sky_cnt;
static s8 gc_wd_sky_state;	/* 0 untried, 1 resident, -1 ring */

/* A chunk of the 3D-skybox ridge (the filter the ring path below uses). */
static qboolean R_GXWorldDLIsSkyline( int i )
{
	const float cz = ( (float)gc_wd_chunk_mins[i * 3 + 2] + (float)gc_wd_chunk_maxs[i * 3 + 2] ) * 0.5f;
	const float hz = (float)gc_wd_chunk_maxs[i * 3 + 2] - (float)gc_wd_chunk_mins[i * 3 + 2];
	const float cx = ( (float)gc_wd_chunk_mins[i * 3 + 0] + (float)gc_wd_chunk_maxs[i * 3 + 0] ) * 0.5f;
	const float cy = ( (float)gc_wd_chunk_mins[i * 3 + 1] + (float)gc_wd_chunk_maxs[i * 3 + 1] ) * 0.5f;
	const u32 bytes = gc_wd_chunk_bytes[i];

	if( cz > -4000.0f || hz < 80.0f )
		return false;
	if( cx * cx + cy * cy < ( 500.0f * 500.0f ))
		return false;
	return bytes != 0 && bytes <= 4096;
}

static void R_GXWorldDLDrawSkyline( void )
{
	Mtx mv, trans, scale, placed, sky;
	Mtx44 proj;
	int i, n = 0, cur_mat = -1;
	const float *org = RI.rvp.vieworigin;

	if( !gc_wd_aram || gc_wd_hdr.version != 2 )
		return;
	/* The frame-start DrawDone retired last frame's slots. */
	gc_wd_sky_ring_n = 0;
	/* Scale the sky volume out to the horizon. Viewing it from inside
	 * the volume stretches the near ground across the sky. The distant
	 * wall chunks are wide dark quads, not the ridge, so they stay out. */
	cur_mat = -1;
	R_GXWorldDLLoadView( mv, proj, org, 4.0f, 131072.0f );
	R_GXWorldDLFog( 4.0f, 131072.0f );
	guMtxTrans( trans, 0.0f, 0.0f, 5108.0f );
	guMtxScale( scale, 16.0f, 16.0f, 16.0f );
	guMtxConcat( scale, trans, placed );
	guMtxConcat( mv, placed, sky );
	GX_LoadProjectionMtx( proj, GX_PERSPECTIVE );
	GX_LoadPosMtxImm( sky, GX_PNMTX0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	/* The skyline is the same 15 or so chunks every frame. Refetching them
	 * from ARAM through a 4-slot ring cost a DMA each and a GP drain every
	 * fourth chunk, twice a frame with the water mirror (0.9 ms a pass).
	 * They are fetched once into the map arena and stay there. */
	if( !gc_wd_sky_state )
	{
		u32 total = 0;

		gc_wd_sky_state = -1;
		gc_wd_sky_cnt = 0;
		for( i = 0; i < (int)gc_wd_hdr.n_chunks && gc_wd_sky_cnt < GC_WD_SKY_MAX; i++ )
		{
			if( !R_GXWorldDLIsSkyline( i ))
				continue;
			gc_wd_sky_idx[gc_wd_sky_cnt] = (u16)i;
			gc_wd_sky_at[gc_wd_sky_cnt++] = (u16)( total >> 5 );
			total += ( gc_wd_chunk_bytes[i] + 31u ) & ~31u;
		}
		if( i == (int)gc_wd_hdr.n_chunks && total && GC_MapLoadBufferTailPeak() != 0
			&& ( gc_wd_sky_mem = (byte *)GC_MapLoadBufferTailAlloc(
				Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()), total )) != NULL )
		{
			for( i = 0; i < gc_wd_sky_cnt; i++ )
			{
				if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_chunks + gc_wd_chunk_ofs[gc_wd_sky_idx[i]],
					gc_wd_sky_mem + ( (u32)gc_wd_sky_at[i] << 5 ), gc_wd_chunk_bytes[gc_wd_sky_idx[i]] ))
					break;
			}
			if( i == gc_wd_sky_cnt )
				gc_wd_sky_state = 1;
		}
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL skyline resident=%d chunks=%d bytes=%u\n",
			gc_wd_sky_state > 0, gc_wd_sky_cnt, (unsigned)total );
	}
	if( gc_wd_sky_state > 0 )
	{
		for( i = 0; i < gc_wd_sky_cnt; i++ )
			R_GXWorldDLEmitChunk( gc_wd_sky_mem + ( (u32)gc_wd_sky_at[i] << 5 ), &cur_mat );
		n = gc_wd_sky_cnt;
	}
	else
	for( i = 0; i < (int)gc_wd_hdr.n_chunks; i++ )
	{
		const float cz = ( (float)gc_wd_chunk_mins[i * 3 + 2]
			+ (float)gc_wd_chunk_maxs[i * 3 + 2] ) * 0.5f;
		const float hz = (float)gc_wd_chunk_maxs[i * 3 + 2]
			- (float)gc_wd_chunk_mins[i * 3 + 2];
		const float cx = ( (float)gc_wd_chunk_mins[i * 3 + 0]
			+ (float)gc_wd_chunk_maxs[i * 3 + 0] ) * 0.5f;
		const float cy = ( (float)gc_wd_chunk_mins[i * 3 + 1]
			+ (float)gc_wd_chunk_maxs[i * 3 + 1] ) * 0.5f;
		const u32 bytes = gc_wd_chunk_bytes[i];

		if( cz > -4000.0f || hz < 80.0f )
			continue;
		if( cx * cx + cy * cy < ( 500.0f * 500.0f ))
			continue;
		if( bytes == 0 || bytes > 4096 )
			continue;
		{
			byte *slot = R_GXWorldDLSkySlot();

			if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_chunks + gc_wd_chunk_ofs[i], slot, bytes ))
				continue;
			R_GXWorldDLEmitChunk( slot, &cur_mat );
		}
		n++;
	}
	/* Far props draw once after the world, in this view, so a nearer cliff
	 * wins and the mesh is not stamped twice into the bloom. */
	R_GXWorldDLClearDepth();
	R_GXWorldDLRestoreHarbor( mv, proj, org );
	{
		static qboolean logged;

		if( !logged )
		{
			logged = true;
			gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL skyline chunks=%d\n", n );
		}
	}
}

/* Chunk sort keys, then (after the chunk loop) the frame's deferred batches.
 * One array: the keys are dead by the time batches are recorded. */
static u64 gc_wd_keys[GC_WD_MAX_CHUNKS];
static u16 gc_wd_mat_count[GC_WD_MAX_MATS * 2 + 1];

/* Material-grouped world draw. Every packed bind that misses the texture slot
 * cache DMAs the texture from ARAM and waits on the GP; drawn chunk by chunk
 * front to back, a frame of dod_charlie changed texture 450-700 times for 36
 * materials (70-120 ms). Resident chunks stay put for the whole frame (the
 * cache never evicts a chunk drawn this frame, nor a lightmap page), so their
 * batches are recorded and drawn grouped by material, front to back inside
 * each group: one bind per material and pool. Forced chunks (shared bounce)
 * and the two-layer blend / env batches still draw in place.
 * Record: chunk(16) batch(16) key = mat*2+far (16) lightmap slot+1 (8). */
static int gc_wd_defer_n;
static int gc_wd_defer_on = -1;

static int R_GXWorldDLMatTexnum( u32 mat )
{
	if( gc_wd_texnum[mat] <= 0 )
	{
		const int again = GC_WorldTexnumByHash( gc_wd_mats[mat].tex_hash );

		if( again <= 0 )
			return 0;
		gc_wd_texnum[mat] = (s16)again;
	}
	return gc_wd_texnum[mat];
}

/* Second layer of a WorldVertexTransition material (Lost Coast sand) for one
 * material group of the deferred queue: $basetexture2 over the layer just
 * drawn, weighted by the vertex alpha. In place this was three texture binds
 * and two blend-state changes per batch, and the beach is all such batches;
 * per group it is one bind and one state change. */
static void R_GXWorldDLDeferBlendLayer( const u16 *idx, int k0, int k1, u32 mat, int far_pool )
{
	const gc_wd_mat_t *m = &gc_wd_mats[mat];
	int k, cur_chunk = -1, cur_lm = -1;
	qboolean tev_set = false;

	if( gc_wd_texnum2[mat] == 0 )
	{
		const int t2 = GC_WorldTexnumByHash( m->hash2 );

		gc_wd_texnum2[mat] = (s16)( t2 > 0 ? t2 : -1 );
	}
	if( gc_wd_texnum2[mat] <= 0 )
		return;
	R_GXCmprSetFar( far_pool ? true : false );
	if( !R_GXWorldDLBindTex( (unsigned)gc_wd_texnum2[mat] ))
		return;
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	for( k = k0; k < k1; k++ )
	{
		const u64 r = gc_wd_keys[idx[k]];
		const int ci = (int)( r >> 48 );
		const int lm = (int)(( r >> 8 ) & 0xffu );
		byte *chunk;
		const gc_wd_batch_t *b;

		if( gc_wd_slots[ci].cache_ofs < 0 )
			continue;
		chunk = gc_wd_cache + gc_wd_slots[ci].cache_ofs;
		b = (const gc_wd_batch_t *)( chunk + 16 ) + (int)(( r >> 32 ) & 0xffffu );
		if( ci != cur_chunk )
		{
			GX_SetArray( GX_VA_POS, chunk + *(const u32 *)( chunk + 4 ), 6 );
			cur_chunk = ci;
		}
		if( lm != cur_lm )
		{
			const int had = gc_wd_chunk_lm;

			gc_wd_chunk_lm = 0;
			if( lm > 0 )
			{
				GX_InitTexObj( &gc_wd_lm_obj, gc_wd_lm_store + (u32)( lm - 1 ) * GC_WD_LM_PAGE_BYTES,
					32, 32, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE );
				GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
				GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
				gc_wd_chunk_lm = 1;
			}
			if( had != gc_wd_chunk_lm )
				tev_set = false;
			cur_lm = lm;
		}
		if( !tev_set )
		{
			gc_wd_tev_key = -1;
			R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
			/* Coverage = vertex alpha, carried through the later stages. */
			GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
			GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA );
			GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
			gc_wd_tev_key = -1;
			tev_set = true;
		}
		GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
		blend_draws++;
	}
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	gc_wd_tev_key = -1;
}

static void R_GXWorldDLDeferFlush( int *drawn )
{
	static u16 idx[GC_WD_MAX_CHUNKS];
	const int nkeys = (int)gc_wd_hdr.n_mats * 2;
	int k, cur_chunk = -1, cur_lm = -1, cur_key = -1, cur_far = -1;
	int group_key = -1, group_start = 0;
	qboolean bound = false;

	if( gc_wd_defer_n <= 0 )
		return;
	/* Counting sort by key: stable, so each material keeps front-to-back. */
	memset( gc_wd_mat_count, 0, sizeof( gc_wd_mat_count[0] ) * ( nkeys + 1 ));
	for( k = 0; k < gc_wd_defer_n; k++ )
		gc_wd_mat_count[(( gc_wd_keys[k] >> 16 ) & 0xffffu ) + 1]++;
	for( k = 1; k <= nkeys; k++ )
		gc_wd_mat_count[k] += gc_wd_mat_count[k - 1];
	for( k = 0; k < gc_wd_defer_n; k++ )
		idx[gc_wd_mat_count[( gc_wd_keys[k] >> 16 ) & 0xffffu]++] = (u16)k;

	for( k = 0; k <= gc_wd_defer_n; k++ )
	{
		const u64 r = gc_wd_keys[idx[k < gc_wd_defer_n ? k : 0]];
		const int ci = (int)( r >> 48 );
		const int bi = (int)(( r >> 32 ) & 0xffffu );
		const int key = k < gc_wd_defer_n ? (int)(( r >> 16 ) & 0xffffu ) : -2;

		/* A material group just ended: its second layer, if it has one. */
		if( key != group_key )
		{
			if( group_key >= 0 && gc_wd_hdr.version == 2 && (u32)( group_key >> 1 ) < GC_WD_BLEND_MATS
				&& gc_wd_mats[group_key >> 1].hash2 )
			{
				R_GXWorldDLDeferBlendLayer( idx, group_start, k, (u32)group_key >> 1, group_key & 1 );
				cur_chunk = -1; cur_lm = -1; cur_key = -1; cur_far = -1;
			}
			group_key = key;
			group_start = k;
		}
		if( k == gc_wd_defer_n )
			break;
		const int lm = (int)(( r >> 8 ) & 0xffu );
		const u32 mat = (u32)key >> 1;
		const int far_pool = key & 1;
		byte *chunk;
		const gc_wd_batch_t *b;

		if( gc_wd_slots[ci].cache_ofs < 0 )
			continue;	/* cannot happen: drawn-this-frame chunks are kept */
		chunk = gc_wd_cache + gc_wd_slots[ci].cache_ofs;
		b = (const gc_wd_batch_t *)( chunk + ( gc_wd_hdr.version == 2 ? 16 : 8 )) + bi;
		if( ci != cur_chunk )
		{
			GX_SetArray( GX_VA_POS, chunk + *(const u32 *)( chunk + 4 ), 6 );
			cur_chunk = ci;
		}
		if( lm != cur_lm )
		{
			const int had = gc_wd_chunk_lm;

			gc_wd_chunk_lm = 0;
			if( lm > 0 )
			{
				GX_InitTexObj( &gc_wd_lm_obj, gc_wd_lm_store + (u32)( lm - 1 ) * GC_WD_LM_PAGE_BYTES,
					32, 32, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE );
				GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
				GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
				gc_wd_chunk_lm = 1;
				gc_wd_lm_binds++;
			}
			if( had != gc_wd_chunk_lm )
				cur_key = -1;	/* lit TEV differs: rebuild the material */
			cur_lm = lm;
		}
		if( key != cur_key )
		{
			const int texnum = R_GXWorldDLMatTexnum( mat );
			const gc_wd_mat_t *m = &gc_wd_mats[mat];

			if( far_pool != cur_far )
			{
				R_GXCmprSetFar( far_pool ? true : false );
				cur_far = far_pool;
			}
			bound = false;
			if( texnum > 0 )
			{
				qboolean ok;

				WD_PF( 4, ok = R_GXWorldDLBindTex( (unsigned)texnum ));
				if( ok )
				{
					R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
					bound = true;
				}
			}
			cur_key = key;
		}
		if( !bound )
			continue;
		GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
		( *drawn )++;
		/* $envmap batches (most of Lost Coast) are grouped too: the reflection
		 * pass is queued exactly as the in-place path queues it. */
		if(( b->pad & 0x8000u ) && ( gc_wd_mats[mat].flags & GC_WD_MAT_ENVMAP ) && !r_gx_mirror
			&& gc_wd_hdr.version == 2 && gc_wd_envq_n < GC_WD_ENVQ_MAX && R_GXWorldDLEnvReady())
		{
			gc_wd_envq[gc_wd_envq_n].chunk = chunk;
			gc_wd_envq[gc_wd_envq_n++].b = b;
		}
	}
	gc_wd_defer_n = 0;
	R_GXCmprSetFar( false );
}

int R_GXWorldDLDraw( void )
{
	static qboolean logged;
	static qboolean logged_eye;
	float org_local[3];
	const float *org = RI.rvp.vieworigin;
	vec3_t normals[4];
	u32 budget;
	u32 i;
	int cluster, drawn = 0, chunks = 0, waiting = 0, cur_mat = -1, bind_miss = 0;

	gc_wd_pf_enter = gettime();
	if( gc_wd_state != 1 )
		return 0;
	/* Retire last frame's display lists before this frame fills the FIFO.
	 * Without it the second present waits forever on the GP. */
	if( gc_wd_frame > 0 )
		GX_DrawDone();
	gc_wd_frame++;
	if( gc_wd_file && ( gc_wd_frame & 127 ) == 0 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL disc totals reads=%u bytes=%u frame=%d\n",
			gc_wd_disc_reads, gc_wd_disc_bytes, gc_wd_frame );
	gc_wd_lm_miss = 0;
	gc_wd_lm_binds = 0;
	gc_wd_chunk_lm = 0;
	/* First ~90 frames: pull a denser visible set so CapFaces listen does not
	 * open on a starved alley; after warmup keep 512 KiB/frame lazy stream. */
	budget = ( gc_wd_frame <= 90 ) ? GC_WD_WARMUP_BUDGET : GC_WD_LOAD_BUDGET;
	/* CapFaces wall-aim normally seeds vieworigin; World DL skips CapFaces so
	 * early frames can sit at (0,0,0) / solid → cluster=-1 → all-vis. Prefer a
	 * non-degenerate eye, then fall back to world AABB center from mins/maxs. */
	if(( !org[0] && !org[1] && !org[2] ) || !isfinite( org[0] ))
	{
		model_t *world = WORLDMODEL;

		if( world )
		{
			org_local[0] = 0.5f * ( world->mins[0] + world->maxs[0] );
			org_local[1] = 0.5f * ( world->mins[1] + world->maxs[1] );
			org_local[2] = 0.5f * ( world->mins[2] + world->maxs[2] ) + 64.0f;
			org = org_local;
		}
	}
	/* Mirror pass: the reflected eye is under the seabed. Visibility and
	 * the cache order come from the real eye; the frustum test below uses
	 * the reflected one. */
	if( r_gx_mirror )
		org = r_gx_mirror_real_org;
	cluster = R_GXWorldDLEyeCluster( org );
	if( cluster < 0 && WORLDMODEL )
	{
		/* Retry from AABB center when the live eye is in solid. */
		model_t *world = WORLDMODEL;

		org_local[0] = 0.5f * ( world->mins[0] + world->maxs[0] );
		org_local[1] = 0.5f * ( world->mins[1] + world->maxs[1] );
		org_local[2] = 0.5f * ( world->mins[2] + world->maxs[2] ) + 64.0f;
		cluster = R_GXWorldDLEyeCluster( org_local );
		if( cluster >= 0 )
			org = org_local;
	}
	if( !logged_eye )
	{
		logged_eye = true;
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: world DL eye=(%.0f,%.0f,%.0f) cluster=%d\n",
			org[0], org[1], org[2], cluster );
	}
	if( cluster >= 0 && cluster != gc_wd_pvs_cluster )
		R_GXWorldDLLoadPVS( cluster );
	else if( gc_wd_pvs_cluster == -2 )
		R_GXWorldDLLoadPVS( -1 );
	R_GXWorldDLFrustum( normals );
	gc_wd_pf_t0 = gettime();
	gc_wd_pf[9] += gc_wd_pf_t0 - gc_wd_pf_enter;

	GX_SetCullMode( GX_CULL_NONE );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_INDEX16 );
	GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_POS, GX_POS_XYZ, GX_S16, 0 );
	if( gc_wd_hdr.version == 2 )
	{
		/* Lost Coast bake: POS index, NRM s8 frac 6, CLR0 RGBA8,
		 * TEX0 s16 frac 9, TEX1 lightmap u8 frac 8. 15 bytes. */
		GX_SetVtxDesc( GX_VA_NRM, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_TEX1, GX_DIRECT );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6 );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_TEX0, GX_TEX_ST, GX_S16, GC_WD_ST_FRAC );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_TEX1, GX_TEX_ST, GX_U8, 8 );
	}
	else
	{
		GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0 );
		GX_SetVtxAttrFmt( GX_VTXFMT6, GX_VA_TEX0, GX_TEX_ST, GX_S16, GC_WD_ST_FRAC );
	}
	R_GXWorldDLSetupLit( 0 );
	R_GXFogBegin();
	if( gc_wd_pf_on < 0 )
		gc_wd_pf_on = gEngfuncs.Sys_CheckParm( "-gcvst" ) ? 1 : 0;
	R_GXCmprSetFar( gc_wd_tex_overflow );
	{
		/* The sky volume is drawn scaled 16x through its own matrix: its
		 * vertex heights are not world heights, so no water clip there. */
		const qboolean clip = r_gx_mirror_clip;

		if( clip )
			R_GXMirrorClipSet( false );
		WD_PF( 5, R_GXWorldDLDrawSkyline());
		if( clip )
			R_GXMirrorClipSet( true );
	}
	R_GXCmprSetFar( false );
	gc_wd_chunk_lm = 0;
	R_GXWorldDLSetupLit( 0 );

	gc_wd_envq_n = 0;
	gc_wd_defer_n = 0;
	if( gc_wd_defer_on < 0 )
		gc_wd_defer_on = gEngfuncs.Sys_CheckParm( "-gcnomatsort" ) ? 0 : 1;
	/* Full world first: draw every PVS-visible chunk. Frustum cull is a later
	 * speed knob (ava-play99-60 sat at 20 chunks with waiting=0 while the
	 * alley still looked starved). Lazy-load still gates on budget/cache. */
	{
		/* Chunks outside the camera go. -gcworlddlfull draws the whole PVS. */
		const qboolean frustum_cull = !gEngfuncs.Sys_CheckParm( "-gcworlddlfull" );
		int pvs_hits = 0, frustum_drop = 0, tower_chunks = 0, forced_n = 0;
		static u16 order[GC_WD_MAX_CHUNKS];
		const qboolean skip_water = GC_GetWaterFaceCount() > 0
			&& !gEngfuncs.Sys_CheckParm( "-gcbakedwater" );
		int norder = 0;
		int oi, pass;
		u32 tower_budget = 288u * 1024u;
		u32 tower_grant, shore_budget;

		for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
		{
			const u16 cl = gc_wd_chunk_cluster[i];

			if( !( gc_wd_pvs[cl >> 3] & ( 1 << ( cl & 7 ))))
				continue;
			pvs_hits++;
			/* Nothing under the water reflects in it. */
			if( r_gx_mirror && (float)gc_wd_chunk_maxs[i * 3 + 2] <= r_gx_mirror_z )
				continue;
			if( frustum_cull && !R_GXWorldDLBoxVisible( i, normals,
				r_gx_mirror ? RI.rvp.vieworigin : org ))
			{
				frustum_drop++;
				continue;
			}
			if( norder < GC_WD_MAX_CHUNKS )
				order[norder++] = (u16)i;
		}
		/* Spend the cache on the ground in front of the camera. Index order
		 * filled it with whatever chunk id came first and left the shore out. */
		gc_wd_sort_org = org;
		/* Key each chunk once (the distance has a sqrt for tall chunks and
		 * qsort asked for it ~10 times per chunk: 2.5 to 4.8 ms a frame in
		 * the harbor), then shell-sort the packed keys. Distances are
		 * positive, so their float bits order like integers. */
		{
			u64 *const keys = gc_wd_keys;
			u64 pf_t = gc_wd_pf_on > 0 ? gettime() : 0;
			int gap, a, bq;

			for( a = 0; a < norder; a++ )
			{
				union { float f; u32 u; } d;

				d.f = R_GXWorldDLChunkD2( order[a] );
				keys[a] = ( (u64)d.u << 16 ) | order[a];
			}
			for( gap = norder / 2; gap > 0; gap = gap == 2 ? 1 : ( gap * 5 ) / 11 )
			{
				for( a = gap; a < norder; a++ )
				{
					const u64 kv = keys[a];

					for( bq = a; bq >= gap && keys[bq - gap] > kv; bq -= gap )
						keys[bq] = keys[bq - gap];
					keys[bq] = kv;
				}
			}
			/* The tower test and the far-pool test only need the box and the
			 * distance just computed. Both go in the spare top bits of the
			 * order entry (chunk ids stay under 8192), so the draw passes do
			 * not redo the distance, the span and two tower tests per chunk:
			 * about fifty int-to-float conversions each, the slow kind on
			 * the Gekko. */
			for( a = 0; a < norder; a++ )
			{
				const u32 ci = (u32)( keys[a] & 0xffffu );
				union { float f; u32 u; } d;
				const float dx = gc_wd_chunk_maxs[ci * 3] - gc_wd_chunk_mins[ci * 3];
				const float dy = gc_wd_chunk_maxs[ci * 3 + 1] - gc_wd_chunk_mins[ci * 3 + 1];
				const float dz = gc_wd_chunk_maxs[ci * 3 + 2] - gc_wd_chunk_mins[ci * 3 + 2];
				const float span = Q_max( dx, Q_max( dy, dz ));

				d.u = (u32)( keys[a] >> 16 );
				order[a] = (u16)( ci
					| ( R_GXWorldDLIsTower( ci ) ? 0x8000u : 0u )
					| (( gc_wd_tex_overflow && d.f > 1400.0f * 1400.0f
						&& span * span < d.f * 0.015625f ) ? 0x4000u : 0u ));
			}
			if( gc_wd_pf_on > 0 )
			{
				gc_wd_pf[0] += gettime() - pf_t;
				gc_wd_pf_cnt[0]++;
			}
		}
		/* Shore first so cliffs/boats lock cache slots; towers take the
		 * reserved leftover (+ Force). Towers-first starved the cliff plates
		 * when IsTower matched ~12 harbor chunks (gaps on the right wall). */
		if( tower_budget > budget / 3u )
			tower_budget = budget / 3u;
		tower_grant = tower_budget;
		shore_budget = budget - tower_grant;

		gc_wd_pf_t1 = gettime();
		for( pass = 0; pass < 2; pass++ )
		{
		u32 *bud;
		const qboolean want_tower = ( pass == 1 );

		if( pass == 1 )
			tower_budget = tower_grant + ( shore_budget ); /* unused shore bytes */
		bud = want_tower ? &tower_budget : &shore_budget;

		for( oi = 0; oi < norder; oi++ )
		{
			const gc_wd_batch_t *b;
			byte *chunk;
			u32 nb, k;
			qboolean forced = false;
			qboolean chunk_far = false;
			int lm_slot = 0;	/* lightmap store slot + 1, 0 = unlit */

			i = order[oi] & 0x1fffu;
			if((( order[oi] & 0x8000u ) != 0 ) != want_tower )
				continue;
			/* Past 1400 units a 128x128 texture is already on its 64x64
			 * mip; bind it from the small-slot pool. */
			/* Only when the pack has more textures than full-size slots. */
			/* A large cliff can cover much of the view even at a distance
			 * (decided with the sort, above). */
			chunk_far = ( order[oi] & 0x4000u ) != 0;
			R_GXCmprSetFar( chunk_far );
			WD_PF( 1, chunk = R_GXWorldDLResident( i, bud ));
			if( !chunk )
			{
				WD_PF( 2, chunk = R_GXWorldDLForce( i ));
				forced = chunk != NULL;
			}
			if( !chunk )
			{
				waiting++;
				continue;
			}
			if( !forced )
				gc_wd_slots[i].used_frame = gc_wd_frame;
			gc_wd_pf_bytes += gc_wd_chunk_bytes[i];
			nb = *(const u16 *)( chunk + 2 );
			GX_SetArray( GX_VA_POS, chunk + *(const u32 *)( chunk + 4 ), 6 );
			/* Version 2 header is 16 bytes: npos, nbatches, pos_ofs, lm_ofs. */
			{
				u32 lm_ofs = 0;
				byte *lm_page = NULL;
				const int prev_lm = gc_wd_chunk_lm;

				gc_wd_chunk_lm = 0;
				/* v2 header: u16 npos, u16 nbatches, u32 pos_ofs, u32 lm_ofs
				 * (page index + 1) at +8; bytes 12–15 are pad before batches. */
				if( gc_wd_hdr.version == 2 )
					lm_ofs = *(const u32 *)( chunk + 8 );
				if( lm_ofs && gc_wd_lm_stream )
				{
					WD_PF( 3, lm_page = R_GXWorldDLLmPage( lm_ofs - 1u ));
					if( lm_page )
					{
						lm_slot = (int)(( lm_page - gc_wd_lm_store ) / GC_WD_LM_PAGE_BYTES ) + 1;
						GX_InitTexObj( &gc_wd_lm_obj, lm_page, 32, 32, GX_TF_RGB565,
							GX_CLAMP, GX_CLAMP, GX_FALSE );
						GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
						GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
						gc_wd_chunk_lm = 1;
						gc_wd_lm_binds++;
					}
				}
				/* Lit TEV changes with the page — rebind materials next batch. */
				if( prev_lm != gc_wd_chunk_lm )
					cur_mat = -1;
			}
			b = (const gc_wd_batch_t *)( chunk + ( gc_wd_hdr.version == 2 ? 16 : 8 ));
			for( k = 0; k < nb; k++, b++ )
			{
				const gc_wd_mat_t *m;

				if( b->mat >= gc_wd_hdr.n_mats )
					continue;
				m = &gc_wd_mats[b->mat];
				/* Harbor water is the transparent packed sheet. The baked
				 * opaque copy sat on the same plane and Z-fought it into
				 * dark polygons. The 3D-skybox ocean keeps its baked copy. */
				if( ( m->flags & GC_WD_MAT_WATER ) && skip_water
					&& gc_wd_chunk_maxs[i * 3 + 2] > -1024 )
					continue;
				/* Plain batch of a resident chunk: draw it with its material
				 * group after the chunk loop (R_GXWorldDLDeferFlush). */
				if( gc_wd_defer_on > 0 && !forced && gc_wd_defer_n < GC_WD_MAX_CHUNKS )
				{
					gc_wd_keys[gc_wd_defer_n++] = ( (u64)i << 48 ) | ( (u64)k << 32 )
						| ( (u64)( b->mat * 2u + ( chunk_far ? 1u : 0u )) << 16 )
						| ( (u64)( lm_slot & 0xff ) << 8 );
					drawn++;
					continue;
				}
				if( (int)b->mat != cur_mat )
				{
					if( gc_wd_texnum[b->mat] < 0 )
						gc_wd_texnum[b->mat] = (s16)GC_WorldTexnumByHash( m->tex_hash );
					/* Do not permanently cache a failed bind — tip-lean may stub
					 * before the WT pack finishes streaming into the pool. */
					if( gc_wd_texnum[b->mat] <= 0 )
					{
						static int miss_logged;
						int again = GC_WorldTexnumByHash( m->tex_hash );

						if( again > 0 )
							gc_wd_texnum[b->mat] = (s16)again;
						else
						{
							bind_miss++;
							if( miss_logged < 8 )
							{
								miss_logged++;
								gEngfuncs.Con_Reportf(
									"Xash3D GameCube: world DL mat bind miss mat=%u hash=0x%08x\n",
									(unsigned)b->mat, (unsigned)m->tex_hash );
							}
							continue;
						}
					}
					{
						qboolean bound;

						WD_PF( 4, bound = R_GXWorldDLBindTex( (unsigned)gc_wd_texnum[b->mat] ));
						if( !bound )
							continue;
					}
					R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
					cur_mat = (int)b->mat;
				}
				GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
				/* WorldVertexTransition (Lost Coast sand): layer 2 over layer 1,
				 * weighted by the displacement vertex alpha. Same lit recipe,
				 * drawn again with $basetexture2 and alpha blending, so it
				 * costs one extra call on blend batches only. */
				if( gc_wd_hdr.version == 2 && m->hash2 && b->mat < GC_WD_BLEND_MATS )
				{
					if( gc_wd_texnum2[b->mat] == 0 )
					{
						int t2 = GC_WorldTexnumByHash( m->hash2 );

						gc_wd_texnum2[b->mat] = (s16)( t2 > 0 ? t2 : -1 );
					}
					if( gc_wd_texnum2[b->mat] > 0
						&& R_GXWorldDLBindTex( (unsigned)gc_wd_texnum2[b->mat] ))
					{
						R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
						/* Coverage = vertex alpha, carried through the later stages. */
						GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
						GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA );
						GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
						gc_wd_tev_key = -1;
						GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP );
						GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
						GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
						GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
						GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
						blend_draws++;
					}
					/* Back to layer 1 for the spec pass and the next batch. */
					if( R_GXWorldDLBindTex( (unsigned)gc_wd_texnum[b->mat] ))
						R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
				}
				/* $envmap: reflective batches carry their normal. Main view
				 * only: the mirror pass is already a reflection. A forced
				 * chunk's memory is reused, so its batches draw now. */
				if(( b->pad & 0x8000u ) && ( m->flags & GC_WD_MAT_ENVMAP ) && !r_gx_mirror
					&& gc_wd_hdr.version == 2 && R_GXWorldDLEnvReady())
				{
					if( !forced )
					{
						if( gc_wd_envq_n < GC_WD_ENVQ_MAX )
						{
							gc_wd_envq[gc_wd_envq_n].chunk = chunk;
							gc_wd_envq[gc_wd_envq_n++].b = b;
						}
					}
					else
					{
						R_GXWorldDLEnvBegin();
						if( R_GXWorldDLEnvSetup( m, b->pad ))
							GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
						R_GXWorldDLEnvOff();
						R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
						if( R_GXWorldDLBindTex( (unsigned)gc_wd_texnum[b->mat] ))
							cur_mat = (int)b->mat;
					}
				}
				drawn++;
			}
			chunks++;
			if( want_tower )
				tower_chunks++;
			if( forced )
			{
				forced_n++;
				GX_DrawDone();
			}
		}
		}
		{
			int deferred_drawn = 0;

			R_GXWorldDLDeferFlush( &deferred_drawn );
			cur_mat = -1;
		}
		gc_wd_pf[7] += gettime() - gc_wd_pf_t1;
		gc_wd_pf[6] += gc_wd_pf_t1 - gc_wd_pf_t0;
		{ const u64 e0 = gettime(); R_GXWorldDLEnvFlush(); gc_wd_pf[8] += gettime() - e0; }
		if( !logged || ( gc_wd_frame & 255 ) == 0 )
			gEngfuncs.Con_Reportf(
				"Xash3D GameCube: world DL pvs=%d frustum_drop=%d cull=%d towers=%d forced=%d\n",
				pvs_hits, frustum_drop, frustum_cull ? 1 : 0, tower_chunks, forced_n );
	}
	R_GXCmprSetFar( false );
	R_GXFogEnd();
	if( gc_wd_pf_bytes > gc_wd_pf_bytes_max )
		gc_wd_pf_bytes_max = gc_wd_pf_bytes;
	gc_wd_pf_bytes = 0;
	if( gc_wd_pf_on > 0 && ++gc_wd_pf_frames >= 120 )
	{
		const float k = 1.0f / ( 1000.0f * 120.0f );

		{
			static unsigned last_hits, last_misses;
			unsigned hits, misses;

			R_GXCmprStats( &hits, &misses );
			gEngfuncs.Con_Reportf( "Xash3D GameCube: texture slots per frame hits=%u misses=%u; chunk ring peak %u of %u Kb\n",
				( hits - last_hits ) / 120u, ( misses - last_misses ) / 120u,
				gc_wd_pf_bytes_max / 1024u, gc_wd_cache_size / 1024u );
			gc_wd_pf_bytes_max = 0;
			last_hits = hits;
			last_misses = misses;
		}
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: worlddl ms/frame sort=%.1f resident=%.1f(%u) force=%.1f(%u) lmpage=%.1f(%u) bindtex=%.1f(%u) skyline=%.1f\n",
			(float)ticks_to_microsecs( gc_wd_pf[0] ) * k,
			(float)ticks_to_microsecs( gc_wd_pf[1] ) * k, gc_wd_pf_cnt[1] / 120u,
			(float)ticks_to_microsecs( gc_wd_pf[2] ) * k, gc_wd_pf_cnt[2] / 120u,
			(float)ticks_to_microsecs( gc_wd_pf[3] ) * k, gc_wd_pf_cnt[3] / 120u,
			(float)ticks_to_microsecs( gc_wd_pf[4] ) * k, gc_wd_pf_cnt[4] / 120u,
			(float)ticks_to_microsecs( gc_wd_pf[5] ) * k );
		gEngfuncs.Con_Reportf( "Xash3D GameCube: worlddl2 ms/call pre=%.1f select+sky=%.1f chunks=%.1f envflush=%.1f lm_miss/call=%.1f\n",
			(float)ticks_to_microsecs( gc_wd_pf[9] ) * k,
			(float)ticks_to_microsecs( gc_wd_pf[6] ) * k,
			(float)ticks_to_microsecs( gc_wd_pf[7] ) * k,
			(float)ticks_to_microsecs( gc_wd_pf[8] ) * k,
			(float)( gc_wd_lm_miss_total - gc_wd_pf_lm_miss ) / 120.0f );
		gc_wd_pf_lm_miss = gc_wd_lm_miss_total;
		memset( gc_wd_pf, 0, sizeof( gc_wd_pf ));
		memset( gc_wd_pf_cnt, 0, sizeof( gc_wd_pf_cnt ));
		gc_wd_pf_frames = 0;
	}
	R_GXTevMatEndAlphaTest();
	GX_SetNumTevStages( 1 );
	GX_SetNumTexGens( 1 );
	/* Opaque world DLs must not leave alpha-test armed for studio/HUD. */
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZCompLoc( GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );

	/* GX-native shot marks — sticky pool (WorldDL has no live msurfaces). */
	{
		extern int GC_WorldDLShotMarkFramesLeft( void );
		extern void GC_WorldDLShotMarkTick( void );
		extern int GC_WorldDLStickyShotMarkCount( void );
		extern qboolean GC_WorldDLStickyShotMark( int i, float *pos, float *normal );
		const int sticky_n = GC_WorldDLStickyShotMarkCount();
		const int pulse = GC_WorldDLShotMarkFramesLeft();

		if( drawn > 0 && org && sticky_n > 0 )
		{
			static qboolean mark_logged;
			int mi;
			vec3_t mark, nrm, right, up, p0, p1, p2, p3;
			const float s = 6.0f;

			GX_SetCullMode( GX_CULL_NONE );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP );
			GX_SetNumChans( 1 );
			GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
				GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
			GX_SetNumTexGens( 0 );
			GX_SetNumTevStages( 1 );
			GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
			GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
			GX_ClearVtxDesc();
			GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
			GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
			GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
			GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );

			for( mi = 0; mi < sticky_n; mi++ )
			{
				if( !GC_WorldDLStickyShotMark( mi, mark, nrm ))
					continue;
				if( fabsf( nrm[2] ) < 0.9f )
				{
					right[0] = nrm[1];
					right[1] = -nrm[0];
					right[2] = 0.0f;
				}
				else
				{
					right[0] = 1.0f;
					right[1] = 0.0f;
					right[2] = 0.0f;
				}
				VectorNormalize( right );
				CrossProduct( nrm, right, up );
				VectorNormalize( up );
				VectorScale( right, s, right );
				VectorScale( up, s, up );
				VectorMA( mark, 0.5f, nrm, mark );
				VectorSubtract( mark, right, p0 ); VectorAdd( p0, up, p0 );
				VectorAdd( mark, right, p1 ); VectorAdd( p1, up, p1 );
				VectorAdd( mark, right, p2 ); VectorSubtract( p2, up, p2 );
				VectorSubtract( mark, right, p3 ); VectorSubtract( p3, up, p3 );
				GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
				GX_Position3f32( p0[0], p0[1], p0[2] ); GX_Color4u8( 28, 22, 16, 210 );
				GX_Position3f32( p1[0], p1[1], p1[2] ); GX_Color4u8( 28, 22, 16, 210 );
				GX_Position3f32( p2[0], p2[1], p2[2] ); GX_Color4u8( 28, 22, 16, 210 );
				GX_Position3f32( p3[0], p3[1], p3[2] ); GX_Color4u8( 28, 22, 16, 210 );
				GX_End();
			}
			/* Restore Flipper state — leaving LEQUAL/no-ZWrite + PASSCLR
			 * punched holes through the viewmodel (ava-play99-104). */
			GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
			GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetCullMode( GX_CULL_BACK );
			GX_SetNumTexGens( 1 );
			GX_SetNumTevStages( 1 );
			GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
			GX_SetTevOp( GX_TEVSTAGE0, GX_REPLACE );
			if( !mark_logged )
			{
				mark_logged = true;
				if( GC_WorldDLStickyShotMark( 0, mark, nrm ))
					gEngfuncs.Con_Reportf(
						"Xash3D GameCube: world DL GX shot mark at (%.0f,%.0f,%.0f) sticky=%d\n",
						mark[0], mark[1], mark[2], sticky_n );
			}
			if( pulse > 0 )
				GC_WorldDLShotMarkTick();
		}
	}

	if( !logged || ( gc_wd_frame & 255 ) == 0 )
	{
		logged = true;
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: world DL frame=%d cluster=%d chunks=%d batches=%d waiting=%d bind_miss=%d lm_bind=%d lm_miss=%d\n",
			gc_wd_frame, cluster, chunks, drawn, waiting, bind_miss,
			gc_wd_lm_binds, gc_wd_lm_miss );
	}
	return drawn;
}

/* Sample a streamed WorldDL lightmap under a world point. Uses the eye
 * cluster already resolved for this frame (no BSP ARAM walk) and prefers
 * chunks already resident so props share the world LM stream. */
qboolean R_GXWorldDLSampleLight( const float *org, byte rgb[3] )
{
	int cluster, i, found = -1, x, y, tile, lx, ly;
	u32 lm_ofs = 0;
	/* ARAM DMA needs a 32-byte aligned target. A plain stack array is not,
	 * and the transfer landed on the aligned address below it: over this
	 * frame's back chain and saved LR (the PC=0x40 crash on the ride). */
	static byte hdr[32] __attribute__(( aligned( 32 )));
	byte *page;
	byte *resident;
	u16 c;
	float u, v, denom, best_d2 = 1.0e30f;

	if( !org || !rgb || !R_GXWorldDLActive() || !gc_wd_lm_stream
		|| !gc_wd_chunk_cluster || !gc_wd_chunk_ofs || !gc_wd_slots )
		return false;
	cluster = gc_wd_pvs_cluster;
	if( cluster < 0 )
		return false;
	for( i = 0; i < (int)gc_wd_hdr.n_chunks; i++ )
	{
		const s16 *imn;
		const s16 *imx;
		float cx, cy, dx, dy, d2;

		if( gc_wd_chunk_cluster[i] != (u16)cluster )
			continue;
		imn = &gc_wd_chunk_mins[i * 3];
		imx = &gc_wd_chunk_maxs[i * 3];
		if( org[0] >= (float)imn[0] - 64.0f && org[0] <= (float)imx[0] + 64.0f
			&& org[1] >= (float)imn[1] - 64.0f && org[1] <= (float)imx[1] + 64.0f )
		{
			found = i;
			break;
		}
		cx = 0.5f * ( (float)imn[0] + (float)imx[0] );
		cy = 0.5f * ( (float)imn[1] + (float)imx[1] );
		dx = org[0] - cx;
		dy = org[1] - cy;
		d2 = dx * dx + dy * dy;
		if( d2 < best_d2 )
		{
			best_d2 = d2;
			found = i;
		}
	}
	if( found < 0 )
		return false;
	resident = ( gc_wd_slots[found].cache_ofs >= 0 )
		? ( gc_wd_cache + gc_wd_slots[found].cache_ofs ) : NULL;
	if( resident )
		lm_ofs = ( gc_wd_hdr.version == 2 ) ? *(const u32 *)( resident + 8 ) : 0;
	else
	{
		if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_chunks + gc_wd_chunk_ofs[found],
			hdr, sizeof( hdr )))
			return false;
		if( gc_wd_hdr.version == 2 )
			lm_ofs = *(const u32 *)( hdr + 8 );
	}
	if( !lm_ofs )
		return false;
	page = R_GXWorldDLLmPage( lm_ofs - 1u );
	if( !page )
		return false;
	{
		const s16 *imn = &gc_wd_chunk_mins[found * 3];
		const s16 *imx = &gc_wd_chunk_maxs[found * 3];

		denom = (float)imx[0] - (float)imn[0];
		u = denom > 1.0f ? ( org[0] - (float)imn[0] ) / denom : 0.5f;
		denom = (float)imx[1] - (float)imn[1];
		v = denom > 1.0f ? ( org[1] - (float)imn[1] ) / denom : 0.5f;
	}
	if( u < 0.0f ) u = 0.0f;
	else if( u > 1.0f ) u = 1.0f;
	if( v < 0.0f ) v = 0.0f;
	else if( v > 1.0f ) v = 1.0f;
	x = (int)( u * 31.0f + 0.5f );
	y = (int)( v * 31.0f + 0.5f );
	if( x < 0 ) x = 0;
	else if( x > 31 ) x = 31;
	if( y < 0 ) y = 0;
	else if( y > 31 ) y = 31;
	tile = ( y >> 2 ) * 8 + ( x >> 2 );
	lx = x & 3;
	ly = y & 3;
	c = *(const u16 *)( page + (u32)tile * 32u + (u32)( ly * 4 + lx ) * 2u );
	rgb[0] = (byte)(( ( c >> 11 ) & 31 ) * 255 / 31 );
	rgb[1] = (byte)(( ( c >> 5 ) & 63 ) * 255 / 63 );
	rgb[2] = (byte)(( c & 31 ) * 255 / 31 );
	return true;
}

#endif /* XASH_GAMECUBE */
