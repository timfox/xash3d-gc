/* Host test for the vehicle core (engine/common/vehicle*.c): Glide's car,
 * tank, plane and helicopter on a fake world of infinite half-spaces (solid
 * behind each plane), which is enough to check the suspension, engine and
 * gearbox, grip, walls, slopes, flight and the camera without a BSP. */
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

static float speed_of( const veh_t *v )
{
	return sqrtf( v->velocity[0] * v->velocity[0] + v->velocity[1] * v->velocity[1] + v->velocity[2] * v->velocity[2] );
}

static void driven( veh_input_t *in )
{
	memset( in, 0, sizeof( *in ));
	in->driven = 1;
}

static void spawn( veh_t *v, veh_type_t type, veh_params_t *p, float z, float yaw )
{
	float o[3] = { 0.0f, 0.0f, 0.0f };

	o[2] = z;
	Veh_DefaultParams( type, p );
	Veh_Init( v, type, p, o, yaw );
}

static void test_names_and_params( void )
{
	veh_params_t p;
	const char *const *names;
	const unsigned short *ofs;
	int t, n, i, j;

	CHECK( Veh_TypeForName( "car" ) == VEH_CAR, "car" );
	CHECK( Veh_TypeForName( "TANK" ) == VEH_TANK, "tank" );
	CHECK( Veh_TypeForName( "plane" ) == VEH_PLANE, "plane" );
	CHECK( Veh_TypeForName( "flight" ) == VEH_PLANE, "flight is the plane" );
	CHECK( Veh_TypeForName( "helicopter" ) == VEH_HELI && Veh_TypeForName( "heli" ) == VEH_HELI, "heli" );
	CHECK( Veh_TypeForName( "boat" ) == VEH_NONE, "unknown" );

	for( t = VEH_CAR; t < VEH_TYPE_COUNT; t++ )
	{
		Veh_DefaultParams( (veh_type_t)t, &p );
		n = Veh_ParamTable( (veh_type_t)t, &names, &ofs );
		CHECK( n > 30, "%s has %d params", Veh_TypeName( (veh_type_t)t ), n );
		for( i = 0; i < n; i++ )
		{
			CHECK( ofs[i] + sizeof( float ) <= sizeof( p ), "offset in range" );
			for( j = i + 1; j < n; j++ )
				CHECK( strcmp( names[i], names[j] ), "duplicate param %s", names[i] );
		}
		CHECK( Veh_ParamPtr( (veh_type_t)t, &p, "cam_dist" ) == &p.c.cam_dist, "cam_dist ptr" );
		CHECK( Veh_ParamPtr( (veh_type_t)t, &p, "spring_strength" ) == &p.c.wheel.spring_strength, "wheel params on every type" );
		CHECK( p.c.mass > 0.0f && p.c.inertia[0] > 0.0f && p.c.inertia[1] > 0.0f && p.c.inertia[2] > 0.0f, "body set up" );
	}
	CHECK( Veh_ParamPtr( VEH_CAR, &p, "gear3" ) != NULL, "car gears" );
	CHECK( Veh_ParamPtr( VEH_TANK, &p, "turret_yaw_speed" ) != NULL, "tank turret" );
	CHECK( Veh_ParamPtr( VEH_PLANE, &p, "gear1" ) == NULL, "planes have no gearbox" );
	CHECK( Veh_ParamPtr( VEH_HELI, &p, "upright_force" ) != NULL, "heli upright" );
	Veh_DefaultParams( VEH_CAR, &p );
	*Veh_ParamPtr( VEH_CAR, &p, "max_rpm" ) = 7000.0f;
	CHECK( p.car.e.max_rpm == 7000.0f, "param write lands" );
}

