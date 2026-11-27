/*
sv_vehicle.c - server binding for the vehicle core (common/vehicle.c)

Game agnostic: vehicles are engine-owned edicts with no game class, so the
same car, tank, plane and helicopter run in every GoldSrc game the engine loads. A
driver's usercmds drive the vehicle instead of PM_Move (SV_VehicleRunCmd);
empty vehicles coast and settle in SV_VehicleFrame until they sleep. The
client builds the chase camera itself (client/cl_vehicle.c) from the vehicle
entity it is told about with a "cl_vehicle" stufftext.

	vehicle spawn <car|tank|plane|heli> [model]	spawn one in front of you (single player or sv_cheats)
	vehicle use			get in the nearest vehicle, or out of yours (also +use)
	vehicle enter / exit
	vehicle remove			remove the vehicle you are in or the nearest one
	vehicle info
	vehicle tune <type> [param [value]]	list or set tunables (sv_cheats for set)

	-vehicle <type>			launch option: spawn one and get in when the map starts

Memory: nothing until the first vehicle command or -vehicle; then one block
from the host pool (about 10 KB: slots, tunables, per-client buttons) that is
reused for every map. No per-frame allocation.
*/
#include "common.h"
#include "server.h"
#include "vehicle.h"

#if XASH_VEHICLES

#define SV_VEH_SLOTS	8	// vehicles alive at once
#define SV_VEH_REACH	160.0f	// how near you must be to get in
#define SV_VEH_SLEEP_STEPS	( VEH_STEP_HZ * 2 )	// empty and still this long: stop simulating

static CVAR_DEFINE_AUTO( vehicle_input_scale, "400", FCVAR_SERVER, "move value that counts as full stick for vehicles (cl_forwardspeed)" );
static CVAR_DEFINE_AUTO( vehicle_model_car, "", FCVAR_SERVER, "model for spawned cars (empty = the driver's own model)" );
static CVAR_DEFINE_AUTO( vehicle_model_plane, "", FCVAR_SERVER, "model for spawned planes (empty = the driver's own model)" );
static CVAR_DEFINE_AUTO( vehicle_model_heli, "", FCVAR_SERVER, "model for spawned helicopters (empty = the driver's own model)" );
static CVAR_DEFINE_AUTO( vehicle_model_tank, "", FCVAR_SERVER, "model for spawned tanks (empty = the driver's own model)" );
static CVAR_DEFINE_AUTO( vehicle_tank_magnitude, "150", FCVAR_SERVER, "env_explosion magnitude of a tank shell (0 = effect only)" );

typedef struct
{
	veh_t	sim;
	int	used;
	int	ent;		// edict number
	int	serial;		// edict serial when spawned
	int	driver;		// client slot + 1, 0 = empty
	int	still_steps;	// empty and at rest, counts toward sleep
	// driver state restored on exit
	int	saved_movetype;
	int	saved_solid;
	int	saved_effects;
	string_t	saved_viewmodel;
	string_t	saved_weaponmodel;
} sv_vehicle_t;

typedef struct
{
	int		spawncount;	// svs.spawncount the slots belong to
	int		autospawn_done;
	veh_params_t	params[VEH_TYPE_COUNT];
	sv_vehicle_t	slots[SV_VEH_SLOTS];
	unsigned	*oldbuttons;	// per client, for +use edges
} sv_vehicle_state_t;

static sv_vehicle_state_t *svv;

typedef struct
{
	edict_t	*self;
} sv_veh_trace_ctx_t;

/*
==============================================================================
state
==============================================================================
*/
static sv_vehicle_state_t *SV_VehState( void )
{
	int i;

	if( !svv )
	{
		svv = Mem_Calloc( host.mempool, sizeof( *svv ) + sizeof( unsigned ) * MAX_CLIENTS );
		svv->oldbuttons = (unsigned *)( svv + 1 );
		for( i = VEH_CAR; i < VEH_TYPE_COUNT; i++ )
			Veh_DefaultParams( (veh_type_t)i, &svv->params[i] );
		svv->spawncount = svs.spawncount;
	}

	// a new map: the old edicts are gone, keep only the tunables
	if( svv->spawncount != svs.spawncount )
	{
		memset( svv->slots, 0, sizeof( svv->slots ));
		memset( svv->oldbuttons, 0, sizeof( unsigned ) * MAX_CLIENTS );
		svv->autospawn_done = 0;
		svv->spawncount = svs.spawncount;
	}
	return svv;
}

