#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings_blob.h"

int main( void )
{
	unsigned char buf[GC_SETTINGS_MAX_BLOB], padded[8192];
	gc_setting_t in[3], out[8];
	int n, got, i;

	/* known CRC32 of "123456789" */
	assert( GC_SettingsCrc32( (const unsigned char *)"123456789", 9 ) == 0xCBF43926u );

	in[0].key = "name"; strcpy( in[0].value, "Pvt. Fox" );
	in[1].key = "sensitivity"; strcpy( in[1].value, "3.5" );
	in[2].key = "gamma"; strcpy( in[2].value, "2.2" );
	n = GC_SettingsPack( buf, sizeof( buf ), in, 3 );
	assert( n > 12 );

	/* round trip, also inside a sector-sized padded buffer as read back from the card */
	memset( padded, 0xFF, sizeof( padded ));
	memcpy( padded, buf, n );
	got = GC_SettingsUnpack( padded, (size_t)n, out, 8 );
	assert( got == 3 && !strcmp( out[0].key, "name" ) && !strcmp( out[0].value, "Pvt. Fox" ) && !strcmp( out[2].value, "2.2" ));

	/* one flipped payload bit, a truncated blob, a bad magic and a bad version are all rejected whole */
	for( i = 12; i < n; i++ )
	{
		unsigned char copy[GC_SETTINGS_MAX_BLOB];

		memcpy( copy, buf, n );
		copy[i] ^= 0x01;
		assert( GC_SettingsUnpack( copy, (size_t)n, out, 8 ) == -1 );
	}
	assert( GC_SettingsUnpack( buf, (size_t)n - 3, out, 8 ) == -1 );
	{
		unsigned char copy[GC_SETTINGS_MAX_BLOB];

		memcpy( copy, buf, n ); copy[0] ^= 1;
		assert( GC_SettingsUnpack( copy, (size_t)n, out, 8 ) == -1 );
		memcpy( copy, buf, n ); copy[4] = 9;
		assert( GC_SettingsUnpack( copy, (size_t)n, out, 8 ) == -1 );
	}

	/* keys off the whitelist and unprintable values are never written */
	in[1].key = "rcon_password";
	assert( GC_SettingsPack( buf, sizeof( buf ), in, 3 ) == 0 );
	in[1].key = "gamma"; strcpy( in[1].value, "a\nb" );
	assert( GC_SettingsPack( buf, sizeof( buf ), in, 2 ) == 0 );
	/* too big for the buffer */
	in[1].key = "gamma"; strcpy( in[1].value, "1" );
	assert( GC_SettingsPack( buf, 20, in, 2 ) == 0 );

	/* the achievement blob travels as 192 hex characters under gc_ach */
	{
		gc_setting_t ach[1];
		gc_setting_t back[2];
		unsigned char big[GC_SETTINGS_MAX_BLOB];
		int m, k;

		ach[0].key = "gc_ach";
		for( k = 0; k < 192; k++ ) ach[0].value[k] = "0123456789abcdef"[k & 15];
		ach[0].value[192] = 0;
		m = GC_SettingsPack( big, sizeof( big ), ach, 1 );
		assert( m > 192 );
		assert( GC_SettingsUnpack( big, (size_t)m, back, 2 ) == 1 );
		assert( !strcmp( back[0].key, "gc_ach" ) && !strcmp( back[0].value, ach[0].value ));
	}

	puts( "settings blob ok" );
	return 0;
}
