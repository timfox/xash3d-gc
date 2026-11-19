/*
model.c - modelloader
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "common.h"
#include "mod_local.h"
#include "sprite.h"
#include "xash3d_mathlib.h"
#include "alias.h"
#include "studio.h"
#include "wadfile.h"
#include "world.h"
#include "enginefeatures.h"
#include "client.h"
#include "server.h"

static model_info_t	mod_crcinfo[MAX_MODELS];
static model_t	mod_known[MAX_MODELS];
static int	mod_numknown = 0;
poolhandle_t      com_studiocache;		// cache for submodels
#if XASH_GAMECUBE
#include "gamecube/mem_gamecube.h"
#include "platform/platform.h"
void FS_ClearFindMissCache( void );
qboolean GC_IsNewGameWorldReady( void );
void R_GXStudioInvalidateBindings( void );
qboolean R_GXWorldDLActive( void );
unsigned GC_GetNewGamePresentCount( void );

static qboolean Mod_GCIsNewGameRoute( void )
{
	return Sys_CheckParm( "-gcnewgame" ) || Sys_CheckParm( "-gcmenuplaystart" );
}

/* Map props share world-array spill with WorldDL's chunk cache. Loading them
 * during entity spawn (before WorldDL Active) left cache at 128 KiB and blacked
 * outdoor Flipper (ava-prop-6 vs prop-2's 448 KiB). Active() loads WorldDL on
 * first call so the cache slice is claimed before the first prop TryAlloc. */
static qboolean Mod_GCMapModelSpillAfterWorldDL( void )
{
	/* Active() loads the map's WorldDL on first call, which claims its chunk
	 * cache slice from the spill before the first prop allocates there. Its
	 * result is not the gate: a map without a world DL (dod_avalanche on the
	 * listen route) leaves the spill free, and the fallback is an FS_LoadFile
	 * malloc that fails with the heap exhausted ("FS_LoadZIPFile: can't alloc
	 * 50669 bytes" for by_banister, 94489 for sherman). If the spill is full
	 * TryAlloc fails and the old heap path still runs. */
	(void)R_GXWorldDLActive();
	return true;
}

/* Map props and player bodies go to the world-array spill first: a team
 * body's mesh core is about 265 KiB, and the studio arena has to keep room
 * for viewmodels, skins and the animation pool. */
size_t Mod_GCWorldArraySpillFree( void );

/* Third-person weapons (models/p_*.mdl) the DoD weapon table precaches: what a
 * bot or another player holds. Lean meshes with a small skin companion. */
static qboolean Mod_GCIsWeaponModelPath( const char *path )
{
	static const char *pweapons[] = {
		"p_garand", "p_k98", "p_mp40", "p_tommy", "p_colt", "p_luger",
		"p_amerk", "p_spade", "p_grenade", "p_stick", "p_bren", "p_stg44",
		"p_spring", "p_30cal", "p_mg42pr", "p_bazooka", "p_pschreck",
		/* BAR, British and para loadouts. */
		"p_barbu", "p_enfield", "p_enfields", "p_sten", "p_piat", "p_webley",
		"p_fairbairn", "p_mills", "p_m1carb", "p_grease", "p_fg42", "p_paraknife",
		/* Bipod down / up third-person models. */
		"p_barbd", "p_brenbd", "p_brenbu", "p_fg42bd", "p_fg42bu", "p_scopedfg42bu", "p_mg34bd", "p_mg34bu", "p_mg42bd", "p_mg42bu",
		NULL
	};
	char bare[64];
	int i;

	if( !path || !Q_stristr( path, "models/p_" ))
		return false;
	COM_FileBase( path, bare, sizeof( bare ));
	for( i = 0; pweapons[i]; i++ )
	{
		if( !Q_stricmp( bare, pweapons[i] ))
			return true;
	}
	return false;
}

static qboolean Mod_GCStudioPrefersSpill( const char *model_path )
{
	return Q_stristr( model_path, "mapmodels/" ) != NULL
		|| Q_stristr( model_path, "models/player" ) != NULL;
}

/* Real MDLs live in the 1 MiB studio BSS arena (lean meshes + viewweapons).
 * Stubbing left GetEyePosition null and stalled denser maps during spawn —
 * the GameCube can hold the early-campaign cast without that compromise. */
/* 1 MiB studio BSS — fill it. Lean meshes + *t skins keep HL1 looking right. */
#define GC_REAL_STUDIO_MAX_NPC    24
#define GC_REAL_STUDIO_MAX_VIEW   5
#define GC_REAL_STUDIO_MAX_BYTES  ( 1536 * 1024 )
#define GC_REAL_STUDIO_MAX_NPC_MENU   16
#define GC_REAL_STUDIO_MAX_BYTES_MENU ( 1536 * 1024 )

static int    gc_real_studio_npc;
static int    gc_real_studio_view;
static size_t gc_real_studio_bytes;
static char   gc_landmark_viewmodel[64];
static model_t *gc_pinned_viewmodels[GC_REAL_STUDIO_MAX_VIEW];
static int     gc_pinned_viewmodel_count;
static int     gc_deferred_studio_attempts;
/* Keep the complete lean c0a0 set and its resident T.mdl headers out of the
 * fragmented MEM1 heap.  The meshes alone are ~384 KiB; their retained skin
 * metadata brings the live set to roughly 0.75 MiB. */
/* Was 1 MiB → 184 KiB for bootstrap CDF headroom. G381: scientist.mdl lean
 * is ~171 KiB; with v_crowbar (~46 KiB) that already exceeds 184, so hallway
 * soft skins never got a BSS mesh (heap zip OOM → stub SetModel). 384 KiB
 * holds crowbar + scientist + barney (~343 KiB) for deferred T.mdl attach;
 * tip-safe c1a0a→c1a0d must stay CHANGELEVEL_READY after this bump. Native
 * Ethernet multiplayer uses the proven 184 KiB boot budget; visual studios
 * stay deferred. tex21 bumped to 384 for full v_colt skins and Host_Init
 * failed gfx.wad (unclaimed 1.32→1.12 Mb) — keep 184 on network. */
/* One studio region shared by model files (bump up from the bottom) and
 * native studio skins (from the top, ref/gx R_GCAllocStudioSkinOverflow).
 * Each side may use whatever the other leaves, so the split follows the
 * map: c0a0's cast is model-heavy, skin-heavy maps get bigger skins.  The
 * total is the old fixed model arena plus the old 384 KiB skin overflow. */
/* 384 KiB skin overflow + 80 KiB moved from the *T.mdl file scratch
 * (mod_studio.c), which now stages large companions in this region. */
#define GC_STUDIO_SKIN_SHARE_BYTES ( 464 * 1024 )
#if XASH_DODGC_NETWORK
	/* Keep 184 KiB — 768 KiB BSS starved Host_Init (ava-prop-1 heap_free=104).
	 * Avalanche lean mapmodels (~249 KiB) promote into world-array spill after
	 * WorldDL shrinks its chunk cache (see Mod_GCPromoteMapModels). */
#define GC_STUDIO_MODEL_SHARE_BYTES (184 * 1024)
#else
/* HL1 intro set on c0a0: crowbar 46 + scientist 175 + barney 129 KiB plus
 * props (~22 KiB) and the two 66 KiB animation slots placed at each reset
 * (~503 KiB).  At 384 KiB barney.mdl never fit, so both c0a0 Barneys had
 * no model: no draw, no animation, no scripted sequence (DispatchAnimEvents
 * "Gibbed monster is thinking!" every frame).  The arena was 1 MiB until
 * the Nov 7 networking merge. */
#define GC_STUDIO_MODEL_SHARE_BYTES (512 * 1024)
#endif
#define GC_STUDIO_BSS_BYTES ( GC_STUDIO_MODEL_SHARE_BYTES + GC_STUDIO_SKIN_SHARE_BYTES )
static byte    gc_studio_bss[GC_STUDIO_BSS_BYTES] __attribute__((aligned( 32 )));
static size_t  gc_studio_bss_bump;
static size_t  gc_studio_skin_top;	/* bytes skins hold at the top */
static unsigned gc_studio_skin_live;
/* Model files may grow up to the lowest skin. */
#define GC_STUDIO_MODEL_LIMIT ( GC_STUDIO_BSS_BYTES - gc_studio_skin_top )
static int     gc_studio_bss_owners; /* resident cache.data slices */
/* Pre-entity promote: mesh only — G376 skin uploads tip MEM1 and crash
 * mid-spawn (c1a1f ~entity 96). Skins re-promote after MAP_READY. */
static qboolean gc_studio_pre_entity_mesh_only;

qboolean Mod_GCStudioPreEntityMeshOnly( void )
{
	return gc_studio_pre_entity_mesh_only;
}

byte *Mod_GCStudioBssAlloc( size_t size );

/* Menu New Game: read a studio sequence-group file (scientist01.mdl ...)
 * straight into the studio arena. The heap path needed the file twice
 * (FS_LoadFile + cache copy, ~130 KiB) and failed during the c0a0 walks. */
byte *Mod_GCStudioArenaLoadFile( const char *path, fs_offset_t *length )
{
	file_t *f = FS_Open( path, "rb", false );
	fs_offset_t flen, total = 0, got;
	byte *slot;

	*length = 0;
	if( !f )
		return NULL;
	flen = FS_FileLength( f );
	slot = flen > 0 ? Mod_GCStudioBssAlloc( (size_t)flen ) : NULL;
	if( !slot )
	{
		FS_Close( f );
		return NULL;
	}
	if( GC_PrefetchRead( path, slot, (unsigned int)flen )
		|| GC_AssetCacheRead( path, slot, (unsigned int)flen ))
	{
		FS_Close( f );
		*length = flen;
		return slot;
	}
	while( total < flen && ( got = FS_Read( f, slot + total, flen - total )) > 0 )
		total += got;
	FS_Close( f );
	if( total != flen )
		return NULL; /* bump hole, rare short read */
	GC_AssetCacheStore( path, slot, (unsigned int)flen );
	*length = flen;
	Con_Reportf( "Xash3D GameCube: studio seqgroup '%s' in arena (%s) bump=%s\n",
		path, Q_memprint( (size_t)flen ), Q_memprint( gc_studio_bss_bump ));
	return slot;
}

/* Menu New Game sequence groups (scientist01..07.mdl, barney01..03.mdl, up
 * to 65 KiB each) live in a few fixed arena slots reused least-recently
 * used. Bump-allocating every group filled the 1 MiB studio arena by c0a0b
 * ("LoadCacheFile: can't load models/scientist04.mdl"). Reusing a slot
 * clears its previous owner's cache_user_t, so Cache_Check reloads it. */
#define GC_SEQGROUP_SLOTS     4
#define GC_SEQGROUP_SLOT_SIZE ( 66 * 1024 )
static struct
{
	byte         *buf;
	cache_user_t *owner;
	unsigned     used;
} gc_seqgroup_slot[GC_SEQGROUP_SLOTS];
static unsigned gc_seqgroup_clock;

static int Mod_GCSeqGroupSlotOf( const void *ptr )
{
	int i;

	for( i = 0; i < GC_SEQGROUP_SLOTS; i++ )
	{
		if( gc_seqgroup_slot[i].buf && (const byte *)ptr >= gc_seqgroup_slot[i].buf
			&& (const byte *)ptr < gc_seqgroup_slot[i].buf + GC_SEQGROUP_SLOT_SIZE )
			return i;
	}
	return -1;
}

byte *Mod_GCSeqGroupLoad( const char *path, cache_user_t *cu, fs_offset_t *length )
{
	file_t *f = NULL;
	fs_offset_t flen, total = 0, got;
	unsigned int cached = 0;
	int i, pick = -1;

	*length = 0;
	/* Animation slots are recycled least-recently-used, so the same group is
	 * read many times per map. After the first read it comes from ARAM. */
	if( GC_AssetCacheLookup( path, &cached ) && cached > 0 && cached <= GC_SEQGROUP_SLOT_SIZE )
		flen = (fs_offset_t)cached;
	else
	{
		cached = 0;
		f = FS_Open( path, "rb", false );
		if( !f )
			return NULL;
		flen = FS_FileLength( f );
	}
	if( flen <= 0 || flen > GC_SEQGROUP_SLOT_SIZE )
	{
		if( f )
			FS_Close( f );
		return NULL;
	}
	for( i = 0; i < GC_SEQGROUP_SLOTS && pick < 0; i++ )
	{
		if( !gc_seqgroup_slot[i].buf )
		{
			gc_seqgroup_slot[i].buf = Mod_GCStudioBssAlloc( GC_SEQGROUP_SLOT_SIZE );
			if( gc_seqgroup_slot[i].buf )
				pick = i;
			break;
		}
		if( !gc_seqgroup_slot[i].owner )
			pick = i;
	}
	if( pick < 0 )
	{
		for( i = 0; i < GC_SEQGROUP_SLOTS; i++ )
		{
			if( gc_seqgroup_slot[i].buf && ( pick < 0 || gc_seqgroup_slot[i].used < gc_seqgroup_slot[pick].used ))
				pick = i;
		}
	}
	if( pick < 0 )
	{
		if( f )
			FS_Close( f );
		return NULL;
	}
	if( gc_seqgroup_slot[pick].owner && Mod_GCSeqGroupSlotOf( gc_seqgroup_slot[pick].owner->data ) == pick )
		gc_seqgroup_slot[pick].owner->data = NULL;
	gc_seqgroup_slot[pick].owner = NULL;
	if( cached && GC_AssetCacheRead( path, gc_seqgroup_slot[pick].buf, cached ))
		total = flen;
	else
	{
		if( !f )
			f = FS_Open( path, "rb", false );
		if( !f )
			return NULL;
		while( total < flen && ( got = FS_Read( f, gc_seqgroup_slot[pick].buf + total, flen - total )) > 0 )
			total += got;
		if( total == flen )
			GC_AssetCacheStore( path, gc_seqgroup_slot[pick].buf, (unsigned int)flen );
	}
	if( f )
		FS_Close( f );
	if( total != flen )
		return NULL;
	gc_seqgroup_slot[pick].owner = cu;
	gc_seqgroup_slot[pick].used = ++gc_seqgroup_clock;
	cu->data = gc_seqgroup_slot[pick].buf;
	*length = flen;
	Con_Reportf( "Xash3D GameCube: studio seqgroup '%s' in slot %d (%s)%s\n", path, pick,
		Q_memprint( (size_t)flen ), cached ? " from ARAM" : "" );
	return gc_seqgroup_slot[pick].buf;
}

void Mod_GCSeqGroupTouch( const void *ptr )
{
	int i = Mod_GCSeqGroupSlotOf( ptr );

	if( i >= 0 )
		gc_seqgroup_slot[i].used = ++gc_seqgroup_clock;
}

/*
 * Viewmodel mesh slots. Bump-allocating every weapon mesh (5 resident, ~470 KiB)
 * left the shared studio region without room for the skins of the weapon that
 * loaded last. Two fixed slots hold the weapon in hand and one spare; reusing a
 * slot abandons its previous owner exactly like the CapFaces/arena-reset paths
 * (mesh pointer cleared, mempool freed). The model_t stays, the draw paths skip
 * a model without a mesh, and the next SetModel promote reads the raw MDL back
 * from the ARAM asset cache into a slot.
 */
