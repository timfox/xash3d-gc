/*
r_gx_cmpr.c - GX CMPR (S3TC/DXT1) encoding of studio skins at upload
Copyright (C) 2026 xash3d-gc contributors

Flipper samples CMPR natively: 4 bits per texel instead of the 16 of the
RGB565 / RGB5A3 native skins, so a character's whole skin set takes a quarter
of the MEM1 it did and keeps its retail resolution.  The encoder is the C form
of the disc build's gx_cmpr_encode_blocks (scripts/build-gamecube-disc.py):
principal-axis endpoints per 4x4 block, three colours plus transparent for
blocks that hold cut-out texels, blocks laid out in 8x8 tiles of 2x2 blocks,
big-endian.  It resamples on the fly (bilinear colour, nearest coverage), so
no full-size intermediate buffer is needed.
*/
#include "r_local.h"

#if XASH_GAMECUBE
#include <gccore.h>
#include <math.h>

static unsigned R_GXCmprPow2( int v, int cap )
{
	unsigned p = 8;

	/* Nearest power of two in log terms (round up past v / sqrt(2)): keeps
	 * retail detail and satisfies GX REPEAT.  96 -> 128, 68 -> 64. */
	while( p < (unsigned)cap && 2u * p * p < (unsigned)v * (unsigned)v )
		p <<= 1;
	return p;
}

void R_GXCmprSkinSize( int w, int h, int *ow, int *oh )
{
	*ow = (int)R_GXCmprPow2( w, 256 );
	*oh = (int)R_GXCmprPow2( h, 256 );
}

size_t R_GXCmprBytes( int w, int h )
{
	return (size_t)w * (size_t)h / 2;
}

static void R_GXCmprSample( const byte *src, int sw, int sh, int dw, int dh, int x, int y,
	int masked, int rgb[3], int *opaque )
{
	/* Texel centre in source space. */
	const float fx = (( x + 0.5f ) * sw ) / dw - 0.5f;
	const float fy = (( y + 0.5f ) * sh ) / dh - 0.5f;
	int x0 = (int)fx, y0 = (int)fy, x1, y1, k;
	float ax, ay;
	const byte *p00, *p01, *p10, *p11, *pn;

	if( fx < 0.0f ) x0 = 0;
	if( fy < 0.0f ) y0 = 0;
	if( x0 > sw - 1 ) x0 = sw - 1;
	if( y0 > sh - 1 ) y0 = sh - 1;
	x1 = x0 + 1 < sw ? x0 + 1 : x0;
	y1 = y0 + 1 < sh ? y0 + 1 : y0;
	ax = fx - x0; ay = fy - y0;
	if( ax < 0.0f ) ax = 0.0f; if( ax > 1.0f ) ax = 1.0f;
	if( ay < 0.0f ) ay = 0.0f; if( ay > 1.0f ) ay = 1.0f;
	p00 = src + ( y0 * sw + x0 ) * 4;
	p01 = src + ( y0 * sw + x1 ) * 4;
	p10 = src + ( y1 * sw + x0 ) * 4;
	p11 = src + ( y1 * sw + x1 ) * 4;
	pn = ( ax < 0.5f ) ? ( ay < 0.5f ? p00 : p10 ) : ( ay < 0.5f ? p01 : p11 );
	*opaque = !masked || pn[3] >= 128;
	for( k = 0; k < 3; k++ )
	{
		float top = p00[k] + ( p01[k] - p00[k] ) * ax;
		float bot = p10[k] + ( p11[k] - p10[k] ) * ax;
		rgb[k] = (int)( top + ( bot - top ) * ay + 0.5f );
	}
	if( masked && *opaque )
	{
		/* Keep cut-out edges from bleeding the transparent key colour in. */
		int sum = 0;
		const byte *q[4] = { p00, p01, p10, p11 };
		int n = 0, acc[3] = { 0, 0, 0 };

		for( k = 0; k < 4; k++ )
		{
			if( q[k][3] < 128 )
				continue;
			acc[0] += q[k][0]; acc[1] += q[k][1]; acc[2] += q[k][2]; n++;
		}
		if( n && n < 4 )
		{
			rgb[0] = acc[0] / n; rgb[1] = acc[1] / n; rgb[2] = acc[2] / n;
		}
		(void)sum;
	}
}

static u16 R_GXTo565( const float c[3] )
{
	int r = (int)( c[0] + 0.5f ), g = (int)( c[1] + 0.5f ), b = (int)( c[2] + 0.5f );

	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return (u16)((( r >> 3 ) << 11 ) | (( g >> 2 ) << 5 ) | ( b >> 3 ));
}

static void R_GXFrom565( u16 v, int c[3] )
{
	const int r = ( v >> 11 ) & 31, g = ( v >> 5 ) & 63, b = v & 31;

	c[0] = ( r << 3 ) | ( r >> 2 );
	c[1] = ( g << 2 ) | ( g >> 4 );
	c[2] = ( b << 3 ) | ( b >> 2 );
}

