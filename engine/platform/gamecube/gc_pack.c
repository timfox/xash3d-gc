/*
gc_pack.c - readers for GxBake's packed GameCube formats. See gc_pack.h.
*/
#include "gc_pack.h"

static uint32_t rd32( const uint8_t *p ) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16( const uint8_t *p ) { return (uint16_t)((p[0] << 8) | p[1]); }

/* --- Yaz0 --------------------------------------------------------------- */

uint32_t gc_yaz0_size( const uint8_t *src, size_t srclen )
{
	if( srclen < 16 || src[0] != 'Y' || src[1] != 'a' || src[2] != 'z' || src[3] != '0' )
		return 0;
	return rd32( src + 4 );
}

uint32_t gc_yaz0_decode( const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen )
{
	uint32_t size = gc_yaz0_size( src, srclen );
	size_t s = 16, d = 0;

	if( !size && srclen < 16 )
		return 0;
	if( size > dstlen )
		return 0;
	while( d < size )
	{
		uint8_t flags;
		int bit;

		if( s >= srclen )
			return 0;
		flags = src[s++];
		for( bit = 0; bit < 8 && d < size; bit++, flags <<= 1 )
		{
			if( flags & 0x80 )
			{
				if( s >= srclen )
					return 0;
				dst[d++] = src[s++];
			}
			else
			{
				size_t dist, len, from;

				if( s + 2 > srclen )
					return 0;
				dist = (((size_t)src[s] & 15) << 8 | src[s + 1]) + 1;
				len = src[s] >> 4;
				s += 2;
				if( len == 0 )
				{
					if( s >= srclen )
						return 0;
					len = (size_t)src[s++] + 0x12;
				}
				else
					len += 2;
				if( dist > d || d + len > size )
					return 0;
				from = d - dist;
				while( len-- )	/* byte by byte: overlapping copies repeat */
					dst[d++] = dst[from++];
			}
		}
	}
	return size;
}

/* --- DSP ADPCM ------------------------------------------------------------ */

static uint32_t nibble_to_sample( uint32_t n )
{
	uint32_t in_frame = n % 16;
	return (n / 16) * GC_DSP_FRAME_SAMPLES + (in_frame > 2 ? in_frame - 2 : 0);
}

int gc_dsp_parse( const uint8_t *blob, size_t len, gc_dsp_t *dsp )
{
	uint32_t frames;
	int i;

	if( len < GC_DSP_HEADER_BYTES )
		return -1;
	dsp->samples = rd32( blob + 0 );
	dsp->nibbles = rd32( blob + 4 );
	dsp->rate = rd32( blob + 8 );
	dsp->loop = rd16( blob + 12 );
	if( rd16( blob + 14 ) != 0 )	/* format 0 = ADPCM */
		return -1;
	dsp->loop_start = nibble_to_sample( rd32( blob + 16 ));
	dsp->loop_end = nibble_to_sample( rd32( blob + 20 ));
	for( i = 0; i < 16; i++ )
		dsp->coefs[i] = (int16_t)rd16( blob + 28 + i * 2 );
	dsp->loop_ps = (uint8_t)rd16( blob + 68 );
	dsp->loop_h1 = (int16_t)rd16( blob + 70 );
	dsp->loop_h2 = (int16_t)rd16( blob + 72 );
	frames = (dsp->samples + GC_DSP_FRAME_SAMPLES - 1) / GC_DSP_FRAME_SAMPLES;
	dsp->frames = blob + GC_DSP_HEADER_BYTES;
	dsp->frames_bytes = frames * GC_DSP_FRAME_BYTES;
	if( len < GC_DSP_HEADER_BYTES + (size_t)dsp->frames_bytes )
		return -1;
	return 0;
}

uint32_t gc_dsp_decode( const gc_dsp_t *dsp, uint32_t first, uint32_t count, gc_dsp_state_t *st, int16_t *out )
{
	uint32_t pos = first, done = 0;
	int32_t h1 = st->h1, h2 = st->h2;

	if( first >= dsp->samples )
		return 0;
	if( count > dsp->samples - first )
		count = dsp->samples - first;
	while( done < count )
	{
		const uint8_t *f = dsp->frames + (pos / GC_DSP_FRAME_SAMPLES) * GC_DSP_FRAME_BYTES;
		const int32_t c1 = dsp->coefs[(f[0] >> 4 & 7) * 2], c2 = dsp->coefs[(f[0] >> 4 & 7) * 2 + 1];
		const int e = f[0] & 15;
		uint32_t k;

		for( k = pos % GC_DSP_FRAME_SAMPLES; k < GC_DSP_FRAME_SAMPLES && done < count; k++, pos++, done++ )
		{
			int32_t n = (k & 1) ? (f[1 + k / 2] & 15) : (f[1 + k / 2] >> 4);
			int32_t v;

			if( n >= 8 )
				n -= 16;
			v = ((n << e) << 11) + 1024 + c1 * h1 + c2 * h2;
			v >>= 11;	/* arithmetic shift, as on Gekko */
			if( v < -32768 ) v = -32768;
			if( v > 32767 ) v = 32767;
			out[done] = (int16_t)v;
			h2 = h1;
			h1 = v;
		}
	}
	st->h1 = (int16_t)h1;
	st->h2 = (int16_t)h2;
	return done;
}

