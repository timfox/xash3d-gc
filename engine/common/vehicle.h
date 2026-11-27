/*
vehicle.h - shared vehicle simulation: car, tank, plane, helicopter

The handling model follows Glide, StyledStrike's vehicle base for Garry's Mod
(MIT licence, github.com/StyledStrike/gmod-glide): a rigid body pushed by
raycast wheels with Glide's spring/damper suspension, traction circle and
slip-angle side grip; a car engine with a fake flywheel, a geared automatic
transmission and an auto clutch; a tank that is that car on tracks with turn
in place and a turret; and Glide's plane and helicopter force models. The
equations and the stock numbers are Glide's; what is different is only what
GoldSrc needs (see docs/VEHICLES.md, "Differences from Glide").

This is the engine-agnostic core. It knows nothing about edicts, cvars or the
network: it steps a vehicle from an input and a world trace callback, and
builds a chase camera. The server binding (server/sv_vehicle.c) drives it from
player usercmds; the client binding (client/cl_vehicle.c) uses the camera
half; tests/fixtures/vehicle_test.c runs it on the host against a fake world.

Budget rules (GameCube):
 - no allocation: a veh_t is a plain struct the caller owns;
 - fixed 60 Hz step with an accumulator, at most VEH_MAX_SUBSTEPS a call;
 - single precision only, a handful of sinf/cosf/atan2f a step;
 - one point trace per wheel (4 for a car, 8 for a tank, 3 for a plane, 4 for
   a helicopter's skids) and one to four hull sweeps a step.

Conventions are GoldSrc: Z up, angles in degrees as PITCH YAW ROLL.
Veh_GetAngles returns studio-model angles (positive pitch = nose up, which is
what a studio entity's v.angles expects); aim angles passed in are view
angles (positive pitch = look down), the same as usercmd viewangles.
Body rates (veh_t.angvel) are Glide's (Source's) local angular velocity:
roll about forward, pitch about left (positive = nose down), yaw about up
(positive = nose left), degrees per second.
*/
#ifndef VEHICLE_H
#define VEHICLE_H

/*
Build switch. The vehicle layer is about 40 KB of code, so the GameCube
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
#define VEH_MAX_WHEELS		8
#define VEH_MAX_GEARS		6	// forward gears; reverse is separate

typedef enum
{
	VEH_NONE = 0,
	VEH_CAR,
	VEH_PLANE,
	VEH_TANK,
	VEH_HELI,
	VEH_TYPE_COUNT
} veh_type_t;

#define VEH_FLIGHT	VEH_PLANE	// the first framework's name for the plane

// input buttons, already mapped from IN_* by Veh_MapInput
#define VEH_BTN_HANDBRAKE	( 1U << 0 )
#define VEH_BTN_FIRE		( 1U << 1 )
#define VEH_BTN_ALTFIRE		( 1U << 2 )
#define VEH_BTN_GENTLE		( 1U << 3 )	// Glide's throttle modifier: 70% throttle
#define VEH_BTN_FREELOOK	( 1U << 4 )	// aircraft: look around without steering
#define VEH_BTN_BRAKE		( 1U << 5 )	// car/tank: brake pedal on its own (throttle + brake = burnout)

// one-shot events raised by a step, cleared by the caller (Veh_TakeEvents)
#define VEH_EV_FIRE		( 1U << 0 )	// tank main gun fired this step
#define VEH_EV_IMPACT		( 1U << 1 )	// hit something at speed (see veh_t.impact_speed)
#define VEH_EV_CRASH		( 1U << 2 )	// aircraft: impact above crash_speed
#define VEH_EV_LANDED		( 1U << 3 )	// came back to the ground after being airborne
#define VEH_EV_TAKEOFF		( 1U << 4 )	// left the ground
#define VEH_EV_SHIFT		( 1U << 5 )	// car/tank changed gear

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

/*
Raw driver input. The type modules read it the way Glide's input actions
are bound: see Veh_MapInput and docs/VEHICLES.md for the per type meaning.
*/
typedef struct veh_input_s
{
	float	throttle;	// -1..1, forwardmove
	float	steer;		// -1..1, sidemove, right positive
	float	lift;		// -1..1, upmove or jump/duck
	float	aim[3];		// view angles: pitch (down positive), yaw, roll
	unsigned	buttons;	// VEH_BTN_*
	int	driven;		// someone is at the controls (engines run, brakes release)
} veh_input_t;