static edict_t *SV_VehEdict( sv_vehicle_t *v )
{
	edict_t *ent;

	if( !v->used || v->ent <= svs.maxclients || v->ent >= svgame.numEntities )
		return NULL;
	ent = SV_EdictNum( v->ent );
	if( !ent || ent->free || ent->serialnumber != v->serial )
		return NULL;
	return ent;
}

static sv_client_t *SV_VehDriver( sv_vehicle_t *v )
{
	sv_client_t *cl;

	if( !v->driver || v->driver > svs.maxclients )
		return NULL;
	cl = &svs.clients[v->driver - 1];
	if( cl->state != cs_spawned || !cl->edict )
		return NULL;
	return cl;
}

static sv_vehicle_t *SV_VehForClient( sv_client_t *cl )
{
	int slot = (int)( cl - svs.clients ) + 1;
	int i;

	if( !svv )
		return NULL;
	SV_VehState();
	for( i = 0; i < SV_VEH_SLOTS; i++ )
	{
		if( svv->slots[i].used && svv->slots[i].driver == slot )
			return &svv->slots[i];
	}
	return NULL;
}

/*
==============================================================================
world interface
==============================================================================
*/
static void SV_VehTrace( void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3], veh_trace_t *out )
{
	sv_veh_trace_ctx_t *c = (sv_veh_trace_ctx_t *)ctx;
	vec3_t mn, mx;
	trace_t tr;
	int ray = !mins[0] && !mins[1] && !mins[2] && !maxs[0] && !maxs[1] && !maxs[2];

	VectorCopy( mins, mn );
	VectorCopy( maxs, mx );
	// wheels ride the world and brush entities; the hull bumps into everything
	tr = SV_Move( start, mn, mx, end, ray ? MOVE_NOMONSTERS : MOVE_NORMAL, c->self, false );

	out->fraction = tr.fraction;
	VectorCopy( tr.endpos, out->endpos );
	VectorCopy( tr.plane.normal, out->normal );
	out->startsolid = tr.startsolid;
	out->allsolid = tr.allsolid;
}

static void SV_VehWorld( veh_world_t *world, sv_veh_trace_ctx_t *ctx, edict_t *self )
{
	ctx->self = self;
	world->trace = SV_VehTrace;
	world->ctx = ctx;
	world->gravity = sv_gravity.value;
}

/*
==============================================================================
edict sync
==============================================================================
*/
static void SV_VehWriteEdict( sv_vehicle_t *v, edict_t *ent )
{
	VectorCopy( v->sim.origin, ent->v.origin );
	VectorCopy( v->sim.velocity, ent->v.velocity );
	Veh_GetAngles( &v->sim, ent->v.angles );
	ent->v.animtime = sv.time;
	SV_LinkEdict( ent, false );
}

static void SV_VehPlaceDriver( sv_vehicle_t *v, edict_t *driver )
{
	vec3_t seat;

	Veh_LocalToWorld( &v->sim, v->sim.p->c.seat, seat );
	// the seat is the eye: stand the player under it
	VectorSubtract( seat, driver->v.view_ofs, driver->v.origin );
	VectorCopy( v->sim.velocity, driver->v.velocity );
	driver->v.flFallVelocity = 0.0f;
	// link with triggers so changelevels and scripted triggers still fire
	SV_LinkEdict( driver, true );
}

static void SV_VehTellClient( sv_client_t *cl, sv_vehicle_t *v )
{
	if( !cl || FBitSet( cl->flags, FCL_FAKECLIENT ))
		return;

	MSG_BeginServerCmd( &cl->netchan.message, svc_stufftext );
	if( v )
	{
		const veh_common_params_t *c = &v->sim.p->c;

		MSG_WriteStringf( &cl->netchan.message, "cl_vehicle \"%d %d %g %g %g %g %g\"\n",
			v->ent, (int)v->sim.type, c->cam_dist, c->cam_height, c->cam_pitch, c->cam_lag, c->cam_follow );
	}
	else
		MSG_WriteString( &cl->netchan.message, "cl_vehicle \"0\"\n" );
}

/*
==============================================================================
spawn, enter, exit, remove
==============================================================================
*/
static int SV_VehFindPrecachedModel( const char *name )
{
	int i;

	for( i = 1; i < MAX_MODELS && sv.model_precache[i][0]; i++ )
	{
		if( !Q_stricmp( sv.model_precache[i], name ))
			return i;
	}
	return 0;
}

