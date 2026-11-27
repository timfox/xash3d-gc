/*
vehicle.c - shared vehicle core: types, tunables, input, fixed step, basis,
ground probe, hull slide move and the chase camera

See vehicle.h for the budget rules. Nothing here allocates or touches engine
state; the bindings pass a veh_world_t with a trace callback.
*/
#include <stddef.h>
#include <string.h>
#include "vehicle_local.h"

#if XASH_VEHICLES

/*
==============================================================================
types and tunables
==============================================================================
*/
static const char *const veh_type_names[VEH_TYPE_COUNT] = { "none", "car", "flight", "tank" };

const char *Veh_TypeName( veh_type_t type )
{
	if( type <= VEH_NONE || type >= VEH_TYPE_COUNT )
		return "none";
	return veh_type_names[type];
}

static int veh_streq( const char *a, const char *b )
{
	while( *a && *b )
	{
		char ca = *a++, cb = *b++;

		if( ca >= 'A' && ca <= 'Z' ) ca += 'a' - 'A';
		if( cb >= 'A' && cb <= 'Z' ) cb += 'a' - 'A';
		if( ca != cb )
			return 0;
	}
	return *a == *b;
}

veh_type_t Veh_TypeForName( const char *name )
{
	int i;

	if( !name )
		return VEH_NONE;
	for( i = VEH_CAR; i < VEH_TYPE_COUNT; i++ )
	{
		if( veh_streq( name, veh_type_names[i] ))
			return (veh_type_t)i;
	}
	if( veh_streq( name, "plane" ) || veh_streq( name, "jet" ) || veh_streq( name, "fly" ))
		return VEH_FLIGHT;
	return VEH_NONE;
}

void Veh_DefaultParams( veh_type_t type, veh_params_t *out )
{
	memset( out, 0, sizeof( *out ));
	switch( type )
	{
	case VEH_CAR: Veh_CarDefaults( &out->car ); break;
	case VEH_TANK: Veh_TankDefaults( &out->tank ); break;
	case VEH_FLIGHT: Veh_FlightDefaults( &out->flight ); break;
	default: break;
	}
}

#define VP_C( f )	#f, offsetof( veh_params_t, c.f )
#define VP_G( b, f )	#f, offsetof( veh_params_t, b.g.f )
#define VP_T( b, f )	#f, offsetof( veh_params_t, b.f )

typedef struct
{
	const char	*name;
	unsigned short	ofs;
} veh_param_def_t;

#define VEH_COMMON_DEFS \
	{ VP_C( gravity_scale ) }, { VP_C( bounce ) }, { VP_C( impact_speed ) }, \
	{ VP_C( cam_dist ) }, { VP_C( cam_height ) }, { VP_C( cam_pitch ) }, \
	{ VP_C( cam_lag ) }, { VP_C( cam_follow ) }

#define VEH_GROUND_DEFS( b ) \
	{ VP_G( b, wheel_x ) }, { VP_G( b, wheel_y ) }, { VP_G( b, wheel_z ) }, \
	{ VP_G( b, wheel_radius ) }, { VP_G( b, susp_length ) }, { VP_G( b, susp_stiffness ) }, \
	{ VP_G( b, susp_damping ) }, { VP_G( b, align_rate ) }, { VP_G( b, accel ) }, \
	{ VP_G( b, max_speed ) }, { VP_G( b, reverse_accel ) }, { VP_G( b, reverse_speed ) }, \
	{ VP_G( b, brake ) }, { VP_G( b, roll_drag ) }, { VP_G( b, boost_scale ) }, \
	{ VP_G( b, grip ) }, { VP_G( b, steer_max ) }, { VP_G( b, steer_rate ) }, \
	{ VP_G( b, steer_falloff ) }, { VP_G( b, wheelbase ) }, { VP_G( b, air_control ) }

