/*
vehicle_local.h - private helpers shared by the vehicle type modules
*/
#ifndef VEHICLE_LOCAL_H
#define VEHICLE_LOCAL_H

#include <math.h>
#include "vehicle.h"

#define VEH_PI		3.14159265f
#define VEH_DEG2RAD	( VEH_PI / 180.0f )
#define VEH_RAD2DEG	( 180.0f / VEH_PI )

static inline float veh_clamp( float x, float lo, float hi )
{
	return x < lo ? lo : ( x > hi ? hi : x );
}

static inline float veh_absf( float x )
{
	return x < 0.0f ? -x : x;
}

// move x toward target by at most step
static inline float veh_approach( float x, float target, float step )
{
	if( x < target )
		return ( x + step > target ) ? target : x + step;
	return ( x - step < target ) ? target : x - step;
}

static inline float veh_dot( const float a[3], const float b[3] )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void veh_copy( const float a[3], float out[3] )
{
	out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
}

static inline void veh_set( float out[3], float x, float y, float z )
{
	out[0] = x; out[1] = y; out[2] = z;
}

// out = a + b * s
static inline void veh_ma( const float a[3], float s, const float b[3], float out[3] )
{
	out[0] = a[0] + b[0] * s;
	out[1] = a[1] + b[1] * s;
	out[2] = a[2] + b[2] * s;
}

static inline void veh_scale( const float a[3], float s, float out[3] )
{
	out[0] = a[0] * s; out[1] = a[1] * s; out[2] = a[2] * s;
}

static inline void veh_cross( const float a[3], const float b[3], float out[3] )
{
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];
	out[0] = x; out[1] = y; out[2] = z;
}

static inline float veh_length( const float a[3] )
{
	return sqrtf( veh_dot( a, a ));
}

static inline float veh_normalize( float a[3] )
{
	float len = veh_length( a );

	if( len > 1e-6f )
	{
		float inv = 1.0f / len;
		a[0] *= inv; a[1] *= inv; a[2] *= inv;
	}
	return len;
}

// view angles (pitch down positive) to a forward vector, GoldSrc AngleVectors
static inline void veh_angle_forward( const float ang[3], float fwd[3] )
{
	float sp = sinf( ang[0] * VEH_DEG2RAD ), cp = cosf( ang[0] * VEH_DEG2RAD );
	float sy = sinf( ang[1] * VEH_DEG2RAD ), cy = cosf( ang[1] * VEH_DEG2RAD );

	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;
}

// heading of the body forward vector, degrees
static inline float veh_yaw_of( const float fwd[3] )
{
	if( veh_absf( fwd[0] ) < 1e-6f && veh_absf( fwd[1] ) < 1e-6f )
		return 0.0f;
	return atan2f( fwd[1], fwd[0] ) * VEH_RAD2DEG;
}

/* Glide's units (see vehicle.h): wheel and engine forces are impulses applied
 * once a Source physics tick; a torque from a force at a point is r x F with
 * r and F in inches turned into VPhysics' kg m^2 degrees. */
#define VEH_GLIDE_TICK		0.015f
#define VEH_TORQUE_SCALE	( 0.0254f * 0.0254f * VEH_RAD2DEG )
#define VEH_TAU			( 2.0f * VEH_PI )
#define VEH_RADS_TO_RPM		( 60.0f / VEH_TAU )

// what a step pushes the body with, gathered before one integration
typedef struct veh_forces_s
{
	float	lin[3];		// world acceleration, units/s^2 (gravity is added later)
	float	ang[3];		// local angular acceleration, deg/s^2: roll, pitch, yaw
	float	drag[3];	// extra angular drag on top of ang_drag (negative, Glide units)
	float	drag_mult[3];	// multiplier on ang_drag (Glide's GetYawDragMultiplier)
} veh_forces_t;

static inline float veh_sign( float x )
{
	return x > 0.0f ? 1.0f : ( x < 0.0f ? -1.0f : 0.0f );
}

// Glide.ExpDecay: b + (a - b) * e^(-decay * dt)
static inline float veh_expdecay( float a, float b, float decay, float dt )
{
	return b + ( a - b ) * expf( -decay * dt );
}

static inline float veh_remap( float x, float a, float b, float c, float d )
{
	if( b == a )
		return c;
	return c + ( d - c ) * ( x - a ) / ( b - a );
}

static inline void veh_right( const veh_t *v, float out[3] )
{
	veh_scale( v->left, -1.0f, out );
}

// world vector to body (forward, left, up) components
static inline void veh_to_local( const veh_t *v, const float w[3], float out[3] )
{
	float x = veh_dot( w, v->fwd ), y = veh_dot( w, v->left ), z = veh_dot( w, v->up );
	out[0] = x; out[1] = y; out[2] = z;
}

// body rotation as a world vector, rad/s
static inline void veh_angvel_world( const veh_t *v, float out[3] )
{
	float w[3];

	veh_scale( v->fwd, v->angvel[0] * VEH_DEG2RAD, w );
	veh_ma( w, v->angvel[1] * VEH_DEG2RAD, v->left, w );
	veh_ma( w, v->angvel[2] * VEH_DEG2RAD, v->up, out );
}

// body: rigid body integration and forces (vehicle.c)
void veh_point_velocity( const veh_t *v, const float point[3], float out[3] );
// a Glide wheel-style force: an impulse every Source tick, at a world point
void veh_add_tick_impulse( const veh_t *v, veh_forces_t *f, const float point[3], const float imp[3], float dt );
// a one-off impulse now (Glide's phys:ApplyForceOffset), at a world point
void veh_apply_impulse( veh_t *v, const float point[3], const float imp[3] );
// a Glide per-mass torque (outAng += x * mass) about a body axis
static inline void veh_add_torque( const veh_t *v, veh_forces_t *f, int axis, float per_mass )
{
	f->ang[axis] += per_mass / v->p->c.inertia[axis];
}
void veh_orthonormalize( veh_t *v );

// wheels (vehicle.c): rays, suspension, traction; returns wheels on the ground
int veh_wheels_physics( veh_t *v, veh_forces_t *f, const veh_world_t *world, float dt );
// glide_wheel Update for wheels off the ground: torque spins the fake mass
void veh_wheels_think( veh_t *v, float dt );
// Glide's slow-speed slope hold (base_glide PhysicsSimulate)
void veh_anti_slide( const veh_t *v, veh_forces_t *f, float dt );
// distance to the ground below the vehicle, up to 10000 (aircraft altitude)
float veh_altitude( const veh_t *v, const veh_world_t *world );

void veh_body_integrate( veh_t *v, const veh_forces_t *f, const veh_world_t *world, float dt );
void veh_track_ground_events( veh_t *v, int was_on_ground );

// car engine, shared with the tank (vehicle_car.c)
void veh_engine_defaults( veh_engine_params_t *e );
float veh_gear_ratio( const veh_engine_params_t *e, int gear );
void veh_car_think( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt );
void veh_car_wheels( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in );
float veh_car_yaw_drag( const veh_t *v );
void veh_car_air( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt );

// type modules
void Veh_CarDefaults( veh_car_params_t *p );
void Veh_TankDefaults( veh_tank_params_t *p );
void Veh_PlaneDefaults( veh_plane_params_t *p );
void Veh_HeliDefaults( veh_heli_params_t *p );
void Veh_CarStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt );
void Veh_TankStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt );
void Veh_PlaneStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt );
void Veh_HeliStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt );

#endif // VEHICLE_LOCAL_H