#define GC_VM_SLOTS      2
#define GC_VM_SLOT_SIZE  ( 128 * 1024 )
static struct
{
	byte    *buf;
	model_t *owner;
	size_t  bytes;
	unsigned used;
} gc_vm_slot[GC_VM_SLOTS];
static unsigned gc_vm_clock;

static qboolean Mod_GCIsViewModelPath( const char *path )
{
	return path && Q_stristr( path, "models/v_" ) != NULL;
}

static void Mod_GCVMSlotsReset( void )
{
	memset( gc_vm_slot, 0, sizeof( gc_vm_slot ));
	gc_vm_clock = 0;
	/* Called at every studio arena reset: the animation pool lives there too. */
	Mod_GCAnimPackReset();
}

static void Mod_GCVMSlotEvict( int i )
{
	model_t *mod = gc_vm_slot[i].owner;
	int k;

	if( !mod )
		return;
	Con_Reportf( "Xash3D GameCube: viewmodel slot %d evicts '%s' (%s)\n", i,
		mod->name[0] ? mod->name : "?", Q_memprint( gc_vm_slot[i].bytes ));
	if( mod->cache.data == gc_vm_slot[i].buf )
	{
		mod->submodels = NULL;
		mod->cache.data = NULL;
		if( mod->mempool )
			Mem_FreePool( &mod->mempool );
	}
	if( gc_real_studio_view > 0 )
		gc_real_studio_view--;
	gc_real_studio_bytes = ( gc_real_studio_bytes > gc_vm_slot[i].bytes )
		? gc_real_studio_bytes - gc_vm_slot[i].bytes : 0;
	for( k = 0; k < gc_pinned_viewmodel_count; k++ )
	{
		if( gc_pinned_viewmodels[k] != mod )
			continue;
		memmove( &gc_pinned_viewmodels[k], &gc_pinned_viewmodels[k + 1],
			(size_t)( gc_pinned_viewmodel_count - k - 1 ) * sizeof( gc_pinned_viewmodels[0] ));
		gc_pinned_viewmodels[--gc_pinned_viewmodel_count] = NULL;
		break;
	}
	gc_vm_slot[i].owner = NULL;
	gc_vm_slot[i].bytes = 0;
	R_GXStudioInvalidateBindings();
}

/* A slot for a viewmodel mesh of this size, evicting the least recently used
 * owner. NULL (caller falls back to the arena bump) if it does not fit. */
static byte *Mod_GCVMSlotAlloc( size_t size )
{
	int i, pick = -1, held = -1;

	if( size == 0 || size > GC_VM_SLOT_SIZE )
		return NULL;
	for( i = 0; i < GC_VM_SLOTS; i++ )
	{
		if( !gc_vm_slot[i].buf )
		{
			gc_vm_slot[i].buf = Mod_GCStudioBssAlloc( GC_VM_SLOT_SIZE );
			if( !gc_vm_slot[i].buf )
				continue;
		}
		if( !gc_vm_slot[i].owner )
		{
			pick = i;
			break;
		}
		/* The weapon in hand is never the victim while another slot can
		 * go: late precaches of a map's other loadouts (v_sten, v_webley
		 * ... on dod_caen) evicted the held v_enfield, which then drew
		 * nothing until it was promoted back. */
		if( gc_landmark_viewmodel[0]
			&& !Q_stricmp( gc_vm_slot[i].owner->name, gc_landmark_viewmodel ))
		{
			if( held < 0 )
				held = i;
			continue;
		}
		if( pick < 0 || gc_vm_slot[i].used < gc_vm_slot[pick].used )
			pick = i;
	}
	if( pick < 0 )
		pick = held;
	if( pick < 0 )
	{
		Con_Reportf( "Xash3D GameCube: viewmodel slot alloc failed (%s, studio bump=%s)\n",
			Q_memprint( size ), Q_memprint( gc_studio_bss_bump ));
		return NULL;
	}
	Mod_GCVMSlotEvict( pick );
	return gc_vm_slot[pick].buf;
}

/* The mesh loaded into a slot belongs to mod now. */
static void Mod_GCVMSlotClaim( model_t *mod, const void *buf, size_t bytes )
{
	int i;

	for( i = 0; i < GC_VM_SLOTS; i++ )
	{
		if( gc_vm_slot[i].buf && gc_vm_slot[i].buf == buf && mod->cache.data == buf )
		{
			gc_vm_slot[i].owner = mod;
			gc_vm_slot[i].bytes = bytes;
			gc_vm_slot[i].used = ++gc_vm_clock;
			return;
		}
	}
}

/* A viewmodel's *t skin merge appends texture headers after the mesh. When
 * the mesh sits in a viewmodel slot the slot has room for it, so the merge
 * stays in place instead of moving the model to a calloc copy on the heap
 * (which also orphaned the slot). NULL when buf is no slot or it won't fit. */
byte *Mod_GCVMSlotGrow( void *buf, size_t new_size )
{
	int i;

	for( i = 0; buf && i < GC_VM_SLOTS; i++ )
	{
		if( gc_vm_slot[i].buf == buf )
			return new_size <= GC_VM_SLOT_SIZE ? (byte *)buf : NULL;
	}
	return NULL;
}

static void Mod_GCVMSlotTouch( const model_t *mod )
{
	int i;

	for( i = 0; mod && i < GC_VM_SLOTS; i++ )
		if( gc_vm_slot[i].owner == mod )
			gc_vm_slot[i].used = ++gc_vm_clock;
}

/* Inside the fixed BSS array: offsets from gc_studio_bss are meaningful. */
static qboolean Mod_GCInStudioBssArray( const void *ptr )
{
	const byte *p = (const byte *)ptr;

	return ptr && p >= gc_studio_bss && p < gc_studio_bss + GC_STUDIO_MODEL_LIMIT;
}

/*
 * Studio overflow into the map's leftover memory.
 *
 * The fixed studio arena filled on monster-heavy maps (c2a4: houndeye, hgrunt,
 * bullsquid, islave skipped, so they spawned without hitboxes or animation)
 * while about 1 MiB of the map-load arena was never carved.  Once the client
 * is in game every map-load-time carve (world arrays, entity tables, packet
 * ring) has been made, so what remains is this map's leftover; studio slices
 * are carved from it on demand.  They live exactly as long as the map: the
 * pre-spawn release forgets them before the old world's arena is discarded.
 * In-place grow/trim/rollback stay BSS-only (the overflow is a plain carve).
 */
#define GC_STUDIO_OVERFLOW_MAX 64
static struct { byte *base; size_t size; } gc_studio_overflow[GC_STUDIO_OVERFLOW_MAX];
static int gc_studio_overflow_count;
static size_t gc_studio_overflow_bytes;

static qboolean Mod_GCInStudioOverflow( const void *ptr )
{
	const byte *p = (const byte *)ptr;
	int i;

	for( i = 0; ptr && i < gc_studio_overflow_count; i++ )
		if( p >= gc_studio_overflow[i].base && p < gc_studio_overflow[i].base + gc_studio_overflow[i].size )
			return true;
	return false;
}

static byte *Mod_GCStudioOverflowAlloc( size_t size )
{
	size_t at;
	byte *slot;

	extern void Mod_GCWorldCanaryCheck( const char *when );

	/* Only between "map active" and the next load: during a changelevel the
	 * client is still in game while the server loads the next map into this
	 * same arena, and a slice carved then lands in the new map's data. */
	if( gc_studio_overflow_count >= GC_STUDIO_OVERFLOW_MAX || !GC_MapLoadBufferIsWorldSettled( ))
		return NULL;
	Mod_GCWorldCanaryCheck( "before studio overflow slice" ); // covers earlier slices' writes
	size = ( size + 31u ) & ~(size_t)31u; // keep the next carve contiguous for GrowLast
	/* Free gaps only: a lean load keeps world arrays above the tail too
	 * (leafs/nodes high slot, clipnode spill); carving at the highwater
	 * landed on c2a4's clipnodes (DSI in PM_HullPointContents). */
	at = GC_MapArenaFind( size );
	if( at == (size_t)-1 )
		return NULL;
	slot = (byte *)GC_MapLoadBufferTailAlloc( at, size );
	if( !slot )
		return NULL;
	gc_studio_overflow[gc_studio_overflow_count].base = slot;
	gc_studio_overflow[gc_studio_overflow_count].size = size;
	gc_studio_overflow_count++;
	gc_studio_overflow_bytes += size;
	Con_Reportf( "Xash3D GameCube: studio overflow slice %s in map leftover (total %s, %d slices)\n",
		Q_memprint( size ), Q_memprint( gc_studio_overflow_bytes ), gc_studio_overflow_count );
	return slot;
}

/* Grow the newest overflow slice in place when nothing was carved after it
 * (the T.mdl texture-header merge after a mesh read). */
static byte *Mod_GCStudioOverflowGrowLast( void *ptr, size_t new_size )
{
	const int last = gc_studio_overflow_count - 1;
	byte *end, *more;
	size_t extra;

	if( last < 0 || ptr != gc_studio_overflow[last].base )
		return NULL;
	if( new_size <= gc_studio_overflow[last].size )
		return (byte *)ptr;
	end = gc_studio_overflow[last].base + gc_studio_overflow[last].size;
	if( GC_MapLoadBufferTailEnd() != end || GC_MapLoadBufferTailPeak() > GC_MapLoadBufferTailHighwater( ))
		return NULL; // something was carved after this slice
	extra = ( new_size - gc_studio_overflow[last].size + 31u ) & ~(size_t)31u;
	{
		extern size_t Mod_GCArenaGap( size_t from, size_t *gap_end );
		const size_t off = (size_t)( end - GC_MapLoadBufferBase( ));
		size_t gap_end = 0;

		/* The extension must be free arena too, not a world array above. */
		if( Mod_GCArenaGap( off, &gap_end ) != off || off + extra > gap_end )
			return NULL;
	}
	more = (byte *)GC_MapLoadBufferTailAlloc( GC_MapLoadBufferTailHighwater(), extra );
	if( more != end )
		return NULL; // cannot happen when the tail ended at the slice
	gc_studio_overflow[last].size += extra;
	gc_studio_overflow_bytes += extra;
	return (byte *)ptr;
}

/* Model-owned studio memory (BSS array or a map-leftover slice): never free(). */
qboolean Mod_GCIsStudioBssCache( const void *ptr )
{
	return Mod_GCInStudioBssArray( ptr ) || Mod_GCInStudioOverflow( ptr );
}

/* Studio skins from the top of the shared region (see GC_STUDIO_BSS_BYTES).
 * Freed as a group: the window rewinds when the last skin goes. */
byte *Mod_GCStudioSkinAlloc( size_t bytes )
{
	const size_t need = ( bytes + 31u ) & ~(size_t)31u;
	const size_t floor = ( gc_studio_bss_bump + 31u ) & ~(size_t)31u;

	if( bytes == 0 || floor > GC_STUDIO_BSS_BYTES
		|| need > GC_STUDIO_BSS_BYTES - floor - gc_studio_skin_top )
		return NULL;
	gc_studio_skin_top += need;
	gc_studio_skin_live++;
	return gc_studio_bss + GC_STUDIO_BSS_BYTES - gc_studio_skin_top;
}

void Mod_GCStudioSkinFree( void )
{
	if( gc_studio_skin_live > 0 )
		gc_studio_skin_live--;
	if( gc_studio_skin_live == 0 )
		gc_studio_skin_top = 0;
}

qboolean Mod_GCIsStudioSkin( const void *ptr )
{
	const byte *p = (const byte *)ptr;

	return gc_studio_skin_top && p >= gc_studio_bss + GC_STUDIO_BSS_BYTES - gc_studio_skin_top
		&& p < gc_studio_bss + GC_STUDIO_BSS_BYTES;
}

size_t Mod_GCStudioSkinUsed( void )
{
	return gc_studio_skin_top;
}

size_t Mod_GCStudioSkinRemaining( void )
{
	const size_t floor = ( gc_studio_bss_bump + 31u ) & ~(size_t)31u;

	if( floor + gc_studio_skin_top >= GC_STUDIO_BSS_BYTES )
		return 0;
	return GC_STUDIO_BSS_BYTES - floor - gc_studio_skin_top;
}

byte *Mod_GCStudioBssAlloc( size_t size )
{
	size_t align_off;

	if( size == 0 )
		return NULL;
	align_off = ( gc_studio_bss_bump + 31u ) & ~31u;
	if( size > GC_STUDIO_MODEL_LIMIT || align_off + size > GC_STUDIO_MODEL_LIMIT )
		return Mod_GCStudioOverflowAlloc( size );
	gc_studio_bss_bump = align_off + size;
	gc_studio_bss_owners++;
	if( Sys_CheckParm( "-gcmodeltrace" ))
		Con_Reportf( "modeltrace alloc %p size=%u bump=%u\n", gc_studio_bss + align_off, (unsigned)size, (unsigned)gc_studio_bss_bump );
	return gc_studio_bss + align_off;
}

/* End of a slice grown in place into a staged companion's room (GrowLast);
 * the unstage rollback must not drop the bump below it. */
static size_t gc_studio_bss_grow_end;

/* Roll back an unsuccessful read only while its slice is still last. */
static void Mod_GCStudioBssRollbackLast( byte *slot, size_t size, size_t previous_bump )
{
	size_t offset;

	if( !Mod_GCInStudioBssArray( slot ) || gc_studio_bss_owners <= 0 )
		return;
	offset = (size_t)( slot - gc_studio_bss );
	if( size > GC_STUDIO_BSS_BYTES - offset || previous_bump > offset )
		return;
	if( gc_studio_bss_bump == offset + size )
	{
		/* A *T.mdl staged above a mesh is where that mesh's merged texture
		 * headers and skin table were grown to: keep them reserved, or the
		 * next model loads over them (garbage skinref -> white NPC). */
		if( gc_studio_bss_grow_end > previous_bump && gc_studio_bss_grow_end <= offset + size )
			gc_studio_bss_bump = gc_studio_bss_grow_end;
		else
		{
			gc_studio_bss_bump = previous_bump;
			gc_studio_bss_owners--;
		}
	}
	gc_studio_bss_grow_end = 0;
}

/* Stage a transient file (a *T.mdl skin companion) at the bottom of the
 * shared studio region; unstage rewinds it if nothing was placed after it. */
byte *Mod_GCStudioArenaStage( size_t size, size_t *previous_bump )
{
	*previous_bump = gc_studio_bss_bump;
	return Mod_GCStudioBssAlloc( size );
}

void Mod_GCStudioArenaUnstage( byte *slot, size_t size, size_t previous_bump )
{
	Mod_GCStudioBssRollbackLast( slot, size, previous_bump );
}

/*
 * G381: grow the last BSS studio slice in place (mesh → mesh+T.mdl headers).
 * Tip CapFaces leaves heap empty; scientist mesh already occupies the bump so
 * a second Mod_GCStudioBssAlloc(merged) fails and we used to keep mesh-only
 * (white NPC / HUD sprite bind).
 */
