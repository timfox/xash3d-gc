/*
gc_thp.h - a THP movie reader for the Xash3D GameCube port (or any libogc program).

THP is the GameCube SDK's movie format: back-to-back frames, each a baseline
4:2:0 JPEG picture plus, optionally, a block of stereo DSP ADPCM. `gxbake video`
writes it from any video FFmpeg can read. This file has everything a player
needs that is not a GX call:

  gc_thp_parse         the header: size, fps, frame count, audio, first frame
  gc_thp_frame         one frame: next frame's size, the picture, the audio block
  gc_thp_audio         an audio block -> interleaved stereo s16 (what ASND takes)
  gc_thp_jpeg          a picture -> Y, Cb, Cr planes (Cb/Cr at half size)
  gc_thp_rgb565_tiled  planes -> a GX_TF_RGB565 texture (4x4 tiles), CPU path
  gc_thp_i8_tiled      one plane -> a GX_TF_I8 texture (8x4 tiles), for a TEV
                       YUV->RGB path that skips the per-pixel CPU conversion
  gc_thp_player_*      playback bookkeeping with no I/O: which bytes to read next,
                       when each picture is due on the audio clock, whether to
                       show, wait or skip it, and looping

Plain C89 plus <stdint.h>, no allocation, byte-wise big-endian reads, bounds
checked. GxBake's tests compile it on the host (-Werror) and compare it with
FFmpeg's decode and with GxBake's own encoders.

A THP picture is not a .jpg file: its scan is not byte-stuffed (a 0xFF in the
entropy data is not followed by 0x00), so gc_thp_jpeg will not read ordinary
JPEG files, and libjpeg will not read THP pictures as they are.
*/
#ifndef GC_THP_H
#define GC_THP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_THP_HEADER_BYTES 0x60	/* read this much to call gc_thp_parse (a no-audio file needs 0x50) */

typedef struct
{
	uint32_t version;		/* 0x00010000 or 0x00011000 */
	uint32_t width, height;	/* multiples of 16 */
	float    fps;
	uint32_t frames;
	uint32_t max_frame_bytes;	/* allocate two buffers of this (32-byte aligned) to double-buffer reads */
	uint32_t max_audio_samples;	/* per frame, per channel */
	uint32_t first_frame;		/* file offset of frame 0 */
	uint32_t first_frame_bytes;	/* size of frame 0; every frame then gives the next one's */
	uint32_t audio_channels;	/* 0 (no audio) or 2 */
	uint32_t audio_rate;		/* 32000 or 48000 */
	uint32_t audio_samples;	/* whole movie, per channel */
} gc_thp_t;

typedef struct
{
	uint32_t next_bytes;		/* size of the following frame (the last frame gives frame 0's) */
	const uint8_t *video;
	uint32_t video_bytes;
	const uint8_t *audio;		/* NULL without audio */
	uint32_t audio_bytes;
} gc_thp_frame_t;

/* Parse the header (at least GC_THP_HEADER_BYTES, or the whole file). 0 on success. */
int gc_thp_parse( const uint8_t *buf, size_t len, gc_thp_t *thp );
/* Split one frame (buf holds the frame, len = its size). 0 on success. */
int gc_thp_frame( const gc_thp_t *thp, const uint8_t *buf, size_t len, gc_thp_frame_t *fr );
/* Decode an audio block to interleaved stereo s16 in native byte order (big-
 * endian on the GameCube, as ASND's VOICE_STEREO_16BIT wants). Writes at most
 * max_samples sample pairs; returns how many, or 0 on a bad block. */
uint32_t gc_thp_audio( const uint8_t *block, size_t len, int16_t *out, uint32_t max_samples );

/* Decode a picture into planes: y is width*height, cb and cr are
 * (width/2)*(height/2), each row-major with no padding. Only what THP uses is
 * supported: baseline (SOF0/SOF1) 8-bit, three components 2x2/1x1/1x1, no
 * restart interval. 0 on success, negative on an unsupported or corrupt picture
 * (a truncated scan decodes as far as it goes and returns 0). */