static const veh_param_def_t veh_car_defs[] =
{
	VEH_COMMON_DEFS, VEH_GROUND_DEFS( car ),
	{ VP_T( car, drift_grip ) }, { VP_T( car, drift_min_speed ) }, { VP_T( car, drift_in_rate ) },
	{ VP_T( car, drift_out_rate ) }, { VP_T( car, drift_yaw_boost ) }, { VP_T( car, handbrake_decel ) },
};

static const veh_param_def_t veh_tank_defs[] =
{
	VEH_COMMON_DEFS, VEH_GROUND_DEFS( tank ),
	{ VP_T( tank, pivot_speed_falloff ) }, { VP_T( tank, reverse_steer_invert ) },
	{ VP_T( tank, turret_yaw_rate ) }, { VP_T( tank, turret_pitch_rate ) },
	{ VP_T( tank, turret_pitch_min ) }, { VP_T( tank, turret_pitch_max ) },
	{ VP_T( tank, turret_height ) }, { VP_T( tank, barrel_length ) },
	{ VP_T( tank, fire_interval ) }, { VP_T( tank, recoil ) },
};

static const veh_param_def_t veh_flight_defs[] =
{
	VEH_COMMON_DEFS,
	{ VP_T( flight, max_thrust ) }, { VP_T( flight, throttle_rate ) }, { VP_T( flight, drag ) },
	{ VP_T( flight, side_drag ) }, { VP_T( flight, cruise_speed ) }, { VP_T( flight, stall_speed ) },
	{ VP_T( flight, max_speed ) }, { VP_T( flight, pitch_rate ) }, { VP_T( flight, roll_rate ) },
	{ VP_T( flight, yaw_rate ) }, { VP_T( flight, rate_response ) }, { VP_T( flight, control_speed ) },
	{ VP_T( flight, mouse_aim ) }, { VP_T( flight, aim_gain ) }, { VP_T( flight, bank_max ) },
	{ VP_T( flight, rotate_speed ) }, { VP_T( flight, ground_friction ) }, { VP_T( flight, crash_speed ) },
};

#define VEH_DEFS_COUNT( a )	( (int)( sizeof( a ) / sizeof( a[0] )))

/* The name/offset views handed out by Veh_ParamTable. Built from the def
 * tables on first use; static, so no allocation and stable pointers. */
#define VEH_MAX_PARAMS	48
static const char *veh_param_names[VEH_TYPE_COUNT][VEH_MAX_PARAMS];
static unsigned short veh_param_ofs[VEH_TYPE_COUNT][VEH_MAX_PARAMS];
static int veh_param_count[VEH_TYPE_COUNT];

static const veh_param_def_t *veh_defs_for( veh_type_t type, int *count )
{
	switch( type )
	{
	case VEH_CAR: *count = VEH_DEFS_COUNT( veh_car_defs ); return veh_car_defs;
	case VEH_TANK: *count = VEH_DEFS_COUNT( veh_tank_defs ); return veh_tank_defs;
	case VEH_FLIGHT: *count = VEH_DEFS_COUNT( veh_flight_defs ); return veh_flight_defs;
	default: *count = 0; return NULL;
	}
}

int Veh_ParamTable( veh_type_t type, const char *const **names, const unsigned short **offsets )
{
	const veh_param_def_t *defs;
	int count, i;

	if( type <= VEH_NONE || type >= VEH_TYPE_COUNT )
		return 0;

	if( !veh_param_count[type] )
	{
		defs = veh_defs_for( type, &count );
		if( count > VEH_MAX_PARAMS )
			count = VEH_MAX_PARAMS;
		for( i = 0; i < count; i++ )
		{
			veh_param_names[type][i] = defs[i].name;
			veh_param_ofs[type][i] = defs[i].ofs;
		}
		veh_param_count[type] = count;
	}

	if( names ) *names = veh_param_names[type];
	if( offsets ) *offsets = veh_param_ofs[type];
	return veh_param_count[type];
}