byte *Mod_GCStudioBssGrowLast( void *ptr, size_t old_size, size_t new_size )
{
	byte *p = (byte *)ptr;
	size_t off;
	size_t old_end;

	if( !ptr || old_size == 0 || new_size < old_size || new_size > GC_STUDIO_MODEL_LIMIT )
		return NULL;
	if( !Mod_GCInStudioBssArray( ptr ))
		return Mod_GCInStudioOverflow( ptr ) ? Mod_GCStudioOverflowGrowLast( ptr, new_size ) : NULL;
	off = (size_t)( p - gc_studio_bss );
	old_end = off + old_size;
	if( off + new_size > GC_STUDIO_MODEL_LIMIT )
		return NULL;
	/* Deferred T.mdl headers: lean load reserved a pad so bump > old_end and
	 * later companions (scientist) sit above that pad. Fit in-place. */
	if( off + new_size <= gc_studio_bss_bump )
	{
		if( off + new_size > old_end )
			gc_studio_bss_grow_end = off + new_size;
		return p;
	}
	/* Accept either exact bump or 32-byte padded bump from Alloc. */
	if( gc_studio_bss_bump != old_end
		&& gc_studio_bss_bump != (( old_end + 31u ) & ~31u ))
		return NULL;
	gc_studio_bss_bump = off + new_size;
	return p;
}

/* After embedded soft-skin upload, only the mesh+texhdr slice stays live.
 * Shrink the bump so Xen companions (islave ~145 KiB) still fit — c4a3 left
 * nihilanth's 420 KiB read pinned and starved the arena (probe 030354).
 * Soft-skin pixels must live in the studio skin overflow (r_image.c), not
 * this bump — TrimLast would otherwise clobber them. */
void Mod_GCStudioBssTrimLast( void *ptr, size_t kept )
{
	byte *p = (byte *)ptr;
	size_t off, new_bump;

	if( !ptr || kept == 0 || !Mod_GCInStudioBssArray( ptr ))
		return;
	off = (size_t)( p - gc_studio_bss );
	if( off >= gc_studio_bss_bump || off + kept > gc_studio_bss_bump )
		return;
	new_bump = ( off + kept + 31u ) & ~31u;
	if( new_bump >= gc_studio_bss_bump )
		return;
	Con_Reportf( "Xash3D GameCube: studio BSS trim kept=%s bump %s -> %s\n",
		Q_memprint( kept ), Q_memprint( gc_studio_bss_bump ), Q_memprint( new_bump ));
	gc_studio_bss_bump = new_bump;
}

static qboolean Mod_GCIsPinnedViewModel( const model_t *mod )
{
	int i;

	if( !mod )
		return false;
	for( i = 0; i < gc_pinned_viewmodel_count; i++ )
	{
		if( gc_pinned_viewmodels[i] == mod )
			return true;
	}
	return false;
}

static void Mod_GCPinViewModel( model_t *mod )
{
	int i;

	if( !mod || Mod_GCIsPinnedViewModel( mod ))
		return;
	if( gc_pinned_viewmodel_count >= GC_REAL_STUDIO_MAX_VIEW )
		return;
	for( i = 0; i < gc_pinned_viewmodel_count; i++ )
	{
		if( gc_pinned_viewmodels[i] == mod )
			return;
	}
	gc_pinned_viewmodels[gc_pinned_viewmodel_count++] = mod;
	Con_Reportf( "Xash3D GameCube: G156 pinned viewmodel %s\n",
		mod->name[0] ? mod->name : "?" );
	/* G506 presentation gate also accepts this as G105 on non-tram maps
	 * where deferred crowbar promote may not run. */
	if( Mod_GCIsNewGameRoute() && mod->name[0]
		&& ( Q_stristr( mod->name, "v_crowbar" )
			|| Q_stristr( mod->name, "v_9mmhandgun" )))
	{
		static qboolean g105_from_pin_logged;

		if( !g105_from_pin_logged )
		{
			g105_from_pin_logged = true;
			Con_Reportf( "Xash3D GameCube: G105 landmark viewmodel ready %s\n",
				mod->name );
			GCube_AppendSwissEvidence( "G105 landmark viewmodel ready %s", mod->name );
		}
	}
}

static qboolean Mod_GCStudioAlreadyResident( model_t *mod )
{
	studiohdr_t *hdr;

	if( !mod || !mod->cache.data )
		return false;
	hdr = (studiohdr_t *)mod->cache.data;
	return ( hdr->numbodyparts > 0 ) ? true : false;
}

static qboolean Mod_GCMapNeedsStudio( const char *path );

static qboolean Mod_GCStudioNameAllowed( const char *name, qboolean *is_viewmodel )
{
	char bare[64];
	static const char *npcs[] = {
		"scientist", "barney", "gman", "player", "roach",
		"headcrab", "houndeye", "zombie",
		"scientist01", "scientist02", "scientist03",
		/* Menu New Game intro props + monster_furniture (c1a1f uses filecabinet). */
		"forklift", "loader", "filecabinet",
		/* Campaign cast — lean mesh + *t skin atlas stays under 1 MiB BSS. */
		"bullsquid", "agrunt", "islave", "hgrunt", "tentacle2", "garg",
		"apache", "osprey", "ichthyosaur", "leech", "barnacle",
		/* Xen's final encounters require these studio headers for boss AI,
		 * animation and visible alien controllers. */
		"nihilanth", "big_mom", "baby_headcrab", "controller",
		/* Tiny lean world-weapon stub for Flipper studio draw admit. */
		"w_crowbar",
		/* Thrown grenade bodies: tiny lean meshes, no skin atlas. */
		"w_grenade", "w_stick", "w_bazooka_rocket",
		/* CapFaces DoD map props (env_model / dod_object) — lean gc_studio. */
		"hk_satchel", "horse_statue", "by_rubble_set", "sandbags",
		"by_canopy", "arc_xer_tree2", "furniture", "bulb",
		/* dod_avalanche env_models — full-fidelity props (ava-prop-1). */
		"sherman", "jagd", "by_banister", "flags",
		/* DoD team bodies, split into a mesh core plus an animation pack
		 * (dod-gc tools/split_studio_mdl.py --pack). */
		"axis-inf", "axis-para", "brit-inf", "us-inf", "us-para",
		/* Lost Coast characters baked to studio v10 (models/lc/, monster_generic). */
		"fisherman", "combine_soldier", "combine_super_soldier", "seagull",
		NULL
	};
	static const char *views[] = {
		"v_crowbar", "v_9mmhandgun", "v_9mmar", "v_shotgun", "v_357",
		/* DoD CapFaces listen — team loadout viewmodels (lean-injected). */
		"v_garand", "v_mp40", "v_98k", "v_tommy",
		"v_colt", "v_luger", "v_amerk", "v_spade",
		/* Mk2 grenade and stick grenade (CHandGrenade viewmodels). */
		"v_grenade", "v_stick",
		/* The rest of the table-driven DoD gun set (dod_weapon.cpp). */
		"v_bren", "v_mp44", "v_spring", "v_scoped98k", "v_30cal", "v_mg42",
		"v_bazooka", "v_panzerschreck",
		/* BAR, British and para loadouts. */
		"v_bar", "v_enfield", "v_enfield_scoped", "v_sten", "v_piat", "v_webley",
		"v_fairbairn", "v_mills", "v_m1carbine", "v_greasegun", "v_fg42", "v_paraknife",
		NULL
	};
	int i;

	if( is_viewmodel )
		*is_viewmodel = false;
	if( !name || !name[0] )
		return false;
	if( Mod_GCIsWeaponModelPath( name ))
		return true;

	COM_FileBase( name, bare, sizeof( bare ));
	for( i = 0; npcs[i]; i++ )
	{
		if( !Q_stricmp( bare, npcs[i] ))
			return true;
	}
	for( i = 0; views[i]; i++ )
	{
		if( !Q_stricmp( bare, views[i] ))
		{
			if( is_viewmodel )
				*is_viewmodel = true;
			return true;
		}
	}
	return false;
}

/* Lean v_30cal (~186 KiB) and v_mg42 (~268 KiB) are bigger than a viewmodel
 * slot and than the whole DoD studio model share, so they stream into the
 * world-array spill like the team bodies. Once loaded they stay resident
 * until the map arena resets, so a switch back costs nothing. */
static qboolean Mod_GCViewModelNeedsSpill( const char *path, size_t size )
{
	return Mod_GCIsViewModelPath( path ) && size > GC_VM_SLOT_SIZE;
}

static qboolean Mod_GCAllowRealStudioLoad( const char *name, size_t filesize )
{
	qboolean is_view = false;

	/* Retail Flipper + New Game probe — allowlisted meshes only. */
	if( !Mod_GCStudioNameAllowed( name, &is_view ))
		return false;
	/* Menu New Game keeps the whole intro cast resident in the 1 MiB studio
	 * BSS arena (scientist + barney + forklift + view meshes ~0.6 MiB). */
	const int max_npc = GC_MenuRunsServer() ? GC_REAL_STUDIO_MAX_NPC_MENU : GC_REAL_STUDIO_MAX_NPC;
	const size_t max_bytes = GC_MenuRunsServer() ? GC_REAL_STUDIO_MAX_BYTES_MENU : GC_REAL_STUDIO_MAX_BYTES;

	if( is_view )
	{
		/* A viewmodel that fits a viewmodel slot replaces the slot's least
		 * recently used owner (Mod_GCVMSlotEvict gives back its count and
		 * bytes), so the slots are its reserved budget. Without this the
		 * team rifle never loaded once props and bodies had spent the byte
		 * budget (ceres b302741eb: "deferred studio budget skip
		 * 'models/v_garand.mdl' (113.42 Kb)" every frame, vm unbound). */
		if( Mod_GCIsViewModelPath( name ) && filesize >= sizeof( studiohdr_t )
			&& filesize <= GC_VM_SLOT_SIZE )
			return true;
		if( gc_real_studio_view >= GC_REAL_STUDIO_MAX_VIEW )
			return false;
		/* The spill has its own limit (the allocation fails cleanly), so
		 * the shared byte budget, spent on props and bodies, does not
		 * apply (ceres PR 98: v_30cal/v_mg42 budget skip). */
		if( Mod_GCViewModelNeedsSpill( name, filesize ) && filesize <= ( 448 * 1024 ))
			return true;
	}
	else if( gc_real_studio_npc >= max_npc && !Mod_GCIsWeaponModelPath( name ))
		return false;
	/* The internally textured Nihilanth is approximately 420 KiB. */
	if( filesize < sizeof( studiohdr_t ) || filesize > ( 448 * 1024 ))
		return false;
	if( gc_real_studio_bytes + filesize > max_bytes )
		return false;
	return true;
}

static void Mod_GCNoteRealStudioLoaded( const char *name, size_t filesize )
{
	qboolean is_view = false;

	Mod_GCStudioNameAllowed( name, &is_view );
	if( is_view )
		gc_real_studio_view++;
	else if( !Mod_GCIsWeaponModelPath( name ))	/* small, own allowlist */
		gc_real_studio_npc++;
	gc_real_studio_bytes += filesize;
	Con_Reportf( "Xash3D GameCube: real studio loaded '%s' (%s) npc=%d view=%d budget=%s/%s\n",
		name, Q_memprint( filesize ), gc_real_studio_npc, gc_real_studio_view,
		Q_memprint( gc_real_studio_bytes ), Q_memprint( GC_REAL_STUDIO_MAX_BYTES ));
}

