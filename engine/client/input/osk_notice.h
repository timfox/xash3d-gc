/*
osk_notice.h - the centred notice panel (controller disconnected, ...), pure C so a host test checks it stays inside the
5% title-safe area (32 px sides, 24 px top and bottom at 640x480).
*/
#pragma once

#define OSK_NOTICE_GLYPH_W 6	// bitmap glyph advance, font pixels
#define OSK_NOTICE_GLYPH_H 8
#define OSK_NOTICE_PAD     14
#define OSK_NOTICE_SAFE_X  32
#define OSK_NOTICE_SAFE_Y  24

typedef struct
{
	int x, y, w, h;
} osk_notice_rect_t;

/* lines of text of at most max_chars characters each, font pixel scale fp; the first line is the title.
 * Returns the panel rect in 640x480 pixels, clamped to the safe area (the caller wraps text to fit max_chars). */
static inline osk_notice_rect_t OSK_NoticeRect( int lines, int max_chars, int fp )
{
	osk_notice_rect_t r;
	const int max_w = 640 - 2 * OSK_NOTICE_SAFE_X, max_h = 480 - 2 * OSK_NOTICE_SAFE_Y;

	r.w = max_chars * OSK_NOTICE_GLYPH_W * fp + 2 * OSK_NOTICE_PAD;
	r.h = lines * ( OSK_NOTICE_GLYPH_H + 2 ) * fp + 2 * OSK_NOTICE_PAD;
	if( r.w > max_w ) r.w = max_w;
	if( r.h > max_h ) r.h = max_h;
	r.x = ( 640 - r.w ) / 2;
	r.y = ( 480 - r.h ) / 2;
	return r;
}
