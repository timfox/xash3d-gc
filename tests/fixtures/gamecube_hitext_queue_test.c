#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hitext_queue.h"

static ht_queue_t q;

int main( void )
{
	int i;

	memset( &q, 0, sizeof( q ));
	/* ARGB 0x80FF8000 becomes RGBA 0xFF800080 */
	assert( htq_push( &q, 5, 95, 10, 20, 8, 16, 0x80FF8000 ));
	assert( q.count == 1 && q.quad[0].rgba == 0xFF800080u && q.quad[0].ch == 5 );
	/* out of range glyph indexes are refused */
	assert( !htq_push( &q, -1, 95, 0, 0, 1, 1, 0 ));
	assert( !htq_push( &q, 95, 95, 0, 0, 1, 1, 0 ));
	assert( q.count == 1 );

	/* a box over a glyph's centre drops it; one beside it does not */
	htq_occlude( &q, 0, 0, 12, 22, 255 );	/* centre (14,28): outside */
	assert( q.count == 1 );
	htq_occlude( &q, 0, 0, 100, 100, 100 );	/* translucent: text shows through */
	assert( q.count == 1 );
	htq_occlude( &q, 0, 0, 100, 100, 255 );
	assert( q.count == 0 );

	/* the queue stops at HT_MAX_QUEUE and keeps order after a partial occlusion */
	for( i = 0; i < HT_MAX_QUEUE; i++ )
		assert( htq_push( &q, 1, 95, (float)i, 0, 2, 2, 0xFFFFFFFF ));
	assert( !htq_push( &q, 1, 95, 0, 0, 2, 2, 0xFFFFFFFF ));
	htq_occlude( &q, 100, -5, 50, 20, 255 );	/* x 100..150: glyphs 99..149 have centres 100..150 */
	assert( q.count == HT_MAX_QUEUE - 51 );
	assert( q.quad[98].x == 98.0f && q.quad[99].x == 150.0f );
	puts( "hitext queue ok" );
	return 0;
}