/*
=============
Mod_GCLoadStudioFile

Read an MDL via FS (prefer tiny gc_studio/ mirror — retail models/ is too large
for reliable ISO9660 lookup after map prep).

G287/G289: stream into BSS bump arena (crowbar+handgun lean meshes) when heap
cannot serve tens of KiB after map prep.
=============
*/
static byte *Mod_GCLoadStudioFile( const char *model_path, fs_offset_t *length, qboolean *from_bss )
{
	char bare[64];
	char mirror[MAX_QPATH];
	byte *buf;
	byte *slot;
	file_t *f;
	fs_offset_t flen, total, got;
	size_t previous_bump;

	*length = 0;
	if( from_bss )
		*from_bss = false;
	COM_FileBase( model_path, bare, sizeof( bare ));
	Q_snprintf( mirror, sizeof( mirror ), "gc_studio/%s.mdl", bare );

	/* Day of Defeat ships split team bodies (gc_studio/us-inf.mdl etc.) and
	 * the renderer draws those from each player's "model" userinfo. The HL1
	 * player.mdl (about 275 KiB) would only be a fallback body, and holding it
	 * in the world-array spill left no room for the team body a bot needs
	 * (ceres 877bb27: axis-inf spill alloc miss, then heap alloc fail). The
	 * server uses a hull-only stub for player.mdl, so skip it here. */
	if( !Q_stricmp( bare, "player" ) && FS_FileExists( "gc_studio/us-inf.mdl", false ))
	{
		static qboolean logged;

		if( !logged )
		{
			logged = true;
			Con_Reportf( "Xash3D GameCube: skip HL1 player body '%s', DoD team bodies present (spill free %s)\n",
				model_path, Q_memprint( Mod_GCWorldArraySpillFree()));
		}
		return NULL;
	}

	/* A model read once this boot stays in ARAM (raw file bytes, before the
	 * in-place byte swap), so an arena reset, a team pick or a changelevel
	 * reloads it by DMA instead of from disc. */
	{
		unsigned int cached = 0;

		if( GC_AssetCacheLookup( mirror, &cached ) && cached >= sizeof( studiohdr_t )
			&& cached <= GC_STUDIO_MODEL_LIMIT )
		{
			if( !Mod_GCAllowRealStudioLoad( model_path, (size_t)cached ))
			{
				Con_Reportf( "Xash3D GameCube: deferred studio budget skip '%s' (%s)\n",
					model_path, Q_memprint( (size_t)cached ));
				return NULL;
			}
			previous_bump = gc_studio_bss_bump;
			/* Map props reloaded from ARAM go back to the world-array spill,
			 * like the disc read below. In the studio BSS, the flags merge
			 * grew into the room the team viewmodel's skins need, and the
			 * rifle drew with a rainbow skin (ceres rfL, DoD charlie). */
			if(( Mod_GCStudioPrefersSpill( model_path )
				|| Mod_GCViewModelNeedsSpill( model_path, (size_t)cached ))
				&& Mod_GCMapModelSpillAfterWorldDL())
			{
				extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
				extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );

				slot = (byte *)Mod_GCWorldArraySpillTryAlloc( (size_t)cached, "mapmodel-studio" );
				if( slot && GC_AssetCacheRead( mirror, slot, cached ))
				{
					Mod_GCWorldArraySpillPinBase( slot, (size_t)cached );
					*length = (fs_offset_t)cached;
					if( from_bss )
						*from_bss = true;
					Con_Reportf( "Xash3D GameCube: mapmodel spill from ARAM '%s' (%s)\n",
						model_path, Q_memprint( (size_t)cached ));
					return slot;
				}
				if( slot )
					Mod_GCWorldArraySpillRewind( slot, (size_t)cached );
				Con_Reportf( "Xash3D GameCube: mapmodel spill from ARAM miss '%s' (%s), studio arena\n",
					model_path, Q_memprint( (size_t)cached ));
				slot = NULL;
			}
			slot = Mod_GCIsViewModelPath( model_path ) ? Mod_GCVMSlotAlloc( (size_t)cached ) : NULL;
			/* Oversized viewmodels belong in the spill (disc path below),
			 * not in the studio arena the skins share. */
			if( !slot && !Mod_GCViewModelNeedsSpill( model_path, (size_t)cached ))
				slot = Mod_GCStudioBssAlloc( (size_t)cached );
			if( slot && GC_AssetCacheRead( mirror, slot, cached ))
			{
				*length = (fs_offset_t)cached;
				if( from_bss )
					*from_bss = true;
				Con_Reportf( "Xash3D GameCube: studio '%s' from ARAM (%s) bump=%s\n",
					model_path, Q_memprint( (size_t)cached ), Q_memprint( gc_studio_bss_bump ));
				return slot;
			}
			if( slot )
				Mod_GCStudioBssRollbackLast( slot, (size_t)cached, previous_bump );
		}
	}

	/* Every studio file takes the read-into-slot path below (studio arena,
	 * viewmodel slot or map spill): the gc_studio/ mirror when the disc has
	 * one, otherwise models/<name>.mdl itself.  Only mirrored models used to,
	 * so barnacle/bullsquid/hgrunt/... went through a whole-file heap malloc
	 * that fragmented MEM1 refused thousands of times per sweep and the
	 * monsters spawned without their model.  The ARAM cache stays keyed by
	 * the mirror name either way. */
	const char *src = FS_FileExists( mirror, false ) ? mirror
		: ( FS_FileExists( model_path, false ) ? model_path : NULL );

	if( src )
	{
		f = FS_Open( src, "rb", false );
		if( f )
		{
			flen = FS_FileLength( f );
			/* Check limits before reserving a persistent arena slice. A
			 * budget rejection after the read cannot free that slice once
			 * other models have been allocated. */
			if( flen >= (fs_offset_t)sizeof( studiohdr_t ) &&
				!Mod_GCAllowRealStudioLoad( model_path, (size_t)flen ))
			{
				FS_Close( f );
				Con_Reportf( "Xash3D GameCube: deferred studio budget skip '%s' (%s)\n",
					model_path, Q_memprint( (size_t)flen ));
				return NULL;
			}
			slot = NULL;
			previous_bump = gc_studio_bss_bump;
			/* Mapmodels prefer world-array spill so the 184 KiB studio BSS
			 * stays free for the team viewmodel (ava-prop-2 tip≈6 KiB).
			 * Only after WorldDL owns its chunk-cache slice. */
			if(( Mod_GCStudioPrefersSpill( model_path )
				|| Mod_GCViewModelNeedsSpill( model_path, (size_t)flen ))
				&& flen >= (fs_offset_t)sizeof( studiohdr_t )
				&& Mod_GCMapModelSpillAfterWorldDL())
			{
				extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
				extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );

				slot = (byte *)Mod_GCWorldArraySpillTryAlloc( (size_t)flen, "mapmodel-studio" );
				if( slot )
				{
					total = 0;
					while( total < flen )
					{
						got = FS_Read( f, slot + total, flen - total );
						if( got <= 0 )
							break;
						total += got;
					}
					FS_Close( f );
					if( total == flen )
					{
						Mod_GCWorldArraySpillPinBase( slot, (size_t)flen );
						/* raw file bytes, before the byte swap: reloads become DMA */
						GC_AssetCacheStore( mirror, slot, (unsigned int)flen );
						*length = flen;
						if( from_bss )
							*from_bss = true;
						Con_Reportf( "Xash3D GameCube: mapmodel spill read '%s' via %s (%s)\n",
							model_path, src, Q_memprint( (size_t)flen ));
						return slot;
					}
					Con_Reportf( "Xash3D GameCube: mapmodel spill short '%s' read=%li want=%li\n",
						src, (long)total, (long)flen );
					slot = NULL;
				}
			}
			if( !slot && flen >= (fs_offset_t)sizeof( studiohdr_t )
				&& flen <= (fs_offset_t)GC_STUDIO_MODEL_LIMIT )
			{
				if( Mod_GCIsViewModelPath( model_path ))
					slot = Mod_GCVMSlotAlloc( (size_t)flen );
				if( !slot && !Mod_GCViewModelNeedsSpill( model_path, (size_t)flen ))
					slot = Mod_GCStudioBssAlloc( (size_t)flen );
			}
			if( slot )
			{
				total = 0;
				while( total < flen )
				{
					got = FS_Read( f, slot + total, flen - total );
					if( got <= 0 )
						break;
					total += got;
				}
				FS_Close( f );
				if( total == flen )
				{
					GC_AssetCacheStore( mirror, slot, (unsigned int)flen );
					*length = flen;
					if( from_bss )
						*from_bss = true;
					Con_Reportf( "Xash3D GameCube: G289 studio BSS read '%s' via %s (%s) bump=%s\n",
						model_path, src, Q_memprint( (size_t)flen ),
						Q_memprint( gc_studio_bss_bump ));
					return slot;
				}
				Con_Reportf( "Xash3D GameCube: G289 studio BSS short '%s' read=%li want=%li\n",
					src, (long)total, (long)flen );
				/* Only release the last allocation: another owner may have
				 * acquired a later slice while the file was being read. */
				Mod_GCStudioBssRollbackLast( slot, (size_t)flen, previous_bump );
			}
			else
			{
				FS_Close( f );
				if( flen > (fs_offset_t)GC_STUDIO_MODEL_LIMIT )
					Con_Reportf( "Xash3D GameCube: G289 studio too large for BSS '%s' (%s)\n",
						src, Q_memprint( (size_t)flen ));
				/* Map props: WorldDL listen leaves spill headroom (chunk cache
				 * capped at 192 KiB). Stream lean mesh there — tip heap is dead.
				 * Gate on WorldDL Active so props cannot starve the cache. */
				/* The lean HL1 player body (about 281 KiB) is over the BSS limit too and
				 * has no heap fallback once client start has spent it, so it takes the
				 * spill the same way, as do the split team bodies. */
				if(( Mod_GCStudioPrefersSpill( model_path ) || !Q_stricmp( bare, "player" )
					|| Mod_GCIsWeaponModelPath( model_path ))
					&& flen >= (fs_offset_t)sizeof( studiohdr_t )
					&& Mod_GCAllowRealStudioLoad( model_path, (size_t)flen )
					&& Mod_GCMapModelSpillAfterWorldDL())
				{
					extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
					extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );

					f = FS_Open( src, "rb", false );
					if( !f )
						f = FS_Open( src, "rb", true );
					if( f )
					{
						slot = (byte *)Mod_GCWorldArraySpillTryAlloc( (size_t)flen, "mapmodel-studio" );
						if( slot )
						{
							total = 0;
							while( total < flen )
							{
								got = FS_Read( f, slot + total, flen - total );
								if( got <= 0 )
									break;
								total += got;
							}
							FS_Close( f );
							if( total == flen )
							{
								Mod_GCWorldArraySpillPinBase( slot, (size_t)flen );
						/* raw file bytes, before the byte swap: reloads become DMA */
						GC_AssetCacheStore( mirror, slot, (unsigned int)flen );
								*length = flen;
								if( from_bss )
									*from_bss = true; /* don't free — spill-backed */
								Con_Reportf( "Xash3D GameCube: mapmodel spill read '%s' via %s (%s)\n",
									model_path, src, Q_memprint( (size_t)flen ));
								return slot;
							}
							Con_Reportf( "Xash3D GameCube: mapmodel spill short '%s' read=%li want=%li\n",
								src, (long)total, (long)flen );
						}
						else
						{
							FS_Close( f );
							Con_Reportf( "Xash3D GameCube: mapmodel spill alloc miss '%s' (%s, spill free %s)\n",
								src, Q_memprint( (size_t)flen ), Q_memprint( Mod_GCWorldArraySpillFree()));
						}
					}
				}
			}
		}
	}

	/* G381: CapFaces/pad tex decode can leave scratch holes that block the
	 * 170 KiB scientist zip malloc (mirror miss buf=0). Purge before heap read. */
	Image_GCPurgeDecodeScratch();
	buf = FS_LoadFileMalloc( mirror, length, false );
	if( buf && *length >= (fs_offset_t)sizeof( studiohdr_t ))
	{
		GC_AssetCacheStore( mirror, buf, (unsigned int)*length );
		Con_Reportf( "Xash3D GameCube: deferred studio read '%s' via %s (%s)\n",
			model_path, mirror, Q_memprint( (size_t)*length ));
		return buf;
	}
	Con_Reportf( "Xash3D GameCube: deferred studio mirror miss '%s' buf=%p len=%li exists=%d\n",
		mirror, buf, (long)*length, FS_FileExists( mirror, false ) ? 1 : 0 );
	if( buf )
	{
		free( buf );
		buf = NULL;
	}
	*length = 0;

	buf = FS_LoadFileMalloc( model_path, length, false );
	if( buf && *length >= (fs_offset_t)sizeof( studiohdr_t ))
	{
		Con_Reportf( "Xash3D GameCube: deferred studio read '%s' via models/ (%s)\n",
			model_path, Q_memprint( (size_t)*length ));
		return buf;
	}
	if( buf )
	{
		free( buf );
		buf = NULL;
	}
	*length = 0;
	return NULL;
}

const char *Mod_GCLandmarkViewModelPath( void )
{
	return gc_landmark_viewmodel[0] ? gc_landmark_viewmodel : NULL;
}

qboolean Mod_GCPromoteStudioPath( const char *path )
{
	model_t *mod;
	byte *buf;
	fs_offset_t length = 0;
	qboolean loaded = false;
	qboolean is_view = false;
	qboolean from_bss = false;

	if( !path || !path[0] )
		return false;

	/* Reject before any FS/disc I/O — tip MEM1 cannot absorb random w_*.mdl. */
	if( !Mod_GCStudioNameAllowed( path, &is_view ))
		return false;

	/* G156: reuse resident mesh — never re-read 130KB from disc under MEM1. */
	mod = Mod_FindName( path, false );
	if( !mod )
		return false;
	if( Mod_GCStudioAlreadyResident( mod ))
	{
		if( is_view )
			Mod_GCPinViewModel( mod );
		return true;
	}
	else if( Mod_GCIsPinnedViewModel( mod ))
	{
		/* Already pinned - nothing to do */
		return true;
	}

	buf = Mod_GCLoadStudioFile( path, &length, &from_bss );
	if( !buf || length < (fs_offset_t)sizeof( studiohdr_t ))
	{
		Con_Reportf( "Xash3D GameCube: deferred studio skip '%s' read=%li\n",
			path, (long)length );
		if( buf && !from_bss )
			free( buf );
		return false;
	}

	if( !Mod_GCAllowRealStudioLoad( path, (size_t)length ))
	{
		Con_Reportf( "Xash3D GameCube: deferred studio budget skip '%s' (%s)\n",
			path, Q_memprint( (size_t)length ));
		if( !from_bss )
			free( buf );
		return false;
	}

	mod = Mod_FindName( path, true );
	if( !mod )
	{
		Con_Reportf( "Xash3D GameCube: deferred studio skip '%s' (not registered)\n", path );
		if( !from_bss )
			free( buf );
		return false;
	}

	if( Mod_GCStudioAlreadyResident( mod ))
	{
		if( !from_bss )
			free( buf );
		Mod_GCStudioNameAllowed( path, &is_view );
		if( is_view )
		{
			Mod_GCVMSlotTouch( mod );
			Mod_GCPinViewModel( mod );
		}
		return true;
	}

	/* G287/G289: ensure we have a valid cache buffer before proceeding */
	if( !buf )
	{
		Con_Reportf( "Xash3D GameCube: deferred studio skip '%s' null buffer\n", path );
		return false;
	}

	mod->cache.data = NULL;
	mod->mempool = 0;
	mod->needload = NL_PRESENT;
	mod->type = mod_studio;

	Image_GCPurgeDecodeScratch();
	Mod_LoadStudioModel( mod, buf, (size_t)length, &loaded );
	/* BSS bump slot is never free()'d — cache may point at it or a calloc copy. */
	if( !from_bss )
		free( buf );
	Image_GCPurgeDecodeScratch();

	if( loaded )
	{
		size_t kept = length;
		studiohdr_t *loaded_hdr = (studiohdr_t *)mod->cache.data;

		if( loaded_hdr && loaded_hdr->length > 0 && (size_t)loaded_hdr->length < kept )
			kept = (size_t)loaded_hdr->length;
		Mod_GCNoteRealStudioLoaded( path, kept );
		{
			/* Flipper-skinned characters (ref/gx/r_gx_actor.c): their
			 * display lists load beside the model, before the prop
			 * cache claims the rest of the studio arena. */
			extern void R_GXActorPreloadModel( model_t *mod );

			R_GXActorPreloadModel( mod );
		}
		Mod_GCStudioNameAllowed( path, &is_view );
		if( is_view )
		{
			Mod_GCVMSlotClaim( mod, buf, kept );
			Mod_GCPinViewModel( mod );
		}
		R_GXStudioInvalidateBindings();
		return true;
	}

	/* Failed promote with spill buffer — rewind so the next env_model can retry. */
	if( from_bss && Mod_GCIsWorldArraySpillCache( buf ))
		Mod_GCWorldArraySpillRewind( buf, (size_t)length );
	Con_Reportf( S_WARN "Xash3D GameCube: deferred studio promote failed '%s'\n", path );
	return false;
}

qboolean Mod_GCEnsureLandmarkViewModel( const char *model_path )
{
	qboolean ok;
	model_t *mod;

	/* Landmark Deploy on retail Flipper too — not only -gcnewgame probes. */
	if( !model_path || !model_path[0] )
		return false;

	/* Fast path: already resident + pinned. */
	mod = Mod_FindName( model_path, false );
	if( Mod_GCStudioAlreadyResident( mod ))
	{
		Mod_GCVMSlotTouch( mod );
		Mod_GCPinViewModel( mod );
		Q_strncpy( gc_landmark_viewmodel, model_path, sizeof( gc_landmark_viewmodel ));
		/* Promoted earlier (precache, or the spill route for the split
		 * v_30cal / v_mg42 cores) without skins: the map-load skin pass skips
		 * weapons nobody holds, so attach them once one is held. Only the
		 * held weapon gets skins: attaching every promoted viewmodel at load
		 * ran the skin uploads out of memory on ceres (v_scoped98k, v_grenade,
		 * v_stick) and left no room for the animation ring. */
		if( Mod_GCIsViewModelPath( model_path ))
		{
			extern qboolean Mod_GCAttachStudioSkins( model_t *m );
			static model_t *skinned_last, *failed_last;
			static double failed_time;

			/* Attach returns at once when the skins are already bound; a
			 * failed attach (OOM) is retried once a second, not per frame. */
			if( mod != failed_last || host.realtime - failed_time >= 1.0 )
			{
				if( Mod_GCAttachStudioSkins( mod ))
				{
					if( skinned_last != mod )
					{
						skinned_last = mod;
						Con_Reportf( "Xash3D GameCube: held viewmodel skins '%s' ok=1 (resident)\n", model_path );
						R_GXStudioInvalidateBindings();
					}
					if( failed_last == mod )
						failed_last = NULL;
				}
				else
				{
					if( failed_last != mod )
						Con_Reportf( "Xash3D GameCube: held viewmodel skins '%s' ok=0 (resident)\n", model_path );
					failed_last = mod;
					failed_time = host.realtime;
				}
			}
		}
		return true;
	}

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();
	ok = Mod_GCPromoteStudioPath( model_path );
	if( ok )
	{
		extern qboolean Mod_GCAttachStudioSkins( model_t *m );

		Q_strncpy( gc_landmark_viewmodel, model_path, sizeof( gc_landmark_viewmodel ));
		/* The map-load skin pass skips viewmodels nobody holds yet
		 * ("deferred skins skip ... viewmodel not held"), so a weapon
		 * promoted here when it is first held needs its *t skins now, or
		 * it draws flat grey (ceres PR 97: v_garand bound, untextured). */
		mod = Mod_FindName( model_path, false );
		if( mod && mod->cache.data && Mod_GCIsViewModelPath( model_path ))
		{
			qboolean skinned = Mod_GCAttachStudioSkins( mod );

			Con_Reportf( "Xash3D GameCube: held viewmodel skins '%s' ok=%d\n",
				model_path, skinned ? 1 : 0 );
			R_GXStudioInvalidateBindings();
		}
	}
	return ok;
}