static const char *SV_VehModelFor( veh_type_t type, const char *requested, edict_t *driver )
{
	const char *name = requested;

	if( COM_StringEmptyOrNULL( name ))
	{
		switch( type )
		{
		case VEH_CAR: name = vehicle_model_car.string; break;
		case VEH_TANK: name = vehicle_model_tank.string; break;
		case VEH_PLANE: name = vehicle_model_plane.string; break;
		case VEH_HELI: name = vehicle_model_heli.string; break;
		default: name = NULL; break;
		}
	}
	if( COM_StringEmptyOrNULL( name ) && driver && driver->v.model )
		name = SV_GetString( driver->v.model );
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	// already precached is free; anything else is a late precache, so it must exist
	if( !SV_VehFindPrecachedModel( name ) && !FS_FileExists( name, false ))
		return NULL;
	return name;
}

static sv_vehicle_t *SV_VehSpawn( veh_type_t type, const vec3_t origin, float yaw, const char *model, edict_t *driver )
{
	sv_vehicle_state_t *st = SV_VehState();
	sv_vehicle_t *v = NULL;
	const char *mdl;
	edict_t *ent;
	vec3_t org;
	int i;

	for( i = 0; i < SV_VEH_SLOTS; i++ )
	{
		if( !st->slots[i].used || !SV_VehEdict( &st->slots[i] ))
		{
			v = &st->slots[i];
			break;
		}
	}
	if( !v )
	{
		Con_Printf( S_WARN "vehicle: all %d vehicle slots are in use\n", SV_VEH_SLOTS );
		return NULL;
	}

	ent = SV_AllocEdict();
	if( !ent )
		return NULL;

	memset( v, 0, sizeof( *v ));
	ent->v.classname = SV_AllocString( va( "vehicle_%s", Veh_TypeName( type )));
	mdl = SV_VehModelFor( type, model, driver );
	if( mdl )
		SV_SetModel( ent, mdl );
	else
		Con_Printf( S_WARN "vehicle: no model for %s, it will be invisible (set vehicle_model_%s)\n", Veh_TypeName( type ), Veh_TypeName( type ));

	ent->v.solid = SOLID_BBOX;
	ent->v.movetype = MOVETYPE_NONE;	// we move it; the edict physics only thinks
	ent->v.takedamage = DAMAGE_NO;
	SV_SetMinMaxSize( ent, st->params[type].c.hull_mins, st->params[type].c.hull_maxs, false );

	VectorCopy( origin, org );
	Veh_Init( &v->sim, type, &st->params[type], org, yaw );
	v->used = 1;
	v->ent = NUM_FOR_EDICT( ent );
	v->serial = ent->serialnumber;
	SV_VehWriteEdict( v, ent );

	Con_Reportf( "vehicle: spawned %s #%d at (%.0f %.0f %.0f) model %s\n", Veh_TypeName( type ), v->ent,
		org[0], org[1], org[2], mdl ? mdl : "-" );
	return v;
}

static void SV_VehEnter( sv_vehicle_t *v, sv_client_t *cl )
{
	edict_t *driver = cl->edict;

	v->driver = (int)( cl - svs.clients ) + 1;
	v->still_steps = 0;
	v->saved_movetype = driver->v.movetype;
	v->saved_solid = driver->v.solid;
	v->saved_effects = driver->v.effects;
	v->saved_viewmodel = driver->v.viewmodel;
	v->saved_weaponmodel = driver->v.weaponmodel;

	// MOVETYPE_NONE also keeps the client's own prediction still
	driver->v.movetype = MOVETYPE_NONE;
	driver->v.solid = SOLID_NOT;
	driver->v.effects |= EF_NODRAW;
	driver->v.viewmodel = 0;
	driver->v.weaponmodel = 0;
	VectorClear( driver->v.basevelocity );

	SV_VehPlaceDriver( v, driver );
	SV_VehTellClient( cl, v );
	SV_ClientPrintf( cl, "vehicle: in the %s (use to get out)\n", Veh_TypeName( v->sim.type ));
}

