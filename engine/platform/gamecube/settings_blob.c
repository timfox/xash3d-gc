#include "settings_blob.h"
#include <string.h>

const char *const gc_settings_keys[] = { "name", "sensitivity", "volume", "MP3Volume", "gamma", "brightness" };
const int gc_settings_key_count = (int)( sizeof( gc_settings_keys ) / sizeof( gc_settings_keys[0] ));

static int KeyAllowed( const char *key, size_t len )
{
	int i;

	for( i = 0; i < gc_settings_key_count; i++ )
		if( strlen( gc_settings_keys[i] ) == len && !memcmp( gc_settings_keys[i], key, len ))
			return 1;
	return 0;
}

static int Printable( const unsigned char *s, size_t len )
{
	size_t i;

	for( i = 0; i < len; i++ )
		if( s[i] < 32 || s[i] > 126 )
			return 0;
	return 1;
}

static void Put16( unsigned char *p, unsigned v ) { p[0] = v & 255; p[1] = ( v >> 8 ) & 255; }
static void Put32( unsigned char *p, unsigned v ) { Put16( p, v & 0xFFFF ); Put16( p + 2, v >> 16 ); }
static unsigned Get16( const unsigned char *p ) { return (unsigned)p[0] | ( (unsigned)p[1] << 8 ); }
static unsigned Get32( const unsigned char *p ) { return Get16( p ) | ( Get16( p + 2 ) << 16 ); }

unsigned int GC_SettingsCrc32( const unsigned char *data, size_t len )
{
	unsigned int crc = 0xFFFFFFFFu;
	size_t i;
	int b;

	for( i = 0; i < len; i++ )
	{
		crc ^= data[i];
		for( b = 0; b < 8; b++ )
			crc = ( crc >> 1 ) ^ ( 0xEDB88320u & (unsigned)-(int)( crc & 1 ));
	}
	return ~crc;
}

int GC_SettingsPack( unsigned char *buf, size_t cap, const gc_setting_t *items, int n )
{
	size_t pos = 12;
	int i;

	if( cap < 12 || n < 0 || n > 255 )
		return 0;
	for( i = 0; i < n; i++ )
	{
		const size_t kl = strlen( items[i].key ), vl = strlen( items[i].value );

		if( !KeyAllowed( items[i].key, kl ) || vl > GC_SETTINGS_MAX_VAL || !Printable( (const unsigned char *)items[i].value, vl ))
			return 0;
		if( pos + 2 + kl + vl > cap )
			return 0;
		buf[pos++] = (unsigned char)kl;
		buf[pos++] = (unsigned char)vl;
		memcpy( buf + pos, items[i].key, kl );
		pos += kl;
		memcpy( buf + pos, items[i].value, vl );
		pos += vl;
	}
	Put32( buf, GC_SETTINGS_MAGIC );
	Put16( buf + 4, GC_SETTINGS_VERSION );
	Put16( buf + 6, (unsigned)n );
	Put32( buf + 8, GC_SettingsCrc32( buf + 12, pos - 12 ));
	return (int)pos;
}

int GC_SettingsUnpack( const unsigned char *buf, size_t len, gc_setting_t *out, int max_out )
{
	size_t pos = 12;
	unsigned count, i;
	int got = 0;

	if( len < 12 || len > GC_SETTINGS_MAX_BLOB || Get32( buf ) != GC_SETTINGS_MAGIC || Get16( buf + 4 ) != GC_SETTINGS_VERSION )
		return -1;
	count = Get16( buf + 6 );
	if( count > 255 )
		return -1;
	/* the CRC covers the entries; the caller may pass a sector-padded buffer, so the payload end is found by parsing */
	for( i = 0; i < count; i++ )
	{
		size_t kl, vl;
		int slot;

		if( pos + 2 > len )
			return -1;
		kl = buf[pos];
		vl = buf[pos + 1];
		pos += 2;
		if( pos + kl + vl > len || vl > GC_SETTINGS_MAX_VAL || !KeyAllowed( (const char *)buf + pos, kl ))
			return -1;
		if( !Printable( buf + pos + kl, vl ))
			return -1;
		slot = got;
		if( slot < max_out )
		{
			int k;

			for( k = 0; k < gc_settings_key_count; k++ )
				if( strlen( gc_settings_keys[k] ) == kl && !memcmp( gc_settings_keys[k], buf + pos, kl ))
					out[slot].key = gc_settings_keys[k];
			memcpy( out[slot].value, buf + pos + kl, vl );
			out[slot].value[vl] = 0;
			got++;
		}
		pos += kl + vl;
	}
	if( Get32( buf + 8 ) != GC_SettingsCrc32( buf + 12, pos - 12 ))
		return -1;
	return got;
}
