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

#if XASH_GAMECUBE

#define GC_VS_BLOCK	1024	/* samples read at once: one 2 KB disc sector, 64 ms */

extern int gc_iso9660_small_reads;

static struct
{
	file_t		*f;
	uint32_t	left;		/* samples still in the file */
	int16_t		buf[GC_VS_BLOCK];
	int		have, pos;
	uint32_t	frac, step;	/* 16.16 source samples per output sample */
	int		s0, s1;
	int		ent;
	float		vol, dist_mult;
} gc_vs;

static void GC_VoiceStreamStop( void )
{
	if( gc_vs.f )
		FS_Close( gc_vs.f );
	memset( &gc_vs, 0, sizeof( gc_vs ));
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
		else if( !memcmp( ck, "data", 4 ))
			break;
		if( size )
			FS_Seek( f, ( size + 1 ) & ~1u, SEEK_CUR );
	}
	if( !rate || size < 2 )
		goto bad;
	gc_vs.f = f;
	gc_vs.left = size / 2;
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
