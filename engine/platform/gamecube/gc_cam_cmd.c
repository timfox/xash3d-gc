/*
gc_cam_cmd.c - `gc_cam <file> <clip>` / `gc_cam stop`: play a GxBake .gcam
camera clip (`gxbake camera`) over the player's view.

The file is loaded whole (a few hundred bytes to a few KiB) and kept until the
clip ends or is stopped; gc_cam.c reads it in place. The clip's clock is
cl.time. The engine keeps its own near/far planes and the PVS stays the
player's, so a clip that flies far from the player can show culled world.
*/
#include "common.h"
#include "client.h"
#include "gc_cam.h"
#include "gc_cam_cmd.h"

static gc_cam_player_t gc_cam_pl;
static byte *gc_cam_blob;
static fs_offset_t gc_cam_len;
static uint gc_cam_frames;

static void GC_CamFree( void )
{
	gc_cam_stop( &gc_cam_pl );
	if( gc_cam_blob )
		Mem_Free( gc_cam_blob );
	gc_cam_blob = NULL;
	gc_cam_len = 0;
}

static byte *GC_CamLoad( const char *name, fs_offset_t *len, char *path, size_t size )
{
	static const char *const prefixes[] = { "", "media/", "valve/media/" };
	size_t i;

	for( i = 0; i < ARRAYSIZE( prefixes ); i++ )
	{
		byte *blob;

		Q_snprintf( path, size, "%s%s", prefixes[i], name );
		COM_DefaultExtension( path, ".gcam", size );
		blob = FS_LoadFile( path, len, false );
		if( blob )
			return blob;
	}
	return NULL;
}

/* Takes blob (frees it on failure). */
static qboolean GC_CamStart( const char *path, byte *blob, fs_offset_t len, const char *clip )
{
	GC_CamFree();
	if( gc_cam_play( &gc_cam_pl, blob, (size_t)len, clip, cl.time ) != 0 )
	{
		Con_Printf( S_ERROR "gc_cam: %s has no clip \"%s\" (gc_cam <file> lists them)\n", path, clip );
		Mem_Free( blob );
		return false;
	}
	gc_cam_blob = blob;
	gc_cam_len = len;
	gc_cam_frames = 0;
	Con_Printf( "gc_cam: playing %s:%s (%.2fs%s) at cl.time %.2f\n", path, gc_cam_pl.clip.name,
		(double)gc_cam_pl.clip.duration, gc_cam_pl.clip.loop ? ", loop" : "", cl.time );
	return true;
}

static void GC_Cam_f( void )
{
	char path[MAX_SYSPATH];
	byte *blob;
	fs_offset_t len = 0;
	int count, i;

	if( Cmd_Argc() == 2 && !Q_stricmp( Cmd_Argv( 1 ), "stop" ))
	{
		if( gc_cam_pl.active )
			Con_Printf( "gc_cam: stopped %s after %u frames\n", gc_cam_pl.clip.name, gc_cam_frames );
		GC_CamFree();
		return;
	}
	if( Cmd_Argc() != 2 && Cmd_Argc() != 3 )
	{
		Con_Printf( S_USAGE "gc_cam <file[.gcam]> <clip> | gc_cam <file> (list clips) | gc_cam stop\n" );
		return;
	}

	blob = GC_CamLoad( Cmd_Argv( 1 ), &len, path, sizeof( path ));
	if( !blob )
	{
		Con_Printf( S_ERROR "gc_cam: can't load %s\n", Cmd_Argv( 1 ));
		return;
	}
	count = gc_cam_count( blob, (size_t)len );
	if( count < 0 )
	{
		Con_Printf( S_ERROR "gc_cam: %s is not a .gcam\n", path );
		Mem_Free( blob );
		return;
	}
	if( Cmd_Argc() == 2 )
	{
		Con_Printf( "gc_cam: %s has %d clip(s)\n", path, count );
		for( i = 0; i < count; i++ )
		{
			gc_cam_clip_t clip;

			if( !gc_cam_clip( blob, (size_t)len, i, &clip ))
				Con_Printf( "  %s  %.2fs%s\n", clip.name, (double)clip.duration, clip.loop ? " loop" : "" );
		}
		Mem_Free( blob );
		return;
	}

	GC_CamStart( path, blob, len, Cmd_Argv( 2 ));
}

