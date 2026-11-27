/*
vehicle.h - shared vehicle simulation: arcade car, flight and tank

This is the engine-agnostic core of the vehicle layer. It knows nothing about
edicts, cvars or the network: it steps a vehicle state from an input and a
world trace callback, and builds a chase camera. The server binding
(server/sv_vehicle.c) drives it from player usercmds; the client binding
(client/cl_vehicle.c) uses the camera half; tests/fixtures/vehicle_test.c
runs it on the host against a fake world.

Budget rules (GameCube):
 - no allocation: a veh_t is a plain struct the caller owns;
 - fixed 60 Hz step with an accumulator, at most VEH_MAX_SUBSTEPS a call;
 - single precision only, a handful of sinf/cosf a step, no atan2 in the
   inner loops (angles are produced once a step, for the entity);
 - car and tank cast one point trace per wheel and sweep one hull a step,
   flight sweeps one hull and probes the ground once.

Conventions are GoldSrc: Z up, angles in degrees as PITCH YAW ROLL.
Veh_GetAngles returns studio-model angles (positive pitch = nose up, which is
what a studio entity's v.angles expects); aim angles passed in are view
angles (positive pitch = look down), the same as usercmd viewangles.
*/
#ifndef VEHICLE_H
#define VEHICLE_H

/*
Build switch. The vehicle layer is about 20 KB of code, so the GameCube
builds leave it out unless asked (XASH_GC_VEHICLES=1 at configure, see the
top-level wscript): HL1 has no vehicles and plays at the MEM1 edge. Every
other target builds it.
*/
#ifndef XASH_VEHICLES
#if XASH_GAMECUBE
#define XASH_VEHICLES 0
#else
#define XASH_VEHICLES 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VEH_STEP_HZ		60
#define VEH_STEP_DT		( 1.0f / (float)VEH_STEP_HZ )
#define VEH_MAX_SUBSTEPS	4	// a 50 ms usercmd is 3 steps; a longer hitch is dropped
#define VEH_MAX_WHEELS		4

typedef enum
{
	VEH_NONE = 0,
	VEH_CAR,
	VEH_FLIGHT,
	VEH_TANK,
	VEH_TYPE_COUNT
} veh_type_t;

// input buttons, already mapped from IN_* by Veh_MapInput
#define VEH_BTN_HANDBRAKE	( 1U << 0 )
#define VEH_BTN_BRAKE		( 1U << 1 )
#define VEH_BTN_FIRE		( 1U << 2 )
#define VEH_BTN_ALTFIRE		( 1U << 3 )
#define VEH_BTN_BOOST		( 1U << 4 )

// one-shot events raised by a step, cleared by the caller (Veh_TakeEvents)
#define VEH_EV_FIRE		( 1U << 0 )	// tank main gun fired this step
#define VEH_EV_IMPACT		( 1U << 1 )	// hit something at speed (see veh_t.impact_speed)
#define VEH_EV_CRASH		( 1U << 2 )	// flight: impact above crash_speed
#define VEH_EV_LANDED		( 1U << 3 )	// came back to the ground after being airborne
#define VEH_EV_TAKEOFF		( 1U << 4 )	// left the ground

typedef struct veh_trace_s
{
	float	fraction;	// 1 = clear
	float	endpos[3];
	float	normal[3];
	int	startsolid;
	int	allsolid;
} veh_trace_t;

/*
The world the vehicle moves in. trace() sweeps an axis aligned box (mins/maxs
may both be zero for a ray) from start to end and must never hit the vehicle
itself or its driver. GoldSrc BSP collision snaps a box to the nearest clip
hull, so keep hull sizes to the stock hulls (see the per type defaults).
*/
typedef struct veh_world_s
{
	void	(*trace)( void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3], veh_trace_t *tr );
	void	*ctx;
	float	gravity;	// units/s^2, sv_gravity (800)
} veh_world_t;

typedef struct veh_input_s
{
	float	throttle;	// -1..1, forward positive
	float	steer;		// -1..1, right positive
	float	lift;		// -1..1, flight throttle lever / climb
	float	aim[3];		// view angles: pitch (down positive), yaw, roll
	unsigned	buttons;	// VEH_BTN_*
} veh_input_t;