/*
=============
Mod_GCCapFacesClaimTeamViewModel

CapFaces DoD listen keeps lean pistols/crowbar in the 184 KiB studio BSS, so
the Allies/Axis rifle (~110 KiB lean) cannot promote after team pick. Abandon
BSS-resident studios, rewind the bump, then promote exactly one team VM so
Mod_GCAttachStudioSkins can merge the downsampled gc_studio/*t.mdl atlas.
=============
*/
qboolean Mod_GCCapFacesClaimTeamViewModel( const char *model_path )
{
	model_t *mod;
	int i, abandoned = 0;
	qboolean ok;

	if( !model_path || !model_path[0] )
		return false;

	mod = Mod_FindName( model_path, false );
	if( !( Mod_GCStudioAlreadyResident( mod ) && Mod_GCIsStudioBssCache( mod->cache.data ) ) )
	{
		gc_pinned_viewmodel_count = 0;
		memset( gc_pinned_viewmodels, 0, sizeof( gc_pinned_viewmodels ));
		for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
		{
			if( mod->type != mod_studio || !mod->cache.data )
				continue;
			if( !Mod_GCIsStudioBssCache( mod->cache.data ))
				continue;
			Con_Reportf( "Xash3D GameCube: CapFaces studio abandon '%s'\n",
				mod->name[0] ? mod->name : "?" );
			mod->submodels = NULL;
			mod->cache.data = NULL;
			if( mod->mempool )
				Mem_FreePool( &mod->mempool );
			abandoned++;
		}
		gc_real_studio_npc = 0;
		gc_real_studio_view = 0;
		gc_real_studio_bytes = 0;
		gc_studio_bss_owners = 0;
		gc_studio_bss_bump = 0;
		Mod_GCVMSlotsReset();
		gc_landmark_viewmodel[0] = '\0';
		Con_Reportf( "Xash3D GameCube: CapFaces studio BSS reclaim abandoned=%d for '%s'\n",
			abandoned, model_path );

		FS_ClearFindMissCache();
		Image_GCPurgeDecodeScratch();
		ok = Mod_GCEnsureLandmarkViewModel( model_path );
		if( !ok )
			return false;
	}
	else
	{
		Mod_GCPinViewModel( mod );
		Q_strncpy( gc_landmark_viewmodel, model_path, sizeof( gc_landmark_viewmodel ));
	}

	/* Skin only the claimed rifle — UploadDeferred also tried mp40 and tip-OOMd. */
	{
		extern qboolean Mod_GCAttachStudioSkins( model_t *mod );

		mod = Mod_FindName( model_path, false );
		if( mod && mod->cache.data )
			ok = Mod_GCAttachStudioSkins( mod );
	}
	return ok;
}

/*
=============
Mod_GCRebindPromotedStudios

G371: after deferred NPC promote, point sv.models[] slots at the resident
mesh. SV_SetModel during spawn often left NULL/stub handles, so EmitBrush
saw studio_seen=0 despite real studio loaded.
=============
*/
void Mod_GCRebindPromotedStudios( void )
{
	int i;
	int rebound = 0;

	if( !SV_Active() )
		return;
	for( i = 1; i < MAX_MODELS && sv.model_precache[i][0]; i++ )
	{
		const char *name = sv.model_precache[i];
		model_t *mod;
		qboolean is_view = false;

		if( !name || Q_strnicmp( name, "models/", 7 ) || !Q_stristr( name, ".mdl" ))
			continue;
		if( !Mod_GCStudioNameAllowed( name, &is_view ))
			continue;
		mod = Mod_FindName( name, false );
		if( !Mod_GCStudioAlreadyResident( mod ))
			continue;
		if( sv.models[i] == mod )
			continue;
		sv.models[i] = mod;
		rebound++;
	}
	if( rebound > 0 )
	{
		R_GXStudioInvalidateBindings();
		Con_Reportf( "Xash3D GameCube: G371 rebound promoted studios slots=%d\n", rebound );
	}
}

/*
=============
Mod_GCPromoteLeanNpcStudios

G375: DumpFrames often holds EFB before present≥24 (G371 gate). Force the
lean NPC mesh promote so denser stills can capture scientists/barneys.
=============
*/
void Mod_GCPromoteLeanNpcStudios( void )
{
	static const char *promote_lean_npc[] = {
		/* Headcrab is the smallest deferred NPC.  Promote it first while the
		 * post-map heap still has a contiguous read/decode window; loading it
		 * after scientist/barney made the ZIP read fail despite the compact
		 * gc_studio mirror being present. */
		"models/headcrab.mdl",
		"models/scientist.mdl",
		"models/barney.mdl",
		NULL
	};
	int i;

	if( !Mod_GCIsNewGameRoute() && !GC_IsNewGameWorldReady() )
		return;

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();
	for( i = 0; promote_lean_npc[i]; i++ )
		Mod_GCPromoteStudioPath( promote_lean_npc[i] );
	Con_Reportf( "Xash3D GameCube: G375 dump NPC studios try npc=%d view=%d budget=%s\n",
		gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ));
	Mod_GCRebindPromotedStudios();
}

/*
=============
Mod_GCPromoteMapModels

WorldDL listen: promote every allowlisted models/mapmodels/*.mdl from the
server precache after MAP_READY — full-skin props are too big for the 184 KiB
boot BSS (sherman≈294 KiB) and must use post-world heap (ava-prop-1).
=============
*/
void Mod_GCPromoteMapModels( void )
{
	int i;
	int tried = 0;
	int ok = 0;

	if( !SV_Active() )
		return;
	if( !Sys_CheckParm( "-gcworldrender" ))
		return;

	/* Tip≈6 KiB — heap promote is dead. Lean meshes load into world-array
	 * spill (WorldDL chunk cache capped at 192 KiB on listen). Viewmodels
	 * stay in the 184 KiB studio BSS. */
	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();
	for( i = 1; i < MAX_MODELS && sv.model_precache[i][0]; i++ )
	{
		const char *name = sv.model_precache[i];
		qboolean is_view = false;
		model_t *mod;

		if( !name || !Q_stristr( name, "mapmodels/" ) || !Q_stristr( name, ".mdl" ))
			continue;
		if( !Mod_GCStudioNameAllowed( name, &is_view ))
			continue;
		tried++;
		if( !Mod_GCPromoteStudioPath( name ))
			continue;
		ok++;
		{
			extern qboolean Mod_GCAttachStudioSkins( model_t *m );

			mod = Mod_FindName( name, false );
			if( mod && mod->cache.data )
				Mod_GCAttachStudioSkins( mod );
		}
	}
	Mod_GCRebindPromotedStudios();
	Con_Reportf( "Xash3D GameCube: mapmodel studios promote ok=%d/%d budget=%s tip=%s\n",
		ok, tried, Q_memprint( gc_real_studio_bytes ), Q_memprint( GC_MemHeapFreeBytes() ));
}

/*
=============
Mod_GCMapNeedsStudio

Whether the running map can use this model, from its own entity lump: the
path itself (monster_generic / furniture / cycler "model" keys) or a class
that uses it.  The fixed preload lists loaded the zombie and headcrab on
every map, so on c0a0 they took the arena before the forklift the map
actually scripts.  Carried weapons still load on demand at SetModel.
=============
*/
static qboolean Mod_GCMapNeedsStudio( const char *path )
{
	static const struct { const char *model; const char *classes[4]; } uses[] = {
		{ "models/scientist.mdl", { "monster_scientist", "monster_sitting_scientist", NULL } },
		{ "models/barney.mdl", { "monster_barney", NULL } },
		{ "models/headcrab.mdl", { "monster_headcrab", NULL } },
		{ "models/zombie.mdl", { "monster_zombie", NULL } },
		{ "models/roach.mdl", { "monster_cockroach", NULL } },
		/* Weapon viewmodels preload only where the map places the weapon;
		 * carried ones load on demand when held (V_SetupViewModel). */
		{ "models/v_crowbar.mdl", { "weapon_crowbar", NULL } },
		{ "models/v_9mmhandgun.mdl", { "weapon_9mmhandgun", "weapon_glock", NULL } },
		{ "models/v_9mmar.mdl", { "weapon_9mmAR", "weapon_mp5", NULL } },
		{ "models/v_shotgun.mdl", { "weapon_shotgun", NULL } },
		{ "models/v_357.mdl", { "weapon_357", "weapon_python", NULL } },
	};
	const char *ents = sv.worldmodel ? sv.worldmodel->entities : NULL;
	int i, j;

	if( !ents || !ents[0] )
		return true;	/* nothing to judge by: keep the old behaviour */
	if( Q_stristr( ents, path ))
		return true;
	for( i = 0; i < (int)ARRAYSIZE( uses ); i++ )
	{
		if( Q_stricmp( path, uses[i].model ))
			continue;
		for( j = 0; uses[i].classes[j]; j++ )
		{
			char quoted[64];

			Q_snprintf( quoted, sizeof( quoted ), "\"%s\"", uses[i].classes[j] );
			if( Q_stristr( ents, quoted ))
				return true;
		}
		return false;
	}
	/* A viewmodel no map entity names waits until it is held, like the ones
	 * above. Lost Coast: preloading v_grenade took 133 KB of the studio arena
	 * at spawn, room the prop chain cache needs in the harbor. DoD: the game
	 * precaches its whole gun table (18 viewmodels, 30-120 KB each plus a
	 * 30-86 KB *t skin scratch), and on dod_avalanche that ran the heap from
	 * 96 KB to 2.4 KB during entity spawn, so the grenade world models, later
	 * entities and listenserver.cfg failed to load (ceres 59f4f145). Only the
	 * held gun is ever drawn, and it loads on demand (V_SetupViewModel and the
	 * lean listen route in cl_frame.c). */
	if( !Q_strnicmp( path, "models/v_", 9 ))
		return false;
	return true;	/* unknown model: keep the old behaviour */
}

static void Mod_GCPromoteIfMapNeeds( const char *path )
{
	if( Mod_GCMapNeedsStudio( path ))
		Mod_GCPromoteStudioPath( path );
	else
		Con_Reportf( "Xash3D GameCube: studio preload skip '%s' (not used on %s)\n",
			path, sv.name[0] ? sv.name : "?" );
}

/*
=============
Mod_GCTryDeferredStudios

Attempt to load deferred studios after map prep when memory is available.
=============
*/
void Mod_GCTryDeferredStudios( void )
{
	/* Try to load additional deferred studios if budget allows. */
	static const char *promote_full[] = {
		"models/v_9mmhandgun.mdl",
		"models/v_9mmar.mdl",
		"models/v_shotgun.mdl",
		"models/v_357.mdl",
		"models/headcrab.mdl",
		"models/zombie.mdl",
		NULL
	};
	/* Lean: handgun + crowbar at present=1. NPCs at later presents (G371)
	 * once Flipper MEM has coalesced — scientist/barney SetModel alone left
	 * stubs (studio_seen=0). */
	static const char *promote_lean[] = {
		"models/v_9mmhandgun.mdl",
		"models/w_crowbar.mdl",
		NULL
	};
	static const char *promote_lean_npc[] = {
		/* Read the smallest deferred NPC before the larger cast meshes consume
		 * the last contiguous MEM1 archive-read window. */
		"models/headcrab.mdl",
		"models/scientist.mdl",
		"models/barney.mdl",
		NULL
	};
	const char **promote;
	int i;
	unsigned presents;

	if( !Mod_GCIsNewGameRoute() && !GC_IsNewGameWorldReady() )
		return;

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();
	/* G105: the direct new-game gameplay route needs a real first-person
	 * mesh before the presentation probe can bind cl.viewent.  Crowbar is
	 * deliberately promoted first because it is the guaranteed starter
	 * weapon and is included in the compact gc_studio allowlist.
	 * Lean -gcnewgame (probe 20260807-023917) previously skipped this under
	 * a -gcfullphysics-only gate and only loaded v_9mmhandgun — G506 WEAK. */
	/* Direct -gcnewgame probes only: on the menu (retail) route the player
	 * has no weapon on the tram, and the held weapon's viewmodel loads on
	 * demand (V_SetupViewModel). */
	if( Mod_GCIsNewGameRoute() && Sys_CheckParm( "-gcnewgame" ))
	{
		if( Mod_GCEnsureLandmarkViewModel( "models/v_crowbar.mdl" ))
			Con_Reportf( "Xash3D GameCube: G105 landmark viewmodel ready models/v_crowbar.mdl\n" );
		else if( Mod_GCEnsureLandmarkViewModel( "models/v_9mmhandgun.mdl" ))
			Con_Reportf( "Xash3D GameCube: G105 landmark viewmodel ready models/v_9mmhandgun.mdl\n" );
		else
			Con_Reportf( S_WARN "Xash3D GameCube: G105 landmark viewmodel unavailable\n" );
	}

	presents = GC_GetNewGamePresentCount();
	/* Menu New Game loads the cast at precache (Mod_LoadModel). */
	if( GC_MenuRunsServer() && presents >= 24 )
		return;
	if( Mod_GCIsNewGameRoute() && !Sys_CheckParm( "-gcfullphysics" )
		&& presents >= 24 )
	{
		/* G371: post-present NPC meshes — denser AM scientists/barneys. */
		promote = promote_lean_npc;
		for( i = 0; promote[i]; i++ )
			Mod_GCPromoteIfMapNeeds( promote[i] );
		Con_Reportf( "Xash3D GameCube: G371 deferred NPC studios try npc=%d view=%d budget=%s presents=%u\n",
			gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ),
			presents );
		Mod_GCRebindPromotedStudios();
		Mod_GCUploadDeferredStudioSkins();
		return;
	}

	promote = ( Mod_GCIsNewGameRoute() && !Sys_CheckParm( "-gcfullphysics" ))
		? promote_lean : promote_full;
	for( i = 0; promote[i]; i++ )
		Mod_GCPromoteIfMapNeeds( promote[i] );

	Con_Reportf( "Xash3D GameCube: deferred studios try npc=%d view=%d budget=%s\n",
		gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ));
	Mod_GCRebindPromotedStudios();
	/* Quality-0 spawn skips companion/view soft skins — upload once tip recovers. */
	Mod_GCUploadDeferredStudioSkins();
}

/*
=============
Mod_GCLoadStartupStudios

Load minimal studios needed for initial map (e.g., player viewweapon).
Runs before map load to avoid late allocation stalls.
=============
*/
void Mod_GCLoadStartupStudios( void )
{
	static const char *promote[] = {
		"models/v_crowbar.mdl",
		"models/v_9mmhandgun.mdl",
		NULL
	};
	int i;

	if( !Mod_GCIsNewGameRoute() && !GC_IsNewGameWorldReady() )
		return;

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();

	for( i = 0; promote[i]; i++ )
		Mod_GCPromoteIfMapNeeds( promote[i] );

	Con_Reportf( "Xash3D GameCube: startup studios loaded npc=%d view=%d budget=%s\n",
		gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ));
}

