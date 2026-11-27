/*
vehicle.c - shared vehicle core: types, tunables, input, fixed step, rigid
body, Glide's raycast wheels, hull slide move and the chase camera

See vehicle.h for the budget rules and where the model comes from. Nothing
here allocates or touches engine state; the bindings pass a veh_world_t with
a trace callback.
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
static const char *const veh_type_names[VEH_TYPE_COUNT] = { "none", "car", "plane", "tank", "heli" };

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
	if( veh_streq( name, "flight" ) || veh_streq( name, "jet" ) || veh_streq( name, "fly" ) || veh_streq( name, "airplane" ))
		return VEH_PLANE;
	if( veh_streq( name, "helicopter" ) || veh_streq( name, "chopper" ) || veh_streq( name, "copter" ))
		return VEH_HELI;
	return VEH_NONE;
}

void Veh_DefaultParams( veh_type_t type, veh_params_t *out )
{
	memset( out, 0, sizeof( *out ));
	switch( type )
	{
	case VEH_CAR: Veh_CarDefaults( &out->car ); break;
	case VEH_TANK: Veh_TankDefaults( &out->tank ); break;
	case VEH_PLANE: Veh_PlaneDefaults( &out->plane ); break;
	case VEH_HELI: Veh_HeliDefaults( &out->heli ); break;
	default: break;
	}
}

typedef struct
{
	const char	*name;
	unsigned short	ofs;
} veh_param_def_t;

#define VP( name, field )	{ name, (unsigned short)offsetof( veh_params_t, field ) }
#define VP_F( b, f )		VP( #f, b.f )

#define VEH_COMMON_DEFS \
	VP( "mass", c.mass ), \
	VP( "inertia_roll", c.inertia[0] ), VP( "inertia_pitch", c.inertia[1] ), VP( "inertia_yaw", c.inertia[2] ), \
	VP( "drag_roll", c.ang_drag[0] ), VP( "drag_pitch", c.ang_drag[1] ), VP( "drag_yaw", c.ang_drag[2] ), \
	VP_F( c, gravity_scale ), VP_F( c, bounce ), VP_F( c, impact_speed ), VP_F( c, crash_speed ), \
	VP_F( c, cam_dist ), VP_F( c, cam_height ), VP_F( c, cam_pitch ), VP_F( c, cam_lag ), VP_F( c, cam_follow ), \
	VP( "wheel_front_x", c.wheel.front_x ), VP( "wheel_front_z", c.wheel.front_z ), \
	VP( "wheel_rear_x", c.wheel.rear_x ), VP( "wheel_rear_z", c.wheel.rear_z ), VP( "wheel_y", c.wheel.y ), \
	VP( "wheel_radius", c.wheel.radius ), VP_F( c.wheel, susp_length ), \
	VP_F( c.wheel, spring_strength ), VP_F( c.wheel, spring_damper ), VP_F( c.wheel, brake_power ), \
	VP_F( c.wheel, fwd_traction_max ), VP_F( c.wheel, side_traction_mult ), VP_F( c.wheel, side_traction_max_ang ), \
	VP_F( c.wheel, side_traction_max ), VP_F( c.wheel, side_traction_min ), \
	VP_F( c.wheel, rolling_resistance ), VP_F( c.wheel, surface_grip )

#define VEH_ENGINE_DEFS( b ) \
	VP_F( b.e, min_rpm ), VP_F( b.e, max_rpm ), VP_F( b.e, min_rpm_torque ), VP_F( b.e, max_rpm_torque ), \
	VP_F( b.e, diff_ratio ), VP_F( b.e, efficiency ), VP_F( b.e, power_distribution ), VP_F( b.e, engine_brake_torque ), \
	VP_F( b.e, flywheel_mass ), VP_F( b.e, flywheel_radius ), VP_F( b.e, flywheel_friction ), VP_F( b.e, flywheel_torque ), \
	VP_F( b.e, turbo ), VP_F( b.e, fast_shift ), VP_F( b.e, shift_delay ), VP_F( b.e, gear_count ), VP_F( b.e, gear_reverse ), \
	VP( "gear1", b.e.gear[0] ), VP( "gear2", b.e.gear[1] ), VP( "gear3", b.e.gear[2] ), \
	VP( "gear4", b.e.gear[3] ), VP( "gear5", b.e.gear[4] ), VP( "gear6", b.e.gear[5] ), \
	VP_F( b.e, max_steer ), VP_F( b.e, steer_cone_rate ), VP_F( b.e, steer_cone_max_speed ), \
	VP_F( b.e, steer_cone_max_angle ), VP_F( b.e, counter_steer ), VP_F( b.e, burnout_force ), VP_F( b.e, unflip_force ), \
	VP( "air_roll", b.e.air_control[0] ), VP( "air_pitch", b.e.air_control[1] ), VP( "air_yaw", b.e.air_control[2] ), \
	VP( "air_max_roll", b.e.air_max_angvel[0] ), VP( "air_max_pitch", b.e.air_max_angvel[1] ), VP( "air_max_yaw", b.e.air_max_angvel[2] )

static const veh_param_def_t veh_car_defs[] =
{
	VEH_COMMON_DEFS, VEH_ENGINE_DEFS( car ),
};

static const veh_param_def_t veh_tank_defs[] =
{
	VEH_COMMON_DEFS, VEH_ENGINE_DEFS( tank ),
	VP_F( tank, wheels_per_side ), VP_F( tank, turn_in_place ),
	VP_F( tank, turret_height ), VP_F( tank, barrel_length ),
	VP_F( tank, turret_pitch_up ), VP_F( tank, turret_pitch_down ),
	VP_F( tank, turret_yaw_speed ), VP_F( tank, turret_yaw_accel ),
	VP_F( tank, fire_interval ), VP_F( tank, recoil ),
};

static const veh_param_def_t veh_plane_defs[] =
{
	VEH_COMMON_DEFS,
	VP( "lift_drag_roll", plane.lift_ang_drag[0] ), VP( "lift_drag_pitch", plane.lift_ang_drag[1] ), VP( "lift_drag_yaw", plane.lift_ang_drag[2] ),
	VP_F( plane, lift_fwd_drag ), VP_F( plane, lift_side_drag ), VP_F( plane, lift_factor ),
	VP_F( plane, max_speed ), VP_F( plane, lift_speed ), VP_F( plane, control_speed ),
	VP_F( plane, engine_force ), VP_F( plane, align_force ),
	VP_F( plane, pitch_force ), VP_F( plane, yaw_force ), VP_F( plane, roll_force ),
	VP_F( plane, power_response ), VP_F( plane, steer_max ), VP_F( plane, steer_cone_max_speed ),
	VP_F( plane, reverse_torque ), VP_F( plane, max_reverse_speed ),
	VP_F( plane, mouse_aim ), VP_F( plane, aim_gain ), VP_F( plane, input_rate ),
};

static const veh_param_def_t veh_heli_defs[] =
{
	VEH_COMMON_DEFS,
	VP_F( heli, base_power ),
	VP( "heli_drag_fwd", heli.drag[0] ), VP( "heli_drag_side", heli.drag[1] ), VP( "heli_drag_up", heli.drag[2] ),
	VP_F( heli, max_fwd_drag ), VP_F( heli, max_side_drag ), VP_F( heli, turbulence ),
	VP_F( heli, push_up_force ), VP_F( heli, pitch_force ), VP_F( heli, yaw_force ), VP_F( heli, roll_force ),
	VP_F( heli, push_fwd_force ), VP_F( heli, max_speed ), VP_F( heli, upright_force ),
	VP_F( heli, power_response ), VP_F( heli, mouse_aim ), VP_F( heli, aim_gain ), VP_F( heli, input_rate ),
};

#define VEH_DEFS_COUNT( a )	( (int)( sizeof( a ) / sizeof( a[0] )))

/* The name/offset views handed out by Veh_ParamTable. Built from the def
 * tables on first use; static, so no allocation and stable pointers. */
