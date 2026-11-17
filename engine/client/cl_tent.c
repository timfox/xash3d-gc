/*
cl_tent.c - temp entity effects management
Copyright (C) 2009 Uncle Mike

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
#include "client.h"
#include "r_efx.h"
#include "entity_types.h"
#include "triangleapi.h"
#include "cl_tent.h"
#include "pm_local.h"
#include "studio.h"
#include "wadfile.h"	// acess decal size
#include "sound.h"
#if XASH_GAMECUBE
#include "server.h"

/*
 * The lean DoD listen route never loads the server's precached sprites into
 * cl.models (only the studio props the lean emit draws), so every temp
 * entity a TE message made had no model and was dropped (fx diag on charlie:
 * nomodel == alloc). Resolve a missing index from the server precache list
 * on first use: sprites load as client sprites, studio models come from the
 * server's resident handle. A failed index is retried after 10 s.
 * Sprites load with smaller frames (gc_sprite_effect_load, cl_sprite.c) and
 * only while 80 KiB of MEM1 stays free, so effects never take the memory
 * the HUD and the view model skins need.
 */
extern qboolean gc_sprite_effect_load;
extern size_t GC_MemHeapFreeBytes( void );
static model_t *CL_GCLoadEffectSprite( const char *name )
{
	model_t *mod;

	/* Leave room for HUD sheets that load mid-match (the 128x128
	 * crosshair needs 32 KiB). */
	if( GC_MemHeapFreeBytes() < 80 * 1024 )
		return NULL;
	gc_sprite_effect_load = true;
	mod = CL_LoadClientSprite( name );
	gc_sprite_effect_load = false;
	return mod;
}

static model_t *CL_GCTentModelHandle( int modelindex )
{
	static double retry_at[MAX_MODELS];
	static const void *tried_world;
	static int fail_logs;
	model_t *mod = ( modelindex >= 0 && modelindex < MAX_MODELS ) ? cl.models[modelindex] : NULL;
	const char *name;

	if( mod || modelindex <= 0 || modelindex >= MAX_MODELS || !SV_Active( ))
		return mod;
	if( tried_world != (const void *)cl.worldmodel )
	{
		memset( retry_at, 0, sizeof( retry_at ));
		tried_world = cl.worldmodel;
	}
	if( host.realtime < retry_at[modelindex] )
		return NULL;
	retry_at[modelindex] = host.realtime + 10.0;
	name = sv.model_precache[modelindex];
	if( !name[0] )
		return NULL;
	if( Q_stristr( name, ".spr" ))
		mod = CL_GCLoadEffectSprite( name );
	else
	{
		mod = SV_ModelHandle( modelindex );
		if( mod && ( mod->needload != NL_PRESENT || !mod->cache.data ) && mod->type != mod_brush )
			mod = NULL;
	}
	if( mod )
		cl.models[modelindex] = mod;
	else if( fail_logs++ < 8 )
		Con_Reportf( "Xash3D GameCube: temp entity model %d %s not loadable (on disc %d, heap %s)\n",
			modelindex, name, FS_FileExists( name, false ), Q_memprint( GC_MemHeapFreeBytes( )));
	return mod;
}
#define CL_ModelHandle CL_GCTentModelHandle

/*
 * Effect sprites were only loaded on first use, and by then a match has
 * handed the free heap to resident textures and models (dod_charlie: 354 KiB
 * free at spawn, 22 KiB a little later), so explosion, steam and blood
 * sprites failed the 80 KiB gate above for the whole map. Load the server's
 * precached sprites right after the map goes active instead, one per frame,
 * while 96 KiB stays free. Measured on charlie, which has 126 KiB free
 * when the client goes active: a 128 KiB floor loaded nothing; a 64 KiB
 * floor loaded four sprites (68 KiB) but the grenade view model skins then
 * failed to allocate.
 */
static void CL_GCPrewarmEffectSprites( void )
{
	static const void *world;
	static int next, loaded;
	static size_t start_free;
	static qboolean done;

	if( !SV_Active() || cls.state != ca_active || !cl.worldmodel )
		return;
	if( world != (const void *)cl.worldmodel )
	{
		world = cl.worldmodel;
		next = 1;
		loaded = 0;
		start_free = 0;
		done = false;
	}
	if( done )
		return;
	for( ; next < MAX_MODELS; next++ )
	{
		const char *name = sv.model_precache[next];
		model_t *mod;

		if( !name[0] )
			break;
		if( cl.models[next] || !Q_stristr( name, ".spr" ))
			continue;
		/* A match uses every free byte later, so the floor alone protects
		 * nothing: with 112 KiB more heap the prewarm took a second sprite
		 * and the grenade view model skins failed again. Cap what the
		 * prewarm itself takes (the explosion sprite is 32 KiB). */
		if( !start_free )
			start_free = GC_MemHeapFreeBytes();
		if( GC_MemHeapFreeBytes() < 96 * 1024
			|| ( start_free > GC_MemHeapFreeBytes()
				&& start_free - GC_MemHeapFreeBytes() >= 24 * 1024 ))
		{
			done = true;
			Con_Reportf( "Xash3D GameCube: effect sprites prewarmed %d, stopped at %s (heap %s)\n",
				loaded, name, Q_memprint( GC_MemHeapFreeBytes( )));
			return;
		}
		gc_sprite_effect_load = true;
		mod = CL_LoadClientSprite( name );
		gc_sprite_effect_load = false;
		if( mod )
		{
			cl.models[next] = mod;
			loaded++;
		}
		next++;
		return; /* one sprite a frame: no load hitch */
	}
	done = true;
	Con_Reportf( "Xash3D GameCube: effect sprites prewarmed %d (heap %s)\n",
		loaded, Q_memprint( GC_MemHeapFreeBytes( )));
}
#endif
#if XASH_GAMECUBE
#include "gamecube/mem_gamecube.h"
#endif

/*
==============================================================

TEMPENTS MANAGEMENT

==============================================================
*/
#define FLASHLIGHT_DISTANCE		2000	// in units
#define SHARD_VOLUME		12.0f	// on shard ever n^3 units
#define MAX_MUZZLEFLASH		3

static TEMPENTITY *cl_active_tents;
static TEMPENTITY *cl_free_tents;
static TEMPENTITY *cl_tempents = NULL;		// entities pool

static model_t *cl_sprite_muzzleflash[MAX_MUZZLEFLASH];	// muzzle flashes
static model_t *cl_sprite_ricochet = NULL;
static model_t *cl_sprite_glow = NULL;
model_t        *cl_sprite_dot = NULL;
model_t        *cl_sprite_shell = NULL;

static const char *const cl_default_sprites[] =
{
	// built-in sprites
	"sprites/muzzleflash1.spr",
	"sprites/muzzleflash2.spr",
	"sprites/muzzleflash3.spr",
	"sprites/dot.spr",
	"sprites/animglow01.spr",
	"sprites/richo1.spr",
	"sprites/shellchrome.spr",
};

static void CL_PlayerDecal( int playerIndex, int textureIndex, int entityIndex, float *pos );

/*
================
CL_LoadClientSprites

INTERNAL RESOURCE
================
*/
void CL_LoadClientSprites( void )
{
	cl_sprite_muzzleflash[0] = CL_LoadClientSprite( cl_default_sprites[0] );
	cl_sprite_muzzleflash[1] = CL_LoadClientSprite( cl_default_sprites[1] );
	cl_sprite_muzzleflash[2] = CL_LoadClientSprite( cl_default_sprites[2] );

	cl_sprite_dot = CL_LoadClientSprite( cl_default_sprites[3] );
	cl_sprite_glow = CL_LoadClientSprite( cl_default_sprites[4] );
	cl_sprite_ricochet = CL_LoadClientSprite( cl_default_sprites[5] );
	cl_sprite_shell = CL_LoadClientSprite( cl_default_sprites[6] );
}

/*
================
CL_AddClientResource

add client-side resource to list
================
*/
void CL_AddClientResource( const char *filename, int type )
{
	resource_t	*p, *pResource;

	for( p = cl.resourcesneeded.pNext; p != &cl.resourcesneeded; p = p->pNext )
	{
		if( !Q_stricmp( p->szFileName, filename ))
			break;
	}

	if( p != &cl.resourcesneeded )
		return; // already in list?

	pResource = Mem_Calloc( cls.mempool, sizeof( resource_t ));

	Q_strncpy( pResource->szFileName, filename, sizeof( pResource->szFileName ));
	pResource->type = type;
	pResource->nIndex = -1; // client resource marker
	pResource->nDownloadSize = 1;
	pResource->ucFlags |= RES_WASMISSING;

	CL_AddToResourceList( pResource, &cl.resourcesneeded );
}

/*
================
CL_AddClientResources

client resources not precached by server
================
*/
void CL_AddClientResources( void )
{
	const char *snd;
	char	filepath[MAX_QPATH];

	// don't request resources from localhost or in quake-compatibility mode
	if( cl.maxclients <= 1 || Host_IsQuakeCompatible( ))
		return;

	// check sprites first
	for( int i = 0; i < ARRAYSIZE( cl_default_sprites ); i++ )
	{
		if( !FS_FileExists( cl_default_sprites[i], false ))
			CL_AddClientResource( cl_default_sprites[i], t_model );
	}

	// then check sounds
	for( int i = 0; ( snd = SoundList_Get( BouncePlayerShell, i )); i++ )
	{
		Q_snprintf( filepath, sizeof( filepath ), DEFAULT_SOUNDPATH "%s", snd );

		if( !FS_FileExists( filepath, false ))
			CL_AddClientResource( snd, t_sound );
	}

	for( int i = 0; ( snd = SoundList_Get( BounceWeaponShell, i )); i++ )
	{
		Q_snprintf( filepath, sizeof( filepath ), DEFAULT_SOUNDPATH "%s", snd );

		if( !FS_FileExists( filepath, false ))
			CL_AddClientResource( snd, t_sound );
	}

	for( int i = 0; ( snd = SoundList_Get( Explode, i )); i++ )
	{
		Q_snprintf( filepath, sizeof( filepath ), DEFAULT_SOUNDPATH "%s", snd );

		if( !FS_FileExists( filepath, false ))
			CL_AddClientResource( snd, t_sound );
	}

#if 0	// ric sounds was precached by server-side
	for( int i = 0; ( snd = SoundList_Get( Ricochet, i )); i++ )
	{
		Q_snprintf( filepath, sizeof( filepath ), DEFAULT_SOUNDPATH "%s", snd );

		if( !FS_FileExists( filepath, false ))
			CL_AddClientResource( snd, t_sound );
	}
#endif
}

/*
================
CL_ClearTempEnts

================
*/
#if XASH_GAMECUBE
/* Entries actually allocated in cl_tempents (CL_InitTempEnts caps the pool
 * below GI->max_tents). Linking GI->max_tents entries wrote free-list
 * pointers past the end of a 4-entry allocation. */
static int cl_tempents_count;
static qboolean gc_tempents_in_spill;
/* The lean emit (cl_frame.c) clears the scene after CL_EmitEntities has
 * linked the temp entities, so it links them again from this list. */
#define GC_TENT_RELINK_MAX 96
static cl_entity_t *gc_tent_linked[GC_TENT_RELINK_MAX];
static int gc_tent_linked_n;
static int gc_tent_update_frame = -1;
/* The pool came from CL_GCEnsureTempEntPool (lean route), which also owns
 * reloading the effect sprites; CL_InitTempEnts' route loads them once. */
static qboolean gc_tempents_lean_pool;
unsigned gc_fx_tent_alloc, gc_fx_tent_overflow, gc_fx_tent_nomodel, gc_fx_te_parsed;
#endif

static void CL_ClearTempEnts( void )
{
	int max_tents = GI->max_tents;
#if XASH_GAMECUBE
	if( Sys_CheckParm( "-gcmap" ))
		max_tents = Q_min( max_tents, 4 );
	if( cl_tempents_count > 0 )
		max_tents = Q_min( max_tents, cl_tempents_count );
#endif

#if XASH_GAMECUBE
	/* A new world (or a reconnect): the relink list points into old temp
	 * entities, and CL_ClearSpriteTextures may free the effect sprites once
	 * the next map loads. Look them up again in CL_GCEnsureTempEntPool. */
	gc_tent_linked_n = 0;
	gc_tent_update_frame = -1;
	if( gc_tempents_lean_pool )
	{
		cl_sprite_muzzleflash[0] = cl_sprite_muzzleflash[1] = cl_sprite_muzzleflash[2] = NULL;
		cl_sprite_ricochet = NULL;
	}

	/* A spill pool (CL_GCEnsureTempEntPool) may already be rewound for the
	 * next map: drop it rather than write the free list into new data. */
	if( cl_tempents && gc_tempents_in_spill )
	{
		cl_tempents = NULL;
		cl_tempents_count = 0;
		gc_tempents_in_spill = false;
		cl_free_tents = cl_active_tents = NULL;
		return;
	}
#endif
	if( !cl_tempents ) return;

	for( int i = 0; i < max_tents - 1; i++ )
	{
		cl_tempents[i].next = &cl_tempents[i+1];
		cl_tempents[i].entity.trivial_accept = INVALID_HANDLE;
	}

	cl_tempents[max_tents-1].next = NULL;
	cl_free_tents = cl_tempents;
	cl_active_tents = NULL;
}

/*
================
CL_InitTempents

================
*/
void CL_InitTempEnts( void )
{
	int max_tents = GI->max_tents;
#if XASH_GAMECUBE
	/* Keep the disconnected retail menu lean; gameplay effects get the larger
	 * pool once the client is active. */
	/* The client boot phase is published before this pool is created, so it is
	 * not a reliable discriminator for the disconnected retail menu. Keep the
	 * GameCube pool lean at allocation time; the normal desktop-sized pool is
	 * not viable alongside the resident mainui module in MEM1. */
	max_tents = Q_min( max_tents, 4 );
#endif

	cl_tempents = Mem_Calloc( cls.mempool, sizeof( TEMPENTITY ) * max_tents );
#if XASH_GAMECUBE
	cl_tempents_count = cl_tempents ? max_tents : 0;
#endif
	CL_ClearTempEnts();

	// load tempent sprites (glowshell, muzzleflashes etc)
	#if XASH_GAMECUBE
	if( !cl.worldmodel )
	{
		/* The retail startup cinematic/menu has no world effects to draw yet;
		 * defer sprite precaching until the first map supplies a world model. */
		Con_Reportf( "Xash3D GameCube: client tempent sprites deferred until world\n" );
		return;
	}
	#endif
	CL_LoadClientSprites ();
}

#if XASH_GAMECUBE
/* Muzzle flashes and ricochet sparks (the lean route skips
 * CL_LoadClientSprites). Logs once per failure with whether the file is on
 * the disc at all. */