/*
=============
Mod_GCLoadNewGameStudios

Prepare-time attempt (often tip-starved). Prefer Mod_GCTryDeferredStudios after
a few Flipper presents when MEM1 has coalesced (G287).
=============
*/
void Mod_GCLoadNewGameStudios( void )
{
	/* Crowbar first — handgun is 134 KiB and starves the tip at prepare. */
	static const char *promote[] = {
		"models/v_crowbar.mdl",
		"models/roach.mdl",
		NULL
	};
	int i;

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();

	for( i = 0; promote[i]; i++ )
		Mod_GCPromoteIfMapNeeds( promote[i] );

	Con_Reportf( "Xash3D GameCube: deferred studio done npc=%d view=%d budget=%s\n",
		gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ));
}

/*
=============
Mod_GCLoadPreEntityStudios

After the world BSP has settled (and FatPVS extras are reclaimed), load the
allowlisted cast into the studio BSS arena so MonsterInit can read eye
positions and bone controllers. Deferring until post-present left c1a1f
hanging inside the second zombie Spawn with a null studiohdr.
=============
*/
void Mod_GCLoadPreEntityStudios( void )
{
	/* c1a1f cast first (proven tip-safe ~0.5 MiB with skins). Heavier
	 * campaign meshes (gman/houndeye/forklift) promote on SetModel demand
	 * so pre-entity does not push studio BSS into the crash cliff. */
#if XASH_DODGC_NETWORK
	/* DoD CapFaces listen: keep the 184 KiB studio BSS empty until team pick
	 * claims one lean rifle (Mod_GCCapFacesClaimTeamViewModel). Preloading
	 * colt/crowbar left no room for garand/mp40. */
	static const char *promote[] = { NULL };
#else
	static const char *promote[] = {
		"models/v_crowbar.mdl",
		"models/w_crowbar.mdl",
		/* Hallway cast before filler NPCs — scientist/barney soft skins need
		 * BSS-resident meshes before headcrab/filecabinet fill the bump. */
		"models/scientist.mdl",
		"models/barney.mdl",
		"models/roach.mdl",
		"models/headcrab.mdl",
		"models/zombie.mdl",
		"models/filecabinet.mdl",
		NULL
	};
#endif
	int i;

	/* -gcmap / session memopt — same gate as the call site, explicit so a
	 * stale outer check cannot skip the cast. */
	if( !GC_MapLoadMemoryOpt() && !Sys_CheckParm( "-gcmap" )
		&& !Sys_CheckParm( "-gcnewgame" ))
		return;

	/* Xen maps need this bounded arena for their own actors. Loading the
	 * unrelated intro cast first left c4a1d with only 17 KiB after its alien
	 * controllers and could exhaust it before Nihilanth or Gonarch spawn. */
	if( !Q_strnicmp( sv.name, "c4a", 3 ))
	{
		Con_Reportf( "Xash3D GameCube: pre-entity studios reserved for Xen map %s\n", sv.name );
		return;
	}

	FS_ClearFindMissCache();
	Image_GCPurgeDecodeScratch();
	gc_studio_pre_entity_mesh_only = true;
	for( i = 0; promote[i]; i++ )
		Mod_GCPromoteIfMapNeeds( promote[i] );
	gc_studio_pre_entity_mesh_only = false;
	Mod_GCRebindPromotedStudios();
	Con_Reportf( "Xash3D GameCube: pre-entity studios npc=%d view=%d budget=%s bump=%s\n",
		gc_real_studio_npc, gc_real_studio_view, Q_memprint( gc_real_studio_bytes ),
		Q_memprint( gc_studio_bss_bump ));
}


static qboolean Mod_GCMapVerboseModelLoad( const char *name )
{
	return false;
}

void Mod_PrintWorldStats_f( void )
{
	Con_Printf( "worldmodel %s: loaded\n", mod_known->name );
}
#endif
CVAR_DEFINE( mod_studiocache, "r_studiocache", "1", FCVAR_ARCHIVE, "enables studio cache for speedup tracing hitboxes" );
CVAR_DEFINE_AUTO( r_wadtextures, "0", FCVAR_LATCH, "completely ignore textures in the bsp-file if enabled" );
CVAR_DEFINE_AUTO( r_showhull, "0", 0, "draw collision hulls 1-3" );
CVAR_DEFINE_AUTO( r_allow_wad3_luma, "0", FCVAR_LATCH|FCVAR_ARCHIVE, "allow usage of luma textures in wad3 (tilde textures)" );

/*
===============================================================================

			MOD COMMON UTILS

===============================================================================
*/
/*
================
Mod_Modellist_f
================
*/
static void Mod_Modellist_f( void )
{
	int	i, nummodels;
	model_t	*mod;

	Con_Printf( "\n" );
	Con_Printf( "-----------------------------------\n" );

	for( i = nummodels = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		const char *color_str;

		if( mod->needload == NL_UNREFERENCED )
			continue; // free slot

		switch( mod->type )
		{
		case mod_alias:
			color_str = S_YELLOW;
			break;
		case mod_studio:
			color_str = S_GREEN;
			break;
		case mod_sprite:
			color_str = S_MAGENTA;
			break;
		case mod_brush:
			color_str = mod->name[0] == '*' ? S_CYAN : S_BLUE;
			break;
		default:
			color_str = S_RED;
			break;
		}

		Con_Printf( "%3d:\t%s%s\n" S_DEFAULT, i, color_str, mod->name );
		nummodels++;
	}

	Con_Printf( "-----------------------------------\n" );
	Con_Printf( "%i total models, %i total allocated slots\n", nummodels, mod_numknown );
	Con_Printf( "\n" );
}

static void Mod_UnloadRenderData( model_t *mod )
{
#if !XASH_DEDICATED
	// loader failed before renderer got a chance to process this model
	if( mod->type == mod_bad )
		return;

	switch( mod->type )
	{
	case mod_sprite:
		Mod_SpriteUnloadTextures( mod->cache.data );
		break;
	default:
		break;
	}

	ref.dllFuncs.Mod_ProcessRenderData( mod, false, NULL, 0 );
#endif
}

/*
================
Mod_FreeUserData
================
*/
static void Mod_FreeUserData( model_t *mod )
{
	// ignore submodels and freed models
	if( mod->needload == NL_UNREFERENCED || mod->name[0] == '*' )
		return;

	if( Host_IsDedicated() )
	{
		if( svgame.physFuncs.Mod_ProcessUserData != NULL )
		{
			// let the server.dll free custom data
			svgame.physFuncs.Mod_ProcessUserData( mod, false, NULL );
		}
	}
	else
	{
		Mod_UnloadRenderData( mod );
	}
}

#if XASH_GAMECUBE
qboolean R_GCIsMapLoadStaticArena( const void *ptr );

static void Mod_FreeLoadBuffer( void *buf )
{
	if( !buf )
		return;
	/* Map-load staging / retained BSP scratch are never Mem_ pool blocks. */
	if( GC_IsMapLoadBuffer( buf ) || R_GCIsMapLoadStaticArena( buf ))
		return;
	if( GC_ReleaseMapLoadBuffer( buf ))
		return;
	if( Mod_GCIsRetainedBspScratch( buf ))
	{
		Mod_GCClearRetainedBspScratch();
		return;
	}
	free( buf );
}

void Mod_ReleaseBrushSourceBuffer( void *buf )
{
	if( !buf )
		return;
	/* The BSP loader calls this while it is about to re-arm the same arena as
	 * scratch.  Drop the borrow without discarding the arena; the old-world
	 * destructor performs the subsequent discard explicitly. */
	if( GC_IsMapLoadBuffer( buf ))
	{
		GC_ReleaseMapLoadBuffer( buf );
		return;
	}
	Mod_FreeLoadBuffer( buf );
	GC_DiscardMapLoadBuffer();
}
#else
#define Mod_FreeLoadBuffer( buf ) Mem_Free( buf )
#endif

/*
================
Mod_FreeModel
================
*/
void Mod_FreeModel( model_t *mod )
{
	// already freed?
	if( !mod || mod->needload == NL_UNREFERENCED )
		return;

#if XASH_GAMECUBE
	/* G156: landmark viewweapons stay resident for Flipper / Deploy rebinds. */
	if( Mod_GCIsPinnedViewModel( mod ))
	{
		Con_Reportf( "Xash3D GameCube: G156 skip free pinned viewmodel %s\n",
			mod->name[0] ? mod->name : "?" );
		mod->needload = NL_PRESENT;
		return;
	}
#endif

	if( mod->type != mod_brush || mod->name[0] != '*' )
	{
		Mod_FreeUserData( mod );
#if XASH_GAMECUBE
		if( mod->type == mod_brush )
		{
			Con_Reportf( "Xash3D GameCube: FreeModel brush userdata done %s tip=%s\n",
				mod->name[0] ? mod->name : "?", Q_memprint( GC_MemHeapFreeBytes() ));
			Mod_GameCubeFreeMallocSurfaces( mod );
			Con_Reportf( "Xash3D GameCube: FreeModel malloc surfaces done %s tip=%s\n",
				mod->name[0] ? mod->name : "?", Q_memprint( GC_MemHeapFreeBytes() ));
		}
		if( mod->type == mod_brush && mod->cache.data )
		{
			/* The world BSP may have retained the map-load arena as renderer
			 * scratch.  This path is model teardown (unlike the staging helper),
			 * so clear that ownership before returning the arena. */
			if( Mod_GCIsRetainedBspScratch( mod->cache.data ))
				Mod_GCClearRetainedBspScratch();
			Mod_ReleaseBrushSourceBuffer( mod->cache.data );
			GC_DiscardMapLoadBuffer();
			mod->cache.data = NULL;
			Con_Reportf( "Xash3D GameCube: FreeModel brush source released %s tip=%s\n",
				mod->name[0] ? mod->name : "?", Q_memprint( GC_MemHeapFreeBytes() ));
		}
		/* Mesh-only New Game studios keep cache on malloc (mempool == 0).
		 * G287 BSS-resident crowbar must not be passed to free(). */
		if( mod->type == mod_studio && !mod->mempool && mod->cache.data )
		{
			if( Mod_GCIsStudioBssCache( mod->cache.data ))
			{
				if( gc_studio_bss_owners > 0 )
					gc_studio_bss_owners--;
				if( gc_studio_bss_owners <= 0 )
				{
					gc_studio_bss_owners = 0;
					gc_studio_bss_bump = 0;
					Mod_GCVMSlotsReset();
				}
				mod->cache.data = NULL;
			}
			else if( Mod_GCIsWorldArraySpillCache( mod->cache.data )
				|| GC_IsMapLoadBuffer( mod->cache.data ))
			{
				/* G289 spill-resident cache (mod_studio.c) lives in the map
				 * arena, never on the libc heap: free() would walk a bogus
				 * chunk header (Flash Guru, DAR=3FA2AB2C). */
				mod->cache.data = NULL;
			}
			else
			{
				free( mod->cache.data );
				mod->cache.data = NULL;
			}
		}
#endif
#if XASH_GAMECUBE
			/* The old world's pool is always freed.  A low-heap skip here
			 * (added for a tram-hop hang) leaked the pool on every changelevel
			 * once the heap sat under 48 KiB, which then kept it there: c2a1's
			 * c1a4i/c1a4b round trips ran out of memory after ~75 loads.  At
			 * low heap, list what the pool still holds so a bad block that
			 * would hang the walk is named in the log. */
			if( mod->type == mod_brush && FBitSet( mod->flags, MODEL_WORLD )
				&& GC_MemHeapFreeBytes() < 48u * 1024u )
			{
				extern void Mem_GCDescribePool( poolhandle_t poolptr, const char *why );
				Mem_GCDescribePool( mod->mempool, "world free at low tip" );
			}
			if( !( mod->type == mod_sprite && mod->mempool == host.mempool ))
				Mem_FreePool( &mod->mempool );
#else
		Mem_FreePool( &mod->mempool );
#endif
	}

	if( mod->type == mod_brush && FBitSet( mod->flags, MODEL_WORLD ) )
	{
		world.version = 0;
		world.shadowdata = NULL;
		world.deluxedata = NULL;

		// data already freed by Mem_FreePool above
		world.hull_models = NULL;
		world.compressed_phs = NULL;
		world.phsofs = NULL;
	}

	memset( mod, 0, sizeof( *mod ));
}

