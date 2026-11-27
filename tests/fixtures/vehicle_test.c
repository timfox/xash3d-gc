/* Host test for the vehicle core (engine/common/vehicle*.c).
 * The world is a set of infinite half-spaces (solid behind each plane), which
 * is enough to check settling, driving, drifting, walls, slopes, flight and
 * the camera without a BSP. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vehicle.h"

static int failures;

#define CHECK( cond, ... ) do { if( !( cond )) { failures++; printf( "FAIL %s:%d: ", __FILE__, __LINE__ ); printf( __VA_ARGS__ ); printf( "\n" ); } } while( 0 )

typedef struct
{
	float n[3];
	float d;
} plane_t;

typedef struct
{
	plane_t planes[4];
	int count;
	int traces;
} fake_world_t;

static void fake_trace( void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3], veh_trace_t *tr )
{
	fake_world_t *w = (fake_world_t *)ctx;
	float best = 1.0f;
	int i, k, hit = -1;

	w->traces++;
	memset( tr, 0, sizeof( *tr ));
	for( i = 0; i < w->count; i++ )
	{
		const plane_t *p = &w->planes[i];
		float o[3], s, e;

		for( k = 0; k < 3; k++ )
			o[k] = p->n[k] > 0.0f ? mins[k] : maxs[k];
		s = p->n[0] * ( start[0] + o[0] ) + p->n[1] * ( start[1] + o[1] ) + p->n[2] * ( start[2] + o[2] ) - p->d;
		e = p->n[0] * ( end[0] + o[0] ) + p->n[1] * ( end[1] + o[1] ) + p->n[2] * ( end[2] + o[2] ) - p->d;
		if( s < 0.0f )
		{
			tr->startsolid = 1;
			if( e < 0.0f )
			{
				tr->allsolid = 1;
				tr->fraction = 0.0f;
				memcpy( tr->endpos, start, sizeof( float ) * 3 );
				memcpy( tr->normal, p->n, sizeof( float ) * 3 );
				return;
			}
			continue;
		}
		if( e < 0.0f )
		{
			float f = ( s - 0.03125f ) / ( s - e );
			if( f < 0.0f ) f = 0.0f;
			if( f < best ) { best = f; hit = i; }
		}
	}
	tr->fraction = best;
	for( k = 0; k < 3; k++ )
		tr->endpos[k] = start[k] + ( end[k] - start[k] ) * best;
	if( hit >= 0 )
		memcpy( tr->normal, w->planes[hit].n, sizeof( float ) * 3 );
}

static fake_world_t fw;
static veh_world_t world;

static void world_flat( void )
{
	memset( &fw, 0, sizeof( fw ));
	fw.planes[0].n[2] = 1.0f;	// floor z = 0
	fw.count = 1;
	world.trace = fake_trace;
	world.ctx = &fw;
	world.gravity = 800.0f;
}

static void world_add_wall_x( float x )
{
	plane_t *p = &fw.planes[fw.count++];
	memset( p, 0, sizeof( *p ));
	p->n[0] = -1.0f;	// solid at x > wall
	p->d = -x;
}

static void world_slope( float deg )
{
	float r = deg * 3.14159265f / 180.0f;

	memset( &fw, 0, sizeof( fw ));
	fw.planes[0].n[0] = -sinf( r );	// rises toward +x
	fw.planes[0].n[2] = cosf( r );
	fw.count = 1;
	world.trace = fake_trace;
	world.ctx = &fw;
	world.gravity = 800.0f;
}

static void run( veh_t *v, const veh_input_t *in, float seconds )
{
	int steps = (int)( seconds * VEH_STEP_HZ + 0.5f ), i;

	for( i = 0; i < steps; i++ )
		Veh_Step( v, in, &world );
}

static float yaw_of( const veh_t *v )
{
	float a[3];
	Veh_GetAngles( v, a );
	return a[1];
}

static float speed2d( const veh_t *v )
{
	return sqrtf( v->velocity[0] * v->velocity[0] + v->velocity[1] * v->velocity[1] );
}

static void test_names_and_params( void )
{
	veh_params_t p;
	const char *const *names;
	const unsigned short *ofs;
	int t, n, i, j;

	CHECK( Veh_TypeForName( "car" ) == VEH_CAR, "car" );
	CHECK( Veh_TypeForName( "TANK" ) == VEH_TANK, "tank" );
	CHECK( Veh_TypeForName( "plane" ) == VEH_FLIGHT, "plane alias" );
	CHECK( Veh_TypeForName( "boat" ) == VEH_NONE, "unknown" );

	for( t = VEH_CAR; t < VEH_TYPE_COUNT; t++ )
	{
		Veh_DefaultParams( (veh_type_t)t, &p );
		n = Veh_ParamTable( (veh_type_t)t, &names, &ofs );
		CHECK( n > 10, "%s has %d params", Veh_TypeName( (veh_type_t)t ), n );
		for( i = 0; i < n; i++ )
		{
			CHECK( ofs[i] + sizeof( float ) <= sizeof( p ), "offset in range" );
			for( j = i + 1; j < n; j++ )
				CHECK( strcmp( names[i], names[j] ), "duplicate param %s", names[i] );
		}
		CHECK( Veh_ParamPtr( (veh_type_t)t, &p, "cam_dist" ) == &p.c.cam_dist, "cam_dist ptr" );
	}
	CHECK( Veh_ParamPtr( VEH_CAR, &p, "drift_grip" ) != NULL, "car drift_grip" );
	CHECK( Veh_ParamPtr( VEH_TANK, &p, "drift_grip" ) == NULL, "tank has no drift" );
	Veh_DefaultParams( VEH_CAR, &p );
	*Veh_ParamPtr( VEH_CAR, &p, "accel" ) = 123.0f;
	CHECK( p.car.g.accel == 123.0f, "param write lands" );
}

static void test_input_map( void )
{
	veh_input_t in;
	float ang[3] = { 10.0f, 90.0f, 0.0f };

	Veh_MapInput( VEH_CAR, 400.0f, -200.0f, 0.0f, VEH_IN_JUMP, ang, 400.0f, &in );
	CHECK( in.throttle == 1.0f && in.steer == -0.5f, "car axes %.2f %.2f", in.throttle, in.steer );
	CHECK( in.buttons & VEH_BTN_HANDBRAKE, "jump is handbrake" );
	Veh_MapInput( VEH_TANK, 0.0f, 0.0f, 0.0f, VEH_IN_ATTACK, ang, 400.0f, &in );
	CHECK( in.buttons & VEH_BTN_FIRE, "attack fires" );
	CHECK( in.aim[1] == 90.0f, "aim copied" );
	Veh_MapInput( VEH_FLIGHT, 0.0f, 0.0f, 0.0f, VEH_IN_DUCK, ang, 400.0f, &in );
	CHECK( in.lift == -1.0f, "duck lowers throttle lever" );
	Veh_MapInput( VEH_CAR, 900.0f, 0.0f, 0.0f, 0, ang, 400.0f, &in );
	CHECK( in.throttle == 1.0f, "clamped" );
}

static void test_angles( void )
{
	static const float yaws[] = { 0.0f, 37.0f, 135.0f, -90.0f };
	veh_params_t p;
	veh_t v;
	float o[3] = { 0, 0, 0 }, a[3];
	unsigned i;

	Veh_DefaultParams( VEH_FLIGHT, &p );
	for( i = 0; i < sizeof( yaws ) / sizeof( yaws[0] ); i++ )
	{
		Veh_Init( &v, VEH_FLIGHT, &p, o, yaws[i] );
		Veh_GetAngles( &v, a );
		CHECK( fabsf( Veh_AngleMod( a[1] - yaws[i] )) < 0.01f && fabsf( a[0] ) < 0.01f && fabsf( a[2] ) < 0.01f,
			"yaw %.1f -> %.2f %.2f %.2f", yaws[i], a[0], a[1], a[2] );
	}
	// nose up 0.2 rad
	Veh_Init( &v, VEH_FLIGHT, &p, o, 0.0f );
	{
		float s = sinf( 0.2f ), c = cosf( 0.2f );
		v.fwd[0] = c; v.fwd[2] = s;
		v.up[0] = -s; v.up[2] = c;
	}
	Veh_GetAngles( &v, a );
	CHECK( fabsf( a[0] - 0.2f * 57.29578f ) < 0.05f, "studio pitch is nose up positive (%.2f)", a[0] );
}

static void test_car( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 40.0f };
	float y0, rest_z;
	unsigned ev;

	world_flat();
	Veh_DefaultParams( VEH_CAR, &p );
	Veh_Init( &v, VEH_CAR, &p, o, 0.0f );
	memset( &in, 0, sizeof( in ));

	run( &v, &in, 3.0f );
	rest_z = v.origin[2];
	CHECK( v.wheels_down == 4, "settled on 4 wheels (%d)", v.wheels_down );
	CHECK( fabsf( v.velocity[2] ) < 2.0f && speed2d( &v ) < 1.0f, "at rest v=(%.2f,%.2f,%.2f)", v.velocity[0], v.velocity[1], v.velocity[2] );
	CHECK( rest_z > -p.c.hull_mins[2] && rest_z < p.car.g.susp_length + p.car.g.wheel_radius, "ride height %.2f", rest_z );
	ev = Veh_TakeEvents( &v );
	CHECK( ev & VEH_EV_LANDED, "landing event" );

	// full throttle in a straight line
	in.throttle = 1.0f;
	run( &v, &in, 6.0f );
	CHECK( v.velocity[0] > p.car.g.max_speed * 0.8f, "accelerates to %.1f", v.velocity[0] );
	CHECK( v.velocity[0] <= p.car.g.max_speed + 1.0f, "top speed held %.1f", v.velocity[0] );
	CHECK( fabsf( v.velocity[1] ) < 1.0f && fabsf( yaw_of( &v )) < 0.1f, "straight" );
	CHECK( fabsf( v.origin[2] - rest_z ) < 2.0f, "ride height stable at speed %.2f vs %.2f", v.origin[2], rest_z );

	// steer right: yaw goes negative (clockwise from above)
	y0 = yaw_of( &v );
	in.throttle = 0.6f;
	in.steer = 1.0f;
	run( &v, &in, 0.5f );
	CHECK( Veh_AngleMod( yaw_of( &v ) - y0 ) < -5.0f, "steer right turns clockwise (%.1f)", yaw_of( &v ));
	CHECK( v.slip < 0.15f, "grip holds a normal turn (slip %.2f)", v.slip );

	// handbrake drift: slide builds, then recovers when released
	in.buttons = VEH_BTN_HANDBRAKE;
	run( &v, &in, 0.6f );
	CHECK( v.drift > 0.8f, "drift factor %.2f", v.drift );
	CHECK( v.slip > 0.2f, "sliding (slip %.2f)", v.slip );
	in.buttons = 0;
	in.steer = 0.0f;
	in.throttle = 0.5f;
	run( &v, &in, 2.0f );
	CHECK( v.drift < 0.05f && v.slip < 0.05f, "recovered drift %.2f slip %.2f", v.drift, v.slip );

	// brake to a stop, then reverse
	in.throttle = -1.0f;
	run( &v, &in, 4.0f );
	CHECK( Veh_ForwardSpeed( &v ) < 0.0f && Veh_ForwardSpeed( &v ) > -p.car.g.reverse_speed - 1.0f, "reverse %.1f", Veh_ForwardSpeed( &v ));

	// coast to rest
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 5.0f );
	CHECK( speed2d( &v ) < 1.0f, "coasts to rest %.2f", speed2d( &v ));
}

static void test_car_wall_and_slope( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 30.0f };
	float max_x = 0.0f;
	unsigned ev = 0;
	int i;

	world_flat();
	world_add_wall_x( 1500.0f );
	Veh_DefaultParams( VEH_CAR, &p );
	Veh_Init( &v, VEH_CAR, &p, o, 0.0f );
	memset( &in, 0, sizeof( in ));
	in.throttle = 1.0f;
	for( i = 0; i < 6 * VEH_STEP_HZ; i++ )
	{
		Veh_Step( &v, &in, &world );
		ev |= Veh_TakeEvents( &v );
		if( v.origin[0] > max_x ) max_x = v.origin[0];
	}
	CHECK( max_x + p.c.hull_maxs[0] <= 1500.0f + 0.1f, "never through the wall (%.2f)", max_x + p.c.hull_maxs[0] );
	CHECK( ev & VEH_EV_IMPACT, "wall impact event" );
	CHECK( v.velocity[0] < 50.0f, "stopped by the wall (%.1f)", v.velocity[0] );

	// a 12 degree slope: the body pitches to match and the car stays put with no throttle
	world_slope( 12.0f );
	o[2] = 40.0f;
	Veh_Init( &v, VEH_CAR, &p, o, 0.0f );
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 3.0f );
	{
		float a[3];
		Veh_GetAngles( &v, a );
		CHECK( fabsf( a[0] - 12.0f ) < 1.5f, "nose up on the slope (%.2f)", a[0] );
		CHECK( v.wheels_down == 4, "all wheels on the slope (%d)", v.wheels_down );
	}
}

static void test_tank( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 30.0f };
	int fires = 0, i;

	world_flat();
	Veh_DefaultParams( VEH_TANK, &p );
	Veh_Init( &v, VEH_TANK, &p, o, 0.0f );
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 2.0f );

	// pivot on the spot
	in.steer = -1.0f;	// left
	run( &v, &in, 1.0f );
	CHECK( yaw_of( &v ) > 40.0f && yaw_of( &v ) < 55.0f, "pivot left at rest (%.1f deg/s)", yaw_of( &v ));
	CHECK( speed2d( &v ) < 5.0f, "pivot does not move the hull (%.1f)", speed2d( &v ));

	// drive: slow top speed, no slide while turning
	in.steer = 0.0f;
	in.throttle = 1.0f;
	run( &v, &in, 4.0f );
	CHECK( Veh_ForwardSpeed( &v ) > p.tank.g.max_speed * 0.8f && Veh_ForwardSpeed( &v ) <= p.tank.g.max_speed + 1.0f, "tank speed %.1f", Veh_ForwardSpeed( &v ));
	in.steer = 1.0f;
	run( &v, &in, 1.0f );
	CHECK( v.slip < 0.05f, "tracks do not slide (%.2f)", v.slip );

	// turret slews toward the view at its rate and respects the pitch limit
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 2.0f );
	in.aim[1] = yaw_of( &v ) + 90.0f;
	in.aim[0] = -60.0f;	// looking far up
	{
		float t0 = v.turret_yaw;

		run( &v, &in, 1.0f );
		CHECK( fabsf( Veh_AngleMod( v.turret_yaw - t0 ) - p.tank.turret_yaw_rate ) < 1.5f, "turret slewed one second's worth (%.1f)", v.turret_yaw - t0 );
	}
	run( &v, &in, 4.0f );
	CHECK( fabsf( v.turret_yaw - 90.0f ) < 0.5f, "turret on target (%.1f)", v.turret_yaw );
	CHECK( fabsf( v.turret_pitch - p.tank.turret_pitch_max ) < 0.01f, "pitch clamped (%.1f)", v.turret_pitch );

	// gun cadence
	in.buttons = VEH_BTN_FIRE;
	for( i = 0; i < 6 * VEH_STEP_HZ; i++ )
	{
		Veh_Step( &v, &in, &world );
		if( Veh_TakeEvents( &v ) & VEH_EV_FIRE )
			fires++;
	}
	CHECK( fires == 3, "fires every %.1f s: %d shots in 6 s", p.tank.fire_interval, fires );
	{
		float org[3], dir[3];
		Veh_TurretMuzzle( &v, org, dir );
		CHECK( fabsf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] - 1.0f ) < 1e-3f, "barrel dir unit" );
		CHECK( org[2] > v.origin[2], "muzzle above the hull" );
	}
}

static void test_flight( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 4000.0f };
	float z0, a[3];
	int i;

	world_flat();
	Veh_DefaultParams( VEH_FLIGHT, &p );

	// level cruise in mouse aim, aim at the horizon
	Veh_Init( &v, VEH_FLIGHT, &p, o, 0.0f );
	v.velocity[0] = p.flight.cruise_speed;
	v.throttle_lever = 0.5f;
	memset( &in, 0, sizeof( in ));
	z0 = v.origin[2];
	run( &v, &in, 6.0f );
	CHECK( fabsf( v.origin[2] - z0 ) < 400.0f, "holds altitude on the horizon (%.0f)", v.origin[2] - z0 );
	CHECK( Veh_ForwardSpeed( &v ) > p.flight.stall_speed, "keeps flying (%.0f)", Veh_ForwardSpeed( &v ));

	// mouse aim turn to the right: heading converges on the view yaw
	in.aim[1] = -90.0f;
	run( &v, &in, 10.0f );
	Veh_GetAngles( &v, a );
	CHECK( fabsf( Veh_AngleMod( a[1] + 90.0f )) < 10.0f, "turned onto the aim (%.1f)", a[1] );
	CHECK( fabsf( a[2] ) < 20.0f, "rolled out after the turn (%.1f)", a[2] );

	// climb on aim
	in.aim[0] = -30.0f;	// look up
	z0 = v.origin[2];
	run( &v, &in, 3.0f );
	CHECK( v.origin[2] > z0 + 200.0f, "climbs toward a raised aim (%.0f)", v.origin[2] - z0 );

	// idle and pull up: stall, the nose falls
	in.aim[0] = -80.0f;
	v.throttle_lever = 0.0f;
	in.lift = -1.0f;
	run( &v, &in, 12.0f );
	Veh_GetAngles( &v, a );
	CHECK( v.velocity[2] < 0.0f, "stalled and sinking (%.0f)", v.velocity[2] );

	// takeoff run from the ground at full throttle, aiming slightly up
	o[2] = -p.c.hull_mins[2] + 1.0f;
	Veh_Init( &v, VEH_FLIGHT, &p, o, 0.0f );
	memset( &in, 0, sizeof( in ));
	in.lift = 1.0f;
	in.aim[0] = -10.0f;
	for( i = 0; i < 20 * VEH_STEP_HZ && v.origin[2] < 200.0f; i++ )
		Veh_Step( &v, &in, &world );
	CHECK( v.origin[2] >= 200.0f, "takes off within 20 s (z %.0f, speed %.0f)", v.origin[2], Veh_ForwardSpeed( &v ));
	CHECK( Veh_TakeEvents( &v ) & VEH_EV_TAKEOFF, "takeoff event" );

	// dive into the ground: crash event, never below the floor
	Veh_Init( &v, VEH_FLIGHT, &p, o, 0.0f );
	v.origin[2] = 300.0f;
	v.velocity[0] = 400.0f;
	v.velocity[2] = -1400.0f;
	{
		unsigned ev = 0;
		float min_z = 1e9f;
		memset( &in, 0, sizeof( in ));
		in.aim[0] = 60.0f;
		for( i = 0; i < 2 * VEH_STEP_HZ; i++ )
		{
			Veh_Step( &v, &in, &world );
			ev |= Veh_TakeEvents( &v );
			if( v.origin[2] < min_z ) min_z = v.origin[2];
		}
		CHECK( ev & VEH_EV_CRASH, "crash event" );
		CHECK( min_z + p.c.hull_mins[2] >= -0.1f, "never through the floor (%.2f)", min_z + p.c.hull_mins[2] );
	}
}

static void test_update_and_determinism( void )
{
	veh_params_t p;
	veh_t a, b;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 30.0f };
	int i;

	world_flat();
	Veh_DefaultParams( VEH_CAR, &p );
	Veh_Init( &a, VEH_CAR, &p, o, 10.0f );
	memset( &in, 0, sizeof( in ));
	CHECK( Veh_Update( &a, &in, &world, 0.05f ) == 3, "50 ms is 3 steps" );
	CHECK( Veh_Update( &a, &in, &world, 1.0f ) == VEH_MAX_SUBSTEPS, "hitch capped" );
	CHECK( a.accum == 0.0f, "hitch remainder dropped" );

	// same inputs, same result, whatever the cmd split
	Veh_Init( &a, VEH_CAR, &p, o, 10.0f );
	Veh_Init( &b, VEH_CAR, &p, o, 10.0f );
	in.throttle = 1.0f;
	in.steer = 0.4f;
	for( i = 0; i < 120; i++ )
		Veh_Update( &a, &in, &world, 0.01f );
	for( i = 0; i < 40; i++ )
		Veh_Update( &b, &in, &world, 0.03f );
	CHECK( a.steps == b.steps && !memcmp( a.origin, b.origin, sizeof( a.origin )), "deterministic across cmd splits (%u/%u)", a.steps, b.steps );

	// trace budget: a car step is wheels + a short slide
	fw.traces = 0;
	Veh_Step( &a, &in, &world );
	CHECK( fw.traces <= VEH_MAX_WHEELS + 4, "car step traces %d", fw.traces );
}

static void test_camera( void )
{
	veh_params_t p;
	veh_cam_t cam;
	float target[3] = { 0.0f, 0.0f, 100.0f };
	float look[3] = { 0.0f, 0.0f, 0.0f };
	float d;
	int i;

	world_flat();
	Veh_DefaultParams( VEH_CAR, &p );
	Veh_CamReset( &cam );
	Veh_CamUpdate( &cam, &p.c, target, look, &world, 0.016f );
	CHECK( cam.origin[0] < -p.c.cam_dist * 0.9f && cam.origin[2] > target[2], "behind and above (%.0f,%.0f)", cam.origin[0], cam.origin[2] );

	// the target moves on: the camera lags, then catches up
	target[0] = 200.0f;
	Veh_CamUpdate( &cam, &p.c, target, look, &world, 0.016f );
	d = target[0] - cam.origin[0];
	CHECK( d > p.c.cam_dist + 100.0f, "lags behind a moving target (%.0f)", d );
	for( i = 0; i < 120; i++ )
		Veh_CamUpdate( &cam, &p.c, target, look, &world, 0.016f );
	d = target[0] - cam.origin[0];
	CHECK( fabsf( d - p.c.cam_dist * cosf( p.c.cam_pitch * 3.14159265f / 180.0f )) < 5.0f, "caught up (%.0f)", d );

	// a wall behind pulls the camera in immediately
	memset( &fw, 0, sizeof( fw ));
	fw.planes[0].n[2] = 1.0f;
	fw.planes[1].n[0] = 1.0f;	// solid at x < 100
	fw.planes[1].d = 100.0f;
	fw.count = 2;
	Veh_CamUpdate( &cam, &p.c, target, look, &world, 0.016f );
	CHECK( cam.origin[0] >= 100.0f, "pulled in front of the wall (%.1f)", cam.origin[0] );

	// follow yaw: deltas, wrap, teleport guard
	Veh_CamReset( &cam );
	Veh_CamFollowYaw( &cam, 170.0f );
	cam.primed = 1;
	CHECK( fabsf( Veh_CamFollowYaw( &cam, -175.0f ) - 15.0f ) < 0.01f, "follow wraps" );
	CHECK( Veh_CamFollowYaw( &cam, 0.0f ) == 0.0f, "teleport not followed" );
}

int main( void )
{
	test_names_and_params();
	test_input_map();
	test_angles();
	test_car();
	test_car_wall_and_slope();
	test_tank();
	test_flight();
	test_update_and_determinism();
	test_camera();

	if( failures )
	{
		printf( "%d vehicle check(s) failed\n", failures );
		return 1;
	}
	printf( "vehicle core ok\n" );
	return 0;
}