/*
Tunables. One block per type; every field is a plain float so the server can
expose them all through one name table (Veh_ParamTable) instead of a cvar
each. Units are GoldSrc units and seconds, angles degrees. Forces are Glide's
own numbers in Glide's units, see the note above veh_common_params_t.
*/
typedef struct veh_wheel_params_s
{
	/* layout, vehicle local: car and helicopter mounts at (front_x, +-y,
	 * front_z) and (rear_x, +-y, rear_z); a tank spreads wheels_per_side
	 * between them; a plane has its main gear at the front pair and one
	 * steering wheel at (rear_x, 0, rear_z), a tail wheel when rear_x < 0 */
	float	front_x, front_z, rear_x, rear_z, y;
	// glide_wheel parameters, same names and meaning
	float	radius;
	float	susp_length;		// suspensionLength
	float	spring_strength;	// springStrength
	float	spring_damper;		// springDamper
	float	brake_power;		// brakePower
	float	fwd_traction_max;	// forwardTractionMax
	float	side_traction_mult;	// sideTractionMultiplier
	float	side_traction_max_ang;	// sideTractionMaxAng (degrees)
	float	side_traction_max;	// sideTractionMax
	float	side_traction_min;	// sideTractionMin
	float	rolling_resistance;	// surfaceResistance (0.05 on every Source surface but ice)
	float	surface_grip;		// surfaceGrip (1)
} veh_wheel_params_t;

/*
Glide's wheel and engine forces are impulses applied once a Source physics
tick (1/66.67 s) to a body whose mass is `mass`; its own lift, drag and
control forces are per-mass accelerations, and its angular terms are torques
against an inertia of mass * inertia[] (m^2, the way VPhysics measures it).
The core keeps those meanings, so Glide's numbers drop straight in.
*/
typedef struct veh_common_params_s
{
	float	hull_mins[3], hull_maxs[3];	// collision box, origin relative
	float	mass;			// ChassisMass
	float	inertia[3];		// roll, pitch, yaw: inertia / mass, m^2
	float	ang_drag[3];		// AngularDrag: roll, pitch, yaw (negative)
	float	gravity_scale;
	float	bounce;			// 0..1 fraction of normal speed kept on a wall hit
	float	impact_speed;		// a hit faster than this raises VEH_EV_IMPACT
	float	crash_speed;		// aircraft: a hit faster than this raises VEH_EV_CRASH
	float	cam_dist, cam_height, cam_pitch;	// chase camera: back, up, extra downward pitch
	float	cam_lag;			// 1/s camera follow rate (bigger = stiffer)
	float	cam_follow;		// 1 = view yaw turns with the vehicle (car), 0 = free
	float	seat[3];			// driver eye, vehicle local (forward, left, up)
	veh_wheel_params_t	wheel;
} veh_common_params_t;

// base_glide_car: engine, transmission, steering, air control
typedef struct veh_engine_params_s
{
	float	min_rpm, max_rpm;
	float	min_rpm_torque, max_rpm_torque;	// MinRPMTorque / MaxRPMTorque
	float	diff_ratio;		// DifferentialRatio
	float	efficiency;		// TransmissionEfficiency
	float	power_distribution;	// -1 rear .. 1 front
	float	engine_brake_torque;
	float	flywheel_mass, flywheel_radius, flywheel_friction, flywheel_torque;
	float	turbo;			// 1 = TurboCharged
	float	fast_shift;		// 1 = FastTransmission
	float	shift_delay;		// switchBaseDelay
	float	gear_count;		// forward gears in use, 1..VEH_MAX_GEARS
	float	gear_reverse;
	float	gear[VEH_MAX_GEARS];
	float	max_steer;		// MaxSteerAngle
	float	steer_cone_rate;	// SteerConeChangeRate
	float	steer_cone_max_speed;	// SteerConeMaxSpeed
	float	steer_cone_max_angle;	// SteerConeMaxAngle
	float	counter_steer;		// CounterSteer
	float	burnout_force;
	float	unflip_force;
	float	air_control[3];		// AirControlForce: roll, pitch, yaw
	float	air_max_angvel[3];	// AirMaxAngularVelocity
} veh_engine_params_t;

typedef struct veh_car_params_s
{
	veh_common_params_t	c;
	veh_engine_params_t	e;
} veh_car_params_t;

typedef struct veh_tank_params_s
{
	veh_common_params_t	c;
	veh_engine_params_t	e;
	float	wheels_per_side;	// 2..4 track wheels, front_x to rear_x
	float	turn_in_place;		// TurnInPlaceTorqueMultiplier, 0 = cannot
	float	turret_height;		// TurretOffset z
	float	barrel_length;
	float	turret_pitch_up;	// -PitchAngMax
	float	turret_pitch_down;	// PitchAngMin
	float	turret_yaw_speed;	// MaxYawSpeed
	float	turret_yaw_accel;	// YawAcceleration
	float	fire_interval;		// FireDelay
	float	recoil;			// TurretRecoilForce
} veh_tank_params_t;