float *Veh_ParamPtr( veh_type_t type, veh_params_t *params, const char *name )
{
	const char *const *names;
	const unsigned short *ofs;
	int i, count = Veh_ParamTable( type, &names, &ofs );

	for( i = 0; i < count; i++ )
	{
		if( veh_streq( names[i], name ))
			return (float *)((unsigned char *)params + ofs[i] );
	}
	return NULL;
}

/*
==============================================================================
small helpers
==============================================================================
*/
float Veh_AngleMod( float a )
{
	while( a > 180.0f ) a -= 360.0f;
	while( a < -180.0f ) a += 360.0f;
	return a;
}

float Veh_SmoothFactor( float rate, float dt )
{
	// 1 - e^-x with e^x ~ 1 + x + x^2/2: monotonic, 0..1, no expf
	float x = rate * dt;

	if( x <= 0.0f )
		return 0.0f;
	return 1.0f - 1.0f / ( 1.0f + x + 0.5f * x * x );
}

/*
==============================================================================
body basis
==============================================================================
*/
void veh_rotate_yaw_world( veh_t *v, float a )
{
	float *axes[3] = { v->fwd, v->left, v->up };
	int i;

	for( i = 0; i < 3; i++ )
	{
		float x = axes[i][0], y = axes[i][1];

		axes[i][0] = x - y * a;
		axes[i][1] = y + x * a;
	}
}

void veh_rotate_body( veh_t *v, float pitch, float roll, float yaw )
{
	float f[3], l[3], u[3];

	veh_copy( v->fwd, f );
	veh_copy( v->left, l );
	veh_copy( v->up, u );

	// nose up: forward toward up
	veh_ma( v->fwd, pitch, u, v->fwd );
	veh_ma( v->up, -pitch, f, v->up );
	// right wing down: left wing (left axis) toward up
	veh_ma( v->left, roll, u, v->left );
	veh_ma( v->up, -roll, l, v->up );
	// nose left: forward toward left
	veh_ma( v->fwd, yaw, l, v->fwd );
	veh_ma( v->left, -yaw, f, v->left );
}

void veh_orthonormalize( veh_t *v )
{
	if( veh_normalize( v->fwd ) < 1e-6f )
		veh_set( v->fwd, 1.0f, 0.0f, 0.0f );
	veh_ma( v->left, -veh_dot( v->left, v->fwd ), v->fwd, v->left );
	if( veh_normalize( v->left ) < 1e-6f )
	{
		// left collapsed onto forward: rebuild from world up
		float wup[3] = { 0.0f, 0.0f, 1.0f };
		veh_cross( wup, v->fwd, v->left );
		if( veh_normalize( v->left ) < 1e-6f )
			veh_set( v->left, 0.0f, 1.0f, 0.0f );
	}
	veh_cross( v->fwd, v->left, v->up );
}

void veh_align_up( veh_t *v, const float target_up[3], float rate, float dt )
{
	float k = Veh_SmoothFactor( rate, dt );
	float up[3];

	up[0] = v->up[0] + ( target_up[0] - v->up[0] ) * k;
	up[1] = v->up[1] + ( target_up[1] - v->up[1] ) * k;
	up[2] = v->up[2] + ( target_up[2] - v->up[2] ) * k;
	if( veh_normalize( up ) < 1e-6f )
		return;

	// keep the heading: project forward onto the new ground plane
	veh_ma( v->fwd, -veh_dot( v->fwd, up ), up, v->fwd );
	if( veh_normalize( v->fwd ) < 1e-6f )
		return;
	veh_copy( up, v->up );
	veh_cross( v->up, v->fwd, v->left );
}

void Veh_GetAngles( const veh_t *v, float angles[3] )
{
	float fz = veh_clamp( v->fwd[2], -1.0f, 1.0f );

	angles[0] = asinf( fz ) * VEH_RAD2DEG;	// studio pitch: nose up positive
	angles[1] = veh_yaw_of( v->fwd );
	angles[2] = atan2f( v->left[2], v->up[2] ) * VEH_RAD2DEG;
}