/* One 4x4 block -> 8 bytes. Returns true when it used the transparent index. */
static qboolean R_GXCmprBlock( const int px[16][3], const int op[16], byte *out )
{
	float mean[3] = { 0, 0, 0 }, cov[6] = { 0, 0, 0, 0, 0, 0 }, axis[3] = { 1, 1, 1 };
	float lo = 1e9f, hi = -1e9f, e[3];
	int n = 0, i, k, it, pal[4][3];
	qboolean has_alpha = false;
	u16 a, b, c0, c1;
	byte idx[16];

	for( i = 0; i < 16; i++ )
	{
		if( !op[i] ) { has_alpha = true; continue; }
		for( k = 0; k < 3; k++ ) mean[k] += px[i][k];
		n++;
	}
	if( !n )
	{
		/* Fully transparent: c0 == c1 selects 3-colour mode, every index 3. */
		out[0] = out[1] = out[2] = out[3] = 0;
		out[4] = out[5] = out[6] = out[7] = 0xFF;
		return true;
	}
	for( k = 0; k < 3; k++ ) mean[k] /= n;
	for( i = 0; i < 16; i++ )
	{
		float d0, d1, d2;

		if( !op[i] ) continue;
		d0 = px[i][0] - mean[0]; d1 = px[i][1] - mean[1]; d2 = px[i][2] - mean[2];
		cov[0] += d0 * d0; cov[1] += d0 * d1; cov[2] += d0 * d2;
		cov[3] += d1 * d1; cov[4] += d1 * d2; cov[5] += d2 * d2;
	}
	for( it = 0; it < 6; it++ )	/* power iteration on the 3x3 covariance */
	{
		float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
		float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
		float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
		float m = x * x + y * y + z * z;

		if( m < 1e-12f ) break;
		m = 1.0f / sqrtf( m );
		axis[0] = x * m; axis[1] = y * m; axis[2] = z * m;
	}
	for( i = 0; i < 16; i++ )
	{
		float t;

		if( !op[i] ) continue;
		t = ( px[i][0] - mean[0] ) * axis[0] + ( px[i][1] - mean[1] ) * axis[1] + ( px[i][2] - mean[2] ) * axis[2];
		if( t < lo ) lo = t;
		if( t > hi ) hi = t;
	}
	for( k = 0; k < 3; k++ ) e[k] = mean[k] + axis[k] * hi;
	a = R_GXTo565( e );
	for( k = 0; k < 3; k++ ) e[k] = mean[k] + axis[k] * lo;
	b = R_GXTo565( e );
	/* c0 > c1: four colours; c0 <= c1: three plus transparent. */
	if( has_alpha ) { c0 = a < b ? a : b; c1 = a < b ? b : a; }
	else { c0 = a > b ? a : b; c1 = a > b ? b : a; }
	R_GXFrom565( c0, pal[0] );
	R_GXFrom565( c1, pal[1] );
	for( k = 0; k < 3; k++ )
	{
		if( has_alpha )
		{
			pal[2][k] = ( pal[0][k] + pal[1][k] ) / 2;
			pal[3][k] = 0;
		}
		else
		{
			pal[2][k] = ( 2 * pal[0][k] + pal[1][k] ) / 3;
			pal[3][k] = ( pal[0][k] + 2 * pal[1][k] ) / 3;
		}
	}
	for( i = 0; i < 16; i++ )
	{
		int best = 0, bestd = 0x7fffffff, j, choices = has_alpha ? 3 : 4;

		if( !op[i] ) { idx[i] = 3; continue; }
		if( c0 == c1 && !has_alpha ) { idx[i] = 0; continue; }
		for( j = 0; j < choices; j++ )
		{
			const int dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
			const int d = dr * dr + dg * dg + db * db;

			if( d < bestd ) { bestd = d; best = j; }
		}
		idx[i] = (byte)best;
	}
	out[0] = (byte)( c0 >> 8 ); out[1] = (byte)c0;
	out[2] = (byte)( c1 >> 8 ); out[3] = (byte)c1;
	for( i = 0; i < 4; i++ )
		out[4 + i] = (byte)(( idx[i * 4] << 6 ) | ( idx[i * 4 + 1] << 4 ) | ( idx[i * 4 + 2] << 2 ) | idx[i * 4 + 3] );
	return has_alpha;
}

/*
Encode an RGBA8 source (sw x sh) into dw x dh GX CMPR at dst (R_GXCmprBytes).
dw, dh are multiples of 8.  masked: alpha < 128 is a cut-out texel.
Returns true when any block uses the transparent index.
*/
qboolean R_GXEncodeCmprSkin( const byte *src, int sw, int sh, int dw, int dh, qboolean masked, byte *dst )
{
	qboolean any_alpha = false;
	int tx, ty, b, i;
	int px[16][3], op[16];
	byte *out = dst;

	for( ty = 0; ty < dh; ty += 8 )
		for( tx = 0; tx < dw; tx += 8 )
			for( b = 0; b < 4; b++ )
			{
				const int bx = tx + ( b & 1 ) * 4, by = ty + ( b >> 1 ) * 4;

				for( i = 0; i < 16; i++ )
					R_GXCmprSample( src, sw, sh, dw, dh, bx + ( i & 3 ), by + ( i >> 2 ), masked, px[i], &op[i] );
				if( R_GXCmprBlock( px, op, out ))
					any_alpha = true;
				out += 8;
			}
	DCFlushRange( dst, (u32)R_GXCmprBytes( dw, dh ));
	return any_alpha;
}
#endif /* XASH_GAMECUBE */