// base_glide_plane: PlaneParams plus the ground steering
typedef struct veh_plane_params_s
{
	veh_common_params_t	c;
	float	lift_ang_drag[3];	// liftAngularDrag: roll, pitch, yaw
	float	lift_fwd_drag, lift_side_drag, lift_factor;
	float	max_speed, lift_speed, control_speed;
	float	engine_force, align_force;
	float	pitch_force, yaw_force, roll_force;
	float	power_response;
	float	steer_max, steer_cone_max_speed;
	float	reverse_torque, max_reverse_speed;
	float	mouse_aim;		// 1 = point-to-aim (Glide MOUSE_FLY_MODE.AIM), 0 = stick
	float	aim_gain;		// mouse aim multiplier (Glide: 15 for planes)
	float	input_rate;		// ExpDecay rate of the control surfaces
} veh_plane_params_t;

// base_glide_heli: HelicopterParams
typedef struct veh_heli_params_s
{
	veh_common_params_t	c;
	float	base_power;
	float	drag[3];		// forward, right, up
	float	max_fwd_drag, max_side_drag;
	float	turbulence;
	float	push_up_force;
	float	pitch_force, yaw_force, roll_force;
	float	push_fwd_force;
	float	max_speed;
	float	upright_force;
	float	power_response;
	float	mouse_aim;
	float	aim_gain;		// Glide: 8 for helicopters
	float	input_rate;
} veh_heli_params_t;

typedef union veh_params_u
{
	veh_common_params_t	c;	// first member of every block
	veh_car_params_t	car;
	veh_tank_params_t	tank;
	veh_plane_params_t	plane;
	veh_heli_params_t	heli;
} veh_params_t;

typedef struct veh_wheel_s
{
	float	mount[3];	// vehicle local
	float	steer_mult;	// steerMultiplier
	int	front;		// isFrontWheel
	int	left;		// track side, for turn in place
	float	distribution;	// share of the axle torque

	// state, as glide_wheel's
	int	contact;
	float	fraction;
	float	last_offset;	// lastSpringOffset
	float	ang_vel;	// rad/s
	float	spin;		// degrees, for a wheel model
	float	torque, brake;
	float	fwd_mult, side_mult;	// forwardTractionMult / sideTractionMult
	float	side_slip, fwd_slip;	// lastSideSlip / lastForwardSlip
	float	normal[3];
} veh_wheel_t;

typedef struct veh_s
{
	veh_type_t	type;
	const veh_params_t	*p;	// caller owned, may be shared by many vehicles

	float	origin[3];	// centre of mass
	float	velocity[3];
	float	fwd[3], left[3], up[3];	// orthonormal body basis
	float	angvel[3];	// local deg/s: roll, pitch (nose down +), yaw (left +)

	veh_wheel_t	wheels[VEH_MAX_WHEELS];
	int	num_wheels;
	int	wheels_down;	// groundedCount
	float	steer_angle;	// wheel yaw, degrees (Glide steerAngle[2])
	float	steer_input;	// smoothed steer (inputSteer)
	float	avg_side_slip, avg_fwd_slip, avg_rpm;

	// car and tank: base_glide_car's engine
	int	engine_on;
	int	gear;		// -1 reverse, 0 neutral, 1.. forward
	float	flywheel;	// flywheel angular velocity, rad/s
	float	engine_throttle;
	float	clutch;
	float	switch_cd;
	float	burnout;
	float	front_torque, rear_torque;
	float	front_brake, rear_brake;
	float	front_traction, rear_traction;
	float	front_side_traction, rear_side_traction;
	float	jturn;
	float	air_in[3];
	int	turning_in_place;

	// tank
	float	turret_yaw;	// relative to the hull, degrees, left positive
	float	turret_pitch;	// up positive
	float	fire_cooldown;

	// aircraft
	float	power;		// 0..2 plane, 0..1.2 helicopter (Glide GetPower)
	float	in_pitch, in_roll, in_yaw;	// smoothed control inputs
	float	altitude;
	float	plane_brake;

	int	on_ground;
	int	hull_ground;	// the hull box rested on a floor last step
	float	accum;		// fixed step accumulator
	float	impact_speed;	// last impact, for VEH_EV_IMPACT/CRASH
	unsigned	events;
	unsigned	steps;	// total fixed steps run
} veh_t;

// type names: "car", "plane", "tank", "heli"
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

// place a fresh vehicle; yaw in degrees. Call again after changing a wheel layout tunable.
void Veh_Init( veh_t *v, veh_type_t type, const veh_params_t *params, const float origin[3], float yaw );
// rebuild the wheel layout from the tunables, keeping the body state
void Veh_LayoutWheels( veh_t *v );

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
#define VEH_IN_SCORE	( 1U << 15 )
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
float Veh_EngineRPM( const veh_t *v );
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
