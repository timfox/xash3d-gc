/*
r_gx_face.c - lip sync on Flipper-skinned actors (GCFA)
Copyright (C) 2026 xash3d-gc contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Source moves a face with flexes: vertex offsets weighed by flex rules. The
bake (lostcoast-gc bake_lostcoast_studio.py write_face) runs the rules for
every phoneme and writes, per face shape the phonemes reach, the offsets it
gives the actor's vertices in the position array's own units, and per
phoneme one weight per shape: gc_studio/<model>.gcf.

The voice stream says which phoneme is being spoken and which comes next
(GC_VoiceLip). Thirty times a second the two phonemes' weights are blended,
the shapes with weight are read from ARAM and summed, and the difference to
what is already applied is added to the actor's position array, which
Flipper reads through the display lists as it is.

  0   "GCFA", u16 version (2), u16 nshape
  8   u16 nslot, u16 nphon, u8 shift, 3 x 0
  16  u32 slots_ofs, u32 dir_ofs, u32 phon_ofs, u32 size
  slots: nslot x u16: index into LOD 0's position array
  dir:   nshape x 8: u32 ofs, u16 count, u16 0
  entries: count x 6: u16 slot table index, s8 dx, dy, dz (position units
         >> shift), u8 0
  phon:  nphon x (u16 code, nshape x s8 (weight - resting weight) * 16),
         padded to even
*/

#include "r_local.h"

#if XASH_GAMECUBE
#include <gccore.h>
#include <string.h>
#include "../../engine/platform/gamecube/mem_gamecube.h"

extern qboolean GC_VoiceLip( int ent, int *code_a, int *code_b, float *frac );
extern const char *GC_ActorFaceAt( int ent, float time, float *seconds );

#define GF_SLOTS	1280
#define GF_SHAPES	64
#define GF_PHONS	64
#define GF_STEP		( 1.0f / 30.0f )
#define GF_CHUNK	170	/* entries read from ARAM at once (1020 bytes) */

static u32 GfU32( const byte *p ) { return ((u32)p[0] << 24 ) | ((u32)p[1] << 16 ) | ((u32)p[2] << 8 ) | p[3]; }
static u16 GfU16( const byte *p ) { return (u16)(( p[0] << 8 ) | p[1] ); }

static struct
{
	int		state;	/* 0 untried, 1 ready, -1 failed */
	const model_t	*model;
	int		asset;
	int		nshape, nslot, nphon, shift, row;
	u32		phon_ofs;
	u16		*slot;			/* nslot, map-load arena tail */
	u16		slot_lo, slot_hi;
	struct { u32 ofs; u16 count; } shape[GF_SHAPES];
	u16		code[GF_PHONS];
	s8		(*applied)[3];		/* what the position array carries now; arena tail */
	qboolean	dirty;			/* applied is not all zero */
	int		row_code[2];		/* phonemes whose weights are loaded */
	s8		row_w[2][GF_SHAPES];
	float		last;
	/* the scene's facial track (scripts/lc/<scene>.gft): "GCFT", u16
	 * version (2), u16 nshape, u16 nsamples, u16 hz * 256, then nsamples x
	 * nshape x s8 ((weight - resting weight) * 16) */
	char		track[64];
	int		track_asset;	/* -1 = not loaded */
	int		track_n;
	float		track_hz;
} gf;