// a free spot for the driver beside, behind or above the vehicle
static qboolean SV_VehExitSpot( sv_vehicle_t *v, edict_t *driver, vec3_t out )
{
	const veh_common_params_t *c = &v->sim.p->c;
	float side = c->hull_maxs[1] + 40.0f;
	float back = c->hull_maxs[0] + 48.0f;
	const float offsets[5][3] =
	{
		{ 0.0f, side, 0.0f },		// driver's side
		{ 0.0f, -side, 0.0f },
		{ -back, 0.0f, 0.0f },
		{ back, 0.0f, 0.0f },
		{ 0.0f, 0.0f, c->hull_maxs[2] + 40.0f },	// on the roof
	};
	edict_t *ent = SV_VehEdict( v );
	int i;

	for( i = 0; i < 5; i++ )
	{
		vec3_t spot;
		trace_t tr;

		Veh_LocalToWorld( &v->sim, offsets[i], spot );
		spot[2] += 8.0f;
		tr = SV_Move( spot, driver->v.mins, driver->v.maxs, spot, MOVE_NORMAL, driver, false );
		if( !tr.startsolid && !tr.allsolid && ( !tr.ent || tr.ent != ent ))
		{
			// and in sight of the vehicle, not through a wall
			vec3_t zero = { 0.0f, 0.0f, 0.0f };
			trace_t los = SV_Move( v->sim.origin, zero, zero, spot, MOVE_NOMONSTERS, ent, false );

			if( los.fraction >= 1.0f )
			{
				VectorCopy( spot, out );
				return true;
			}
		}
	}
	return false;
}

static qboolean SV_VehExit( sv_vehicle_t *v, qboolean force )
{
	sv_client_t *cl = SV_VehDriver( v );
	edict_t *driver;
	vec3_t spot;

	if( !cl )
	{
		v->driver = 0;
		return true;
	}
	driver = cl->edict;

	if( !SV_VehExitSpot( v, driver, spot ))
	{
		if( !force )
		{
			SV_ClientPrintf( cl, "vehicle: no room to get out here\n" );
			return false;
		}
		Veh_LocalToWorld( &v->sim, v->sim.p->c.seat, spot );
	}

	driver->v.movetype = v->saved_movetype;
	driver->v.solid = v->saved_solid;
	driver->v.effects = v->saved_effects;
	driver->v.viewmodel = v->saved_viewmodel;
	driver->v.weaponmodel = v->saved_weaponmodel;
	VectorCopy( spot, driver->v.origin );
	VectorCopy( v->sim.velocity, driver->v.velocity );
	driver->v.flFallVelocity = 0.0f;
	SV_LinkEdict( driver, true );

	v->driver = 0;
	v->still_steps = 0;
	SV_VehTellClient( cl, NULL );
	return true;
}

static void SV_VehRemove( sv_vehicle_t *v )
{
	edict_t *ent = SV_VehEdict( v );

	if( v->driver )
		SV_VehExit( v, true );
	if( ent )
		SV_FreeEdict( ent );
	memset( v, 0, sizeof( *v ));
}

static sv_vehicle_t *SV_VehNearest( const edict_t *pl, float reach )
{
	sv_vehicle_t *best = NULL;
	float best_d = reach * reach;
	int i;

	if( !svv )
		return NULL;
	SV_VehState();
	for( i = 0; i < SV_VEH_SLOTS; i++ )
	{
		sv_vehicle_t *v = &svv->slots[i];
		vec3_t d;
		float d2;

		if( !SV_VehEdict( v ) || v->driver )
			continue;
		VectorSubtract( v->sim.origin, pl->v.origin, d );
		d2 = DotProduct( d, d );
		if( d2 < best_d )
		{
			best_d = d2;
			best = v;
		}
	}
	return best;
}