/* --- GCSK --------------------------------------------------------------- */

#define GSK_HEADER 16
#define GSK_ENTRY 32

int gc_gsk_parse( const uint8_t *blob, size_t len )
{
	uint32_t count;

	if( len < GSK_HEADER || blob[0] != 'G' || blob[1] != 'C' || blob[2] != 'S' || blob[3] != 'K' )
		return -1;
	count = rd32( blob + 4 );
	if( rd32( blob + 8 ) != 1 || count > 4096 || GSK_HEADER + (size_t)count * GSK_ENTRY > len )
		return -1;
	return (int)count;
}

int gc_gsk_entry( const uint8_t *blob, size_t len, int i, gc_gsk_entry_t *e )
{
	const uint8_t *p;
	uint32_t to, tb, lo, lb;
	int n = gc_gsk_parse( blob, len );

	if( i < 0 || i >= n )
		return -1;
	p = blob + GSK_HEADER + i * GSK_ENTRY;
	e->skin = rd16( p );
	e->fmt = p[2];
	e->tlut_fmt = p[3];
	e->width = rd16( p + 4 );
	e->height = rd16( p + 6 );
	e->tlut_entries = rd16( p + 8 );
	e->flags = rd16( p + 10 );
	to = rd32( p + 12 ); tb = rd32( p + 16 ); lo = rd32( p + 20 ); lb = rd32( p + 24 );
	if( (size_t)to + tb > len || (lb && (size_t)lo + lb > len) )
		return -1;
	e->texels = blob + to;
	e->texel_bytes = tb;
	e->tlut = lb ? blob + lo : NULL;
	e->tlut_bytes = lb;
	return 0;
}

#ifdef GC_PACK_GX
int gc_gsk_bind( const gc_gsk_entry_t *e, GXTexObj *obj, GXTlutObj *tlut, u32 tlut_name, u8 wrap )
{
	if( e->fmt == GX_TF_CI4 || e->fmt == GX_TF_CI8 )
	{
		if( !e->tlut )
			return -1;
		GX_InitTlutObj( tlut, (void *)e->tlut, e->tlut_fmt, e->tlut_entries );
		GX_LoadTlut( tlut, tlut_name );
		GX_InitTexObjCI( obj, (void *)e->texels, e->width, e->height, e->fmt, wrap, wrap, GX_FALSE, tlut_name );
	}
	else
		GX_InitTexObj( obj, (void *)e->texels, e->width, e->height, e->fmt, wrap, wrap, GX_FALSE );
	GX_InitTexObjLOD( obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1 );
	return 0;
}
#endif

/* --- GCSV --------------------------------------------------------------- */

#define GSV_HEADER 16
#define GSV_ENTRY 24

int gc_gsv_parse( const uint8_t *blob, size_t len )
{
	uint32_t count;

	if( len < GSV_HEADER || blob[0] != 'G' || blob[1] != 'C' || blob[2] != 'S' || blob[3] != 'V' )
		return -1;
	count = rd32( blob + 8 );
	if( rd32( blob + 4 ) != 1 || count > 4096 || GSV_HEADER + (size_t)count * GSV_ENTRY > len )
		return -1;
	return (int)count;
}

int gc_gsv_entry( const uint8_t *blob, size_t len, int i, gc_gsv_entry_t *e )
{
	const uint8_t *p;
	uint32_t po, no;
	int n = gc_gsv_parse( blob, len );

	if( i < 0 || i >= n )
		return -1;
	p = blob + GSV_HEADER + i * GSV_ENTRY;
	e->bodypart = rd16( p );
	e->model = rd16( p + 2 );
	e->nverts = rd32( p + 4 );
	e->nnorms = rd32( p + 8 );
	e->pos_frac = p[12];
	e->nrm_frac = p[13];
	po = rd32( p + 16 );
	no = rd32( p + 20 );
	if( e->pos_frac > 15 || e->nverts > 0x100000 || e->nnorms > 0x100000
		|| (size_t)po + (size_t)e->nverts * 6 > len || (size_t)no + (size_t)e->nnorms * 3 > len )
		return -1;
	e->positions = blob + po;
	e->normals = blob + no;
	return 0;
}

void gc_gsv_dequant( const gc_gsv_entry_t *e, float *pos, float *nrm )
{
	uint32_t i;
	float ps = 1.0f / (float)(1L << e->pos_frac);
	float ns = 1.0f / (float)(1L << e->nrm_frac);

	for( i = 0; i < e->nverts * 3; i++ )
		pos[i] = (float)(int16_t)rd16( e->positions + i * 2 ) * ps;
	for( i = 0; i < e->nnorms * 3; i++ )
		nrm[i] = (float)(int8_t)e->normals[i] * ns;
}
