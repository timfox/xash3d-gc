/*
sv_gc_campaign.c - GameCube auto-campaign driver for recorded playthroughs
Copyright (C) 2026 xash3d-gc contributors

-gcautocampaign (gamecube.cfg "autocampaign [seconds]"): after a map has run
for the given time (default 20 s), move the player into the
trigger_changelevel that leads to the first map not visited yet, so the
game's own trigger fires the level change (landmark, carried entities and
all).  If nothing has changed level a few seconds later (use-only or locked
triggers), queue that trigger's changelevel directly and log it as FORCED.
Every map arrival is logged with the session time, so a recording of the run
can be cut and labelled by map.
*/

#include "common.h"
#include "server.h"

#if XASH_GAMECUBE

qboolean SV_GCEntityInSolid( edict_t *ent );

#define GC_CAMPAIGN_MAX_TRIGGERS	16
#define GC_CAMPAIGN_MAX_VISITED		160
#define GC_CAMPAIGN_FORCE_AFTER		6.0
#define GC_CAMPAIGN_SCRIPTED_WAIT	180.0

typedef struct
{
	char map[32];
	char landmark[32];
	char model[16];
} gc_campaign_trigger_t;

static gc_campaign_trigger_t gc_camp_triggers[GC_CAMPAIGN_MAX_TRIGGERS];
static int gc_camp_num_triggers;
static char gc_camp_visited[GC_CAMPAIGN_MAX_VISITED][32];
static int gc_camp_num_visited;
static char gc_camp_prev[32];
static char gc_camp_current[32];
static double gc_camp_map_start;
static double gc_camp_session_start;
static qboolean gc_camp_moved, gc_camp_forced;
static int gc_camp_target = -1;
static int gc_camp_enabled = -1;

static qboolean SV_GCCampaignEnabled( void )
{
	if( gc_camp_enabled < 0 )
		gc_camp_enabled = Sys_CheckParm( "-gcautocampaign" ) ? 1 : 0;
	return gc_camp_enabled > 0;
}

qboolean SV_GCAutoCampaignActive( void )
{
	return SV_GCCampaignEnabled();
}

static qboolean SV_GCCampaignVisited( const char *map )
{
	int i;

	for( i = 0; i < gc_camp_num_visited; i++ )
	{
		if( !Q_stricmp( gc_camp_visited[i], map ))
			return true;
	}
	return false;
}

/* Called from SV_LoadFromFile before the entities spawn: remember every
 * trigger_changelevel (target map, landmark, brush model). */
void SV_GCCampaignScanEntities( const char *mapname, const char *entities )
{
	char token[256], key[64];
	gc_campaign_trigger_t cur;
	qboolean is_trigger = false;
	const char *p = entities;

	if( !SV_GCCampaignEnabled() || !entities )
		return;

	gc_camp_num_triggers = 0;
	memset( &cur, 0, sizeof( cur ));
	while(( p = COM_ParseFile( (char *)p, token, sizeof( token ))) != NULL )
	{
		if( token[0] == '{' )
		{
			memset( &cur, 0, sizeof( cur ));
			is_trigger = false;
			continue;
		}
		if( token[0] == '}' )
		{
			if( is_trigger && cur.map[0] && gc_camp_num_triggers < GC_CAMPAIGN_MAX_TRIGGERS )
				gc_camp_triggers[gc_camp_num_triggers++] = cur;
			continue;
		}
		Q_strncpy( key, token, sizeof( key ));
		if(( p = COM_ParseFile( (char *)p, token, sizeof( token ))) == NULL )
			break;
		if( !Q_stricmp( key, "classname" ))
			is_trigger = !Q_stricmp( token, "trigger_changelevel" );
		else if( !Q_stricmp( key, "map" ))
			Q_strncpy( cur.map, token, sizeof( cur.map ));
		else if( !Q_stricmp( key, "landmark" ))
			Q_strncpy( cur.landmark, token, sizeof( cur.landmark ));
		else if( !Q_stricmp( key, "model" ))
			Q_strncpy( cur.model, token, sizeof( cur.model ));
	}

	/* Arrival bookkeeping. */
	if( Q_stricmp( gc_camp_current, mapname ))
	{
		Q_strncpy( gc_camp_prev, gc_camp_current, sizeof( gc_camp_prev ));
		Q_strncpy( gc_camp_current, mapname, sizeof( gc_camp_current ));
	}
	if( !SV_GCCampaignVisited( mapname ) && gc_camp_num_visited < GC_CAMPAIGN_MAX_VISITED )
		Q_strncpy( gc_camp_visited[gc_camp_num_visited++], mapname, sizeof( gc_camp_visited[0] ));
	if( gc_camp_session_start <= 0.0 )
		gc_camp_session_start = Platform_DoubleTime();
	gc_camp_map_start = -1.0; /* armed at the first server frame of the map */
	gc_camp_moved = gc_camp_forced = false;
	gc_camp_target = -1;
	Con_Printf( "Xash3D GameCube: CAMPAIGN map %s at t=%.1fs (from %s) exits=%d\n",
		mapname, Platform_DoubleTime() - gc_camp_session_start,
		gc_camp_prev[0] ? gc_camp_prev : "start", gc_camp_num_triggers );
}