/*
==============================================================================
tank shell
==============================================================================
*/
static void SV_VehTankShell( sv_vehicle_t *v, edict_t *ent, edict_t *driver )
{
	vec3_t org, dir, end, zero = { 0.0f, 0.0f, 0.0f };
	edict_t *boom;
	trace_t tr;

	Veh_TurretMuzzle( &v->sim, org, dir );
	VectorMA( org, 8192.0f, dir, end );
	tr = SV_Move( org, zero, zero, end, MOVE_NORMAL, ent, false );
	VectorMA( tr.endpos, -8.0f, dir, end );

	/* The game's own env_explosion does the damage, sprite and sound the way
	 * that game does them; a game without one gets sparks only. */
	boom = vehicle_tank_magnitude.value > 0.0f ? SV_CreateNamedEntity( NULL, SV_AllocString( "env_explosion" )) : NULL;
	if( boom && boom->pvPrivateData && svgame.dllFuncs.pfnKeyValue && svgame.dllFuncs.pfnSpawn && svgame.dllFuncs.pfnUse )
	{
		char mag[16];
		KeyValueData kvd = { 0 };

		Q_snprintf( mag, sizeof( mag ), "%d", (int)vehicle_tank_magnitude.value );
		kvd.szClassName = (char *)"env_explosion";
		kvd.szKeyName = (char *)"iMagnitude";
		kvd.szValue = mag;
		svgame.dllFuncs.pfnKeyValue( boom, &kvd );
		VectorCopy( end, boom->v.origin );
		boom->v.owner = driver ? driver : ent;
		svgame.dllFuncs.pfnSpawn( boom );
		svgame.dllFuncs.pfnUse( boom, driver ? driver : ent );
	}
	else
	{
		if( boom )
			SV_FreeEdict( boom );
		MSG_BeginServerCmd( &sv.datagram, svc_temp_entity );
		MSG_WriteByte( &sv.datagram, TE_SPARKS );
		MSG_WriteVec3Coord( &sv.datagram, end );
	}
}

static void SV_VehEvents( sv_vehicle_t *v, edict_t *ent, edict_t *driver )
{
	unsigned ev = Veh_TakeEvents( &v->sim );

	if( ev & VEH_EV_FIRE )
		SV_VehTankShell( v, ent, driver );
	if( ev & VEH_EV_CRASH )
		Con_Reportf( "vehicle: #%d crashed at %.0f u/s\n", v->ent, v->sim.impact_speed );
}

/*
==============================================================================
per command / per frame
==============================================================================
*/
static void SV_VehAutospawn( sv_client_t *cl )
{
	char typename[32];
	veh_type_t type;
	vec3_t org;
	edict_t *pl = cl->edict;
	sv_vehicle_t *v;

	svv->autospawn_done = 1;
	if( !Sys_GetParmFromCmdLine( "-vehicle", typename ))
		return;
	type = Veh_TypeForName( typename );
	if( type == VEH_NONE )
	{
		Con_Printf( S_WARN "-vehicle: unknown type '%s' (car, tank, plane, heli)\n", typename );
		return;
	}

	// spawn on the player: they get straight in
	VectorCopy( pl->v.origin, org );
	org[2] += 8.0f;
	v = SV_VehSpawn( type, org, pl->v.angles[YAW], NULL, pl );
	if( v )
		SV_VehEnter( v, cl );
}

qboolean SV_VehicleRunCmd( sv_client_t *cl, const usercmd_t *ucmd, float frametime )
{
	sv_vehicle_t *v;
	edict_t *driver = cl->edict, *ent;
	unsigned pressed;
	int slot = (int)( cl - svs.clients );
	veh_input_t in;
	veh_world_t world;
	sv_veh_trace_ctx_t ctx;

	if( !svv )
	{
		// nothing to do until a vehicle command or the launch option
		static int launch_option = -1;

		if( launch_option < 0 )
			launch_option = Sys_CheckParm( "-vehicle" ) ? 1 : 0;
		if( !launch_option )
			return false;
	}
	SV_VehState();

	if( slot < 0 || slot >= MAX_CLIENTS || !driver )
		return false;
	pressed = ucmd->buttons & ~svv->oldbuttons[slot];
	svv->oldbuttons[slot] = ucmd->buttons;

	if( !svv->autospawn_done && slot == 0 && cl->state == cs_spawned && driver->v.health > 0.0f && sv.state == ss_active )
		SV_VehAutospawn( cl );

	v = SV_VehForClient( cl );
	if( !v )
	{
		// +use beside a vehicle gets in
		if(( pressed & IN_USE ) && driver->v.health > 0.0f && driver->v.deadflag == DEAD_NO )
		{
			sv_vehicle_t *near = SV_VehNearest( driver, SV_VEH_REACH );

			if( near )
			{
				SV_VehEnter( near, cl );
				return true;
			}
		}
		return false;
	}

	ent = SV_VehEdict( v );
	if( !ent )
	{
		// the vehicle edict went away under us
		v->used = 0;
		SV_VehExit( v, true );
		return false;
	}

	if( driver->v.health <= 0.0f || driver->v.deadflag != DEAD_NO )
	{
		SV_VehExit( v, true );
		return false;
	}

	if(( pressed & IN_USE ) && SV_VehExit( v, false ))
		return false;

	Veh_MapInput( v->sim.type, ucmd->forwardmove, ucmd->sidemove, ucmd->upmove, ucmd->buttons,
		ucmd->viewangles, vehicle_input_scale.value, &in );
	SV_VehWorld( &world, &ctx, ent );
	Veh_Update( &v->sim, &in, &world, frametime );

	SV_VehWriteEdict( v, ent );
	SV_VehPlaceDriver( v, driver );
	VectorCopy( ucmd->viewangles, driver->v.v_angle );
	SV_VehEvents( v, ent, driver );
	return true;
}

