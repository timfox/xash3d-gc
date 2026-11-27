/*
vehicle_tank.c - tracked tank: differential steering, pivot turns, slewing turret

The hull uses the same raycast suspension as the car, with track grip (no
sliding) and a turn rate instead of a wheel angle, so it pivots on the spot.
The turret slews toward the driver's view at a fixed rate within pitch
limits; the main gun raises VEH_EV_FIRE and kicks the hull back. What a shot
does (trace, damage, effects) belongs to the binding.
*/
#include "vehicle_local.h"

#if XASH_VEHICLES

void Veh_TankDefaults( veh_tank_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_ground_params_t *g = &p->g;

	veh_set( c->hull_mins, -32.0f, -32.0f, -4.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 60.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.05f;
	c->impact_speed = 160.0f;
	c->cam_dist = 260.0f;
	c->cam_height = 96.0f;
	c->cam_pitch = 6.0f;
	c->cam_lag = 6.0f;
	c->cam_follow = 0.0f;	// the view aims the turret; the hull turns under it
	veh_set( c->seat, 0.0f, 0.0f, 48.0f );

	g->wheel_x = 56.0f;
	g->wheel_y = 36.0f;
	g->wheel_z = 0.0f;
	g->wheel_radius = 10.0f;
	g->susp_length = 10.0f;
	g->susp_stiffness = 2400.0f;
	g->susp_damping = 22.0f;
	g->align_rate = 6.0f;
	g->accel = 300.0f;
	g->max_speed = 260.0f;
	g->reverse_accel = 200.0f;
	g->reverse_speed = 160.0f;
	g->brake = 900.0f;
	g->roll_drag = 400.0f;
	g->boost_scale = 1.0f;
	g->grip = 6000.0f;
	g->steer_max = 50.0f;	// deg/s turn rate at rest
	g->steer_rate = 0.0f;
	g->steer_falloff = 0.0f;
	g->wheelbase = 0.0f;
	g->air_control = 0.0f;

	p->pivot_speed_falloff = 0.6f;
	p->reverse_steer_invert = 1.0f;
	p->turret_yaw_rate = 45.0f;
	p->turret_pitch_rate = 20.0f;
	p->turret_pitch_min = -8.0f;
	p->turret_pitch_max = 20.0f;
	p->turret_height = 40.0f;
	p->barrel_length = 96.0f;
	p->fire_interval = 2.5f;
	p->recoil = 60.0f;
}

void Veh_TurretMuzzle( const veh_t *v, float org[3], float dir[3] )
{
	const veh_tank_params_t *p = &v->p->tank;
	float yaw = ( veh_yaw_of( v->fwd ) + v->turret_yaw ) * VEH_DEG2RAD;
	float pitch = v->turret_pitch * VEH_DEG2RAD;
	float cp = cosf( pitch );
	float pivot[3];

	// turret yaw is about the hull up axis; for the barrel use the hull's
	// heading plus a world pitch (hull tilt is small for a tank)
	dir[0] = cp * cosf( yaw );
	dir[1] = cp * sinf( yaw );
	dir[2] = sinf( pitch );
	veh_ma( v->origin, p->turret_height, v->up, pivot );
	veh_ma( pivot, p->barrel_length, dir, org );
}

void Veh_TankStep( veh_t *v, const veh_input_t *in, const veh_world_t *world )
{
	const veh_tank_params_t *p = &v->p->tank;
	const veh_ground_params_t *g = &p->g;
	const float dt = VEH_STEP_DT;
	const int was_on_ground = v->on_ground;
	float traction, vf, speed_frac, turn, throttle;

	v->velocity[2] -= world->gravity * p->c.gravity_scale * dt;

	v->on_ground = veh_ground_probe( v, g, world, dt ) > 0;
	traction = (float)v->wheels_down / (float)v->num_wheels;
	vf = veh_dot( v->velocity, v->fwd );
	speed_frac = veh_clamp( veh_absf( vf ) / g->max_speed, 0.0f, 1.0f );

	throttle = ( in->buttons & VEH_BTN_BRAKE ) ? 0.0f : in->throttle;
	if( in->buttons & VEH_BTN_BRAKE )
	{
		float drag = g->brake * traction * dt;
		veh_ma( v->velocity, -veh_clamp( vf, -drag, drag ), v->fwd, v->velocity );
	}
	else
		veh_ground_drive( v, g, throttle, 1.0f, 1.0f, traction, dt );
	veh_ground_grip( v, g->grip, traction, dt );

	// differential steering: full rate at rest, less at speed; tracks need ground
	turn = in->steer * g->steer_max * ( 1.0f - ( 1.0f - p->pivot_speed_falloff ) * speed_frac );
	if( p->reverse_steer_invert > 0.5f && vf < -10.0f )
		turn = -turn;
	veh_rotate_yaw_world( v, -turn * VEH_DEG2RAD * traction * dt );
	veh_orthonormalize( v );

	// turret: slew toward the view, relative to the hull heading
	{
		float hull_yaw = veh_yaw_of( v->fwd );
		float want_yaw = Veh_AngleMod( in->aim[1] - hull_yaw );
		float want_pitch = veh_clamp( -in->aim[0], p->turret_pitch_min, p->turret_pitch_max );
		float err = Veh_AngleMod( want_yaw - v->turret_yaw );

		v->turret_yaw = Veh_AngleMod( v->turret_yaw + veh_clamp( err, -p->turret_yaw_rate * dt, p->turret_yaw_rate * dt ));
		v->turret_pitch = veh_approach( v->turret_pitch, want_pitch, p->turret_pitch_rate * dt );
	}

	// main gun
	if( v->fire_cooldown > 0.0f )
		v->fire_cooldown -= dt;
	if(( in->buttons & VEH_BTN_FIRE ) && v->fire_cooldown <= 0.0f )
	{
		float org[3], dir[3];

		Veh_TurretMuzzle( v, org, dir );
		v->fire_cooldown = p->fire_interval;
		v->events |= VEH_EV_FIRE;
		// recoil along the ground only
		v->velocity[0] -= dir[0] * p->recoil;
		v->velocity[1] -= dir[1] * p->recoil;
	}

	v->impact_speed = 0.0f;
	veh_slide_move( v, world, dt );
	veh_track_ground_events( v, was_on_ground );
}

#endif // XASH_VEHICLES
