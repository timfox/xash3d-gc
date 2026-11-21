/*
cl_cine.c - cinematic camera: play a keyframed path, lock time, stamp frames

The camera path (cine_path.c) owns the view while a shot plays. Game time
can be locked to the film frame rate so every rendered frame is exactly one
film frame however slowly it renders, which is what makes offline capture
work on an emulator or a slow machine: Dolphin dumps every frame it shows,
the engine stamps a frame number in the top-left corner, and the assembler
keeps one image per stamp.

	cine_play <name> [preroll]	play cine/<name>.cam; preroll seconds hold on key 0 first
	cine_stop			hand the view back
	cine_mark [t] [mode]		print a key for the current view, append it to cine/marks.cam
	cine_info			shot, clock and frame

	cine_fps 30		lock game time to 1/cine_fps per frame while a shot plays (0 = real time)
	cine_hud 0		draw the HUD during a shot
	cine_viewmodel 0	draw the weapon during a shot
	cine_stamp 1		draw the frame stamp during a shot
	cine_capture 0		write a screenshot per frame to cine/<name>/fNNNNN.tga (desktop builds)
	cine_onend ""		console text to run when a shot ends, e.g. "cine_play shot02" or "quit"

Log lines "CINE START", "CINE DONE" are for capture scripts.
*/
#include "common.h"
#include "client.h"
#include "cine_path.h"

static CVAR_DEFINE_AUTO( cine_fps, "30", 0, "lock game time to 1/cine_fps per frame while a cinematic shot plays (0 = real time)" );
static CVAR_DEFINE_AUTO( cine_hud, "0", 0, "draw the HUD during a cinematic shot" );
static CVAR_DEFINE_AUTO( cine_viewmodel, "0", 0, "draw the weapon during a cinematic shot" );
static CVAR_DEFINE_AUTO( cine_stamp, "1", 0, "draw the frame-number stamp during a cinematic shot" );
static CVAR_DEFINE_AUTO( cine_capture, "0", 0, "write a screenshot for every frame of a cinematic shot" );
static CVAR_DEFINE_AUTO( cine_onend, "", 0, "console text to run when a cinematic shot ends" );
/* The shot clock for the game: a listen server's scripted actors and effects
 * follow it instead of server time, which on a slow machine (the GameCube
 * under Dolphin) can run behind the locked film clock. CINE_NO_CLOCK when no
 * shot is loaded. */
#define CINE_NO_CLOCK	-100000.0f
static CVAR_DEFINE_AUTO( cine_clock, "-100000", 0, "seconds into the current cinematic shot (negative in preroll), -100000 when none" );

typedef enum
{
	CINE_IDLE = 0,
	CINE_WAITING,	// shot loaded, world not up yet
	CINE_PREROLL,	// world up, holding key 0
	CINE_PLAYING
} cine_state_t;

static struct
{
	cine_path_t	path;
	char		name[64];
	cine_state_t	state;
	double		clock;	// seconds into the shot (negative during preroll)
	int		frame;	// film frame number of the current host frame
} cine;

static qboolean CL_CineWorldUp( void )
{
	return cls.state == ca_active && cl.video_prepped && !cls.disable_screen;
}

static qboolean CL_CineLoad( const char *name )
{
	char path[MAX_QPATH], err[128];
	fs_offset_t len = 0;
	byte *text;
	qboolean ok;

	Q_snprintf( path, sizeof( path ), "cine/%s.cam", name );
	text = FS_LoadFile( path, &len, false );
	if( !text )
	{
		Con_Printf( S_ERROR "cine: no %s\n", path );
		return false;
	}
	ok = Cine_ParsePath( &cine.path, (const char *)text, err, sizeof( err ));
	Mem_Free( text );
	if( !ok )
	{
		Con_Printf( S_ERROR "cine: %s %s\n", path, err );
		return false;
	}
	return true;
}