/*
===============================================================================

			MODEL INITIALIZE\SHUTDOWN

===============================================================================
*/
/*
================
Mod_Init
================
*/
void Mod_Init( void )
{
	com_studiocache = Mem_AllocPool( "Studio Cache" );
	if( !com_studiocache )
		return;
	Cvar_RegisterVariable( &mod_studiocache );
	Cvar_RegisterVariable( &r_wadtextures );
	Cvar_RegisterVariable( &r_showhull );
	Cvar_RegisterVariable( &r_allow_wad3_luma );

	Cmd_AddCommand( "mapstats", Mod_PrintWorldStats_f, "show stats for currently loaded map" );
	Cmd_AddCommand( "modellist", Mod_Modellist_f, "display loaded models list" );

	Mod_ResetStudioAPI ();
	Mod_InitStudioHull ();
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll( void )
{
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
#if XASH_GAMECUBE
	/* Full purge — drop G156 pins so FreeModel can reclaim viewweapons. */
	gc_pinned_viewmodel_count = 0;
	memset( gc_pinned_viewmodels, 0, sizeof( gc_pinned_viewmodels ));
#endif
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeModel( &mod_known[i] );
#if XASH_GAMECUBE
	gc_real_studio_npc = 0;
	gc_real_studio_view = 0;
	gc_real_studio_bytes = 0;
	gc_deferred_studio_attempts = 0;
	gc_studio_bss_bump = 0;
	gc_studio_bss_owners = 0;
	Mod_GCVMSlotsReset();
	gc_landmark_viewmodel[0] = '\0';
#endif
	mod_numknown = 0;
}

/*
================
Mod_ClearUserData
================
*/
void Mod_ClearUserData( void )
{
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeUserData( &mod_known[i] );
}

/*
================
Mod_Shutdown
================
*/
void Mod_Shutdown( void )
{
	Mod_FreeAll();
	Mem_FreePool( &com_studiocache );
}

/*
===============================================================================

			MODELS MANAGEMENT

===============================================================================
*/
/*
==================
Mod_FindName

never return NULL
==================
*/
model_t *Mod_FindName( const char *filename, qboolean trackCRC )
{
	char	modname[MAX_QPATH];
	model_t	*mod;
	int	i;

	Q_strncpy( modname, filename, sizeof( modname ));

	// search the currently loaded models
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( !Q_stricmp( mod->name, modname ))
		{
			/* Mesh-only GC studios keep cache on calloc (mempool==0). Treat
			 * any model with cache.data as already loaded so Mod_ForName does
			 * not downgrade them back to gcmap stubs. */
			if( mod->mempool || mod->cache.data || mod->name[0] == '*' )
				mod->needload = NL_PRESENT;
			else
				mod->needload = NL_NEEDS_LOADED;

			return mod;
		}
	}

	// find a free model slot spot
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_UNREFERENCED )
			break; // this is a valid spot
	}

	if( i == mod_numknown )
	{
		if( mod_numknown == MAX_MODELS )
			Host_Error( "MAX_MODELS limit exceeded (%d)\n", MAX_MODELS );
		mod_numknown++;
	}

	// copy name, so model loader can find model file
	Q_strncpy( mod->name, modname, sizeof( mod->name ));
	if( trackCRC ) mod_crcinfo[i].flags = FCRC_SHOULD_CHECKSUM;
	else mod_crcinfo[i].flags = 0;
	mod->needload = NL_NEEDS_LOADED;
	mod_crcinfo[i].initialCRC = 0;

	return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *Mod_LoadModel( model_t *mod, qboolean crash )
{
	char		tempname[MAX_QPATH];
	char		loadname[MAX_QPATH];
	fs_offset_t		length = 0;
	qboolean		loaded, loaded2 = false;

	if( !mod )
	{
		Host_Error( "%s: mod == NULL\n", __func__ );
		return NULL;
	}

	// check if already loaded (or inline bmodel)
	/* Mesh-only New Game studios use calloc cache with mempool==0 — still loaded. */
	if( mod->mempool || mod->cache.data || mod->name[0] == '*' )
	{
		mod->needload = NL_PRESENT;
		return mod;
	}

	if( mod->needload != NL_NEEDS_LOADED )
	{
		Host_Error( "%s: trying to load model not marked for loading (%d)\n", __func__, mod->needload );
		return NULL;
	}

	// store modelname to show error
	Q_strncpy( tempname, mod->name, sizeof( tempname ));
	COM_FixSlashes( tempname );
	Q_strncpy( loadname, tempname, sizeof( loadname ));

#if XASH_GAMECUBE
	if( Mod_GCMapVerboseModelLoad( tempname ))
	{
		Con_Reportf( "Xash3D GameCube: Mod_LoadModel request mod='%s' temp='%s'\n", mod->name, tempname );
	}
#endif

#if XASH_GAMECUBE
	if( GC_MapLoadMemoryOpt() && !world.loading )
	{
		const char *ext = COM_FileExtension( tempname );

		if( ext && ( !Q_stricmp( ext, "mdl" ) || !Q_stricmp( ext, "spr" )) )
		{
			/* Monster AI reads studiohdr during Spawn (eye position, controllers).
			 * Promote allowlisted meshes into the studio BSS arena now for every
			 * memopt route — not only the retail menu server. */
			qboolean is_view = false;

			/* World precache registers every weapon; a viewmodel the map does
			 * not place stays deferred and loads when held (V_SetupViewModel). */
			/* Third-person weapons likewise load when a player first holds
			 * one (CL lean player emit): precaching the whole DoD table put
			 * all 17 in memory at spawn, twice across the map reload. */
			if( !Q_stricmp( ext, "mdl" )
				&& Mod_GCStudioNameAllowed( tempname, &is_view )
				&& !Mod_GCIsWeaponModelPath( tempname )
				&& !( is_view && !Mod_GCMapNeedsStudio( tempname ))
				&& Mod_GCPromoteStudioPath( tempname ))
				return mod;
			mod->needload = NL_NEEDS_LOADED;
			return mod;
		}
	}
#endif

	byte *buf = NULL;
#if XASH_GAMECUBE
	/* Prefer the early-reserved contiguous buffer for world BSPs so retail New
	 * Game is not blocked by heap fragmentation after menu/client churn.
	 * DoD retail maps keep a large LUMP_LIGHTING (~1 MiB). Loading the full BSP
	 * into the 3.75 MiB arena leaves too little high-slot for msurface blocks
	 * (dod_flash 9801 surfs ≈1.72 MiB → Mem_Alloc fatal with BBA). Always
	 * compact-load without LUMP_LIGHTING when present — G154 reloads samples
	 * from disc into ARAM for CapFaces. Full image only when there is no LM. */
	if( !Q_strnicmp( loadname, "maps/", 5 ))
	{
		fs_offset_t filesize = FS_FileSize( loadname, false );

		if( filesize > (fs_offset_t)sizeof( uint ))
		{
			file_t *f = FS_Open( loadname, "rb", false );
			dheader_t hdr;
			int light_ofs = 0, light_len = 0;
			size_t compact_need = (size_t)filesize;
			size_t light_align_pad = 0;
			qboolean prefer_compact = false;

			if( f && FS_Read( f, &hdr, sizeof( hdr )) == (fs_offset_t)sizeof( hdr ))
			{
				const int ver = LittleLong( hdr.version );

				if( ver == HLBSP_VERSION || ver == Q1BSP_VERSION )
				{
					light_ofs = LittleLong( hdr.lumps[LUMP_LIGHTING].fileofs );
					light_len = LittleLong( hdr.lumps[LUMP_LIGHTING].filelen );
				}
			}

			/* Post-lighting lumps keep disc alignment. Stripping a lighting
			 * blob whose length is not 4-byte aligned shifts every later lump
			 * (textures/ents/vis) onto a misaligned address. On BE GameCube
			 * that makes dmiptexlump_t.nummiptex read as 0 → empty textures →
			 * Mod_LoadTexInfo null-deref (dod_flash ship 20261110-083723). */
			if( light_len > 0 && light_ofs > 0
				&& (size_t)light_ofs + (size_t)light_len <= (size_t)filesize )
			{
				light_align_pad = (size_t)light_len & 3u;
				compact_need = (size_t)filesize - (size_t)light_len + light_align_pad;
				prefer_compact = compact_need < (size_t)filesize;
			}

			if( f && prefer_compact
				&& ( buf = GC_BorrowMapLoadBuffer( compact_need )) != NULL )
			{
				size_t out = 0;
				byte chunk[4096];
				fs_offset_t pos = 0;
				qboolean ok = true;
				int i;

				FS_Seek( f, 0, SEEK_SET );
				while( pos < filesize && ok )
				{
					size_t want;
					fs_offset_t got;

					if( light_len > 0 && pos >= light_ofs
						&& pos < (fs_offset_t)( light_ofs + light_len ))
					{
						if( light_align_pad )
						{
							if( out + light_align_pad > compact_need )
							{
								ok = false;
								break;
							}
							memset( buf + out, 0, light_align_pad );
							out += light_align_pad;
						}
						pos = (fs_offset_t)( light_ofs + light_len );
						FS_Seek( f, pos, SEEK_SET );
						continue;
					}

					want = sizeof( chunk );
					if( light_len > 0 && pos < light_ofs
						&& pos + (fs_offset_t)want > light_ofs )
						want = (size_t)( light_ofs - pos );
					if( pos + (fs_offset_t)want > filesize )
						want = (size_t)( filesize - pos );
					if( out + want > compact_need )
					{
						ok = false;
						break;
					}

					got = FS_Read( f, chunk, want );
					if( got != (fs_offset_t)want )
					{
						ok = false;
						break;
					}
					memcpy( buf + out, chunk, want );
					out += want;
					pos += (fs_offset_t)want;
				}

				if( ok && out == compact_need && out >= sizeof( dheader_t ))
				{
					dheader_t *dh = (dheader_t *)buf;
					const int shrink = light_len - (int)light_align_pad;

					dh->lumps[LUMP_LIGHTING].fileofs = 0;
					dh->lumps[LUMP_LIGHTING].filelen = 0;
					for( i = 0; i < HEADER_LUMPS; i++ )
					{
						const int ofs = LittleLong( dh->lumps[i].fileofs );
						const int len = LittleLong( dh->lumps[i].filelen );

						if( i == LUMP_LIGHTING || len <= 0 )
							continue;
						if( ofs > light_ofs )
							dh->lumps[i].fileofs = LittleLong( ofs - shrink );
					}
					length = (fs_offset_t)out;
					Con_Reportf( "Xash3D GameCube: map load compact (no lighting) %s → %s (G154 disc LM)%s\n",
						Q_memprint( (size_t)filesize ), Q_memprint( out ),
						light_align_pad ? " +align" : "" );
				}
				else
				{
					GC_ReleaseMapLoadBuffer( buf );
					buf = NULL;
					length = 0;
					Con_Reportf( S_ERROR "Xash3D GameCube: compact map load failed need=%s out=%lu\n",
						Q_memprint( compact_need ), (unsigned long)out );
				}
			}
			else if( f && ( buf = GC_BorrowMapLoadBuffer( (size_t)filesize )) != NULL )
			{
				FS_Seek( f, 0, SEEK_SET );
				length = FS_Read( f, buf, filesize );
				if( length == filesize )
				{
					Con_Reportf( "Xash3D GameCube: map load using reserved buffer %s\n",
						Q_memprint( (size_t)filesize ));
				}
				else
				{
					GC_ReleaseMapLoadBuffer( buf );
					buf = NULL;
					length = 0;
				}
			}

			if( f )
				FS_Close( f );
		}
	}
	if( !buf )
#endif
	{
		// Check file exists before attempting to load
		if( !FS_FileExists( loadname, false ))
		{
#if XASH_GAMECUBE
			if( GC_MapLoadMemoryOpt() && ( !Q_strncmp( loadname, "maps", 4 ) || !Q_strncmp( loadname, "models", 6 )))
			{
				fs_offset_t filesize = FS_FileSize( loadname, false );

				Con_Reportf( "Xash3D GameCube: model file missing mod='%s' path='%s' size=%li\n",
					mod->name, loadname, (long)filesize );
			}
#endif
			memset( mod, 0, sizeof( model_t ));

			if( crash ) Host_Error( "Could not load model %s from disk\n", loadname );
			else Con_Printf( S_ERROR "Could not load model %s from disk\n", loadname );

			return NULL;
		}

		buf = FS_LoadFile( loadname, &length, false );
	}

	if( !buf || length < sizeof( uint ))
	{
#if XASH_GAMECUBE
		if( GC_MapLoadMemoryOpt() && ( !Q_strncmp( loadname, "maps", 4 ) || !Q_strncmp( loadname, "models", 6 )))
		{
			fs_offset_t filesize = FS_FileSize( loadname, false );
			qboolean exists = FS_FileExists( loadname, false );

			Con_Reportf( "Xash3D GameCube: model file failed mod='%s' path='%s' exists=%d size=%li length=%li buf=%p\n",
				mod->name, loadname, exists, (long)filesize, (long)length, (void *)buf );
		}
#endif
		memset( mod, 0, sizeof( model_t ));

		if( crash ) Host_Error( "Could not load model %s from disk\n", loadname );
		else Con_Printf( S_ERROR "Could not load model %s from disk\n", loadname );

		return NULL;
	}

	Con_Reportf( "loading %s\n", mod->name );
	mod->needload = NL_PRESENT;
	mod->type = mod_bad;

	// call the apropriate loader
	switch( *(uint *)buf )
	{
	case LittleLong( IDSTUDIOHEADER ):
		Mod_LoadStudioModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDSPRITEHEADER ):
		Mod_LoadSpriteModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDALIASHEADER ):
		Mod_LoadAliasModel( mod, buf, &loaded );
		break;
	case LittleLong( Q1BSP_VERSION ):
	case LittleLong( HLBSP_VERSION ):
	case LittleLong( QBSP2_VERSION ):
		Mod_LoadBrushModel( mod, buf, length, &loaded );
		break;
	default:
		Mod_FreeLoadBuffer( buf );
		if( crash ) Host_Error( "%s has unknown format\n", tempname );
		else Con_Printf( S_ERROR "%s has unknown format\n", tempname );
		return NULL;
	}

	if( loaded )
	{
		if( world.loading )
			SetBits( mod->flags, MODEL_WORLD ); // mark worldmodel

		if( Host_IsDedicated() )
		{
			if( svgame.physFuncs.Mod_ProcessUserData != NULL )
			{
				// let the server.dll load custom data
				svgame.physFuncs.Mod_ProcessUserData( mod, true, buf );
			}
			loaded2 = true;
		}
#if !XASH_DEDICATED
		else
		{
			loaded2 = ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length );
		}
#endif
	}

	if( mod->type == mod_alias )
	{
		aliashdr_t *hdr = mod->cache.data;
		if( hdr ) // clean up temporary pointer after passing the alias model to the renderer
			hdr->pposeverts = NULL;
	}

	if( !loaded || !loaded2 )
	{
		Mod_FreeModel( mod );
		Mod_FreeLoadBuffer( buf );

		if( crash ) Host_Error( "Could not load model %s\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s\n", tempname );

		return NULL;
	}

	model_info_t *p = &mod_crcinfo[mod - mod_known];
	mod->needload = NL_PRESENT;

	if( FBitSet( p->flags, FCRC_SHOULD_CHECKSUM ))
	{
		uint32_t currentCRC;

		CRC32_Init( &currentCRC );
		CRC32_ProcessBuffer( &currentCRC, buf, length );
		currentCRC = CRC32_Final( currentCRC );

		if( FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		{
			if( currentCRC != p->initialCRC )
				Host_Error( "%s has a bad checksum\n", tempname );
		}
		else
		{
			SetBits( p->flags, FCRC_CHECKSUM_DONE );
			p->initialCRC = currentCRC;
		}
	}
#if XASH_GAMECUBE
	if( mod->type != mod_brush || mod->cache.data != buf )
#endif
	{
#if XASH_GAMECUBE
		/* Don't free map load buffer - it's managed by GC system */
		if( !GC_IsMapLoadBuffer( buf ) && !R_GCIsMapLoadStaticArena( buf ))
#endif
			Mod_FreeLoadBuffer( buf );
	}

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName( const char *name, qboolean crash, qboolean trackCRC )
{
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	model_t *mod = Mod_FindName( name, trackCRC );
	return Mod_LoadModel( mod, crash );
}

/*
==================
Mod_PurgeStudioCache

free studio cache on change level
==================
*/
static void Mod_PurgeStudioCache( void )
{
	// refresh hull data
	SetBits( r_showhull.flags, FCVAR_CHANGED );
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
#if XASH_GAMECUBE
	/* G156: unpin viewmodels before freeing to avoid dangling references. */
	gc_pinned_viewmodel_count = 0;
	memset( gc_pinned_viewmodels, 0, sizeof( gc_pinned_viewmodels ));
#endif
#if XASH_GAMECUBE && !XASH_DEDICATED
	{
		extern void CL_GCLeaveMapArena( void );
		CL_GCLeaveMapArena();
	}
#endif
	// release previois map
	Mod_FreeModel( mod_known );	// world is stuck on slot #0 always
#if XASH_GAMECUBE
	/* The world source may be retained as BSP scratch without being present in
	 * cache.data on every load route.  At this point the old model is fully
	 * detached, so force the arena ownership transition before the next map's
	 * FS_LoadFile attempt. */
	Mod_GCClearRetainedBspScratch();
	GC_DiscardMapLoadBuffer();
	Con_Reportf( "Xash3D GameCube: changelevel old world map-load arena released\n" );
#endif

	// we should release all the world submodels
	// and clear studio sequences
	for( int i = 1; i < mod_numknown; i++ )
	{
		if( mod_known[i].needload == NL_UNREFERENCED )
			continue;

#if XASH_GAMECUBE
		/* The spill was rewound above. A studio model whose data lives in it
		 * (mapmodel-studio, merged T.mdl blocks) would otherwise be marked
		 * free-unused, reused by the next map as present, and read memory the
		 * new world DL tables and chunk cache now own. Drop it so the next
		 * load fetches it again. */
		if( mod_known[i].type == mod_studio && mod_known[i].cache.data
			&& Mod_GCIsWorldArraySpillCache( mod_known[i].cache.data ))
		{
			Mod_FreeModel( &mod_known[i] );
			continue;
		}
#endif
		if( mod_known[i].type == mod_studio )
			mod_known[i].submodels = NULL;

		if( mod_known[i].name[0] == '*' )
			Mod_FreeModel( &mod_known[i] );
		else
			mod_known[i].needload = NL_FREE_UNUSED;
	}

	Mem_EmptyPool( com_studiocache );
	Mod_ClearStudioCache();
}

/* Purge the old world before a caller reserves the next BSP buffer. */
void Mod_PurgeForMapLoad( void )
{
	Mod_PurgeStudioCache();
}

/*
==================
Mod_LoadWorld

Loads in the map and all submodels
==================
*/
model_t *Mod_LoadWorld( const char *name, qboolean preload )
{
	// already loaded?
	if( !Q_stricmp( mod_known->name, name ))
		return mod_known;

#if XASH_GAMECUBE
	Con_Reportf( "Xash3D GameCube: Mod_LoadWorld begin name=%s preload=%d\n",
		name ? name : "?", preload ? 1 : 0 );
#endif
	// free sequence files on studiomodels
	Mod_PurgeStudioCache();
#if XASH_GAMECUBE
	Con_Reportf( "Xash3D GameCube: Mod_LoadWorld after purge name=%s\n",
		name ? name : "?" );
#endif

	// load the newmap
	world.loading = true;
	model_t *pworld = Mod_FindName( name, false );
	if( preload ) Mod_LoadModel( pworld, true );
#if XASH_GAMECUBE
	if( GC_MapLoadMemoryOpt() )
	{
		fs_offset_t filesize = FS_FileSize( name, false );
		qboolean exists = FS_FileExists( name, false );

		Con_Reportf( "Xash3D GameCube: Mod_LoadWorld request='%s' registered='%s' exists=%d size=%li preload=%d\n",
			name, pworld ? pworld->name : "(null)", exists, (long)filesize, preload );
	}
	Con_Reportf( "Xash3D GameCube: Mod_LoadWorld ready name=%s\n",
		name ? name : "?" );
#endif
	world.loading = false;

	if( pworld != mod_known )
	{
		Host_Error( "%s: soft assert world model registration mismatch (%p != %p)\n",
			__func__, (void *)pworld, (void *)mod_known );
		return NULL;
	}

	return pworld;
}

/*
==================
Mod_FreeUnused

Purge all unused models
==================
*/
void Mod_FreeUnused( void )
{
	model_t	*mod;
	int	i;

	// never tries to release worldmodel
	for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_FREE_UNUSED )
			Mod_FreeModel( mod );
	}
}