/* Bring a facial track into ARAM. */
static qboolean R_GXFaceTrack( const char *path )
{
	gc_aram_cache_t *cache = GC_AramAssetCacheGet();
	byte buf[1024];
	u32 size, done;
	int index;
	file_t *f;

	if( !Q_strcmp( gf.track, path ))
		return gf.track_asset >= 0;
	Q_strncpy( gf.track, path, sizeof( gf.track ));
	gf.track_asset = -1;
	if( !cache )
		return false;
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	if( gEngfuncs.fsapi->Read( f, buf, 12 ) != 12 || memcmp( buf, "GCFT", 4 ) || GfU16( buf + 4 ) != 2
		|| GfU16( buf + 6 ) != gf.nshape )
	{
		gEngfuncs.fsapi->Close( f );
		return false;
	}
	gf.track_n = GfU16( buf + 8 );
	gf.track_hz = GfU16( buf + 10 ) / 256.0f;
	size = 12 + (u32)gf.track_n * gf.nshape;
	gEngfuncs.fsapi->Seek( f, 0, SEEK_SET );
	index = GC_AramCacheFind( cache, path );
	if( index < 0 )
	{
		index = GC_AramCacheBegin( cache, path, size );
		for( done = 0; index >= 0 && done < size; )
		{
			const u32 n = Q_min( size - done, (u32)sizeof( buf ));

			if( gEngfuncs.fsapi->Read( f, buf, n ) != (fs_offset_t)n || !GC_AramCacheWrite( cache, index, done, buf, n ))
			{
				GC_AramCacheAbort( cache, index );
				index = -1;
				break;
			}
			done += n;
		}
		if( index >= 0 && !GC_AramCacheCommit( cache, index ))
			index = -1;
	}
	gEngfuncs.fsapi->Close( f );
	gf.track_asset = index;
	gEngfuncs.Con_Reportf( "Xash3D GameCube: face track %s: %d samples at %.0f Hz %s\n", path, gf.track_n, gf.track_hz,
		index >= 0 ? "in ARAM" : "FAILED" );
	return index >= 0;
}


static qboolean R_GXFaceLoad( const model_t *mod )
{
	gc_aram_cache_t *cache = GC_AramAssetCacheGet();
	char path[64], base[32];
	byte buf[1024];
	u32 size, slots_ofs, dir_ofs, done;
	int i, index;
	file_t *f;

	gf.state = -1;
	gf.model = mod;
	if( !cache )
		return false;
	COM_FileBase( mod->name, base, sizeof( base ));
	Q_snprintf( path, sizeof( path ), "gc_studio/%s.gcf", base );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	if( gEngfuncs.fsapi->Read( f, buf, 32 ) != 32 || memcmp( buf, "GCFA", 4 ) || GfU16( buf + 4 ) != 2 )
		goto fail;
	gf.nshape = GfU16( buf + 6 );
	gf.nslot = GfU16( buf + 8 );
	gf.nphon = GfU16( buf + 10 );
	gf.shift = buf[12];
	slots_ofs = GfU32( buf + 16 );
	dir_ofs = GfU32( buf + 20 );
	gf.phon_ofs = GfU32( buf + 24 );
	size = GfU32( buf + 28 );
	gf.row = ( 2 + gf.nshape + 1 ) & ~1;
	if( gf.nshape > GF_SHAPES || gf.nslot > GF_SLOTS || gf.nphon > GF_PHONS || gf.shift > 8 )
		goto fail;
	/* MEM1's heap is down to a few KB on Lost Coast: the working tables go
	 * in what the map-load arena has left over, not in static data. */
	if( !gf.slot && GC_MapLoadBufferTailPeak() != 0 )
	{
		byte *mem = (byte *)GC_MapLoadBufferTailAlloc(
			Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()), (size_t)gf.nslot * 5 );

		if( mem )
		{
			gf.slot = (u16 *)mem;
			gf.applied = (s8 (*)[3])( mem + (size_t)gf.nslot * 2 );
		}
	}
	if( !gf.slot )
		goto fail;
	gEngfuncs.fsapi->Seek( f, slots_ofs, SEEK_SET );
	if( gEngfuncs.fsapi->Read( f, gf.slot, gf.nslot * 2 ) != gf.nslot * 2 )
		goto fail;	/* big-endian on disc, as the CPU */
	gf.slot_lo = 0xffff;
	gf.slot_hi = 0;
	for( i = 0; i < gf.nslot; i++ )
	{
		if( gf.slot[i] < gf.slot_lo ) gf.slot_lo = gf.slot[i];
		if( gf.slot[i] > gf.slot_hi ) gf.slot_hi = gf.slot[i];
	}
	gEngfuncs.fsapi->Seek( f, dir_ofs, SEEK_SET );
	for( i = 0; i < gf.nshape; i++ )
	{
		if( gEngfuncs.fsapi->Read( f, buf, 8 ) != 8 )
			goto fail;
		gf.shape[i].ofs = GfU32( buf );
		gf.shape[i].count = GfU16( buf + 4 );
	}
	gEngfuncs.fsapi->Seek( f, gf.phon_ofs, SEEK_SET );
	for( i = 0; i < gf.nphon; i++ )
	{
		if( gEngfuncs.fsapi->Read( f, buf, gf.row ) != gf.row )
			goto fail;
		gf.code[i] = GfU16( buf );
	}
	/* the whole file to ARAM, a block at a time */
	gEngfuncs.fsapi->Seek( f, 0, SEEK_SET );
	index = GC_AramCacheFind( cache, path );
	if( index < 0 )
	{
		index = GC_AramCacheBegin( cache, path, size );
		if( index < 0 )
			goto fail;
		for( done = 0; done < size; )
		{
			const u32 n = Q_min( size - done, (u32)sizeof( buf ));

			if( gEngfuncs.fsapi->Read( f, buf, n ) != (fs_offset_t)n || !GC_AramCacheWrite( cache, index, done, buf, n ))
			{
				GC_AramCacheAbort( cache, index );
				goto fail;
			}
			done += n;
		}
		if( !GC_AramCacheCommit( cache, index ))
			goto fail;
	}
	GC_AramCachePin( cache, index );
	gEngfuncs.fsapi->Close( f );
	gf.asset = index;
	gf.row_code[0] = gf.row_code[1] = -1;
	gf.track[0] = 0;
	gf.track_asset = -1;
	memset( gf.applied, 0, (size_t)gf.nslot * 3 );
	gf.dirty = false;
	gf.state = 1;
	gEngfuncs.Con_Reportf( "Xash3D GameCube: face %s: %d shapes, %d vertices, %d phonemes, %u bytes in ARAM\n",
		path, gf.nshape, gf.nslot, gf.nphon, (unsigned)size );
	return true;