static int SV_GCCampaignPickExit( void )
{
	int i;

	/* First a map never visited, then the way back (hub maps), then any. */
	for( i = 0; i < gc_camp_num_triggers; i++ )
	{
		if( Q_stricmp( gc_camp_triggers[i].map, sv.name ) && !SV_GCCampaignVisited( gc_camp_triggers[i].map ))
			return i;
	}
	for( i = 0; i < gc_camp_num_triggers; i++ )
	{
		if( Q_stricmp( gc_camp_triggers[i].map, gc_camp_prev ))
			continue;
		return i;
	}
	return gc_camp_num_triggers > 0 ? 0 : -1;
}

static edict_t *SV_GCCampaignFindTrigger( const gc_campaign_trigger_t *t )
{
	int i;

	if( !t->model[0] )
		return NULL;
	for( i = svs.maxclients + 1; i < svgame.numEntities; i++ )
	{
		edict_t *e = SV_EdictNum( i );

		if( !SV_IsValidEdict( e ))
			continue;
		if( Q_stricmp( SV_GetString( e->v.classname ), "trigger_changelevel" ))
			continue;
		if( !Q_stricmp( SV_GetString( e->v.model ), t->model ))
			return e;
	}
	return NULL;
}

/* Visible label in the recording: what happens next is the autopilot. */
static void SV_GCCampaignCenterPrint( const char *text )
{
	sv_client_t *cl = svs.clients;

	if( !cl || cl->state != cs_spawned || FBitSet( cl->flags, FCL_FAKECLIENT ))
		return;
	MSG_BeginServerCmd( &cl->netchan.message, svc_centerprint );
	MSG_WriteString( &cl->netchan.message, text );
}

/* Probe-only console script (gamecube.cfg "probecmd ..." → -gcprobecmd):
 * ';'-separated console commands run on the first map the local client
 * spawns in; a token "@<seconds>" holds the rest until the map has run that
 * long.  Used to script HUD evidence runs (give items, take damage, open the
 * weapon menu) without a pad. */