void Veh_LocalToWorld( const veh_t *v, const float local[3], float out[3] )
{
	float o[3];

	veh_ma( v->origin, local[0], v->fwd, o );
	veh_ma( o, local[1], v->left, o );
	veh_ma( o, local[2], v->up, out );
}

float Veh_ForwardSpeed( const veh_t *v )
{
	return veh_dot( v->velocity, v->fwd );
}

/*
==============================================================================
setup, input, stepping
==============================================================================
*/
void Veh_Init( veh_t *v, veh_type_t type, const veh_params_t *params, const float origin[3], float yaw )
{
	float s = sinf( yaw * VEH_DEG2RAD ), c = cosf( yaw * VEH_DEG2RAD );

	memset( v, 0, sizeof( *v ));
	v->type = type;
	v->p = params;
	veh_copy( origin, v->origin );
	veh_set( v->fwd, c, s, 0.0f );
	veh_set( v->left, -s, c, 0.0f );
	veh_set( v->up, 0.0f, 0.0f, 1.0f );
	veh_set( v->ground_normal, 0.0f, 0.0f, 1.0f );

	if( type == VEH_CAR )
		Veh_GroundInitWheels( v, &params->car.g );
	else if( type == VEH_TANK )
		Veh_GroundInitWheels( v, &params->tank.g );
	else if( type == VEH_FLIGHT )
		v->throttle_lever = 0.0f;
}

void Veh_MapInput( veh_type_t type, float forwardmove, float sidemove, float upmove, unsigned in_buttons, const float viewangles[3], float scale, veh_input_t *out )
{
	float inv = scale > 1.0f ? 1.0f / scale : 1.0f / 400.0f;

	memset( out, 0, sizeof( *out ));
	out->throttle = veh_clamp( forwardmove * inv, -1.0f, 1.0f );
	out->steer = veh_clamp( sidemove * inv, -1.0f, 1.0f );
	out->lift = veh_clamp( upmove * inv, -1.0f, 1.0f );
	veh_copy( viewangles, out->aim );

	switch( type )
	{
	case VEH_CAR:
		if( in_buttons & VEH_IN_JUMP ) out->buttons |= VEH_BTN_HANDBRAKE;
		if( in_buttons & ( VEH_IN_RUN|VEH_IN_ATTACK2 )) out->buttons |= VEH_BTN_BOOST;
		break;
	case VEH_TANK:
		if( in_buttons & VEH_IN_ATTACK ) out->buttons |= VEH_BTN_FIRE;
		if( in_buttons & VEH_IN_ATTACK2 ) out->buttons |= VEH_BTN_ALTFIRE;
		if( in_buttons & VEH_IN_JUMP ) out->buttons |= VEH_BTN_BRAKE;
		break;
	case VEH_FLIGHT:
		// jump/duck work the throttle lever on keyboards and pads without an up axis
		if( in_buttons & VEH_IN_JUMP ) out->lift = 1.0f;
		if( in_buttons & VEH_IN_DUCK ) out->lift = -1.0f;
		if( in_buttons & VEH_IN_ATTACK ) out->buttons |= VEH_BTN_FIRE;
		if( in_buttons & VEH_IN_ATTACK2 ) out->buttons |= VEH_BTN_ALTFIRE;
		if( in_buttons & VEH_IN_RUN ) out->buttons |= VEH_BTN_BOOST;
		break;
	default:
		break;
	}
}

void Veh_Step( veh_t *v, const veh_input_t *in, const veh_world_t *world )
{
	switch( v->type )
	{
	case VEH_CAR: Veh_CarStep( v, in, world ); break;
	case VEH_TANK: Veh_TankStep( v, in, world ); break;
	case VEH_FLIGHT: Veh_FlightStep( v, in, world ); break;
	default: return;
	}
	v->steps++;
}

