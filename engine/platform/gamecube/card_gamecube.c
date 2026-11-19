/*
card_gamecube.c - save the player's name and settings to a memory card, and read them back

Rules (the player's saves are not ours to risk):
  * Saving and loading NEVER format a card. A card that is blank, from another region or broken answers
    GC_CARD_NEEDS_FORMAT; only gc_card_format, which the menu runs after the player has answered "Format" in a
    dialog that says what it erases, formats it, and only straight after a NEEDS_FORMAT answer.
  * Nothing is written without the player asking (the Options pages call gc_card_save when they close).
  * One file, "DayOfDefeat_Settings", one sector. Another file on the card is never touched.
Commands: gc_card_save, gc_card_load, gc_card_format confirmed. Cvar gc_card_status shows the last result.
*/
#include "common.h"
#include "client.h"

#if XASH_GAMECUBE
#include <gccore.h>
#include <ogc/card.h>
#include <malloc.h>
#include <string.h>
#include "settings_blob.h"

#define GC_CARD_FILE "DayOfDefeat_Settings"

typedef enum
{
	GC_CARD_OK = 0,
	GC_CARD_NO_CARD,
	GC_CARD_NEEDS_FORMAT,
	GC_CARD_NO_SPACE,
	GC_CARD_NO_SAVE,
	GC_CARD_BAD_SAVE,
	GC_CARD_ERROR
} gc_card_status_t;

static const char *const gc_card_text[] =
{
	"saved", "no memory card in slot A", "the card must be formatted first", "not enough space on the card",
	"no saved settings on the card", "the saved settings are damaged", "card error"
};

static CVAR_DEFINE_AUTO( gc_card_status, "", FCVAR_READ_ONLY, "memory card: result of the last save / load" );
static qboolean gc_card_format_allowed; // set only by a NEEDS_FORMAT answer, consumed by gc_card_format
static unsigned char *gc_card_work;      // 40 KiB work area, allocated while mounted only

static void GC_CardResult( gc_card_status_t st )
{
	Cvar_FullSet( gc_card_status.name, gc_card_text[st], FCVAR_READ_ONLY );
	Con_Reportf( "Xash3D GameCube: memory card: %s\n", gc_card_text[st] );
}

static gc_card_status_t GC_CardMount( void )
{
	s32 mem = 0, sect = 0, ret;

	ret = CARD_ProbeEx( CARD_SLOTA, &mem, &sect );
	if( ret == CARD_ERROR_NOCARD || ret == CARD_ERROR_WRONGDEVICE )
		return GC_CARD_NO_CARD;
	if( ret < 0 && ret != CARD_ERROR_BROKEN && ret != CARD_ERROR_ENCODING )
		return GC_CARD_ERROR;
	gc_card_work = memalign( 32, CARD_WORKAREA_SIZE );
	if( !gc_card_work )
		return GC_CARD_ERROR;
	ret = CARD_Mount( CARD_SLOTA, gc_card_work, NULL );
	if( ret == CARD_ERROR_READY || ret == CARD_ERROR_UNLOCKED )
		return GC_CARD_OK;
	free( gc_card_work );
	gc_card_work = NULL;
	if( ret == CARD_ERROR_BROKEN || ret == CARD_ERROR_ENCODING )
		return GC_CARD_NEEDS_FORMAT;
	return ret == CARD_ERROR_NOCARD ? GC_CARD_NO_CARD : GC_CARD_ERROR;
}

static void GC_CardUnmount( void )
{
	CARD_Unmount( CARD_SLOTA );
	free( gc_card_work );
	gc_card_work = NULL;
}

static gc_card_status_t GC_CardSave( void )
{
	gc_setting_t items[16];
	unsigned char blob[GC_SETTINGS_MAX_BLOB];
	unsigned char *sector = NULL;
	card_file file;
	u32 sect_size = 0;
	int i, n, len;
	gc_card_status_t st = GC_CardMount();
	s32 ret;

	if( st != GC_CARD_OK )
		return st;
	for( i = 0, n = 0; i < gc_settings_key_count && n < 16; i++ )
	{
		items[n].key = gc_settings_keys[i];
		Q_strncpy( items[n].value, Cvar_VariableString( gc_settings_keys[i] ), sizeof( items[n].value ));
		if( items[n].value[0] )
			n++;
	}
	len = GC_SettingsPack( blob, sizeof( blob ), items, n );
	if( !len || CARD_GetSectorSize( CARD_SLOTA, &sect_size ) < 0 || sect_size < GC_SETTINGS_MAX_BLOB )
	{
		GC_CardUnmount();
		return GC_CARD_ERROR;
	}
	sector = memalign( 32, sect_size );
	if( !sector )
	{
		GC_CardUnmount();
		return GC_CARD_ERROR;
	}
	memset( sector, 0, sect_size );
	memcpy( sector, blob, (size_t)len );
	ret = CARD_Open( CARD_SLOTA, GC_CARD_FILE, &file );
	if( ret == CARD_ERROR_NOFILE )
		ret = CARD_Create( CARD_SLOTA, GC_CARD_FILE, sect_size, &file );
	if( ret == CARD_ERROR_READY )
	{
		ret = CARD_Write( &file, sector, sect_size, 0 );
		CARD_Close( &file );
	}
	free( sector );
	GC_CardUnmount();
	if( ret == CARD_ERROR_READY )
		return GC_CARD_OK;
	if( ret == CARD_ERROR_INSSPACE || ret == CARD_ERROR_NOENT || ret == CARD_ERROR_LIMIT )
		return GC_CARD_NO_SPACE;
	return GC_CARD_ERROR;
}