static void test_input_map( void )
{
	veh_input_t in;
	float ang[3] = { 10.0f, 90.0f, 0.0f };

	Veh_MapInput( VEH_CAR, 400.0f, -200.0f, 0.0f, VEH_IN_JUMP, ang, 400.0f, &in );
	CHECK( in.throttle == 1.0f && in.steer == -0.5f, "car axes %.2f %.2f", in.throttle, in.steer );
	CHECK( in.buttons & VEH_BTN_HANDBRAKE, "jump is handbrake" );
	CHECK( in.driven, "a mapped usercmd is a driver" );
	Veh_MapInput( VEH_CAR, 0.0f, 0.0f, 0.0f, VEH_IN_RUN | VEH_IN_DUCK, ang, 400.0f, &in );
	CHECK( in.buttons & VEH_BTN_GENTLE, "walk is the throttle modifier" );
	CHECK( in.buttons & VEH_BTN_BRAKE, "duck is a brake pedal" );
	Veh_MapInput( VEH_TANK, 0.0f, 0.0f, 0.0f, VEH_IN_ATTACK, ang, 400.0f, &in );
	CHECK( in.buttons & VEH_BTN_FIRE, "attack fires" );
	CHECK( in.aim[1] == 90.0f, "aim copied" );
	Veh_MapInput( VEH_PLANE, 0.0f, 0.0f, 0.0f, VEH_IN_DUCK, ang, 400.0f, &in );
	CHECK( in.lift == -1.0f, "duck is throttle down" );
	Veh_MapInput( VEH_HELI, 0.0f, 0.0f, 0.0f, VEH_IN_JUMP | VEH_IN_ATTACK2, ang, 400.0f, &in );
	CHECK( in.lift == 1.0f && ( in.buttons & VEH_BTN_FREELOOK ), "jump is collective up, attack2 free look" );
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

	Veh_DefaultParams( VEH_PLANE, &p );
	for( i = 0; i < sizeof( yaws ) / sizeof( yaws[0] ); i++ )
	{
		Veh_Init( &v, VEH_PLANE, &p, o, yaws[i] );
		Veh_GetAngles( &v, a );
		CHECK( fabsf( Veh_AngleMod( a[1] - yaws[i] )) < 0.01f && fabsf( a[0] ) < 0.01f && fabsf( a[2] ) < 0.01f,
			"yaw %.1f -> %.2f %.2f %.2f", yaws[i], a[0], a[1], a[2] );
	}
	// nose up 0.2 rad
	Veh_Init( &v, VEH_PLANE, &p, o, 0.0f );
	{
		float s = sinf( 0.2f ), c = cosf( 0.2f );
		v.fwd[0] = c; v.fwd[2] = s;
		v.up[0] = -s; v.up[2] = c;
	}
	Veh_GetAngles( &v, a );
	CHECK( fabsf( a[0] - 0.2f * 57.29578f ) < 0.05f, "studio pitch is nose up positive (%.2f)", a[0] );

	// body rates are Glide's: positive yaw rate turns the nose left
	world.gravity = 0.0f;
	Veh_Init( &v, VEH_PLANE, &p, o, 0.0f );
	v.angvel[2] = 30.0f;
	memset( &fw, 0, sizeof( fw ));
	world.trace = fake_trace;
	world.ctx = &fw;
	{
		veh_input_t in;
		memset( &in, 0, sizeof( in ));
		p.c.ang_drag[2] = 0.0f;
		p.plane.yaw_force = 0.0f;
		p.plane.align_force = 0.0f;
		run( &v, &in, 1.0f );
	}
	CHECK( yaw_of( &v ) > 25.0f && yaw_of( &v ) < 31.0f, "yaw rate +30 turns left (%.1f)", yaw_of( &v ));
	world.gravity = 800.0f;
}

