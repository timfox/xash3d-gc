/*
 * Lost Coast soundscapes (Source env_soundscape) on the GameCube.
 *
 * The rules come from the game: tools/bake_lostcoast_soundscape.py flattens
 * scripts/soundscapes_lostcoast.txt for each env_soundscape in the map into
 * media/ss_<map>.gcs. Every rule is a Source "playrandom": every `time`
 * seconds one wave of the rule's list plays whole, at a random volume and
 * pitch, either unpositioned or from a random point on a 36-unit ring around
 * the listener. Scheduling follows game/client/c_soundscape.cpp: the first
 * play of a rule comes 0.5 x a random interval after its soundscape starts.
 *
 * Waves are 22.05 kHz mono IMA ADPCM kept in ARAM (the heap has none to
 * spare). Voices decode straight out of ARAM through one 1 KB window and
 * are added to the mixer's paint buffer, so they share its volume and clip.
 */
#include "platform/platform.h"

#if XASH_GAMECUBE

#include "common.h"
#include "client.h"
#include "sound.h"
#include "platform/gamecube/mem_gamecube.h"

#define GC_SS_MAX_ENTS		8
#define GC_SS_MAX_RULES		48
#define GC_SS_MAX_REFS		256
#define GC_SS_MAX_WAVES		48
#define GC_SS_VOICES		6
#define GC_SS_WINDOW		1024
#define GC_SS_RING		36.0f	/* DEFAULT_SOUND_RADIUS */

typedef struct { int16_t org[3]; uint16_t radius, first_rule, n_rules; uint32_t pad; } gc_ss_ent_t;
typedef struct
{
	uint16_t	tmin, tmax;	/* 1/100 s */
	uint8_t		vmin, vmax, pmin, pmax, mode, lvl;
	int16_t		pos[3];
	uint16_t	first_ref;
	uint8_t		n_ref, pad[5];
} gc_ss_rule_t;
typedef struct { uint32_t ofs, samples, pad; } gc_ss_wave_t;
typedef struct
{
	uint32_t	base;		/* ARAM address of the wave */
	uint32_t	samples, pos;	/* total, next to decode */
	uint32_t	frac, step;	/* 16.16 source samples per output sample */
	int		pred, index, s0, s1;
	int		vol_l, vol_r;	/* 0..256 */
} gc_ss_voice_t;