/*
Tunables. One block per type; every field is a plain float so the server can
expose them all through one name table (Veh_ParamTable) instead of a cvar
each. Speeds are units/s, accelerations units/s^2, angles degrees, rates
degrees/s, times seconds.
*/
typedef struct veh_common_params_s
{
	float	hull_mins[3], hull_maxs[3];	// collision box, origin relative
	float	gravity_scale;
	float	bounce;			// 0..1 fraction of normal speed kept on a wall hit
	float	impact_speed;		// a hit faster than this raises VEH_EV_IMPACT
	float	cam_dist, cam_height, cam_pitch;	// chase camera: back, up, extra downward pitch
	float	cam_lag;			// 1/s camera follow rate (bigger = stiffer)
	float	cam_follow;		// 1 = view yaw turns with the vehicle (car), 0 = free (tank, flight aim)
	float	seat[3];			// driver eye, vehicle local (forward, left, up)
} veh_common_params_t;

typedef struct veh_ground_params_s
{
	// chassis and suspension (raycast wheels)
	float	wheel_x, wheel_y, wheel_z;	// wheel mounts at (+-x, +-y, z), vehicle local
	float	wheel_radius;
	float	susp_length;		// travel below the mount
	float	susp_stiffness;		// accel at full compression
	float	susp_damping;		// accel per unit/s of compression change
	float	align_rate;		// 1/s body pitch/roll follows the ground
	// drive
	float	accel, max_speed;
	float	reverse_accel, reverse_speed;
	float	brake;			// decel when throttle opposes motion
	float	roll_drag;		// decel with no throttle
	float	boost_scale;		// accel and top speed while VEH_BTN_BOOST
	// grip and steering
	float	grip;			// lateral decel the tyres/tracks can give
	float	steer_max;		// car: wheel angle; tank: turn rate at rest
	float	steer_rate;		// how fast the wheel angle follows the input
	float	steer_falloff;		// 0..1 steering lost at top speed
	float	wheelbase;		// car: bicycle model length
	float	air_control;		// 0..1 yaw control while airborne
} veh_ground_params_t;

typedef struct veh_car_params_s
{
	veh_common_params_t	c;
	veh_ground_params_t	g;
	// drift: the handbrake drops rear grip; the slide keeps going while
	// steering into it and recovers when the handbrake is released
	float	drift_grip;		// grip multiplier at full drift
	float	drift_min_speed;	// below this the handbrake only brakes
	float	drift_in_rate;		// 1/s drift factor rise while handbrake held
	float	drift_out_rate;		// 1/s drift factor decay
	float	drift_yaw_boost;	// extra yaw rate multiplier at full drift
	float	handbrake_decel;
} veh_car_params_t;

typedef struct veh_tank_params_s
{
	veh_common_params_t	c;
	veh_ground_params_t	g;
	float	pivot_speed_falloff;	// turn rate kept at top speed (0..1)
	float	reverse_steer_invert;	// 1 = steering mirrors in reverse like a car
	float	turret_yaw_rate, turret_pitch_rate;
	float	turret_pitch_min, turret_pitch_max;	// up positive
	float	turret_height;		// pivot above origin
	float	barrel_length;
	float	fire_interval;
	float	recoil;			// speed kick along -barrel
} veh_tank_params_t;

typedef struct veh_flight_params_s
{
	veh_common_params_t	c;
	float	max_thrust;		// accel at full throttle
	float	throttle_rate;		// lever travel per second at full input
	float	drag;			// 1/s linear drag along the flight path
	float	side_drag;		// 1/s sideslip and sink kill (how "on rails" it flies)
	float	cruise_speed;		// lift equals weight here at level flight
	float	stall_speed;		// below this lift collapses and the nose drops
	float	max_speed;
	float	pitch_rate, roll_rate, yaw_rate;	// body rates at full authority
	float	rate_response;		// 1/s how fast body rates follow the command
	float	control_speed;		// full control authority at and above this airspeed
	float	mouse_aim;		// 1 = fly toward the view direction, 0 = stick
	float	aim_gain;		// mouse aim: pitch/yaw command per unit of error
	float	bank_max;		// mouse aim: bank angle used to turn
	float	rotate_speed;		// on the ground: nose may lift above this speed
	float	ground_friction;
	float	crash_speed;
} veh_flight_params_t;

typedef union veh_params_u
{
	veh_common_params_t	c;	// first member of every block
	veh_car_params_t	car;
	veh_tank_params_t	tank;
	veh_flight_params_t	flight;
} veh_params_t;

typedef struct veh_wheel_s
{
	float	mount[3];	// vehicle local
	float	compress;	// 0..1
	float	compress_prev;
	int	contact;
	float	normal[3];
} veh_wheel_t;