static void test_car( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	unsigned ev = 0, shifts = 0;
	float rest_z, y0, slip_grip, slip_hand;
	int i, top_gear = 0;

	world_flat();
	spawn( &v, VEH_CAR, &p, 40.0f, 0.0f );
	driven( &in );
	run( &v, &in, 2.0f );
	rest_z = v.origin[2];
	CHECK( v.wheels_down == 4, "settled on 4 wheels (%d)", v.wheels_down );
	CHECK( speed_of( &v ) < 2.0f, "at rest %.2f", speed_of( &v ));
	CHECK( rest_z + p.c.hull_mins[2] > 0.0f, "hull rides above the ground (%.2f)", rest_z + p.c.hull_mins[2] );
	CHECK( v.wheels[0].last_offset > 1.0f && v.wheels[0].last_offset < p.c.wheel.susp_length, "spring part compressed (%.2f)", v.wheels[0].last_offset );
	CHECK( Veh_TakeEvents( &v ) & VEH_EV_LANDED, "landing event" );
	CHECK( v.engine_on && Veh_EngineRPM( &v ) < p.car.e.min_rpm * 1.3f, "idling (%.0f rpm)", Veh_EngineRPM( &v ));

	// full throttle: the automatic box climbs through the gears
	in.throttle = 1.0f;
	for( i = 0; i < 60 * 12; i++ )
	{
		Veh_Step( &v, &in, &world );
		ev = Veh_TakeEvents( &v );
		if( ev & VEH_EV_SHIFT ) shifts++;
		if( v.gear > top_gear ) top_gear = v.gear;
		CHECK( Veh_EngineRPM( &v ) <= p.car.e.max_rpm * 1.1f, "rpm %.0f past the limiter", Veh_EngineRPM( &v ));
	}
	CHECK( top_gear >= 3 && shifts >= 3, "shifted up to gear %d (%u shifts)", top_gear, shifts );
	CHECK( Veh_ForwardSpeed( &v ) > 1200.0f, "accelerates to %.1f", Veh_ForwardSpeed( &v ));
	CHECK( fabsf( v.velocity[1] ) < 1.0f && fabsf( yaw_of( &v )) < 0.1f, "straight" );
	CHECK( fabsf( v.origin[2] - rest_z ) < 3.0f, "ride height stable at speed %.2f vs %.2f", v.origin[2], rest_z );

	// steering right turns clockwise
	y0 = yaw_of( &v );
	in.steer = 1.0f;
	run( &v, &in, 0.5f );
	CHECK( Veh_AngleMod( yaw_of( &v ) - y0 ) < -5.0f, "steer right turns clockwise (%.1f)", yaw_of( &v ));
	CHECK( v.wheels_down == 4, "stays on its wheels in a fast turn" );

	// brake to a stop, then the gearbox goes to reverse
	in.steer = 0.0f;
	in.throttle = -1.0f;
	run( &v, &in, 6.0f );
	CHECK( v.gear == -1 && Veh_ForwardSpeed( &v ) < -100.0f, "reversing (gear %d, %.1f)", v.gear, Veh_ForwardSpeed( &v ));

	// in reverse the pedals swap: forward brakes the backward roll
	{
		float back = Veh_ForwardSpeed( &v );

		in.throttle = 1.0f;
		run( &v, &in, 1.0f );
		CHECK( Veh_ForwardSpeed( &v ) > back * 0.5f, "forward brakes a reverse roll (%.1f from %.1f)", Veh_ForwardSpeed( &v ), back );
	}

	// pull away gently and let go: the idle brake in first holds it
	Veh_Init( &v, VEH_CAR, &p, v.origin, 0.0f );
	run( &v, &in, 0.4f );
	in.throttle = 0.0f;
	run( &v, &in, 5.0f );
	CHECK( speed_of( &v ) < 3.0f && v.gear == 1, "comes to rest in first (%.2f, gear %d)", speed_of( &v ), v.gear );

	// the handbrake locks the rear wheels and slides the tail out
	Veh_Init( &v, VEH_CAR, &p, v.origin, 0.0f );
	in.throttle = 1.0f;
	run( &v, &in, 6.0f );
	in.steer = 0.6f;
	run( &v, &in, 0.6f );
	slip_grip = fabsf( v.avg_side_slip );
	Veh_Init( &v, VEH_CAR, &p, v.origin, 0.0f );
	in.steer = 0.0f;
	run( &v, &in, 6.0f );
	in.steer = 0.6f;
	in.buttons = VEH_BTN_HANDBRAKE;
	run( &v, &in, 0.6f );
	slip_hand = fabsf( v.avg_side_slip );
	CHECK( fabsf( v.wheels[2].ang_vel ) < fabsf( v.wheels[0].ang_vel ) * 0.5f, "rear wheels locked (%.1f vs %.1f front)", v.wheels[2].ang_vel, v.wheels[0].ang_vel );
	CHECK( slip_hand > slip_grip * 1.5f && slip_hand > 0.1f, "handbrake slides (%.3f vs %.3f grip)", slip_hand, slip_grip );
	in.buttons = 0;

	// burnout: throttle and brake together at rest spin the wheels in place,
	// and steering swings the tail round
	Veh_Init( &v, VEH_CAR, &p, v.origin, 0.0f );
	in.throttle = 0.0f;
	in.steer = 0.0f;
	run( &v, &in, 2.0f );
	{
		float x0 = v.origin[0];

		in.throttle = 1.0f;
		in.buttons = VEH_BTN_BRAKE;
		run( &v, &in, 1.5f );
		CHECK( v.burnout > 0.9f, "burnout (%.2f)", v.burnout );
		CHECK( fabsf( v.wheels[2].ang_vel ) * p.c.wheel.radius > 200.0f, "rear wheels spinning (%.0f u/s)", v.wheels[2].ang_vel * p.c.wheel.radius );
		CHECK( fabsf( v.origin[0] - x0 ) < 60.0f, "held in place (%.1f)", v.origin[0] - x0 );
		in.steer = 1.0f;
		run( &v, &in, 1.0f );
		CHECK( yaw_of( &v ) < -0.5f, "steering right swings it round (%.1f)", yaw_of( &v ));
		in.buttons = 0;
		in.steer = 0.0f;
	}

	// with nobody in it the engine stops and the brakes hold it
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 6.0f );
	CHECK( !v.engine_on && speed_of( &v ) < 3.0f, "parked (engine %d, %.2f)", v.engine_on, speed_of( &v ));
}

