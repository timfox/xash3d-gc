/*
vehicle_tank.c - Glide's tank: base_glide_car on track wheels with Glide's
tank engine numbers, turn in place and a turret (base_glide_tank)

The tracks are wheels with a lot of grip whose steering angle runs front +1
to rear -1 (Glide's Rhino), so the hull turns about its middle. Standing
still with only steering held, the engine drives one side forward and the
other back instead (isTurningInPlace).
*/
#include <string.h>
#include "vehicle_local.h"

#if XASH_VEHICLES

void Veh_TankDefaults( veh_tank_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_wheel_params_t *w = &c->wheel;
	veh_engine_params_t *e = &p->e;

	veh_set( c->hull_mins, -32.0f, -32.0f, -16.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 48.0f );
	// base_glide_tank: 20 t; inertia of a 340x150x110 unit hull
	c->mass = 20000.0f;
	veh_set( c->inertia, 1.9f, 6.8f, 7.4f );
	veh_set( c->ang_drag, -0.5f, -0.5f, -3.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.05f;
	c->impact_speed = 160.0f;
	c->crash_speed = 0.0f;
	c->cam_dist = 300.0f;
	c->cam_height = 100.0f;
	c->cam_pitch = 6.0f;
	c->cam_lag = 6.0f;
	c->cam_follow = 0.0f;	// the view aims the turret; the hull turns under it
	veh_set( c->seat, 0.0f, 0.0f, 56.0f );

	// Glide's Rhino: four wheels at (88 | -76, +-55, -5), tank wheel params
	w->front_x = 88.0f;
	w->rear_x = -76.0f;
	w->front_z = w->rear_z = -5.0f;
	w->y = 55.0f;
	w->radius = 15.0f;
	w->susp_length = 15.0f;
	w->spring_strength = 6000.0f;
	w->spring_damper = 30000.0f;
	w->brake_power = 20000.0f;
	w->fwd_traction_max = 50000.0f;
	w->side_traction_mult = 800.0f;
	w->side_traction_max_ang = 25.0f;
	w->side_traction_max = 12000.0f;
	w->side_traction_min = 10000.0f;
	w->rolling_resistance = 0.05f;
	w->surface_grip = 1.0f;

	veh_engine_defaults( e );
	e->engine_brake_torque = 40000.0f;
	e->min_rpm_torque = 55000.0f;	// Rhino
	e->max_rpm_torque = 40000.0f;
	e->diff_ratio = 0.75f;
	e->efficiency = 1.0f;
	e->power_distribution = 0.0f;
	e->gear_count = 1.0f;
	e->gear_reverse = 3.0f;
	e->gear[0] = 3.0f;
	e->max_steer = 30.0f;
	e->steer_cone_rate = 8.0f;
	e->steer_cone_max_speed = 500.0f;
	e->steer_cone_max_angle = 0.25f;
	e->counter_steer = 0.75f;
	e->unflip_force = 0.2f;
	veh_set( e->air_control, 0.08f, 0.03f, 0.02f );

	p->wheels_per_side = 2.0f;
	p->turn_in_place = 3.0f;
	p->turret_height = 50.0f;
	p->barrel_length = 120.0f;
	p->turret_pitch_up = 25.0f;
	p->turret_pitch_down = 10.0f;
	p->turret_yaw_speed = 50.0f;
	p->turret_yaw_accel = 1500.0f;
	p->fire_interval = 2.0f;
	p->recoil = 50.0f;
}

static void veh_turret_dir( const veh_t *v, float dir[3] )
{
	float yaw = v->turret_yaw * VEH_DEG2RAD, pitch = v->turret_pitch * VEH_DEG2RAD;
	float cp = cosf( pitch );

	veh_scale( v->fwd, cp * cosf( yaw ), dir );
	veh_ma( dir, cp * sinf( yaw ), v->left, dir );
	veh_ma( dir, sinf( pitch ), v->up, dir );
}

void Veh_TurretMuzzle( const veh_t *v, float org[3], float dir[3] )
{
	const veh_tank_params_t *p = &v->p->tank;
	float pivot[3];

	veh_turret_dir( v, dir );
	veh_ma( v->origin, p->turret_height, v->up, pivot );
	veh_ma( pivot, p->barrel_length, dir, org );
}

// UpdateTurret: the barrel follows the aim within its limits
static void veh_turret( veh_t *v, const veh_tank_params_t *p, const veh_input_t *in, float dt )
{
	float aim[3], local[3], target_yaw, target_pitch, diff;

	veh_angle_forward( in->aim, aim );
	veh_to_local( v, aim, local );
	target_yaw = atan2f( local[1], local[0] ) * VEH_RAD2DEG;
	target_pitch = asinf( veh_clamp( local[2], -1.0f, 1.0f )) * VEH_RAD2DEG;
	target_pitch = veh_clamp( target_pitch, -p->turret_pitch_down, p->turret_pitch_up );

	v->turret_pitch = veh_expdecay( v->turret_pitch, target_pitch, 10.0f, dt );
	diff = Veh_AngleMod( target_yaw - v->turret_yaw );
	v->turret_yaw += veh_clamp( diff * p->turret_yaw_accel * 0.01f, -p->turret_yaw_speed, p->turret_yaw_speed ) * dt;
	v->turret_yaw = Veh_AngleMod( v->turret_yaw );
}

static void veh_tank_fire( veh_t *v, const veh_tank_params_t *p, const veh_input_t *in, float dt )
{
	if( v->fire_cooldown > 0.0f )
		v->fire_cooldown -= dt;
	if(( in->buttons & VEH_BTN_FIRE ) && v->fire_cooldown <= 0.0f )
	{
		float org[3], dir[3], kick[3];

		Veh_TurretMuzzle( v, org, dir );
		v->fire_cooldown = p->fire_interval;
		v->events |= VEH_EV_FIRE;
		// recoil: ApplyForceOffset( dir * mass * -TurretRecoilForce, muzzle )
		veh_scale( dir, -v->p->c.mass * p->recoil, kick );
		veh_apply_impulse( v, org, kick );
	}
}

/*
EngineThink and UpdateSteering overrides: turning in place drives the left
track one way and the right the other, with the wheels splayed to 70 degrees.
*/
static int veh_tank_turn_in_place( veh_t *v, const veh_tank_params_t *p, const veh_input_t *in, float dt )
{
	const veh_engine_params_t *e = &p->e;
	float throttle_in = in->throttle > 0.0f ? in->throttle : 0.0f;
	float brake_in = in->throttle < 0.0f ? -in->throttle : 0.0f;
	float steer = veh_clamp( in->steer, -1.0f, 1.0f );
	float range, power, torque;

	v->turning_in_place = p->turn_in_place > 0.0f && v->engine_on && v->wheels_down == v->num_wheels
		&& veh_absf( veh_dot( v->velocity, v->fwd )) < 100.0f && veh_absf( steer ) > 0.1f
		&& veh_absf( throttle_in + brake_in ) < 0.1f;
	if( !v->turning_in_place )
		return 0;

	v->gear = 1;
	v->engine_throttle = veh_expdecay( v->engine_throttle, veh_absf( steer ), 4.0f, dt );
	range = e->max_rpm - e->min_rpm;
	power = range > 0.0f ? ( Veh_EngineRPM( v ) - e->min_rpm ) / range : 0.0f;
	power = veh_expdecay( power, v->engine_throttle * 0.5f, 2.0f, dt );
	v->flywheel = ( e->min_rpm + range * power ) / VEH_RADS_TO_RPM;

	// steering right drives the left track forward
	torque = e->max_rpm_torque * p->turn_in_place * steer * v->engine_throttle;
	v->front_torque = torque;	// left side
	v->rear_torque = -torque;	// right side
	v->front_brake = v->rear_brake = 0.0f;
	v->front_traction = v->rear_traction = 1.0f;

	v->steer_input = veh_expdecay( v->steer_input, steer, 4.0f, dt );
	v->steer_angle = v->steer_input * -70.0f;
	return 1;
}

void Veh_TankStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt )
{
	const veh_tank_params_t *p = &v->p->tank;
	const veh_engine_params_t *e = &p->e;

	if( !veh_tank_turn_in_place( v, p, in, dt ))
		veh_car_think( v, e, in, dt );
	else
		veh_car_air( v, e, in, dt );

	veh_wheels_think( v, dt );
	veh_car_wheels( v, e, in );
	if( v->turning_in_place )
	{
		// each side's torque split over that side's wheels
		int i, left = 0, right = 0;

		for( i = 0; i < v->num_wheels; i++ )
		{
			if( v->wheels[i].left ) left++;
			else right++;
		}
		for( i = 0; i < v->num_wheels; i++ )
		{
			veh_wheel_t *w = &v->wheels[i];
			float share = 0.5f + e->power_distribution * 0.5f;

			if( w->left )
				w->torque = left ? share / (float)left * v->front_torque : 0.0f;
			else
				w->torque = right ? ( 1.0f - share ) / (float)right * v->rear_torque : 0.0f;
		}
	}

	if( in->driven )
		veh_turret( v, p, in, dt );
	veh_tank_fire( v, p, in, dt );

	f->drag_mult[2] = veh_car_yaw_drag( v );
	veh_wheels_physics( v, f, world, dt );
	veh_anti_slide( v, f, dt );
}

#endif // XASH_VEHICLES
