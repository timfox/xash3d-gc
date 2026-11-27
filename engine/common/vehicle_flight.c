/*
vehicle_flight.c - arcade flight model

Thrust along the nose, drag along the path, lift that grows with the square
of airspeed and collapses below the stall speed, and a sideslip/sink kill that
makes the aircraft fly where it points. Body rates follow the controls with a
response lag and lose authority at low airspeed. A banked aircraft turns
(coordinated turn), so rolling and pulling is how you turn.

Two control schemes:
 - mouse aim (mouse_aim 1, default): the view is the target; an autopilot
   pitches, banks and uses a little rudder to bring the nose onto it. This is
   what works with both a mouse and the GameCube C-stick.
 - stick (mouse_aim 0): forward/back pitch, left/right roll.
Throttle is a lever: jump/duck (or the up axis) move it, and in mouse-aim mode
forward/back move it too.
*/
#include "vehicle_local.h"

#if XASH_VEHICLES

void Veh_FlightDefaults( veh_flight_params_t *p )
{
	veh_common_params_t *c = &p->c;

	veh_set( c->hull_mins, -32.0f, -32.0f, -16.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 48.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.2f;
	c->impact_speed = 200.0f;
	c->cam_dist = 300.0f;
	c->cam_height = 64.0f;
	c->cam_pitch = 4.0f;
	c->cam_lag = 5.0f;
	c->cam_follow = 0.0f;
	veh_set( c->seat, 16.0f, 0.0f, 24.0f );

	p->max_thrust = 700.0f;
	p->throttle_rate = 0.6f;
	p->drag = 0.35f;
	p->side_drag = 2.0f;
	p->cruise_speed = 700.0f;
	p->stall_speed = 350.0f;
	p->max_speed = 1600.0f;
	p->pitch_rate = 70.0f;
	p->roll_rate = 160.0f;
	p->yaw_rate = 25.0f;
	p->rate_response = 6.0f;
	p->control_speed = 500.0f;
	p->mouse_aim = 1.0f;
	p->aim_gain = 3.0f;
	p->bank_max = 70.0f;
	p->rotate_speed = 400.0f;
	p->ground_friction = 150.0f;
	p->crash_speed = 600.0f;
}

static void Veh_FlightControls( const veh_t *v, const veh_flight_params_t *p, const veh_input_t *in, float cmd[3] )
{
	if( p->mouse_aim > 0.5f )
	{
		float dir[3], dx, dy, dz, bank, bank_want;

		veh_angle_forward( in->aim, dir );
		dx = veh_dot( dir, v->fwd );
		dy = veh_dot( dir, v->left );
		dz = veh_dot( dir, v->up );

		// pitch toward the target; if it is behind, pull hard the short way
		cmd[0] = ( dx > 0.0f ) ? veh_clamp( dz * p->aim_gain, -1.0f, 1.0f ) : ( dz >= 0.0f ? 1.0f : -1.0f );

		// bank into the turn: target left (dy > 0) means left wing down (negative roll)
		bank = atan2f( v->left[2], v->up[2] ) * VEH_RAD2DEG;
		bank_want = -veh_clamp( dy * p->aim_gain, -1.0f, 1.0f ) * p->bank_max;
		if( dx > 0.95f )
			bank_want *= ( 1.0f - dx ) * 20.0f > 1.0f ? 1.0f : ( 1.0f - dx ) * 20.0f;	// level off on target
		cmd[1] = veh_clamp(( bank_want - bank ) / 45.0f + in->steer, -1.0f, 1.0f );

		// a touch of rudder for the last few degrees
		cmd[2] = veh_clamp( dy * p->aim_gain * 0.5f, -1.0f, 1.0f );
	}
	else
	{
		cmd[0] = -in->throttle;	// stick forward: nose down
		cmd[1] = in->steer;
		cmd[2] = 0.0f;
	}
}

void Veh_FlightStep( veh_t *v, const veh_input_t *in, const veh_world_t *world )
{
	const veh_flight_params_t *p = &v->p->flight;
	const float dt = VEH_STEP_DT;
	const float g = world->gravity * p->c.gravity_scale;
	const int was_on_ground = v->on_ground;
	const float max_rates[3] = { p->pitch_rate, p->roll_rate, p->yaw_rate };
	float cmd[3], lever_in, airspeed, authority, k, lift, perp[3];
	int i;

	v->impact_speed = 0.0f;

	// throttle lever
	lever_in = in->lift + ( p->mouse_aim > 0.5f ? in->throttle : 0.0f );
	v->throttle_lever = veh_clamp( v->throttle_lever + veh_clamp( lever_in, -1.0f, 1.0f ) * p->throttle_rate * dt, 0.0f, 1.0f );

	airspeed = veh_dot( v->velocity, v->fwd );
	authority = veh_clamp( airspeed / p->control_speed, 0.15f, 1.0f );

	Veh_FlightControls( v, p, in, cmd );
	if( v->on_ground )
	{
		// rolling: no roll, nose lifts only past rotate speed, rudder steers
		cmd[1] = 0.0f;
		if( airspeed < p->rotate_speed && cmd[0] > 0.0f )
			cmd[0] = 0.0f;
		cmd[2] = veh_clamp( cmd[2] * 2.0f - in->steer, -1.0f, 1.0f );
	}

	k = Veh_SmoothFactor( p->rate_response, dt );
	for( i = 0; i < 3; i++ )
		v->rates[i] += ( cmd[i] * max_rates[i] * authority - v->rates[i] ) * k;

	veh_rotate_body( v, v->rates[0] * VEH_DEG2RAD * dt, v->rates[1] * VEH_DEG2RAD * dt, v->rates[2] * VEH_DEG2RAD * dt );

	// coordinated turn: a bank swings the nose round, faster when slow
	if( !v->on_ground )
	{
		float turn = g / ( airspeed > p->stall_speed ? airspeed : p->stall_speed ) * -v->left[2];
		veh_rotate_yaw_world( v, turn * dt );
	}

	// stalled: the nose falls toward the ground
	if( !v->on_ground && airspeed < p->stall_speed )
	{
		float sink = ( 1.0f - veh_clamp( airspeed / p->stall_speed, 0.0f, 1.0f )) * 40.0f * VEH_DEG2RAD * dt;
		if( v->fwd[2] > -0.9f )
			veh_rotate_body( v, -sink, 0.0f, 0.0f );
	}
	veh_orthonormalize( v );

	// forces
	airspeed = veh_dot( v->velocity, v->fwd );
	veh_ma( v->velocity, v->throttle_lever * p->max_thrust * (( in->buttons & VEH_BTN_BOOST ) ? 1.3f : 1.0f ) * dt, v->fwd, v->velocity );
	veh_ma( v->velocity, -p->drag * dt, v->velocity, v->velocity );

	{
		float s = airspeed > 0.0f ? airspeed / p->cruise_speed : 0.0f;

		lift = g * s * s;
		if( airspeed < p->stall_speed )
		{
			float t = veh_clamp( airspeed / p->stall_speed, 0.0f, 1.0f );
			lift *= t * t;
		}
		/* Lift only ever carries the weight: climbing and turning come from
		 * pointing the nose (the sideslip kill below pulls the flight path
		 * onto it), so level flight holds altitude at any speed past cruise
		 * and a banked turn sinks a little unless the nose is raised. */
		lift = veh_clamp( lift, 0.0f, g );
		veh_ma( v->velocity, lift * dt, v->up, v->velocity );
	}

	// sideslip and sink kill, scaled by airspeed: it flies where it points
	airspeed = veh_dot( v->velocity, v->fwd );
	veh_ma( v->velocity, -airspeed, v->fwd, perp );
	veh_ma( v->velocity, -veh_clamp( p->side_drag * authority * dt, 0.0f, 1.0f ), perp, v->velocity );

	v->velocity[2] -= g * dt;

	// ground roll
	{
		static const float zero[3] = { 0.0f, 0.0f, 0.0f };
		float end[3];
		veh_trace_t tr;

		veh_copy( v->origin, end );
		end[2] += p->c.hull_mins[2] - 4.0f;
		world->trace( world->ctx, v->origin, zero, zero, end, &tr );
		v->on_ground = tr.fraction < 1.0f && tr.normal[2] > 0.7f && v->velocity[2] <= 1.0f;
		if( v->on_ground && !was_on_ground )
		{
			// touchdown: a hard one is an impact, too hard is a crash
			v->impact_speed = -v->velocity[2];
			if( v->impact_speed > p->crash_speed )
				v->events |= VEH_EV_CRASH;
			else if( v->impact_speed > p->c.impact_speed )
				v->events |= VEH_EV_IMPACT;
		}
		if( v->on_ground )
		{
			float vf = veh_dot( v->velocity, v->fwd );
			float drag = p->ground_friction * dt * (( in->buttons & VEH_BTN_ALTFIRE ) ? 4.0f : 1.0f );
			static const float world_up[3] = { 0.0f, 0.0f, 1.0f };

			if( v->velocity[2] < 0.0f )
				v->velocity[2] = 0.0f;
			if( v->throttle_lever < 0.05f )
				veh_ma( v->velocity, -veh_clamp( vf, -drag, drag ), v->fwd, v->velocity );
			// wings level, nose not below the horizon
			if( v->fwd[2] < 0.0f )
				veh_rotate_body( v, -v->fwd[2], 0.0f, 0.0f );
			// keep the wings level; the nose stays free once past rotate speed
			if( vf < p->rotate_speed || v->rates[0] <= 0.0f )
				veh_align_up( v, world_up, 8.0f, dt );
			veh_ma( v->velocity, -veh_dot( v->velocity, v->left ), v->left, v->velocity );
			veh_orthonormalize( v );
		}
	}

	{
		float speed = veh_length( v->velocity );

		if( speed > p->max_speed )
			veh_scale( v->velocity, p->max_speed / speed, v->velocity );
	}

	{
		float touchdown = v->impact_speed;

		v->impact_speed = 0.0f;
		veh_slide_move( v, world, dt );
		if( touchdown > v->impact_speed )
			v->impact_speed = touchdown;
	}
	if( v->impact_speed > p->crash_speed )
	{
		v->events |= VEH_EV_CRASH;
		veh_scale( v->velocity, 0.3f, v->velocity );
		v->throttle_lever *= 0.5f;
	}
	veh_track_ground_events( v, was_on_ground );
}

#endif // XASH_VEHICLES