static gc_card_status_t GC_CardLoad( void )
{
	gc_setting_t items[16];
	unsigned char *sector = NULL;
	card_file file;
	u32 sect_size = 0;
	int i, n;
	gc_card_status_t st = GC_CardMount();
	s32 ret;

	if( st != GC_CARD_OK )
		return st;
	ret = CARD_Open( CARD_SLOTA, GC_CARD_FILE, &file );
	if( ret == CARD_ERROR_NOFILE )
	{
		GC_CardUnmount();
		return GC_CARD_NO_SAVE;
	}
	if( ret != CARD_ERROR_READY || CARD_GetSectorSize( CARD_SLOTA, &sect_size ) < 0 || sect_size < GC_SETTINGS_MAX_BLOB )
	{
		GC_CardUnmount();
		return GC_CARD_ERROR;
	}
	sector = memalign( 32, sect_size );
	if( !sector )
	{
		CARD_Close( &file );
		GC_CardUnmount();
		return GC_CARD_ERROR;
	}
	ret = CARD_Read( &file, sector, sect_size, 0 );
	CARD_Close( &file );
	GC_CardUnmount();
	if( ret != CARD_ERROR_READY )
	{
		free( sector );
		return GC_CARD_ERROR;
	}
	n = GC_SettingsUnpack( sector, GC_SETTINGS_MAX_BLOB, items, 16 );
	free( sector );
	if( n < 0 )
		return GC_CARD_BAD_SAVE;
	for( i = 0; i < n; i++ )
		Cvar_DirectSet( Cvar_FindVar( items[i].key ), items[i].value );
	return GC_CARD_OK;
}

static void GC_CardSave_f( void )
{
	const gc_card_status_t st = GC_CardSave();

	gc_card_format_allowed = st == GC_CARD_NEEDS_FORMAT;
	GC_CardResult( st );
}

static void GC_CardLoad_f( void )
{
	const gc_card_status_t st = GC_CardLoad();

	gc_card_format_allowed = st == GC_CARD_NEEDS_FORMAT;
	GC_CardResult( st );
}

/* Only after the player has said yes to the format dialog: "gc_card_format confirmed" straight after a NEEDS_FORMAT. */
static void GC_CardFormat_f( void )
{
	s32 ret;
	gc_card_status_t st;

	if( Cmd_Argc() < 2 || Q_strcmp( Cmd_Argv( 1 ), "confirmed" ) || !gc_card_format_allowed )
	{
		Con_Printf( "gc_card_format: refused (the card is only formatted after the player confirms)\n" );
		return;
	}
	gc_card_format_allowed = false;
	st = GC_CardMount();
	if( st == GC_CARD_OK )
	{
		GC_CardUnmount(); // it mounts fine now (reinserted?): nothing to format, never touch a healthy card
		GC_CardResult( GC_CARD_OK );
		return;
	}
	if( st != GC_CARD_NEEDS_FORMAT )
	{
		GC_CardResult( st );
		return;
	}
	ret = CARD_Format( CARD_SLOTA );
	st = ret == CARD_ERROR_READY ? GC_CardSave() : GC_CARD_ERROR;
	GC_CardResult( st );
}

void GC_CardInit( void )
{
	static qboolean done;

	if( done )
		return;
	done = true;
	CARD_Init( "DODG", "01" );
	Cvar_RegisterVariable( &gc_card_status );
	Cmd_AddCommand( "gc_card_save", GC_CardSave_f, "save name and settings to the memory card in slot A" );
	Cmd_AddCommand( "gc_card_load", GC_CardLoad_f, "load name and settings from the memory card in slot A" );
	Cmd_AddCommand( "gc_card_format", GC_CardFormat_f, "format the card after the player confirmed (gc_card_format confirmed)" );
}
#else
void GC_CardInit( void ) {}
#endif