static void CL_CineEnd( qboolean finished )
{
	if( cine.state == CINE_IDLE )
		return;
	if( finished )
		Con_Printf( "CINE DONE %s frames=%d\n", cine.name, cine.frame + 1 );
	else
		Con_Printf( "CINE STOP %s frame=%d\n", cine.name, cine.frame );
	cine.state = CINE_IDLE;
	Cvar_DirectSetValue( &cine_clock, CINE_NO_CLOCK );
	if( finished && cine_onend.string[0] )
	{
		Cbuf_AddText( cine_onend.string );
		Cbuf_AddText( "\n" );
	}
}

static void CL_CinePlay_f( void )
{
	const char *name;

	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "cine_play <name> [preroll seconds]\n" );
		return;
	}
	name = Cmd_Argv( 1 );
	if( Q_strchr( name, '/' ) || Q_strchr( name, '\\' ) || Q_strstr( name, ".." ))
	{
		Con_Printf( S_ERROR "cine: shot names are plain names under cine/\n" );
		return;
	}
	CL_CineEnd( false );
	if( !CL_CineLoad( name ))
		return;
	Q_strncpy( cine.name, name, sizeof( cine.name ));
	cine.clock = Cmd_Argc() > 2 ? -fabs( Q_atof( Cmd_Argv( 2 ))) : 0.0;
	cine.frame = -1;
	cine.state = CINE_WAITING;
	Cvar_DirectSetValue( &cine_clock, (float)cine.clock );
	Con_Printf( "cine: %s loaded, %d keys, %.2f s\n", name, cine.path.count, Cine_Duration( &cine.path ));
}

static void CL_CineStop_f( void )
{
	CL_CineEnd( false );
}

static void CL_CineInfo_f( void )
{
	static const char *states[] = { "idle", "waiting for world", "preroll", "playing" };

	Con_Printf( "cine: %s %s clock=%.3f frame=%d fps=%g\n", cine.state ? cine.name : "-",
		states[cine.state], cine.clock, cine.frame, cine_fps.value );
}

static void CL_CineMark_f( void )
{
	char line[160];
	float t = Cmd_Argc() > 1 ? Q_atof( Cmd_Argv( 1 )) : 0.0f;
	int mode = CINE_SMOOTH;
	file_t *f;

	if( Cmd_Argc() > 2 )
	{
		if( !Q_stricmp( Cmd_Argv( 2 ), "linear" )) mode = CINE_LINEAR;
		else if( !Q_stricmp( Cmd_Argv( 2 ), "hold" )) mode = CINE_HOLD;
	}
	if( !CL_CineWorldUp( ))
	{
		Con_Printf( S_ERROR "cine_mark: no map running\n" );
		return;
	}
	Cine_FormatKey( line, sizeof( line ), t, refState.vieworg, refState.viewangles,
		bound( 10.0f, cl.local.scr_fov, 150.0f ), mode );
	Con_Printf( "%s\n", line );

	if( !FS_FileExists( "cine/marks.cam", true ))
	{
		if(( f = FS_Open( "cine/marks.cam", "w", true )) != NULL )
		{
			FS_Printf( f, "cam 1\n# map %s; set each key's time before playing\n", clgame.mapname );
			FS_Close( f );
		}
	}
	if(( f = FS_Open( "cine/marks.cam", "a", true )) != NULL )
	{
		FS_Printf( f, "%s\n", line );
		FS_Close( f );
	}
}

void CL_CineInit( void )
{
	Cvar_RegisterVariable( &cine_fps );
	Cvar_RegisterVariable( &cine_hud );
	Cvar_RegisterVariable( &cine_viewmodel );
	Cvar_RegisterVariable( &cine_stamp );
	Cvar_RegisterVariable( &cine_capture );
	Cvar_RegisterVariable( &cine_onend );
	Cvar_RegisterVariable( &cine_clock );
	Cmd_AddCommand( "cine_play", CL_CinePlay_f, "play a cinematic camera path from cine/<name>.cam" );
	Cmd_AddCommand( "cine_stop", CL_CineStop_f, "stop the cinematic camera" );
	Cmd_AddCommand( "cine_mark", CL_CineMark_f, "print a camera key for the current view and append it to cine/marks.cam" );
	Cmd_AddCommand( "cine_info", CL_CineInfo_f, "cinematic camera state" );
}

