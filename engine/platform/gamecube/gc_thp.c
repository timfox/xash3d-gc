/*
gc_thp.c - THP movie reader: container, DSP ADPCM audio, baseline JPEG pictures.
See gc_thp.h. Written for GxBake's `gxbake video` output; reads Nintendo-made
THP files too as long as their pictures are 4:2:0 without restart markers.
*/
#include "gc_thp.h"

#include <string.h>

static uint32_t rd32( const uint8_t *p )
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int16_t rd16s( const uint8_t *p )
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd16( const uint8_t *p )
{
	return ((uint32_t)p[0] << 8) | p[1];
}

/* --- container ------------------------------------------------------------- */

int gc_thp_parse( const uint8_t *buf, size_t len, gc_thp_t *thp )
{
	union { uint32_t u; float f; } fps;
	uint32_t comp, count, pos, i, ver;

	memset( thp, 0, sizeof( *thp ));
	if( len < 0x30 || memcmp( buf, "THP", 4 ))
		return -1;
	ver = rd32( buf + 4 );
	thp->version = ver;
	thp->max_frame_bytes = rd32( buf + 8 );
	thp->max_audio_samples = rd32( buf + 12 );
	fps.u = rd32( buf + 16 );
	thp->fps = fps.f;
	thp->frames = rd32( buf + 20 );
	thp->first_frame_bytes = rd32( buf + 24 );
	comp = rd32( buf + 32 );
	thp->first_frame = rd32( buf + 40 );
	if( comp > len || len - comp < 20 )
		return -1;
	count = rd32( buf + comp );
	if( count > 16 )
		return -1;
	pos = comp + 20;
	for( i = 0; i < count; i++ )
	{
		uint8_t type = buf[comp + 4 + i];

		if( type == 0 )
		{
			if( pos + 8 > len ) return -1;
			thp->width = rd32( buf + pos );
			thp->height = rd32( buf + pos + 4 );
			pos += ver == 0x00011000 ? 12 : 8;
		}
		else if( type == 1 )
		{
			if( pos + 12 > len ) return -1;
			thp->audio_channels = rd32( buf + pos );
			thp->audio_rate = rd32( buf + pos + 4 );
			thp->audio_samples = rd32( buf + pos + 8 );
			pos += ver == 0x00011000 ? 16 : 12;
		}
		else
			return -1;
	}
	if( !thp->width || !thp->height || (thp->width | thp->height) & 15 || thp->frames == 0 )
		return -1;
	if( thp->audio_channels && thp->audio_channels != 2 )
		return -1;
	return 0;
}

int gc_thp_frame( const gc_thp_t *thp, const uint8_t *buf, size_t len, gc_thp_frame_t *fr )
{
	uint32_t head = thp->audio_channels ? 16 : 12;

	memset( fr, 0, sizeof( *fr ));
	if( len < head )
		return -1;
	fr->next_bytes = rd32( buf );
	fr->video_bytes = rd32( buf + 8 );
	fr->audio_bytes = thp->audio_channels ? rd32( buf + 12 ) : 0;
	if( fr->video_bytes > len - head || fr->audio_bytes > len - head - fr->video_bytes )
		return -1;
	fr->video = buf + head;
	fr->audio = fr->audio_bytes ? buf + head + fr->video_bytes : NULL;
	return 0;
}

/* --- audio ----------------------------------------------------------------- */

