/*
vehicle_car.c - Glide's car: base_glide_car's engine, transmission, steering
and air control, on the raycast wheels in vehicle.c

The order of a step is Glide's: OnPostThink (engine, gears, clutch,
steering, air control) and WheelThink (hand torque and brakes to the wheels),
then PhysicsSimulate (the wheel forces). The tank (vehicle_tank.c) runs the
same engine with its own numbers.
*/
#include <string.h>
#include "vehicle_local.h"

#if XASH_VEHICLES

void veh_engine_defaults( veh_engine_params_t *e )
{
	// base_glide_car OnPostInitialize / EngineInit / GetGears
	e->min_rpm = 500.0f;
	e->max_rpm = 6000.0f;
	e->min_rpm_torque = 3200.0f;
	e->max_rpm_torque = 4200.0f;
	e->diff_ratio = 0.55f;
	e->efficiency = 0.8f;
	e->power_distribution = -0.9f;
	e->engine_brake_torque = 2000.0f;
	e->flywheel_mass = 80.0f;
	e->flywheel_radius = 0.5f;
	e->flywheel_friction = -6000.0f;
	e->flywheel_torque = 20000.0f;
	e->turbo = 0.0f;
	e->fast_shift = 0.0f;
	e->shift_delay = 0.3f;
	e->gear_count = 6.0f;
	e->gear_reverse = 2.5f;
	e->gear[0] = 2.8f;
	e->gear[1] = 1.7f;
	e->gear[2] = 1.2f;
	e->gear[3] = 0.9f;
	e->gear[4] = 0.75f;
	e->gear[5] = 0.7f;
	e->max_steer = 35.0f;
	e->steer_cone_rate = 6.0f;
	e->steer_cone_max_speed = 1800.0f;
	e->steer_cone_max_angle = 0.25f;
	e->counter_steer = 0.1f;
	e->burnout_force = 25.0f;
	e->unflip_force = 6.0f;
	veh_set( e->air_control, 0.8f, 0.3f, 0.2f );
	veh_set( e->air_max_angvel, 150.0f, 200.0f, 150.0f );
}

