/*
vehicle_car.c - arcade car: raycast suspension, bicycle steering, handbrake drift

The handling target is the late-90s arcade racer: high grip until the
handbrake breaks the rear loose, then a held powerslide that keeps going while
the player steers into it and straightens out when the handbrake is released.
There is no tyre model. Grip is a lateral deceleration budget, steering is a
yaw rate from the bicycle model, and drift scales both.
*/
#include "vehicle_local.h"

#if XASH_VEHICLES

void Veh_CarDefaults( veh_car_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_ground_params_t *g = &p->g;

	// GoldSrc large hull (64x64x64), set so the box rides just above the wheels
	veh_set( c->hull_mins, -32.0f, -32.0f, -6.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 58.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.25f;
	c->impact_speed = 250.0f;
	c->cam_dist = 180.0f;
	c->cam_height = 56.0f;
	c->cam_pitch = 8.0f;
	c->cam_lag = 8.0f;
	c->cam_follow = 1.0f;
	veh_set( c->seat, -8.0f, 14.0f, 30.0f );

	g->wheel_x = 52.0f;
	g->wheel_y = 30.0f;
	g->wheel_z = 0.0f;
	g->wheel_radius = 12.0f;
	g->susp_length = 16.0f;
	g->susp_stiffness = 2400.0f;	// rests at a third of travel under 800 gravity
	g->susp_damping = 18.0f;
	g->align_rate = 10.0f;
	g->accel = 900.0f;
	g->max_speed = 1100.0f;
	g->reverse_accel = 500.0f;
	g->reverse_speed = 350.0f;
	g->brake = 1800.0f;
	g->roll_drag = 150.0f;
	g->boost_scale = 1.35f;
	g->grip = 3000.0f;
	g->steer_max = 32.0f;
	g->steer_rate = 160.0f;
	g->steer_falloff = 0.55f;
	g->wheelbase = 110.0f;
	g->air_control = 0.15f;

	p->drift_grip = 0.22f;
	p->drift_min_speed = 250.0f;
	p->drift_in_rate = 4.0f;
	p->drift_out_rate = 1.6f;
	p->drift_yaw_boost = 0.8f;
	p->handbrake_decel = 400.0f;
}

void Veh_CarStep( veh_t *v, const veh_input_t *in, const veh_world_t *world )
{
	const veh_car_params_t *p = &v->p->car;
	const veh_ground_params_t *g = &p->g;
	const float dt = VEH_STEP_DT;
	const int was_on_ground = v->on_ground;
	const int handbrake = ( in->buttons & VEH_BTN_HANDBRAKE ) != 0;
	const int boost = ( in->buttons & VEH_BTN_BOOST ) != 0;
	float traction, vf, speed_frac, target, steer_lim, yaw_rate, grip;

	v->velocity[2] -= world->gravity * p->c.gravity_scale * dt;

	v->on_ground = veh_ground_probe( v, g, world, dt ) > 0;
	traction = (float)v->wheels_down / (float)v->num_wheels;
	vf = veh_dot( v->velocity, v->fwd );
	speed_frac = veh_clamp( veh_absf( vf ) / g->max_speed, 0.0f, 1.0f );

	// drift factor: builds while the handbrake is held at speed
	if( handbrake && v->on_ground && veh_absf( vf ) > p->drift_min_speed )
		v->drift = veh_approach( v->drift, 1.0f, p->drift_in_rate * dt );
	else
		v->drift = veh_approach( v->drift, 0.0f, p->drift_out_rate * dt );

	// drive: the handbrake cuts drive and drags, it does not lock everything
	if( handbrake )
	{
		float drag = p->handbrake_decel * traction * dt;

		vf = veh_dot( v->velocity, v->fwd );
		veh_ma( v->velocity, -veh_clamp( vf, -drag, drag ), v->fwd, v->velocity );
	}
	else
	{
		veh_ground_drive( v, g, in->throttle, boost ? g->boost_scale : 1.0f, boost ? g->boost_scale : 1.0f, traction, dt );
	}

	grip = g->grip * ( 1.0f + ( p->drift_grip - 1.0f ) * v->drift );
	veh_ground_grip( v, grip, traction, dt );

	// steering: wheel angle chases the stick, less of it at speed
	steer_lim = g->steer_max * ( 1.0f - g->steer_falloff * speed_frac );
	target = in->steer * steer_lim;
	v->steer_angle = veh_approach( v->steer_angle, target, g->steer_rate * dt );

	// bicycle model, small angle: yaw rate = speed * angle / wheelbase (right = negative yaw)
	vf = veh_dot( v->velocity, v->fwd );
	yaw_rate = -( v->steer_angle * VEH_DEG2RAD ) * vf / g->wheelbase;
	yaw_rate *= 1.0f + p->drift_yaw_boost * v->drift;
	yaw_rate = veh_clamp( yaw_rate, -4.2f, 4.2f );	// ~240 deg/s
	if( !v->on_ground )
		yaw_rate *= g->air_control;
	else
		yaw_rate *= 0.35f + 0.65f * traction;

	veh_rotate_yaw_world( v, yaw_rate * dt );
	veh_orthonormalize( v );

	v->impact_speed = 0.0f;
	veh_slide_move( v, world, dt );
	veh_track_ground_events( v, was_on_ground );
}

#endif // XASH_VEHICLES