#if XASH_GAMECUBE && defined( XASH_DODGC_NETWORK ) && XASH_DODGC_NETWORK
/*
================
GC_StaticBrushes

media/wd_<map>_sb.gcd (scripts/bake_worlddl_goldsrc.py): the brush entities
whose faces are baked into the world DL because nothing can ever change them
(unnamed opaque func_wall / func_illusionary at the lump origin).  The server
keeps them out of the packets (dod_avalanche has 252 entities in view and
190 of them are such walls), the client collides with them from this list.
Entry: model number of "*N", bit 15 set when solid.  The list only counts
when the world DL on the disc is the one it was baked with.
================
*/
#define GC_SB_MAX	256
#define GC_SB_MODELS	1024
#define GC_SB_SOLID	0x8000

static word gc_sb_list[GC_SB_MAX];
static byte gc_sb_bits[GC_SB_MODELS / 8];
static int gc_sb_count;
static int gc_sb_serial;
static char gc_sb_map[64];

const word *GC_StaticBrushes( const char *mapname, int *count, int *serial )
{
	char base[64];

	if( mapname && mapname[0] )
	{
		COM_FileBase( mapname, base, sizeof( base ));
		if( Q_stricmp( base, gc_sb_map ))
		{
			char path[MAX_SYSPATH];
			fs_offset_t len = 0;
			byte *buf;

			Q_strncpy( gc_sb_map, base, sizeof( gc_sb_map ));
			memset( gc_sb_bits, 0, sizeof( gc_sb_bits ));
			gc_sb_count = 0;
			gc_sb_serial++;

			Q_snprintf( path, sizeof( path ), "media/wd_%s_sb.gcd", base );
			buf = FS_LoadFile( path, &len, false );
			if( buf )
			{
				if( len >= 12 && !memcmp( buf, "GCsb", 4 ) && (( buf[4] << 8 ) | buf[5] ) == 1 )
				{
					const int n = ( buf[6] << 8 ) | buf[7];
					const fs_offset_t wd_want = ((fs_offset_t)buf[8] << 24 ) | ( buf[9] << 16 ) | ( buf[10] << 8 ) | buf[11];
					fs_offset_t wd_have;

					Q_snprintf( path, sizeof( path ), "media/wd_%s.gcd", base );
					wd_have = FS_FileSize( path, false );
					if( wd_have == wd_want && n <= GC_SB_MAX && len >= 12 + n * 2 )
					{
						for( int i = 0; i < n; i++ )
						{
							const word v = (word)(( buf[12 + i * 2] << 8 ) | buf[13 + i * 2] );
							const int num = v & ~GC_SB_SOLID;

							if( num < 1 || num >= GC_SB_MODELS )
								continue;
							gc_sb_bits[num >> 3] |= (byte)( 1 << ( num & 7 ));
							gc_sb_list[gc_sb_count++] = v;
						}
					}
					Con_Reportf( "Xash3D GameCube: static brushes map=%s listed=%d used=%d world DL %d of %d bytes\n",
						base, n, gc_sb_count, (int)wd_have, (int)wd_want );
				}
				Mem_Free( buf );
			}
		}
	}
	if( count ) *count = gc_sb_count;
	if( serial ) *serial = gc_sb_serial;
	return gc_sb_list;
}

qboolean GC_StaticBrushModel( int num )
{
	if( num < 1 || num >= GC_SB_MODELS )
		return false;
	return ( gc_sb_bits[num >> 3] & ( 1 << ( num & 7 ))) != 0;
}
#endif

#if XASH_GAMECUBE
/*
================
Mod_GCReleaseStudioArena

Menu route, before a map loads: the studio arena is a bump allocator that
only rewinds when every model in it is gone. Models that stayed (the pinned
view models) kept it from ever rewinding while the others were reloaded on
top, and the intro ride ended at 1020 of 1024 KB. Release all of them here;
the new map loads its own set during the LOADING hold.
================
*/
void Mod_GCReleaseStudioArena( void )
{
	const size_t before = gc_studio_bss_bump;
	model_t *mod;
	int i, freed = 0;

	/* The DoD play route needs this release too: a body model kept in a
	 * slice of the old map's arena (us-inf.mdl) survived the level change
	 * and the next map loaded its planes and nodes over it (ceres, charlie
	 * to avalanche: WORLD CANARY CHANGED parts=planes,nodes,clipnodes). */
#if !( defined( XASH_DODGC_NETWORK ) && XASH_DODGC_NETWORK )
	if( !GC_MenuRunsServer( ))
		return;
#endif
	{
		extern void R_GXActorSlotsReset( void );
		R_GXActorSlotsReset();
	}
	/* The level change has begun: no more claims against this map's arena. */
	GC_MapLoadBufferWorldSettled( false );
	gc_pinned_viewmodel_count = 0;
	memset( gc_pinned_viewmodels, 0, sizeof( gc_pinned_viewmodels ));
	for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload != NL_UNREFERENCED && mod->type == mod_studio
			&& mod->cache.data && Mod_GCIsStudioBssCache( mod->cache.data ))
		{
			/* Mod_FreeModel → UnloadRenderData hangs on the first mid-tram
			 * BSS studio (v_crowbar, probe 20261105-053602) even with
			 * texture-table bounds checks. Abandon the BSS payload and keep
			 * the mod slot (name/type) so the next map can reload it;
			 * memset'ing the slot left c0a0a silent after MAP_READY. */
			Con_Reportf( "Xash3D GameCube: studio arena abandon '%s'\n",
				mod->name[0] ? mod->name : "?" );
			mod->submodels = NULL;
			mod->cache.data = NULL;
			if( mod->mempool )
				Mem_FreePool( &mod->mempool );
			mod->needload = NL_UNREFERENCED;
			freed++;
		}
	}
	gc_real_studio_npc = 0;
	gc_real_studio_view = 0;
	gc_real_studio_bytes = 0;
	gc_deferred_studio_attempts = 0;
	gc_studio_bss_owners = 0;
	gc_studio_bss_bump = 0;
	if( gc_studio_overflow_count )
	{
		extern void Mod_GCWorldCanaryCheck( const char *when );
		Mod_GCWorldCanaryCheck( "studio overflow release" );
	}
	if( gc_studio_overflow_count )
		Con_Reportf( "Xash3D GameCube: studio overflow released slices=%d bytes=%s\n",
			gc_studio_overflow_count, Q_memprint( gc_studio_overflow_bytes ));
	gc_studio_overflow_count = 0;
	gc_studio_overflow_bytes = 0;
	/* The animation slots are in the arena too: a slot kept across this reset
	 * wrote the next animation file over the new map's skins. Two are placed
	 * first, so a full arena still has animations; the others come on demand. */
	memset( gc_seqgroup_slot, 0, sizeof( gc_seqgroup_slot ));
	Mod_GCVMSlotsReset();
	gc_seqgroup_slot[0].buf = Mod_GCStudioBssAlloc( GC_SEQGROUP_SLOT_SIZE );
	gc_seqgroup_slot[1].buf = Mod_GCStudioBssAlloc( GC_SEQGROUP_SLOT_SIZE );
	Con_Reportf( "Xash3D GameCube: studio arena released models=%d bytes=%u\n", freed, (uint)before );
	GC_AramReport( "studio-release" );
}

void Mod_GcmapMarkPrecacheFreeable( void )
{
	model_t *mod;
	int i, marked;

	if( !GC_MapLoadMemoryOpt())
		return;

	marked = 0;
	for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_UNREFERENCED )
			continue;
		if( mod->name[0] == '*' )
			continue;
		/* Never a world BSP: after a changelevel the current level need not
		 * sit in slot 0, and freeing it unpinned its live surfaces. */
		if( mod->type == mod_brush && !Q_strnicmp( mod->name, "maps/", 5 ))
			continue;
		if( mod->needload == NL_PRESENT )
		{
			/* Keep New Game deferred real studio meshes for world present.
			 * Menu New Game: the intro cast is resident in the studio arena
			 * and referenced by both sv.models and cl.models; freeing it let
			 * a later promote reuse the slot for another model. */
			if(( Sys_CheckParm( "-gcnewgame" ) || GC_MenuRunsServer()) && mod->type == mod_studio )
			{
				studiohdr_t *hdr = (studiohdr_t *)mod->cache.data;
				if( hdr && hdr->numbodyparts > 0 )
					continue;
			}
			mod->needload = NL_FREE_UNUSED;
			marked++;
		}
	}

	Mem_EmptyPool( com_studiocache );
	Mod_ClearStudioCache();
	Mod_FreeUnused();
	Con_Reportf( "Xash3D GameCube: gcmap released %d precache models for world render\n", marked );
}
#endif

/*
===============================================================================

			MODEL ROUTINES

===============================================================================
*/
/*
===============
Mod_Calloc

===============
*/
void *GAME_EXPORT Mod_Calloc( int number, size_t size )
{
	if( number <= 0 || size <= 0 )
		return NULL;

	cache_user_t *cu = (cache_user_t *)Mem_Calloc( com_studiocache, sizeof( cache_user_t ) + number * size );
	cu->data = (void *)cu; // make sure that cu->data is not NULL

	return cu;
}

/*
===============
Mod_CacheCheck

===============
*/
void *GAME_EXPORT Mod_CacheCheck( cache_user_t *c )
{
	if( !c->data )
		return NULL;

#if XASH_GAMECUBE
	/* Sequence groups loaded into the studio arena stay resident until
	 * their slot is reused (which clears c->data). */
	if( Mod_GCIsStudioBssCache( c->data ))
	{
		Mod_GCSeqGroupTouch( c->data );
		return c->data;
	}
#endif
	if( !Mem_IsAllocatedExt( com_studiocache, c->data ))
		return NULL;

	return c->data;
}

/*
===============
Mod_LoadCacheFile

===============
*/
void GAME_EXPORT Mod_LoadCacheFile( const char *filename, cache_user_t *cu )
{
	char	modname[MAX_QPATH];
	fs_offset_t	size;

	Assert( cu != NULL );

	if( COM_StringEmptyOrNULL( filename ))
		return;

	Q_strncpy( modname, filename, sizeof( modname ));
	COM_FixSlashes( modname );

	byte *buf = NULL;
#if XASH_GAMECUBE
	/* Menu New Game: the client studio renderer's sequence groups go to
	 * the studio arena like R_StudioGetAnim's (one copy, no heap). */
	if( GC_MenuRunsServer() && ( buf = Mod_GCSeqGroupLoad( modname, cu, &size )) != NULL )
		cu->data = buf;
	else
#endif
	{
	buf = FS_LoadFile( modname, &size, false );
	if( !buf || !size ) Host_Error( "LoadCacheFile: ^1can't load %s^7\n", filename );
	cu->data = Mem_Malloc( com_studiocache, size );
	memcpy( cu->data, buf, size );
	Mem_Free( buf );
	}

	// this handles when studio model renderer tries to load sequence files on it's own
	// which is what they always do in HLSDK
#if XASH_BIG_ENDIAN
	if( size >= sizeof( int ) && LittleLong( IDSEQGRPHEADER ) == *(uint *)cu->data )
	{
		studiohdr_t *phdr = (studiohdr_t *)REF_GET_PARM( PARM_GET_STUDIO_HDR, 0 );
		if( !phdr )
			return;

		mstudioseqdesc_t *pseq = (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex );

		for( int i = 0; i < phdr->numseq; i++ )
		{
			if( pseq[i].seqgroup == 0 )
				continue;

			mstudioseqgroup_t *pgrp = (mstudioseqgroup_t *)((byte *)phdr + phdr->seqgroupindex ) + pseq[i].seqgroup;

			// assuming filename passes seqgroup's name
			if( !Q_stricmp( pgrp->name, filename ))
				Mod_SwapStudioSeqGroupAnims( phdr, &pseq[i], (byte *)cu->data );
		}
	}
#endif
}

/*
==================
Mod_ValidateCRC

==================
*/
qboolean Mod_ValidateCRC( const char *name, uint32_t crc )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( !FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		return true;
	if( p->initialCRC == crc )
		return true;
	return false;
}

/*
==================
Mod_NeedCRC

==================
*/
void Mod_NeedCRC( const char *name, qboolean needCRC )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( needCRC ) SetBits( p->flags, FCRC_SHOULD_CHECKSUM );
	else ClearBits( p->flags, FCRC_SHOULD_CHECKSUM );
}

#if XASH_LLVM_LIBFUZZER

static const uint8_t *fuzz_data;
static size_t fuzz_size;

static byte *Fuzz_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	byte *buf = Mem_Malloc( host.mempool, fuzz_size );
	memcpy( buf, fuzz_data, fuzz_size );
	*filesizeptr = fuzz_size;
	return buf;
}

int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size );
int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size )
{
	model_t mod = { .name = "test", .needload = NL_NEEDS_LOADED };

	Memory_Init();

	host.type = HOST_DEDICATED;
	host.mempool = Mem_AllocPool( "fuzzing pool" );
	fuzz_data = Data;
	fuzz_size = Size;
	refState.draw_surfaces = NULL;

	g_fsapi.LoadFile = Fuzz_LoadFile;

	if( Mod_LoadModel( &mod, false ) && mod.mempool )
		Mem_FreePool( &mod.mempool );

	Mem_FreePool( &host.mempool );

	return 0;
}

#endif // XASH_LLVM_LIBFUZZER
