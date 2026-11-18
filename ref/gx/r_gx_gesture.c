/*
r_gx_gesture.c - choreography gestures over a studio actor's pose (GCGS)
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
A Source gesture is a stack of animation layers. lostcoast-gc
tools/bake_lostcoast_gestures.py runs that stack on the host and keeps, per
bone, the local rotation it adds over the actor's talking pose, at 10 Hz:
scripts/lc/<model>.gst. The file lives in ARAM; only the two frames either
side of each playing gesture are held in MEM1.

  0   "GCGS", u16 version (2), u16 ngest
  8   u16 nbones, u16 0, u32 names_ofs, u32 dir_ofs
  names: nbones x char[32]
  dir:   ngest x 48: char name[28], u16 nframes, u8 nrot, u8 npos,
         f32 seconds, u32 bones_ofs, u32 frames_ofs, u32 0
  bones: nrot x u8, then npos x u8 (indices into names)
  frames: nframes x ( nrot x 3 x s16 (quat x, y, z * 32767, w >= 0),
          npos x 3 x s16 (translation * 256, in the parent's space) )
*/

#include "r_local.h"

#if XASH_GAMECUBE
#include <gccore.h>
#include <string.h>
#include "../../engine/platform/gamecube/mem_gamecube.h"

extern int GC_ActorGesturesAt( int ent, float time, const char **names, float *cycles, float *weights, int max );

#define GST_MAX		24	/* gestures and look poses in a file */
#define GST_BONES	64	/* bones named by a file */
#define GST_ROT		48	/* bones one gesture turns */
#define GST_POS		8	/* bones one gesture also moves */
#define GST_SLOTS	10	/* gestures and look poses playing at once */

typedef struct
{
	char	name[28];
	u16	nframes;
	u8	nrot, npos;
	u32	frames_ofs;
	u8	bone[GST_ROT + GST_POS];	/* studio bone, 0xff = not in this model; turned, then moved */
} gst_gesture_t;

/* MEM1 is down to a few KB of heap on Lost Coast, so nothing here is static
 * data: the directory sits in what the map-load arena has left over, and
 * the two frames a playing gesture needs are read from ARAM onto the stack
 * each time (a few hundred bytes a gesture). */
static struct
{
	int		state;	/* 0 untried, 1 ready, -1 failed */
	const model_t	*model;
	int		asset;
	int		ngest;
	gst_gesture_t	*gest;	/* GST_MAX, map-load arena tail */
} gst;

static u32 GstU32( const byte *p ) { return ((u32)p[0] << 24 ) | ((u32)p[1] << 16 ) | ((u32)p[2] << 8 ) | p[3]; }
static u16 GstU16( const byte *p ) { return (u16)(( p[0] << 8 ) | p[1] ); }

static qboolean R_GXGestureLoad( const model_t *mod, const studiohdr_t *hdr )
{
	gc_aram_cache_t *cache = GC_AramAssetCacheGet();
	const mstudiobone_t *bones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );
	char path[64], base[32];
	byte buf[1024];
	u8 bonemap[GST_BONES];
	u32 size, names_ofs, dir_ofs, done;
	int i, j, nbones, index;
	file_t *f;

	gst.state = -1;
	gst.model = mod;
	if( !cache )
		return false;
	if( !gst.gest && GC_MapLoadBufferTailPeak() != 0 )
		gst.gest = (gst_gesture_t *)GC_MapLoadBufferTailAlloc(
			Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()), sizeof( gst_gesture_t ) * GST_MAX );
	if( !gst.gest )
		return false;
	COM_FileBase( mod->name, base, sizeof( base ));
	Q_snprintf( path, sizeof( path ), "scripts/lc/%s.gst", base );
	f = gEngfuncs.fsapi->Open( path, "rb", false );
	if( !f )
		return false;
	if( gEngfuncs.fsapi->Read( f, buf, 32 ) != 32 || memcmp( buf, "GCGS", 4 ) || GstU16( buf + 4 ) != 2 )
		goto fail;
	gst.ngest = GstU16( buf + 6 );
	nbones = GstU16( buf + 8 );
	names_ofs = GstU32( buf + 12 );
	dir_ofs = GstU32( buf + 16 );
	if( gst.ngest > GST_MAX || nbones > GST_BONES )
		goto fail;
	gEngfuncs.fsapi->Seek( f, names_ofs, SEEK_SET );
	for( i = 0; i < nbones; i++ )
	{
		if( gEngfuncs.fsapi->Read( f, buf, 32 ) != 32 )
			goto fail;
		buf[31] = 0;
		bonemap[i] = 0xff;
		for( j = 0; j < hdr->numbones; j++ )
		{
			if( !Q_stricmp( bones[j].name, (const char *)buf ))
			{
				bonemap[i] = (u8)j;
				break;
			}
		}
	}
	for( i = 0; i < gst.ngest; i++ )
	{
		gst_gesture_t *g = &gst.gest[i];
		u32 bones_ofs;

		gEngfuncs.fsapi->Seek( f, dir_ofs + i * 48, SEEK_SET );
		if( gEngfuncs.fsapi->Read( f, buf, 48 ) != 48 )
			goto fail;
		memcpy( g->name, buf, sizeof( g->name ));
		g->name[sizeof( g->name ) - 1] = 0;
		g->nframes = GstU16( buf + 28 );
		g->nrot = buf[30];
		g->npos = buf[31];
		bones_ofs = GstU32( buf + 36 );
		g->frames_ofs = GstU32( buf + 40 );
		if( g->nrot > GST_ROT || g->npos > GST_POS || g->nframes < 1 )
			goto fail;
		gEngfuncs.fsapi->Seek( f, bones_ofs, SEEK_SET );
		if( gEngfuncs.fsapi->Read( f, buf, g->nrot + g->npos ) != g->nrot + g->npos )
			goto fail;
		for( j = 0; j < g->nrot + g->npos; j++ )
			g->bone[j] = buf[j] < nbones ? bonemap[buf[j]] : 0xff;
	}
	/* the whole file to ARAM, a block at a time */
	gEngfuncs.fsapi->Seek( f, 0, SEEK_END );
	size = (u32)gEngfuncs.fsapi->Tell( f );
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
	gst.asset = index;
	gst.state = 1;
	gEngfuncs.Con_Reportf( "Xash3D GameCube: gestures %s: %d gestures, %u bytes in ARAM\n", path, gst.ngest, (unsigned)size );
	return true;