static void test_car_wall_slope_air( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	unsigned ev = 0;
	float max_x = -1e9f, a[3];
	int i;

	world_flat();
	world_add_wall_x( 1500.0f );
	spawn( &v, VEH_CAR, &p, 30.0f, 0.0f );
	driven( &in );
	run( &v, &in, 1.0f );
	in.throttle = 1.0f;
	for( i = 0; i < 60 * 8; i++ )
	{
		Veh_Step( &v, &in, &world );
		ev |= Veh_TakeEvents( &v );
		if( v.origin[0] > max_x ) max_x = v.origin[0];
	}
	CHECK( max_x + p.c.hull_maxs[0] <= 1500.0f + 0.1f, "never through the wall (%.2f)", max_x + p.c.hull_maxs[0] );
	CHECK( ev & VEH_EV_IMPACT, "wall impact event" );
	CHECK( v.velocity[0] < 50.0f, "stopped by the wall (%.1f)", v.velocity[0] );

	// a 12 degree ramp: the body pitches up and every wheel stays down
	world_slope( 12.0f );
	spawn( &v, VEH_CAR, &p, 40.0f, 0.0f );
	driven( &in );
	run( &v, &in, 2.0f );
	Veh_GetAngles( &v, a );
	CHECK( fabsf( a[0] - 12.0f ) < 1.5f, "nose up on the slope (%.2f)", a[0] );
	CHECK( v.wheels_down == 4, "all wheels on the slope (%d)", v.wheels_down );
	in.throttle = 1.0f;
	run( &v, &in, 3.0f );
	CHECK( Veh_ForwardSpeed( &v ) > 300.0f, "drives up the slope (%.0f)", Veh_ForwardSpeed( &v ));

	// airborne: steering rolls it (UpdateAirControls)
	world_flat();
	spawn( &v, VEH_CAR, &p, 3000.0f, 0.0f );
	v.velocity[0] = 600.0f;
	driven( &in );
	in.steer = 1.0f;
	run( &v, &in, 0.3f );
	CHECK( v.angvel[0] > 30.0f && v.angvel[0] <= p.car.e.air_max_angvel[0] + 5.0f, "air roll %.1f deg/s", v.angvel[0] );

	// on its roof: steering at rest unflips it
	spawn( &v, VEH_CAR, &p, 60.0f, 0.0f );
	v.up[2] = -1.0f;
	v.left[1] = -1.0f;
	driven( &in );
	run( &v, &in, 1.0f );
	in.steer = 1.0f;
	run( &v, &in, 2.5f );
	in.steer = 0.0f;
	run( &v, &in, 3.0f );
	Veh_GetAngles( &v, a );
	CHECK( fabsf( a[2] ) < 10.0f && v.wheels_down == 4, "unflipped (roll %.1f, wheels %d)", a[2], v.wheels_down );
}