static const int16_t gc_ss_step[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
	449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
	9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t gc_ss_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static gc_ss_ent_t gc_ss_ents[GC_SS_MAX_ENTS];
static gc_ss_rule_t gc_ss_rules[GC_SS_MAX_RULES];
static uint16_t gc_ss_refs[GC_SS_MAX_REFS];
static gc_ss_wave_t gc_ss_waves[GC_SS_MAX_WAVES];
static float gc_ss_next[GC_SS_MAX_RULES];
static gc_ss_voice_t gc_ss_voice[GC_SS_VOICES];
static byte gc_ss_win[GC_SS_WINDOW] __attribute__(( aligned( 32 )));
static int gc_ss_n_ent, gc_ss_n_rule, gc_ss_n_ref, gc_ss_n_wave, gc_ss_rate;
static int gc_ss_state;		/* 0 untried, 1 ready, -1 absent */
static int gc_ss_cur = -1;	/* active env_soundscape */
static uint32_t gc_ss_aram;
static unsigned gc_ss_played;

static qboolean GC_SoundscapeLoad( void )
{
	file_t *f;
	uint32_t hdr[8], data_ofs = 0, data_bytes = 0, done;
	int step = 0;
	char path[64];

	if( gc_ss_state )
		return gc_ss_state > 0;
	gc_ss_state = -1;
	Q_snprintf( path, sizeof( path ), "media/ss_%s.gcs", clgame.mapname );
	f = FS_Open( path, "rb", false );
	if( !f )
	{
		Con_Reportf( "Xash3D GameCube: soundscape pack %s absent\n", path );
		return false;
	}
	if( FS_Read( f, hdr, 32 ) == 32 && hdr[0] == 0x47435353u )	/* "GCSS" */
	{
		gc_ss_n_ent = (int)( hdr[1] & 0xffffu );
		gc_ss_n_rule = (int)( hdr[2] >> 16 );
		gc_ss_n_wave = (int)( hdr[2] & 0xffffu );
		gc_ss_n_ref = (int)( hdr[3] >> 16 );
		gc_ss_rate = (int)( hdr[3] & 0xffffu );
		data_ofs = hdr[4];
		data_bytes = hdr[5];
		step = 1;
		if( gc_ss_n_ent <= GC_SS_MAX_ENTS && gc_ss_n_rule <= GC_SS_MAX_RULES
			&& gc_ss_n_wave <= GC_SS_MAX_WAVES && gc_ss_n_ref <= GC_SS_MAX_REFS
			&& gc_ss_rate > 0 && !( data_bytes & 31u ) && ( step = 2 )
			&& FS_Read( f, gc_ss_ents, gc_ss_n_ent * 16 ) == gc_ss_n_ent * 16
			&& FS_Read( f, gc_ss_rules, gc_ss_n_rule * 24 ) == gc_ss_n_rule * 24
			&& FS_Read( f, gc_ss_refs, (( gc_ss_n_ref * 2 ) + 31 ) & ~31 ) > 0
			&& FS_Read( f, gc_ss_waves, gc_ss_n_wave * 12 ) == gc_ss_n_wave * 12 && ( step = 3 )
			&& FS_Seek( f, (fs_offset_t)data_ofs, SEEK_SET ) == 0 && ( step = 4 )
			&& ( gc_ss_aram = GC_AramReserveTagged( data_bytes + GC_SS_WINDOW, "soundscape" )) != 0 )
		{
			for( done = 0; done < data_bytes; done += GC_SS_WINDOW )
			{
				const uint32_t n = Q_min( (uint32_t)GC_SS_WINDOW, data_bytes - done );

				if( FS_Read( f, gc_ss_win, n ) != (fs_offset_t)n )
					break;
				GC_AramTransfer( gc_ss_aram + done, gc_ss_win, n, true );
			}
			if( done >= data_bytes )
				gc_ss_state = 1;
		}
	}
	FS_Close( f );
	Con_Reportf( "Xash3D GameCube: soundscape pack %s %s (step %d, refs %d, %u Kb): %d entities, %d rules, %d waves, %d Hz\n",
		path, gc_ss_state > 0 ? "ready" : "FAILED", step, gc_ss_n_ref, data_bytes / 1024u,
		gc_ss_n_ent, gc_ss_n_rule, gc_ss_n_wave, gc_ss_rate );
	if( gc_ss_state <= 0 )
		GC_AramReport( "soundscape" );
	return gc_ss_state > 0;
}

static float GC_SoundscapeInterval( const gc_ss_rule_t *r )
{
	return COM_RandomFloat( (float)r->tmin, (float)r->tmax ) * 0.01f;
}

static void GC_SoundscapePlay( const gc_ss_rule_t *r )
{
	const gc_ss_wave_t *w;
	gc_ss_voice_t *v = NULL;
	float vol, pitch, pan = 0.0f;
	int i;

	if( !r->n_ref || r->mode == 2 )
		return;	/* fixed positions: none resolve in this map (suppressed on PC too) */
	w = &gc_ss_waves[gc_ss_refs[r->first_ref + COM_RandomLong( 0, r->n_ref - 1 )] % GC_SS_MAX_WAVES];
	if( !w->samples )
		return;
	for( i = 0; i < GC_SS_VOICES; i++ )
	{
		if( !gc_ss_voice[i].samples )
		{
			v = &gc_ss_voice[i];
			break;
		}
	}
	if( !v )
		return;
	vol = COM_RandomFloat( (float)r->vmin, (float)r->vmax ) * ( 1.0f / 255.0f );
	pitch = COM_RandomFloat( (float)r->pmin, (float)r->pmax ) * 0.01f;
	if( r->mode == 1 )
	{
		/* origin + 36 * ( cos * right + sin * forward ): only the bearing
		 * matters at that range, and its right component is the cosine. */
		pan = cosf( COM_RandomFloat( -180.0f, 180.0f ) * ( M_PI_F / 180.0f ));
	}
	memset( v, 0, sizeof( *v ));
	v->base = gc_ss_aram + w->ofs;
	v->step = (uint32_t)( 65536.0f * pitch * (float)gc_ss_rate / (float)SOUND_DMA_SPEED );
	if( r->mode == 1 )
	{
		/* Two-speaker spatialization: each side gets half the volume
		 * swung by the bearing's right component. */
		v->vol_l = (int)( 256.0f * vol * 0.5f * ( 1.0f - pan ));
		v->vol_r = (int)( 256.0f * vol * 0.5f * ( 1.0f + pan ));
	}
	else
		v->vol_l = v->vol_r = (int)( 256.0f * vol );
	v->samples = w->samples;
	gc_ss_played++;
}

/* Add the playing voices to `count` stereo frames of the paint buffer. */
void GC_SoundscapeMix( int *paint, int count, int gain )
{
	int i, n;

	if( gc_ss_state <= 0 )
		return;
	for( i = 0; i < GC_SS_VOICES; i++ )
	{
		gc_ss_voice_t *v = &gc_ss_voice[i];
		uint32_t win0 = 0xffffffffu;
		const int gl = ( v->vol_l * gain ) >> 8, gr = ( v->vol_r * gain ) >> 8;

		if( !v->samples )
			continue;
		for( n = 0; n < count; n++ )
		{
			int s;

			v->frac += v->step;
			while( v->frac >= 65536u )
			{
				v->frac -= 65536u;
				v->s0 = v->s1;
				if( v->pos < v->samples )
				{
					const uint32_t byte_i = v->pos >> 1;
					int code, st, d;

					if( byte_i - win0 >= GC_SS_WINDOW )
					{
						win0 = byte_i & ~31u;
						GC_AramTransfer( v->base + win0, gc_ss_win, GC_SS_WINDOW, false );
					}
					code = gc_ss_win[byte_i - win0];
					code = ( v->pos & 1 ) ? ( code & 15 ) : ( code >> 4 );
					st = gc_ss_step[v->index];
					d = st >> 3;
					if( code & 4 ) d += st;
					if( code & 2 ) d += st >> 1;
					if( code & 1 ) d += st >> 2;
					v->pred += ( code & 8 ) ? -d : d;
					if( v->pred > 32767 ) v->pred = 32767;
					else if( v->pred < -32768 ) v->pred = -32768;
					v->index += gc_ss_index[code & 7];
					if( v->index < 0 ) v->index = 0;
					else if( v->index > 88 ) v->index = 88;
					v->s1 = v->pred;
					v->pos++;
				}
				else if( v->pos++ > v->samples )
				{
					v->samples = 0;	/* tail interpolated out: voice is free */
					break;
				}
				else
					v->s1 = 0;
			}
			if( !v->samples )
				break;
			s = v->s0 + ((( v->s1 - v->s0 ) * (int)( v->frac >> 4 )) >> 12 );
			paint[n * 2 + 0] += ( s * gl ) >> 8;
			paint[n * 2 + 1] += ( s * gr ) >> 8;
		}
	}
}

void GC_CapFacesSoundscapeArm( void )
{
}

void GC_CapFacesSoundscapeFrame( void )
{
	static int tick;
	const float now = (float)host.realtime;
	const float *eye = refState.vieworg;
	int i, best = -1;
	float best_d = 0.0f;

	if( cls.state != ca_active || !GC_SoundscapeLoad( ))
		return;
	/* The env_soundscape whose radius holds the listener takes over; the
	 * current one stays until another does. (The PC server also wants a
	 * clear line of sight to the entity.) */
	for( i = 0; i < gc_ss_n_ent; i++ )
	{
		const gc_ss_ent_t *e = &gc_ss_ents[i];
		const float dx = eye[0] - (float)e->org[0], dy = eye[1] - (float)e->org[1];
		const float dz = eye[2] - (float)e->org[2];
		const float d = dx * dx + dy * dy + dz * dz;

		if( d <= (float)e->radius * (float)e->radius && ( best < 0 || d < best_d ))
		{
			best = i;
			best_d = d;
		}
	}
	if( best >= 0 && best != gc_ss_cur )
	{
		const gc_ss_ent_t *e = &gc_ss_ents[best];

		gc_ss_cur = best;
		for( i = 0; i < e->n_rules && e->first_rule + i < GC_SS_MAX_RULES; i++ )
			gc_ss_next[e->first_rule + i] = now + 0.5f * GC_SoundscapeInterval( &gc_ss_rules[e->first_rule + i] );
		Con_Reportf( "Xash3D GameCube: soundscape entity %d active (%d rules)\n", best, (int)e->n_rules );
	}
	if( gc_ss_cur >= 0 )
	{
		const gc_ss_ent_t *e = &gc_ss_ents[gc_ss_cur];

		for( i = 0; i < e->n_rules && e->first_rule + i < GC_SS_MAX_RULES; i++ )
		{
			const int ri = e->first_rule + i;

			if( now < gc_ss_next[ri] )
				continue;
			GC_SoundscapePlay( &gc_ss_rules[ri] );
			gc_ss_next[ri] = now + GC_SoundscapeInterval( &gc_ss_rules[ri] );
		}
	}
	if(( ++tick & 255 ) == 0 )
	{
		extern void S_GCLogAudioTelemetry( const char *tag );

		Con_Reportf( "Xash3D GameCube: soundscape played=%u\n", gc_ss_played );
		S_GCLogAudioTelemetry( "soundscape-tick" );
	}
}

#else

void GC_CapFacesSoundscapeArm( void )
{
}

void GC_CapFacesSoundscapeFrame( void )
{
}

#endif