static void GC_CamPreload( void );

void GC_CamInit( void )
{
	Cmd_AddCommand( "gc_cam", GC_Cam_f, "play a GxBake .gcam camera clip over the view: gc_cam <file> <clip> | stop" );
	GC_CamPreload();
}

/* -gccam <file>:<clip> (gamecube.cfg `gccam`). The file is read at init,
 * while the heap has room (c0a0 New Game leaves under 4 KiB, too little for
 * even the file handle), and the clip starts at the first rendered view so
 * its clock starts with the map. */
static byte *gc_cam_pending;
static fs_offset_t gc_cam_pending_len;
static char gc_cam_pending_path[MAX_SYSPATH];
static char gc_cam_pending_clip[GC_CAM_NAME];

static void GC_CamPreload( void )
{
	char spec[64], *colon;

	if( !Sys_GetParmFromCmdLine( "-gccam", spec ))
		return;
	colon = Q_strchr( spec, ':' );
	if( !colon )
		return;
	*colon = '\0';
	Q_strncpy( gc_cam_pending_clip, colon + 1, sizeof( gc_cam_pending_clip ));
	gc_cam_pending = GC_CamLoad( spec, &gc_cam_pending_len, gc_cam_pending_path, sizeof( gc_cam_pending_path ));
	if( !gc_cam_pending || gc_cam_count( gc_cam_pending, (size_t)gc_cam_pending_len ) < 0 )
	{
		Con_Printf( S_ERROR "-gccam: can't load a .gcam from %s\n", spec );
		if( gc_cam_pending )
			Mem_Free( gc_cam_pending );
		gc_cam_pending = NULL;
		return;
	}
	Con_Reportf( "Xash3D GameCube: -gccam preloaded %s (%d bytes), clip %s at first view\n",
		gc_cam_pending_path, (int)gc_cam_pending_len, gc_cam_pending_clip );
}

static void GC_CamAutoStart( void )
{
	byte *blob = gc_cam_pending;

	if( !blob )
		return;
	gc_cam_pending = NULL;
	GC_CamStart( gc_cam_pending_path, blob, gc_cam_pending_len, gc_cam_pending_clip );
}

qboolean GC_CamApplyView( float *vieworg, float *viewangles, float *fov_x )
{
	gc_cam_view_t v;

	GC_CamAutoStart();
	if( !gc_cam_pl.active )
		return false;
	if( !gc_cam_update( &gc_cam_pl, cl.time, 4.0f / 3.0f, &v ))
	{
		Con_Printf( "gc_cam: %s ended after %u frames\n", gc_cam_pl.clip.name, gc_cam_frames );
		GC_CamFree();
		return false;
	}
	VectorCopy( v.vieworg, vieworg );
	VectorCopy( v.viewangles, viewangles );
	if( fov_x )
		*fov_x = v.fov_x;
	if( gc_cam_frames == 0 || !( gc_cam_frames % 60 ))
		Con_Reportf( "Xash3D GameCube: gc_cam %s t=%.2f org=(%.0f,%.0f,%.0f) ang=(%.1f,%.1f,%.1f) fov=%.1f\n",
			gc_cam_pl.clip.name, (double)v.t, (double)v.vieworg[0], (double)v.vieworg[1], (double)v.vieworg[2],
			(double)v.viewangles[0], (double)v.viewangles[1], (double)v.viewangles[2], (double)v.fov_x );
	gc_cam_frames++;
	return true;
}