static void test_tank( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float org[3], dir[3], kick_v;
	int i, fires = 0;

	world_flat();
	spawn( &v, VEH_TANK, &p, 40.0f, 0.0f );
	driven( &in );
	run( &v, &in, 2.0f );
	CHECK( v.wheels_down == v.num_wheels && v.num_wheels == 4, "settled on its %d track wheels (%d)", v.num_wheels, v.wheels_down );
	CHECK( speed_of( &v ) < 2.0f, "tank at rest %.2f", speed_of( &v ));

	// turn in place: steering alone at rest pivots the hull
	in.steer = -1.0f;
	run( &v, &in, 2.0f );
	CHECK( v.turning_in_place, "turning in place" );
	CHECK( yaw_of( &v ) > 20.0f, "pivots left at rest (%.1f)", yaw_of( &v ));
	CHECK( sqrtf( v.origin[0] * v.origin[0] + v.origin[1] * v.origin[1] ) < 40.0f, "pivot stays put (%.1f, %.1f)", v.origin[0], v.origin[1] );

	// drive
	in.steer = 0.0f;
	in.throttle = 1.0f;
	run( &v, &in, 6.0f );
	CHECK( !v.turning_in_place && Veh_ForwardSpeed( &v ) > 300.0f, "tank drives at %.1f", Veh_ForwardSpeed( &v ));
	CHECK( fabsf( v.avg_side_slip ) < 0.05f, "tracks do not slide (%.3f)", v.avg_side_slip );

	// turret: slews toward the aim at its max speed, pitch is limited
	in.throttle = 0.0f;
	in.aim[0] = -45.0f;	// look up past the barrel's limit
	in.aim[1] = yaw_of( &v ) + 90.0f;
	run( &v, &in, 0.5f );
	{
		float t0 = v.turret_yaw;
		run( &v, &in, 1.0f );
		CHECK( Veh_AngleMod( v.turret_yaw - t0 ) <= p.tank.turret_yaw_speed + 0.5f && Veh_AngleMod( v.turret_yaw - t0 ) > p.tank.turret_yaw_speed * 0.8f,
			"turret slewed one second's worth (%.1f)", v.turret_yaw - t0 );
	}
	run( &v, &in, 3.0f );
	CHECK( fabsf( Veh_AngleMod( v.turret_yaw + yaw_of( &v ) - in.aim[1] )) < 2.0f, "turret on target (%.1f)", v.turret_yaw );
	CHECK( fabsf( v.turret_pitch - p.tank.turret_pitch_up ) < 0.5f, "pitch clamped (%.1f)", v.turret_pitch );

	// gun: one shot per fire interval, with recoil
	run( &v, &in, 3.0f );
	in.buttons = VEH_BTN_FIRE;
	kick_v = 0.0f;
	for( i = 0; i < 6 * VEH_STEP_HZ; i++ )
	{
		float before[3];
		memcpy( before, v.velocity, sizeof( before ));
		Veh_Step( &v, &in, &world );
		if( Veh_TakeEvents( &v ) & VEH_EV_FIRE )
		{
			float d = ( v.velocity[0] - before[0] ) * dir[0] + ( v.velocity[1] - before[1] ) * dir[1];
			fires++;
			Veh_TurretMuzzle( &v, org, dir );
			d = ( v.velocity[0] - before[0] ) * dir[0] + ( v.velocity[1] - before[1] ) * dir[1];
			if( d < kick_v ) kick_v = d;
		}
	}
	CHECK( fires == 3, "fires every %.1f s: %d shots in 6 s", p.tank.fire_interval, fires );
	CHECK( kick_v < -5.0f, "recoil kicks the hull back (%.1f)", kick_v );
	Veh_TurretMuzzle( &v, org, dir );
	CHECK( fabsf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] - 1.0f ) < 1e-3f, "barrel dir unit" );
	CHECK( org[2] > v.origin[2], "muzzle above the hull" );
}