static void SV_GCProbeCmdFrame( void )
{
	static char script[512];
	static int pos = -1;
	static double start;
	char cmd[128];

	if( pos == -2 )
		return;
	if( pos == -1 )
	{
		if( !Sys_GetParmFromCmdLine( "-gcprobecmd", script ))
		{
			pos = -2;
			return;
		}
		if( sv.state != ss_active || !svs.clients || svs.clients[0].state != cs_spawned )
			return;
		pos = 0;
		start = sv.time;
		Con_Printf( "Xash3D GameCube: probecmd start map=%s time=%.1f\n", sv.name, sv.time );
	}
	if( sv.state != ss_active )
		return;
	while( script[pos] )
	{
		int n = 0, tok;

		while( script[pos] == ';' || script[pos] == ' ' )
			pos++;
		tok = pos;
		while( script[pos] && script[pos] != ';' && n < (int)sizeof( cmd ) - 1 )
			cmd[n++] = script[pos++];
		while( n > 0 && cmd[n - 1] == ' ' )
			n--;
		cmd[n] = '\0';
		if( !n )
			continue;
		if( cmd[0] == '@' )
		{
			double at = Q_atof( cmd + 1 );

			if( sv.time - start < at )
			{
				pos = tok; /* re-read this marker next frame */
				return;
			}
			Con_Printf( "Xash3D GameCube: probecmd mark %s map=%s t=%.1f\n", cmd, sv.name, sv.time - start );
			continue;
		}
		Con_Printf( "Xash3D GameCube: probecmd exec %s\n", cmd );
		Cbuf_AddText( cmd );
		Cbuf_AddText( "\n" );
	}
	pos = -2;
	Con_Printf( "Xash3D GameCube: probecmd done\n" );
}

/* Live-map bridge (tools/mapbridge in dod-gc). If the disc carries a fixed-size
 * slot file "livemap.txt" ("<serial> <mapname>"), poll it about once a second and
 * run "map <mapname>" when the serial changes. The host rewrites the slot (and
 * the dod_live.bsp slot before it) inside the ISO in place; Dolphin serves the
 * image from the host file, so the next DVD read sees the new bytes.
 * Off by default: set cvar gc_livemap 1 (e.g. in the disc autoexec.cfg). */
static void SV_GCLiveMapFrame( void )
{
	static int last_serial = -1, frames;
	static double next_poll;
	char name[64];
	int serial, n;
	fs_offset_t len = 0;
	byte *buf;

	static convar_t *gc_livemap;

	if( !gc_livemap )
		gc_livemap = Cvar_Get( "gc_livemap", "0", FCVAR_SERVER, "poll livemap.txt on the disc and reload dod_live (mapbridge dev tool)" );
	if( !gc_livemap->value || host.realtime < next_poll )
		return;
	next_poll = host.realtime + ( last_serial < 0 ? 2.0 : 1.0 );
	buf = FS_LoadFile( "livemap.txt", &len, false );
	if( !buf )
		return;
	name[0] = '\0';
	n = sscanf(( const char * )buf, "%d %63s", &serial, name );
	Mem_Free( buf );
	if( n != 2 || strchr( name, '/' ) || strchr( name, '\\' ))
		return;
	if( last_serial < 0 ) /* first poll: adopt the current serial, do not reload */
	{
		last_serial = serial;
		Con_Printf( "Xash3D GameCube: livemap armed serial=%d map=%s\n", serial, name );
		return;
	}
	if( serial == last_serial )
		return;
	last_serial = serial;
	(void)frames;
	Con_Printf( "Xash3D GameCube: livemap reload serial=%d map=%s\n", serial, name );
	Cbuf_AddText( va( "map %s\n", name ));
}

