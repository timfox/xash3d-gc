/*
gc_pack.h - readers for GxBake's packed GameCube formats, for the Xash3D GC port.

Drop-in, dependency-free C89 (plus <stdint.h>). Everything here is plain
integer code that runs the same on Gekko and on a PC, so it can be host-tested
(GxBake's tests compile port/test_gc_pack.c and compare against its encoders).
The GX calls live behind GC_PACK_GX (define it on the GameCube build).

  Yaz0        gc_yaz0_size, gc_yaz0_decode            (any file GxBake wrote with `gxbake yaz0`)
  DSP ADPCM   gc_dsp_parse, gc_dsp_decode             (.dsp from `gxbake adpcm` / `squeeze --emit`)
  GCSK skins  gc_gsk_parse, gc_gsk_entry, gc_gsk_bind (.gsk from `squeeze --emit`)
  GCSV verts  gc_gsv_parse, gc_gsv_entry, gc_gsv_dequant (.gsv from `squeeze --emit`)

Formats: GxBake docs/packing.md. All file data is big-endian; these readers
use byte loads, so they do not care about the host's byte order or alignment.
*/
#ifndef GC_PACK_H
#define GC_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Yaz0 --------------------------------------------------------------- */

/* Decompressed size, or 0 if src is not Yaz0. */
uint32_t gc_yaz0_size( const uint8_t *src, size_t srclen );
/* Decode into dst (dstlen >= gc_yaz0_size). Returns bytes written, or 0 on a
 * truncated or corrupt stream (never reads or writes out of bounds). */
uint32_t gc_yaz0_decode( const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen );

/* --- DSP ADPCM ------------------------------------------------------------ */

#define GC_DSP_HEADER_BYTES 96
#define GC_DSP_FRAME_SAMPLES 14
#define GC_DSP_FRAME_BYTES 8

typedef struct
{
	uint32_t samples;
	uint32_t nibbles;
	uint32_t rate;
	uint16_t loop;		/* 1 = loops */
	uint32_t loop_start;	/* sample index */
	uint32_t loop_end;	/* last sample played, inclusive */
	int16_t  coefs[16];	/* 8 pairs, 1.11 fixed point */
	uint8_t  loop_ps;	/* frame header at the loop start */
	int16_t  loop_h1, loop_h2;	/* decoder history at the loop start */
	const uint8_t *frames;	/* points into the file */
	uint32_t frames_bytes;
} gc_dsp_t;

typedef struct
{
	int16_t h1, h2;
} gc_dsp_state_t;

/* Parse a .dsp file; returns 0 on success. dsp->frames points into blob. */
int gc_dsp_parse( const uint8_t *blob, size_t len, gc_dsp_t *dsp );
/* Decode `count` samples starting at sample `first` (which must be a frame
 * start, i.e. a multiple of 14, unless you continue a previous call with the
 * same state). Writes count s16 samples to out. Returns samples written. */
uint32_t gc_dsp_decode( const gc_dsp_t *dsp, uint32_t first, uint32_t count, gc_dsp_state_t *st, int16_t *out );

/* --- GCSK skin packs ---------------------------------------------------- */

#define GC_GSK_FLAG_ALPHA 1

typedef struct
{
	uint16_t skin;		/* index into the model's mstudiotexture_t array */
	uint8_t  fmt;		/* GXTexFmt: GX_TF_CI8 (9), GX_TF_CI4 (8), GX_TF_CMPR (14), GX_TF_I4, ... */
	uint8_t  tlut_fmt;	/* GXTlutFmt: GX_TL_IA8 (0), GX_TL_RGB565 (1), GX_TL_RGB5A3 (2) */
	uint16_t width, height;
	uint16_t tlut_entries;	/* 0 for direct formats; a multiple of 16 otherwise */
	uint16_t flags;
	const uint8_t *texels;	/* 32-byte aligned when the file is loaded 32-byte aligned */
	uint32_t texel_bytes;
	const uint8_t *tlut;	/* NULL for direct formats */
	uint32_t tlut_bytes;
} gc_gsk_entry_t;

/* Number of skins in a .gsk file, or -1 if it is not one (or is truncated). */
int gc_gsk_parse( const uint8_t *blob, size_t len );
/* Fill *e for skin i; returns 0 on success. */
int gc_gsk_entry( const uint8_t *blob, size_t len, int i, gc_gsk_entry_t *e );

/* --- GCSV s16 vertices ------------------------------------------------- */

typedef struct
{
	uint16_t bodypart, model;
	uint32_t nverts, nnorms;
	uint8_t  pos_frac;	/* value = s16 / 2^pos_frac: the GQR dequantise scale */
	uint8_t  nrm_frac;	/* 6: GX's s8 normal format */
	const uint8_t *positions;	/* big-endian s16 x,y,z; 32-byte aligned in the file */
	const uint8_t *normals;	/* s8 x,y,z */
} gc_gsv_entry_t;

/* Number of models in a .gsv file, or -1. */
int gc_gsv_parse( const uint8_t *blob, size_t len );
int gc_gsv_entry( const uint8_t *blob, size_t len, int i, gc_gsv_entry_t *e );
/* Portable dequantise to floats (pos and nrm hold 3 × count each). On the
 * GameCube use psq_l with GQR type s16 and scale pos_frac instead: same
 * numbers, no conversion loop. */
void gc_gsv_dequant( const gc_gsv_entry_t *e, float *pos, float *nrm );

#ifdef GC_PACK_GX
#include <ogc/gx.h>
/* Initialise a texture object (and TLUT object, loaded into tlut_name) for a
 * skin. Call DCFlushRange on the file once after loading it. Returns 0 on
 * success. CI skins: up to 16 TLUTs of 256 entries can be resident at once
 * (GX_TLUT0..GX_TLUT15); give each skin drawn in a frame its own. */
int gc_gsk_bind( const gc_gsk_entry_t *e, GXTexObj *obj, GXTlutObj *tlut, u32 tlut_name, u8 wrap );
#endif

#ifdef __cplusplus
}
#endif
#endif
