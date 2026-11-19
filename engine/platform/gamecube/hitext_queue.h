/*
hitext_queue.h - the glyph queue behind GC_HiTextGlyph / GC_HiTextOcclude (pure C, host-testable)
*/
#pragma once

#define HT_MAX_QUEUE 900

typedef struct
{
	float x, y, h;     // 640x480 space: the cell's top left and its height
	float w;           // cell width
	unsigned short ch; // index into the atlas glyph table
	unsigned int rgba; // 0xRRGGBBAA
} ht_quad_t;

typedef struct
{
	ht_quad_t quad[HT_MAX_QUEUE];
	int       count;
} ht_queue_t;

/* ulRGBA is ARGB (as pfnDrawCharacter gets it). Returns 0 when the glyph index is out of range or the queue is full. */
static inline int htq_push( ht_queue_t *q, int index, int glyph_count, float x, float y, float w, float h, int ulRGBA )
{
	ht_quad_t *t;

	if( index < 0 || index >= glyph_count || q->count >= HT_MAX_QUEUE )
		return 0;
	t = &q->quad[q->count++];
	t->x = x;
	t->y = y;
	t->w = w;
	t->h = h;
	t->ch = (unsigned short)index;
	t->rgba = ( (unsigned)( ulRGBA & 0x00FFFFFF ) << 8 ) | (unsigned)(( ulRGBA >> 24 ) & 0xFF );
	return 1;
}

/* An opaque box drawn after some text covers it: drop the queued glyphs whose centre is under it. */
static inline void htq_occlude( ht_queue_t *q, int x, int y, int w, int h, int alpha )
{
	const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	int i, out = 0;

	if( !q->count || alpha < 160 )
		return;
	for( i = 0; i < q->count; i++ )
	{
		const float cx = q->quad[i].x + q->quad[i].w * 0.5f, cy = q->quad[i].y + q->quad[i].h * 0.5f;

		if( cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1 )
			continue;
		q->quad[out++] = q->quad[i];
	}
	q->count = out;
}