static void CL_GCLoadEffectSprites( void )
{
	static int fail_logs;

	if( !cl_sprite_muzzleflash[0] )
	{
		cl_sprite_muzzleflash[0] = CL_GCLoadEffectSprite( cl_default_sprites[0] );
		cl_sprite_muzzleflash[1] = CL_GCLoadEffectSprite( cl_default_sprites[1] );
		cl_sprite_muzzleflash[2] = CL_GCLoadEffectSprite( cl_default_sprites[2] );
	}
	if( !cl_sprite_ricochet )	/* TE_GUNSHOTDECAL bullet impacts */
		cl_sprite_ricochet = CL_GCLoadEffectSprite( cl_default_sprites[5] );
	if(( !cl_sprite_muzzleflash[0] || !cl_sprite_ricochet ) && fail_logs < 4 )
	{
		fail_logs++;
		Con_Reportf( "Xash3D GameCube: client sprites: %s %s, %s %s (on disc: %d %d)\n",
			cl_default_sprites[0], cl_sprite_muzzleflash[0] ? "ok" : "failed",
			cl_default_sprites[5], cl_sprite_ricochet ? "ok" : "failed",
			FS_FileExists( cl_default_sprites[0], false ), FS_FileExists( cl_default_sprites[5], false ));
	}
}

/*
================
CL_GCEnsureTempEntPool

DoD's lean listen route never runs CL_InitTempEnts (MEM1 is too tight at
client init), so every temp entity was refused: no muzzle flash, smoke,
explosion, ricochet or shell. Once a map is up, give the pool 16 entries
(~2 KiB each): from the heap when it keeps a 64 KiB margin, else from the
world-array spill, which is rewound per map, so it is redone per world.
Also loads the muzzle flash sprites the route skipped.
================
*/
void CL_GCEnsureTempEntPool( void )
{
	extern size_t GC_MemHeapFreeBytes( void );
	extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
	extern void Mod_GCWorldArraySpillPinBase( const void *ptr, size_t bytes );
	extern qboolean Mod_GCWorldArraySpillHolds( const void *ptr, size_t bytes );
	static double next_try, next_sprite_try;
	const int want = 16;
	const size_t bytes = sizeof( TEMPENTITY ) * want;
	const char *where = "heap";
	int i;

	/* A spill pool dies with its map: drop it once the spill was rewound. */
	if( cl_tempents && gc_tempents_in_spill
		&& !Mod_GCWorldArraySpillHolds( cl_tempents, sizeof( TEMPENTITY ) * cl_tempents_count ))
	{
		cl_tempents = NULL;
		cl_tempents_count = 0;
		cl_free_tents = cl_active_tents = NULL;
		gc_tempents_in_spill = false;
	}
	if( !cl.worldmodel )
		return;
	if( cl_tempents )
	{
		/* The pool outlives a changelevel; the sprites do not. */
		if(( !cl_sprite_muzzleflash[0] || !cl_sprite_ricochet ) && host.realtime >= next_sprite_try )
		{
			next_sprite_try = host.realtime + 5.0;
			CL_GCLoadEffectSprites();
		}
		return;
	}
	if( host.realtime < next_try )
		return;
	next_try = host.realtime + 5.0;
	/* World spill first: 27 KiB of heap for the pool left the view model
	 * and the crosshair without texture memory (dark rifle, crosshair OOM
	 * on charlie). The heap only when it is plentiful. */
	cl_tempents = Mod_GCWorldArraySpillTryAlloc( bytes, "client tempents" );
	if( cl_tempents )
	{
		memset( cl_tempents, 0, bytes );
		Mod_GCWorldArraySpillPinBase( cl_tempents, bytes );
		gc_tempents_in_spill = true;
		where = "spill";
	}
	else if( GC_MemHeapFreeBytes() > bytes + 256 * 1024 )
		cl_tempents = Mem_Calloc( cls.mempool, bytes );
	if( !cl_tempents )
	{
		static int fail_logs;

		cl_free_tents = cl_active_tents = NULL;
		if( fail_logs++ < 3 )
			Con_Reportf( "Xash3D GameCube: client tempents unavailable (%s needed, heap %s)\n",
				Q_memprint( bytes ), Q_memprint( GC_MemHeapFreeBytes( )));
		return;
	}
	cl_tempents_count = want;
	for( i = 0; i < want - 1; i++ )
	{
		cl_tempents[i].next = &cl_tempents[i + 1];
		cl_tempents[i].entity.trivial_accept = INVALID_HANDLE;
	}
	cl_tempents[want - 1].next = NULL;
	cl_tempents[want - 1].entity.trivial_accept = INVALID_HANDLE;
	cl_free_tents = cl_tempents;
	cl_active_tents = NULL;
	gc_tempents_lean_pool = true;
	next_sprite_try = host.realtime + 5.0;
	CL_GCLoadEffectSprites();
	Con_Reportf( "Xash3D GameCube: client tempents %d in %s (%s), muzzle flash %s, heap %s\n",
		want, where, Q_memprint( bytes ), cl_sprite_muzzleflash[0] ? "loaded" : "missing",
		Q_memprint( GC_MemHeapFreeBytes( )));
}
#endif

/*
================
CL_FreeTempEnts

================
*/
void CL_FreeTempEnts( void )
{
#if XASH_GAMECUBE
	/* A spill pool belongs to the map arena, not the mempool. */
	if( cl_tempents && !gc_tempents_in_spill )
		Mem_Free( cl_tempents );
	cl_tempents_count = 0;
	gc_tempents_in_spill = false;
#else
	if( cl_tempents )
		Mem_Free( cl_tempents );
#endif
	cl_tempents = NULL;
}

/*
==============
CL_PrepareTEnt

set default values
==============
*/
static void CL_PrepareTEnt( TEMPENTITY *pTemp, model_t *pmodel )
{
	int	frameCount = 0;
	int	modelIndex = 0;
	int	modelHandle = pTemp->entity.trivial_accept;

	memset( pTemp, 0, sizeof( *pTemp ));

	// use these to set per-frame and termination conditions / actions
	pTemp->entity.trivial_accept = modelHandle; // keep unchanged
	pTemp->flags = FTENT_NONE;
	pTemp->die = cl.time + 0.75f;

	if( pmodel ) frameCount = pmodel->numframes;
	else pTemp->flags |= FTENT_NOMODEL;

	pTemp->entity.curstate.modelindex = modelIndex;
	pTemp->entity.curstate.rendermode = kRenderNormal;
	pTemp->entity.curstate.renderfx = kRenderFxNone;
	pTemp->entity.curstate.rendercolor.r = 255;
	pTemp->entity.curstate.rendercolor.g = 255;
	pTemp->entity.curstate.rendercolor.b = 255;
	pTemp->frameMax = Q_max( 0, frameCount - 1 );
	pTemp->entity.curstate.renderamt = 255;
	pTemp->entity.curstate.body = 0;
	pTemp->entity.curstate.skin = 0;
	pTemp->entity.model = pmodel;
	pTemp->fadeSpeed = 0.5f;
	pTemp->hitSound = 0;
	pTemp->clientIndex = 0;
	pTemp->bounceFactor = 1;
	pTemp->entity.curstate.scale = 1.0f;
}

/*
==============
CL_TempEntPlaySound

play collide sound
==============
*/
static void CL_TempEntPlaySound( TEMPENTITY *pTemp, float damp )
{
	const char	*soundname = NULL;
	qboolean	isshellcasing = false;

	Assert( pTemp != NULL );

	float fvol = 0.8f;

	switch( pTemp->hitSound )
	{
	case BOUNCE_GLASS:
		soundname = SoundList_GetRandom( BounceGlass );
		break;
	case BOUNCE_METAL:
		soundname = SoundList_GetRandom( BounceMetal );
		break;
	case BOUNCE_FLESH:
		soundname = SoundList_GetRandom( BounceFlesh );
		break;
	case BOUNCE_WOOD:
		soundname = SoundList_GetRandom( BounceWood );
		break;
	case BOUNCE_SHRAP:
#if XASH_GAMECUBE
		if( GC_MapLoadMemoryOpt())
			soundname = "weapons/ric1.wav";
		else
#endif
		soundname = SoundList_GetRandom( Ricochet );
		break;
	case BOUNCE_SHOTSHELL:
		soundname = SoundList_GetRandom( BounceWeaponShell );
		isshellcasing = true; // shell casings have different playback parameters
		fvol = 0.5f;
		break;
	case BOUNCE_SHELL:
		soundname = SoundList_GetRandom( BouncePlayerShell );
		isshellcasing = true; // shell casings have different playback parameters
		break;
	case BOUNCE_CONCRETE:
		soundname = SoundList_GetRandom( BounceConcrete );
		break;
	default:	// null sound
		return;
	}

	if( !soundname )
		return;

	int zvel = abs( pTemp->entity.baseline.origin[2] );

	// only play one out of every n
	if( isshellcasing )
	{
		// play first bounce, then 1 out of 3
		if( zvel < 200 && COM_RandomLong( 0, 3 ))
			return;
	}
	else
	{
		if( COM_RandomLong( 0, 5 ))
			return;
	}

	if( damp > 0.0f )
	{
		int	pitch;

		if( isshellcasing )
			fvol *= Q_min( 1.0f, ((float)zvel) / 350.0f );
		else fvol *= Q_min( 1.0f, ((float)zvel) / 450.0f );

		if( !COM_RandomLong( 0, 3 ) && !isshellcasing )
			pitch = COM_RandomLong( 95, 105 );
		else pitch = PITCH_NORM;

		sound_t handle = S_RegisterSound( soundname );
		S_StartSound( pTemp->entity.origin, -(pTemp - cl_tempents), CHAN_BODY, handle, fvol, ATTN_NORM, pitch, SND_STOP_LOOPING );
	}
}

/*
==============
CL_TEntAddEntity

add entity to renderlist
==============
*/
static int CL_TempEntAddEntity( cl_entity_t *pEntity )
{
	vec3_t mins, maxs;

	Assert( pEntity != NULL );

	if( !pEntity->model )
		return 0;

	VectorAdd( pEntity->origin, pEntity->model->mins, mins );
	VectorAdd( pEntity->origin, pEntity->model->maxs, maxs );

	// g-cont. just use PVS from previous frame
	if( TriBoxInPVS( mins, maxs ))
	{
		VectorCopy( pEntity->angles, pEntity->curstate.angles );
		VectorCopy( pEntity->origin, pEntity->curstate.origin );
		VectorCopy( pEntity->angles, pEntity->latched.prevangles );
		VectorCopy( pEntity->origin, pEntity->latched.prevorigin );

		// add to list
		CL_AddVisibleEntity( pEntity, ET_TEMPENTITY );
#if XASH_GAMECUBE
		if( gc_tent_linked_n < GC_TENT_RELINK_MAX )
			gc_tent_linked[gc_tent_linked_n++] = pEntity;
#endif

		return 1;
	}

	return 0;
}

/*
==============
CL_AddTempEnts

temp-entities will be added on a user-side
setup client callback
==============
*/
void CL_TempEntUpdate( void )
{
	double	ft = cl.time - cl.oldtime;
	float	gravity = clgame.movevars.gravity;

#if XASH_GAMECUBE
	gc_tent_linked_n = 0;
	gc_tent_update_frame = (int)host.framecount;
	CL_GCPrewarmEffectSprites();
#endif
	clgame.dllFuncs.pfnTempEntUpdate( ft, cl.time, gravity, &cl_free_tents, &cl_active_tents, CL_TempEntAddEntity, CL_TempEntPlaySound );
}

#if XASH_GAMECUBE
/*
==============
CL_GCRelinkTempEnts

The lean emit clears the scene CL_EmitEntities filled. Link this frame's
temp entities (muzzle flashes, smoke and explosion sprites, shells, gibs)
again; if the temp entities were not updated this frame at all, run the
events and the update here. Returns how many were linked.
==============
*/
int CL_GCRelinkTempEnts( void )
{
	static int logs;
	static convar_t *gc_fxtest;
	static double next_fxtest;
	int i, ran = 0;

	CL_GCEnsureTempEntPool();
	/* gc_fxtest 1: a muzzle flash sprite 64 units in front of the view every
	 * second, living a second, so a capture can prove the sprite path draws
	 * without having to catch a 0.1 s flash. */
	if( !gc_fxtest )
		gc_fxtest = Cvar_Get( "gc_fxtest", "0", 0, "GameCube test: 1 shows a muzzle flash sprite in front of the view every second" );
	if( gc_fxtest && gc_fxtest->value && cl_tempents && cl_sprite_muzzleflash[0] && host.realtime >= next_fxtest )
	{
		static int fx_logs;
		vec3_t forward, org;
		TEMPENTITY *t;

		next_fxtest = host.realtime + 1.0;
		AngleVectors( cl.viewangles, forward, NULL, NULL );
		VectorMA( refState.vieworg, 64.0f, forward, org );
		t = CL_TempEntAlloc( org, cl_sprite_muzzleflash[0] );
		if( t )
		{
			t->entity.curstate.rendermode = kRenderTransAdd;
			t->entity.curstate.renderamt = 255;
			t->entity.curstate.scale = 0.5f;
			t->die = cl.time + 1.0f;
		}
		if( fx_logs++ < 4 )
			Con_Reportf( "Xash3D GameCube: fx test sprite %s at (%.0f %.0f %.0f)\n",
				t ? "spawned" : "refused", org[0], org[1], org[2] );
	}
	if( gc_tent_update_frame != (int)host.framecount )
	{
		CL_FireEvents();
		CL_TempEntUpdate();
		ran = 1;
	}
	else
	{
		for( i = 0; i < gc_tent_linked_n; i++ )
			CL_AddVisibleEntity( gc_tent_linked[i], ET_TEMPENTITY );
	}
	{
		/* Every 5 s: why effects do or do not show on this route. */
		extern unsigned gc_fx_particles_alloc, gc_fx_particles_noframe, gc_fx_events_fired;
		static double next_diag;
		TEMPENTITY *t;
		int active = 0, nomodel = 0;

		if( host.realtime >= next_diag )
		{
			next_diag = host.realtime + 5.0;
			for( t = cl_active_tents; t && active < 1024; t = t->next )
			{
				active++;
				if( !t->entity.model )
					nomodel++;
			}
			Con_Reportf( "Xash3D GameCube: fx diag cl.time=%.3f dt=%.4f pool=%d active=%d (no model %d) linked=%d %s; "
				"tents alloc=%u overflow=%u nomodel=%u; TE=%u events=%u; particles alloc=%u refused(dt=0)=%u; muzzle sprite=%s\n",
				cl.time, cl.time - cl.oldtime, cl_tempents_count, active, nomodel, gc_tent_linked_n,
				ran ? "updated here" : "relinked", gc_fx_tent_alloc, gc_fx_tent_overflow, gc_fx_tent_nomodel,
				gc_fx_te_parsed, gc_fx_events_fired, gc_fx_particles_alloc, gc_fx_particles_noframe,
				cl_sprite_muzzleflash[0] ? "loaded" : "missing" );
			gc_fx_tent_alloc = gc_fx_tent_overflow = gc_fx_tent_nomodel = gc_fx_te_parsed = 0;
			gc_fx_events_fired = gc_fx_particles_alloc = gc_fx_particles_noframe = 0;
		}
	}
	if( gc_tent_linked_n > 0 && logs < 6 )
	{
		logs++;
		Con_Reportf( "Xash3D GameCube: lean temp entities linked %d (%s)\n",
			gc_tent_linked_n, ran ? "updated here" : "relinked" );
	}
	return gc_tent_linked_n;
}
#endif

