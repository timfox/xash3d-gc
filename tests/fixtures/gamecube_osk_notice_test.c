#include <assert.h>
#include <stdio.h>
#include "osk_notice.h"

static void inside_safe( osk_notice_rect_t r )
{
	assert( r.x >= OSK_NOTICE_SAFE_X && r.y >= OSK_NOTICE_SAFE_Y );
	assert( r.x + r.w <= 640 - OSK_NOTICE_SAFE_X && r.y + r.h <= 480 - OSK_NOTICE_SAFE_Y );
	/* the 2 px border the caller draws around it stays on screen too */
	assert( r.x - 2 >= 0 && r.y - 2 >= 0 && r.x + r.w + 2 <= 640 && r.y + r.h + 2 <= 480 );
}

int main( void )
{
	osk_notice_rect_t r;

	/* the controller notice: 3 lines, 23 characters, font scale 2 */
	r = OSK_NoticeRect( 3, 23, 2 );
	inside_safe( r );
	assert( r.x == ( 640 - r.w ) / 2 && r.y == ( 480 - r.h ) / 2 );	/* centred */
	/* oversized text is clamped to the safe area, never past it */
	r = OSK_NoticeRect( 40, 90, 2 );
	inside_safe( r );
	assert( r.w == 640 - 2 * OSK_NOTICE_SAFE_X && r.h == 480 - 2 * OSK_NOTICE_SAFE_Y );
	puts( "osk notice ok" );
	return 0;
}
