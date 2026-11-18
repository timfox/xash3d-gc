/*
voice_stream.c - an actor's voice line streamed off the disc (GameCube)
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
The capture and smoke routes draw entities straight from their edicts, so
the sound messages the game code sends never reach a client: the Lost Coast
fisherman moved his mouth to nothing. His lines are 4.8 MB of 16 kHz PCM,
which neither the sound heap nor ARAM can hold, so the one line being spoken
is read from the disc a sector at a time and added to the mixer's paint
buffer, placed by the speaker's position like any other entity sound.
*/

#include "common.h"
#include "client.h"
#include "server.h"
#include "sound.h"
#include "platform/gamecube/mem_gamecube.h"

#if XASH_GAMECUBE

#define GC_VS_BLOCK	1024	/* samples read at once: one 2 KB disc sector, 64 ms */

#if XASH_GAMECUBE_LIBOGC2
/* The libogc2 stack reads the disc through its own iso9660 library, so iso9660_gamecube.c (which defines this) is not
 * built; keep the flag so the voice stream links. It changes nothing there. */
int gc_iso9660_small_reads;
#else
extern int gc_iso9660_small_reads;
#endif

static struct
{
	file_t		*f;
	uint32_t	left;		/* samples still in the file */
	uint32_t	total, rate;
	int16_t		*buf;		/* GC_VS_BLOCK samples, map-load arena tail */
	int		have, pos;
	uint32_t	frac, step;	/* 16.16 source samples per output sample */
	int		s0, s1;
	int		ent;
	float		vol, dist_mult;
} gc_vs;

/* The line's phonemes ("GCLP" chunk, lostcoast-gc bake_lostcoast_actors.py):
 * what the mouth should be saying at each moment of the wave. */
#define GC_VS_LIP_MAX	192

static struct
{
	int		n;
	uint16_t	*code, *t0, *t1;	/* GC_VS_LIP_MAX each, ms; arena tail */
} gc_vs_lip;

/* MEM1's heap is down to a few KB on Lost Coast: the sample block and the
 * phoneme list live in what the map-load arena has left over. */
static int16_t *gc_vs_mem;

static qboolean GC_VoiceStreamMem( void )
{
	if( !gc_vs_mem && GC_MapLoadBufferTailPeak() != 0 )
	{
		gc_vs_mem = (int16_t *)GC_MapLoadBufferTailAlloc( Q_max( GC_MapLoadBufferTailHighwater(), GC_MapLoadBufferTailPeak()),
			( GC_VS_BLOCK + GC_VS_LIP_MAX * 3 ) * sizeof( int16_t ));
		if( gc_vs_mem )
		{
			gc_vs_lip.code = (uint16_t *)( gc_vs_mem + GC_VS_BLOCK );
			gc_vs_lip.t0 = gc_vs_lip.code + GC_VS_LIP_MAX;
			gc_vs_lip.t1 = gc_vs_lip.t0 + GC_VS_LIP_MAX;
		}
	}
	return gc_vs_mem != NULL;
}

static void GC_VoiceStreamStop( void )
{
	if( gc_vs.f )
		FS_Close( gc_vs.f );
	memset( &gc_vs, 0, sizeof( gc_vs ));
	gc_vs.buf = gc_vs_mem;
	gc_vs_lip.n = 0;
}