/*
==============
CL_TEntAddEntity

free the first low priority tempent it finds.
==============
*/
static qboolean CL_FreeLowPriorityTempEnt( void )
{
	TEMPENTITY	*pActive = cl_active_tents;
	TEMPENTITY	*pPrev = NULL;

	while( pActive )
	{
		if( pActive->priority == TENTPRIORITY_LOW )
		{
			// remove from the active list.
			if( pPrev ) pPrev->next = pActive->next;
			else cl_active_tents = pActive->next;

			// add to the free list.
			pActive->next = cl_free_tents;
			cl_free_tents = pActive;

			return true;
		}

		pPrev = pActive;
		pActive = pActive->next;
	}

	return false;
}

/*
==============
CL_TempEntAlloc

alloc normal\low priority tempentity
==============
*/
TEMPENTITY *CL_TempEntAlloc( const vec3_t org, model_t *pmodel )
{
	static float cl_lasttimewarn;
	TEMPENTITY	*pTemp;

	if( !cl_free_tents )
	{
#if XASH_GAMECUBE
		gc_fx_tent_overflow++;
#endif
		if( cl_lasttimewarn < host.realtime )
		{
			Con_DPrintf( "Overflow %d temporary ents!\n", GI->max_tents );
			cl_lasttimewarn = host.realtime + 1.0f;
		}
		return NULL;
	}

	pTemp = cl_free_tents;
	cl_free_tents = pTemp->next;

	CL_PrepareTEnt( pTemp, pmodel );
#if XASH_GAMECUBE
	gc_fx_tent_alloc++;
	if( !pmodel )
		gc_fx_tent_nomodel++;
#endif

	pTemp->priority = TENTPRIORITY_LOW;
	if( org ) VectorCopy( org, pTemp->entity.origin );

	pTemp->next = cl_active_tents;
	cl_active_tents = pTemp;

	return pTemp;
}

/*
==============
CL_TempEntAllocHigh

alloc high priority tempentity
==============
*/
TEMPENTITY *CL_TempEntAllocHigh( const vec3_t org, model_t *pmodel )
{
	TEMPENTITY	*pTemp;

	if( !cl_free_tents )
	{
		// no temporary ents free, so find the first active low-priority temp ent
		// and overwrite it.
		CL_FreeLowPriorityTempEnt();
	}

	if( !cl_free_tents )
	{
		// didn't find anything? The tent list is either full of high-priority tents
		// or all tents in the list are still due to live for > 10 seconds.
		Con_DPrintf( "Couldn't alloc a high priority TENT!\n" );
		return NULL;
	}

	// Move out of the free list and into the active list.
	pTemp = cl_free_tents;
	cl_free_tents = pTemp->next;

	CL_PrepareTEnt( pTemp, pmodel );

	pTemp->priority = TENTPRIORITY_HIGH;
	if( org ) VectorCopy( org, pTemp->entity.origin );

	pTemp->next = cl_active_tents;
	cl_active_tents = pTemp;

	return pTemp;
}

/*
==============
CL_TempEntAlloc

alloc normal priority tempentity with no model
==============
*/
TEMPENTITY *CL_TempEntAllocNoModel( const vec3_t org )
{
	return CL_TempEntAlloc( org, NULL );
}

/*
==============
CL_TempEntAlloc

custom tempentity allocation
==============
*/
TEMPENTITY * GAME_EXPORT CL_TempEntAllocCustom( const vec3_t org, model_t *model, int high, void (*pfn)( TEMPENTITY*, float, float ))
{
	TEMPENTITY	*pTemp;

	if( high )
	{
		pTemp = CL_TempEntAllocHigh( org, model );
	}
	else
	{
		pTemp = CL_TempEntAlloc( org, model );
	}

	if( pTemp && pfn )
	{
		pTemp->flags |= FTENT_CLIENTCUSTOM;
		pTemp->callback = pfn;
		pTemp->die = cl.time;
	}

	return pTemp;
}

/*
==============================================================

	EFFECTS BASED ON TEMPENTS (presets)

==============================================================
*/
/*
==============
R_FizzEffect

Create a fizz effect
==============
*/
void GAME_EXPORT R_FizzEffect( cl_entity_t *ent, int modelIndex, int density )
{
	const float base_time = cl.time - 0.1f;
	model_t	*mod = CL_ModelHandle( modelIndex );
	vec3_t  volume;
	vec2_t  speed;

	if( !ent || !ent->model || !modelIndex || !mod )
		return;

	vec3_t mins = Vec3( ent->model->mins );
	vec3_t maxs = Vec3( ent->model->maxs );

	if( ent->angles[1] != 0.0f )
	{
		const float base_speed = ( ent->curstate.rendercolor.b ? -1.0f : 1.0f ) * ( ent->curstate.rendercolor.r * 256.0f + ent->curstate.rendercolor.g );
		SinCos( DEG2RAD( ent->angles[1] ), &speed[1], &speed[0] );
		speed[0] *= base_speed;
		speed[1] *= base_speed;
	}
	else speed[0] = speed[1] = 0.0f;

	VectorSubtract( maxs, mins, volume );

	for( int i = 0; i <= density; i++ )
	{
		TEMPENTITY *tent;
		vec3_t origin = Vec3( mins );

		origin[0] += COM_RandomLong( 0, (int)volume[0] - 1 );
		origin[1] += COM_RandomLong( 0, (int)volume[1] - 1 );

		if( !( tent = CL_TempEntAlloc( origin, mod )))
			return;

		tent->x = origin[0];
		tent->y = origin[1];
		tent->die = base_time;
		tent->flags |= FTENT_SINEWAVE;
		tent->entity.curstate.rendermode = kRenderTransAlpha;
		Vector2Copy( speed, tent->entity.baseline.origin );

		tent->entity.baseline.origin[2] = COM_RandomLong( 80, 140 );
		tent->die += volume[2] / tent->entity.baseline.origin[2];
		tent->entity.curstate.frame = COM_RandomLong( 0, tent->frameMax );
		tent->entity.curstate.scale = 1.0f / COM_RandomFloat( 2.0f, 5.0f );
	}
}

/*
==============
R_Bubbles

Create bubbles
==============
*/
void GAME_EXPORT R_Bubbles( const vec3_t mins, const vec3_t maxs, float height, int modelIndex, int count, float speed )
{
	TEMPENTITY	*pTemp;
	vec3_t		origin;
	model_t		*mod;

	if(( mod = CL_ModelHandle( modelIndex )) == NULL )
		return;

	for( int i = 0; i < count; i++ )
	{
		origin[0] = COM_RandomLong( mins[0], maxs[0] );
		origin[1] = COM_RandomLong( mins[1], maxs[1] );
		origin[2] = COM_RandomLong( mins[2], maxs[2] );
		pTemp = CL_TempEntAlloc( origin, mod );
		if( !pTemp ) return;

		pTemp->flags |= FTENT_SINEWAVE;

		pTemp->x = origin[0];
		pTemp->y = origin[1];
		float sine, cosine;
		float angle = COM_RandomFloat( -M_PI, M_PI );
		SinCos( angle, &sine, &cosine );

		float zspeed = COM_RandomLong( 80, 140 );
		VectorSet( pTemp->entity.baseline.origin, speed * cosine, speed * sine, zspeed );
		pTemp->die = cl.time + ((height - (origin[2] - mins[2])) / zspeed) - 0.1f;
		pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );

		// Set sprite scale
		pTemp->entity.curstate.scale = 1.0f / COM_RandomFloat( 2.0f, 5.0f );
		pTemp->entity.curstate.rendermode = kRenderTransAlpha;
		pTemp->entity.curstate.renderamt = 255;
	}
}

/*
==============
R_BubbleTrail

Create bubble trail
==============
*/
void GAME_EXPORT R_BubbleTrail( const vec3_t start, const vec3_t end, float height, int modelIndex, int count, float speed )
{
	TEMPENTITY	*pTemp;
	vec3_t		origin;
	model_t		*mod;

	if(( mod = CL_ModelHandle( modelIndex )) == NULL )
		return;

	for( int i = 0; i < count; i++ )
	{
		float dist = COM_RandomFloat( 0, 1.0 );
		VectorLerp( start, dist, end, origin );
		pTemp = CL_TempEntAlloc( origin, mod );
		if( !pTemp ) return;

		pTemp->flags |= FTENT_SINEWAVE;

		pTemp->x = origin[0];
		pTemp->y = origin[1];
		float sine, cosine;
		float angle = COM_RandomFloat( -M_PI, M_PI );
		SinCos( angle, &sine, &cosine );

		float zspeed = COM_RandomLong( 80, 140 );
		VectorSet( pTemp->entity.baseline.origin, speed * cosine, speed * sine, zspeed );
		pTemp->die = cl.time + ((height - (origin[2] - start[2])) / zspeed) - 0.1f;
		pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );

		// Set sprite scale
		pTemp->entity.curstate.scale = 1.0f / COM_RandomFloat( 2.0f, 5.0f );
		pTemp->entity.curstate.rendermode = kRenderTransAlpha;
		pTemp->entity.curstate.renderamt = 255;
	}
}

/*
==============
R_AttachTentToPlayer

Attaches entity to player
==============
*/
void GAME_EXPORT R_AttachTentToPlayer( int client, int modelIndex, float zoffset, float life )
{
	TEMPENTITY	*pTemp;
	cl_entity_t	*pClient;
	model_t		*pModel;

	if( client <= 0 || client > cl.maxclients )
		return;

	pClient = CL_GetEntityByIndex( client );

	if( !pClient || pClient->curstate.messagenum != cl.parsecount )
		return;

	if(( pModel = CL_ModelHandle( modelIndex )) == NULL )
		return;

	vec3_t position = Vec3( pClient->origin );
	position[2] += zoffset;

	pTemp = CL_TempEntAllocHigh( position, pModel );
	if( !pTemp ) return;

	pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
	pTemp->entity.curstate.framerate = 1;

	pTemp->clientIndex = client;
	pTemp->tentOffset[0] = 0;
	pTemp->tentOffset[1] = 0;
	pTemp->tentOffset[2] = zoffset;
	pTemp->die = cl.time + life;
	pTemp->flags |= FTENT_PLYRATTACHMENT|FTENT_PERSIST;

	// is the model a sprite?
	if( pModel->type == mod_sprite )
	{
		pTemp->flags |= FTENT_SPRANIMATE|FTENT_SPRANIMATELOOP;
		pTemp->entity.curstate.framerate = 10;
	}
	else
	{
		// no animation support for attached clientside studio models.
		pTemp->frameMax = 0;
	}

	pTemp->entity.curstate.frame = 0;
}

/*
==============
R_KillAttachedTents

Detach entity from player
==============
*/
void GAME_EXPORT R_KillAttachedTents( int client )
{
	TEMPENTITY *tent;

	if( client <= 0 || client > cl.maxclients )
		return;

	for( tent = cl_active_tents; tent; tent = tent->next )
	{
		if( FBitSet( tent->flags, FTENT_PLYRATTACHMENT ) && tent->clientIndex == client )
			tent->die = cl.time;
	}
}

/*
==============
R_RicochetSprite

Create ricochet sprite
==============
*/
void GAME_EXPORT R_RicochetSprite( const vec3_t pos, model_t *pmodel, float duration, float scale )
{
	TEMPENTITY	*pTemp;

	pTemp = CL_TempEntAlloc( pos, pmodel );
	if( !pTemp ) return;

	pTemp->entity.curstate.rendermode = kRenderGlow;
	pTemp->entity.curstate.renderamt = pTemp->entity.baseline.renderamt = 200;
	pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
	pTemp->entity.curstate.scale = scale;
	pTemp->die = cl.time + duration;
	pTemp->flags = FTENT_FADEOUT;
	pTemp->fadeSpeed = 8;

	pTemp->entity.curstate.frame = 0;
	pTemp->entity.angles[ROLL] = 45.0f * COM_RandomLong( 0, 7 );
}

/*
==============
R_RocketFlare

Create rocket flare
==============
*/
void GAME_EXPORT R_RocketFlare( const vec3_t pos )
{
	TEMPENTITY	*pTemp;

	if( !cl_sprite_glow ) return;

	pTemp = CL_TempEntAlloc( pos, cl_sprite_glow );
	if ( !pTemp ) return;

	pTemp->entity.curstate.rendermode = kRenderGlow;
	pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
	pTemp->entity.curstate.renderamt = 200;
	pTemp->entity.curstate.framerate = 1.0;
	pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );
	pTemp->entity.curstate.scale = 1.0;
	pTemp->die = cl.time + 0.01f;	// when 100 fps die at next frame
	pTemp->entity.curstate.effects = EF_NOINTERP;
}

/*
==============
R_MuzzleFlash

Do muzzleflash
==============
*/
void GAME_EXPORT R_MuzzleFlash( const vec3_t pos, int type )
{
	TEMPENTITY	*pTemp;

	int index = ( type % 10 ) % MAX_MUZZLEFLASH;
	float scale = ( type / 10 ) * 0.1f;
	if( scale == 0.0f ) scale = 0.5f;

	if( !cl_sprite_muzzleflash[index] )
		return;

	// must set position for right culling on render
	pTemp = CL_TempEntAllocHigh( pos, cl_sprite_muzzleflash[index] );
	if( !pTemp ) return;
	pTemp->entity.curstate.rendermode = kRenderTransAdd;
	pTemp->entity.curstate.renderamt = 255;
	pTemp->entity.curstate.framerate = 10;
	pTemp->entity.curstate.renderfx = 0;
	pTemp->die = cl.time + 0.01; // die at next frame
	pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );
	pTemp->flags |= FTENT_SPRANIMATE|FTENT_SPRANIMATELOOP;
	pTemp->entity.curstate.scale = scale;

	if( index == 0 ) pTemp->entity.angles[2] = COM_RandomLong( 0, 20 ); // rifle flash
	else pTemp->entity.angles[2] = COM_RandomLong( 0, 359 );

	CL_TempEntAddEntity( &pTemp->entity );
}