static void test_plane( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float a[3], z0;
	unsigned ev = 0;
	int i, airborne = 0;

	// take off: full throttle down the runway, then aim up
	world_flat();
	spawn( &v, VEH_PLANE, &p, 60.0f, 0.0f );
	driven( &in );
	run( &v, &in, 1.0f );
	CHECK( v.wheels_down == 3, "sits on its gear (%d)", v.wheels_down );
	Veh_GetAngles( &v, a );
	CHECK( a[0] > 3.0f, "tail dragger sits nose up (%.1f)", a[0] );
	Veh_TakeEvents( &v );
	in.throttle = 1.0f;
	for( i = 0; i < 60 * 40 && !airborne; i++ )
	{
		in.aim[0] = Veh_ForwardSpeed( &v ) > p.plane.lift_speed * 0.8f ? -15.0f : 0.0f;
		Veh_Step( &v, &in, &world );
		ev |= Veh_TakeEvents( &v );
		airborne = v.origin[2] > 400.0f;
	}
	CHECK( airborne, "takes off within 40 s (z %.0f, speed %.0f, power %.2f)", v.origin[2], Veh_ForwardSpeed( &v ), v.power );
	CHECK( ev & VEH_EV_TAKEOFF, "takeoff event" );
	CHECK( v.power > 1.5f, "engine spooled up (%.2f)", v.power );

	// level off and hold height with the throttle let go; like Glide, level
	// flight wants the nose a few degrees above the horizon
	in.throttle = 0.0f;
	in.aim[0] = -5.0f;
	run( &v, &in, 4.0f );
	z0 = v.origin[2];
	run( &v, &in, 6.0f );
	CHECK( fabsf( v.origin[2] - z0 ) < 600.0f, "holds altitude on the horizon (%.0f)", v.origin[2] - z0 );
	CHECK( Veh_ForwardSpeed( &v ) > p.plane.control_speed, "keeps flying (%.0f)", Veh_ForwardSpeed( &v ));

	// mouse aim: look 90 degrees right and the plane comes round
	in.aim[1] = -90.0f;
	run( &v, &in, 12.0f );
	Veh_GetAngles( &v, a );
	CHECK( fabsf( Veh_AngleMod( a[1] + 90.0f )) < 15.0f, "turned onto the aim (%.1f)", a[1] );

	// roll: strafe banks it
	in.steer = 1.0f;
	run( &v, &in, 0.5f );
	CHECK( v.angvel[0] > 40.0f, "rolls right (%.1f deg/s)", v.angvel[0] );
	in.steer = 0.0f;

	// crash: dive into the ground
	spawn( &v, VEH_PLANE, &p, 600.0f, 0.0f );
	v.velocity[0] = 400.0f;
	v.velocity[2] = -1400.0f;
	driven( &in );
	ev = 0;
	{
		float min_z = 1e9f;
		for( i = 0; i < 120; i++ )
		{
			Veh_Step( &v, &in, &world );
			ev |= Veh_TakeEvents( &v );
			if( v.origin[2] < min_z ) min_z = v.origin[2];
		}
		CHECK( ev & VEH_EV_CRASH, "crash event" );
		CHECK( min_z + p.c.hull_mins[2] >= -0.1f, "never through the floor (%.2f)", min_z + p.c.hull_mins[2] );
	}
}