int gc_thp_jpeg( const uint8_t *jpg, size_t len, int width, int height, uint8_t *y, uint8_t *cb, uint8_t *cr );

/* JFIF YCbCr -> RGB565, nearest chroma, written as a GX_TF_RGB565 texture
 * (4x4 texel tiles, tiles left to right then top to bottom, native-endian
 * u16). width and height are multiples of 4 (THP sizes are multiples of 16). */
void gc_thp_rgb565_tiled( const uint8_t *y, const uint8_t *cb, const uint8_t *cr, int width, int height, uint16_t *dst );
/* One plane as a GX_TF_I8 texture (8x4 tiles). width multiple of 8, height of 4. */
void gc_thp_i8_tiled( const uint8_t *plane, int width, int height, uint8_t *dst );

/* --- playback ---------------------------------------------------------------
 * The player does no I/O and calls no audio or GX function; the caller reads,
 * queues sound and draws. A frame is always read and its sound always queued;
 * only the picture may be skipped. The loop is:
 *
 *   gc_thp_player_init( &pl, &thp, loop );
 *   while( gc_thp_player_next( &pl, &off, &bytes ))   read bytes at off (DVD, async)
 *   {
 *       gc_thp_player_take( &pl, buf, bytes, &cue );   split it, advance
 *       queue gc_thp_audio( cue.fr.audio, ... ) on the ASND voice
 *       switch( gc_thp_player_decide( &cue, now ))     now = the clock below
 *         GC_THP_WAIT: not due yet, keep the last picture up and ask again later
 *         GC_THP_SHOW: gc_thp_jpeg, upload, draw
 *         GC_THP_SKIP: the next picture is already due; don't decode this one
 *   }
 *
 * The clock runs at gc_thp_player_rate ticks a second: sample pairs played
 * (audio_rate) when the movie has sound, so the picture follows the sound and
 * never drifts from it; milliseconds since the start otherwise. It is a uint32
 * that may wrap (24 h at 48 kHz); comparisons are done modulo 2^32. A picture
 * is due from cue.due up to, not including, cue.until (the next one's due). */
enum { GC_THP_WAIT = 0, GC_THP_SHOW = 1, GC_THP_SKIP = 2 };

typedef struct
{
	gc_thp_t thp;
	int loop;
	uint32_t read;		/* frames taken so far, counting every pass of a loop */
	uint32_t offset, bytes;	/* where the next frame is in the file, and its size */
	uint32_t clock;		/* with audio: sample pairs before the next frame's block */
} gc_thp_player_t;

typedef struct
{
	uint32_t index;		/* frame within the movie, 0 .. frames-1 */
	uint32_t pass;		/* 0 the first time through, 1 after the first loop, ... */
	uint32_t due, until;	/* clock ticks: the picture belongs on screen in [due, until) */
	uint32_t samples;	/* sample pairs in this frame's audio block (0 without audio) */
	gc_thp_frame_t fr;
} gc_thp_cue_t;

/* Start at frame 0. loop != 0 wraps to frame 0 after the last frame forever. 0 on success. */
int gc_thp_player_init( gc_thp_player_t *pl, const gc_thp_t *thp, int loop );
/* Ticks per second of the playback clock: audio_rate with sound, else 1000. */
uint32_t gc_thp_player_rate( const gc_thp_player_t *pl );
/* 1 and where to read the next frame, or 0 at the end of a movie that does not loop. */
int gc_thp_player_next( const gc_thp_player_t *pl, uint32_t *offset, uint32_t *bytes );
/* The frame gc_thp_player_next named has been read into buf (len bytes): split it into
 * cue->fr, give its timing, and move on to the following frame. 0 on success; on a bad
 * frame the player does not advance. */
int gc_thp_player_take( gc_thp_player_t *pl, const uint8_t *buf, size_t len, gc_thp_cue_t *cue );
/* GC_THP_WAIT, GC_THP_SHOW or GC_THP_SKIP for this cue at clock time now. */
int gc_thp_player_decide( const gc_thp_cue_t *cue, uint32_t now );

#ifdef __cplusplus
}
#endif
#endif