int Veh_Update( veh_t *v, const veh_input_t *in, const veh_world_t *world, float dt )
{
	int steps = 0;

	if( !v->p || dt <= 0.0f )
		return 0;

	// the epsilon keeps a 50 ms cmd at exactly 3 steps despite float rounding
	v->accum += dt;
	while( v->accum >= VEH_STEP_DT - 1e-5f && steps < VEH_MAX_SUBSTEPS )
	{
		Veh_Step( v, in, world );
		v->accum -= VEH_STEP_DT;
		steps++;
	}
	// a hitch longer than the substep cap is dropped, not queued
	if( v->accum >= VEH_STEP_DT - 1e-5f )
		v->accum = 0.0f;
	return steps;
}

unsigned Veh_TakeEvents( veh_t *v )
{
	unsigned ev = v->events;

	v->events = 0;
	return ev;
}

/*
==============================================================================
ground vehicles: raycast wheels
==============================================================================
*/
void Veh_GroundInitWheels( veh_t *v, const veh_ground_params_t *g )
{
	static const float sx[VEH_MAX_WHEELS] = { 1.0f, 1.0f, -1.0f, -1.0f };
	static const float sy[VEH_MAX_WHEELS] = { 1.0f, -1.0f, 1.0f, -1.0f };
	int i;

	v->num_wheels = VEH_MAX_WHEELS;
	for( i = 0; i < VEH_MAX_WHEELS; i++ )
	{
		veh_set( v->wheels[i].mount, sx[i] * g->wheel_x, sy[i] * g->wheel_y, g->wheel_z );
		v->wheels[i].compress = v->wheels[i].compress_prev = 0.0f;
		v->wheels[i].contact = 0;
	}
}

int veh_ground_probe( veh_t *v, const veh_ground_params_t *g, const veh_world_t *world, float dt )
{
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	float reach = g->susp_length + g->wheel_radius;
	float normal_sum[3] = { 0.0f, 0.0f, 0.0f };
	float accel = 0.0f;
	int i, down = 0;

	for( i = 0; i < v->num_wheels; i++ )
	{
		veh_wheel_t *w = &v->wheels[i];
		float start[3], end[3];
		veh_trace_t tr;

		w->compress_prev = w->compress;
		Veh_LocalToWorld( v, w->mount, start );
		veh_ma( start, -reach, v->up, end );
		world->trace( world->ctx, start, zero, zero, end, &tr );

		if( tr.fraction < 1.0f && !tr.allsolid && tr.normal[2] > 0.3f )
		{
			float dist = tr.fraction * reach;
			float speed;

			w->compress = veh_clamp(( reach - dist ) / g->susp_length, 0.0f, 1.0f );
			w->contact = 1;
			veh_copy( tr.normal, w->normal );
			normal_sum[0] += tr.normal[0];
			normal_sum[1] += tr.normal[1];
			normal_sum[2] += tr.normal[2];

			// spring pushes, damper resists the compression speed
			speed = ( w->compress - w->compress_prev ) * g->susp_length / dt;
			accel += veh_clamp( g->susp_stiffness * w->compress + g->susp_damping * speed, 0.0f, g->susp_stiffness * 2.0f );
			down++;
		}
		else
		{
			w->compress = 0.0f;
			w->contact = 0;
		}
	}

	v->wheels_down = down;
	if( down )
	{
		float n[3];

		veh_copy( normal_sum, n );
		veh_normalize( n );
		veh_copy( n, v->ground_normal );
		veh_ma( v->velocity, accel / (float)v->num_wheels * dt, v->up, v->velocity );
		// two wheels or more: the chassis follows the ground slope
		if( down >= 2 )
			veh_align_up( v, n, g->align_rate, dt );
	}
	else
	{
		static const float world_up[3] = { 0.0f, 0.0f, 1.0f };

		veh_set( v->ground_normal, 0.0f, 0.0f, 1.0f );
		// airborne: slowly level out so landings come down on the wheels
		veh_align_up( v, world_up, g->align_rate * 0.15f, dt );
	}
	return down;
}

