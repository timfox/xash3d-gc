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

/* first-order rotations of the body basis by small angles (radians); the
 * basis is re-orthonormalised once a step by veh_orthonormalize */
void veh_rotate_yaw_world( veh_t *v, float a );	// about world Z, CCW positive
void veh_rotate_body( veh_t *v, float pitch, float roll, float yaw );	// nose up, right wing down, nose left
void veh_orthonormalize( veh_t *v );
// tilt the body so up approaches target_up at rate (1/s), keeping heading
void veh_align_up( veh_t *v, const float target_up[3], float rate, float dt );

// ground vehicles: wheel rays, suspension, slope alignment; returns wheels down
int veh_ground_probe( veh_t *v, const veh_ground_params_t *g, const veh_world_t *world, float dt );
// throttle/brake/reverse along the body forward axis
void veh_ground_drive( veh_t *v, const veh_ground_params_t *g, float throttle, float accel_scale, float speed_scale, float traction, float dt );
// lateral grip: kill sideways speed by at most grip*dt
void veh_ground_grip( veh_t *v, float grip, float traction, float dt );

// swept hull move with sliding, bounce and impact events
void veh_slide_move( veh_t *v, const veh_world_t *world, float dt );

void veh_track_ground_events( veh_t *v, int was_on_ground );

// type modules
void Veh_CarDefaults( veh_car_params_t *p );
void Veh_TankDefaults( veh_tank_params_t *p );
void Veh_FlightDefaults( veh_flight_params_t *p );
void Veh_CarStep( veh_t *v, const veh_input_t *in, const veh_world_t *world );
void Veh_TankStep( veh_t *v, const veh_input_t *in, const veh_world_t *world );
void Veh_FlightStep( veh_t *v, const veh_input_t *in, const veh_world_t *world );
void Veh_GroundInitWheels( veh_t *v, const veh_ground_params_t *g );

#endif // VEHICLE_LOCAL_H