/*
==============
R_BloodSprite

Create a high priority blood sprite
and some blood drops. This is high-priority tent
==============
*/
void GAME_EXPORT R_BloodSprite( const vec3_t org, int colorIndex, int modelIndex, int modelIndex2, float size )
{
	model_t		*pModel, *pModel2;
	TEMPENTITY	*pTemp;

	colorIndex += COM_RandomLong( 1, 3 );
	int impactindex = colorIndex;
	int spatterindex = colorIndex - 1;

	// validate the model first
	if(( pModel = CL_ModelHandle( modelIndex )) != NULL )
	{
		vec3_t pos = Vec3( org );
		pos[2] += COM_RandomFloat( 2.0f, 4.0f ); // make offset from ground (snarks issues)

		// large, single blood sprite is a high-priority tent
		if(( pTemp = CL_TempEntAllocHigh( pos, pModel )) != NULL )
		{
			pTemp->entity.curstate.rendermode = kRenderTransTexture;
			pTemp->entity.curstate.renderfx = kRenderFxClampMinScale;
			pTemp->entity.curstate.scale = COM_RandomFloat( size / 25.0f, size / 35.0f );
			pTemp->flags = FTENT_SPRANIMATE;

			pTemp->entity.curstate.rendercolor = clgame.palette[impactindex];
			pTemp->entity.baseline.renderamt = pTemp->entity.curstate.renderamt = 250;

			pTemp->entity.curstate.framerate = pTemp->frameMax * 4.0f; // Finish in 0.250 seconds
			pTemp->die = cl.time + (pTemp->frameMax / pTemp->entity.curstate.framerate ); // play the whole thing once

			pTemp->entity.curstate.frame = 0;
			pTemp->bounceFactor = 0;
			pTemp->entity.angles[2] = COM_RandomLong( 0, 360 );
		}
	}

	// validate the model first
	if(( pModel2 = CL_ModelHandle( modelIndex2 )) != NULL )
	{
		int splatter = size + ( COM_RandomLong( 1, 8 ) + COM_RandomLong( 1, 8 ));

		for( int i = 0; i < splatter; i++ )
		{
			// create blood drips
			if(( pTemp = CL_TempEntAlloc( org, pModel2 )) != NULL )
			{
				pTemp->entity.curstate.rendermode = kRenderTransTexture;
				pTemp->entity.curstate.renderfx = kRenderFxClampMinScale;
				pTemp->entity.curstate.scale = COM_RandomFloat( size / 15.0f, size / 25.0f );
				pTemp->flags = FTENT_ROTATE | FTENT_SLOWGRAVITY | FTENT_COLLIDEWORLD;

				pTemp->entity.curstate.rendercolor = clgame.palette[spatterindex];
				pTemp->entity.baseline.renderamt = pTemp->entity.curstate.renderamt = 250;

				pTemp->entity.baseline.origin[0] = COM_RandomFloat( -96.0f, 95.0f );
				pTemp->entity.baseline.origin[1] = COM_RandomFloat( -96.0f, 95.0f );
				pTemp->entity.baseline.origin[2] = COM_RandomFloat( -32.0f, 95.0f );
				pTemp->entity.baseline.angles[0] = COM_RandomFloat( -256.0f, -255.0f );
				pTemp->entity.baseline.angles[1] = COM_RandomFloat( -256.0f, -255.0f );
				pTemp->entity.baseline.angles[2] = COM_RandomFloat( -256.0f, -255.0f );

				pTemp->die = cl.time + COM_RandomFloat( 1.0f, 3.0f );

				pTemp->entity.curstate.frame = COM_RandomLong( 1, pTemp->frameMax );

				if( pTemp->entity.curstate.frame > 8.0f )
					pTemp->entity.curstate.frame = pTemp->frameMax;

				pTemp->entity.angles[2] = COM_RandomFloat( 0.0f, 360.0f );
				pTemp->bounceFactor	= 0.0f;
			}
		}
	}
}

/*
==============
R_BreakModel

Create a shards
==============
*/
void GAME_EXPORT R_BreakModel( const vec3_t pos, const vec3_t size, const vec3_t dir, float random, float life, int count, int modelIndex, char flags )
{
	TEMPENTITY	*pTemp;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
		return;

	char type = flags & BREAK_TYPEMASK;

	if( count == 0 )
	{
		// assume surface (not volume)
		count = (size[0] * size[1] + size[1] * size[2] + size[2] * size[0]) / (3 * SHARD_VOLUME * SHARD_VOLUME);
	}

	// limit to 100 pieces
	if( count > 100 ) count = 100;

	for( int i = 0; i < count; i++ )
	{
		vec3_t	vecSpot;
		int j;

		for( j = 0; j < 32; j++ )
		{
			// fill up the box with stuff
			vecSpot[0] = pos[0] + COM_RandomFloat( -0.5f, 0.5f ) * size[0];
			vecSpot[1] = pos[1] + COM_RandomFloat( -0.5f, 0.5f ) * size[1];
			vecSpot[2] = pos[2] + COM_RandomFloat( -0.5f, 0.5f ) * size[2];

			if( PM_CL_PointContents( vecSpot, NULL ) != CONTENTS_SOLID )
				break; // valid spot
		}

		if( j == 32 ) continue; // a piece completely stuck in the wall, ignore it

		pTemp = CL_TempEntAlloc( vecSpot, pmodel );
		if( !pTemp ) return;

		// keep track of break_type, so we know how to play sound on collision
		pTemp->hitSound = type;

		if( pmodel->type == mod_sprite )
			pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );
		else if( pmodel->type == mod_studio )
			pTemp->entity.curstate.body = COM_RandomLong( 0, pTemp->frameMax );

		pTemp->flags |= FTENT_COLLIDEWORLD | FTENT_FADEOUT | FTENT_SLOWGRAVITY;

		if( COM_RandomLong( 0, 255 ) < 200 )
		{
			pTemp->flags |= FTENT_ROTATE;
			pTemp->entity.baseline.angles[0] = COM_RandomFloat( -256, 255 );
			pTemp->entity.baseline.angles[1] = COM_RandomFloat( -256, 255 );
			pTemp->entity.baseline.angles[2] = COM_RandomFloat( -256, 255 );
		}

		if (( COM_RandomLong( 0, 255 ) < 100 ) && FBitSet( flags, BREAK_SMOKE ))
			pTemp->flags |= FTENT_SMOKETRAIL;

		if(( type == BREAK_GLASS ) || FBitSet( flags, BREAK_TRANS ))
		{
			pTemp->entity.curstate.rendermode = kRenderTransTexture;
			pTemp->entity.curstate.renderamt = pTemp->entity.baseline.renderamt = 128;
		}
		else
		{
			pTemp->entity.curstate.rendermode = kRenderNormal;
			pTemp->entity.curstate.renderamt = pTemp->entity.baseline.renderamt = 255; // set this for fadeout
		}

		pTemp->entity.baseline.origin[0] = dir[0] + COM_RandomFloat( -random, random );
		pTemp->entity.baseline.origin[1] = dir[1] + COM_RandomFloat( -random, random );
		pTemp->entity.baseline.origin[2] = dir[2] + COM_RandomFloat( 0, random );

		pTemp->die = cl.time + life + COM_RandomFloat( 0.0f, 1.0f ); // Add an extra 0-1 secs of life
	}
}

/*
==============
R_TempModel

Create a temp model with gravity, sounds and fadeout
==============
*/
TEMPENTITY *R_TempModel( const vec3_t pos, const vec3_t dir, const vec3_t angles, float life, int modelIndex, int soundtype )
{
	// alloc a new tempent
	TEMPENTITY	*pTemp;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
		return NULL;

	pTemp = CL_TempEntAlloc( pos, pmodel );
	if( !pTemp ) return NULL;

	pTemp->flags = (FTENT_COLLIDEWORLD|FTENT_GRAVITY);
	VectorCopy( dir, pTemp->entity.baseline.origin );
	VectorCopy( angles, pTemp->entity.angles );

	// keep track of shell type
	switch( soundtype )
	{
	case TE_BOUNCE_SHELL:
		pTemp->hitSound = BOUNCE_SHELL;
		pTemp->entity.baseline.angles[0] = COM_RandomFloat( -512, 511 );
		pTemp->entity.baseline.angles[1] = COM_RandomFloat( -255, 255 );
		pTemp->entity.baseline.angles[2] = COM_RandomFloat( -255, 255 );
		pTemp->flags |= FTENT_ROTATE;
		break;
	case TE_BOUNCE_SHOTSHELL:
		pTemp->hitSound = BOUNCE_SHOTSHELL;
		pTemp->entity.baseline.angles[0] = COM_RandomFloat( -512, 511 );
		pTemp->entity.baseline.angles[1] = COM_RandomFloat( -255, 255 );
		pTemp->entity.baseline.angles[2] = COM_RandomFloat( -255, 255 );
		pTemp->flags |= FTENT_ROTATE|FTENT_SLOWGRAVITY;
		break;
	}

	if( pmodel->type == mod_sprite )
		pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );
	else pTemp->entity.curstate.body = COM_RandomLong( 0, pTemp->frameMax );

	pTemp->die = cl.time + life;

	return pTemp;
}

/*
==============
R_DefaultSprite

Create an animated sprite
==============
*/
TEMPENTITY *R_DefaultSprite( const vec3_t pos, int spriteIndex, float framerate )
{
	TEMPENTITY	*pTemp;
	model_t		*psprite;

	// don't spawn while paused
	if( cl.time == cl.oldtime )
		return NULL;

	if(( psprite = CL_ModelHandle( spriteIndex )) == NULL || psprite->type != mod_sprite )
	{
		Con_Reportf( "No Sprite %d!\n", spriteIndex );
		return NULL;
	}

	pTemp = CL_TempEntAlloc( pos, psprite );
	if( !pTemp ) return NULL;

	pTemp->entity.curstate.scale = 1.0f;
	pTemp->flags |= FTENT_SPRANIMATE;
	if( framerate == 0 ) framerate = 10;

	pTemp->entity.curstate.framerate = framerate;
	pTemp->die = cl.time + (float)pTemp->frameMax / framerate;
	pTemp->entity.curstate.frame = 0;

	return pTemp;
}

/*
===============
R_SparkShower

Create an animated moving sprite
===============
*/
void GAME_EXPORT R_SparkShower( const vec3_t pos )
{
	TEMPENTITY	*pTemp;

	pTemp = CL_TempEntAllocNoModel( pos );
	if( !pTemp ) return;

	pTemp->entity.baseline.origin[0] = COM_RandomFloat( -300.0f, 300.0f );
	pTemp->entity.baseline.origin[1] = COM_RandomFloat( -300.0f, 300.0f );
	pTemp->entity.baseline.origin[2] = COM_RandomFloat( -200.0f, 200.0f );

	pTemp->flags |= FTENT_SLOWGRAVITY | FTENT_COLLIDEWORLD | FTENT_SPARKSHOWER;

	pTemp->entity.curstate.framerate = COM_RandomFloat( 0.5f, 1.5f );
	pTemp->entity.curstate.scale = cl.time;
	pTemp->die = cl.time + 0.5;
}

/*
===============
R_TempSprite

Create an animated moving sprite
===============
*/
TEMPENTITY *R_TempSprite( vec3_t pos, const vec3_t dir, float scale, int modelIndex, int rendermode, int renderfx, float a, float life, int flags )
{
	TEMPENTITY	*pTemp;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
	{
		Con_Reportf( S_ERROR "No model %d!\n", modelIndex );
		return NULL;
	}

	pTemp = CL_TempEntAlloc( pos, pmodel );
	if( !pTemp ) return NULL;

	pTemp->entity.curstate.framerate = 10;
	pTemp->entity.curstate.rendermode = rendermode;
	pTemp->entity.curstate.renderfx = renderfx;
	pTemp->entity.curstate.scale = scale;
	pTemp->entity.baseline.renderamt = a * 255;
	pTemp->entity.curstate.renderamt = a * 255;
	pTemp->flags |= flags;

	VectorCopy( dir, pTemp->entity.baseline.origin );

	if( life ) pTemp->die = cl.time + life;
	else pTemp->die = cl.time + ( pTemp->frameMax * 0.1f ) + 1.0f;
	pTemp->entity.curstate.frame = 0;

	return pTemp;
}

/*
===============
R_Sprite_Explode

apply params for exploding sprite
===============
*/
void GAME_EXPORT R_Sprite_Explode( TEMPENTITY *pTemp, float scale, int flags )
{
	if( !pTemp )
		return;

	qboolean noadditive = FBitSet( flags, TE_EXPLFLAG_NOADDITIVE );
	qboolean drawalpha  = FBitSet( flags, TE_EXPLFLAG_DRAWALPHA );
	qboolean rotate     = FBitSet( flags, TE_EXPLFLAG_ROTATE );

	pTemp->entity.curstate.scale = scale;
	pTemp->entity.baseline.origin[2] = 8.0f;
	pTemp->entity.origin[2] = pTemp->entity.origin[2] + 10.0f;
	if( rotate )
		pTemp->entity.angles[2] = COM_RandomFloat( 0.0, 360.0f );

	pTemp->entity.curstate.rendermode = noadditive ? kRenderNormal :
		drawalpha ? kRenderTransAlpha : kRenderTransAdd;
	pTemp->entity.curstate.renderamt  = noadditive ? 0xff : 0xb4;
	pTemp->entity.curstate.renderfx = 0;
	pTemp->entity.curstate.rendercolor.r = 0;
	pTemp->entity.curstate.rendercolor.g = 0;
	pTemp->entity.curstate.rendercolor.b = 0;
}

/*
===============
R_Sprite_Smoke

apply params for smoke sprite
===============
*/
void GAME_EXPORT R_Sprite_Smoke( TEMPENTITY *pTemp, float scale )
{
	if( !pTemp ) return;

	int iColor = COM_RandomLong( 20, 35 );
	pTemp->entity.curstate.rendermode = kRenderTransAlpha;
	pTemp->entity.curstate.renderfx = kRenderFxNone;
	pTemp->entity.baseline.origin[2] = 30;
	pTemp->entity.curstate.rendercolor.r = iColor;
	pTemp->entity.curstate.rendercolor.g = iColor;
	pTemp->entity.curstate.rendercolor.b = iColor;
	pTemp->entity.origin[2] += 20;
	pTemp->entity.curstate.scale = scale;
}

static void R_Spray_Generic( const char *func, const vec3_t pos, const vec3_t dir, int modelIndex, int count, int speed, int spread, int rendermode, qboolean sprite_spray )
{
	model_t *mod = CL_ModelHandle( modelIndex );
	float noise = spread / 100.0f;
	float znoise = noise * 1.5f;

	znoise = Q_min( 1.0f, znoise );

	if( !mod )
	{
		Con_Reportf( "%s: No model %d!\n", func, modelIndex );
		return;
	}

	for( int i = 0; i < count; i++ )
	{
		TEMPENTITY *tent = CL_TempEntAlloc( pos, mod );
		vec3_t noise_vec, vout;

		if( !tent )
			break;

		tent->flags |= FTENT_SLOWGRAVITY;
		tent->entity.curstate.rendermode = rendermode;
		tent->entity.curstate.renderamt = 255;
		tent->entity.baseline.renderamt = 255;
		tent->entity.curstate.renderfx = kRenderFxNoDissipation;

		noise_vec[0] = COM_RandomFloat( -noise, noise );
		noise_vec[1] = COM_RandomFloat( -noise, noise );
		noise_vec[2] = COM_RandomFloat( 0.0f, znoise );
		float rand_speed = COM_RandomFloat( speed * 0.8f, speed * 1.2f );

		VectorAdd( dir, noise_vec, vout );
		VectorScale( vout, rand_speed, tent->entity.baseline.origin );

		if( sprite_spray ) // if TE_SPRITE_SPRAY
		{
			tent->flags |= FTENT_FADEOUT;
			tent->fadeSpeed = 2.0f;

			tent->entity.curstate.framerate = 0.5f;
			tent->die = cl.time + 0.35;
			tent->entity.curstate.frame = COM_RandomLong( 0, tent->frameMax ); // frameMax inclusive
		}
		else
		{
			tent->flags |= FTENT_COLLIDEWORLD;

			if( tent->frameMax > 1 )
			{
				tent->flags |= FTENT_SPRANIMATE;
				tent->entity.curstate.framerate = 10.0f;
				tent->die = cl.time + tent->frameMax * 0.1;
			}
			else
			{
				tent->entity.curstate.framerate = 1.0f;
				tent->die = cl.time + 0.35;
			}
			tent->entity.curstate.frame = 0.0f;
		}
	}
}

