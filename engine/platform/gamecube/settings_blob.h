/*
settings_blob.h - the player's saved name and settings as one small checked blob (pure C, host-testable).

Layout (little endian, so it reads the same in the host test and on the console after the byte swap done here):
  u32 magic "DODS"; u16 version (1); u16 count; u32 crc32 of everything after the header;
  count x { u8 key_len; u8 val_len; key bytes; val bytes }   (no terminators)
Only keys on the whitelist are written or accepted; values are printable ASCII (the player name is sanitized by the
caller). A blob that fails any check is rejected as a whole: a damaged save never half-applies.
*/
#pragma once
#include <stddef.h>

#define GC_SETTINGS_MAGIC   0x53444F44u	/* "DODS" little endian */
#define GC_SETTINGS_VERSION 1
#define GC_SETTINGS_MAX_VAL 255	/* gc_ach carries the 96-byte achievement blob as 192 hex characters */
#define GC_SETTINGS_MAX_BLOB 1024

typedef struct
{
	const char *key;
	char value[GC_SETTINGS_MAX_VAL + 1];
} gc_setting_t;

/* The settings that are saved. Order is the file order. */
extern const char *const gc_settings_keys[];
extern const int gc_settings_key_count;

unsigned int GC_SettingsCrc32( const unsigned char *data, size_t len );
/* Fill buf (cap GC_SETTINGS_MAX_BLOB) from n settings; returns the blob length or 0 if it does not fit / a key is not
 * whitelisted / a value is unprintable or too long. */
int GC_SettingsPack( unsigned char *buf, size_t cap, const gc_setting_t *items, int n );
/* Parse; returns the number of settings (>= 0) filled into out (cap max_out), or -1 for a rejected blob. */
int GC_SettingsUnpack( const unsigned char *buf, size_t len, gc_setting_t *out, int max_out );