static void test_heli( void )
{
	veh_params_t p;
	veh_t v;
	veh_input_t in;
	float a[3];

	world_flat();
	spawn( &v, VEH_HELI, &p, 40.0f, 0.0f );
	driven( &in );
	run( &v, &in, 6.0f );
	CHECK( v.wheels_down == 4 && v.power > 0.7f, "on its skids, rotor up to speed (%d, %.2f)", v.wheels_down, v.power );

	// collective up, then let go: it climbs, then hovers
	in.throttle = 1.0f;
	run( &v, &in, 4.0f );
	CHECK( v.origin[2] > 400.0f, "climbs (%.0f)", v.origin[2] );
	in.throttle = 0.0f;
	run( &v, &in, 6.0f );
	CHECK( fabsf( v.velocity[2] ) < 30.0f, "hovers (%.1f u/s)", v.velocity[2] );

	// look down ahead: the nose dips and it flies forward
	in.aim[0] = 30.0f;
	run( &v, &in, 4.0f );
	CHECK( Veh_ForwardSpeed( &v ) > 150.0f, "flies forward (%.0f)", Veh_ForwardSpeed( &v ));
	Veh_GetAngles( &v, a );

	// look left: the tail rotor yaws it round
	in.aim[0] = 0.0f;
	in.aim[1] = 90.0f;
	run( &v, &in, 5.0f );
	CHECK( fabsf( Veh_AngleMod( yaw_of( &v ) - 90.0f )) < 15.0f, "yawed to the aim (%.1f)", yaw_of( &v ));
	Veh_GetAngles( &v, a );
	CHECK( fabsf( a[2] ) < 25.0f, "stays upright (roll %.1f)", a[2] );

	// pilot out: the rotor winds down and it comes down
	memset( &in, 0, sizeof( in ));
	run( &v, &in, 30.0f );
	CHECK( v.wheels_down > 0 && v.power < 0.3f, "back on the ground (%d, %.2f)", v.wheels_down, v.power );
}

static void test_update_and_determinism( void )
{
	veh_params_t p;
	veh_t a, b;
	veh_input_t in;
	float o[3] = { 0.0f, 0.0f, 30.0f };
	int t, i;

	world_flat();
	Veh_DefaultParams( VEH_CAR, &p );
	Veh_Init( &a, VEH_CAR, &p, o, 10.0f );
	memset( &in, 0, sizeof( in ));
	CHECK( Veh_Update( &a, &in, &world, 0.05f ) == 3, "50 ms is 3 steps" );
	CHECK( Veh_Update( &a, &in, &world, 1.0f ) == VEH_MAX_SUBSTEPS, "hitch capped" );
	CHECK( a.accum == 0.0f, "hitch remainder dropped" );

	for( t = VEH_CAR; t < VEH_TYPE_COUNT; t++ )
	{
		// same inputs, same result, whatever the cmd split
		Veh_DefaultParams( (veh_type_t)t, &p );
		Veh_Init( &a, (veh_type_t)t, &p, o, 10.0f );
		Veh_Init( &b, (veh_type_t)t, &p, o, 10.0f );
		driven( &in );
		in.throttle = 1.0f;
		in.steer = 0.4f;
		for( i = 0; i < 120; i++ )
			Veh_Update( &a, &in, &world, 0.01f );
		for( i = 0; i < 40; i++ )
			Veh_Update( &b, &in, &world, 0.03f );
		b.accum = a.accum;	// the split leaves a different remainder
		CHECK( a.steps == b.steps && !memcmp( &a, &b, sizeof( a )), "%s deterministic across cmd splits (%u/%u)", Veh_TypeName( (veh_type_t)t ), a.steps, b.steps );

		// trace budget: wheels, an altitude probe for aircraft, a short slide
		fw.traces = 0;
		Veh_Step( &a, &in, &world );
		CHECK( fw.traces <= a.num_wheels + 1 + 4, "%s step traces %d", Veh_TypeName( (veh_type_t)t ), fw.traces );
	}
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
	test_car_wall_slope_air();
	test_tank();
	test_plane();
	test_heli();
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