/*
===============
R_Spray

Throws a shower of sprites or models
===============
*/
void GAME_EXPORT R_Spray( const vec3_t pos, const vec3_t dir, int modelIndex, int count, int speed, int spread, int rendermode )
{
	R_Spray_Generic( __func__, pos, dir, modelIndex, count, speed, spread, rendermode, false );
}

/*
===============
R_Sprite_Spray

Spray of alpha sprites
===============
*/
void GAME_EXPORT R_Sprite_Spray( const vec3_t pos, const vec3_t dir, int modelIndex, int count, int speed, int spread )
{
	R_Spray_Generic( __func__, pos, dir, modelIndex, count, speed, spread, kRenderTransAlpha, true );
}

/*
===============
R_Sprite_Trail

Line of moving glow sprites with gravity,
fadeout, and collisions
===============
*/
void GAME_EXPORT R_Sprite_Trail( int type, vec3_t start, vec3_t end, int modelIndex, int count, float life, float size, float amp, int renderamt, float speed )
{
	TEMPENTITY	*pTemp;
	vec3_t		delta, dir;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
		return;

	VectorSubtract( end, start, delta );
	VectorNormalize2( delta, dir );

	amp /= 256.0f;

	for( int i = 0; i < count; i++ )
	{
		vec3_t	pos, vel;

		// Be careful of divide by 0 when using 'count' here...
		if( i == 0 ) VectorCopy( start, pos );
		else VectorMA( start, ( i / ( count - 1.0f )), delta, pos );

		pTemp = CL_TempEntAlloc( pos, pmodel );
		if( !pTemp ) return;

		pTemp->flags = (FTENT_COLLIDEWORLD|FTENT_SPRCYCLE|FTENT_FADEOUT|FTENT_SLOWGRAVITY);

		VectorScale( dir, speed, vel );
		vel[0] += COM_RandomFloat( -127.0f, 128.0f ) * amp;
		vel[1] += COM_RandomFloat( -127.0f, 128.0f ) * amp;
		vel[2] += COM_RandomFloat( -127.0f, 128.0f ) * amp;
		VectorCopy( vel, pTemp->entity.baseline.origin );
		VectorCopy( pos, pTemp->entity.origin );

		pTemp->entity.curstate.scale = size;
		pTemp->entity.curstate.rendermode = kRenderGlow;
		pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
		pTemp->entity.curstate.renderamt = pTemp->entity.baseline.renderamt = renderamt;

		pTemp->entity.curstate.frame = COM_RandomLong( 0, pTemp->frameMax );
		pTemp->die = cl.time + life + COM_RandomFloat( 0.0f, 4.0f );
	}
}

/*
===============
R_FunnelSprite

Create a funnel effect with custom sprite
===============
*/
void GAME_EXPORT R_FunnelSprite( const vec3_t org, int modelIndex, int reverse )
{
	TEMPENTITY	*pTemp;
	vec3_t		dir, dest;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
	{
		Con_Reportf( S_ERROR "no model %d!\n", modelIndex );
		return;
	}

	for( int i = -8; i < 8; i++ )
	{
		for( int j = -8; j < 8; j++ )
		{
			pTemp = CL_TempEntAlloc( org, pmodel );
			if( !pTemp ) return;

			dest[0] = (i * 32.0f) + org[0];
			dest[1] = (j * 32.0f) + org[1];
			dest[2] = org[2] + COM_RandomFloat( 100.0f, 800.0f );

			if( reverse )
			{
				VectorCopy( org, pTemp->entity.origin );
				VectorSubtract( dest, pTemp->entity.origin, dir );
			}
			else
			{
				VectorCopy( dest, pTemp->entity.origin );
				VectorSubtract( org, pTemp->entity.origin, dir );
			}

			pTemp->entity.curstate.rendermode = kRenderGlow;
			pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
			pTemp->entity.baseline.renderamt = pTemp->entity.curstate.renderamt = 200;
			pTemp->entity.baseline.angles[2] = COM_RandomFloat( -100.0f, 100.0f );
			pTemp->entity.curstate.framerate = COM_RandomFloat( 0.1f, 0.4f );
			pTemp->flags = FTENT_ROTATE|FTENT_FADEOUT;

			float vel = dest[2] / 8.0f;
			if( vel < 64.0f ) vel = 64.0f;
			float dist = VectorNormalizeLength( dir );
			vel += COM_RandomFloat( 64.0f, 128.0f );
			VectorScale( dir, vel, pTemp->entity.baseline.origin );
			pTemp->die = cl.time + (dist / vel) - 0.5f;
			pTemp->fadeSpeed = 2.0f;
		}
	}
}

/*
===============
R_SparkEffect

Create a streaks + ricochet sprite
===============
*/
void GAME_EXPORT R_SparkEffect( const vec3_t pos, int count, int velocityMin, int velocityMax )
{
	R_RicochetSprite( pos, cl_sprite_ricochet, 0.1f, COM_RandomFloat( 0.5f, 1.0f ));
	R_SparkStreaks( pos, count, velocityMin, velocityMax );
}

/*
==============
R_RicochetSound

Make a random ricochet sound
==============
*/
static void R_RicochetSoundByName( const vec3_t pos, const char *name )
{
#if XASH_GAMECUBE
	/* G126: only ric1 is preloaded/pinned; alias all ric* here too
	 * (CL_TempEntPlaySound BOUNCE_SHRAP picks random ric before ByName). */
	if( GC_MapLoadMemoryOpt()
		&& name && !Q_strnicmp( name, "weapons/ric", 11 ))
		name = "weapons/ric1.wav";
#endif
	sound_t handle = S_RegisterSound( name );
	S_StartSound( pos, 0, CHAN_AUTO, handle, VOL_NORM, 1.0, 100, 0 );
}

static void R_RicochetSoundByIndex( const vec3_t pos, int idx )
{
#if XASH_GAMECUBE
	/* G126: only ric1 is preloaded/pinned under memopt; other variants miss. */
	if( GC_MapLoadMemoryOpt())
	{
		R_RicochetSoundByName( pos, "weapons/ric1.wav" );
		return;
	}
#endif
	const char *name = SoundList_Get( Ricochet, idx );
	if( name )
		R_RicochetSoundByName( pos, name );
}

void GAME_EXPORT R_RicochetSound( const vec3_t pos )
{
#if XASH_GAMECUBE
	/* G126: only ric1 is preloaded/pinned under memopt; other variants miss. */
	if( GC_MapLoadMemoryOpt())
	{
		R_RicochetSoundByName( pos, "weapons/ric1.wav" );
		return;
	}
#endif
	const char *name = SoundList_GetRandom( Ricochet );
	if( name )
		R_RicochetSoundByName( pos, name );
}

/*
==============
R_Projectile

Create an projectile entity
==============
*/
void GAME_EXPORT R_Projectile( const vec3_t origin, const vec3_t velocity, int modelIndex, int life, int owner, void (*hitcallback)( TEMPENTITY*, pmtrace_t* ))
{
	TEMPENTITY	*pTemp;
	model_t		*pmodel;
	vec3_t		dir;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
		return;

	pTemp = CL_TempEntAllocHigh( origin, pmodel );
	if( !pTemp ) return;

	VectorCopy( velocity, pTemp->entity.baseline.origin );

	if( pmodel->type == mod_sprite )
	{
		SetBits( pTemp->flags, FTENT_SPRANIMATE );

		if( pTemp->frameMax < 10 )
		{
			SetBits( pTemp->flags, FTENT_SPRANIMATE|FTENT_SPRANIMATELOOP );
			pTemp->entity.curstate.framerate = 10;
		}
		else
		{
			pTemp->entity.curstate.framerate = pTemp->frameMax / life;
		}
	}
	else
	{
		pTemp->frameMax = 0;
		VectorNormalize2( velocity, dir );
		VectorAngles( dir, pTemp->entity.angles );
	}

	pTemp->flags |= FTENT_COLLIDEALL|FTENT_PERSIST|FTENT_COLLIDEKILL;
	pTemp->clientIndex = bound( 1, owner, cl.maxclients );
	pTemp->entity.baseline.renderamt = 255;
	pTemp->hitcallback = hitcallback;
	pTemp->die = cl.time + life;
}

/*
==============
R_TempSphereModel

Spherical shower of models, picks from set
==============
*/
void GAME_EXPORT R_TempSphereModel( const vec3_t pos, float speed, float life, int count, int modelIndex )
{
	TEMPENTITY	*pTemp;

	// create temp models
	for( int i = 0; i < count; i++ )
	{
		pTemp = CL_TempEntAlloc( pos, CL_ModelHandle( modelIndex ));
		if( !pTemp ) return;

		pTemp->entity.curstate.body = COM_RandomLong( 0, pTemp->frameMax );

		if( COM_RandomLong( 0, 255 ) < 10 )
			pTemp->flags |= FTENT_SLOWGRAVITY;
		else pTemp->flags |= FTENT_GRAVITY;

		if( COM_RandomLong( 0, 255 ) < 200 )
		{
			pTemp->flags |= FTENT_ROTATE;
			pTemp->entity.baseline.angles[0] = COM_RandomFloat( -256.0f, -255.0f );
			pTemp->entity.baseline.angles[1] = COM_RandomFloat( -256.0f, -255.0f );
			pTemp->entity.baseline.angles[2] = COM_RandomFloat( -256.0f, -255.0f );
		}

		if( COM_RandomLong( 0, 255 ) < 100 )
			pTemp->flags |= FTENT_SMOKETRAIL;

		pTemp->flags |= FTENT_FLICKER | FTENT_COLLIDEWORLD;
		pTemp->entity.curstate.rendermode = kRenderNormal;
		pTemp->entity.curstate.effects = i & 31;
		pTemp->entity.baseline.origin[0] = COM_RandomFloat( -1.0f, 1.0f );
		pTemp->entity.baseline.origin[1] = COM_RandomFloat( -1.0f, 1.0f );
		pTemp->entity.baseline.origin[2] = COM_RandomFloat( -1.0f, 1.0f );

		VectorNormalize( pTemp->entity.baseline.origin );
		VectorScale( pTemp->entity.baseline.origin, speed, pTemp->entity.baseline.origin );
		pTemp->die = cl.time + life;
	}
}

/*
==============
R_Explosion

Create an explosion (scale is magnitude)
==============
*/
void GAME_EXPORT R_Explosion( vec3_t pos, int model, float scale, float framerate, int flags )
{
	if( scale != 0.0f )
	{
		// create explosion sprite
		R_Sprite_Explode( R_DefaultSprite( pos, model, framerate ), scale, flags );

		if( !FBitSet( flags, TE_EXPLFLAG_NOPARTICLES ))
			R_FlickerParticles( pos );

		if( !FBitSet( flags, TE_EXPLFLAG_NODLIGHTS ))
		{
			// big flash
			dlight_t *dl = CL_AllocDlight( 0 );
			VectorCopy( pos, dl->origin );
			dl->radius = 200;
			dl->color.r = 250;
			dl->color.g = 250;
			dl->color.b = 150;
			dl->die = cl.time + 0.01f;
			dl->decay = 800;

			// red glow
			dl = CL_AllocDlight( 0 );
			VectorCopy( pos, dl->origin );
			dl->radius = 150;
			dl->color.r = 255;
			dl->color.g = 190;
			dl->color.b = 40;
			dl->die = cl.time + 1.0f;
			dl->decay = 200;
		}
	}

	if( !FBitSet( flags, TE_EXPLFLAG_NOSOUND ))
	{
		const char *name = SoundList_GetRandom( Explode );
		if( name )
		{
			sound_t hSound = S_RegisterSound( name );
			S_StartSound( pos, 0, CHAN_STATIC, hSound, VOL_NORM, 0.3f, PITCH_NORM, 0 );
		}
	}
}

/*
==============
R_PlayerSprites

Create a particle smoke around player
==============
*/
void GAME_EXPORT R_PlayerSprites( int client, int modelIndex, int count, int size )
{
	TEMPENTITY	*pTemp;
	cl_entity_t	*pEnt;
	vec3_t		dir;

	pEnt = CL_GetEntityByIndex( client );

	if( !pEnt || !pEnt->player )
		return;

	for( int i = 0; i < count; i++ )
	{
		vec3_t position = Vec3( pEnt->origin );
		position[0] += COM_RandomFloat( -10.0f, 10.0f );
		position[1] += COM_RandomFloat( -10.0f, 10.0f );
		position[2] += COM_RandomFloat( -20.0f, 36.0f );

		pTemp = CL_TempEntAlloc( position, CL_ModelHandle( modelIndex ));
		if( !pTemp ) return;

		VectorSubtract( pTemp->entity.origin, pEnt->origin, pTemp->tentOffset );

		if ( i != 0 )
		{
			pTemp->flags |= FTENT_PLYRATTACHMENT;
			pTemp->clientIndex = client;
		}
		else
		{
			VectorSubtract( position, pEnt->origin, dir );
			VectorNormalize( dir );
			VectorScale( dir, 60, dir );
			VectorCopy( dir, pTemp->entity.baseline.origin );
			pTemp->entity.baseline.origin[1] = COM_RandomFloat( 20.0f, 60.0f );
		}

		pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
		pTemp->entity.curstate.framerate = COM_RandomFloat( 1.0f - (size / 100.0f ), 1.0f );

		if( pTemp->frameMax > 1 )
		{
			pTemp->flags |= FTENT_SPRANIMATE;
			pTemp->entity.curstate.framerate = 20.0f;
			pTemp->die = cl.time + (pTemp->frameMax * 0.05f);
		}
		else
		{
			pTemp->die = cl.time + 0.35f;
		}
	}
}

/*
==============
R_FireField

Makes a field of fire
==============
*/
void GAME_EXPORT R_FireField( float *org, int radius, int modelIndex, int count, int flags, float life )
{
	TEMPENTITY	*pTemp;
	model_t		*pmodel;

	if(( pmodel = CL_ModelHandle( modelIndex )) == NULL )
		return;

	for( int i = 0; i < count; i++ )
	{
		vec3_t pos = Vec3( org );
		pos[0] += COM_RandomFloat( -radius, radius );
		pos[1] += COM_RandomFloat( -radius, radius );

		if( !FBitSet( flags, TEFIRE_FLAG_PLANAR ))
			pos[2] += COM_RandomFloat( -radius, radius );

		pTemp = CL_TempEntAlloc( pos, pmodel );
		if( !pTemp ) return;

		if( FBitSet( flags, TEFIRE_FLAG_ALPHA ))
		{
			pTemp->entity.curstate.rendermode = kRenderTransAlpha;
			pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
			pTemp->entity.baseline.renderamt = pTemp->entity.curstate.renderamt = 128;
		}
		else if( FBitSet( flags, TEFIRE_FLAG_ADDITIVE ))
		{
			pTemp->entity.curstate.rendermode = kRenderTransAdd;
			pTemp->entity.curstate.renderamt = 180;
		}
		else
		{
			pTemp->entity.curstate.rendermode = kRenderNormal;
			pTemp->entity.curstate.renderfx = kRenderFxNoDissipation;
			pTemp->entity.baseline.renderamt = pTemp->entity.curstate.renderamt = 255;
		}

		pTemp->entity.curstate.framerate = COM_RandomFloat( 0.75f, 1.25f );
		float time = life + COM_RandomFloat( -0.25f, 0.5f );
		pTemp->die = cl.time + time;

		if( pTemp->frameMax > 1 )
		{
			pTemp->flags |= FTENT_SPRANIMATE;

			if( FBitSet( flags, TEFIRE_FLAG_LOOP ))
			{
				pTemp->entity.curstate.framerate = 15.0f;
				pTemp->flags |= FTENT_SPRANIMATELOOP;
			}
			else
			{
				pTemp->entity.curstate.framerate = pTemp->frameMax / time;
			}
		}

		if( FBitSet( flags, TEFIRE_FLAG_ALLFLOAT ) || ( FBitSet( flags, TEFIRE_FLAG_SOMEFLOAT ) && !COM_RandomLong( 0, 1 )))
		{
			// drift sprite upward
			pTemp->entity.baseline.origin[2] = COM_RandomFloat( 10.0f, 30.0f );
		}
	}
}

