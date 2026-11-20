#include "../engine/common/pm_layout.h"
#include <assert.h>
#include <stdio.h>

int main( void )
{
	int v[3];

	/* parse */
	assert( PM_LayoutParse( "140400,256,32", v ) && v[0] == 140400 && v[1] == 256 && v[2] == 32 );
	assert( PM_LayoutParse( "325072,600,64", v ) && v[0] == 325072 && v[1] == 600 && v[2] == 64 );
	assert( !PM_LayoutParse( "", v ) && !PM_LayoutParse( 0, v ));
	assert( !PM_LayoutParse( "1,2", v ) && !PM_LayoutParse( "1,2,3,4", v ) && !PM_LayoutParse( "1,2,x", v ));
	assert( !PM_LayoutParse( "1,,3", v ) && !PM_LayoutParse( "1,2,3 ", v ) && !PM_LayoutParse( "-1,2,3", v ));
	assert( !PM_LayoutParse( "999999999999,2,3", v ));
	/* match: no published layout means no check */
	assert( PM_LayoutMatches( 0, 140400, 256, 32 ) && PM_LayoutMatches( "", 140400, 256, 32 ));
	assert( PM_LayoutMatches( "140400,256,32", 140400, 256, 32 ));
	/* the failure that crashed in PM_InitTextureTypes: DLL 256/32, engine stock 600/64 */
	assert( !PM_LayoutMatches( "140400,256,32", 325072, 600, 64 ));
	assert( !PM_LayoutMatches( "325072,600,64", 140400, 256, 32 ));
	/* same size, different caps, and garbage, both fail */
	assert( !PM_LayoutMatches( "140400,256,48", 140400, 256, 32 ));
	assert( !PM_LayoutMatches( "garbage", 140400, 256, 32 ));
	puts( "pm_layout test PASS" );
	return 0;
}