void SV_VehicleFrame( float frametime )
{
	static const veh_input_t idle = { 0 };
	int i;

	if( !svv || sv.state != ss_active )
		return;
	SV_VehState();

	for( i = 0; i < SV_VEH_SLOTS; i++ )
	{
		sv_vehicle_t *v = &svv->slots[i];
		veh_world_t world;
		sv_veh_trace_ctx_t ctx;
		edict_t *ent;

		if( !v->used )
			continue;
		ent = SV_VehEdict( v );
		if( !ent )
		{
			if( v->driver )
				SV_VehExit( v, true );
			memset( v, 0, sizeof( *v ));
			continue;
		}
		if( v->driver )
		{
			// driver gone (disconnect, level change): hand the vehicle back
			if( !SV_VehDriver( v ))
				v->driver = 0;
			else
				continue;	// stepped by the driver's usercmds
		}
		if( v->still_steps >= SV_VEH_SLEEP_STEPS )
			continue;

		SV_VehWorld( &world, &ctx, ent );
		v->still_steps += Veh_Update( &v->sim, &idle, &world, frametime );
		if( !v->sim.on_ground || DotProduct( v->sim.velocity, v->sim.velocity ) > 4.0f )
			v->still_steps = 0;
		SV_VehWriteEdict( v, ent );
		SV_VehEvents( v, ent, NULL );
	}
}

/*
==============================================================================
console
==============================================================================
*/
static void SV_VehTune( sv_client_t *cl )
{
	sv_vehicle_state_t *st = SV_VehState();
	veh_type_t type = Veh_TypeForName( Cmd_Argv( 2 ));
	const char *const *names;
	const unsigned short *ofs;
	float *f;
	int i, n;

	if( type == VEH_NONE )
	{
		SV_ClientPrintf( cl, "vehicle tune <car|tank|plane|heli> [param [value]]\n" );
		return;
	}

	if( Cmd_Argc() < 4 )
	{
		n = Veh_ParamTable( type, &names, &ofs );
		for( i = 0; i < n; i++ )
			SV_ClientPrintf( cl, "  %-20s %g\n", names[i], *(float *)((byte *)&st->params[type] + ofs[i] ));
		return;
	}

	f = Veh_ParamPtr( type, &st->params[type], Cmd_Argv( 3 ));
	if( !f )
	{
		SV_ClientPrintf( cl, "vehicle: %s has no '%s'\n", Veh_TypeName( type ), Cmd_Argv( 3 ));
		return;
	}
	if( Cmd_Argc() >= 5 )
	{
		if( !Cvar_VariableInteger( "sv_cheats" ) && svs.maxclients > 1 )
		{
			SV_ClientPrintf( cl, "vehicle tune needs sv_cheats 1 in multiplayer\n" );
			return;
		}
		*f = Q_atof( Cmd_Argv( 4 ));
		for( i = 0; i < SV_VEH_SLOTS; i++ )
		{
			sv_vehicle_t *v = &st->slots[i];

			if( !v->used || v->sim.type != type )
				continue;
			// wheel layout and torque split tunables take effect at once
			Veh_LayoutWheels( &v->sim );
			v->still_steps = 0;
			// drivers' cameras pick up the new camera tunables
			if( v->driver )
				SV_VehTellClient( SV_VehDriver( v ), v );
		}
	}
	SV_ClientPrintf( cl, "%s %s = %g\n", Veh_TypeName( type ), Cmd_Argv( 3 ), *f );
}

