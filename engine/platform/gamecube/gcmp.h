/*
gcmp.h - baked GX CMPR sidecar files ("GCMP"), platform independent
Copyright (C) 2026 xash3d-gc contributors

A .cmp file is a 32-byte header followed by GX CMPR (DXT1) texels laid out in
8x8 tiles of 2x2 blocks, big-endian. The disc builder writes them next to the
original image so the engine can use a texture without decoding the TGA/BMP.

Header (big-endian):
  0  "GCMP"
  4  u16 width            (multiple of 8)
  6  u16 height           (multiple of 8)
  8  u8  format           0 = CMPR
  9  u8  mip levels       1 = base level only
  10 u16 cache_version    0 = legacy file (menu tiles, banner), else GC_GCMP_VERSION
  12 u32 source size      bytes of the original file
  16 u32 source crc32     of the original file
  20 12 bytes zero

The engine uses a file only when the header parses and, for versioned uses, the
cache_version equals GC_GCMP_VERSION; otherwise it loads the original image.
Header only, no libc beyond stdint/stddef, so it can be tested on the host.
*/
#ifndef GCMP_H
#define GCMP_H

#include <stddef.h>
#include <stdint.h>

#define GC_GCMP_HEADER_BYTES 32u
#define GC_GCMP_VERSION      1u
#define GC_GCMP_FORMAT_CMPR  0u

typedef struct
{
	uint16_t w, h;
	uint8_t  format, mips;
	uint16_t version;
	uint32_t src_size, src_crc;
} gc_gcmp_header_t;

/* 1 and *out filled if buf (len bytes) is a valid header, else 0. */
static inline int GC_GcmpParseHeader( const uint8_t *buf, size_t len, gc_gcmp_header_t *out )
{
	if( !buf || len < GC_GCMP_HEADER_BYTES || buf[0] != 'G' || buf[1] != 'C' || buf[2] != 'M' || buf[3] != 'P' )
		return 0;
	out->w = (uint16_t)(( buf[4] << 8 ) | buf[5] );
	out->h = (uint16_t)(( buf[6] << 8 ) | buf[7] );
	out->format = buf[8];
	out->mips = buf[9] ? buf[9] : 1;
	out->version = (uint16_t)(( buf[10] << 8 ) | buf[11] );
	out->src_size = ( (uint32_t)buf[12] << 24 ) | ( (uint32_t)buf[13] << 16 ) | ( (uint32_t)buf[14] << 8 ) | buf[15];
	out->src_crc = ( (uint32_t)buf[16] << 24 ) | ( (uint32_t)buf[17] << 16 ) | ( (uint32_t)buf[18] << 8 ) | buf[19];
	if( out->w < 8 || out->h < 8 || ( out->w & 7 ) || ( out->h & 7 ) || out->format != GC_GCMP_FORMAT_CMPR )
		return 0;
	return 1;
}

/* Bytes of the base level of a w x h CMPR image. */
static inline uint32_t GC_GcmpBaseBytes( unsigned w, unsigned h )
{
	return (uint32_t)w * h / 2u;
}

/* Bytes of one row of 8x8 tiles. */
static inline uint32_t GC_GcmpTileRowBytes( unsigned w )
{
	return ( w / 8u ) * 32u;
}

static inline uint16_t GC_GcmpBE16( const uint8_t *p )
{
	return (uint16_t)(( p[0] << 8 ) | p[1] );
}

static inline void GC_GcmpExpand565( uint16_t c, int rgb[3] )
{
	const int r = ( c >> 11 ) & 31, g = ( c >> 5 ) & 63, b = c & 31;

	rgb[0] = ( r << 3 ) | ( r >> 2 );
	rgb[1] = ( g << 2 ) | ( g >> 4 );
	rgb[2] = ( b << 3 ) | ( b >> 2 );
}

/*
 * One texel (x, y) of an image of width w, from `tile_row`, the bytes of the row
 * of 8x8 tiles that contains y (GC_GcmpTileRowBytes(w) bytes). Returns RGB565;
 * the 1-bit-alpha index of a 3-colour block decodes to black.
 */
static inline uint16_t GC_GcmpTexel( const uint8_t *tile_row, unsigned x, unsigned y )
{
	const uint8_t *blk = tile_row + ( x >> 3 ) * 32u + ((( y & 7u ) >> 2 ) * 2u + (( x & 7u ) >> 2 )) * 8u;
	const uint16_t c0 = GC_GcmpBE16( blk ), c1 = GC_GcmpBE16( blk + 2 );
	const unsigned idx = ( blk[4 + ( y & 3u )] >> ( 6u - 2u * ( x & 3u ))) & 3u;
	int e0[3], e1[3], rgb[3], k;

	GC_GcmpExpand565( c0, e0 );
	GC_GcmpExpand565( c1, e1 );
	for( k = 0; k < 3; k++ )
	{
		if( idx == 0 )
			rgb[k] = e0[k];
		else if( idx == 1 )
			rgb[k] = e1[k];
		else if( c0 > c1 )
			rgb[k] = idx == 2 ? ( 2 * e0[k] + e1[k] ) / 3 : ( e0[k] + 2 * e1[k] ) / 3;
		else
			rgb[k] = idx == 2 ? ( e0[k] + e1[k] ) / 2 : 0;
	}
	return (uint16_t)((( rgb[0] >> 3 ) << 11 ) | (( rgb[1] >> 2 ) << 5 ) | ( rgb[2] >> 3 ));
}

#endif /* GCMP_H */