#define VEH_MAX_PARAMS	80
static const char *veh_param_names[VEH_TYPE_COUNT][VEH_MAX_PARAMS];
static unsigned short veh_param_ofs[VEH_TYPE_COUNT][VEH_MAX_PARAMS];
static int veh_param_count[VEH_TYPE_COUNT];

static const veh_param_def_t *veh_defs_for( veh_type_t type, int *count )
{
	switch( type )
	{
	case VEH_CAR: *count = VEH_DEFS_COUNT( veh_car_defs ); return veh_car_defs;
	case VEH_TANK: *count = VEH_DEFS_COUNT( veh_tank_defs ); return veh_tank_defs;
	case VEH_PLANE: *count = VEH_DEFS_COUNT( veh_plane_defs ); return veh_plane_defs;
	case VEH_HELI: *count = VEH_DEFS_COUNT( veh_heli_defs ); return veh_heli_defs;
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

float Veh_EngineRPM( const veh_t *v )
{
	return v->flywheel * VEH_RADS_TO_RPM;
}

/*
==============================================================================
rigid body
==============================================================================
*/
void veh_point_velocity( const veh_t *v, const float point[3], float out[3] )
{
	float w[3], r[3], c[3];

	veh_angvel_world( v, w );
	r[0] = point[0] - v->origin[0];
	r[1] = point[1] - v->origin[1];
	r[2] = point[2] - v->origin[2];
	veh_cross( w, r, c );
	out[0] = v->velocity[0] + c[0];
	out[1] = v->velocity[1] + c[1];
	out[2] = v->velocity[2] + c[2];
}

// linear and local angular response to an impulse J (kg units/s) at a world point
static void veh_impulse_response( const veh_t *v, const float point[3], const float imp[3], float dv[3], float dw[3] )
{
	const veh_common_params_t *c = &v->p->c;
	float inv_mass = 1.0f / c->mass;
	float r[3], t[3], tl[3];
	int i;

	veh_scale( imp, inv_mass, dv );
	r[0] = point[0] - v->origin[0];
	r[1] = point[1] - v->origin[1];
	r[2] = point[2] - v->origin[2];
	veh_cross( r, imp, t );
	veh_to_local( v, t, tl );
	for( i = 0; i < 3; i++ )
		dw[i] = tl[i] * VEH_TORQUE_SCALE * inv_mass / c->inertia[i];
}

void veh_add_tick_impulse( const veh_t *v, veh_forces_t *f, const float point[3], const float imp[3], float dt )
{
	float dv[3], dw[3];
	int i;

	(void)dt;
	veh_impulse_response( v, point, imp, dv, dw );
	// once a Source tick, so this much a second
	for( i = 0; i < 3; i++ )
	{
		f->lin[i] += dv[i] * ( 1.0f / VEH_GLIDE_TICK );
		f->ang[i] += dw[i] * ( 1.0f / VEH_GLIDE_TICK );
	}
}

void veh_apply_impulse( veh_t *v, const float point[3], const float imp[3] )
{
	float dv[3], dw[3];
	int i;

	veh_impulse_response( v, point, imp, dv, dw );
	for( i = 0; i < 3; i++ )
	{
		v->velocity[i] += dv[i];
		v->angvel[i] += dw[i];
	}
}

#define VEH_MAX_ANGVEL	1080.0f	// deg/s, a safety cap like Glide's ClampForce
#define VEH_MAX_SPEED	4000.0f

static void veh_slide_move( veh_t *v, const veh_world_t *world, float dt );

void veh_body_integrate( veh_t *v, const veh_forces_t *f, const veh_world_t *world, float dt )
{
	const veh_common_params_t *c = &v->p->c;
	float w[3], speed;
	int i;

	// linear: forces plus gravity
	v->velocity[0] += f->lin[0] * dt;
	v->velocity[1] += f->lin[1] * dt;
	v->velocity[2] += ( f->lin[2] - world->gravity * c->gravity_scale ) * dt;
	speed = veh_length( v->velocity );
	if( speed > VEH_MAX_SPEED )
		veh_scale( v->velocity, VEH_MAX_SPEED / speed, v->velocity );

	// angular: Glide's AngularDrag (a torque of drag * angvel * mass) is
	// integrated implicitly, so a stiff drag on a light axis stays stable
	for( i = 0; i < 3; i++ )
	{
		float k = -( c->ang_drag[i] * f->drag_mult[i] + f->drag[i] ) / c->inertia[i];

		if( k < 0.0f )
			k = 0.0f;
		v->angvel[i] = ( v->angvel[i] + f->ang[i] * dt ) / ( 1.0f + k * dt );
		v->angvel[i] = veh_clamp( v->angvel[i], -VEH_MAX_ANGVEL, VEH_MAX_ANGVEL );
	}

	// rotate the basis by the body rate, first order, then re-orthonormalise
	veh_angvel_world( v, w );
	{
		float *axes[3] = { v->fwd, v->left, v->up };

		for( i = 0; i < 3; i++ )
		{
			float d[3];

			veh_cross( w, axes[i], d );
			veh_ma( axes[i], dt, d, axes[i] );
		}
	}
	veh_orthonormalize( v );

	v->hull_ground = 0;
	veh_slide_move( v, world, dt );
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
wheels: glide_wheel
==============================================================================
*/
void Veh_LayoutWheels( veh_t *v )
{
	const veh_wheel_params_t *wp = &v->p->c.wheel;
	int i, n = 0, front = 0, rear = 0;

#define VEH_ADD_WHEEL( x, y, z, steer ) \
	do { \
		veh_wheel_t *w_ = &v->wheels[n++]; \
		veh_set( w_->mount, ( x ), ( y ), ( z )); \
		w_->steer_mult = ( steer ); \
		w_->front = ( x ) > 0.0f; \
		w_->left = ( y ) > 0.0f; \
	} while( 0 )

	switch( v->type )
	{
	case VEH_TANK:
	{
		int k, per_side = (int)veh_clamp( v->p->tank.wheels_per_side, 2.0f, (float)( VEH_MAX_WHEELS / 2 ));

		for( k = 0; k < per_side; k++ )
		{
			float t = (float)k / (float)( per_side - 1 );
			float x = wp->front_x + ( wp->rear_x - wp->front_x ) * t;
			float z = wp->front_z + ( wp->rear_z - wp->front_z ) * t;
			float steer = 1.0f - 2.0f * t;	// Glide's Rhino: front +1, rear -1

			VEH_ADD_WHEEL( x, wp->y, z, steer );
			VEH_ADD_WHEEL( x, -wp->y, z, steer );
		}
		break;
	}
	case VEH_PLANE:
		// main gear, then the steering nose or tail wheel
		VEH_ADD_WHEEL( wp->front_x, wp->y, wp->front_z, 0.0f );
		VEH_ADD_WHEEL( wp->front_x, -wp->y, wp->front_z, 0.0f );
		VEH_ADD_WHEEL( wp->rear_x, 0.0f, wp->rear_z, wp->rear_x < 0.0f ? -1.0f : 1.0f );
		break;
	case VEH_HELI:
		VEH_ADD_WHEEL( wp->front_x, wp->y, wp->front_z, 0.0f );
		VEH_ADD_WHEEL( wp->front_x, -wp->y, wp->front_z, 0.0f );
		VEH_ADD_WHEEL( wp->rear_x, wp->y, wp->rear_z, 0.0f );
		VEH_ADD_WHEEL( wp->rear_x, -wp->y, wp->rear_z, 0.0f );
		break;
	default:
		VEH_ADD_WHEEL( wp->front_x, wp->y, wp->front_z, 1.0f );
		VEH_ADD_WHEEL( wp->front_x, -wp->y, wp->front_z, 1.0f );
		VEH_ADD_WHEEL( wp->rear_x, wp->y, wp->rear_z, 0.0f );
		VEH_ADD_WHEEL( wp->rear_x, -wp->y, wp->rear_z, 0.0f );
		break;
	}
#undef VEH_ADD_WHEEL
	v->num_wheels = n;

	// base_glide_car UpdatePowerDistribution: front/rear split by wheel count
	for( i = 0; i < n; i++ )
	{
		if( v->wheels[i].front ) front++;
		else rear++;
	}
	for( i = 0; i < n; i++ )
	{
		float pd = 0.0f, share;

		if( v->type == VEH_CAR ) pd = v->p->car.e.power_distribution;
		else if( v->type == VEH_TANK ) pd = v->p->tank.e.power_distribution;
		share = 0.5f + pd * 0.5f;
		if( v->wheels[i].front )
			v->wheels[i].distribution = front ? share / (float)front : 0.0f;
		else
			v->wheels[i].distribution = rear ? ( 1.0f - share ) / (float)rear : 0.0f;
	}
}

// Glide.TractionRamp
static float veh_traction_ramp( float slip, float max_ang, float tmax, float tmin )
{
	float a = max_ang / 90.0f, x;

	if( slip < a || a >= 1.0f )
		return tmax;
	x = ( slip - a ) / ( 1.0f - a );
	return tmax * ( 1.0f - x ) + tmin * x;
}

/*
glide_wheel DoPhysics, one wheel. The forces are Glide's numbers and Glide's
units: impulses once a Source tick (veh_add_tick_impulse).
*/
static int veh_wheel_physics( veh_t *v, veh_wheel_t *w, veh_forces_t *f, const veh_world_t *world, float total_speed, float dt )
{
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	const veh_wheel_params_t *p = &v->p->c.wheel;
	float pos[3], end[3], contact[3], wfwd[3], rt[3], n[3], vel[3], force[3];
	float max_len = p->susp_length + p->radius;
	float a = v->steer_angle * w->steer_mult * VEH_DEG2RAD;
	float up_align, vel_f, vel_r, vel_u, offset, spring, damper;
	float grip = p->surface_grip, max_traction, brake_force, fwd_force, grip_loss, tc;
	float ground_ang_vel, slip_angle, side_force;
	veh_trace_t tr;

	Veh_LocalToWorld( v, w->mount, pos );
	veh_ma( pos, -max_len, v->up, end );
	world->trace( world->ctx, pos, zero, zero, end, &tr );

	w->fraction = veh_clamp( tr.fraction, p->radius / max_len, 1.0f );
	w->contact = tr.fraction < 1.0f && !tr.allsolid && veh_dot( tr.normal, v->up ) > 0.0f;
	if( !w->contact )
	{
		// glide_wheel Update: slips are zero off the ground; the spring relaxes
		w->last_offset = 0.0f;
		return 0;
	}

	veh_ma( pos, -max_len * w->fraction, v->up, contact );
	veh_point_velocity( v, contact, vel );
	veh_copy( tr.normal, n );
	veh_copy( n, w->normal );
	up_align = veh_dot( v->up, n );

	// steered wheel forward; right is forward x ground normal
	veh_scale( v->fwd, cosf( a ), wfwd );
	veh_ma( wfwd, sinf( a ), v->left, wfwd );
	veh_cross( wfwd, n, rt );

	vel_f = veh_dot( wfwd, vel );
	vel_r = veh_dot( rt, vel ) * up_align;
	vel_u = veh_dot( n, vel );

	// a hard landing on the gear is a crash for an aircraft
	if( -vel_u > v->impact_speed )
		v->impact_speed = -vel_u;
	if( v->p->c.crash_speed > 0.0f && -vel_u > v->p->c.crash_speed )
		v->events |= VEH_EV_CRASH;

	// suspension spring and damper (the 0.015 is Glide's, see glide_wheel)
	offset = max_len - w->fraction * max_len;
	spring = offset * p->spring_strength;
	damper = (( w->last_offset - offset ) / dt ) * p->spring_damper * VEH_GLIDE_TICK;
	w->last_offset = offset;

	// about to bottom out: drop the damper, cancel the fall, lift the body
	if( up_align > 0.5f && vel_u < 0.0f && offset + veh_absf( vel_u * dt ) > p->susp_length )
	{
		float excess = offset + veh_absf( vel_u * dt ) - p->susp_length;

		damper = 0.0f;
		spring -= vel_u / dt;
		veh_ma( v->origin, excess / (float)v->num_wheels, n, v->origin );
	}

	veh_scale( n, ( spring - damper ) * up_align, force );

	// rolling resistance
	veh_ma( force, -p->rolling_resistance * vel_f, wfwd, force );

	// brake and torque, losing grip on the traction circle
	max_traction = p->fwd_traction_max * grip * w->fwd_mult;
	brake_force = ( vel_f > 0.0f ? -w->brake : w->brake ) * p->brake_power * grip;
	// Source friction holds a stopped wheel; a brake never pushes one
	brake_force *= veh_clamp( veh_absf( vel_f ) * 0.2f, 0.0f, 1.0f );
	fwd_force = w->torque + brake_force;
	{
		float side = veh_absf( vel_r ) < max_traction ? veh_absf( vel_r ) : max_traction;

		tc = sqrtf( side * side + fwd_force * fwd_force );
	}
	grip_loss = tc - max_traction;
	if( grip_loss < 0.0f )
		grip_loss = 0.0f;
	fwd_force = fwd_force - grip_loss * veh_sign( fwd_force ) + veh_clamp( brake_force * 0.5f, -max_traction, max_traction );
	veh_ma( force, fwd_force, wfwd, force );

	// wheel spin: the ground's rate plus what grip loss lets the torque add
	ground_ang_vel = vel_f / p->radius;
	w->ang_vel = veh_approach( w->ang_vel, ground_ang_vel + grip_loss * veh_sign( w->torque ), dt * 200.0f );
	grip_loss = ground_ang_vel - w->ang_vel;
	w->fwd_slip = grip_loss;

	// side slip angle, 0..1 for 0..90 degrees
	slip_angle = atan2f( vel_r, veh_absf( vel_f )) / VEH_PI * 2.0f;
	w->side_slip = slip_angle * veh_clamp( total_speed * 0.005f, 0.0f, 1.0f ) * 2.0f;
	max_traction = veh_traction_ramp( veh_absf( slip_angle ), p->side_traction_max_ang, p->side_traction_max, p->side_traction_min );

	side_force = -veh_dot( rt, vel ) * p->side_traction_mult * w->side_mult * 0.75f;
	side_force *= 1.0f - veh_clamp( veh_absf( grip_loss ) * 0.1f, 0.0f, 1.0f ) * 0.75f;
	grip *= veh_clamp( offset, 0.0f, 1.0f );
	veh_ma( force, veh_clamp( side_force, -max_traction, max_traction ) * grip, rt, force );
	// a small unclamped side force that helps a lot at high speed
	veh_ma( force, vel_r * p->side_traction_mult * -0.1f, rt, force );

	veh_add_tick_impulse( v, f, contact, force, dt );
	return 1;
}

int veh_wheels_physics( veh_t *v, veh_forces_t *f, const veh_world_t *world, float dt )
{
	float total_speed = veh_length( v->velocity );
	int i, down = 0;

	for( i = 0; i < v->num_wheels; i++ )
		down += veh_wheel_physics( v, &v->wheels[i], f, world, total_speed, dt );
	v->wheels_down = down;
	return down;
}

void veh_wheels_think( veh_t *v, float dt )
{
	int i;

	for( i = 0; i < v->num_wheels; i++ )
	{
		veh_wheel_t *w = &v->wheels[i];

		w->spin = fmodf( w->spin - w->ang_vel * VEH_RAD2DEG * dt, 360.0f );
		if( !w->contact )
		{
			// let the torque spin the wheel's fake mass, and slow down
			w->ang_vel += ( w->torque / 8.0f ) * dt;
			w->ang_vel = veh_approach( w->ang_vel, 0.0f, dt * 4.0f );
			w->fwd_slip = 0.0f;
			w->side_slip = 0.0f;
		}
	}
}

void veh_anti_slide( const veh_t *v, veh_forces_t *f, float dt )
{
	float speed, factor, rt[3], vr;

	if( v->wheels_down < 1 )
		return;
	speed = veh_length( v->velocity ) + veh_absf( v->angvel[2] );
	factor = 1.0f - veh_clamp( speed / 30.0f, 0.0f, 1.0f );
	if( factor <= 0.1f )
		return;
	veh_right( v, rt );
	vr = veh_dot( rt, v->velocity );
	veh_ma( f->lin, -( vr / dt ) * factor, rt, f->lin );
}

float veh_altitude( const veh_t *v, const veh_world_t *world )
{
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	float start[3], end[3];
	veh_trace_t tr;

	veh_copy( v->origin, start );
	start[2] += v->p->c.hull_mins[2] * 0.9f;
	veh_copy( start, end );
	end[2] -= 10000.0f;
	world->trace( world->ctx, start, zero, zero, end, &tr );
	return tr.fraction < 1.0f ? tr.fraction * 10000.0f : 10000.0f;
}

/*
==============================================================================
setup, input, stepping
==============================================================================
*/
void Veh_Init( veh_t *v, veh_type_t type, const veh_params_t *params, const float origin[3], float yaw )
{
	float s = sinf( yaw * VEH_DEG2RAD ), c = cosf( yaw * VEH_DEG2RAD );
	int i;

	memset( v, 0, sizeof( *v ));
	v->type = type;
	v->p = params;
	veh_copy( origin, v->origin );
	veh_set( v->fwd, c, s, 0.0f );
	veh_set( v->left, -s, c, 0.0f );
	veh_set( v->up, 0.0f, 0.0f, 1.0f );

	if( type <= VEH_NONE || type >= VEH_TYPE_COUNT )
		return;

	Veh_LayoutWheels( v );
	for( i = 0; i < VEH_MAX_WHEELS; i++ )
	{
		v->wheels[i].fwd_mult = v->wheels[i].side_mult = 1.0f;
		v->wheels[i].fraction = 1.0f;
	}
	v->clutch = 1.0f;
	v->jturn = 1.0f;
	v->front_traction = v->rear_traction = 1.0f;
	v->front_side_traction = v->rear_side_traction = 1.0f;
}

void Veh_MapInput( veh_type_t type, float forwardmove, float sidemove, float upmove, unsigned in_buttons, const float viewangles[3], float scale, veh_input_t *out )
{
	float inv = scale > 1.0f ? 1.0f / scale : 1.0f / 400.0f;

	memset( out, 0, sizeof( *out ));
	out->throttle = veh_clamp( forwardmove * inv, -1.0f, 1.0f );
	out->steer = veh_clamp( sidemove * inv, -1.0f, 1.0f );
	out->lift = veh_clamp( upmove * inv, -1.0f, 1.0f );
	veh_copy( viewangles, out->aim );
	out->driven = 1;

	if( in_buttons & VEH_IN_ATTACK ) out->buttons |= VEH_BTN_FIRE;
	if( in_buttons & VEH_IN_ATTACK2 ) out->buttons |= VEH_BTN_ALTFIRE;

	switch( type )
	{
	case VEH_CAR:
	case VEH_TANK:
		if( in_buttons & VEH_IN_JUMP ) out->buttons |= VEH_BTN_HANDBRAKE;
		if( in_buttons & VEH_IN_RUN ) out->buttons |= VEH_BTN_GENTLE;
		if( in_buttons & VEH_IN_DUCK ) out->buttons |= VEH_BTN_BRAKE;
		break;
	case VEH_PLANE:
	case VEH_HELI:
		// jump/duck are the throttle on keyboards and pads without an up axis
		if( in_buttons & VEH_IN_JUMP ) out->lift = 1.0f;
		if( in_buttons & VEH_IN_DUCK ) out->lift = -1.0f;
		if( in_buttons & VEH_IN_ATTACK2 ) out->buttons |= VEH_BTN_FREELOOK;
		break;
	default:
		break;
	}
}

/*
GoldSrc collides the body as an axis aligned box, which cannot tip over an
edge the way Glide's VPhysics hull does. While the box rests on a floor and
the wheels do not hold the body, gravity tips it: onto its wheels while it is
less than 90 degrees over, onto its roof past that (where the driver unflips
it, as in Glide).
*/
static void veh_settle_hull( veh_t *v, veh_forces_t *f )
{
	static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
	float axis[3], local[3], s;
	int i;

	if( !v->hull_ground || v->wheels_down >= 2 || v->up[2] < -0.97f )
		return;
	veh_cross( v->up, world_up, axis );	// |axis| = sin(tilt)
	s = v->up[2] >= 0.0f ? 1.0f : -1.0f;
	veh_to_local( v, axis, local );
	for( i = 0; i < 2; i++ )
		f->ang[i] += s * local[i] * 720.0f;
	f->drag[0] += -2.0f * v->p->c.inertia[0];
	f->drag[1] += -2.0f * v->p->c.inertia[1];
}

void Veh_Step( veh_t *v, const veh_input_t *in, const veh_world_t *world )
{
	const float dt = VEH_STEP_DT;
	int was_on_ground = v->on_ground;
	veh_forces_t f;

	// a tune to zero would divide by it
	if( v->p->c.mass <= 0.0f || v->p->c.inertia[0] <= 0.0f || v->p->c.inertia[1] <= 0.0f || v->p->c.inertia[2] <= 0.0f )
		return;

	memset( &f, 0, sizeof( f ));
	v->impact_speed = 0.0f;
	f.drag_mult[0] = f.drag_mult[1] = f.drag_mult[2] = 1.0f;

	switch( v->type )
	{
	case VEH_CAR: Veh_CarStep( v, in, world, &f, dt ); break;
	case VEH_TANK: Veh_TankStep( v, in, world, &f, dt ); break;
	case VEH_PLANE: Veh_PlaneStep( v, in, world, &f, dt ); break;
	case VEH_HELI: Veh_HeliStep( v, in, world, &f, dt ); break;
	default: return;
	}

	veh_settle_hull( v, &f );
	veh_body_integrate( v, &f, world, dt );
	v->on_ground = v->wheels_down > 0;
	veh_track_ground_events( v, was_on_ground );
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

static void veh_slide_move( veh_t *v, const veh_world_t *world, float dt )
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
		{
			v->events |= VEH_EV_IMPACT;
			// a hard knock takes most of the spin out, as VPhysics friction would
			veh_scale( v->angvel, 0.5f, v->angvel );
		}
		if( c->crash_speed > 0.0f && into > c->crash_speed )
			v->events |= VEH_EV_CRASH;

		if( tr.normal[2] > 0.7f )
			v->hull_ground = 1;

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