fail:
	gEngfuncs.fsapi->Close( f );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: face %s: load failed\n", path );
	return false;
}

/* Load the model's face shapes now (the first frame of a level), not at the
 * first word spoken: reading 40 KB off the disc costs a visible frame. */
void R_GXFaceWarm( const model_t *mod )
{
	if( mod && gf.state == 0 )
		R_GXFaceLoad( mod );
}

/* The shape weights of a phoneme (0 = rest) into row k. */
static void R_GXFaceRow( int k, int code )
{
	byte buf[2 + GF_SHAPES + 2];
	int i;

	if( gf.row_code[k] == code )
		return;
	gf.row_code[k] = code;
	memset( gf.row_w[k], 0, sizeof( gf.row_w[k] ));
	if( !code )
		return;
	for( i = 0; i < gf.nphon; i++ )
	{
		if( gf.code[i] != code )
			continue;
		if( GC_AramCacheRead( GC_AramAssetCacheGet(), gf.asset, gf.phon_ofs + (u32)i * gf.row, buf, gf.row ))
			memcpy( gf.row_w[k], buf + 2, gf.nshape );
		return;
	}
}

/* Move the mouth of e to what it is saying. pos is LOD 0's position array
 * (s16 x 3 per vertex) inside the loaded actor. */
void R_GXFaceApply( const cl_entity_t *e, const model_t *mod, s16 *pos, int nverts )
{
	s16 acc[GF_SLOTS][3];
	byte chunk[GF_CHUNK * 6];
	s8 tw[2][GF_SHAPES];
	int code_a = 0, code_b = 0, i, k;
	float frac = 0.0f, tsec = 0.0f, tfrac = 0.0f;
	const float now = gp_cl ? (float)gp_cl->time : 0.0f;
	const char *track;
	qboolean speaking, any = false, tracked = false;

	if( !e || e->index <= 0 || !mod || !pos )
		return;
	speaking = GC_VoiceLip( e->index, &code_a, &code_b, &frac );
	track = GC_ActorFaceAt( e->index, now, &tsec );
	if( !speaking && !track && ( gf.state != 1 || gf.model != mod || !gf.dirty ))
		return;
	if( gf.state == 0 || ( gf.state == -1 && gf.model != mod ))
		R_GXFaceLoad( mod );
	if( gf.state != 1 || gf.model != mod || gf.slot_hi >= nverts )
		return;
	if( now >= gf.last && now - gf.last < GF_STEP )
		return;
	gf.last = now;
	R_GXFaceRow( 0, code_a );
	R_GXFaceRow( 1, code_b );
	/* the scene's brows, eyelids and cheeks: the two samples either side */
	if( track && R_GXFaceTrack( track ) && gf.track_n > 0 )
	{
		const float fi = tsec * gf.track_hz;
		const int i0 = bound( 0, (int)fi, gf.track_n - 1 );
		const int i1 = Q_min( i0 + 1, gf.track_n - 1 );

		tfrac = bound( 0.0f, fi - i0, 1.0f );
		if( GC_AramCacheRead( GC_AramAssetCacheGet(), gf.track_asset, 12 + (u32)i0 * gf.nshape, tw[0], gf.nshape )
			&& GC_AramCacheRead( GC_AramAssetCacheGet(), gf.track_asset, 12 + (u32)i1 * gf.nshape, tw[1], gf.nshape ))
			tracked = true;
	}
	memset( acc, 0, sizeof( acc[0] ) * gf.nslot );
	for( i = 0; i < gf.nshape; i++ )
	{
		/* weight * 16 (signed, from the resting face), blended, in 1/256 */
		/* (the flex rules are near enough linear that the scene's weights and
		 * the phoneme's add) */
		const int w = (int)(( gf.row_w[0][i] * ( 1.0f - frac ) + gf.row_w[1][i] * frac
			+ ( tracked ? tw[0][i] * ( 1.0f - tfrac ) + tw[1][i] * tfrac : 0.0f )) * 16.0f );
		u32 ofs = gf.shape[i].ofs;
		int left = gf.shape[i].count;

		if( !w )
			continue;
		any = true;
		while( left > 0 )
		{
			const int n = Q_min( left, GF_CHUNK );
			const byte *p = chunk;

			if( !GC_AramCacheRead( GC_AramAssetCacheGet(), gf.asset, ofs, chunk, (u32)n * 6 ))
				return;
			for( k = 0; k < n; k++, p += 6 )
			{
				const int si = ( p[0] << 8 ) | p[1];

				if( si >= gf.nslot )
					continue;
				acc[si][0] += (s16)(( (s8)p[2] * w ) >> 8 );
				acc[si][1] += (s16)(( (s8)p[3] * w ) >> 8 );
				acc[si][2] += (s16)(( (s8)p[4] * w ) >> 8 );
			}
			ofs += (u32)n * 6;
			left -= n;
		}
	}
	if( !any && !gf.dirty )
		return;
	gf.dirty = false;
	for( i = 0; i < gf.nslot; i++ )
	{
		s16 *v = pos + gf.slot[i] * 3;

		for( k = 0; k < 3; k++ )
		{
			const int want = bound( -127, acc[i][k], 127 );

			if( want != gf.applied[i][k] )
			{
				v[k] = (s16)( v[k] + (( want - gf.applied[i][k] ) << gf.shift ));
				gf.applied[i][k] = (s8)want;
			}
			if( want )
				gf.dirty = true;
		}
	}
	DCStoreRange( pos + gf.slot_lo * 3, ( gf.slot_hi - gf.slot_lo + 1 ) * 6 );
}

#endif // XASH_GAMECUBE