qboolean SV_VehicleClientCommand( sv_client_t *cl )
{
	const char *sub;
	edict_t *pl = cl->edict;
	sv_vehicle_t *v;

	if( Q_strcmp( Cmd_Argv( 0 ), "vehicle" ))
		return false;
	if( cl->state != cs_spawned || !pl || sv.background )
		return true;

	SV_VehState();
	sub = Cmd_Argv( 1 );

	if( !Q_stricmp( sub, "spawn" ))
	{
		veh_type_t type = Veh_TypeForName( Cmd_Argv( 2 ));
		vec3_t org, fwd;
		float dist;

		if( svs.maxclients > 1 && !Cvar_VariableInteger( "sv_cheats" ))
		{
			SV_ClientPrintf( cl, "vehicle spawn needs sv_cheats 1 in multiplayer\n" );
			return true;
		}
		if( type == VEH_NONE )
		{
			SV_ClientPrintf( cl, "vehicle spawn <car|tank|plane|heli> [model]\n" );
			return true;
		}
		fwd[0] = cosf( DEG2RAD( pl->v.v_angle[YAW] ));
		fwd[1] = sinf( DEG2RAD( pl->v.v_angle[YAW] ));
		fwd[2] = 0.0f;
		dist = svv->params[type].c.hull_maxs[0] + 64.0f;
		VectorMA( pl->v.origin, dist, fwd, org );
		org[2] += 16.0f;
		v = SV_VehSpawn( type, org, pl->v.v_angle[YAW], Cmd_Argc() > 3 ? Cmd_Argv( 3 ) : NULL, pl );
		if( v )
			SV_ClientPrintf( cl, "vehicle: %s #%d spawned, use it to get in\n", Veh_TypeName( type ), v->ent );
	}
	else if( !Q_stricmp( sub, "use" ) || !Q_stricmp( sub, "enter" ) || !Q_stricmp( sub, "exit" ))
	{
		v = SV_VehForClient( cl );
		if( v && Q_stricmp( sub, "enter" ))
			SV_VehExit( v, false );
		else if( !v && Q_stricmp( sub, "exit" ))
		{
			v = SV_VehNearest( pl, SV_VEH_REACH );
			if( v )
				SV_VehEnter( v, cl );
			else
				SV_ClientPrintf( cl, "vehicle: nothing to get in near you\n" );
		}
	}
	else if( !Q_stricmp( sub, "remove" ))
	{
		v = SV_VehForClient( cl );
		if( !v )
			v = SV_VehNearest( pl, 1024.0f );
		if( v )
			SV_VehRemove( v );
	}
	else if( !Q_stricmp( sub, "tune" ))
	{
		SV_VehTune( cl );
	}
	else if( !Q_stricmp( sub, "info" ))
	{
		int i;

		for( i = 0; i < SV_VEH_SLOTS; i++ )
		{
			v = &svv->slots[i];
			if( !SV_VehEdict( v ))
				continue;
			SV_ClientPrintf( cl, "#%d %s at (%.0f %.0f %.0f) speed %.0f wheels %d/%d %s%s\n", v->ent,
				Veh_TypeName( v->sim.type ), v->sim.origin[0], v->sim.origin[1], v->sim.origin[2],
				VectorLength( v->sim.velocity ), v->sim.wheels_down, v->sim.num_wheels, v->driver ? "driven" : "empty",
				v->still_steps >= SV_VEH_SLEEP_STEPS ? " asleep" : "" );
			if( v->sim.type == VEH_CAR || v->sim.type == VEH_TANK )
				SV_ClientPrintf( cl, "    gear %d, %.0f rpm, clutch %.2f, slip %.2f\n", v->sim.gear, Veh_EngineRPM( &v->sim ), v->sim.clutch, v->sim.avg_side_slip );
			else
				SV_ClientPrintf( cl, "    power %.2f, altitude %.0f\n", v->sim.power, v->sim.altitude );
		}
	}
	else
	{
		SV_ClientPrintf( cl, "vehicle spawn <car|tank|plane|heli> [model] | use | enter | exit | remove | info | tune <type> [param [value]]\n" );
	}
	return true;
}

void SV_VehicleInit( void )
{
	Cvar_RegisterVariable( &vehicle_input_scale );
	Cvar_RegisterVariable( &vehicle_model_car );
	Cvar_RegisterVariable( &vehicle_model_plane );
	Cvar_RegisterVariable( &vehicle_model_heli );
	Cvar_RegisterVariable( &vehicle_model_tank );
	Cvar_RegisterVariable( &vehicle_tank_magnitude );
}

#endif // XASH_VEHICLES