/* -gccapture: an offline film capture, where frame rate matters less than
 * what the shot shows. The GameCube route then draws every map face in view
 * (no PVS or small/far face culls, larger face budgets), the whole cast and
 * map props in range, and gives temp entities a bigger pool. */
qboolean CL_GCCaptureMode( void )
{
	static int capture = -1;

	if( capture < 0 )
		capture = Sys_CheckParm( "-gccapture" ) ? 1 : 0;
	return capture != 0;
}

qboolean CL_CineActive( void )
{
	return cine.state == CINE_PREROLL || cine.state == CINE_PLAYING;
}

/*
Called once per host frame with the frame's game time step. Locks the step
to the film rate and advances the shot clock, so the clock and the game
move together.
*/
void CL_CineFrameTime( double *frametime )
{
	if( cine.state == CINE_IDLE )
		return;

	if( cine.state == CINE_WAITING )
	{
		if( !CL_CineWorldUp( ))
			return;
		cine.state = cine.clock < 0.0 ? CINE_PREROLL : CINE_PLAYING;
		Con_Printf( "CINE START %s keys=%d dur=%.3f fps=%g\n", cine.name, cine.path.count,
			Cine_Duration( &cine.path ), cine_fps.value );
	}
	else
	{
		if( cine_fps.value > 0.0f )
			*frametime = 1.0 / cine_fps.value;
		cine.clock += *frametime;
	}

	if( cine.state == CINE_PREROLL && cine.clock >= 0.0 )
	{
		cine.clock = 0.0;
		cine.state = CINE_PLAYING;
	}
	Cvar_DirectSetValue( &cine_clock, (float)cine.clock );
	if( cine.state != CINE_PLAYING )
		return;

	cine.frame++;
	if( cine.clock > Cine_Duration( &cine.path ) + 1e-4 )
	{
		cine.frame--;
		CL_CineEnd( true );
		return;
	}

	if( cine_capture.value != 0.0f && cls.scrshot_action == scrshot_inactive )
	{
		Q_snprintf( cls.shotname, sizeof( cls.shotname ), "cine/%s/f%05d.tga", cine.name, cine.frame );
		cls.scrshot_action = scrshot_normal;
	}
}

// The camera for this frame. False when no shot owns the view.
qboolean CL_CineView( vec3_t org, vec3_t ang, float *fov )
{
	float o[3], a[3], f;

	if( !CL_CineActive( ))
		return false;
	if( !Cine_Sample( &cine.path, cine.clock < 0.0 ? 0.0f : (float)cine.clock, o, a, &f ))
		return false;
	VectorCopy( o, org );
	VectorCopy( a, ang );
	if( fov )
		*fov = f;
	return true;
}

qboolean CL_CineHideHUD( void )
{
	return CL_CineActive() && cine_hud.value == 0.0f;
}

qboolean CL_CineHideViewModel( void )
{
	return CL_CineActive() && cine_viewmodel.value == 0.0f;
}

/*
The frame stamp: CINE_STAMP_BITS cells along the top-left edge of the
picture, each 8x8 in the 640x480 virtual screen. Only playing frames are
stamped; preroll frames carry none, so the assembler skips them.
*/
void CL_CineDraw2D( void )
{
	unsigned int word;
	float cell;
	int i;

	if( cine.state != CINE_PLAYING || cine_stamp.value == 0.0f || cine.frame < 0 )
		return;
	cell = refState.width > 0 ? refState.width / 80.0f : 8.0f;
	word = Cine_StampWord( (unsigned int)cine.frame );
	for( i = 0; i < CINE_STAMP_BITS; i++ )
	{
		byte v = ( word >> i ) & 1u ? 255 : 0;

		ref.dllFuncs.FillRGBA( kRenderNormal, i * cell, 0, cell, cell, v, v, v, 255 );
	}
}