void veh_ground_drive( veh_t *v, const veh_ground_params_t *g, float throttle, float accel_scale, float speed_scale, float traction, float dt )
{
	float vf = veh_dot( v->velocity, v->fwd );
	float dv = 0.0f;

	if( traction <= 0.0f )
		return;

	if( throttle > 0.01f )
	{
		if( vf < -10.0f )
			dv = g->brake * throttle * dt;	// rolling back: brake first
		else
		{
			float top = g->max_speed * speed_scale;
			float headroom = veh_clamp( 1.0f - vf / top, 0.0f, 1.0f );

			dv = g->accel * accel_scale * throttle * headroom * dt;
		}
	}
	else if( throttle < -0.01f )
	{
		if( vf > 10.0f )
			dv = g->brake * throttle * dt;	// throttle negative: decelerates
		else
		{
			float headroom = veh_clamp( 1.0f + vf / g->reverse_speed, 0.0f, 1.0f );

			dv = g->reverse_accel * throttle * headroom * dt;
		}
	}
	else
	{
		// coast: rolling drag to rest, never past zero
		float drag = g->roll_drag * dt;

		dv = ( vf > 0.0f ) ? -( vf < drag ? vf : drag ) : ( -vf < drag ? -vf : drag );
	}

	veh_ma( v->velocity, dv * traction, v->fwd, v->velocity );
}

void veh_ground_grip( veh_t *v, float grip, float traction, float dt )
{
	float vl = veh_dot( v->velocity, v->left );
	float max_dl = grip * traction * dt;
	float dl = veh_clamp( -vl, -max_dl, max_dl );
	float speed = veh_length( v->velocity );

	veh_ma( v->velocity, dl, v->left, v->velocity );
	v->slip = speed > 1.0f ? veh_absf( vl ) / speed : 0.0f;
}

void veh_track_ground_events( veh_t *v, int was_on_ground )
{
	if( v->on_ground && !was_on_ground )
		v->events |= VEH_EV_LANDED;
	else if( !v->on_ground && was_on_ground )
		v->events |= VEH_EV_TAKEOFF;
}

/*
==============================================================================
hull slide move
==============================================================================
*/
#define VEH_MAX_CLIP_PLANES	4

static int veh_unstick( veh_t *v, const veh_world_t *world )
{
	static const float lifts[3] = { 4.0f, 10.0f, 18.0f };
	const veh_common_params_t *c = &v->p->c;
	int i;

	for( i = 0; i < 3; i++ )
	{
		float test[3];
		veh_trace_t tr;

		veh_copy( v->origin, test );
		test[2] += lifts[i];
		world->trace( world->ctx, test, c->hull_mins, c->hull_maxs, test, &tr );
		if( !tr.startsolid && !tr.allsolid )
		{
			veh_copy( test, v->origin );
			return 1;
		}
	}
	return 0;
}

void veh_slide_move( veh_t *v, const veh_world_t *world, float dt )
{
	const veh_common_params_t *c = &v->p->c;
	float planes[VEH_MAX_CLIP_PLANES][3];
	float time_left = dt;
	int numplanes = 0;
	int bump;

	for( bump = 0; bump < VEH_MAX_CLIP_PLANES && time_left > 0.0f; bump++ )
	{
		float end[3], vn, into;
		veh_trace_t tr;
		int i;

		veh_ma( v->origin, time_left, v->velocity, end );
		world->trace( world->ctx, v->origin, c->hull_mins, c->hull_maxs, end, &tr );

		if( tr.allsolid )
		{
			// started inside something: lift out or stop dead
			if( !veh_unstick( v, world ))
				veh_set( v->velocity, 0.0f, 0.0f, 0.0f );
			return;
		}

		if( tr.fraction > 0.0f )
		{
			veh_copy( tr.endpos, v->origin );
			numplanes = 0;
		}
		if( tr.fraction >= 1.0f )
			return;

		time_left -= time_left * tr.fraction;

		vn = veh_dot( v->velocity, tr.normal );
		into = -vn;
		if( into > v->impact_speed )
			v->impact_speed = into;
		if( into > c->impact_speed )
			v->events |= VEH_EV_IMPACT;

		if( numplanes < VEH_MAX_CLIP_PLANES )
		{
			veh_copy( tr.normal, planes[numplanes] );
			numplanes++;
		}

		// clip against the new plane, bouncing a little off walls (not floors)
		{
			float overbounce = 1.0f + ( tr.normal[2] < 0.7f ? c->bounce : 0.0f );

			veh_ma( v->velocity, -vn * overbounce, tr.normal, v->velocity );
		}

		// still running into an earlier plane: slide along the crease
		for( i = 0; i < numplanes - 1; i++ )
		{
			if( veh_dot( v->velocity, planes[i] ) < 0.0f )
			{
				float dir[3], d;

				veh_cross( planes[i], tr.normal, dir );
				if( veh_normalize( dir ) < 1e-6f )
				{
					veh_set( v->velocity, 0.0f, 0.0f, 0.0f );
					return;
				}
				d = veh_dot( dir, v->velocity );
				veh_scale( dir, d, v->velocity );
				break;
			}
		}
	}
}