/*
==============
R_MultiGunshot

Client version of shotgun shot
==============
*/
void GAME_EXPORT R_MultiGunshot( const vec3_t org, const vec3_t dir, const vec3_t noise, int count, int decalCount, int *decalIndices )
{
	pmtrace_t	trace;
	vec3_t	right, up;
	vec3_t	vecDir, vecEnd;

	VectorVectors( dir, right, up );
	vec3_t vecSrc = Vec3( org );

	for( int i = 0; i < count; i++ )
	{
		// get circular gaussian spread
		float x, y, z;
		do {
			x = COM_RandomFloat( -0.5f, 0.5f ) + COM_RandomFloat( -0.5f, 0.5f );
			y = COM_RandomFloat( -0.5f, 0.5f ) + COM_RandomFloat( -0.5f, 0.5f );
			z = x * x + y * y;
		} while( z > 1.0f );

		for( int j = 0; j < 3; j++ )
		{
			vecDir[j] = dir[j] + x * noise[0] * right[j] + y * noise[1] * up[j];
			vecEnd[j] = vecSrc[j] + 4096.0f * vecDir[j];
		}

		trace = CL_TraceLine( vecSrc, vecEnd, PM_STUDIO_IGNORE );

		// paint decals
		if( trace.fraction != 1.0f )
		{
			physent_t	*pe = NULL;

			if( i & 2 ) R_RicochetSound( trace.endpos );
			R_BulletImpactParticles( trace.endpos );

			if( trace.ent >= 0 && trace.ent < clgame.pmove->numphysent )
				pe = &clgame.pmove->physents[trace.ent];

			if( pe && ( pe->solid == SOLID_BSP || pe->movetype == MOVETYPE_PUSHSTEP ))
			{
				cl_entity_t *e = CL_GetEntityByIndex( pe->info );
				int decalIndex = CL_DecalIndex( decalIndices[COM_RandomLong( 0, decalCount-1 )] );
				CL_DecalShoot( decalIndex, e->index, 0, trace.endpos, 0 );
			}
		}
	}
}

/*
==============
R_Sprite_WallPuff

Create a wallpuff
==============
*/
void GAME_EXPORT R_Sprite_WallPuff( TEMPENTITY *pTemp, float scale )
{
	if( !pTemp ) return;

	pTemp->entity.curstate.renderamt = 255;
	pTemp->entity.curstate.rendermode = kRenderTransAlpha;
	pTemp->entity.angles[ROLL] = COM_RandomLong( 0, 359 );
	pTemp->entity.baseline.origin[2] = 30;
	pTemp->entity.curstate.scale = scale;
	pTemp->die = cl.time + 0.01f;
}



