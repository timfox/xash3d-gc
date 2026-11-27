/*
cl_vehicle.c - chase camera for the engine vehicles (server/sv_vehicle.c)

The server says which entity we are driving with a stufftext:

	cl_vehicle "<entnum> <type> <dist> <height> <pitch> <lag> <follow>"
	cl_vehicle "0"			on foot again

and from then on the eye orbits that entity's interpolated origin using our
own view angles, so looking around has no network delay. With follow set (the
car) the view yaw turns with the vehicle, so the camera swings in behind a
turning car unless the player looks away. The camera half of the vehicle core
(common/vehicle.c) does the smoothing and the wall pull-in.

	cl_vehicle_cam 1	0 = ride in the seat (first person, the player is placed there)
*/
#include "common.h"
#include "client.h"
#include "vehicle.h"
#include "pm_defs.h"

#if XASH_VEHICLES

static CVAR_DEFINE_AUTO( cl_vehicle, "0", 0, "set by the server: the vehicle entity we drive and its camera" );
static CVAR_DEFINE_AUTO( cl_vehicle_cam, "1", FCVAR_ARCHIVE, "chase camera while driving a vehicle (0 = first person from the seat)" );

static struct
{
	char			value[96];	// cl_vehicle as last parsed
	int			ent;
	int			type;
	veh_common_params_t	cam;
	veh_cam_t		state;
	double			last_time;
	int			servercount;	// the map the server told us on
} clv;

static void CL_VehicleParse( void )
{
	float dist = 0.0f, height = 0.0f, pitch = 0.0f, lag = 0.0f, follow = 0.0f;
	int ent = 0, type = 0;
	const char *s = cl_vehicle.string;

	if( !Q_strcmp( s, clv.value ))
		return;
	Q_strncpy( clv.value, s, sizeof( clv.value ));

	if( sscanf( s, "%d %d %f %f %f %f %f", &ent, &type, &dist, &height, &pitch, &lag, &follow ) < 2 || ent <= 0 )
	{
		clv.ent = 0;
		return;
	}

	clv.ent = ent;
	clv.type = type;
	clv.servercount = cl.servercount;
	clv.cam.cam_dist = dist;
	clv.cam.cam_height = height;
	clv.cam.cam_pitch = pitch;
	clv.cam.cam_lag = lag > 0.0f ? lag : 6.0f;
	clv.cam.cam_follow = follow;
	Veh_CamReset( &clv.state );
}

static void CL_VehicleTrace( void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3], veh_trace_t *out )
{
	vec3_t s, e;
	pmtrace_t tr;

	(void)ctx; (void)mins; (void)maxs;
	VectorCopy( start, s );
	VectorCopy( end, e );
	// the camera only needs to stay out of the walls
	tr = CL_TraceLine( s, e, PM_WORLD_ONLY );
	out->fraction = tr.fraction;
	VectorCopy( tr.endpos, out->endpos );
	VectorCopy( tr.plane.normal, out->normal );
	out->startsolid = tr.startsolid;
	out->allsolid = tr.allsolid;
}

qboolean CL_VehicleActive( void )
{
	CL_VehicleParse();
	// a level change ends the ride: the new map has its own vehicles
	if( clv.ent > 0 && clv.servercount != cl.servercount )
	{
		Cvar_DirectSet( &cl_vehicle, "0" );
		CL_VehicleParse();
	}
	return clv.ent > 0 && cls.state == ca_active;
}

qboolean CL_VehicleHideViewModel( void )
{
	return CL_VehicleActive() && cl_vehicle_cam.value != 0.0f;
}

/*
CL_VehicleView

Called once a rendered view, after the client dll placed it. Returns true and
fills org/ang when the vehicle camera owns the eye.
*/
qboolean CL_VehicleView( vec3_t org, vec3_t ang )
{
	cl_entity_t *e;
	veh_world_t world;
	vec3_t target;
	float dt;

	if( !CL_VehicleActive() || cl_vehicle_cam.value == 0.0f )
		return false;

	dt = (float)( cl.time - clv.last_time );
	clv.last_time = cl.time;
	if( dt < 0.0f || dt > 0.25f )
		dt = 0.0f;

	e = CL_GetEntityByIndex( clv.ent );
	if( e && e->model && e->curstate.messagenum == cl.parsecount )
	{
		VectorCopy( e->origin, target );
		// a car's view turns with it; the player can still look round
		if( clv.cam.cam_follow > 0.5f )
			cl.viewangles[YAW] = anglemod( cl.viewangles[YAW] + Veh_CamFollowYaw( &clv.state, e->angles[YAW] ));
	}
	else
	{
		// an invisible vehicle is not sent: orbit the driver, who rides in the seat
		VectorCopy( cl.simorg, target );
	}

	world.trace = CL_VehicleTrace;
	world.ctx = NULL;
	world.gravity = 0.0f;
	Veh_CamUpdate( &clv.state, &clv.cam, target, cl.viewangles, &world, dt );
	VectorCopy( clv.state.origin, org );
	VectorCopy( clv.state.angles, ang );
	return true;
}

void CL_VehicleInit( void )
{
	Cvar_RegisterVariable( &cl_vehicle );
	Cvar_RegisterVariable( &cl_vehicle_cam );
}

#endif // XASH_VEHICLES