/*
==============================================================================
chase camera
==============================================================================
*/
void Veh_CamReset( veh_cam_t *cam )
{
	memset( cam, 0, sizeof( *cam ));
}

float Veh_CamFollowYaw( veh_cam_t *cam, float vehicle_yaw )
{
	float delta = 0.0f;

	if( cam->primed )
	{
		delta = Veh_AngleMod( vehicle_yaw - cam->last_yaw );
		if( veh_absf( delta ) > 90.0f )
			delta = 0.0f;	// a teleport or respawn, not a turn
	}
	cam->last_yaw = vehicle_yaw;
	return delta;
}

void Veh_CamUpdate( veh_cam_t *cam, const veh_common_params_t *p, const float target[3], const float look[3], const veh_world_t *world, float dt )
{
	static const float cam_mins[3] = { -4.0f, -4.0f, -4.0f };
	static const float cam_maxs[3] = { 4.0f, 4.0f, 4.0f };
	float ang[3], dir[3], pivot[3], want[3];
	float want_dist, cur[3], cur_dist;
	int blocked = 0;

	ang[0] = veh_clamp( look[0] + p->cam_pitch, -60.0f, 80.0f );
	ang[1] = look[1];
	ang[2] = 0.0f;
	veh_angle_forward( ang, dir );

	veh_copy( target, pivot );
	pivot[2] += p->cam_height;
	veh_ma( pivot, -p->cam_dist, dir, want );

	if( world && world->trace )
	{
		veh_trace_t tr;

		world->trace( world->ctx, pivot, cam_mins, cam_maxs, want, &tr );
		if( tr.fraction < 1.0f )
		{
			veh_copy( tr.endpos, want );
			blocked = 1;
		}
	}

	cur[0] = cam->origin[0] - pivot[0];
	cur[1] = cam->origin[1] - pivot[1];
	cur[2] = cam->origin[2] - pivot[2];
	cur_dist = veh_length( cur );
	cur[0] = want[0] - pivot[0];
	cur[1] = want[1] - pivot[1];
	cur[2] = want[2] - pivot[2];
	want_dist = veh_length( cur );

	if( !cam->primed || cur_dist > p->cam_dist * 3.0f + 256.0f || ( blocked && want_dist < cur_dist ))
	{
		// first frame, teleport, or a wall pulled the camera in: snap
		veh_copy( want, cam->origin );
	}
	else
	{
		float k = Veh_SmoothFactor( p->cam_lag, dt );

		cam->origin[0] += ( want[0] - cam->origin[0] ) * k;
		cam->origin[1] += ( want[1] - cam->origin[1] ) * k;
		cam->origin[2] += ( want[2] - cam->origin[2] ) * k;
	}

	// look from the camera at the point the view aims through the pivot
	cam->angles[0] = ang[0];
	cam->angles[1] = ang[1];
	cam->angles[2] = 0.0f;
	cam->primed = 1;
}

#endif // XASH_VEHICLES