/*
==============
CL_ParseTempEntity

handle temp-entity messages
==============
*/
void CL_ParseTempEntity( sizebuf_t *msg, connprotocol_t proto )
{
	sizebuf_t		buf, *pbuf;
	byte		msg_data[2048];
	int		type, color, count, flags;
	int		decalIndex = 0, modelIndex = 0, entityIndex = 0;
	float		scale, life, frameRate, vel, random;
	float		brightness, r, g, b;
	vec3_t		pos, pos2, ang;
	int		decalIndices[1];	// just stub
	TEMPENTITY	*pTemp;
	cl_entity_t	*pEnt;
	dlight_t		*dl;
	sound_t	hSound;
	const char *name;

#if XASH_GAMECUBE
	gc_fx_te_parsed++;
#endif
	if( proto != PROTO_GOLDSRC )
	{
		int iSize = MSG_ReadWord( msg );

		// this will probably be fatal anyway
		if( iSize > sizeof( msg_data ))
		{
			Con_Printf( S_ERROR "%s: Temp buffer overflow!\n", __func__ );
			MSG_SeekToBit( msg, iSize << 3, SEEK_CUR );
			return;
		}

		// parse user message into buffer
		MSG_ReadBytes( msg, msg_data, sizeof( msg_data ), iSize );

		// init a safe tempbuffer
		MSG_Init( &buf, "TempEntity", msg_data, iSize );

		pbuf = &buf;
	}
	else
	{
		pbuf = msg;
	}

	type = MSG_ReadByte( pbuf );

	switch( type )
	{
	case TE_BEAMPOINTS:
	case TE_BEAMENTPOINT:
	case TE_LIGHTNING:
	case TE_BEAMENTS:
	case TE_BEAM:
	case TE_BEAMSPRITE:
	case TE_BEAMTORUS:
	case TE_BEAMDISK:
	case TE_BEAMCYLINDER:
	case TE_BEAMFOLLOW:
	case TE_BEAMRING:
	case TE_BEAMHOSE:
	case TE_KILLBEAM:
		CL_ParseViewBeam( pbuf, type );
		break;
	case TE_GUNSHOT:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		R_RicochetSound( pos );
		R_RunParticleEffect( pos, vec3_origin, 0, 20 );
		break;
	case TE_EXPLOSION:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		scale = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		frameRate = MSG_ReadByte( pbuf );
		flags = MSG_ReadByte( pbuf );
		R_Explosion( pos, modelIndex, scale, frameRate, flags );
		break;
	case TE_TAREXPLOSION:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		R_BlobExplosion( pos );

		if(( name = SoundList_Get( Explode, 0 )))
		{
			hSound = S_RegisterSound( name );
			S_StartSound( pos, -1, CHAN_AUTO, hSound, VOL_NORM, 1.0f, PITCH_NORM, 0 );
		}
		break;
	case TE_SMOKE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		scale = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		frameRate = MSG_ReadByte( pbuf );
		pTemp = R_DefaultSprite( pos, modelIndex, frameRate );
		R_Sprite_Smoke( pTemp, scale );
		break;
	case TE_TRACER:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		R_TracerEffect( pos, pos2 );
		break;
	case TE_SPARKS:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		R_SparkShower( pos );
		break;
	case TE_LAVASPLASH:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		R_LavaSplash( pos );
		break;
	case TE_TELEPORT:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		R_TeleportSplash( pos );
		break;
	case TE_EXPLOSION2:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		color = MSG_ReadByte( pbuf );
		count = MSG_ReadByte( pbuf );
		R_ParticleExplosion2( pos, color, count );

		dl = CL_AllocDlight( 0 );
		VectorCopy( pos, dl->origin );
		dl->radius = 350;
		dl->die = cl.time + 0.5;
		dl->decay = 300;

		if(( name = SoundList_Get( Explode, 0 )))
		{
			hSound = S_RegisterSound( name );
			S_StartSound( pos, -1, CHAN_AUTO, hSound, VOL_NORM, 1.0f, PITCH_NORM, 0 );
		}
		break;
	case TE_BSPDECAL:
	case TE_DECAL:
	case TE_WORLDDECAL:
	case TE_WORLDDECALHIGH:
	case TE_DECALHIGH:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		if( type == TE_BSPDECAL )
		{
			decalIndex = MSG_ReadShort( pbuf );
			entityIndex = MSG_ReadShort( pbuf );
			if( entityIndex )
				modelIndex = MSG_ReadShort( pbuf );
			else modelIndex = 0;
		}
		else
		{
			decalIndex = MSG_ReadByte( pbuf );
			if( type == TE_DECALHIGH || type == TE_WORLDDECALHIGH )
				decalIndex += 256;

			if( type == TE_DECALHIGH || type == TE_DECAL )
				entityIndex = MSG_ReadShort( pbuf );
			else entityIndex = 0;

			pEnt = CL_GetEntityByIndex( entityIndex );

			// this might be obvious, but do not exit _before_ all data has been read or we will get parsing error later
			if( !pEnt )
			{
				Con_Reportf( "%s: ignored temp entity message %d, invalid entity num %d\n", __func__, type, entityIndex );
				break;
			}

			modelIndex = pEnt->curstate.modelindex;
		}
		CL_DecalShoot( CL_DecalIndex( decalIndex ), entityIndex, modelIndex, pos, type == TE_BSPDECAL ? FDECAL_PERMANENT : 0 );
		break;
	case TE_IMPLOSION:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		scale = MSG_ReadByte( pbuf );
		count = MSG_ReadByte( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_Implosion( pos, scale, count, life );
		break;
	case TE_SPRITETRAIL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		life = (float)MSG_ReadByte( pbuf ) * 0.1f;
		scale = (float)MSG_ReadByte( pbuf );
		if( !scale ) scale = 1.0f;
		else scale *= 0.1f;
		vel = (float)MSG_ReadByte( pbuf ) * 10;
		random = (float)MSG_ReadByte( pbuf ) * 10;
		R_Sprite_Trail( type, pos, pos2, modelIndex, count, life, scale, random, 255, vel );
		break;
	case TE_SPRITE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		scale = (float)MSG_ReadByte( pbuf ) * 0.1f;
		brightness = (float)MSG_ReadByte( pbuf ) / 255.0f;

		R_TempSprite( pos, vec3_origin, scale, modelIndex,
			kRenderTransAdd, kRenderFxNone, brightness, 0.0, FTENT_SPRANIMATE );
		break;
	case TE_GLOWSPRITE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		life = (float)MSG_ReadByte( pbuf ) * 0.1f;
		scale = (float)MSG_ReadByte( pbuf ) * 0.1f;
		brightness = (float)MSG_ReadByte( pbuf ) / 255.0f;

		R_TempSprite( pos, vec3_origin, scale, modelIndex,
			kRenderGlow, kRenderFxNoDissipation, brightness, life, FTENT_FADEOUT );
		break;
	case TE_STREAK_SPLASH:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		color = MSG_ReadByte( pbuf );
		count = MSG_ReadShort( pbuf );
		vel = (float)MSG_ReadShort( pbuf );
		random = (float)MSG_ReadShort( pbuf );
		R_StreakSplash( pos, pos2, color, count, vel, -random, random );
		break;
	case TE_DLIGHT:
		dl = CL_AllocDlight( 0 );
		dl->origin[0] = MSG_ReadCoord( pbuf );
		dl->origin[1] = MSG_ReadCoord( pbuf );
		dl->origin[2] = MSG_ReadCoord( pbuf );
		dl->radius = (float)(MSG_ReadByte( pbuf ) * 10.0f);
		dl->color.r = MSG_ReadByte( pbuf );
		dl->color.g = MSG_ReadByte( pbuf );
		dl->color.b = MSG_ReadByte( pbuf );
		dl->die = cl.time + (float)(MSG_ReadByte( pbuf ) * 0.1f);
		dl->decay = (float)(MSG_ReadByte( pbuf ) * 10.0f);
		break;
	case TE_ELIGHT:
		dl = CL_AllocElight( MSG_ReadShort( pbuf ));
		dl->origin[0] = MSG_ReadCoord( pbuf );
		dl->origin[1] = MSG_ReadCoord( pbuf );
		dl->origin[2] = MSG_ReadCoord( pbuf );
		dl->radius = MSG_ReadCoord( pbuf );
		dl->color.r = MSG_ReadByte( pbuf );
		dl->color.g = MSG_ReadByte( pbuf );
		dl->color.b = MSG_ReadByte( pbuf );
		life = (float)MSG_ReadByte( pbuf ) * 0.1f;
		dl->die = cl.time + life;
		dl->decay = MSG_ReadCoord( pbuf );
		if( life != 0 ) dl->decay /= life;
		break;
	case TE_TEXTMESSAGE:
		CL_ParseTextMessage( pbuf );
		break;
	case TE_LINE:
	case TE_BOX:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		life = (float)(MSG_ReadShort( pbuf ) * 0.1f);
		r = MSG_ReadByte( pbuf );
		g = MSG_ReadByte( pbuf );
		b = MSG_ReadByte( pbuf );
		if( type == TE_LINE ) R_ParticleLine( pos, pos2, r, g, b, life );
		else R_ParticleBox( pos, pos2, r, g, b, life );
		break;
	case TE_LARGEFUNNEL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		flags = MSG_ReadShort( pbuf );
		R_LargeFunnel( pos, flags );
		R_FunnelSprite( pos, modelIndex, flags );
		break;
	case TE_BLOODSTREAM:
	case TE_BLOOD:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		color = MSG_ReadByte( pbuf );
		count = MSG_ReadByte( pbuf );
		if( type == TE_BLOOD ) R_Blood( pos, pos2, color, count );
		else R_BloodStream( pos, pos2, color, count );
		break;
	case TE_SHOWLINE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		R_ShowLine( pos, pos2 );
		break;
	case TE_FIZZ:
		entityIndex = MSG_ReadShort( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		scale = MSG_ReadByte( pbuf );	// same as density
		pEnt = CL_GetEntityByIndex( entityIndex );
		R_FizzEffect( pEnt, modelIndex, scale );
		break;
	case TE_MODEL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		ang[0] = 0.0f;
		ang[1] = MSG_ReadAngle( pbuf ); // yaw angle
		ang[2] = 0.0f;
		modelIndex = MSG_ReadShort( pbuf );
		flags = MSG_ReadByte( pbuf );	// sound flags
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_TempModel( pos, pos2, ang, life, modelIndex, flags );
		break;
	case TE_EXPLODEMODEL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		vel = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadShort( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_TempSphereModel( pos, vel, life, count, modelIndex );
		break;
	case TE_BREAKMODEL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		ang[0] = MSG_ReadCoord( pbuf );
		ang[1] = MSG_ReadCoord( pbuf );
		ang[2] = MSG_ReadCoord( pbuf );
		random = (float)MSG_ReadByte( pbuf ) * 10.0f;
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		flags = MSG_ReadByte( pbuf );
		R_BreakModel( pos, pos2, ang, random, life, count, modelIndex, (char)flags );
		break;
	case TE_GUNSHOTDECAL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		entityIndex = MSG_ReadShort( pbuf );
		decalIndex = MSG_ReadByte( pbuf );
		CL_DecalShoot( CL_DecalIndex( decalIndex ), entityIndex, 0, pos, 0 );
		R_BulletImpactParticles( pos );
		flags = COM_RandomLong( 0, 0x7fff );
		if( flags < 0x3fff && ( count = SoundList_Count( Ricochet )))
			R_RicochetSoundByIndex( pos, flags % count );
		break;
	case TE_SPRAY:
	case TE_SPRITE_SPRAY:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		vel = (float)MSG_ReadByte( pbuf );
		random = (float)MSG_ReadByte( pbuf );
		if( type == TE_SPRAY )
		{
			flags = MSG_ReadByte( pbuf );	// rendermode
			R_Spray( pos, pos2, modelIndex, count, vel, random, flags );
		}
		else R_Sprite_Spray( pos, pos2, modelIndex, count, vel * 2.0f, random );
		break;
	case TE_ARMOR_RICOCHET:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		scale = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_RicochetSprite( pos, cl_sprite_ricochet, 0.1f, scale );
		R_RicochetSound( pos );
		break;
	case TE_PLAYERDECAL:
		color = MSG_ReadByte( pbuf ) - 1; // playernum
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		entityIndex = MSG_ReadShort( pbuf );
		decalIndex = MSG_ReadByte( pbuf );
		CL_PlayerDecal( color, decalIndex, entityIndex, pos );
		break;
	case TE_BUBBLES:
	case TE_BUBBLETRAIL:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		scale = MSG_ReadCoord( pbuf );	// water height
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		vel = MSG_ReadCoord( pbuf );
		if( type == TE_BUBBLES ) R_Bubbles( pos, pos2, scale, modelIndex, count, vel );
		else R_BubbleTrail( pos, pos2, scale, modelIndex, count, vel );
		break;
	case TE_BLOODSPRITE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );	// sprite #1
		decalIndex = MSG_ReadShort( pbuf );	// sprite #2
		color = MSG_ReadByte( pbuf );
		scale = (float)MSG_ReadByte( pbuf );
		R_BloodSprite( pos, color, modelIndex, decalIndex, scale );
		break;
	case TE_PROJECTILE:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		life = MSG_ReadByte( pbuf );
		color = MSG_ReadByte( pbuf );	// playernum
		R_Projectile( pos, pos2, modelIndex, life, color, NULL );
		break;
	case TE_PLAYERSPRITES:
		color = MSG_ReadShort( pbuf );	// entitynum
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		random = (float)MSG_ReadByte( pbuf );
		R_PlayerSprites( color, modelIndex, count, random );
		break;
	case TE_PARTICLEBURST:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		scale = (float)MSG_ReadShort( pbuf );
		color = MSG_ReadByte( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_ParticleBurst( pos, scale, color, life );
		break;
	case TE_FIREFIELD:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		scale = (float)MSG_ReadShort( pbuf );
		modelIndex = MSG_ReadShort( pbuf );
		count = MSG_ReadByte( pbuf );
		flags = MSG_ReadByte( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_FireField( pos, scale, modelIndex, count, flags, life );
		break;
	case TE_PLAYERATTACHMENT:
		color = MSG_ReadByte( pbuf );	// playernum
		scale = MSG_ReadCoord( pbuf );	// height
		modelIndex = MSG_ReadShort( pbuf );
		life = (float)(MSG_ReadShort( pbuf ) * 0.1f);
		R_AttachTentToPlayer( color, modelIndex, scale, life );
		break;
	case TE_KILLPLAYERATTACHMENTS:
		color = MSG_ReadByte( pbuf );	// playernum
		R_KillAttachedTents( color );
		break;
	case TE_MULTIGUNSHOT:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf ) * 0.1f;
		pos2[1] = MSG_ReadCoord( pbuf ) * 0.1f;
		pos2[2] = MSG_ReadCoord( pbuf ) * 0.1f;
		ang[0] = MSG_ReadCoord( pbuf ) * 0.01f;
		ang[1] = MSG_ReadCoord( pbuf ) * 0.01f;
		ang[2] = 0.0f;
		count = MSG_ReadByte( pbuf );
		decalIndices[0] = MSG_ReadByte( pbuf );
		R_MultiGunshot( pos, pos2, ang, count, 1, decalIndices );
		break;
	case TE_USERTRACER:
		pos[0] = MSG_ReadCoord( pbuf );
		pos[1] = MSG_ReadCoord( pbuf );
		pos[2] = MSG_ReadCoord( pbuf );
		pos2[0] = MSG_ReadCoord( pbuf );
		pos2[1] = MSG_ReadCoord( pbuf );
		pos2[2] = MSG_ReadCoord( pbuf );
		life = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		color = MSG_ReadByte( pbuf );
		scale = (float)(MSG_ReadByte( pbuf ) * 0.1f);
		R_UserTracerParticle( pos, pos2, life, color, scale, 0, NULL );
		break;
	default:
		Con_DPrintf( S_ERROR "%s: illegible TE message %i\n", __func__, type );
		break;
	}

	// throw warning
	if( MSG_CheckOverflow( pbuf ))
		Con_DPrintf( S_WARN "%s: overflow TE message %i\n", __func__, type );
}


/*
==============================================================

LIGHT STYLE MANAGEMENT

==============================================================
*/
#define STYLE_LERPING_THRESHOLD	3.0f // because we wan't interpolate fast sequences (like on\off)

/*
================
CL_ClearLightStyles
================
*/
static void CL_ClearLightStyles( void )
{
	memset( cl.lightstyles, 0, sizeof( cl.lightstyles ));
}

void CL_SetLightstyle( int style, const char *s, float f )
{
	if( unlikely( style < 0 || style >= MAX_LIGHTSTYLES ))
	{
		Con_Printf( S_WARN "%s: ignored invalid lightstyle id %d\n", __func__, style );
		return;
	}

	lightstyle_t *ls = &cl.lightstyles[style];

	ls->length = Q_strncpy( ls->pattern, s, sizeof( ls->pattern ));
	ls->time = f; // set local time

	for( int i = 0; i < ls->length; i++ )
		ls->map[i] = (float)(s[i] - 'a');

	ls->interp = (ls->length <= 1) ? false : true;

	// check for allow interpolate
	// NOTE: fast flickering styles looks ugly when interpolation is running
	for( int i = 0; i < ( ls->length - 1 ); i++ )
	{
		float val1 = ls->map[( i + 0 ) % ls->length];
		float val2 = ls->map[( i + 1 ) % ls->length];

		if( fabs( val1 - val2 ) > STYLE_LERPING_THRESHOLD )
		{
			ls->interp = false;
			break;
		}
	}

	if( ls->length >= 1 )
		Con_Reportf( "Lightstyle %i (%s), interp %s\n", style, ls->pattern, ls->interp ? "Yes" : "No" );
}

/*
==============================================================

DLIGHT MANAGEMENT

==============================================================
*/
static dlight_t cl_dlights[MAX_DLIGHTS];
static dlight_t cl_elights[MAX_ELIGHTS];

/*
================
CL_ClearDlights
================
*/
static void CL_ClearDlights( void )
{
	memset( cl_dlights, 0, sizeof( cl_dlights ));
	memset( cl_elights, 0, sizeof( cl_elights ));
}

/*
===============
CL_AllocDlight

===============
*/
dlight_t *CL_AllocDlight( int key )
{
	dlight_t	*dl;
	int	i;

	// first look for an exact key match
	if( key )
	{
		for( i = 0, dl = cl_dlights; i < MAX_DLIGHTS; i++, dl++ )
		{
			if( dl->key == key )
			{
				// reuse this light
				memset( dl, 0, sizeof( *dl ));
				dl->key = key;
				return dl;
			}
		}
	}

	// then look for anything else
	for( i = 0, dl = cl_dlights; i < MAX_DLIGHTS; i++, dl++ )
	{
		if( dl->die < cl.time && dl->key == 0 )
		{
			memset( dl, 0, sizeof( *dl ));
			dl->key = key;
			return dl;
		}
	}

	// otherwise grab first dlight
	dl = &cl_dlights[0];
	memset( dl, 0, sizeof( *dl ));
	dl->key = key;

	return dl;
}

/*
===============
CL_AllocElight

===============
*/
dlight_t *CL_AllocElight( int key )
{
	dlight_t	*dl;
	int	i;

	// first look for an exact key match
	if( key )
	{
		for( i = 0, dl = cl_elights; i < MAX_ELIGHTS; i++, dl++ )
		{
			if( dl->key == key )
			{
				// reuse this light
				memset( dl, 0, sizeof( *dl ));
				dl->key = key;
				return dl;
			}
		}
	}

	// then look for anything else
	for( i = 0, dl = cl_elights; i < MAX_ELIGHTS; i++, dl++ )
	{
		if( dl->die < cl.time && dl->key == 0 )
		{
			memset( dl, 0, sizeof( *dl ));
			dl->key = key;
			return dl;
		}
	}

	// otherwise grab first dlight
	dl = &cl_elights[0];
	memset( dl, 0, sizeof( *dl ));
	dl->key = key;

	return dl;
}

/*
===============
CL_DecayLights

===============
*/
void CL_DecayLights( void )
{
	const float time = cl.time;
	const float dt = cl.time - cl.oldtime;

	for( int i = 0; i < MAX_DLIGHTS; i++ )
	{
		dlight_t *dl = &cl_dlights[i];

		if( !dl->radius )
			continue;

		if( dl->die < time )
		{
			memset( dl, 0, sizeof( *dl ));
			continue;
		}

		dl->radius -= dt * dl->decay;
		if( dl->radius <= 0 )
			memset( dl, 0, sizeof( *dl ));
	}

	for( int i = 0; i < MAX_ELIGHTS; i++ )
	{
		dlight_t *dl = &cl_elights[i];

		if( !dl->radius )
			continue;

		if( dl->die < time )
		{
			memset( dl, 0, sizeof( *dl ));
			continue;
		}

		dl->radius -= dt * dl->decay;
		if( dl->radius <= 0 )
			memset( dl, 0, sizeof( *dl ));
	}
}

dlight_t *CL_GetDynamicLight( int number )
{
	Assert( number >= 0 && number < MAX_DLIGHTS );
	return &cl_dlights[number];
}

dlight_t *CL_GetEntityLight( int number )
{
	Assert( number >= 0 && number < MAX_ELIGHTS );
	return &cl_elights[number];
}

/*
================
CL_UpdateFlashlight

update client flashlight
================
*/
static void CL_UpdateFlashlight( cl_entity_t *ent )
{
	vec3_t		forward, view_ofs;
	vec3_t		vecSrc, vecEnd;
	pmtrace_t		trace;
	cl_entity_t	*hit;
	dlight_t		*dl;

	if( ent->index == ( cl.playernum + 1 ))
	{
		// local player case
		AngleVectors( cl.viewangles, forward, NULL, NULL );
		VectorCopy( cl.viewheight, view_ofs );
	}
	else	// non-local player case
	{
		vec3_t	v_angle;

		// NOTE: pitch divided by 3.0 twice. So we need apply 3^2 = 9
		v_angle[PITCH] = ent->curstate.angles[PITCH] * 9.0f;
		v_angle[YAW] = ent->angles[YAW];
		v_angle[ROLL] = 0.0f; // roll not used

		AngleVectors( v_angle, forward, NULL, NULL );
		view_ofs[0] = view_ofs[1] = 0.0f;

		// FIXME: these values are hardcoded ...
		if( ent->curstate.usehull == 1 )
			view_ofs[2] = 12.0f;	// VEC_DUCK_VIEW;
		else view_ofs[2] = 28.0f;		// DEFAULT_VIEWHEIGHT
	}

	VectorAdd( ent->origin, view_ofs, vecSrc );
	VectorMA( vecSrc, FLASHLIGHT_DISTANCE, forward, vecEnd );

	trace = CL_TraceLine( vecSrc, vecEnd, PM_STUDIO_BOX );

	// update flashlight endpos
	dl = CL_AllocDlight( ent->index );
#if 1
	hit = CL_GetEntityByIndex( clgame.pmove->physents[trace.ent].info );
	if( hit && hit->model && ( hit->model->type == mod_alias || hit->model->type == mod_studio ))
		VectorCopy( hit->origin, dl->origin );
	else VectorCopy( trace.endpos, dl->origin );
#else
	VectorCopy( trace->endpos, dl->origin );
#endif
	// compute falloff
	float falloff = trace.fraction * FLASHLIGHT_DISTANCE;
	if( falloff < 500.0f ) falloff = 1.0f;
	else falloff = 500.0f / falloff;
	falloff *= falloff;

	// apply brigthness to dlight
	dl->color.r = dl->color.g = dl->color.b = bound( 0, falloff * 255, 255 );
	dl->die = cl.time + 0.01f; // die on next frame
	dl->radius = 80;
}

static void R_EntityDimlight( cl_entity_t *ent, int key )
{
	dlight_t *dl = CL_AllocDlight( key );

	VectorCopy( ent->origin, dl->origin );
	dl->color.r = dl->color.g = dl->color.b = 100;
	dl->radius = COM_RandomFloat( 200.0f, 231.0f );
	dl->die = cl.time + 0.001;
}

static void R_EntityLight( cl_entity_t *ent, int key )
{
	dlight_t *dl = CL_AllocDlight( key );

	VectorCopy( ent->origin, dl->origin );
	dl->color.r = dl->color.g = dl->color.b = 100;
	dl->radius = 200;
	dl->die = cl.time + 0.001;

	R_RocketFlare( ent->origin );
}

static void R_EntityBrightlight( cl_entity_t *ent, int key, int radius )
{
	dlight_t *dl = CL_AllocDlight( key );

	VectorCopy( ent->origin, dl->origin );
	dl->origin[2] += 16.0f;
	dl->color.r = dl->color.g = dl->color.b = 250;
	if( !radius )
		dl->radius = COM_RandomFloat( 400.0f, 431.0f );
	else dl->radius = 400;
	dl->die = cl.time + 0.001;
}

/*
================
CL_AddEntityEffects

apply various effects to entity origin or attachment
================
*/
void CL_AddEntityEffects( cl_entity_t *ent )
{
	// players have special set of effects, from CL_LinkPlayers
	if( ent->player && ent->index != cl.viewentity )
	{
		if( FBitSet( ent->curstate.effects, EF_BRIGHTLIGHT ))
			R_EntityBrightlight( ent, ent->index /* 4 in GoldSrc */, 0 );

		if( FBitSet( ent->curstate.effects, EF_DIMLIGHT ))
			R_EntityDimlight( ent, ent->index /* 4 in GoldSrc */ );
	}
	else if( RP_LOCALCLIENT( ent ))
	{
		// from CL_PlayerFlashlight
		if( FBitSet( ent->curstate.effects, EF_BRIGHTLIGHT ))
			R_EntityBrightlight( ent, ent->index /* 1 in GoldSrc */, 400 );
		else if( FBitSet( ent->curstate.effects, EF_DIMLIGHT ))
		{
			if( Host_IsQuakeCompatible( ))
				R_EntityDimlight( ent, ent->index );
			else
				CL_UpdateFlashlight( ent );
		}
	}
	else
	{
		// from CL_LinkPacketEntities
		if( FBitSet( ent->curstate.effects, EF_BRIGHTFIELD ))
			R_EntityParticles( ent );

		if( FBitSet( ent->curstate.effects, EF_BRIGHTLIGHT ))
			R_EntityBrightlight( ent, ent->index, 0 );

		if( FBitSet( ent->curstate.effects, EF_DIMLIGHT ))
			R_EntityDimlight( ent, ent->index );

		if( FBitSet( ent->curstate.effects, EF_LIGHT ))
			R_EntityLight( ent, ent->curstate.number );
	}

	// studio models are handle muzzleflashes difference
	if( FBitSet( ent->curstate.effects, EF_MUZZLEFLASH ) && ent->model && ent->model->type == mod_alias )
	{
		dlight_t	*dl = CL_AllocDlight( ent->index );
		vec3_t	fv;

		ClearBits( ent->curstate.effects, EF_MUZZLEFLASH );
		dl->color.r = dl->color.g = dl->color.b = 100;
		VectorCopy( ent->origin, dl->origin );
		AngleVectors( ent->angles, fv, NULL, NULL );
		dl->origin[2] += 16.0f;
		VectorMA( dl->origin, 18, fv, dl->origin );
		dl->radius = COM_RandomFloat( 200, 231 );
		dl->die = cl.time + 0.1;
		dl->minlight = 32;
	}
}

/*
================
CL_AddModelEffects

these effects will be enable by flag in model header
================
*/
void CL_AddModelEffects( cl_entity_t *ent )
{
	vec3_t	neworigin;
	vec3_t	oldorigin;

	if( !ent->model || ent->player )
		return;

	if( ent->model->type != mod_alias && ent->model->type != mod_studio )
		return;

	if( cls.demoplayback == DEMO_QUAKE1 )
	{
		VectorCopy( ent->baseline.vuser1, oldorigin );
		VectorCopy( ent->origin, ent->baseline.vuser1 );
		VectorCopy( ent->origin, neworigin );
	}
	else
	{
		VectorCopy( ent->prevstate.origin, oldorigin );
		VectorCopy( ent->curstate.origin, neworigin );
	}

	// NOTE: this completely over control about angles and don't broke interpolation
	if( FBitSet( ent->model->flags, STUDIO_ROTATE ))
		ent->angles[1] = anglemod( 100.0f * cl.time );

	if( FBitSet( ent->model->flags, STUDIO_GIB ))
		R_RocketTrail( oldorigin, neworigin, 2 );
	else if( FBitSet( ent->model->flags, STUDIO_ZOMGIB ))
		R_RocketTrail( oldorigin, neworigin, 4 );
	else if( FBitSet( ent->model->flags, STUDIO_TRACER ))
		R_RocketTrail( oldorigin, neworigin, 3 );
	else if( FBitSet( ent->model->flags, STUDIO_TRACER2 ))
		R_RocketTrail( oldorigin, neworigin, 5 );
	else if( FBitSet( ent->model->flags, STUDIO_ROCKET ))
	{
		dlight_t	*dl = CL_AllocDlight( ent->curstate.number );

		dl->color.r = dl->color.g = dl->color.b = 200;
		VectorCopy( ent->origin, dl->origin );

		// XASH SPECIFIC: get radius from head entity
		if( ent->curstate.rendermode != kRenderNormal )
			dl->radius = Q_max( 0, ent->curstate.renderamt - 55 );
		else dl->radius = 200;

		dl->die = cl.time + 0.01f;

		R_RocketTrail( oldorigin, neworigin, 0 );
	}
	else if( FBitSet( ent->model->flags, STUDIO_GRENADE ))
		R_RocketTrail( oldorigin, neworigin, 1 );
	else if( FBitSet( ent->model->flags, STUDIO_TRACER3 ))
		R_RocketTrail( oldorigin, neworigin, 6 );
}

/*
================
CL_TestLights

if cl_testlights is set, create 32 lights models
================
*/
void CL_TestLights( void )
{
	vec3_t	forward, right;

	if( !cl_testlights.value )
		return;

	int numLights = bound( 1, cl_testlights.value, MAX_DLIGHTS );
	AngleVectors( cl.viewangles, forward, right, NULL );

	for( int i = 0; i < numLights; i++ )
	{
		dlight_t *dl = &cl_dlights[i];

		float r = 64 * ((i % 4) - 1.5f );
		float f = 64 * ( i / 4) + 128;

		for( int j = 0; j < 3; j++ )
			dl->origin[j] = cl.simorg[j] + forward[j] * f + right[j] * r;

		dl->color.r = ((((i % 6) + 1) & 1)>>0) * 255;
		dl->color.g = ((((i % 6) + 1) & 2)>>1) * 255;
		dl->color.b = ((((i % 6) + 1) & 4)>>2) * 255;
		dl->radius = Q_max( 64, 200 - 5 * numLights );
		dl->die = cl.time + host.frametime;
	}
}

/*
==============================================================

DECAL MANAGEMENT

==============================================================
*/
/*
===============
CL_FireCustomDecal

custom temporary decal
===============
*/
void GAME_EXPORT CL_FireCustomDecal( int textureIndex, int entityIndex, int modelIndex, float *pos, int flags, float scale )
{
	ref.dllFuncs.R_DecalShoot( textureIndex, entityIndex, modelIndex, pos, flags, scale );
}

/*
===============
CL_DecalShoot

normal temporary decal
===============
*/
void GAME_EXPORT CL_DecalShoot( int textureIndex, int entityIndex, int modelIndex, float *pos, int flags )
{
	CL_FireCustomDecal( textureIndex, entityIndex, modelIndex, pos, flags, 1.0f );
}

/*
===============
CL_PlayerDecal

spray custom colored decal (clan logo etc)
===============
*/
static void CL_PlayerDecal( int playernum, int customIndex, int entityIndex, float *pos )
{
	int		textureIndex = 0;
	customization_t	*pCust = NULL;

	if( playernum < MAX_CLIENTS )
		pCust = cl.players[playernum].customdata.pNext;

	if( pCust != NULL && pCust->pBuffer != NULL && pCust->pInfo != NULL )
	{
		if( FBitSet( pCust->resource.ucFlags, RES_CUSTOM ) && pCust->resource.type == t_decal && pCust->bTranslated )
		{
			if( !pCust->nUserData1 )
			{
				char decalname[MAX_VA_STRING];
				int width, height;

				Q_snprintf( decalname, sizeof( decalname ), "player%dlogo%d", playernum, customIndex );
				textureIndex = ref.dllFuncs.GL_FindTexture( decalname );
				if( textureIndex != 0 )
					ref.dllFuncs.GL_FreeTexture( textureIndex );

				pCust->nUserData1 = GL_LoadTextureInternal( decalname, pCust->pInfo, TF_DECAL );

				width = REF_GET_PARM( PARM_TEX_WIDTH, pCust->nUserData1 );
				height = REF_GET_PARM( PARM_TEX_HEIGHT, pCust->nUserData1 );

				if( width > cl_logomaxdim.value || height > cl_logomaxdim.value )
				{
					double scale = cl_logomaxdim.value / Q_max( width, height );
					width = round( width * scale );
					height = round( height * scale );
					ref.dllFuncs.R_OverrideTextureSourceSize( pCust->nUserData1, width, height ); // default custom decal from HL1
				}
			}
			textureIndex = pCust->nUserData1;
		}
	}

	CL_DecalShoot( textureIndex, entityIndex, 0, pos, FDECAL_CUSTOM );
}

/*
===============
CL_DecalIndexFromName

get decal global index from decalname
===============
*/
int GAME_EXPORT CL_DecalIndexFromName( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return 0;

	// look through the loaded sprite name list for SpriteName
	for( int i = 1; i < MAX_DECALS && host.draw_decals[i][0]; i++ )
	{
		if( !Q_stricmp( name, host.draw_decals[i] ))
			return i;
	}
	return 0; // invalid decal
}

/*
===============
CL_DecalIndex

get texture index from decal global index
===============
*/
#if XASH_GAMECUBE
/*
=============
CL_GCEnsureLeanDecalKind / CL_GCEnsureLeanDecalForName

G293/G295: tip-safe 16×16 embeds — no disc/WAD alloc (bootstrap TGA inject
hung New Game; fat decals.wad MIPs tip under memopt).
Kinds: shot (gun), scorch (burn), blood, break (glass/crack).
=============
*/
typedef enum
{
	GC_LEAN_DECAL_SHOT = 0,
	GC_LEAN_DECAL_SCORCH,
	GC_LEAN_DECAL_BLOOD,
	GC_LEAN_DECAL_BREAK,
	GC_LEAN_DECAL_KINDS
} gc_lean_decal_kind_t;

static int CL_GCEnsureLeanDecalKind( gc_lean_decal_kind_t kind )
{
	static int tex[GC_LEAN_DECAL_KINDS];
	static byte pixels[GC_LEAN_DECAL_KINDS][16 * 16 * 4];
	static const char *names[GC_LEAN_DECAL_KINDS] = {
		"#gc_lean_shot1",
		"#gc_lean_scorch1",
		"#gc_lean_blood1",
		"#gc_lean_break1",
	};
	static const byte rgb[GC_LEAN_DECAL_KINDS][3] = {
		{ 48, 36, 24 },
		{ 28, 24, 20 },
		{ 140, 24, 24 },
		{ 160, 170, 180 },
	};
	int x, y;
	byte *dst;

	if( kind < 0 || kind >= GC_LEAN_DECAL_KINDS )
		kind = GC_LEAN_DECAL_SHOT;
	if( tex[kind] > 0 )
		return tex[kind];

	dst = pixels[kind];
	for( y = 0; y < 16; y++ )
	{
		for( x = 0; x < 16; x++ )
		{
			float dx = (float)x - 7.5f;
			float dy = (float)y - 7.5f;
			float d = sqrtf( dx * dx + dy * dy );
			int i = ( y * 16 + x ) * 4;
			byte a;

			if( d > 7.5f )
				a = 0;
			else if( kind == GC_LEAN_DECAL_BREAK )
			{
				/* Thin cross crack. */
				int ax = x - 8;
				int ay = y - 8;
				if( ax < 0 ) ax = -ax;
				if( ay < 0 ) ay = -ay;
				a = ( ax <= 1 || ay <= 1 )
					? (byte)( 200.0f * ( 1.0f - d / 7.5f )) : 0;
			}
			else
				a = (byte)( 220.0f * ( 1.0f - d / 7.5f ));
			dst[i + 0] = rgb[kind][0];
			dst[i + 1] = rgb[kind][1];
			dst[i + 2] = rgb[kind][2];
			dst[i + 3] = a;
		}
	}

	tex[kind] = ref.dllFuncs.GL_CreateTexture( names[kind], 16, 16, dst,
		TF_DECAL | TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA );
	if( tex[kind] > 0 )
		Con_Reportf( "Xash3D GameCube: G295 lean %s decal tex=%d (embedded 16x16)\n",
			names[kind] + 9, tex[kind] ); /* skip "#gc_lean_" */
	return tex[kind];
}

int CL_GCEnsureLeanShotDecal( void )
{
	return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_SHOT );
}

int CL_GCEnsureLeanDecalForName( const char *name )
{
	const char *n;

	if( !name || !name[0] )
		return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_SHOT );

	n = name;
	if( n[0] == '{' )
		n++;

	if( !Q_strnicmp( n, "blood", 5 ) || !Q_strnicmp( n, "yblood", 6 )
		|| !Q_strnicmp( n, "bigblood", 8 ) || !Q_strnicmp( n, "bloodhand", 9 ))
		return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_BLOOD );
	if( !Q_strnicmp( n, "scorch", 6 ) || !Q_strnicmp( n, "smscorch", 8 )
		|| !Q_strnicmp( n, "oil", 3 ) || !Q_strnicmp( n, "K_slime", 7 ))
		return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_SCORCH );
	if( !Q_strnicmp( n, "break", 5 ) || !Q_strnicmp( n, "crack", 5 ))
		return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_BREAK );
	/* shot / bigshot / gaussshot / hand / default tip-safe fallback */
	return CL_GCEnsureLeanDecalKind( GC_LEAN_DECAL_SHOT );
}
#endif