/* An entity sound on the voice channel: stream it. False = not ours. */
qboolean GC_VoiceStreamStart( int ent, const char *sample, float vol, float attn )
{
	char path[96];
	byte hdr[12], ck[8], fmt[16];
	uint32_t rate = 0, size;
	file_t *f;

	if( !sample || !sample[0] || sample[0] == '!' || sample[0] == '*' )
		return false;
	if( !GC_VoiceStreamMem( ))
		return false;
	Q_snprintf( path, sizeof( path ), "sound/%s", sample );
	/* small disc reads from here to the return: the header is 44 bytes */
	gc_iso9660_small_reads = 1;
	f = FS_Open( path, "rb", false );
	if( !f )
	{
		gc_iso9660_small_reads = 0;
		return false;
	}
	GC_VoiceStreamStop();
	if( FS_Read( f, hdr, 12 ) != 12 || memcmp( hdr, "RIFF", 4 ) || memcmp( hdr + 8, "WAVE", 4 ))
		goto bad;
	for( ;; )
	{
		if( FS_Read( f, ck, 8 ) != 8 )
			goto bad;
		size = ck[4] | ( ck[5] << 8 ) | ( ck[6] << 16 ) | ((uint32_t)ck[7] << 24 );
		if( !memcmp( ck, "fmt ", 4 ))
		{
			if( size < 16 || FS_Read( f, fmt, 16 ) != 16 )
				goto bad;
			/* PCM, mono, 16 bit */
			if( fmt[0] != 1 || fmt[2] != 1 || fmt[14] != 16 )
				goto bad;
			rate = fmt[4] | ( fmt[5] << 8 ) | ( fmt[6] << 16 );
			size -= 16;
		}
		else if( !memcmp( ck, "GCLP", 4 ) && size >= 2 )
		{
			byte ev[6];
			int i, n;

			if( FS_Read( f, ev, 2 ) != 2 )
				goto bad;
			n = ( ev[0] << 8 ) | ev[1];
			size -= 2;
			gc_vs_lip.n = 0;
			for( i = 0; i < n && size >= 6; i++, size -= 6 )
			{
				if( FS_Read( f, ev, 6 ) != 6 )
					goto bad;
				if( i < GC_VS_LIP_MAX )
				{
					gc_vs_lip.code[i] = (uint16_t)(( ev[0] << 8 ) | ev[1] );
					gc_vs_lip.t0[i] = (uint16_t)(( ev[2] << 8 ) | ev[3] );
					gc_vs_lip.t1[i] = (uint16_t)(( ev[4] << 8 ) | ev[5] );
					gc_vs_lip.n = i + 1;
				}
			}
		}
		else if( !memcmp( ck, "data", 4 ))
			break;
		if( size )
			FS_Seek( f, ( size + 1 ) & ~1u, SEEK_CUR );
	}
	if( !rate || size < 2 )
		goto bad;
	gc_vs.f = f;
	gc_vs.left = gc_vs.total = size / 2;
	gc_vs.rate = rate;
	gc_vs.step = (uint32_t)( 65536.0f * (float)rate / (float)SOUND_DMA_SPEED );
	gc_vs.ent = ent;
	gc_vs.vol = bound( 0.0f, vol, 1.0f );
	gc_vs.dist_mult = attn / 1000.0f;	/* SND_CLIP_DISTANCE */
	gc_iso9660_small_reads = 0;
	return true;
bad:
	gc_iso9660_small_reads = 0;
	FS_Close( f );
	return false;
}

/* What ent's mouth is saying now: the phoneme being spoken (code_a), the one
 * it is moving toward (code_b) and how far along (0..1). A code of 0 is the
 * mouth at rest. False when ent is not speaking. Source blends a phoneme
 * into the one that follows it across its own length when the two touch
 * (c_baseflex.cpp AddVisemesForSentence); a phoneme with silence on either
 * side eases from or to rest over GC_VS_LIP_EASE ms. */
#define GC_VS_LIP_EASE	50

qboolean GC_VoiceLip( int ent, int *code_a, int *code_b, float *frac )
{
	uint32_t played, ms;
	int i;

	*code_a = *code_b = 0;
	*frac = 0.0f;
	if( !gc_vs.f || gc_vs.ent != ent || !gc_vs.rate )
		return false;
	played = gc_vs.total - gc_vs.left - (uint32_t)( gc_vs.have - gc_vs.pos );
	ms = (uint32_t)(( (uint64_t)played * 1000u ) / gc_vs.rate );
	for( i = 0; i < gc_vs_lip.n; i++ )
	{
		const uint32_t t0 = gc_vs_lip.t0[i], t1 = gc_vs_lip.t1[i];

		if( ms < t0 || ms >= t1 || t1 <= t0 )
			continue;
		if( i + 1 < gc_vs_lip.n && gc_vs_lip.t0[i + 1] <= t1 + 2 )
		{
			*code_a = gc_vs_lip.code[i];
			*code_b = gc_vs_lip.code[i + 1];
			*frac = (float)( ms - t0 ) / (float)( t1 - t0 );
		}
		else if( t1 - ms < GC_VS_LIP_EASE )
		{
			*code_a = gc_vs_lip.code[i];
			*frac = 1.0f - (float)( t1 - ms ) / (float)GC_VS_LIP_EASE;
		}
		else
			*code_a = gc_vs_lip.code[i];
		if(( i == 0 || gc_vs_lip.t1[i - 1] + 2 < t0 ) && ms - t0 < GC_VS_LIP_EASE && !*code_b )
		{
			/* out of silence: from rest into this phoneme */
			*code_b = gc_vs_lip.code[i];
			*code_a = 0;
			*frac = (float)( ms - t0 ) / (float)GC_VS_LIP_EASE;
		}
		break;
	}
	return true;
}