typedef struct veh_s
{
	veh_type_t	type;
	const veh_params_t	*p;	// caller owned, may be shared by many vehicles

	float	origin[3];
	float	velocity[3];
	float	fwd[3], left[3], up[3];	// orthonormal body basis

	// ground vehicles
	veh_wheel_t	wheels[VEH_MAX_WHEELS];
	int	num_wheels;
	int	wheels_down;
	float	ground_normal[3];
	float	steer_angle;	// degrees, car
	float	drift;		// 0..1, car
	float	slip;		// sideways speed / speed, for effects

	// tank
	float	turret_yaw;	// relative to hull, degrees
	float	turret_pitch;	// up positive
	float	fire_cooldown;

	// flight
	float	rates[3];	// pitch (nose up), roll (right wing down), yaw (left) deg/s
	float	throttle_lever;	// 0..1

	int	on_ground;
	float	accum;		// fixed step accumulator
	float	impact_speed;	// last impact, for VEH_EV_IMPACT/CRASH
	unsigned	events;
	unsigned	steps;	// total fixed steps run
} veh_t;

// type names: "car", "flight", "tank"
const char *Veh_TypeName( veh_type_t type );
veh_type_t Veh_TypeForName( const char *name );

// stock tunables for a type (copy them, then edit the copy)
void Veh_DefaultParams( veh_type_t type, veh_params_t *out );

/*
Reflection over a type's tunables, for "vehicle tune" style console access:
Veh_ParamTable returns the count and fills *names / *offsets with static
arrays (offsets are byte offsets of floats inside veh_params_t).
*/
int Veh_ParamTable( veh_type_t type, const char *const **names, const unsigned short **offsets );
float *Veh_ParamPtr( veh_type_t type, veh_params_t *params, const char *name );

// place a fresh vehicle; yaw in degrees
void Veh_Init( veh_t *v, veh_type_t type, const veh_params_t *params, const float origin[3], float yaw );

/*
Usercmd to vehicle input. Button bits are the GoldSrc IN_* values (stable
since 1998). scale is the full-deflection move value (cl_forwardspeed, 400).
*/
#define VEH_IN_ATTACK	( 1U << 0 )
#define VEH_IN_JUMP	( 1U << 1 )
#define VEH_IN_DUCK	( 1U << 2 )
#define VEH_IN_USE	( 1U << 5 )
#define VEH_IN_ATTACK2	( 1U << 11 )
#define VEH_IN_RUN	( 1U << 12 )
#define VEH_IN_RELOAD	( 1U << 13 )
void Veh_MapInput( veh_type_t type, float forwardmove, float sidemove, float upmove, unsigned in_buttons, const float viewangles[3], float scale, veh_input_t *out );

/*
Advance by dt seconds of real time: runs whole fixed steps and keeps the
remainder. Returns the number of steps run.
*/
int Veh_Update( veh_t *v, const veh_input_t *in, const veh_world_t *world, float dt );
void Veh_Step( veh_t *v, const veh_input_t *in, const veh_world_t *world );	// one fixed step

unsigned Veh_TakeEvents( veh_t *v );

// studio entity angles (pitch nose-up positive, yaw, roll)
void Veh_GetAngles( const veh_t *v, float angles[3] );
// world position of a vehicle local point (forward, left, up)
void Veh_LocalToWorld( const veh_t *v, const float local[3], float out[3] );
float Veh_ForwardSpeed( const veh_t *v );
// tank: muzzle position and barrel direction
void Veh_TurretMuzzle( const veh_t *v, float org[3], float dir[3] );

/*
Chase camera. State is owned by the caller (one per local view). target is
the vehicle origin, look the view angles (pitch down positive). With
p->cam_follow the caller should add Veh_CamFollowYaw's result to its view yaw
each frame so the camera swings round behind a turning car.
*/
typedef struct veh_cam_s
{
	float	origin[3];
	float	angles[3];
	float	last_yaw;	// vehicle yaw last frame, for follow
	int	primed;
} veh_cam_t;

void Veh_CamReset( veh_cam_t *cam );
float Veh_CamFollowYaw( veh_cam_t *cam, float vehicle_yaw );
void Veh_CamUpdate( veh_cam_t *cam, const veh_common_params_t *p, const float target[3], const float look[3], const veh_world_t *world, float dt );

// small helpers shared with the bindings
float Veh_AngleMod( float a );		// -180..180
float Veh_SmoothFactor( float rate, float dt );	// 1 - e^(-rate*dt), cheap

#ifdef __cplusplus
}
#endif

#endif // VEHICLE_H