/* Once per server frame while a map is running. */
void SV_GCCampaignFrame( void )
{
	char delay_arg[16];
	double delay = 20.0, elapsed;
	gc_campaign_trigger_t *t;
	edict_t *player, *trig;

	SV_GCProbeCmdFrame();
	SV_GCLiveMapFrame();

	{
		static unsigned calls;

		if( SV_GCCampaignEnabled() && ( ++calls % 600 ) == 1 )
			Con_Printf( "Xash3D GameCube: CAMPAIGN tick n=%u map=%s cur=%s state=%d time=%.1f start=%.1f\n",
				calls, sv.name, gc_camp_current, sv.state, sv.time, gc_camp_map_start );
	}
	if( !SV_GCCampaignEnabled() || sv.state != ss_active || svs.maxclients < 1 )
		return;
	if( Q_stricmp( gc_camp_current, sv.name ))
		return; /* map not scanned (loaded some other way) */
	if( gc_camp_map_start < 0.0 )
		gc_camp_map_start = sv.time;
	/* Never pull the player off a ride the game is giving them (the c0a0
	 * tram): the level change is the train's to make. Restart the clock
	 * while they stand on a train. */
	{
		edict_t *pl = SV_EdictNum( 1 );
		edict_t *ground = ( SV_IsValidEdict( pl ) && SV_IsValidEdict( pl->v.groundentity ))
			? pl->v.groundentity : NULL;

		if( ground && ( !Q_stricmp( SV_ClassName( ground ), "func_tracktrain" )
			|| !Q_stricmp( SV_ClassName( ground ), "func_train" )))
		{
			gc_camp_map_start = sv.time;
			return;
		}
	}
	if( Sys_GetParmFromCmdLine( "-gcautocampaign", delay_arg ) && atof( delay_arg ) > 1.0 )
		delay = atof( delay_arg );
	elapsed = sv.time - gc_camp_map_start;
	if( elapsed < delay )
		return;

	if( gc_camp_target < 0 )
	{
		gc_camp_target = SV_GCCampaignPickExit();
		if( gc_camp_target < 0 )
		{
			if( !gc_camp_forced )
			{
				gc_camp_forced = true;
				Con_Printf( "Xash3D GameCube: CAMPAIGN end: %s has no trigger_changelevel\n", sv.name );
			}
			return;
		}
	}
	t = &gc_camp_triggers[gc_camp_target];
	player = SV_EdictNum( 1 );

	/* A named trigger_changelevel is fired by the map itself (the c0a0 tram
	 * path_tracks Use "transition"); its brush is a small marker the player
	 * never stands in. Let the scripted ride finish, as in retail: walking
	 * into the marker leaves the player inside the next map's matching
	 * marker back, which bounces straight back. */
	trig = SV_GCCampaignFindTrigger( t );
	if( trig && trig->v.targetname && elapsed < delay + GC_CAMPAIGN_SCRIPTED_WAIT )
		return;

	if( !gc_camp_moved )
	{
		gc_camp_moved = true;
		if( trig && SV_IsValidEdict( player ))
		{
			vec3_t entry;
			int k;

			/* Step the player just across the trigger face nearest to
			 * where they stand, as walking into it would. Dropping them at
			 * the trigger centre put them deep in the next map's matching
			 * trigger back (c0a0b -> c0a0c bounced straight back), which a
			 * real crossing never does. */
			/* A doorway trigger is thin along the walking direction.  Cross
			 * that axis from the player's side, and stay inside the doorway's
			 * width on the other axis: entering from the side (c1a4 *56 at
			 * x=-214) put the hull in the corridor wall of c1a4k. */
			{
				const int thin = ( trig->v.absmax[0] - trig->v.absmin[0] )
					<= ( trig->v.absmax[1] - trig->v.absmin[1] ) ? 0 : 1;

				for( k = 0; k < 2; k++ )
				{
					float lo, hi;

					if( k == thin )
					{
						lo = trig->v.absmin[k] - player->v.maxs[k] + 2.0f;
						hi = trig->v.absmax[k] - player->v.mins[k] - 2.0f;
					}
					else
					{
						lo = trig->v.absmin[k] - player->v.mins[k] + 1.0f;
						hi = trig->v.absmax[k] - player->v.maxs[k] - 1.0f;
					}
					entry[k] = player->v.origin[k];
					if( lo <= hi )
						entry[k] = bound( lo, entry[k], hi );
					else
						entry[k] = ( trig->v.absmin[k] + trig->v.absmax[k] ) * 0.5f;
				}
			}
			/* Height: feet on the doorway floor, as a player walking through
			 * it would be.  Clamping the current height let the hull
			 * overlap the trigger by 2 units from above or below (c2a3 from
			 * z=1556 entered *56 at the lintel; c1a1 entered *4 with its feet
			 * 70 units under the floor), and the landmark offset carried that
			 * into the next map as a stuck arrival. */
			/* Some triggers start below the floor (c1a1a *15 is 250 units
			 * tall from under the c1a1atof doorway), so search upward from
			 * the trigger's bottom for the first height the hull fits:
			 * that is the doorway floor.  Off-centre spots that never fit
			 * (inside a wall) retry on the trigger's centre line. */
			{
				const float top = trig->v.absmax[2] - player->v.mins[2] - 2.0f;
				int pass;

				for( pass = 0; pass < 2; pass++ )
				{
					if( pass == 1 )
					{
						entry[0] = ( trig->v.absmin[0] + trig->v.absmax[0] ) * 0.5f;
						entry[1] = ( trig->v.absmin[1] + trig->v.absmax[1] ) * 0.5f;
					}
					for( entry[2] = trig->v.absmin[2] - player->v.mins[2] + 1.0f;
						entry[2] <= top; entry[2] += 4.0f )
					{
						VectorCopy( entry, player->v.origin );
						if( !SV_GCEntityInSolid( player ))
							break;
					}
					if( entry[2] <= top )
						break;
				}
				if( pass == 2 ) // nothing fits: fall back to the trigger floor
				{
					entry[2] = trig->v.absmin[2] - player->v.mins[2] + 1.0f;
					VectorCopy( entry, player->v.origin );
				}
			}
			VectorClear( player->v.velocity );
			SV_LinkEdict( player, true );
			SV_GCCampaignCenterPrint( va( "AUTOPILOT: step into exit -> %s", t->map ));
			Con_Printf( "Xash3D GameCube: CAMPAIGN walk-in %s -> %s via %s (landmark %s) at (%.0f,%.0f,%.0f) stuck=%d\n",
				sv.name, t->map, t->model, t->landmark[0] ? t->landmark : "-",
				entry[0], entry[1], entry[2], SV_GCEntityInSolid( player ) ? 1 : 0 );
		}
		else
			Con_Printf( "Xash3D GameCube: CAMPAIGN no trigger edict for %s -> %s\n", sv.name, t->map );
		return;
	}
	/* Keep walking toward the trigger centre at running speed until its own
	 * touch (and the game's transition-volume check) fires, as it would for
	 * a player crossing it. */
	if( !gc_camp_forced && SV_IsValidEdict( player )
		&& ( trig = SV_GCCampaignFindTrigger( t )) != NULL )
	{
		vec3_t center, step;
		float dist;

		VectorAverage( trig->v.absmin, trig->v.absmax, center );
		center[2] = player->v.origin[2]; // stay on the floor while walking in
		VectorSubtract( center, player->v.origin, step );
		dist = VectorLength( step );
		if( dist > 1.0f )
		{
			VectorScale( step, Q_min( dist, 320.0f * (float)sv.frametime ) / dist, step );
			VectorAdd( player->v.origin, step, player->v.origin );
			VectorClear( player->v.velocity );
			SV_LinkEdict( player, true );
		}
	}
	if( !gc_camp_forced && elapsed >= delay + GC_CAMPAIGN_FORCE_AFTER
		+ (( trig && trig->v.targetname ) ? GC_CAMPAIGN_SCRIPTED_WAIT : 0.0 ))
	{
		gc_camp_forced = true;
		SV_GCCampaignCenterPrint( va( "AUTOPILOT: forced level change -> %s", t->map ));
		Con_Printf( "Xash3D GameCube: CAMPAIGN FORCED changelevel %s -> %s (landmark %s)\n",
			sv.name, t->map, t->landmark[0] ? t->landmark : "-" );
		SV_QueueChangeLevel( t->map, t->landmark[0] ? t->landmark : NULL );
	}
}

#endif /* XASH_GAMECUBE */
