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
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include "../../engine/platform/gamecube/mem_gamecube.h"

#define GC_WD_FLAG_LIGHT_X1	0x0001	/* GoldSrc lighting: lightmap pages at x1 */
#define GC_WD_FLAG_NO_NRM	0x0002	/* v2 vertices without the normal (never read): 12 bytes */
/* v2 vertices carry a one-byte index into the chunk's own RGBA table (header
 * word 12) instead of four colour bytes: 9 bytes a vertex. A lit chunk reads
 * nothing but the alpha of that colour, and the chunk cache is what the view
 * from the start of d2_lostcoast ran out of (1037 KB wanted, 906 KB of
 * cache: a dozen chunks read from ARAM again every frame, 30 fps). A chunk
 * with no table (the 3D skybox, lit by its vertex colours) keeps them. */
#define GC_WD_FLAG_CLR_INDEX	0x0004
/* Lightmap pages are LZ4 blocks. The region at the lightmap stream is [u32 n_pages][u32 offset[n_pages + 1]],
 * 32-aligned page blobs after it; a 2048-byte blob is a raw page. scripts/bake_worlddl_goldsrc.py pack_lm_pages_lz4.
 * 1 MiB of pages a map shrinks to about 290 KiB of ARAM. */
#define GC_WD_FLAG_LM_LZ4	0x0010
/* A crowded leaf has more than one lightmap page: the low three bits of a batch's pad word are its page, counted
 * from the chunk's first (scripts/bake_worlddl_goldsrc.py). GoldSrc packs only; Lost Coast uses pad for $envmap. */
#define GC_WD_FLAG_LM_MULTI	0x0040
#define GC_WD_FLAG_LM_CMPR	0x0080	/* lightmap pages are 32x32 CMPR, 512 bytes each */
/* The PVS rows are a random-access LZ4 region of GC_WD_PVS_BLOCK-byte blocks (header word ofs_pvs is the region
 * start, pvsdir row offsets stay raw offsets into the uncompressed rows). pack_blocks_lz4. About 0.4 MiB a map. */
#define GC_WD_FLAG_PVS_LZ4	0x0020
#define GC_WD_PVS_BLOCK		4096u
/* One staging buffer for every LZ4 blob read in this file (lightmap pages, PVS blocks, light-face blocks): each
 * decode finishes before the next starts. */
static byte gc_wd_stage[GC_WD_PVS_BLOCK] __attribute__(( aligned( 32 )));
static byte gc_wd_pvs_blk[GC_WD_PVS_BLOCK] __attribute__(( aligned( 32 )));
static s32 gc_wd_pvs_blk_id = -1;
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
/* DoD packs do have lightmap pages (v2, one or more per leaf), and a view of
 * dod_charlie binds about 40: with 8 slots only the nearest leaves were
 * lightmapped and the rest fell to vertex colour, in hard-edged patches. The
 * store is taken from the heap at world load when it has the room. */
#define GC_WD_LM_SLOTS_MAX	510	/* slot + 1 sits in the low 16 bits of a deferred draw record */
#define GC_WD_LM_HEAP_BYTES	( 128u * 1024u )
/* One hint entry a page. A DoD map has 2000 lit leaves and more (dod_caen), each with a page or several. */
#define GC_WD_LM_HINT		4096u
#else
#define GC_WD_LM_HINT		2048u
#define GC_WD_LM_SLOTS		192
#define GC_WD_LM_SLOTS_MAX	GC_WD_LM_SLOTS
#endif
#define GC_WD_LM_PAGE_BYTES	2048u
/* Pages stored as CMPR (512 bytes, GoldSrc packs with GC_WD_FLAG_LM_CMPR): four times the pages in the same store. */
#define GC_WD_LM_CMPR_BYTES	512u

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
extern unsigned Mod_GCWorldArraySpillGeneration( void );
extern int GC_WorldTexnumByHash( unsigned hash );
extern int GC_GetWaterFaceCount( void );
extern size_t Mod_GCBspScratchFloor( void );
extern qboolean Mod_GCLZ4Decode( const byte *ip, const byte *iend, byte *op, byte *oend );

/* Decode cost counters, logged with the 256-frame world DL line: how many LZ4 blocks were decoded and the time they
 * took (kinds: 0 lightmap page, 1 light-face record block, 2 PVS block, 3 light-face lighting block). */
static u32 gc_wd_lz4_n[4], gc_wd_lf_hits[2];
static u64 gc_wd_lz4_ticks[4];

static qboolean R_GXWdLz4Decode( int kind, const byte *ip, const byte *iend, byte *op, byte *oend )
{
	const u64 t0 = gettime();
	const qboolean ok = Mod_GCLZ4Decode( ip, iend, op, oend );

	gc_wd_lz4_ticks[kind] += gettime() - t0;
	gc_wd_lz4_n[kind]++;
	return ok;
}

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
extern qboolean R_GXWorldDLBindChecker( void );
extern qboolean R_GXTexnumMasked( unsigned texnum );
/* The last world texture bound has a cut-out mask: its batches alpha-test. */
static qboolean gc_wd_masked;
static qboolean R_GXWorldDLBind( unsigned texnum )
{
	const qboolean ok = R_GXWorldDLBindTex( texnum );

	gc_wd_masked = ok && R_GXTexnumMasked( texnum );
	return ok;
}
extern unsigned r_gx_hot_weight;
extern unsigned r_gx_cmpr_near_miss_total;
extern qboolean R_GXTexnumIsLargeChain( unsigned texnum );
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

/* Detail (I8 mod2x) and cliff emboss heights (I8) live in media/tevmat.gcm.
 * Sand uses the real normal map: IndTex warps the sun disc, then the alpha
 * mask punches the wet glints (retail $normalmapalphaenvmapmask). */
#define GC_TEV_MAX		96
#define GC_TEV_DETAIL		5440
#define GC_TEV_ENVMASK		2048
#define GC_TEV_MAX_ENVMASK	8	/* mask index base of the 32x32 table (format) */
#define GC_TEV_EMBOSS		4096
/* Only Lost Coast bakes media/tevmat.gcm (detail, envmask and emboss
 * tiles from its VMTs). Other games keep one slot of each table instead of
 * ~124 KiB of MEM1 BSS; a pack with more than one is ignored whole. */
#if defined( XASH_GC_LOSTCOAST ) && XASH_GC_LOSTCOAST
#define GC_TEV_MAX_DETAIL	8
#define GC_TEV_ENVMASK_SLOTS	8
#define GC_TEV_MAX_EMBOSS	16
#else
#define GC_TEV_MAX_DETAIL	1
#define GC_TEV_ENVMASK_SLOTS	1
#define GC_TEV_MAX_EMBOSS	1
#endif
#define GC_TEV_F_ENVMASK	0x04
#define GC_TEV_F_EMBOSS		0x20
static u8 gc_tev_rec[GC_TEV_MAX][8];
static u8 gc_tev_nmat, gc_tev_ndet, gc_tev_nmask, gc_tev_nemboss;
static s8 gc_tev_bound = -1;
/* Detail tiles stay in MEM1. The map ARAM window is the world pack; a
 * second reserve returns that same base and would wipe the BSP tree. */
static u8 gc_tev_store[GC_TEV_MAX_DETAIL * GC_TEV_DETAIL] __attribute__(( aligned( 32 )));
static GXTexObj gc_tev_obj;
static u8 gc_tev_envmask[GC_TEV_ENVMASK_SLOTS * GC_TEV_ENVMASK] __attribute__(( aligned( 32 )));
/* Mask index GC_TEV_MAX_ENVMASK and up: a second table of 32x32 masks
 * (tevmat.gcm trailer GCes). The Lost Coast church's frescoes and walls each
 * carry their own mask, nineteen more than the 64x64 table holds, and a
 * material without a mask in a table gets no reflection. */
#define GC_TEV_ENVMASK_SMALL		512
#if defined( XASH_GC_LOSTCOAST ) && XASH_GC_LOSTCOAST
#define GC_TEV_MAX_ENVMASK_SMALL	24
#else
#define GC_TEV_MAX_ENVMASK_SMALL	1
#endif
static u8 gc_tev_envmask_small[GC_TEV_MAX_ENVMASK_SMALL * GC_TEV_ENVMASK_SMALL] __attribute__(( aligned( 32 )));
static u8 gc_tev_nmask_small;
static GXTexObj gc_tev_envmask_obj;
static s8 gc_tev_envmask_bound = -1;
static u8 gc_tev_emboss[GC_TEV_MAX_EMBOSS * GC_TEV_EMBOSS] __attribute__(( aligned( 32 )));
static GXTexObj gc_tev_emboss_obj;
static s8 gc_tev_emboss_bound = -1;
/* The tiles are shade factors, not heights (GCeh trailer byte 5 = 1): half
 * of 1 + N.L across the surface for the map's sun, taken from the material's
 * normal map, and the stage is lit x tile x 2. The height version took the
 * difference of two samples four texels apart of a made-up height (the mean
 * of the normal's X and Y): a second derivative, which lit every bump's rim
 * and gave the cliffs a quilted look retail does not have. */
static qboolean gc_tev_emboss_shade;
/* d2_lostcoast env_sun → UV emboss offset (bake_tevmat.SUN_TANGENT x/y). */
#define GC_TEV_EMBOSS_EPS	(4.0f / 64.0f)
static const float gc_tev_emboss_ds = -0.7206f * GC_TEV_EMBOSS_EPS;
static const float gc_tev_emboss_dt = -0.6716f * GC_TEV_EMBOSS_EPS;
static byte *gc_wd_force;
static u32 gc_wd_force_size;
static int gc_wd_tev_gens = 1;
/* Lightmap pages: copied once from ARAM, kept at full 32×32. TEXMAP2. */
static byte gc_wd_lm_bss[GC_WD_LM_SLOTS * GC_WD_LM_PAGE_BYTES] __attribute__(( aligned( 32 )));
static byte *gc_wd_lm_store = gc_wd_lm_bss;	/* the BSS store, or gc_wd_lm_heap */
static byte *gc_wd_lm_heap;	/* a larger store from the heap, freed with the world */
static qboolean gc_wd_lm_in_arena;	/* gc_wd_lm_store is a map arena block: dropped with the arena */
static qboolean gc_wd_lm_hint_exact;	/* no more pages in the pack than hint entries */
static u32 gc_wd_lm_refused;	/* page asks that found no slot (vertex colour drawn instead) */
static int gc_wd_lm_nslots = GC_WD_LM_SLOTS;
static u32 gc_wd_lm_page_bytes = GC_WD_LM_PAGE_BYTES;	/* 2048 RGB565 or 512 CMPR */
static u8 gc_wd_lm_fmt = GX_TF_RGB565;
static u32 gc_wd_lm_stream;	/* file offset of page 0 */
static s32 gc_wd_lm_slot_page[GC_WD_LM_SLOTS_MAX];
static s32 gc_wd_lm_slot_frame[GC_WD_LM_SLOTS_MAX];
static int gc_wd_lm_miss, gc_wd_lm_miss_total;
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
/* When a slot was last asked for within its frame: chunks are walked nearest
 * first, so the highest number of last frame is the farthest page. */
static u16 gc_wd_lm_slot_seq[GC_WD_LM_SLOTS_MAX];
#define GC_WD_LM_STEAL_MARGIN 48u	/* asks: a view makes 250 to 500 a frame, several a leaf */
#define GC_WD_LM_STEALS_FRAME 24	/* 12 KiB of ARAM reads a frame at most */
#define GC_WD_LM_TOUCH( i ) ( gc_wd_lm_slot_seq[i] = seq < 0xffffu ? seq++ : seq )
#else
#define GC_WD_LM_TOUCH( i ) ((void)0)
#endif
static u32 gc_wd_pf_lm_refused;
/* A second chunk of lightmap slots from the heap, added during play when pages are refused and the heap has
 * room (R_GXWorldDLLmExtGrow), given back when the heap is tight. Slots past gc_wd_lm_nslots - gc_wd_lm_ext_n
 * live in it. A later map of a match loads with less heap and its store falls short (a return to dod_avalanche
 * got the 32 BSS pages and drew 183 to 244 leaves a frame without their lightmap); this chunk is how it
 * gets them back once the match settles. */
static byte *gc_wd_lm_ext;
static int gc_wd_lm_ext_n;

static inline byte *R_GXWdLmSlotPtr( int i )
{
	const int base_n = gc_wd_lm_nslots - gc_wd_lm_ext_n;

	if( i < base_n )
		return gc_wd_lm_store + (u32)i * gc_wd_lm_page_bytes;
	return gc_wd_lm_ext + (u32)( i - base_n ) * gc_wd_lm_page_bytes;
}

static inline int R_GXWdLmSlotOf( const byte *page )
{
	if( gc_wd_lm_ext_n && page >= gc_wd_lm_ext && page < gc_wd_lm_ext + (u32)gc_wd_lm_ext_n * gc_wd_lm_page_bytes )
		return gc_wd_lm_nslots - gc_wd_lm_ext_n + (int)(( page - gc_wd_lm_ext ) / gc_wd_lm_page_bytes );
	return (int)(( page - gc_wd_lm_store ) / gc_wd_lm_page_bytes );
}
static int gc_wd_lm_binds;
static int gc_wd_chunk_lm;	/* current chunk uses TEXMAP2 lightmap */
static GXTexObj gc_wd_lm_obj;
/* Declared early — LmPage / SetupLit sit above the rest of the WorldDL state. */
static gc_wd_header_t gc_wd_hdr;
static int gc_wd_frame;
static char gc_wd_mapbase[40];
static void R_GXWorldDLFog( f32 z_near, f32 z_far );