/* Add the line to `count` stereo frames of the paint buffer. */
void GC_VoiceStreamMix( int *paint, int count, int gain )
{
	float lscale = 1.0f, rscale = 1.0f;
	int n, gl, gr;

	if( !gc_vs.f )
		return;
	/* S_SpatializeChannel, from where the speaker stands now */
	if( gc_vs.ent > 0 && gc_vs.ent < svgame.numEntities && sv.state == ss_active )
	{
		const edict_t *ed = SV_EdictNum( gc_vs.ent );
		vec3_t dir, right;
		float dist, dot;

		if( ed && !ed->free )
		{
			VectorAverage( ed->v.mins, ed->v.maxs, dir );
			VectorAdd( dir, ed->v.origin, dir );
			/* the camera, not snd.origin: the sound listener is not moved
			 * on these routes */
			VectorSubtract( dir, refState.vieworg, dir );
			dist = VectorNormalizeLength( dir ) * gc_vs.dist_mult;
			AngleVectors( refState.viewangles, NULL, right, NULL );
			dot = gc_vs.dist_mult > 0.0f ? DotProduct( right, dir ) : 0.0f;
			rscale = bound( 0.0f, ( 1.0f - dist ) * ( 1.0f + dot ), 1.0f );
			lscale = bound( 0.0f, ( 1.0f - dist ) * ( 1.0f - dot ), 1.0f );
		}
	}
	gl = (int)( 256.0f * gc_vs.vol * lscale ) * gain >> 8;
	gr = (int)( 256.0f * gc_vs.vol * rscale ) * gain >> 8;
	for( n = 0; n < count; n++ )
	{
		int s;

		gc_vs.frac += gc_vs.step;
		while( gc_vs.frac >= 65536u )
		{
			gc_vs.frac -= 65536u;
			gc_vs.s0 = gc_vs.s1;
			if( gc_vs.pos >= gc_vs.have )
			{
				const uint32_t want = Q_min( gc_vs.left, (uint32_t)GC_VS_BLOCK );
				int i;

				fs_offset_t got = 0;

				if( want )
				{
					/* only the sectors this block touches, not the disc layer's
					 * whole 32 KB buffer (a 20 ms stall every second) */
					gc_iso9660_small_reads = 1;
					got = FS_Read( gc_vs.f, gc_vs.buf, want * 2 );
					gc_iso9660_small_reads = 0;
				}
				if( !want || got != (fs_offset_t)( want * 2 ))
				{
					GC_VoiceStreamStop();
					return;
				}
				for( i = 0; i < (int)want; i++ )	/* little-endian file */
				{
					const uint16_t x = (uint16_t)gc_vs.buf[i];

					gc_vs.buf[i] = (int16_t)(( x >> 8 ) | ( x << 8 ));
				}
				gc_vs.left -= want;
				gc_vs.have = (int)want;
				gc_vs.pos = 0;
			}
			gc_vs.s1 = gc_vs.buf[gc_vs.pos++];
		}
		s = gc_vs.s0 + ((( gc_vs.s1 - gc_vs.s0 ) * (int)( gc_vs.frac >> 4 )) >> 12 );
		paint[n * 2 + 0] += ( s * gl ) >> 8;
		paint[n * 2 + 1] += ( s * gr ) >> 8;
	}
}

#endif // XASH_GAMECUBE