fail:
	gEngfuncs.fsapi->Close( f );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: gestures %s: load failed\n", path );
	return false;
}

/* The two frames either side of a gesture's cycle, into q. */
static qboolean R_GXGestureFrames( int gi, int frame, s16 q[2][GST_ROT + GST_POS][3] )
{
	const gst_gesture_t *g = &gst.gest[gi];
	const u32 stride = (u32)( g->nrot + g->npos ) * 6;
	const int nread = frame + 1 < g->nframes ? 2 : 1;

	if( !GC_AramCacheRead( GC_AramAssetCacheGet(), gst.asset, g->frames_ofs + (u32)frame * stride, q[0], stride * nread ))
		return false;
	if( nread == 1 )
		memcpy( q[1], q[0], stride );
	else if( g->nrot + g->npos < GST_ROT + GST_POS )
		memmove( q[1], (byte *)q[0] + stride, stride );
	return true;
}

/* q = q * d, both (x, y, z, w) */
static void GstQuatMul( float *q, const float *d )
{
	const float ax = q[0], ay = q[1], az = q[2], aw = q[3];

	q[0] = aw * d[0] + ax * d[3] + ay * d[2] - az * d[1];
	q[1] = aw * d[1] - ax * d[2] + ay * d[3] + az * d[0];
	q[2] = aw * d[2] + ax * d[1] - ay * d[0] + az * d[3];
	q[3] = aw * d[3] - ax * d[0] - ay * d[1] - az * d[2];
}

/* Turn the bones of e by every gesture playing on it. q is the studio pose
 * (local rotations) before the bone matrices are built. */
void R_GXGestureApply( const cl_entity_t *e, const studiohdr_t *hdr, vec4_t *q, vec3_t *pos, float time )
{
	const char *names[GST_SLOTS];
	float cycles[GST_SLOTS], weights[GST_SLOTS];
	int i, j, n;

	if( !e || e->index <= 0 || !e->model )
		return;
	n = GC_ActorGesturesAt( e->index, time, names, cycles, weights, GST_SLOTS );
	if( n <= 0 )
		return;
	if( gst.state == 0 || ( gst.model != e->model && gst.state != 1 ))
		R_GXGestureLoad( e->model, hdr );
	if( gst.state != 1 || gst.model != e->model )
		return;
	for( i = 0; i < n; i++ )
	{
		const gst_gesture_t *g = NULL;
		s16 fq[2][GST_ROT + GST_POS][3];
		float f, t, w = weights[i];
		int gi, frame;

		for( gi = 0; gi < gst.ngest; gi++ )
		{
			if( !Q_stricmp( gst.gest[gi].name, names[i] ))
			{
				g = &gst.gest[gi];
				break;
			}
		}
		if( !g || w <= 0.0f )
			continue;
		f = bound( 0.0f, cycles[i], 1.0f ) * ( g->nframes - 1 );
		frame = Q_min( (int)f, g->nframes - 1 );
		t = f - frame;
		if( !R_GXGestureFrames( gi, frame, fq ))
			continue;
		for( j = 0; j < g->nrot; j++ )
		{
			const int b = g->bone[j];
			float d[4], ww, len;

			if( b == 0xff || b >= hdr->numbones )
				continue;
			d[0] = ( fq[0][j][0] + ( fq[1][j][0] - fq[0][j][0] ) * t ) * ( w / 32767.0f );
			d[1] = ( fq[0][j][1] + ( fq[1][j][1] - fq[0][j][1] ) * t ) * ( w / 32767.0f );
			d[2] = ( fq[0][j][2] + ( fq[1][j][2] - fq[0][j][2] ) * t ) * ( w / 32767.0f );
			/* w of the unit delta, then eased toward identity by the weight */
			ww = 1.0f - ( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] ) / ( w * w );
			ww = ww > 0.0f ? sqrtf( ww ) : 0.0f;
			d[3] = 1.0f + ( ww - 1.0f ) * w;
			len = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3] );
			if( len < 0.0001f )
				continue;
			d[0] /= len; d[1] /= len; d[2] /= len; d[3] /= len;
			GstQuatMul( q[b], d );
		}
		/* and the bones it moves: the hat off the head, the hips in a shift */
		for( j = g->nrot; j < g->nrot + g->npos; j++ )
		{
			const int b = g->bone[j];
			int k;

			if( b == 0xff || b >= hdr->numbones )
				continue;
			for( k = 0; k < 3; k++ )
				pos[b][k] += ( fq[0][j][k] + ( fq[1][j][k] - fq[0][j][k] ) * t ) * ( w / 256.0f );
		}
	}
}

void R_GXGestureReset( void )
{
	memset( &gst, 0, sizeof( gst ));	/* the arena block goes with the map */
}

#endif // XASH_GAMECUBE