static qboolean R_GXWorldDLFetch( u32 file_ofs, void *dst, u32 len );
static void R_GXWorldDLLoadPVS( int cluster );
extern int R_GXCmprSlotCount( void );

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
	const qboolean small = idx >= GC_TEV_MAX_ENVMASK;

	if( small ? idx - GC_TEV_MAX_ENVMASK >= gc_tev_nmask_small : idx >= gc_tev_nmask )
		return;
	if( gc_tev_envmask_bound != (s8)idx )
	{
		if( small )
			GX_InitTexObj( &gc_tev_envmask_obj,
				gc_tev_envmask_small + (u32)( idx - GC_TEV_MAX_ENVMASK ) * GC_TEV_ENVMASK_SMALL,
				32, 32, GX_TF_I4, GX_REPEAT, GX_REPEAT, GX_FALSE );
		else
		GX_InitTexObj( &gc_tev_envmask_obj,
			gc_tev_envmask + (u32)idx * GC_TEV_ENVMASK,
			64, 64, GX_TF_I4, GX_REPEAT, GX_REPEAT, GX_FALSE );
		GX_InitTexObjFilterMode( &gc_tev_envmask_obj, GX_LINEAR, GX_LINEAR );
		gc_tev_envmask_bound = (s8)idx;
	}
	GX_LoadTexObj( &gc_tev_envmask_obj, GX_TEXMAP5 );
}

static void R_GXTevBindEmboss( u8 idx )
{
	if( idx >= gc_tev_nemboss )
		return;
	if( gc_tev_emboss_bound != (s8)idx )
	{
		GX_InitTexObj( &gc_tev_emboss_obj,
			gc_tev_emboss + (u32)idx * GC_TEV_EMBOSS,
			64, 64, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE );
		GX_InitTexObjFilterMode( &gc_tev_emboss_obj, GX_LINEAR, GX_LINEAR );
		gc_tev_emboss_bound = (s8)idx;
	}
	/* TEXMAP3: soft-decal alpha is mutually exclusive with emboss mats. */
	GX_LoadTexObj( &gc_tev_emboss_obj, GX_TEXMAP3 );
}

/* The flags that change the material's own stages. A reflection mask alone
 * does not (it is read by the reflection pass): such a material draws with
 * the plain lit stages, without a TEV set-up of its own at every change of
 * material. */
static int R_GXTevMatFlags( int id )
{
	if( id < 1 || id > (int)gc_tev_nmat )
		return 0;
	return gc_tev_rec[id - 1][0] & ~GC_TEV_F_ENVMASK;
}

static int R_GXTevMatEnvMask( int id )
{
	if( id < 1 || id > (int)gc_tev_nmat )
		return -1;
	if( !( gc_tev_rec[id - 1][0] & GC_TEV_F_ENVMASK ))
		return -1;
	if( gc_tev_rec[id - 1][5] >= GC_TEV_MAX_ENVMASK
		? gc_tev_rec[id - 1][5] - GC_TEV_MAX_ENVMASK >= gc_tev_nmask_small
		: gc_tev_rec[id - 1][5] >= gc_tev_nmask )
		return -1;
	return gc_tev_rec[id - 1][5];
}

/* True when the material's envmap mask is one of the small-table ones. */
static qboolean R_GXTevMatEnvSmall( int id )
{
	return id >= 1 && id <= (int)gc_tev_nmat && ( gc_tev_rec[id - 1][0] & GC_TEV_F_ENVMASK )
		&& gc_tev_rec[id - 1][5] >= GC_TEV_MAX_ENVMASK;
}

static int R_GXTevMatEmboss( int id )
{
	u8 flags, idx;

	if( id < 1 || id > (int)gc_tev_nmat )
		return -1;
	flags = gc_tev_rec[id - 1][0];
	if( !( flags & GC_TEV_F_EMBOSS ))
		return -1;
	/* Height index lives in the envmask byte, or in the detail byte when
	 * the material also carries an envmask (shared 8-byte record). */
	idx = ( flags & GC_TEV_F_ENVMASK ) ? gc_tev_rec[id - 1][1]
					   : gc_tev_rec[id - 1][5];
	if( idx >= gc_tev_nemboss )
		return -1;
	return idx;
}

/*
 * One copy of each page, full 32×32. A hit returns the cached bytes and
 * does not touch ARAM. A miss copies the page once into a slot the GPU
 * is done with (the frame starts with DrawDone). Slots already used this
 * frame are left alone.
 */
/* Page `page` into the 2048-byte slot dst (32-aligned). A compressed page is staged in a 2 KiB buffer and decoded into
 * the slot (an exact-fit slot cannot be decoded in place: the last match always ends a byte past the read pointer). offset[i] is the blob start (32-aligned)
 * with the count of zero padding bytes after the LZ4 stream in its low 5 bits. */
static qboolean R_GXWorldDLLmFetch( u32 page, byte *dst )
{
	static byte dir[64] __attribute__(( aligned( 32 )));
	u32 w0, w1, ofs, pad, stored;
	u32 e = 4u + 4u * page;	/* offset[page], offset[page + 1]: 8 bytes that may straddle a 32-byte line */

	if( !( gc_wd_hdr.flags & GC_WD_FLAG_LM_LZ4 ))
		return R_GXWorldDLFetch( gc_wd_lm_stream + page * gc_wd_lm_page_bytes, dst, gc_wd_lm_page_bytes );
	if( !R_GXWorldDLFetch( gc_wd_lm_stream, dir, 32 ))
		return false;
	if( page >= *(const u32 *)dir )
		return false;
	if( !R_GXWorldDLFetch( gc_wd_lm_stream + ( e & ~31u ), dir, 64 ))
		return false;
	w0 = *(const u32 *)( dir + ( e & 31u ));
	w1 = *(const u32 *)( dir + ( e & 31u ) + 4u );
	ofs = w0 & ~31u;
	pad = w0 & 31u;
	stored = ( w1 & ~31u ) - ofs;
	if( !stored || stored > gc_wd_lm_page_bytes || pad >= stored )
		return false;
	if( stored == gc_wd_lm_page_bytes && !pad )	/* a raw page */
		return R_GXWorldDLFetch( gc_wd_lm_stream + ofs, dst, gc_wd_lm_page_bytes );
	if( !R_GXWorldDLFetch( gc_wd_lm_stream + ofs, gc_wd_stage, stored ))
		return false;
	if( !R_GXWdLz4Decode( 0, gc_wd_stage, gc_wd_stage + stored - pad, dst, dst + gc_wd_lm_page_bytes ))
		return false;
	DCFlushRange( dst, gc_wd_lm_page_bytes );
	return true;
}

/* Back to the BSS store. Also the map arena reset hook: the arena hands its blocks
 * out again on the next map load, so a store in it must not outlive the map. */
static void R_GXWorldDLLmDropStore( void )
{
	int i;

	if( gc_wd_lm_in_arena || gc_wd_lm_ext )
		GX_DrawDone();	/* no list may still read the block */
	if( gc_wd_lm_ext )
		free( gc_wd_lm_ext );
	gc_wd_lm_ext = NULL;
	gc_wd_lm_ext_n = 0;
	gc_wd_lm_in_arena = false;
	gc_wd_lm_store = gc_wd_lm_bss;
	gc_wd_lm_nslots = GC_WD_LM_SLOTS;
	for( i = 0; i < GC_WD_LM_SLOTS_MAX; i++ )
	{
		gc_wd_lm_slot_page[i] = -1;
		gc_wd_lm_slot_frame[i] = -1000;
	}
}

static int gc_wd_state;	/* defined below: 0 untried, 1 ready, -1 absent */

/* Lightmap pages a frame that found no slot, averaged over the last GC_WD_LM_RATE_FRAMES frames (x16). */
#define GC_WD_LM_RATE_FRAMES 64
static u32 gc_wd_lm_rate_mark, gc_wd_lm_rate_x16;
static int gc_wd_lm_rate_frame;

static void R_GXWorldDLLmRateTick( void )
{
	if( gc_wd_frame - gc_wd_lm_rate_frame < GC_WD_LM_RATE_FRAMES )
		return;
	gc_wd_lm_rate_x16 = ( gc_wd_lm_refused - gc_wd_lm_rate_mark ) * 16u / (u32)( gc_wd_frame - gc_wd_lm_rate_frame );
	gc_wd_lm_rate_mark = gc_wd_lm_refused;
	gc_wd_lm_rate_frame = gc_wd_frame;
}

/* Refused lightmap pages a frame lately, x16 (16 = one leaf a frame drawn without its lightmap). */
u32 R_GXWorldDLLmRefusedRateX16( void )
{
	return gc_wd_state == 1 && gc_wd_lm_stream ? gc_wd_lm_rate_x16 : 0;
}

/* Bytes the lightmap store's second chunk would take at most now, 0 when it cannot have one (full, or has one). */
size_t R_GXWorldDLLmExtWant( void )
{
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
	int n;

	if( gc_wd_state != 1 || !gc_wd_lm_stream || gc_wd_lm_ext )
		return 0;
	n = GC_WD_LM_SLOTS_MAX - gc_wd_lm_nslots;
	if( n > (int)( GC_WD_LM_HEAP_BYTES / gc_wd_lm_page_bytes ))
		n = (int)( GC_WD_LM_HEAP_BYTES / gc_wd_lm_page_bytes );
	if( n < 16 )
		return 0;
	return (size_t)n * gc_wd_lm_page_bytes;
#else
	return 0;
#endif
}

/* Add the second chunk, at most max_bytes (at the start of a world pass, after GX_DrawDone). The heap has no
 * long runs, so it takes the largest of max, half and a quarter that is there. Returns bytes taken. */
size_t R_GXWorldDLLmExtGrow( size_t max_bytes )
{
	size_t bytes = R_GXWorldDLLmExtWant();
	int i, n;

	if( bytes > max_bytes )
		bytes = max_bytes - max_bytes % gc_wd_lm_page_bytes;
	while( bytes >= 16u * gc_wd_lm_page_bytes )
	{
		gc_wd_lm_ext = (byte *)memalign( 32, bytes );
		if( gc_wd_lm_ext )
			break;
		bytes = ( bytes / 2u ) - ( bytes / 2u ) % gc_wd_lm_page_bytes;
	}
	if( !gc_wd_lm_ext )
		return 0;
	n = (int)( bytes / gc_wd_lm_page_bytes );
	for( i = gc_wd_lm_nslots; i < gc_wd_lm_nslots + n; i++ )
	{
		gc_wd_lm_slot_page[i] = -1;
		gc_wd_lm_slot_frame[i] = -1000;
	}
	gc_wd_lm_ext_n = n;
	gc_wd_lm_nslots += n;
	gEngfuncs.Con_Reportf( "Xash3D GameCube: lightmap store: %d more slots from the heap (%u Kb), %d in all, %.1f pages a frame were refused\n",
		n, (unsigned)( bytes / 1024u ), gc_wd_lm_nslots, (float)gc_wd_lm_rate_x16 / 16.0f );
	return bytes;
}

/* Give the second chunk back (the heap is tight). Returns bytes freed. */
size_t R_GXWorldDLLmExtShrink( void )
{
	const size_t bytes = (size_t)gc_wd_lm_ext_n * gc_wd_lm_page_bytes;
	int i;

	if( !gc_wd_lm_ext )
		return 0;
	GX_DrawDone();
	for( i = gc_wd_lm_nslots - gc_wd_lm_ext_n; i < gc_wd_lm_nslots; i++ )
	{
		gc_wd_lm_slot_page[i] = -1;
		gc_wd_lm_slot_frame[i] = -1000;
	}
	gc_wd_lm_nslots -= gc_wd_lm_ext_n;
	gc_wd_lm_ext_n = 0;
	free( gc_wd_lm_ext );
	gc_wd_lm_ext = NULL;
	GX_InvalidateTexAll();
	gEngfuncs.Con_Reportf( "Xash3D GameCube: lightmap store: heap tight, %u Kb of slots go back, %d left\n",
		(unsigned)( bytes / 1024u ), gc_wd_lm_nslots );
	return bytes;
}