uint32_t gc_thp_audio( const uint8_t *block, size_t len, int16_t *out, uint32_t max_samples )
{
	uint32_t size, n, c;

	if( len < 80 )
		return 0;
	size = rd32( block );
	n = rd32( block + 4 );
	if( (n + 13) / 14 * 8 > size || size > (len - 80) / 2 )
		return 0;
	if( n > max_samples )
		n = max_samples;
	for( c = 0; c < 2; c++ )
	{
		const uint8_t *coef = block + 8 + 32 * c;
		const uint8_t *f = block + 80 + size * c;
		int32_t h1 = rd16s( block + 72 + 4 * c ), h2 = rd16s( block + 74 + 4 * c );
		uint32_t i;

		for( i = 0; i < n; i++ )
		{
			const uint8_t *fh = f + (i / 14) * 8;
			const uint32_t k = i % 14;
			const int32_t c1 = rd16s( coef + (fh[0] >> 4 & 7) * 4 ), c2 = rd16s( coef + (fh[0] >> 4 & 7) * 4 + 2 );
			int32_t q = (k & 1) ? (fh[1 + k / 2] & 15) : (fh[1 + k / 2] >> 4);
			int32_t v;

			if( q >= 8 )
				q -= 16;
			v = q * (int32_t)( 1L << ((fh[0] & 15) + 11) ) + 1024	/* q << shift << 11, without shifting a negative */
				+ c1 * h1 + c2 * h2;
			v >>= 11;	/* arithmetic shift, as on Gekko */
			if( v < -32768 ) v = -32768;
			if( v > 32767 ) v = 32767;
			out[i * 2 + c] = (int16_t)v;
			h2 = h1;
			h1 = v;
		}
	}
	return n;
}

/* --- JPEG ------------------------------------------------------------------ */

#define LOOKUP_BITS 9

typedef struct
{
	uint8_t  vals[256];
	int32_t  maxcode[18];		/* largest code of each length, -1 if none */
	int32_t  valptr[17];		/* index in vals of the first code of each length */
	int32_t  mincode[17];
	uint16_t lookup[1 << LOOKUP_BITS];	/* length << 8 | value; 0 = longer than LOOKUP_BITS */
	int      present;
} huff_t;

typedef struct
{
	const uint8_t *p, *end;
	uint32_t acc;	/* MSB-aligned */
	int      nbits;
} bits_t;