void Veh_CarDefaults( veh_car_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_wheel_params_t *w = &c->wheel;

	// GoldSrc large hull (64x64x64), set so the box rides just above the wheels
	veh_set( c->hull_mins, -32.0f, -32.0f, -10.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 54.0f );
	// Glide's Infernus: 800 kg; a 180x80x50 unit body for the inertia
	c->mass = 800.0f;
	veh_set( c->inertia, 0.5f, 1.9f, 2.1f );
	veh_set( c->ang_drag, -0.5f, -0.5f, -3.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.2f;
	c->impact_speed = 250.0f;
	c->crash_speed = 0.0f;
	c->cam_dist = 200.0f;
	c->cam_height = 56.0f;
	c->cam_pitch = 6.0f;
	c->cam_lag = 8.0f;
	c->cam_follow = 1.0f;
	veh_set( c->seat, -12.0f, 18.0f, 26.0f );

	// glide_wheel with base_glide_car's NW defaults, Infernus layout
	w->front_x = 60.0f;
	w->rear_x = -60.0f;
	w->front_z = w->rear_z = 4.0f;
	w->y = 36.0f;
	w->radius = 15.0f;
	w->susp_length = 10.0f;
	w->spring_strength = 500.0f;
	w->spring_damper = 2000.0f;
	w->brake_power = 3000.0f;
	w->fwd_traction_max = 2200.0f;
	w->side_traction_mult = 20.0f;
	w->side_traction_max_ang = 25.0f;
	w->side_traction_max = 2400.0f;
	w->side_traction_min = 800.0f;
	w->rolling_resistance = 0.05f;
	w->surface_grip = 1.0f;

	veh_engine_defaults( &p->e );
}

float veh_gear_ratio( const veh_engine_params_t *e, int gear )
{
	int count = (int)veh_clamp( e->gear_count, 1.0f, (float)VEH_MAX_GEARS );

	if( gear < 0 )
		return e->gear_reverse;
	if( gear == 0 || gear > count )
		return 0.0f;
	return veh_clamp( e->gear[gear - 1], 0.05f, 20.0f );	// Glide.ClampGearRatio
}

static int veh_max_gear( const veh_engine_params_t *e )
{
	return (int)veh_clamp( e->gear_count, 1.0f, (float)VEH_MAX_GEARS );
}

static void veh_set_flywheel_rpm( veh_t *v, float rpm )
{
	v->flywheel = ( rpm > 0.0f ? rpm : 0.0f ) / VEH_RADS_TO_RPM;
}

static void veh_engine_accelerate( veh_t *v, const veh_engine_params_t *e, float torque, float dt )
{
	float inertia = 0.5f * e->flywheel_mass * e->flywheel_radius * e->flywheel_radius;

	if( inertia > 0.0f )
		v->flywheel += torque / inertia * dt;
}

// TransmissionToEngineRPM: Glide's avgPoweredRPM is an RPM already, and the
// 60/TAU is applied again; kept, since every Glide tune is built on it
static float veh_trans_to_engine_rpm( const veh_t *v, const veh_engine_params_t *e, int gear )
{
	return v->avg_rpm * veh_gear_ratio( e, gear ) * e->diff_ratio * VEH_RADS_TO_RPM;
}

static void veh_switch_gear( veh_t *v, const veh_engine_params_t *e, int gear, float cooldown )
{
	if( v->gear == gear )
		return;
	if( gear < -1 ) gear = -1;
	if( gear > veh_max_gear( e )) gear = veh_max_gear( e );
	v->switch_cd = cooldown * ( gear == 1 ? 0.0f : ( e->fast_shift > 0.5f ? 0.5f : 1.0f ));
	v->clutch = 1.0f;
	v->gear = gear;
	v->events |= VEH_EV_SHIFT;
}

static float veh_transmission_torque( const veh_t *v, const veh_engine_params_t *e, int gear, float tmin, float tmax )
{
	float t = veh_remap( Veh_EngineRPM( v ), e->min_rpm, e->max_rpm, tmin, tmax );

	t = veh_clamp( t, tmin < tmax ? tmin : tmax, tmin < tmax ? tmax : tmin );
	t *= 1.0f - v->clutch;
	t *= veh_gear_ratio( e, gear ) * e->diff_ratio * e->efficiency;
	return gear == -1 ? -t : t;
}

static void veh_auto_gear( veh_t *v, const veh_engine_params_t *e, float throttle, float brake, float fwd_speed )
{
	float min_rpm = e->min_rpm, max_rpm = e->max_rpm * 0.95f, threshold;
	int max_gear = veh_max_gear( e );
	int target, i;

	// trying to go backwards?
	if( fwd_speed < 100.0f && brake > 0.2f )
	{
		veh_switch_gear( v, e, -1, 0.0f );
		return;
	}
	// still rolling back in reverse
	if( v->gear < 0 && fwd_speed < -100.0f )
		return;
	// wheels slipping forward
	if( veh_absf( v->avg_fwd_slip ) > 3.0f )
		return;

	target = v->gear < 1 ? 1 : ( v->gear > max_gear ? max_gear : v->gear );
	for( i = 1; i <= max_gear; i++ )
	{
		float rpm = veh_trans_to_engine_rpm( v, e, i );

		if(( i == 1 && rpm < min_rpm ) || ( rpm > min_rpm && rpm < max_rpm ))
		{
			target = i;
			break;
		}
	}

	// downshift only when the RPM has dropped, sooner off the throttle
	threshold = min_rpm + ( max_rpm - min_rpm ) * ( throttle > 0.01f ? 0.3f : 0.8f );
	if( target < v->gear && Veh_EngineRPM( v ) > threshold )
		return;
	veh_switch_gear( v, e, target, e->shift_delay );
}

/*
EngineThink with EngineClutch. Returns nothing; leaves the axle torques,
brakes and traction multipliers on the vehicle for veh_car_wheels.
*/
static void veh_engine_think( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt )
{
	float fwd_speed = veh_dot( v->velocity, v->fwd );
	float gentle = ( in->buttons & VEH_BTN_GENTLE ) ? 0.7f : 1.0f;
	float throttle_in = ( in->throttle > 0.0f ? in->throttle : 0.0f ) * gentle;
	float brake_in = in->throttle < 0.0f ? -in->throttle : 0.0f;
	int handbrake = ( in->buttons & VEH_BTN_HANDBRAKE ) != 0;
	int gear = v->gear;

	// one move axis can't hold both pedals; the brake button is the second
	if( in->buttons & VEH_BTN_BRAKE )
		brake_in = 1.0f;
	float rpm, clutch, trans_rpm = 0.0f, throttle, gear_torque, avail, front;

	if( v->burnout > 0.0f )
	{
		veh_switch_gear( v, e, 1, 0.0f );
		if( throttle_in < 0.1f || brake_in < 0.1f )
			v->burnout = 0.0f;
	}
	else
		veh_auto_gear( v, e, throttle_in, brake_in, fwd_speed );

	// reverse swaps the pedals
	if( gear < 0 )
	{
		float t = throttle_in;
		throttle_in = brake_in;
		brake_in = t;
	}

	rpm = Veh_EngineRPM( v );

	// auto clutch (EngineClutch)
	if( v->switch_cd > 0.0f )
	{
		v->switch_cd -= dt;
		throttle_in = 0.0f;
		clutch = 0.0f;
	}
	else if( handbrake )
		clutch = 1.0f;
	else if( v->wheels_down < 1 && veh_absf( fwd_speed ) > 30.0f )
		clutch = 1.0f;	// airborne and moving
	else if( fwd_speed < -50.0f && brake_in > 0.0f && v->gear < 0 )
		clutch = 1.0f;	// braking out of a reverse roll
	else if( veh_absf( fwd_speed ) > 200.0f )
		clutch = 0.0f;
	else
		clutch = throttle_in > 0.1f ? 0.0f : 1.0f;

	if( throttle_in > 0.1f && brake_in > 0.1f && veh_absf( fwd_speed ) < 50.0f )
	{
		// burnout: hold throttle and brake; steering spins the car on its nose
		int front_burnout = e->power_distribution > 0.0f;
		float force, dir[3];
		int i;

		v->burnout = veh_approach( v->burnout, 1.0f, dt * 2.0f );
		clutch = 0.0f;

		force = v->p->c.mass * e->burnout_force * v->burnout * dt;
		force *= v->steer_input * veh_clamp( veh_absf( v->avg_fwd_slip ) * 0.1f, 0.0f, 1.0f );
		if( front_burnout )
			veh_right( v, dir );
		else
			veh_copy( v->left, dir );
		veh_scale( dir, force, dir );

		v->front_brake = front_burnout ? 0.0f : 0.25f;
		v->rear_brake = front_burnout ? 0.25f : 0.0f;
		v->front_traction = front_burnout ? 0.25f : 2.0f;
		v->rear_traction = front_burnout ? 2.0f : 0.25f;

		for( i = 0; i < v->num_wheels; i++ )
		{
			const veh_wheel_t *w = &v->wheels[i];
			float local[3], pos[3];

			if( w->front != front_burnout )
				continue;
			// pushed at twice the body's length, as Glide uses the OBB
			veh_copy( w->mount, local );
			local[0] = local[0] > 0.0f ? v->p->c.hull_maxs[0] * 2.0f : v->p->c.hull_mins[0] * 2.0f;
			Veh_LocalToWorld( v, local, pos );
			veh_apply_impulse( v, pos, dir );
		}
	}
	else if( handbrake )
	{
		v->front_traction = 1.0f;
		v->rear_traction = 0.5f;
		v->front_brake = 0.0f;
		v->rear_brake = 0.5f;
		v->clutch = 1.0f;
		clutch = 1.0f;
	}
	else
	{
		// hold the car when idling in first or reverse
		if(( gear == -1 || gear == 1 ) && throttle_in < 0.05f && brake_in < 0.1f && v->wheels_down > 1 && rpm < e->min_rpm * 1.2f )
			brake_in = 0.2f;
		v->front_brake = v->rear_brake = brake_in * 0.5f;
		v->front_traction = v->rear_traction = 1.0f;
	}

	v->clutch = veh_approach( v->clutch, clutch, dt * (( gear < 2 && throttle_in > 0.1f ) ? 6.0f : 2.0f ));
	clutch = v->clutch;

	// engine RPM follows the wheels through the clutch
	if( gear != 0 )
	{
		trans_rpm = veh_trans_to_engine_rpm( v, e, gear );
		if( gear < 0 )
			trans_rpm = -trans_rpm;
		rpm = rpm * clutch + ( trans_rpm > 0.0f ? trans_rpm : 0.0f ) * ( 1.0f - clutch );
	}

	throttle = v->engine_throttle;
	gear_torque = veh_transmission_torque( v, e, gear, e->min_rpm_torque, e->max_rpm_torque );
	avail = gear_torque * throttle;

	if( trans_rpm < 0.0f )
		avail += gear_torque * 2.0f;	// rolling against the gear: hard engine braking
	else
		avail -= veh_transmission_torque( v, e, gear, e->engine_brake_torque, e->engine_brake_torque ) * ( 1.0f - throttle ) * 0.5f;

	if( rpm > e->max_rpm )
	{
		if( rpm > e->max_rpm * 1.2f )
			avail = 0.0f;
		rpm = e->max_rpm;
	}
	rpm = veh_clamp( rpm, e->min_rpm, e->max_rpm );
	veh_set_flywheel_rpm( v, rpm );

	if( e->turbo > 0.5f )
		avail *= 1.0f + ( rpm / e->max_rpm ) * 0.3f;
	if( v->burnout > 0.0f )
		avail += avail * v->burnout * 0.1f;

	front = 0.5f + e->power_distribution * 0.5f;
	v->front_torque = avail * front;
	v->rear_torque = avail * ( 1.0f - front );

	// the flywheel spins up with the throttle against its friction
	throttle = veh_approach( throttle, throttle_in, dt * 4.0f );
	veh_engine_accelerate( v, e, e->flywheel_friction + e->flywheel_torque * throttle, dt );
	v->engine_throttle = throttle;
}

static void veh_steering( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt )
{
	float input = veh_clamp( in->steer, -1.0f, 1.0f ), abs_input = veh_absf( input );
	float side_slip = veh_clamp( v->avg_side_slip, -1.0f, 1.0f );
	float total_speed = veh_length( v->velocity );
	float cone_factor = e->steer_cone_max_speed > 0.0f ? veh_clamp( total_speed / e->steer_cone_max_speed, 0.0f, 1.0f ) : 0.0f;
	float cone = 1.0f - cone_factor * ( 1.0f - e->steer_cone_max_angle );
	float counter;

	// the cone narrows with speed, but never below the slide angle
	cone = veh_clamp( cone, veh_absf( side_slip ), 1.0f );
	v->steer_input = veh_expdecay( v->steer_input, input * cone, e->steer_cone_rate, dt );

	// counter-steer when sliding fast with the wheel let go
	counter = veh_clamp( side_slip * cone_factor * ( 1.0f - abs_input ), -1.0f, 1.0f ) * e->counter_steer;
	v->steer_angle = -veh_clamp( v->steer_input + counter, -1.0f, 1.0f ) * e->max_steer;

	// J-turn: the front lets go when going backwards fast
	if( veh_dot( v->velocity, v->fwd ) < -100.0f )
		v->jturn = 0.5f;
	else
		v->jturn = veh_expdecay( v->jturn, 1.0f, 2.0f, dt );

	if( v->burnout > 0.1f )
	{
		int front_burnout = e->power_distribution > 0.0f;

		v->front_side_traction = front_burnout ? 0.5f : 1.0f;
		v->rear_side_traction = front_burnout ? 1.0f : 0.5f;
	}
	else
	{
		v->front_side_traction = v->jturn;
		v->rear_side_traction = 1.0f;
	}
}

void veh_car_air( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt )
{
	float mass = v->p->c.mass;

	if( v->wheels_down >= 1 )
	{
		veh_set( v->air_in, 0.0f, 0.0f, 0.0f );
		return;
	}

	if( veh_length( v->velocity ) > 200.0f )
	{
		// UpdateAirControls: steer rolls, throttle leans (pitch), within a max spin
		float want[3];
		int i;

		want[0] = veh_clamp( in->steer, -1.0f, 1.0f );
		want[1] = veh_clamp( in->throttle, -1.0f, 1.0f );
		want[2] = 0.0f;
		for( i = 0; i < 3; i++ )
		{
			float input, mult;

			v->air_in[i] = veh_approach( v->air_in[i], want[i], dt );
			input = want[i];
			mult = e->air_max_angvel[i] > 0.0f ? veh_clamp( 1.0f - veh_absf( v->angvel[i] / e->air_max_angvel[i] ), 0.0f, 1.0f ) : 1.0f;
			// only limit when spinning the same way as the input
			if(( input > 0.0f && v->angvel[i] > 0.0f ) || ( input < 0.0f && v->angvel[i] < 0.0f ))
				input *= mult;
			v->angvel[i] += e->air_control[i] * input * mass * dt;
		}
	}
	else if( veh_absf( v->steer_input ) >= 0.1f )
	{
		// UpdateUnflip: on its back, steering rolls it over
		float ang[3];

		Veh_GetAngles( v, ang );
		if( veh_absf( ang[2] ) >= 70.0f )
		{
			float force = v->steer_input * mass * veh_clamp( 1.0f - veh_absf( v->angvel[0] ) / 50.0f, 0.0f, 1.0f ) * e->unflip_force;

			v->angvel[0] += force * dt;
		}
	}
}

/*
OnPostThink for a car or a tank. With no driver the engine is off and the
brakes hold it, as Glide does when the driver gets out.
*/
void veh_car_think( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in, float dt )
{
	if( in->driven && !v->engine_on )
	{
		// TurnOn: the starter spins the flywheel up
		v->engine_on = 1;
		veh_set_flywheel_rpm( v, e->max_rpm * 0.75f );
	}
	else if( !in->driven && v->engine_on )
	{
		v->engine_on = 0;
		v->engine_throttle = 0.0f;
		v->flywheel = 0.0f;
	}

	if( v->engine_on )
		veh_engine_think( v, e, in, dt );
	else
	{
		float brake = veh_clamp( 0.05f + (( in->buttons & VEH_BTN_HANDBRAKE ) ? 0.5f : ( in->throttle < 0.0f ? -in->throttle : 0.0f )), 0.0f, 1.0f );

		v->front_torque = v->rear_torque = 0.0f;
		v->front_brake = v->rear_brake = brake;
		v->front_traction = v->rear_traction = 1.0f;
	}

	veh_steering( v, e, in, dt );
	veh_car_air( v, e, in, dt );
}

/*
WheelThink: torque, brakes and traction to each wheel, the handbrake locks
the rear, and no wheel spins faster than the gear lets the engine.
*/
void veh_car_wheels( veh_t *v, const veh_engine_params_t *e, const veh_input_t *in )
{
	float ratio = veh_gear_ratio( e, v->gear ) * e->diff_ratio;
	float max_rpm = ratio > 0.0f ? Veh_EngineRPM( v ) / ratio : 1e9f;
	int handbrake = ( in->buttons & VEH_BTN_HANDBRAKE ) != 0;
	float avg = 0.0f, side = 0.0f, fwd = 0.0f;
	int i;

	for( i = 0; i < v->num_wheels; i++ )
	{
		veh_wheel_t *w = &v->wheels[i];
		float rpm = w->ang_vel * VEH_RADS_TO_RPM;

		side += w->side_slip;
		fwd += w->fwd_slip;
		avg += rpm * w->distribution;

		if( !v->turning_in_place )
			w->torque = w->distribution * ( w->front ? v->front_torque : v->rear_torque );
		w->brake = w->front ? v->front_brake : v->rear_brake;
		w->fwd_mult = w->front ? v->front_traction : v->rear_traction;
		w->side_mult = w->front ? v->front_side_traction : v->rear_side_traction;

		if( handbrake && !w->front )
			w->ang_vel = 0.0f;
		if( rpm > max_rpm )
			w->ang_vel = max_rpm / VEH_RADS_TO_RPM;
	}
	v->avg_rpm = avg;
	if( v->num_wheels )
	{
		v->avg_side_slip = side / (float)v->num_wheels;
		v->avg_fwd_slip = fwd / (float)v->num_wheels;
	}
}

// GetYawDragMultiplier: none reversing (J-turns), more the faster it goes
float veh_car_yaw_drag( const veh_t *v )
{
	if( v->wheels_down < 1 )
		return 1.0f;
	if( veh_dot( v->velocity, v->fwd ) < 0.0f )
		return 0.0f;
	return veh_clamp( veh_length( v->velocity ) / 1000.0f, 0.0f, 1.0f );
}

void Veh_CarStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt )
{
	const veh_engine_params_t *e = &v->p->car.e;

	veh_car_think( v, e, in, dt );
	veh_wheels_think( v, dt );
	veh_car_wheels( v, e, in );

	f->drag_mult[2] = veh_car_yaw_drag( v );
	veh_wheels_physics( v, f, world, dt );
	veh_anti_slide( v, f, dt );
}

#endif // XASH_VEHICLES