static byte *R_GXWorldDLLmPage( u32 page )
{
	static u16 hint[GC_WD_LM_HINT];	/* page & ( GC_WD_LM_HINT - 1 ) -> slot it was last found in */
	static int full_frame = -1;	/* frame in which every slot was already in use */
	int i, victim = -1;
	s32 oldest = 0x7fffffff;
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
	static int seq_frame = -1, steals;
	static u16 seq;
	int far_slot = -1;

	if( seq_frame != gc_wd_frame )
	{
		seq_frame = gc_wd_frame;
		seq = 0;
		steals = 0;
	}
#endif

	i = hint[page & ( GC_WD_LM_HINT - 1u )];
	if( i < gc_wd_lm_nslots && gc_wd_lm_slot_page[i] == (s32)page )
	{
		gc_wd_lm_slot_frame[i] = gc_wd_frame;
		GC_WD_LM_TOUCH( i );
		return R_GXWdLmSlotPtr( i );
	}
	/* A wide view asks for more pages than there are slots (500 chunks in
	 * the Lost Coast harbor, 192 slots). Once the cache is full for this
	 * frame every further request walked all the slots to learn that again:
	 * up to 2.5 ms a world pass. With no more pages than hint entries the hint is exact,
	 * so a hint miss on a full frame means the page is not resident. */
	if( full_frame == gc_wd_frame && gc_wd_lm_hint_exact )
		return NULL;
	for( i = 0; i < gc_wd_lm_nslots; i++ )
	{
		if( gc_wd_lm_slot_page[i] == (s32)page )
		{
			gc_wd_lm_slot_frame[i] = gc_wd_frame;
			GC_WD_LM_TOUCH( i );
			hint[page & ( GC_WD_LM_HINT - 1u )] = (u16)i;
			return R_GXWdLmSlotPtr( i );
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
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
		else if( gc_wd_lm_slot_frame[i] == gc_wd_frame - 1
			&& ( far_slot < 0 || gc_wd_lm_slot_seq[i] > gc_wd_lm_slot_seq[far_slot] ))
			far_slot = i;
#endif
	}
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
	/* One world pass a frame here. When the slots run out the ask is from a
	 * chunk nearer than every page of last frame that has not been asked for
	 * yet, so it takes the farthest of those. Without this a far leaf kept its
	 * slot for as long as it stayed in view and the nearer leaf that came into
	 * view was drawn vertex lit: leaves changed shading as the camera moved,
	 * and a leaf at the edge of the view lost and won its slot in turn. */
	/* Only from a page that was clearly farther: asked for at least
	 * GC_WD_LM_STEAL_MARGIN asks later last frame than this ask comes now.
	 * Two leaves at about the same distance swap places in the walk as the
	 * camera moves; without the margin they took the last slot from each
	 * other in turn. A page that is resident stays until a leaf well nearer
	 * needs its slot or it has been out of view for two frames. */
	if( victim < 0 && far_slot >= 0 && steals < GC_WD_LM_STEALS_FRAME
		&& (u32)gc_wd_lm_slot_seq[far_slot] >= (u32)seq + GC_WD_LM_STEAL_MARGIN )
	{
		victim = far_slot;
		steals++;
	}
#endif
	if( victim < 0 )
	{
		full_frame = gc_wd_frame;
		return NULL;
	}
	if( !R_GXWorldDLLmFetch( page, R_GXWdLmSlotPtr( victim ) ))
		return NULL;
	if( gc_wd_lm_slot_page[victim] >= 0 )
		GX_InvalidateTexAll();
	gc_wd_lm_slot_page[victim] = (s32)page;
	gc_wd_lm_slot_frame[victim] = gc_wd_frame;
	GC_WD_LM_TOUCH( victim );
	gc_wd_lm_miss++;
	gc_wd_lm_miss_total++;
	hint[page & ( GC_WD_LM_HINT - 1u )] = (u16)victim;
	return R_GXWdLmSlotPtr( victim );
}

/* Diffuse × lightmap page (TEXMAP2), or diffuse × vertex colour when no page. */
/* The TEV recipe R_GXTevMatAppend last left in place: ( id << 1 ) | lightmap,
 * or -1 when anything else touched the stages. Most materials share recipe
 * 0, and in the harbor nearly every batch changes material: skipping the
 * identical setup saved 1.3 ms per world pass (two passes with the mirror). */
static int gc_wd_tev_key = -1;
/* Set by the draw loops for the chunk in hand: past 1100 units, in the 3D
 * skybox and in the water mirror the $detail stage is left out. The tile
 * repeats every 70 units or so and its smallest mip is 8x8: at that range it
 * no longer averages out, and the white buildings across the harbor were
 * covered in moire. Source's detail textures mip down to nothing. */
static int gc_wd_detail_off;

/* Soft decals (recipe flag 0x10, lostcoast-gc tools/bake_infodecals.py).
 * Source blends a $translucent decal by its alpha; a CMPR texture has one
 * alpha bit, which turned graffiti into fat cut-outs. The paint decals share
 * one atlas whose alpha is a second texture, media/da_<map>.bin: 128x128 I4,
 * 8 KB resident in the map arena. */
static int gc_wd_dalpha_state;	/* 0 untried, 1 ready, -1 absent */
static GXTexObj gc_wd_dalpha_obj;
static qboolean gc_wd_soft_on;

static qboolean R_GXWorldDLDecalAlpha( void )
{
	char path[96];
	byte head[32], *mem;
	file_t *f;
	u32 w, h, bytes;

	if( gc_wd_dalpha_state )
		return gc_wd_dalpha_state > 0;
	if( GC_MapLoadBufferTailPeak() == 0 )
		return false;	/* the arena is still loading the map: ask again */
	gc_wd_dalpha_state = -1;
	Q_snprintf( path, sizeof( path ), "media/da_%s.bin", gc_wd_mapbase );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	if( gEngfuncs.fsapi->Read( f, head, 32 ) == 32 && head[0] == 'G' && head[1] == 'C'
		&& head[2] == 'd' && head[3] == 'a'
		&& ( w = ( head[4] << 8 ) | head[5] ) >= 8 && ( h = ( head[6] << 8 ) | head[7] ) >= 8
		&& w <= 256 && h <= 256 && !( w & 7 ) && !( h & 7 )
		&& ( bytes = w * h / 2u ) != 0
		&& ( mem = (byte *)GC_MapLoadBufferTailAlloc(
			Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()), bytes )) != NULL
		&& gEngfuncs.fsapi->Read( f, mem, bytes ) == (fs_offset_t)bytes )
	{
		DCFlushRange( mem, bytes );
		GX_InitTexObj( &gc_wd_dalpha_obj, mem, (u16)w, (u16)h, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE );
		GX_InitTexObjFilterMode( &gc_wd_dalpha_obj, GX_LINEAR, GX_LINEAR );
		gc_wd_dalpha_state = 1;
	}
	gEngfuncs.fsapi->Close( f );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL decal alpha %s ready=%d\n", path, gc_wd_dalpha_state > 0 );
	return gc_wd_dalpha_state > 0;
}

static void R_GXWorldDLSoftOff( void )
{
	if( !gc_wd_soft_on )
		return;
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	gc_wd_soft_on = false;
	gc_wd_tev_key = -1;
}

/* Point the vertex arrays at a chunk: positions, and its colour table when
 * the pack indexes colours. gc_wd_clr_mode: 0 unknown, 1 direct, 2 indexed. */
static u8 gc_wd_clr_mode;

static void R_GXWorldDLChunkArrays( const byte *chunk )
{
	GX_SetArray( GX_VA_POS, (void *)( chunk + *(const u32 *)( chunk + 4 )), 6 );
	if( gc_wd_hdr.flags & GC_WD_FLAG_CLR_INDEX )
	{
		const u32 clr = *(const u32 *)( chunk + 12 );
		const u8 mode = clr ? 2 : 1;

		if( gc_wd_clr_mode != mode )
		{
			GX_SetVtxDesc( GX_VA_CLR0, clr ? GX_INDEX8 : GX_DIRECT );
			gc_wd_clr_mode = mode;
		}
		if( clr )
			GX_SetArray( GX_VA_CLR0, (void *)( chunk + clr ), 4 );
	}
}