static const uint8_t zigzag[64] =
{
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static int build_huff( huff_t *h, const uint8_t *counts, const uint8_t *vals, int nvals )
{
	int len, i, k = 0;
	int32_t code = 0;

	memset( h, 0, sizeof( *h ));
	memcpy( h->vals, vals, (size_t)nvals );
	for( len = 1; len <= 16; len++ )
	{
		h->valptr[len] = k;
		h->mincode[len] = code;
		for( i = 0; i < counts[len - 1]; i++, k++, code++ )
		{
			if( len <= LOOKUP_BITS )
			{
				int fill = 1 << (LOOKUP_BITS - len), j;
				int base = (int)(code << (LOOKUP_BITS - len));

				for( j = 0; j < fill; j++ )
					h->lookup[base + j] = (uint16_t)((len << 8) | vals[k]);
			}
		}
		h->maxcode[len] = counts[len - 1] ? code - 1 : -1;
		if( code > (1 << len))
			return -1;	/* over-subscribed */
		code <<= 1;
	}
	h->maxcode[17] = 0x7FFFFFFF;
	h->present = 1;
	return 0;
}

static void fill( bits_t *b )
{
	/* THP scans are not byte-stuffed: every byte is data. Past the end, zeros. */
	while( b->nbits <= 24 )
	{
		uint32_t byte = b->p < b->end ? *b->p++ : 0;

		b->acc |= byte << (24 - b->nbits);
		b->nbits += 8;
	}
}

static int32_t getbits( bits_t *b, int n )
{
	int32_t v;

	if( !n )
		return 0;
	fill( b );
	v = (int32_t)(b->acc >> (32 - n));
	b->acc <<= n;
	b->nbits -= n;
	return v;
}

static int32_t extend( int32_t v, int s )
{
	return v < (1 << (s - 1)) ? v - ((1 << s) - 1) : v;
}

static int decode_huff( bits_t *b, const huff_t *h )
{
	uint16_t look;
	int len;

	fill( b );
	look = h->lookup[b->acc >> (32 - LOOKUP_BITS)];
	if( look )
	{
		len = look >> 8;
		b->acc <<= len;
		b->nbits -= len;
		return look & 0xFF;
	}
	for( len = LOOKUP_BITS + 1; len <= 16; len++ )
	{
		int32_t code = (int32_t)(b->acc >> (32 - len));

		if( code <= h->maxcode[len] )
		{
			b->acc <<= len;
			b->nbits -= len;
			return h->vals[h->valptr[len] + code - h->mincode[len]];
		}
	}
	return -1;
}

/* libjpeg's accurate integer IDCT (jidctint.c, Loeffler-Ligtenberg-Moschytz). */
#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE( x, n ) (((x) + ((int32_t)1 << ((n) - 1))) >> (n))
#define F0_298 2446
#define F0_390 3196
#define F0_541 4433
#define F0_765 6270
#define F0_899 7373
#define F1_175 9633
#define F1_501 12299
#define F1_847 15137
#define F1_961 16069
#define F2_053 16819
#define F2_562 20995
#define F3_072 25172

static uint8_t clamp255( int32_t v )
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void idct( const int32_t *in, uint8_t *out, int stride )
{
	int32_t ws[64];
	int i;

	for( i = 0; i < 8; i++ )
	{
		const int32_t *c = in + i;
		int32_t *w = ws + i;
		int32_t t0, t1, t2, t3, t10, t11, t12, t13, z1, z2, z3, z4, z5;

		if( !c[8] && !c[16] && !c[24] && !c[32] && !c[40] && !c[48] && !c[56] )
		{
			int32_t dc = c[0] * (1 << PASS1_BITS);

			w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
			continue;
		}
		z2 = c[16]; z3 = c[48];
		z1 = (z2 + z3) * F0_541;
		t2 = z1 - z3 * F1_847;
		t3 = z1 + z2 * F0_765;
		t0 = (c[0] + c[32]) * (1 << CONST_BITS);
		t1 = (c[0] - c[32]) * (1 << CONST_BITS);
		t10 = t0 + t3; t13 = t0 - t3; t11 = t1 + t2; t12 = t1 - t2;
		t0 = c[56]; t1 = c[40]; t2 = c[24]; t3 = c[8];
		z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2; z4 = t1 + t3;
		z5 = (z3 + z4) * F1_175;
		t0 *= F0_298; t1 *= F2_053; t2 *= F3_072; t3 *= F1_501;
		z1 *= -F0_899; z2 *= -F2_562; z3 *= -F1_961; z4 *= -F0_390;
		z3 += z5; z4 += z5;
		t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
		w[0] = DESCALE( t10 + t3, CONST_BITS - PASS1_BITS );
		w[56] = DESCALE( t10 - t3, CONST_BITS - PASS1_BITS );
		w[8] = DESCALE( t11 + t2, CONST_BITS - PASS1_BITS );
		w[48] = DESCALE( t11 - t2, CONST_BITS - PASS1_BITS );
		w[16] = DESCALE( t12 + t1, CONST_BITS - PASS1_BITS );
		w[40] = DESCALE( t12 - t1, CONST_BITS - PASS1_BITS );
		w[24] = DESCALE( t13 + t0, CONST_BITS - PASS1_BITS );
		w[32] = DESCALE( t13 - t0, CONST_BITS - PASS1_BITS );
	}
	for( i = 0; i < 8; i++ )
	{
		const int32_t *w = ws + i * 8;
		uint8_t *o = out + i * stride;
		int32_t t0, t1, t2, t3, t10, t11, t12, t13, z1, z2, z3, z4, z5;
		const int sh = CONST_BITS + PASS1_BITS + 3;

		z2 = w[2]; z3 = w[6];
		z1 = (z2 + z3) * F0_541;
		t2 = z1 - z3 * F1_847;
		t3 = z1 + z2 * F0_765;
		t0 = (w[0] + w[4]) * (1 << CONST_BITS);
		t1 = (w[0] - w[4]) * (1 << CONST_BITS);
		t10 = t0 + t3; t13 = t0 - t3; t11 = t1 + t2; t12 = t1 - t2;
		t0 = w[7]; t1 = w[5]; t2 = w[3]; t3 = w[1];
		z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2; z4 = t1 + t3;
		z5 = (z3 + z4) * F1_175;
		t0 *= F0_298; t1 *= F2_053; t2 *= F3_072; t3 *= F1_501;
		z1 *= -F0_899; z2 *= -F2_562; z3 *= -F1_961; z4 *= -F0_390;
		z3 += z5; z4 += z5;
		t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
		o[0] = clamp255( DESCALE( t10 + t3, sh ) + 128 );
		o[7] = clamp255( DESCALE( t10 - t3, sh ) + 128 );
		o[1] = clamp255( DESCALE( t11 + t2, sh ) + 128 );
		o[6] = clamp255( DESCALE( t11 - t2, sh ) + 128 );
		o[2] = clamp255( DESCALE( t12 + t1, sh ) + 128 );
		o[5] = clamp255( DESCALE( t12 - t1, sh ) + 128 );
		o[3] = clamp255( DESCALE( t13 + t0, sh ) + 128 );
		o[4] = clamp255( DESCALE( t13 - t0, sh ) + 128 );
	}
}

typedef struct
{
	huff_t   dc[2], ac[2];
	uint16_t q[4][64];	/* zigzag order */
	int      comp_id[3], comp_q[3], comp_dc[3], comp_ac[3];
} jpeg_t;

static int decode_block( bits_t *b, jpeg_t *j, int c, int32_t *pred, uint8_t *out, int stride )
{
	int32_t blk[64];
	const uint16_t *q = j->q[j->comp_q[c]];
	const huff_t *ac = &j->ac[j->comp_ac[c]];
	int s, k;

	memset( blk, 0, sizeof( blk ));
	s = decode_huff( b, &j->dc[j->comp_dc[c]] );
	if( s < 0 || s > 11 )
		return -1;
	*pred += s ? extend( getbits( b, s ), s ) : 0;
	blk[0] = *pred * q[0];
	for( k = 1; k < 64; )
	{
		int rs = decode_huff( b, ac ), r;

		if( rs < 0 )
			return -1;
		r = rs >> 4;
		s = rs & 15;
		if( s )
		{
			k += r;
			if( k > 63 )
				return -1;
			blk[zigzag[k]] = extend( getbits( b, s ), s ) * q[k];
			k++;
		}
		else if( r == 15 )
			k += 16;
		else
			break;
	}
	idct( blk, out, stride );
	return 0;
}

int gc_thp_jpeg( const uint8_t *jpg, size_t len, int width, int height, uint8_t *y, uint8_t *cb, uint8_t *cr )
{
	static jpeg_t j;	/* ~6 KiB of tables: static so the GameCube's small thread stacks are safe; not reentrant */
	size_t pos = 2;
	int have_sof = 0;

	memset( &j, 0, sizeof( j ));
	if( len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8 )
		return -1;
	while( pos + 4 <= len )
	{
		uint32_t marker, seg;
		const uint8_t *d;

		if( jpg[pos] != 0xFF )
			return -2;
		marker = jpg[pos + 1];
		if( marker == 0xFF ) { pos++; continue; }	/* fill byte */
		seg = rd16( jpg + pos + 2 );
		if( seg < 2 || pos + 2 + seg > len )
			return -2;
		d = jpg + pos + 4;
		seg -= 2;
		if( marker == 0xDB )	/* DQT */
		{
			uint32_t i = 0;

			while( i + 65 <= seg )
			{
				int k;

				if( d[i] >> 4 || (d[i] & 15) > 3 ) return -3;	/* 16-bit tables: not baseline */
				for( k = 0; k < 64; k++ )
					j.q[d[i] & 15][k] = d[i + 1 + k];
				i += 65;
			}
		}
		else if( marker == 0xC4 )	/* DHT */
		{
			uint32_t i = 0;

			while( i + 17 <= seg )
			{
				int tc = d[i] >> 4, th = d[i] & 15, n = 0, k;
				huff_t *h;

				for( k = 0; k < 16; k++ )
					n += d[i + 1 + k];
				if( th > 1 || tc > 1 || n > 256 || i + 17 + (uint32_t)n > seg )
					return -3;
				h = tc ? &j.ac[th] : &j.dc[th];
				if( build_huff( h, d + i + 1, d + i + 17, n ))
					return -2;
				i += 17 + (uint32_t)n;
			}
		}
		else if( marker == 0xC0 || marker == 0xC1 )	/* SOF0/SOF1 */
		{
			int c;

			if( seg < 15 || d[0] != 8 || d[5] != 3 )
				return -3;
			if( (int)rd16( d + 1 ) != height || (int)rd16( d + 3 ) != width )
				return -4;
			for( c = 0; c < 3; c++ )
			{
				j.comp_id[c] = d[6 + c * 3];
				if( d[7 + c * 3] != (c ? 0x11 : 0x22) )
					return -3;	/* only 4:2:0 */
				j.comp_q[c] = d[8 + c * 3] & 3;
			}
			have_sof = 1;
		}
		else if( marker == 0xDD )	/* DRI */
		{
			if( seg >= 2 && rd16( d ))
				return -3;
		}
		else if( (marker >= 0xC2 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) )
			return -3;	/* progressive, lossless, arithmetic */
		else if( marker == 0xDA )	/* SOS */
		{
			bits_t b;
			int32_t pred[3] = { 0, 0, 0 };
			int c, mx, my;

			if( !have_sof || seg < 1 || d[0] != 3 || seg < 1 + 3u * 2 + 3 )
				return -3;
			for( c = 0; c < 3; c++ )
			{
				int id = d[1 + c * 2], k;

				for( k = 0; k < 3 && j.comp_id[k] != id; k++ )
					;
				if( k != c )
					return -3;	/* scan order must match the frame's */
				j.comp_dc[c] = d[2 + c * 2] >> 4 & 1;
				j.comp_ac[c] = d[2 + c * 2] & 1;
				if( !j.dc[j.comp_dc[c]].present || !j.ac[j.comp_ac[c]].present )
					return -3;
			}
			b.p = jpg + pos + 2 + seg + 2;
			b.end = jpg + len;	/* EOI and padding are never reached: decoding stops at the last MCU */
			b.acc = 0;
			b.nbits = 0;
			for( my = 0; my < height / 16; my++ )
			{
				for( mx = 0; mx < width / 16; mx++ )
				{
					uint8_t *yo = y + (size_t)my * 16 * width + mx * 16;

					if( decode_block( &b, &j, 0, &pred[0], yo, width )
						|| decode_block( &b, &j, 0, &pred[0], yo + 8, width )
						|| decode_block( &b, &j, 0, &pred[0], yo + 8 * width, width )
						|| decode_block( &b, &j, 0, &pred[0], yo + 8 * width + 8, width )
						|| decode_block( &b, &j, 1, &pred[1], cb + (size_t)my * 8 * (width / 2) + mx * 8, width / 2 )
						|| decode_block( &b, &j, 2, &pred[2], cr + (size_t)my * 8 * (width / 2) + mx * 8, width / 2 ))
						return -5;
				}
			}
			return 0;
		}
		pos += 2 + 2 + seg;
	}
	return -2;
}

/* --- textures -------------------------------------------------------------- */

void gc_thp_rgb565_tiled( const uint8_t *y, const uint8_t *cb, const uint8_t *cr, int width, int height, uint16_t *dst )
{
	int tx, ty, r, c;

	for( ty = 0; ty < height; ty += 4 )
	for( tx = 0; tx < width; tx += 4 )
	for( r = 0; r < 4; r++ )
	for( c = 0; c < 4; c++ )
	{
		const int px = tx + c, py = ty + r;
		const int32_t l = y[py * width + px];
		const int32_t u = (int32_t)cb[(py >> 1) * (width >> 1) + (px >> 1)] - 128;
		const int32_t v = (int32_t)cr[(py >> 1) * (width >> 1) + (px >> 1)] - 128;
		const uint8_t R = clamp255( l + ((91881 * v + 32768) >> 16));
		const uint8_t G = clamp255( l + ((-22554 * u - 46802 * v + 32768) >> 16));
		const uint8_t B = clamp255( l + ((116130 * u + 32768) >> 16));

		*dst++ = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
	}
}

void gc_thp_i8_tiled( const uint8_t *plane, int width, int height, uint8_t *dst )
{
	int tx, ty, r;

	for( ty = 0; ty < height; ty += 4 )
	for( tx = 0; tx < width; tx += 8 )
	for( r = 0; r < 4; r++ )
	{
		memcpy( dst, plane + (size_t)(ty + r) * width + tx, 8 );
		dst += 8;
	}
}

/* --- playback ----------------------------------------------------------------------- */

int gc_thp_player_init( gc_thp_player_t *pl, const gc_thp_t *thp, int loop )
{
	if( !thp->frames || !thp->first_frame_bytes || thp->fps <= 0.0f )
		return -1;
	pl->thp = *thp;
	pl->loop = loop != 0;
	pl->read = 0;
	pl->offset = thp->first_frame;
	pl->bytes = thp->first_frame_bytes;
	pl->clock = 0;
	return 0;
}

uint32_t gc_thp_player_rate( const gc_thp_player_t *pl )
{
	return pl->thp.audio_channels ? pl->thp.audio_rate : 1000;
}

int gc_thp_player_next( const gc_thp_player_t *pl, uint32_t *offset, uint32_t *bytes )
{
	if( !pl->loop && pl->read >= pl->thp.frames )
		return 0;
	*offset = pl->offset;
	*bytes = pl->bytes;
	return 1;
}

static uint32_t frame_ms( const gc_thp_t *thp, uint32_t k )
{
	/* exact for whole-millisecond frame times; wraps like the clock after 2^32 ms */
	double t = (double)k * 1000.0 / (double)thp->fps + 0.5;

	t -= 4294967296.0 * (double)(uint32_t)( t / 4294967296.0 );
	return (uint32_t)t;
}

int gc_thp_player_take( gc_thp_player_t *pl, const uint8_t *buf, size_t len, gc_thp_cue_t *cue )
{
	const gc_thp_t *thp = &pl->thp;
	uint32_t index;

	if( !pl->loop && pl->read >= thp->frames )
		return -1;
	if( len < pl->bytes || gc_thp_frame( thp, buf, pl->bytes, &cue->fr ))
		return -1;
	if( cue->fr.audio && cue->fr.audio_bytes < 8 )
		return -1;
	index = pl->read % thp->frames;
	cue->index = index;
	cue->pass = pl->read / thp->frames;
	if( thp->audio_channels )
	{
		cue->samples = cue->fr.audio ? rd32( cue->fr.audio + 4 ) : 0;
		cue->due = pl->clock;
		pl->clock += cue->samples;
		cue->until = pl->clock;
	}
	else
	{
		cue->samples = 0;
		cue->due = frame_ms( thp, pl->read );
		cue->until = frame_ms( thp, pl->read + 1 );
	}
	pl->read++;
	if( index + 1 == thp->frames )
	{
		/* the last frame's next_bytes names frame 0, but the header is the authority */
		pl->offset = thp->first_frame;
		pl->bytes = thp->first_frame_bytes;
	}
	else
	{
		pl->offset += pl->bytes;
		pl->bytes = cue->fr.next_bytes;
	}
	return 0;
}

int gc_thp_player_decide( const gc_thp_cue_t *cue, uint32_t now )
{
	if( (int32_t)( now - cue->due ) < 0 )
		return GC_THP_WAIT;
	if( (int32_t)( now - cue->until ) >= 0 )
		return GC_THP_SKIP;
	return GC_THP_SHOW;
}