/*
===============
CL_DecalIndex

get texture index from decal global index
===============
*/
int GAME_EXPORT CL_DecalIndex( int id )
{
	id = bound( 0, id, MAX_DECALS - 1 );

	if( cl.decal_index[id] == 0 )
	{
		int gl_texturenum = 0;

		Image_SetForceFlags( IL_LOAD_DECAL );

#if XASH_GAMECUBE
		/*
		 * G295: under memopt map all `{` decals to lean embeds — never open
		 * fat decals.wad MIPs (W_ReadLump tip).
		 */
		if( GC_MapLoadMemoryOpt() && host.draw_decals[id][0] == '{' )
		{
			gl_texturenum = CL_GCEnsureLeanDecalForName( host.draw_decals[id] );
			cl.decal_index[id] = gl_texturenum;
			Image_ClearForceFlags();
			return gl_texturenum;
		}
#endif

		if( Mod_AllowMaterials( ))
		{
			string decalname;

			if( Q_snprintf( decalname, sizeof( decalname ), "materials/decals/%s.tga", host.draw_decals[id] ) > 0 )
			{
				if( g_fsapi.FileExists( decalname, false ))
				{
					gl_texturenum = ref.dllFuncs.GL_LoadTexture( decalname, NULL, 0, TF_DECAL );
					if( host_allow_materials.value == 2.0f )
						Con_Printf( "Looking for %s decal replacement...%s (%s)\n", host.draw_decals[id], gl_texturenum != 0 ? S_GREEN "OK" : S_RED "FAIL", decalname );
				}
				else if( host_allow_materials.value == 2.0f )
					Con_Printf( "Looking for %s decal replacement..." S_YELLOW "MISS (%s)\n", host.draw_decals[id], decalname );
			}
			else if( host_allow_materials.value == 2.0f )
				Con_Printf( "Looking for %s decal replacement..." S_YELLOW "MISS (overflow)\n", host.draw_decals[id] );

			if( gl_texturenum )
			{
				byte *fin;

#if XASH_GAMECUBE
				/* G293: lean embed already sized — skip fat wad reopen under tip. */
				if( !GC_MapLoadMemoryOpt())
#endif
				{
					Q_snprintf( decalname, sizeof( decalname ), "decals.wad/%s", host.draw_decals[id] );

					if(( fin = g_fsapi.LoadFile( decalname, NULL, false )) != NULL )
					{
						mip_t *mip = (mip_t *)fin;
						ref.dllFuncs.R_OverrideTextureSourceSize( gl_texturenum, mip->width, mip->height );
						Mem_Free( fin );
					}
				}
			}
		}

		if( !gl_texturenum )
			gl_texturenum = ref.dllFuncs.GL_LoadTexture( host.draw_decals[id], NULL, 0, TF_DECAL );

		cl.decal_index[id] = gl_texturenum;
		Image_ClearForceFlags();
	}

	return cl.decal_index[id];
}

/*
===============
CL_DecalRemoveAll

remove all decals with specified texture
===============
*/
void GAME_EXPORT CL_DecalRemoveAll( int textureIndex )
{
	int id = bound( 0, textureIndex, MAX_DECALS - 1 );
	ref.dllFuncs.R_DecalRemoveAll( cl.decal_index[id] );
}

/*
==============================================================

EFRAGS MANAGEMENT

==============================================================
*/
/*
=======================
CL_ClearStaticEntities

e.g. by demo request
=======================
*/
void CL_ClearStaticEntities( void )
{
	if( host.type == HOST_DEDICATED )
		return;

	// clear out efrags in case the level hasn't been reloaded
	for( int i = 0; i < cl.worldmodel->numleafs; i++ )
		cl.worldmodel->leafs[i+1].efrags = NULL;

	clgame.numStatics = 0;

	CL_ClearEfrags ();
}

/*
==============
CL_ClearEffects
==============
*/
void CL_ClearEffects( void )
{
	CL_ClearEfrags ();
	CL_ClearDlights ();
	CL_ClearTempEnts ();
	CL_ClearViewBeams ();
	CL_ClearParticles ();
	CL_ClearLightStyles ();
}