static void R_GXWorldDLSetupLit( int lightmap )
{
	gc_wd_tev_key = -1;
	R_GXWorldDLSoftOff();
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

/* id 0 restores the lit diffuse stage. Authored $detail adds mod2x (unlit
 * chunks only). $bumpmap without $envmap adds emboss on lit colour. */
static void R_GXTevMatAppendStages( int id, int a, int b );

/* GoldSrc '{' textures carry their mask in the CMPR alpha bit, but the lit
 * setup turned the alpha test off for every batch, so lattices and netting
 * drew their holes black (dod_charlie's flag tower *178, the trench netting
 * *114). A plain material (no Source $alphatest record) cuts out when its
 * texture is masked. */
static void R_GXTevMatAppend( int id, int a, int b )
{
	R_GXTevMatAppendStages( id, a, b );
	if( id < 1 || id > (int)gc_tev_nmat )
	{
		if( gc_wd_masked )
		{
			GX_SetAlphaCompare( GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetZCompLoc( GX_FALSE );
		}
		else
		{
			GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetZCompLoc( GX_TRUE );
		}
	}
}

static void R_GXTevMatAppendStages( int id, int a, int b )
{
	u8 flags, didx, aref;
	u16 scale_fp;
	float scale;
	Mtx tm;
	int base_stages;
	int stages;
	int ntex;
	int eidx;
	u8 tc0, tc1, st;

	(void)a;
	(void)b;
	if( id < 1 || id > (int)gc_tev_nmat )
		id = 0;
	if( gc_wd_tev_key == (( id << 2 ) | ( gc_wd_detail_off ? 2 : 0 ) | ( gc_wd_chunk_lm ? 1 : 0 )))
		return;
	R_GXWorldDLSetupLit( gc_wd_chunk_lm );
	gc_wd_tev_key = ( id << 2 ) | ( gc_wd_detail_off ? 2 : 0 ) | ( gc_wd_chunk_lm ? 1 : 0 );
	base_stages = gc_wd_chunk_lm ? 2 : 1;
	stages = base_stages;
	ntex = gc_wd_tev_gens > 1 ? 2 : 1;
	if( !id )
		return;
	flags = gc_tev_rec[id - 1][0];
	didx = gc_tev_rec[id - 1][1];
	scale_fp = (u16)( ( gc_tev_rec[id - 1][2] << 8 ) | gc_tev_rec[id - 1][3] );
	aref = gc_tev_rec[id - 1][4];
	/* Authored $detail, mod2x over the lit colour, on lightmapped chunks too.
	 * It was held back to unlit chunks when it punched white voids into the
	 * harbor cliffs; with the lighting as it is now it does not (checked at
	 * the harbor, the cliff path, the courtyard and the church), and without
	 * it the cliff beside the path was a smear: its 256x256 base covers
	 * 1024 units, four units a texel, and the detail tile is what Source
	 * puts the rock's grain in. */
	if( ( flags & 0x01 ) && scale_fp && didx < gc_tev_ndet && !gc_wd_detail_off )
	{
		scale = (float)scale_fp / 256.0f;
		R_GXTevBindDetail( didx );
		tm[0][0] = scale; tm[0][1] = 0.0f; tm[0][2] = 0.0f; tm[0][3] = 0.0f;
		tm[1][0] = 0.0f; tm[1][1] = scale; tm[1][2] = 0.0f; tm[1][3] = 0.0f;
		tm[2][0] = 0.0f; tm[2][1] = 0.0f; tm[2][2] = 1.0f; tm[2][3] = 0.0f;
		GX_LoadTexMtxImm( tm, GX_TEXMTX0, GX_MTX2x4 );
		ntex = gc_wd_tev_gens > 1 ? 3 : 2;
		GX_SetNumTexGens( (u8)ntex );
		GX_SetTexCoordGen( GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0 );
		stages = base_stages + 1;
		GX_SetNumTevStages( (u8)stages );
		GX_SetTevOrder( GX_TEVSTAGE0 + base_stages, GX_TEXCOORD2, GX_TEXMAP1, GX_COLORNULL );
		GX_SetTevColorIn( GX_TEVSTAGE0 + base_stages, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
		GX_SetTevColorOp( GX_TEVSTAGE0 + base_stages, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0 + base_stages, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( GX_TEVSTAGE0 + base_stages, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	/* Cliff / rock emboss: lit × (0.5 + h(uv) − h(uv+d)) × 2. Works on
	 * lightmapped chunks (the old mod2x bump relief did not). */
	eidx = R_GXTevMatEmboss( id );
	if( eidx >= 0 && !( flags & 0x10 ))
	{
		R_GXTevBindEmboss( (u8)eidx );
		if( gc_tev_emboss_shade )
		{
			tm[0][0] = 1.0f; tm[0][1] = 0.0f; tm[0][2] = 0.0f; tm[0][3] = 0.0f;
			tm[1][0] = 0.0f; tm[1][1] = 1.0f; tm[1][2] = 0.0f; tm[1][3] = 0.0f;
			tm[2][0] = 0.0f; tm[2][1] = 0.0f; tm[2][2] = 1.0f; tm[2][3] = 0.0f;
			GX_LoadTexMtxImm( tm, GX_TEXMTX1, GX_MTX2x4 );
			tc0 = (u8)( GX_TEXCOORD0 + ntex );
			GX_SetTexCoordGen( tc0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1 );
			ntex += 1;
			GX_SetNumTexGens( (u8)ntex );
			st = (u8)( GX_TEVSTAGE0 + stages );
			GX_SetTevDirect( st );
			GX_SetTevOrder( st, tc0, GX_TEXMAP3, GX_COLORNULL );
			GX_SetTevColorIn( st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
			GX_SetTevColorOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
			GX_SetTevAlphaIn( st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
			GX_SetTevAlphaOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
			stages += 1;
			GX_SetNumTevStages( (u8)stages );
		}
		else
		{
		/* TEXCOORD for h(uv) and h(uv+d): identity scale, sun offset in mtx1. */
		tm[0][0] = 1.0f; tm[0][1] = 0.0f; tm[0][2] = 0.0f; tm[0][3] = 0.0f;
		tm[1][0] = 0.0f; tm[1][1] = 1.0f; tm[1][2] = 0.0f; tm[1][3] = 0.0f;
		tm[2][0] = 0.0f; tm[2][1] = 0.0f; tm[2][2] = 1.0f; tm[2][3] = 0.0f;
		GX_LoadTexMtxImm( tm, GX_TEXMTX1, GX_MTX2x4 );
		tm[0][3] = gc_tev_emboss_ds;
		tm[1][3] = gc_tev_emboss_dt;
		GX_LoadTexMtxImm( tm, GX_TEXMTX2, GX_MTX2x4 );
		tc0 = (u8)( GX_TEXCOORD0 + ntex );
		tc1 = (u8)( tc0 + 1 );
		GX_SetTexCoordGen( tc0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1 );
		GX_SetTexCoordGen( tc1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2 );
		ntex += 2;
		GX_SetNumTexGens( (u8)ntex );

		st = (u8)( GX_TEVSTAGE0 + stages );
		/* Save lit colour. HALF+h overflows 8-bit at mid-grey, so compute
		 * factor as h0 − h1 + ½ via TB_ADDHALF instead. */
		GX_SetTevDirect( st );
		GX_SetTevOrder( st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
		GX_SetTevColorIn( st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV );
		GX_SetTevColorOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0 );
		GX_SetTevAlphaIn( st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		st++;
		/* h(uv) */
		GX_SetTevDirect( st );
		GX_SetTevOrder( st, tc0, GX_TEXMAP3, GX_COLORNULL );
		GX_SetTevColorIn( st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
		GX_SetTevColorOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		st++;
		/* h0 − h(uv+d) + 0.5 */
		GX_SetTevDirect( st );
		GX_SetTevOrder( st, tc1, GX_TEXMAP3, GX_COLORNULL );
		GX_SetTevColorIn( st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV );
		GX_SetTevColorOp( st, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		st++;
		/* lit × factor × 2 */
		GX_SetTevDirect( st );
		GX_SetTevOrder( st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
		GX_SetTevColorIn( st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C0, GX_CC_ZERO );
		GX_SetTevColorOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV );
		GX_SetTevAlphaOp( st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		st++;
		stages = (int)( st - GX_TEVSTAGE0 );
		GX_SetNumTevStages( (u8)stages );
		}
	}
	/* Source $alphatest, $translucent and $decal: the alpha is the shape of
	 * the grate, rail or decal, so it cuts out on lightmapped chunks as well
	 * as vertex-lit ones. (It used to be held back on lightmapped chunks,
	 * from a time when the flag reached materials that had no cut-out; the
	 * bake now sets it from the VMT alone and encodes the alpha bit for the
	 * same materials.) Without it the rail and the Combine grate drew their
	 * holes as black blocks. */
	if( flags & 0x02 )
	{
		GX_SetAlphaCompare( GX_GEQUAL, aref ? aref : 128, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetZCompLoc( GX_FALSE );
	}
	/* Soft decal: one more stage takes the alpha from the I4 page (same
	 * texture coordinates as the atlas), and the quad blends over the wall
	 * already drawn. Decal materials are the last in the pack, so their
	 * group comes after every wall's. No depth write: nothing sorts
	 * against a decal a unit off its wall. */
	if(( flags & 0x10 ) && R_GXWorldDLDecalAlpha())
	{
		const u8 soft = (u8)( GX_TEVSTAGE0 + base_stages );

		GX_LoadTexObj( &gc_wd_dalpha_obj, GX_TEXMAP3 );
		GX_SetNumTevStages( (u8)( base_stages + 1 ));
		GX_SetTevDirect( soft );
		GX_SetTevOrder( soft, GX_TEXCOORD0, GX_TEXMAP3, GX_COLORNULL );
		GX_SetTevColorIn( soft, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV );
		GX_SetTevColorOp( soft, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( soft, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
		GX_SetTevAlphaOp( soft, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetAlphaCompare( GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetZCompLoc( GX_FALSE );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		gc_wd_soft_on = true;
	}
}
static void R_GXTevMatEndAlphaTest( void ) {}
/* Real Flipper fog lives in r_gx_bloom.c (d2_lostcoast env_fog_controller). */
extern void R_GXFogBegin( void );
extern void R_GXFogEnd( void );

static int gc_wd_state;	/* 0 untried, 1 ready, -1 absent */
static char gc_wd_map[64];
static size_t gc_wd_spill_mark;
static unsigned gc_wd_spill_gen;
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
static byte *gc_wd_cache_heap;	/* the cache when it had to come from the heap */
static byte *gc_wd_tables_heap;	/* the tables when the spill had no room */
static byte *gc_wd_force_heap;	/* -gccapture: the overflow bounce when the cache has no tail to spare */
static u32 gc_wd_force_heap_size;
qboolean R_GXCaptureMode( void );
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
	GC_MemPressureReset();	/* the next map's loads move the heap a long way */
	if( gc_wd_file )
		gEngfuncs.fsapi->Close( gc_wd_file );
	gc_wd_file = NULL;
	gc_wd_cache = NULL;
	if( gc_wd_cache_heap )
	{
		GX_DrawDone();	/* a list in it may still be in the FIFO */
		free( gc_wd_cache_heap );
		gc_wd_cache_heap = NULL;
	}
	if( gc_wd_tables_heap )
	{
		free( gc_wd_tables_heap );
		gc_wd_tables_heap = NULL;
	}
	if( gc_wd_lm_heap )
	{
		GX_DrawDone();
		free( gc_wd_lm_heap );
		gc_wd_lm_heap = NULL;
	}
	R_GXWorldDLLmDropStore();	/* an arena store goes back with the map arena */
	if( gc_wd_force_heap )
	{
		GX_DrawDone();
		free( gc_wd_force_heap );
		gc_wd_force_heap = NULL;
		gc_wd_force_heap_size = 0;
	}
	gc_wd_force = NULL;
	gc_wd_force_size = 0;
	gc_wd_slots = NULL;
	gc_wd_texnum = NULL;
	gc_wd_pvs = NULL;
	gc_wd_aram = 0;
	gc_wd_aram_prefix = gc_wd_aram_lm_start = gc_wd_aram_lm_bytes = 0;
	gc_wd_disc_reads = gc_wd_disc_bytes = 0;
	gc_wd_pvs_cluster = -2;
	gc_wd_pvs_blk_id = -1;
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
	gc_tev_nmask_small = 0;
	gc_tev_nemboss = 0;
	gc_tev_emboss_shade = false;
	gc_tev_bound = -1;
	gc_tev_envmask_bound = -1;
	gc_tev_emboss_bound = -1;
	f = gEngfuncs.fsapi->Open( "media/tevmat.gcm", "rb", false );
	if( f && gEngfuncs.fsapi->Read( f, hdr, 8 ) == 8
		&& hdr[0] == 'G' && hdr[1] == 'C' && hdr[2] == 'm' && hdr[3] == 't'
		&& hdr[4] == 0 && hdr[5] == 1 )
	{
		nmat = hdr[6];
		gc_tev_ndet = hdr[7];
		if( nmat > 0 && nmat <= GC_TEV_MAX && gc_tev_ndet <= GC_TEV_MAX_DETAIL
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
				/* Optional trailers after detail tiles: GCem then GCeh. */
				while( gEngfuncs.fsapi->Read( f, trail, 8 ) == 8
					&& trail[0] == 'G' && trail[1] == 'C' )
				{
					if( trail[2] == 'e' && trail[3] == 'm'
						&& trail[4] > 0 && trail[4] <= GC_TEV_ENVMASK_SLOTS )
					{
						u32 mbytes = (u32)trail[4] * GC_TEV_ENVMASK;

						if( gEngfuncs.fsapi->Read( f, gc_tev_envmask, mbytes ) != (fs_offset_t)mbytes )
							break;
						DCFlushRange( gc_tev_envmask, mbytes );
						gc_tev_nmask = trail[4];
						continue;
					}
					if( trail[2] == 'e' && trail[3] == 's'
						&& trail[4] > 0 && trail[4] <= GC_TEV_MAX_ENVMASK_SMALL )
					{
						u32 sbytes = (u32)trail[4] * GC_TEV_ENVMASK_SMALL;

						if( gEngfuncs.fsapi->Read( f, gc_tev_envmask_small, sbytes ) != (fs_offset_t)sbytes )
							break;
						DCFlushRange( gc_tev_envmask_small, sbytes );
						gc_tev_nmask_small = trail[4];
						continue;
					}
					if( trail[2] == 'e' && trail[3] == 'h'
						&& trail[4] > 0 && trail[4] <= GC_TEV_MAX_EMBOSS )
					{
						u32 hbytes = (u32)trail[4] * GC_TEV_EMBOSS;

						if( gEngfuncs.fsapi->Read( f, gc_tev_emboss, hbytes ) != (fs_offset_t)hbytes )
							break;
						DCFlushRange( gc_tev_emboss, hbytes );
						gc_tev_nemboss = trail[4];
						gc_tev_emboss_shade = trail[5] == 1;
						continue;
					}
					break;
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
			"Xash3D GameCube: tevmat mats=%u details=%u envmasks=%u+%u emboss=%u\n",
			(unsigned)gc_tev_nmat, (unsigned)gc_tev_ndet,
			(unsigned)gc_tev_nmask, (unsigned)gc_tev_nmask_small, (unsigned)gc_tev_nemboss );
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
		/* A full spill left the world undrawn (state -1, CapFaces already
		 * skipped for the DL): take the tables from the heap when that
		 * leaves it a working margin. */
		extern size_t GC_MemHeapFreeBytes( void );

		if( GC_MemHeapFreeBytes() >= tables + 128u * 1024u )
			gc_wd_tables_heap = tbl = (byte *)memalign( 32, tables );
		if( tbl )
		{
			memset( tbl, 0, tables );
			gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL tables %u Kb from the heap (spill full)\n",
				tables / 1024u );
		}
	}
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
					 * the props'. The near slots turn over by least recent use,
					 * which holds while a view binds fewer textures than there
					 * are slots (the map has 96, six of them decal atlases that
					 * are each seen in one place). */
					gc_wd_tex_overflow = GC_WorldTexCount() > nslots;
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
				/* A 128x128 chain is 10944 bytes and every DoD world texture
				 * but a few 256x128 ones draws at that size or smaller (the
				 * 256x256 ones drop their top mip in a 24 KB slot too), so
				 * slots of that size hold twice as many textures in the
				 * same gap. Past sixteen, a few take a whole 256x128 chain. */
				const size_t small = 10944u;
				size_t big = 44u * 1024u;
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
				if( gn > 48 )
					gn = 48;
				if( !bn )
				{
					big = 21984u;
					while( gn > 16 && bn < 6 )
					{
						gn -= 2;
						bn++;
					}
				}
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
		/* The spill can be too full for even the smallest cache: dod_caen's
		 * hull clipnodes (254 KiB) land in it on some builds and leave
		 * 48 KiB, and the map then drew no world at all. Take the smallest
		 * cache from a gap between the world arrays in the map arena, or
		 * from the heap when that leaves it a working margin. */
		if( !gc_wd_cache )
		{
			extern size_t GC_MemHeapFreeBytes( void );
			const char *from = NULL;
			size_t at;

			try_sz = cache_lo;
			at = GC_MapArenaFind( try_sz );
			if( at != (size_t)-1 )
			{
				GC_RetainMapLoadBufferBorrow();
				gc_wd_cache = (byte *)GC_MapLoadBufferTailAlloc( at, try_sz );
				if( gc_wd_cache )
				{
					gc_wd_cache_in_arena = true;
					from = "map arena gap";
				}
			}
			if( !gc_wd_cache && GC_MemHeapFreeBytes() >= try_sz + 128u * 1024u )
			{
				gc_wd_cache_heap = (byte *)memalign( 32, try_sz );
				gc_wd_cache = gc_wd_cache_heap;
				if( gc_wd_cache )
				{
					gc_wd_cache_in_arena = true;	/* not in the spill: pin the tables only */
					from = "heap";
				}
			}
			gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL chunk cache %u KiB from %s (spill free %u KiB, arena gap %u, heap free %u)\n",
				try_sz / 1024u, from ? from : "nowhere", (unsigned)( Mod_GCWorldArraySpillFreeLasting() / 1024u ),
				(unsigned)GC_MapArenaLargestGap(), (unsigned)GC_MemHeapFreeBytes());
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
	if( gc_wd_tables_heap )
	{
		if( !gc_wd_cache_in_arena )	/* only the cache is in the spill */
			Mod_GCWorldArraySpillPinBase( gc_wd_cache, try_sz );
	}
	else if( gc_wd_cache_in_arena )
		Mod_GCWorldArraySpillPinBase( tbl, tables );
	else
		Mod_GCWorldArraySpillPinBase( tbl, (size_t)(( gc_wd_cache + try_sz ) - tbl ));
	gc_wd_spill_mark = Mod_GCWorldArraySpillUsed();
	gc_wd_spill_gen = Mod_GCWorldArraySpillGeneration();

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
		gc_wd_lm_page_bytes = ( gc_wd_hdr.flags & GC_WD_FLAG_LM_CMPR ) ? GC_WD_LM_CMPR_BYTES : GC_WD_LM_PAGE_BYTES;
		gc_wd_lm_fmt = ( gc_wd_hdr.flags & GC_WD_FLAG_LM_CMPR ) ? GX_TF_CMPR : GX_TF_RGB565;
		/* Raw pages: the stream's size says how many there are. LZ4 pages: one a chunk at most. */
		gc_wd_lm_hint_exact = ( gc_wd_hdr.flags & GC_WD_FLAG_LM_LZ4 )
			? ( gc_wd_hdr.n_chunks <= GC_WD_LM_HINT && !( gc_wd_hdr.flags & GC_WD_FLAG_LM_MULTI ))
			: ( gc_wd_hdr.file_size >= gc_wd_lm_stream
				&& ( gc_wd_hdr.file_size - gc_wd_lm_stream ) / gc_wd_lm_page_bytes <= GC_WD_LM_HINT );
		gc_wd_lm_nslots = (int)( sizeof( gc_wd_lm_bss ) / gc_wd_lm_page_bytes );
#if GC_WD_LM_SLOTS_MAX > GC_WD_LM_SLOTS
		/* The store comes from a gap in the map arena first: the heap gate
		 * below (640 KiB kept free) fails on busy maps with bots, the store
		 * then fell to the 16 KiB BSS (32 CMPR pages for a view that binds
		 * 40 to 100), and leaves swapped between lightmap and vertex colour
		 * frame to frame. The arena gap is there on every DoD map and costs
		 * the heap nothing. */
		if( !gc_wd_lm_heap && !gc_wd_lm_in_arena )
		{
			/* Twice the store first: avalanche and caen show more than 254 pages down their long views, and a
			 * leaf without a slot draws vertex lit until one frees (Tim: moments of missing lightmaps). */
			static const u32 tries[] = { 2u * GC_WD_LM_HEAP_BYTES, GC_WD_LM_HEAP_BYTES, 96u * 1024u, 64u * 1024u };
			unsigned t;

			for( t = 0; t < sizeof( tries ) / sizeof( tries[0] ) && !gc_wd_lm_in_arena; t++ )
			{
				const size_t at = GC_MapArenaFind( (size_t)tries[t] );
				byte *mem;

				if( at == (size_t)-1 )
					continue;
				GC_RetainMapLoadBufferBorrow();
				mem = (byte *)GC_MapLoadBufferTailAlloc( at, (size_t)tries[t] );
				if( mem && !( (uintptr_t)mem & 31u ))
				{
					gc_wd_lm_store = mem;
					gc_wd_lm_nslots = (int)( tries[t] / gc_wd_lm_page_bytes );
					gc_wd_lm_in_arena = true;
					GC_SetMapArenaResetHook( R_GXWorldDLLmDropStore );
				}
			}
		}
		if( !gc_wd_lm_heap && !gc_wd_lm_in_arena )
		{
			extern size_t GC_MemHeapFreeBytes( void );
			/* 640 KiB stays for what the map still loads from the heap (prop chain cache, view model skins,
			 * sounds). A later map of the same match finds most of that already loaded: a return to
			 * dod_avalanche had 507 KB free, every try failed, the store fell to the 32 pages in BSS and
			 * 183 to 244 leaves a frame drew without their lightmap. The last tries keep less. */
			static const u32 tries[][2] = {
				{ 2u * GC_WD_LM_HEAP_BYTES, 640u * 1024u }, { GC_WD_LM_HEAP_BYTES, 640u * 1024u },
				{ GC_WD_LM_HEAP_BYTES, 320u * 1024u }, { 80u * 1024u, 256u * 1024u }, { 48u * 1024u, 256u * 1024u },
			};
			unsigned t;

			for( t = 0; t < sizeof( tries ) / sizeof( tries[0] ) && !gc_wd_lm_heap; t++ )
			{
				const size_t bytes = (size_t)tries[t][0];

				if( GC_MemHeapFreeBytes() < bytes + tries[t][1] )
					continue;
				gc_wd_lm_heap = (byte *)memalign( 32, bytes );
				if( gc_wd_lm_heap )
				{
					gc_wd_lm_store = gc_wd_lm_heap;
					gc_wd_lm_nslots = (int)( bytes / gc_wd_lm_page_bytes );
				}
			}
		}
#endif
		if( gc_wd_lm_nslots > GC_WD_LM_SLOTS_MAX )
			gc_wd_lm_nslots = GC_WD_LM_SLOTS_MAX;
		for( i = 0; i < (u32)gc_wd_lm_nslots; i++ )
		{
			gc_wd_lm_slot_page[i] = -1;
			gc_wd_lm_slot_frame[i] = -1000;
		}
		GX_InvalidateTexAll();
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: world DL lightmap pages stream@%u slots=%d (%u Kb%s%s)\n",
			(unsigned)gc_wd_lm_stream, gc_wd_lm_nslots,
			(unsigned)( (u32)gc_wd_lm_nslots * gc_wd_lm_page_bytes / 1024u ),
			gc_wd_lm_heap ? " from heap" : gc_wd_lm_in_arena ? " from map arena" : " BSS", gc_wd_lm_fmt == GX_TF_CMPR ? ", CMPR pages" : "" );
	}

	gc_wd_tev_gens = gc_wd_hdr.version == 2 ? 2 : 1;
	/* A map that got no texture slots from the map arena binds every world
	 * texture through one 24 KB bounce: 13 to 28 ARAM reads a frame, 2.8 to
	 * 5.8 ms (avalanche shows 37 textures at once). Its chunk cache in the
	 * spill is often far larger than any view can fill: avalanche and
	 * glider hold about 380 KB and draw 39 KB a frame. Measure the most
	 * chunk bytes one cluster's PVS can show, keep that plus a quarter, and
	 * turn the rest of the cache into texture slots. */
	if( !gc_wd_cache_in_arena && !cmpr_adopted && gc_wd_aram && gc_wd_pvs
		&& R_GXCmprSlotCount() < 8 && gc_wd_hdr.n_clusters > 0
		&& gc_wd_hdr.n_clusters * 4u <= gc_wd_cache_size
		&& !gEngfuncs.Sys_CheckParm( "-gcnocacheslots" ))
	{
		const u32 visbytes = ( gc_wd_hdr.n_clusters + 7 ) >> 3;
		u32 *const per_cluster = (u32 *)gc_wd_cache;	/* the cache is idle until the first frame */
		const u32 slot_bytes = 10944u;	/* a 128x128 chain */
		const u32 big_bytes = 21984u;	/* a 256x128 chain */
		u32 need = 0, keep, floor_bytes, c, k, total = 0, open_rows = 0;
		int nslots = 0, big_n = 0;

		memset( per_cluster, 0, gc_wd_hdr.n_clusters * 4u );
		for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
		{
			if( gc_wd_chunk_cluster[i] < gc_wd_hdr.n_clusters )
			{
				per_cluster[gc_wd_chunk_cluster[i]] += gc_wd_chunk_bytes[i];
				total += gc_wd_chunk_bytes[i];
			}
		}
		for( c = 0; c < gc_wd_hdr.n_clusters; c++ )
		{
			u32 sum = 0;

			R_GXWorldDLLoadPVS( (int)c );
			for( k = 0; k < visbytes; k++ )
			{
				u32 bits = gc_wd_pvs[k], j;

				for( j = 0; bits; j++, bits >>= 1 )
				{
					if(( bits & 1u ) && k * 8u + j < gc_wd_hdr.n_clusters )
						sum += per_cluster[k * 8u + j];
				}
			}
			/* A cluster with no row sees everything; the eye is only in
			 * one when it is outside the map. */
			if( sum >= total )
				open_rows++;
			else if( sum > need )
				need = sum;
		}
		if( open_rows == gc_wd_hdr.n_clusters )
			need = total;
		floor_bytes = Q_max( 96u * 1024u, gc_wd_hdr.max_chunk * 2u );
		/* The PVS bound is before the frustum test, and more than half of a
		 * DoD map's clusters have no row at all, so it is far above what a
		 * frame draws (avalanche: 204 KB bound, 39 KB drawn). charlie and
		 * caen run on a 96 KB cache; twice that is kept at most. */
		keep = need + (( gc_wd_hdr.max_chunk + 31u ) & ~31u );
		keep = Q_min( keep, 192u * 1024u );
		keep = ( Q_max( keep, floor_bytes ) + 32767u ) & ~32767u;
		if( gc_wd_cache_size > keep )
		{
			u32 spare = gc_wd_cache_size - keep;

			nslots = (int)Q_min( spare / slot_bytes, 48u );
			/* Past sixteen full slots, a few take whole 256x128 chains. */
			while( nslots > 16 && big_n < 6
				&& (u32)( nslots - 2 ) * slot_bytes + (u32)( big_n + 1 ) * big_bytes <= spare )
			{
				nslots -= 2;
				big_n++;
			}
		}
		if( nslots >= 8 )
		{
			byte *tex = gc_wd_cache + keep;

			gc_wd_cache_size = keep;
			R_GXCmprBounceAdopt( tex, nslots, slot_bytes, 0, 0 );
			if( big_n )
				R_GXCmprBigAdopt( tex + (size_t)nslots * slot_bytes, big_n, big_bytes );
		}
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL widest PVS %u Kb of chunks, cache %u Kb, texture slots from its tail: %d x %u + %d x %u\n",
			need / 1024u, gc_wd_cache_size / 1024u, nslots >= 8 ? nslots : 0, slot_bytes,
			nslots >= 8 ? big_n : 0, big_bytes );
	}
#if defined( XASH_DODGC_NETWORK ) && XASH_DODGC_NETWORK
	{
		/* Whatever the arena or the cache tail gave: fill up to 48 near slots from the heap, keeping 512 KiB
		 * of it for the match (bots, HUD, props and sounds took about 260 KB on avalanche with 4 bots). */
		extern int R_GXCmprExtAdopt( int want_near, unsigned slot_bytes, size_t heap_floor );

		(void)R_GXCmprExtAdopt( 48, 10944u, 512u * 1024u );
	}
#endif
	gc_wd_pvs_cluster = -2;
	gc_wd_pvs_blk_id = -1;
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
		/* -gccapture: a 96 KiB cache holds the near ground of a wide shot
		 * and nothing else. dod_charlie from the beach had ~1600 of 1830
		 * chunks in view "waiting" every frame, never drawn: the bluff top,
		 * its flag tower and the far bunkers were missing. Frame rate does
		 * not matter there, so a bounce from the heap lets every chunk in
		 * view draw, one at a time, straight from ARAM. */
		if( !gc_wd_force && R_GXCaptureMode())
		{
			extern size_t GC_MemHeapFreeBytes( void );

			if( gc_wd_force_heap && gc_wd_force_heap_size < tail )
			{
				GX_DrawDone();
				free( gc_wd_force_heap );
				gc_wd_force_heap = NULL;
			}
			if( !gc_wd_force_heap && GC_MemHeapFreeBytes() >= tail + 128u * 1024u )
			{
				gc_wd_force_heap = (byte *)memalign( 32, tail );
				gc_wd_force_heap_size = gc_wd_force_heap ? tail : 0;
			}
			if( gc_wd_force_heap )
			{
				gc_wd_force = gc_wd_force_heap;
				gc_wd_force_size = tail;
			}
			gEngfuncs.Con_Reportf( "Xash3D GameCube: capture world DL overflow bounce %u KiB %s\n",
				tail / 1024u, gc_wd_force_heap ? "from the heap" : "not allocated (heap low): far chunks will not draw" );
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
/* Version 3 (DODGC_WD_LZ4=1): the lighting lump is a random-access LZ4 region; the records stay raw (version 2 packed
 * both, and the light-point walk reads the faces of every node on a ray: 0.5 ms a frame in cache misses). Regions of GC_LF_BLOCK-byte
 * blocks (scripts/bake_worlddl_goldsrc.py pack_blocks_lz4). A block is staged, decoded into one of GC_LF_CACHE
 * block buffers (1 for the records, 2 for the lighting: a sample is read once per light style, far apart) and read
 * from there. 4 KiB of MEM1 for about 1.3 MiB of ARAM a map. */
#define GC_LF_BLOCK	1024u
#define GC_LF_CACHE	3
static u32 gc_lf_ver;
static byte gc_lf_blk[GC_LF_CACHE][GC_LF_BLOCK] __attribute__(( aligned( 32 )));
static s32 gc_lf_blk_id[GC_LF_CACHE];	/* ( region << 24 ) | block, -1 empty */
static int gc_lf_lit_rr;

/* Block `blk` of the region at ARAM offset region_ofs holding raw_total bytes -> a cached buffer, or NULL. */
static const byte *R_GXLfBlock( u32 region, u32 region_ofs, u32 raw_total, u32 blk )
{
	u32 w[2] __attribute__(( aligned( 32 )));
	u32 raw_len, rawsz, ofs, pad, stored;
	s32 id = (s32)(( region << 24 ) | blk );
	int i, slot;

	if( blk * GC_LF_BLOCK >= raw_total )
		return NULL;
	for( i = 0; i < GC_LF_CACHE; i++ )
		if( gc_lf_blk_id[i] == id )
		{
			gc_wd_lf_hits[region ? 1 : 0]++;
			return gc_lf_blk[i];
		}
	slot = region == 0 ? 0 : 1 + ( gc_lf_lit_rr++ & 1 );
	raw_len = Q_min( GC_LF_BLOCK, raw_total - blk * GC_LF_BLOCK );
	rawsz = ( raw_len + 31u ) & ~31u;
	GC_AramCopy( gc_lf_aram + region_ofs + 4u + 4u * blk, w, 8, false );
	ofs = w[0] & ~31u;
	pad = w[0] & 31u;
	stored = ( w[1] & ~31u ) - ofs;
	gc_lf_blk_id[slot] = -1;
	if( !stored || stored > rawsz || pad >= stored )
		return NULL;
	if( stored == rawsz && !pad )
		GC_AramCopy( gc_lf_aram + region_ofs + ofs, gc_lf_blk[slot], raw_len, false );
	else
	{
		GC_AramCopy( gc_lf_aram + region_ofs + ofs, gc_wd_stage, stored, false );
		if( !R_GXWdLz4Decode( region ? 3 : 1, gc_wd_stage, gc_wd_stage + stored - pad, gc_lf_blk[slot], gc_lf_blk[slot] + raw_len ))
			return NULL;
	}
	gc_lf_blk_id[slot] = id;
	return gc_lf_blk[slot];
}

/* len bytes at raw offset ofs of the record (region 0) or lighting (region 1) stream. */
static qboolean R_GXLfRead2( u32 region, u32 ofs, void *out, u32 len )
{
	const u32 region_ofs = region ? gc_lf_lit_ofs : gc_lf_rec_ofs;
	const u32 raw_total = region ? gc_lf_lit_bytes : gc_lf_faces * GC_LF_REC;
	byte *dst = (byte *)out;

	while( len )
	{
		const u32 blk = ofs / GC_LF_BLOCK, off = ofs % GC_LF_BLOCK;
		const u32 n = Q_min( len, GC_LF_BLOCK - off );
		const byte *b = R_GXLfBlock( region, region_ofs, raw_total, blk );

		if( !b )
			return false;
		memcpy( dst, b + off, n );
		dst += n;
		ofs += n;
		len -= n;
	}
	return true;
}

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
	if( gEngfuncs.fsapi->Read( f, hdr, 32 ) != 32 || memcmp( hdr, "GClf", 4 )
		|| ( hdr[1] != 1u && !(( hdr[1] == 2u || hdr[1] == 3u ) && hdr[7] == GC_LF_BLOCK && hdr[6] >= hdr[4] ))
		|| hdr[3] != 32u || hdr[4] < 32u + ( hdr[1] != 2u ? hdr[2] * GC_LF_REC : 0u ))
	{
		gEngfuncs.Con_Reportf( "Xash3D GameCube: light faces bad header %s\n", path );
		gEngfuncs.fsapi->Close( f );
		return;
	}
	size = ( ( hdr[1] >= 2u ? hdr[6] : hdr[4] + hdr[5] ) + 31u ) & ~31u;
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
	if( done < ( hdr[1] >= 2u ? hdr[6] : hdr[4] + hdr[5] ))
	{
		gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: light faces short read %u/%u\n", done, hdr[4] + hdr[5] );
		return;
	}
	gc_lf_ver = hdr[1];
	for( done = 0; done < GC_LF_CACHE; done++ )
		gc_lf_blk_id[done] = -1;
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
	if( gc_lf_ver == 2u )
		return R_GXLfRead2( 0, (u32)first * GC_LF_REC, out, (u32)num * GC_LF_REC );
	GC_AramCopy( gc_lf_aram + gc_lf_rec_ofs + (u32)first * GC_LF_REC, out, (u32)num * GC_LF_REC, false );
	return true;
}

qboolean R_GXLightSamplesRead( u32 ofs, void *out, u32 len )
{
	if( !gc_lf_faces || ofs >= gc_lf_lit_bytes || len > gc_lf_lit_bytes - ofs )
		return false;
	if( gc_lf_ver >= 2u )
		return R_GXLfRead2( 1, ofs, out, len );
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
		|| ( gc_wd_state == 1 && ( Mod_GCWorldArraySpillGeneration() != gc_wd_spill_gen
			|| Mod_GCWorldArraySpillUsed() < gc_wd_spill_mark ))))
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


/* Byte `raw` of the uncompressed PVS rows, or -1. */
static int R_GXWorldDLPvsByte( u32 raw )
{
	static byte dir[64] __attribute__(( aligned( 32 )));
	const u32 blk = raw / GC_WD_PVS_BLOCK;
	u32 w0, w1, ofs, pad, stored, e;

	if( (s32)blk == gc_wd_pvs_blk_id )
		return gc_wd_pvs_blk[raw % GC_WD_PVS_BLOCK];
	gc_wd_pvs_blk_id = -1;
	e = 4u + 4u * blk;
	if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_pvs, dir, 32 ) || blk >= *(const u32 *)dir )
		return -1;
	if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_pvs + ( e & ~31u ), dir, 64 ))
		return -1;
	w0 = *(const u32 *)( dir + ( e & 31u ));
	w1 = *(const u32 *)( dir + ( e & 31u ) + 4u );
	ofs = w0 & ~31u;
	pad = w0 & 31u;
	stored = ( w1 & ~31u ) - ofs;
	if( !stored || stored > GC_WD_PVS_BLOCK || pad >= stored )
		return -1;
	if( stored == GC_WD_PVS_BLOCK && !pad )
	{
		if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_pvs + ofs, gc_wd_pvs_blk, GC_WD_PVS_BLOCK ))
			return -1;
	}
	else
	{
		if( !R_GXWorldDLFetch( gc_wd_hdr.ofs_pvs + ofs, gc_wd_stage, stored )
			|| !R_GXWdLz4Decode( 2, gc_wd_stage, gc_wd_stage + stored - pad, gc_wd_pvs_blk, gc_wd_pvs_blk + GC_WD_PVS_BLOCK ))
			return -1;
	}
	gc_wd_pvs_blk_id = (s32)blk;
	return gc_wd_pvs_blk[raw % GC_WD_PVS_BLOCK];
}

static void R_GXWorldDLLoadPVSLz4( u32 row, u32 visbytes )
{
	u32 out = 0;

	memset( gc_wd_pvs, 0, visbytes );
	while( out < visbytes )
	{
		int b = R_GXWorldDLPvsByte( row++ );

		if( b < 0 )
			break;
		if( b )
			gc_wd_pvs[out++] = (byte)b;
		else
		{
			b = R_GXWorldDLPvsByte( row++ );
			if( b < 0 )
				break;
			out += (u32)b;
		}
	}
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
	if( gc_wd_hdr.flags & GC_WD_FLAG_PVS_LZ4 )
	{
		R_GXWorldDLLoadPVSLz4( row, visbytes );
		return;
	}
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
/* 2048 was under most DoD packs (falaise 2269, kraftstoff 2437, caen 2981,
 * zalec 3117): past it no chunk ever became resident and the whole world
 * stayed undrawn, props floating over the sky. */
#define GC_WD_RES_MAX	4096
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
	if( gc_wd_hdr.n_chunks > GC_WD_RES_MAX )
	{
		static qboolean res_max_logged;

		if( !res_max_logged )
		{
			res_max_logged = true;
			gEngfuncs.Con_Reportf( S_WARN "Xash3D GameCube: world DL pack has %u chunks, resident list holds %u: world not drawn\n",
				(uint)gc_wd_hdr.n_chunks, (uint)GC_WD_RES_MAX );
		}
		return NULL;
	}
	if( bytes > *budget || bytes > gc_wd_cache_size || bytes == 0 )
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

/* The frustum test for chunk boxes, in integers. The boxes are s16, and the
 * float version converted up to twelve of them per chunk for the ~1200
 * chunks of a harbor PVS, twice a frame (the Gekko has no int-to-float
 * instruction: each one goes through the stack). Normals at 12 fraction
 * bits against whole-unit boxes: the products stay under 2^29. */
static struct
{
	s32 n[4][3];
	s32 org[3];
	u8 ofs[4][3];	/* 0 = mins, 3 = maxs: the corner furthest along the normal */
} gc_wd_boxtest;

static void R_GXWorldDLBoxTestSet( vec3_t normals[4], const float *org )
{
	int p, a;

	for( a = 0; a < 3; a++ )
		gc_wd_boxtest.org[a] = (s32)org[a];
	for( p = 0; p < 4; p++ )
	{
		for( a = 0; a < 3; a++ )
		{
			gc_wd_boxtest.n[p][a] = (s32)( normals[p][a] * 4096.0f );
			gc_wd_boxtest.ofs[p][a] = normals[p][a] >= 0.0f ? 3 : 0;
		}
	}
}

/* mins and maxs are separate arrays: box[0] = mins, box[1] = maxs. */
static inline qboolean R_GXWorldDLBoxVisible( u32 i )
{
	const s16 *const box[2] = { &gc_wd_chunk_mins[i * 3], &gc_wd_chunk_maxs[i * 3] };
	int p;

	for( p = 0; p < 4; p++ )
	{
		const s32 *n = gc_wd_boxtest.n[p];
		const u8 *o = gc_wd_boxtest.ofs[p];
		const s32 dist = n[0] * ( (s32)box[o[0] != 0][0] - gc_wd_boxtest.org[0] )
			+ n[1] * ( (s32)box[o[1] != 0][1] - gc_wd_boxtest.org[1] )
			+ n[2] * ( (s32)box[o[2] != 0][2] - gc_wd_boxtest.org[2] );

		/* -8 units as before, and one more for the eye rounded to a unit. */
		if( dist < -9 * 4096 )
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
qboolean R_GXCaptureMode( void );

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
	if( cl < 0 || (u32)cl >= gc_wd_hdr.n_clusters || R_GXCaptureMode( ))
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
			R_GXWorldDLChunkArrays( chunk );
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
	R_GXWorldDLChunkArrays( chunk );
	gc_wd_detail_off = 1;	/* the 3D skybox: always far */
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
			if( !R_GXWorldDLBind( (unsigned)gc_wd_texnum[b->mat] ))
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
	if( !( gc_wd_hdr.flags & GC_WD_FLAG_NO_NRM ))
		GX_SetVtxDesc( GX_VA_NRM, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	gc_wd_clr_mode = 1;
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX1, GX_DIRECT );
	if( !( gc_wd_hdr.flags & GC_WD_FLAG_NO_NRM ))
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
/* Chunks past 1100 units this frame. A material whose mask sits in the
 * small table (the church's frescoes and walls, the monastery's plaster)
 * takes no reflection pass out there: from the harbor those passes cost
 * 0.7 ms a frame for a sheen nobody can see at that range. */
static u8 gc_wd_env_far[( GC_WD_MAX_CHUNKS + 7 ) / 8];
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

/* Batches of one material group that lie within 768 units of the eye: how
 * much of the near view a texture bind stands for. A count of all batches
 * ranked the grass of the far hills above the sand underfoot. */
static unsigned R_GXWorldDLNearBatches( const u16 *idx, int k, int key )
{
	unsigned n = 0;
	int last = -1;
	qboolean near_chunk = false;

	for( ; k < gc_wd_defer_n && (int)(( gc_wd_keys[idx[k]] >> 16 ) & 0xffffu ) == key; k++ )
	{
		const int ci = (int)( gc_wd_keys[idx[k]] >> 48 );

		if( ci != last )
		{
			near_chunk = R_GXWorldDLChunkD2( ci ) < 768.0f * 768.0f;
			last = ci;
		}
		if( near_chunk )
			n++;
	}
	return n;
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
	r_gx_hot_weight = ( far_pool || r_gx_mirror || !R_GXTexnumIsLargeChain( (unsigned)gc_wd_texnum2[mat] )) ? 0u
		: R_GXWorldDLNearBatches( idx, k0, (int)(( gc_wd_keys[idx[k0]] >> 16 ) & 0xffffu ));
	if( !R_GXWorldDLBind( (unsigned)gc_wd_texnum2[mat] ))
		return;
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	for( k = k0; k < k1; k++ )
	{
		const u64 r = gc_wd_keys[idx[k]];
		const int ci = (int)( r >> 48 );
		const int lm = (int)( r & 0xffffu );
		byte *chunk;
		const gc_wd_batch_t *b;

		if( gc_wd_slots[ci].cache_ofs < 0 )
			continue;
		chunk = gc_wd_cache + gc_wd_slots[ci].cache_ofs;
		b = (const gc_wd_batch_t *)( chunk + 16 ) + (int)(( r >> 32 ) & 0xffffu );
		if( ci != cur_chunk )
		{
			R_GXWorldDLChunkArrays( chunk );
			cur_chunk = ci;
		}
		if( lm != cur_lm )
		{
			const int had = gc_wd_chunk_lm;

			gc_wd_chunk_lm = 0;
			if( lm > 0 )
			{
				GX_InitTexObj( &gc_wd_lm_obj, R_GXWdLmSlotPtr( lm - 1 ),
					32, 32, gc_wd_lm_fmt, GX_CLAMP, GX_CLAMP, GX_FALSE );
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
		const int detail_off = r_gx_mirror || (( gc_wd_env_far[ci >> 3] >> ( ci & 7 )) & 1 );
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
		const int lm = (int)( r & 0xffffu );
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
			R_GXWorldDLChunkArrays( chunk );
			cur_chunk = ci;
		}
		if( lm != cur_lm )
		{
			const int had = gc_wd_chunk_lm;

			gc_wd_chunk_lm = 0;
			if( lm > 0 )
			{
				GX_InitTexObj( &gc_wd_lm_obj, R_GXWdLmSlotPtr( lm - 1 ),
					32, 32, gc_wd_lm_fmt, GX_CLAMP, GX_CLAMP, GX_FALSE );
				GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
				GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
				gc_wd_chunk_lm = 1;
				gc_wd_lm_binds++;
			}
			if( had != gc_wd_chunk_lm )
				cur_key = -1;	/* lit TEV differs: rebuild the material */
			cur_lm = lm;
		}
		if( detail_off != gc_wd_detail_off )
		{
			gc_wd_detail_off = detail_off;
			if( R_GXTevMatFlags( gc_wd_mats[mat].tevmat ) & 0x01 )
				cur_key = -1;	/* the detail stage comes or goes */
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

				/* How much of the near view this bind stands for: the
				 * batches of its group (the hot window's priority). */
				if( !far_pool && !r_gx_mirror && R_GXTexnumIsLargeChain( (unsigned)texnum ))
					r_gx_hot_weight = R_GXWorldDLNearBatches( idx, k, key );
				WD_PF( 4, ok = R_GXWorldDLBind( (unsigned)texnum ));
				if( ok )
				{
					R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
					bound = true;
				}
			}
			else if( R_GXWorldDLBindChecker())
			{
				/* Material whose texture is not in the pack: the missing-texture checker, not a vanished batch. */
				gc_wd_masked = false;
				R_GXTevMatAppend( 0, 1, 1 );
				bound = true;
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
			&& !(( gc_wd_env_far[ci >> 3] >> ( ci & 7 )) & 1 && R_GXTevMatEnvSmall( gc_wd_mats[mat].tevmat ))
			&& gc_wd_hdr.version == 2 && gc_wd_envq_n < GC_WD_ENVQ_MAX && R_GXWorldDLEnvReady())
		{
			gc_wd_envq[gc_wd_envq_n].chunk = chunk;
			gc_wd_envq[gc_wd_envq_n++].b = b;
		}
	}
	gc_wd_defer_n = 0;
	R_GXCmprSetFar( false );
	R_GXWorldDLSoftOff();	/* the decal group is the last one */
}

/* Dynamic lights on the display-list world (flashlight, muzzle flashes,
 * explosions). Retail adds each dlight to the lightmap before the texture
 * multiply: diffuse * ( lightmap + light ) = the world pass + an additive
 * diffuse * light pass, drawn here over the chunks the world pass drew that
 * touch a light. Falloff is ( 1 - d / radius ) from one 32x32 I8 radial
 * texture, sampled in XY and again on its middle row for Z (both from the
 * vertex position), so no per-face data is needed. Intensity follows the
 * software renderer: ( radius - dist ) light units on the 0..255 lightmap
 * scale, times the light's colour. */
#define GC_WD_DLIGHT_CHUNKS	96
#define GC_WD_DLIGHT_TEXELS	15.0f	/* falloff radius in texels (edge texels stay 0) */
static u8 gc_wd_dl_falloff[32 * 32] __attribute__(( aligned( 32 )));
static GXTexObj gc_wd_dl_obj;
static qboolean gc_wd_dl_ready;
static byte *gc_wd_dl_chunk[GC_WD_DLIGHT_CHUNKS];
static u16 gc_wd_dl_index[GC_WD_DLIGHT_CHUNKS];
static int gc_wd_dl_nchunks;
static int gc_wd_dl_lights[MAX_DLIGHTS];
static int gc_wd_dl_nlights;
static u32 gc_wd_dl_stat[3];	/* frames with lights, chunk draws, overflow */

static void R_GXWorldDLDynInit( void )
{
	int x, y;

	for( y = 0; y < 32; y++ )
	{
		for( x = 0; x < 32; x++ )
		{
			const float dx = (float)x + 0.5f - 16.0f, dy = (float)y + 0.5f - 16.0f;
			float v = 1.0f - sqrtf( dx * dx + dy * dy ) / GC_WD_DLIGHT_TEXELS;

			if( v < 0.0f )
				v = 0.0f;
			/* I8 tiles are 8x4 texels. */
			gc_wd_dl_falloff[(( y >> 2 ) * 4 + ( x >> 3 )) * 32 + ( y & 3 ) * 8 + ( x & 7 )] = (u8)( v * 255.0f + 0.5f );
		}
	}
	DCFlushRange( gc_wd_dl_falloff, sizeof( gc_wd_dl_falloff ));
	GX_InitTexObj( &gc_wd_dl_obj, gc_wd_dl_falloff, 32, 32, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE );
	GX_InitTexObjFilterMode( &gc_wd_dl_obj, GX_LINEAR, GX_LINEAR );
	gc_wd_dl_ready = true;
}

/* The lights alive this frame; the world pass then records its chunks that
 * touch one of them. */
static void R_GXWorldDLDynBegin( void )
{
	int i;

	gc_wd_dl_nchunks = 0;
	gc_wd_dl_nlights = 0;
	if( r_gx_mirror || !gp_dlights || !gp_cl )
		return;
	for( i = 0; i < MAX_DLIGHTS; i++ )
	{
		const dlight_t *l = &gp_dlights[i];

		if( l->die >= gp_cl->time && l->radius > 1.0f && ( l->color.r | l->color.g | l->color.b ))
			gc_wd_dl_lights[gc_wd_dl_nlights++] = i;
	}
}

static void R_GXWorldDLDynNote( u32 i, byte *chunk )
{
	int n;

	if( !gc_wd_dl_nlights )
		return;
	for( n = 0; n < gc_wd_dl_nlights; n++ )
	{
		const dlight_t *l = &gp_dlights[gc_wd_dl_lights[n]];
		const float r = l->radius;

		if( l->origin[0] + r < gc_wd_chunk_mins[i * 3 + 0] || l->origin[0] - r > gc_wd_chunk_maxs[i * 3 + 0]
			|| l->origin[1] + r < gc_wd_chunk_mins[i * 3 + 1] || l->origin[1] - r > gc_wd_chunk_maxs[i * 3 + 1]
			|| l->origin[2] + r < gc_wd_chunk_mins[i * 3 + 2] || l->origin[2] - r > gc_wd_chunk_maxs[i * 3 + 2] )
			continue;
		if( gc_wd_dl_nchunks >= GC_WD_DLIGHT_CHUNKS )
		{
			gc_wd_dl_stat[2]++;
			return;
		}
		gc_wd_dl_index[gc_wd_dl_nchunks] = (u16)i;
		gc_wd_dl_chunk[gc_wd_dl_nchunks++] = chunk;
		return;
	}
}

static void R_GXWorldDLDynDraw( void )
{
	const u8 scale = ( gc_wd_hdr.flags & GC_WD_FLAG_LIGHT_X1 ) ? GX_CS_SCALE_1 : GX_CS_SCALE_2;
	int n, c, cur_mat;

	if( !gc_wd_dl_nlights || !gc_wd_dl_nchunks )
		return;
	if( !gc_wd_dl_ready )
		R_GXWorldDLDynInit();
	gc_wd_dl_stat[0]++;

	GX_LoadTexObj( &gc_wd_dl_obj, GX_TEXMAP7 );
	GX_SetNumChans( 0 );
	GX_SetNumIndStages( 0 );
	GX_SetNumTexGens( 3 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetTexCoordGen( GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX6 );
	GX_SetTexCoordGen( GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX7 );
	GX_SetNumTevStages( 4 );
	/* 0: XY falloff */
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP7, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	/* 1: x Z falloff */
	GX_SetTevDirect( GX_TEVSTAGE1 );
	GX_SetTevOrder( GX_TEVSTAGE1, GX_TEXCOORD2, GX_TEXMAP7, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	/* 2: x light colour (konst) */
	GX_SetTevDirect( GX_TEVSTAGE2 );
	GX_SetTevOrder( GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
	GX_SetTevKColorSel( GX_TEVSTAGE2, GX_TEV_KCSEL_K0 );
	GX_SetTevColorIn( GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	/* 3: x diffuse, the lightmap stage's scale; alpha from the diffuse */
	GX_SetTevDirect( GX_TEVSTAGE3 );
	GX_SetTevOrder( GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
	GX_SetTevAlphaOp( GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	/* Holes in alpha-tested textures stay unlit. */
	GX_SetAlphaCompare( GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZCompLoc( GX_FALSE );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_EQUAL, GX_FALSE );

	for( n = 0; n < gc_wd_dl_nlights; n++ )
	{
		const dlight_t *l = &gp_dlights[gc_wd_dl_lights[n]];
		const float r = l->radius;
		const float k = GC_WD_DLIGHT_TEXELS / ( 32.0f * r );
		const float lum = r > 255.0f ? 1.0f : r / 255.0f;
		Mtx tm;
		GXColor kc;

		memset( tm, 0, sizeof( tm ));
		tm[0][0] = k; tm[0][3] = 0.5f - l->origin[0] * k;
		tm[1][1] = k; tm[1][3] = 0.5f - l->origin[1] * k;
		GX_LoadTexMtxImm( tm, GX_TEXMTX6, GX_MTX2x4 );
		memset( tm, 0, sizeof( tm ));
		tm[0][2] = k; tm[0][3] = 0.5f - l->origin[2] * k;
		tm[1][3] = 0.5f;
		GX_LoadTexMtxImm( tm, GX_TEXMTX7, GX_MTX2x4 );
		kc.r = (u8)( l->color.r * lum );
		kc.g = (u8)( l->color.g * lum );
		kc.b = (u8)( l->color.b * lum );
		kc.a = 255;
		GX_SetTevKColor( GX_KCOLOR0, kc );

		cur_mat = -1;
		for( c = 0; c < gc_wd_dl_nchunks; c++ )
		{
			const u32 i = gc_wd_dl_index[c];
			byte *chunk = gc_wd_dl_chunk[c];
			const gc_wd_batch_t *b;
			u32 nb, kk;

			if( l->origin[0] + r < gc_wd_chunk_mins[i * 3 + 0] || l->origin[0] - r > gc_wd_chunk_maxs[i * 3 + 0]
				|| l->origin[1] + r < gc_wd_chunk_mins[i * 3 + 1] || l->origin[1] - r > gc_wd_chunk_maxs[i * 3 + 1]
				|| l->origin[2] + r < gc_wd_chunk_mins[i * 3 + 2] || l->origin[2] - r > gc_wd_chunk_maxs[i * 3 + 2] )
				continue;
			nb = *(const u16 *)( chunk + 2 );
			R_GXWorldDLChunkArrays( chunk );
			b = (const gc_wd_batch_t *)( chunk + 16 );
			for( kk = 0; kk < nb; kk++, b++ )
			{
				if( b->mat >= gc_wd_hdr.n_mats || gc_wd_texnum[b->mat] <= 0 )
					continue;
				if( (int)b->mat != cur_mat )
				{
					if( !R_GXWorldDLBind( (unsigned)gc_wd_texnum[b->mat] ))
						continue;
					cur_mat = (int)b->mat;
				}
				GX_CallDispList( chunk + b->dl_ofs, b->dl_bytes );
			}
			gc_wd_dl_stat[1]++;
		}
	}

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetZCompLoc( GX_TRUE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetNumChans( 1 );
	R_GXWorldDLSetupLit( 0 );
	if(( gc_wd_dl_stat[0] & 255 ) == 1 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world dlights frames=%u chunk draws=%u overflow=%u lights=%d chunks=%d\n",
			gc_wd_dl_stat[0], gc_wd_dl_stat[1], gc_wd_dl_stat[2], gc_wd_dl_nlights, gc_wd_dl_nchunks );
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
	int cluster, drawn = 0, chunks = 0, waiting = 0, cur_mat = -1, bind_miss = 0, bind_fail = 0;
	/* The camera's own eye. `org` may move to the world centre below when
	 * the eye sits in solid or outside the map (a film camera over the sea
	 * or inside a hedgehog): that is right for picking a PVS row, but the
	 * frustum test and the draw order need the real eye or the chunks in
	 * front of the camera are culled from the wrong place. */
	const float *eye = RI.rvp.vieworigin;
	const qboolean eye_ok = isfinite( eye[0] ) && ( eye[0] || eye[1] || eye[2] );

	gc_wd_pf_enter = gettime();
	if( gc_wd_state != 1 )
		return 0;
	/* Retire last frame's display lists before this frame fills the FIFO.
	 * Without it the second present waits forever on the GP. */
	if( gc_wd_frame > 0 )
		GX_DrawDone();
	gc_wd_frame++;
	R_GXWorldDLLmRateTick();
	if( gc_wd_file && ( gc_wd_frame & 127 ) == 0 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL disc totals reads=%u bytes=%u frame=%d\n",
			gc_wd_disc_reads, gc_wd_disc_bytes, gc_wd_frame );
	gc_wd_lm_miss = 0;
	gc_wd_lm_binds = 0;
	gc_wd_chunk_lm = 0;
	/* First ~90 frames: pull a denser visible set so CapFaces listen does not
	 * open on a starved alley; after warmup keep 512 KiB/frame lazy stream. */
	budget = ( gc_wd_frame <= 90 ) ? GC_WD_WARMUP_BUDGET : GC_WD_LOAD_BUDGET;
	/* -gccapture: frame rate does not matter, so every chunk in view loads
	 * this frame instead of streaming in over the next ones. */
	if( R_GXCaptureMode())
		budget = 0xffffffffu;
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
		 * TEX0 s16 frac 9, TEX1 lightmap u8 frac 8. 15 bytes, or 12 when
		 * the bake leaves the normal out (nothing reads it: no normal
		 * texgen, lighting channels off). */
		if( !( gc_wd_hdr.flags & GC_WD_FLAG_NO_NRM ))
			GX_SetVtxDesc( GX_VA_NRM, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
		gc_wd_clr_mode = 1;
		GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
		GX_SetVtxDesc( GX_VA_TEX1, GX_DIRECT );
		if( !( gc_wd_hdr.flags & GC_WD_FLAG_NO_NRM ))
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
	R_GXWorldDLDynBegin();
	if( gc_wd_defer_on < 0 )
		gc_wd_defer_on = gEngfuncs.Sys_CheckParm( "-gcnomatsort" ) ? 0 : 1;
	/* Full world first: draw every PVS-visible chunk. Frustum cull is a later
	 * speed knob (ava-play99-60 sat at 20 chunks with waiting=0 while the
	 * alley still looked starved). Lazy-load still gates on budget/cache. */
	{
		/* Chunks outside the camera go. -gcworlddlfull draws the whole PVS. */
		const qboolean frustum_cull = !gEngfuncs.Sys_CheckParm( "-gcworlddlfull" );
		/* -gccapture: every chunk in view, not only the eye's PVS row. */
		const qboolean capture = R_GXCaptureMode();
		int pvs_hits = 0, frustum_drop = 0, tower_chunks = 0, forced_n = 0;
		static u16 order[GC_WD_MAX_CHUNKS];
		const qboolean skip_water = GC_GetWaterFaceCount() > 0
			&& !gEngfuncs.Sys_CheckParm( "-gcbakedwater" );
		int norder = 0;
		int oi, pass;
		u32 tower_budget = 288u * 1024u;
		u32 tower_grant, shore_budget;

		R_GXWorldDLBoxTestSet( normals, ( r_gx_mirror || eye_ok ) ? RI.rvp.vieworigin : org );
		for( i = 0; i < gc_wd_hdr.n_chunks; i++ )
		{
			const u16 cl = gc_wd_chunk_cluster[i];

			if( !capture && !( gc_wd_pvs[cl >> 3] & ( 1 << ( cl & 7 ))))
				continue;
			pvs_hits++;
			/* Nothing under the water reflects in it. */
			if( r_gx_mirror && (float)gc_wd_chunk_maxs[i * 3 + 2] <= r_gx_mirror_z )
				continue;
			if( frustum_cull && !R_GXWorldDLBoxVisible( i ))
			{
				frustum_drop++;
				continue;
			}
			if( norder < GC_WD_MAX_CHUNKS )
				order[norder++] = (u16)i;
		}
		/* Spend the cache on the ground in front of the camera. Index order
		 * filled it with whatever chunk id came first and left the shore out. */
		gc_wd_sort_org = ( eye_ok && !r_gx_mirror ) ? eye : org;
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
			/* Which chunks sample the small far textures. The base rule (past
			 * 1400 units and under an eighth of the distance across) leaves
			 * some views with more near textures than slots: from the
			 * monastery terrace the whole map is in sight and 31 textures
			 * were read from ARAM again every frame (5 ms a pass). While a
			 * frame misses more than a few, the rule reaches nearer and to
			 * larger chunks, one step at a time; it steps back after two
			 * seconds without a miss. Not in the mirror pass, which has
			 * already bound its textures by then. */
			static const float far_dist[4] = { 1400.0f, 1100.0f, 850.0f, 650.0f };
			static const float far_frac[4] = { 0.015625f, 0.04f, 0.0625f, 0.11f };
			static unsigned far_seen;
			static int far_level, far_calm, far_wait = 120, far_since_back = 1000;
			float far_d2, far_span;

			if( !r_gx_mirror )
			{
				const unsigned now = r_gx_cmpr_near_miss_total;

				if( far_since_back < 1000 )
					far_since_back++;
				if( now - far_seen > 6u )
				{
					if( far_level < 3 )
						far_level++;
					/* Stepping back brought the misses straight back: the
					 * view still needs this level, so wait longer before
					 * trying again (a retry costs one frame of reads). */
					far_wait = far_since_back < 30 ? Q_min( far_wait * 4, 1920 ) : 120;
					far_calm = 0;
				}
				else if( now != far_seen )
					far_calm = 0;
				else if( ++far_calm > far_wait && far_level > 0 )
				{
					far_level--;
					far_calm = 0;
					far_since_back = 0;
				}
				far_seen = now;
			}
			far_d2 = far_dist[far_level] * far_dist[far_level];
			far_span = far_frac[far_level];
			for( a = 0; a < norder; a++ )
			{
				const u32 ci = (u32)( keys[a] & 0xffffu );
				union { float f; u32 u; } d;
				const float dx = gc_wd_chunk_maxs[ci * 3] - gc_wd_chunk_mins[ci * 3];
				const float dy = gc_wd_chunk_maxs[ci * 3 + 1] - gc_wd_chunk_mins[ci * 3 + 1];
				const float dz = gc_wd_chunk_maxs[ci * 3 + 2] - gc_wd_chunk_mins[ci * 3 + 2];
				const float span = Q_max( dx, Q_max( dy, dz ));

				d.u = (u32)( keys[a] >> 16 );
				if( d.f > 1100.0f * 1100.0f )
					gc_wd_env_far[ci >> 3] |= (u8)( 1u << ( ci & 7u ));
				else
					gc_wd_env_far[ci >> 3] &= (u8)~( 1u << ( ci & 7u ));
				order[a] = (u16)( ci
					| ( R_GXWorldDLIsTower( ci ) ? 0x8000u : 0u )
					| (( gc_wd_tex_overflow && d.f > far_d2
						&& span * span < d.f * far_span ) ? 0x4000u : 0u ));
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
			u32 lm_first = 0;	/* the chunk's first lightmap page + 1, 0 = none */
			int lm_delta = 0;	/* page of the chunk bound now, counted from the first */

			i = order[oi] & 0x1fffu;
			if((( order[oi] & 0x8000u ) != 0 ) != want_tower )
				continue;
			/* Past 1400 units a 128x128 texture is already on its 64x64
			 * mip; bind it from the small-slot pool. */
			/* Only when the pack has more textures than full-size slots. */
			/* A large cliff can cover much of the view even at a distance
			 * (decided with the sort, above). */
			chunk_far = ( order[oi] & 0x4000u ) != 0;
			{
				const int off = r_gx_mirror || (( gc_wd_env_far[i >> 3] >> ( i & 7 )) & 1 );

				if( off != gc_wd_detail_off )
				{
					gc_wd_detail_off = off;
					cur_mat = -1;	/* the detail stage comes or goes */
				}
			}
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
			{
				gc_wd_slots[i].used_frame = gc_wd_frame;
				/* A forced chunk's buffer is reused right away. */
				R_GXWorldDLDynNote( i, chunk );
			}
			gc_wd_pf_bytes += gc_wd_chunk_bytes[i];
			nb = *(const u16 *)( chunk + 2 );
			R_GXWorldDLChunkArrays( chunk );
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
					lm_first = lm_ofs;
					WD_PF( 3, lm_page = R_GXWorldDLLmPage( lm_ofs - 1u ));
					if( lm_page )
					{
						lm_slot = R_GXWdLmSlotOf( lm_page ) + 1;
						GX_InitTexObj( &gc_wd_lm_obj, lm_page, 32, 32, gc_wd_lm_fmt,
							GX_CLAMP, GX_CLAMP, GX_FALSE );
						GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
						GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
						gc_wd_chunk_lm = 1;
						gc_wd_lm_binds++;
					}
					else
						gc_wd_lm_refused++;
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
				/* A batch on another lightmap page of this chunk. A page that
				 * is not resident leaves the batch vertex lit, as a chunk
				 * without its page is. */
				if( lm_first && ( gc_wd_hdr.flags & GC_WD_FLAG_LM_MULTI )
					&& (int)( b->pad & 7u ) != lm_delta )
				{
					const int had = gc_wd_chunk_lm;
					byte *pg = NULL;

					lm_delta = (int)( b->pad & 7u );
					WD_PF( 3, pg = R_GXWorldDLLmPage( lm_first - 1u + (u32)lm_delta ));
					gc_wd_chunk_lm = 0;
					lm_slot = 0;
					if( pg )
					{
						lm_slot = R_GXWdLmSlotOf( pg ) + 1;
						GX_InitTexObj( &gc_wd_lm_obj, pg, 32, 32, gc_wd_lm_fmt,
							GX_CLAMP, GX_CLAMP, GX_FALSE );
						GX_InitTexObjFilterMode( &gc_wd_lm_obj, GX_LINEAR, GX_LINEAR );
						GX_LoadTexObj( &gc_wd_lm_obj, GX_TEXMAP2 );
						gc_wd_chunk_lm = 1;
						gc_wd_lm_binds++;
					}
					else
						gc_wd_lm_refused++;
					if( had != gc_wd_chunk_lm )
						cur_mat = -1;
				}
				/* Plain batch of a resident chunk: draw it with its material
				 * group after the chunk loop (R_GXWorldDLDeferFlush). */
				if( gc_wd_defer_on > 0 && !forced && gc_wd_defer_n < GC_WD_MAX_CHUNKS )
				{
					gc_wd_keys[gc_wd_defer_n++] = ( (u64)i << 48 ) | ( (u64)k << 32 )
						| ( (u64)( b->mat * 2u + ( chunk_far ? 1u : 0u )) << 16 )
						| (u64)( lm_slot & 0xffff );
					drawn++;
					continue;
				}
				if( (int)b->mat != cur_mat )
				{
					unsigned miss_tex = 0;

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
									"Xash3D GameCube: world DL mat bind miss mat=%u hash=0x%08x (drawn as the pink/black checker)\n",
									(unsigned)b->mat, (unsigned)m->tex_hash );
							}
							/* A material whose texture is not in the pack draws as the missing-texture checker
							 * instead of vanishing.  Not cached: the pack may still stream the real one in. */
							miss_tex = 0xFFFFFFFFu;	/* bind the checker below */
						}
					}
					{
						qboolean bound;

						if( miss_tex == 0xFFFFFFFFu )
							{
								bound = R_GXWorldDLBindChecker();
								gc_wd_masked = false;
							}
							else
								WD_PF( 4, bound = R_GXWorldDLBind( (unsigned)gc_wd_texnum[b->mat] ));
						if( !bound )
						{
							bind_fail++;
							continue;
						}
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
						&& R_GXWorldDLBind( (unsigned)gc_wd_texnum2[b->mat] ))
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
					if( R_GXWorldDLBind( (unsigned)gc_wd_texnum[b->mat] ))
						R_GXTevMatAppend( R_GXTevMatFlags( m->tevmat ) ? m->tevmat : 0, 1, 1 );
				}
				/* $envmap: reflective batches carry their normal. Main view
				 * only: the mirror pass is already a reflection. A forced
				 * chunk's memory is reused, so its batches draw now. */
				if(( b->pad & 0x8000u ) && ( m->flags & GC_WD_MAT_ENVMAP ) && !r_gx_mirror
					&& !(( gc_wd_env_far[i >> 3] >> ( i & 7 )) & 1 && R_GXTevMatEnvSmall( m->tevmat ))
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
						if( R_GXWorldDLBind( (unsigned)gc_wd_texnum[b->mat] ))
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
		/* Capture runs check every chunk in view got drawn: waiting and
		 * bind_fail should stay 0, cluster -1 means the eye is outside the
		 * map (all chunks are candidates then). */
		if( capture && !r_gx_mirror && ( gc_wd_frame & 63 ) == 0 )
			gEngfuncs.Con_Reportf(
				"Xash3D GameCube: capture world DL frame=%d eye=(%.0f,%.0f,%.0f) cluster=%d chunks=%u in_view=%d frustum_drop=%d waiting=%d bind_miss=%d bind_fail=%d forced=%d zfar=%.0f\n",
				gc_wd_frame, eye[0], eye[1], eye[2], cluster, (uint)gc_wd_hdr.n_chunks,
				norder, frustum_drop, waiting, bind_miss, bind_fail, forced_n, RI.farClip );
	}
	R_GXCmprSetFar( false );
	R_GXFogEnd();
	R_GXWorldDLDynDraw();
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
		gEngfuncs.Con_Reportf( "Xash3D GameCube: lightmap pages per frame refused=%.1f of %d slots%s, %d in play chunk; heap %u Kb%s\n",
			(float)( gc_wd_lm_refused - gc_wd_pf_lm_refused ) / 120.0f, gc_wd_lm_nslots,
			gc_wd_lm_heap ? " (heap)" : gc_wd_lm_in_arena ? " (map arena)" : " (BSS)", gc_wd_lm_ext_n,
			(unsigned)( GC_MemHeapFreeBytes() / 1024u ), GC_MemPressureTight() ? " tight" : "" );
		gc_wd_pf_lm_refused = gc_wd_lm_refused;
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
		if( gc_wd_lz4_n[0] + gc_wd_lz4_n[1] + gc_wd_lz4_n[2] + gc_wd_lz4_n[3] )
			gEngfuncs.Con_Reportf( "Xash3D GameCube: world DL LZ4 decodes (since load, %d frames): lightmap %u in %u us, lf records %u in %u us (%u hits), lf lighting %u in %u us (%u hits), pvs %u in %u us\n",
				gc_wd_frame, gc_wd_lz4_n[0], (unsigned)ticks_to_microsecs( gc_wd_lz4_ticks[0] ),
				gc_wd_lz4_n[1], (unsigned)ticks_to_microsecs( gc_wd_lz4_ticks[1] ), gc_wd_lf_hits[0],
				gc_wd_lz4_n[3], (unsigned)ticks_to_microsecs( gc_wd_lz4_ticks[3] ), gc_wd_lf_hits[1],
				gc_wd_lz4_n[2], (unsigned)ticks_to_microsecs( gc_wd_lz4_ticks[2] ));
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
	if( gc_wd_lm_fmt != GX_TF_RGB565 )
		return false;	/* reads RGB565 texels; GoldSrc packs light points from the light faces */
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
