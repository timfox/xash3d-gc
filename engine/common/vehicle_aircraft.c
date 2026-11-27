/*
vehicle_aircraft.c - Glide's plane and helicopter (base_glide_aircraft's
SimulatePlane and SimulateHelicopter, base_glide_plane, base_glide_heli)

Both fly on forces, not on rails: Glide's lift, drag and control torques on
the rigid body in vehicle.c, landing on raycast wheels (the plane's gear, the
helicopter's skids). Mouse aim is Glide's MOUSE_FLY_MODE.AIM: the view
direction becomes pitch and rudder input, so the aircraft turns toward where
the player looks. With mouse_aim 0 the keys fly it directly.
*/
#include <string.h>
#include "vehicle_local.h"

#if XASH_VEHICLES

static void veh_aircraft_wheels( veh_wheel_params_t *w )
{
	// base_glide_aircraft CreateWheel tweaks
	w->fwd_traction_max = 50000.0f;
	w->side_traction_mult = 200.0f;
	w->brake_power = 1000.0f;
	w->side_traction_max_ang = 25.0f;
	w->side_traction_max = 2400.0f;
	w->side_traction_min = 800.0f;
	w->rolling_resistance = 0.05f;
	w->surface_grip = 1.0f;
	w->susp_length = 10.0f;
	w->spring_strength = 1000.0f;
	w->spring_damper = 4000.0f;
}

void Veh_PlaneDefaults( veh_plane_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_wheel_params_t *w = &c->wheel;

	veh_set( c->hull_mins, -32.0f, -32.0f, -40.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 24.0f );
	// Glide's Stunt plane: 800 kg, tail dragger; a 250x280x80 unit airframe
	c->mass = 800.0f;
	veh_set( c->inertia, 4.5f, 3.7f, 7.6f );
	veh_set( c->ang_drag, -2.0f, -2.0f, -10.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.2f;
	c->impact_speed = 250.0f;
	c->crash_speed = 600.0f;
	c->cam_dist = 340.0f;
	c->cam_height = 60.0f;
	c->cam_pitch = 4.0f;
	c->cam_lag = 6.0f;
	c->cam_follow = 0.0f;
	veh_set( c->seat, -5.0f, 0.0f, 10.0f );

	veh_aircraft_wheels( w );
	w->brake_power = 800.0f;	// base_glide_plane
	w->front_x = 56.0f;		// main gear
	w->front_z = -40.0f;
	w->y = 40.0f;
	w->rear_x = -145.0f;		// tail wheel
	w->rear_z = -8.0f;
	w->radius = 8.0f;

	// PlaneParams. The speeds are Glide's scaled by 0.6: Glide's 1600 unit/s
	// lift speed needs a runway longer than a GoldSrc map is wide.
	veh_set( p->lift_ang_drag, -5.0f, -10.0f, -3.0f );
	p->lift_fwd_drag = 0.1f;
	p->lift_side_drag = 3.0f;
	p->lift_factor = 0.15f;
	p->max_speed = 1080.0f;
	p->lift_speed = 960.0f;
	p->control_speed = 720.0f;
	p->engine_force = 200.0f;
	p->align_force = 300.0f;
	p->pitch_force = 1000.0f;
	p->yaw_force = 500.0f;
	p->roll_force = 1200.0f;
	p->power_response = 0.15f;
	p->steer_max = 40.0f;
	p->steer_cone_max_speed = 800.0f;
	p->reverse_torque = 1000.0f;
	p->max_reverse_speed = -300.0f;
	p->mouse_aim = 1.0f;
	p->aim_gain = 15.0f;
	p->input_rate = 10.0f;
}

void Veh_HeliDefaults( veh_heli_params_t *p )
{
	veh_common_params_t *c = &p->c;
	veh_wheel_params_t *w = &c->wheel;

	veh_set( c->hull_mins, -32.0f, -32.0f, -20.0f );
	veh_set( c->hull_maxs, 32.0f, 32.0f, 44.0f );
	// base_glide 700 kg; the inertia of a 450x100x130 unit helicopter
	c->mass = 700.0f;
	veh_set( c->inertia, 1.4f, 11.7f, 11.4f );
	veh_set( c->ang_drag, -10.0f, -18.0f, -10.0f );
	c->gravity_scale = 1.0f;
	c->bounce = 0.2f;
	c->impact_speed = 200.0f;
	c->crash_speed = 500.0f;
	c->cam_dist = 380.0f;
	c->cam_height = 80.0f;
	c->cam_pitch = 6.0f;
	c->cam_lag = 6.0f;
	c->cam_follow = 0.0f;
	veh_set( c->seat, 40.0f, 16.0f, 20.0f );

	veh_aircraft_wheels( w );
	w->front_x = 50.0f;
	w->rear_x = -50.0f;
	w->front_z = w->rear_z = -10.0f;
	w->y = 40.0f;
	w->radius = 4.0f;

	// HelicopterParams
	p->base_power = 0.0f;
	veh_set( p->drag, 0.3f, 0.5f, 0.5f );
	p->max_fwd_drag = 200.0f;
	p->max_side_drag = 300.0f;
	p->turbulence = 50.0f;
	p->push_up_force = 250.0f;
	p->pitch_force = 700.0f;
	p->yaw_force = 700.0f;
	p->roll_force = 700.0f;
	p->push_fwd_force = 10.0f;
	p->max_speed = 2000.0f;
	p->upright_force = 1000.0f;
	p->power_response = 0.2f;
	p->mouse_aim = 1.0f;
	p->aim_gain = 8.0f;
	p->input_rate = 6.0f;
}

/*
glide/server/input.lua HandleMouseInput, MOUSE_FLY_MODE.AIM: pitch and rudder
that turn the nose toward the view direction, damped by the body rates.
*/
static void veh_mouse_aim( const veh_t *v, const veh_input_t *in, float gain, float dt, float *pitch, float *rudder )
{
	float target[3], rt[3];
	float pitch_drag = veh_clamp( v->angvel[1] * -0.1f, -3.0f, 3.0f ) * dt * 40.0f;
	float rudder_drag = veh_clamp( v->angvel[2] * 0.1f, -3.0f, 3.0f ) * dt * 40.0f;

	if( in->buttons & VEH_BTN_FREELOOK )
	{
		*pitch = *rudder = 0.0f;
		return;
	}
	veh_angle_forward( in->aim, target );
	veh_right( v, rt );
	*pitch = veh_clamp( veh_dot( target, v->up ) * -gain + pitch_drag, -1.0f, 1.0f );
	*rudder = veh_clamp( veh_dot( target, rt ) * gain + rudder_drag, -1.0f, 1.0f );
}

/*
==============================================================================
plane
==============================================================================
*/
static void veh_plane_think( veh_t *v, const veh_plane_params_t *p, const veh_input_t *in, float throttle, float dt )
{
	float pitch, yaw, roll = veh_clamp( in->steer, -1.0f, 1.0f );
	float fwd_speed = veh_dot( v->velocity, v->fwd ), total = veh_length( v->velocity );
	float torque = 0.0f, side = 0.0f, inv, s;
	int i, grounded = 0;

	if( p->mouse_aim > 0.5f )
		veh_mouse_aim( v, in, p->aim_gain, dt, &pitch, &yaw );
	else
	{
		pitch = veh_clamp( in->throttle, -1.0f, 1.0f );	// stick: forward is nose down
		yaw = 0.0f;
	}
	v->in_pitch = veh_expdecay( v->in_pitch, pitch, p->input_rate, dt );
	v->in_roll = veh_expdecay( v->in_roll, roll, p->input_rate, dt );
	v->in_yaw = veh_expdecay( v->in_yaw, yaw, p->input_rate, dt );

	// the engine starts on throttle, stops when the pilot gets out
	if( !v->engine_on && in->driven && throttle > 0.0f )
		v->engine_on = 1;
	else if( !in->driven && v->engine_on )
	{
		v->engine_on = 0;
		v->plane_brake = 0.1f;
	}

	for( i = 0; i < v->num_wheels; i++ )
		grounded |= v->wheels[i].contact;

	if( v->engine_on )
	{
		if( grounded )
			v->power = veh_approach( v->power, 1.0f + veh_clamp( throttle, -0.2f, 1.0f ), dt * p->power_response );
		else
		{
			float response = throttle < 0.0f ? p->power_response * 0.75f : p->power_response;

			v->power = veh_approach( v->power, 1.0f + throttle, dt * response );
			if( throttle < 0.0f && v->power < 0.8f )
				v->power = 0.8f;
		}
	}
	else
		v->power = v->power > 0.0f ? veh_approach( v->power, 0.0f, dt * p->power_response * 0.6f ) : 0.0f;

	// UpdatePlaneWheels: brakes, a little reverse, nose wheel steering
	if( !in->driven )
		;	// keep the parking brake set on exit
	else if( throttle < 0.0f && fwd_speed < 100.0f )
	{
		v->plane_brake = 0.1f;
		if( fwd_speed > p->max_reverse_speed )
			torque = -p->reverse_torque;
	}
	else if( throttle < 0.0f && fwd_speed > 0.0f )
		v->plane_brake = 1.0f;
	else
		v->plane_brake = 0.5f;

	for( i = 0; i < v->num_wheels; i++ )
	{
		veh_wheel_t *w = &v->wheels[i];

		w->brake = v->plane_brake;
		w->torque = torque;
		w->fwd_mult = w->side_mult = 1.0f;
		if( w->contact )
			side += w->side_slip;
	}
	side = v->num_wheels ? veh_clamp( side / (float)v->num_wheels, -1.0f, 1.0f ) : 0.0f;

	inv = 1.0f - veh_clamp( total / p->steer_cone_max_speed, 0.0f, 0.9f );
	s = v->in_yaw * inv;
	s = veh_clamp( s + veh_clamp( side * ( 1.0f - inv ), -0.5f, 0.5f ), -1.0f, 1.0f );
	v->steer_angle = s * -p->steer_max;
}

// SimulatePlane
static void veh_plane_forces( veh_t *v, const veh_plane_params_t *p, veh_forces_t *f, float throttle, float dt )
{
	static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
	float local[3], rt[3], vel[3], speed, lift, max_speed, control = 1.0f;
	int i, grounded = v->wheels_down > 0;

	veh_to_local( v, v->velocity, local );
	veh_right( v, rt );
	speed = local[0];

	lift = veh_clamp( veh_absf( speed ) / p->lift_speed, 0.0f, 1.0f );
	lift *= lift;

	// drag along and across the flight path
	veh_ma( f->lin, -veh_clamp( speed, -500.0f, 500.0f ) * p->lift_fwd_drag * lift, v->fwd, f->lin );
	veh_ma( f->lin, local[1] * p->lift_side_drag * lift, rt, f->lin );
	for( i = 0; i < 3; i++ )
		f->drag[i] += p->lift_ang_drag[i] * lift;

	// lift: take away a share of the sink across the wings, every tick
	veh_ma( f->lin, ( -local[2] * lift * p->lift_factor ) / dt, v->up, f->lin );

	// weathervane toward the direction of travel
	veh_copy( v->velocity, vel );
	if( veh_normalize( vel ) > 1e-3f )
	{
		veh_add_torque( v, f, 1, -veh_dot( vel, v->up ) * p->align_force );
		veh_add_torque( v, f, 2, -veh_dot( vel, rt ) * p->align_force );
	}
	// a bank pulls the nose round a little
	veh_add_torque( v, f, 2, veh_dot( world_up, rt ) * p->yaw_force * 0.2f );

	// speed limit, and less authority beyond it
	max_speed = veh_remap( v->power, 0.0f, 2.0f, p->lift_speed, p->max_speed );
	if( speed > max_speed )
	{
		control = 1.0f + veh_clamp( 1.0f - speed / max_speed, -1.0f, 0.0f ) * 0.75f;
		if( speed > max_speed * 1.2f )
			veh_ma( f->lin, p->engine_force * -2.0f, v->fwd, f->lin );
	}

	// engine: with no input in the air it holds its power
	if( !grounded && veh_absf( throttle ) < 0.1f )
		throttle = v->power < 1.0f ? v->power : 1.0f;
	if( throttle > 0.0f && speed < max_speed )
		veh_ma( f->lin, p->engine_force * v->power * throttle, v->fwd, f->lin );
	else if( throttle < 0.0f && speed > p->lift_speed * 0.8f )
		veh_ma( f->lin, p->engine_force * ( v->power < 1.0f ? v->power : 1.0f ) * throttle, v->fwd, f->lin );

	// control surfaces bite with airspeed
	control *= veh_clamp( veh_absf( speed * 0.5f ) / p->control_speed, 0.0f, 1.0f );
	veh_add_torque( v, f, 0, v->in_roll * p->roll_force * control );
	veh_add_torque( v, f, 1, v->in_pitch * p->pitch_force * control );
	veh_add_torque( v, f, 2, -v->in_yaw * p->yaw_force * control );
}

void Veh_PlaneStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt )
{
	const veh_plane_params_t *p = &v->p->plane;
	float throttle;

	// mouse aim: forward/back or jump/duck is the throttle; stick: jump/duck only
	if( p->mouse_aim > 0.5f )
		throttle = veh_clamp( in->throttle + in->lift, -1.0f, 1.0f );
	else
		throttle = veh_clamp( in->lift, -1.0f, 1.0f );
	if( !in->driven )
		throttle = 0.0f;

	v->altitude = veh_altitude( v, world );
	veh_plane_think( v, p, in, throttle, dt );
	veh_wheels_think( v, dt );
	veh_wheels_physics( v, f, world, dt );
	veh_plane_forces( v, p, f, throttle, dt );
	veh_anti_slide( v, f, dt );
}

/*
==============================================================================
helicopter
==============================================================================
*/
static void veh_heli_think( veh_t *v, const veh_heli_params_t *p, const veh_input_t *in, float throttle, float dt )
{
	float pitch, yaw, roll = veh_clamp( in->steer, -1.0f, 1.0f );
	int i;

	if( p->mouse_aim > 0.5f )
	{
		veh_mouse_aim( v, in, p->aim_gain, dt, &pitch, &yaw );
		v->in_pitch = pitch;
		v->in_roll = veh_expdecay( v->in_roll, roll, p->input_rate, dt );
		v->in_yaw = yaw;
	}
	else
	{
		pitch = veh_clamp( in->throttle, -1.0f, 1.0f );
		yaw = 0.0f;
		v->in_pitch = veh_expdecay( v->in_pitch, pitch, p->input_rate, dt );
		v->in_roll = veh_expdecay( v->in_roll, roll, p->input_rate, dt );
		v->in_yaw = veh_expdecay( v->in_yaw, yaw, p->input_rate, dt );
	}

	// the rotor spins up when the pilot gets in and winds down after
	v->engine_on = in->driven;
	if( v->engine_on )
	{
		float offset = throttle * 0.2f;

		if( v->altitude < 25.0f && throttle < 0.1f )
			offset -= 0.1f;
		v->power = veh_approach( v->power, 1.0f + offset, dt * p->power_response );
	}
	else
		v->power = v->power > 0.0f ? veh_approach( v->power, 0.0f, dt * p->power_response * 0.5f ) : 0.0f;

	// skids: always a bit of brake, no drive
	for( i = 0; i < v->num_wheels; i++ )
	{
		v->wheels[i].brake = 0.5f;
		v->wheels[i].torque = 0.0f;
		v->wheels[i].fwd_mult = v->wheels[i].side_mult = 1.0f;
	}
	v->steer_angle = 0.0f;
}

// SimulateHelicopter, scaled by how much the rotor is turning
static void veh_heli_forces( veh_t *v, const veh_heli_params_t *p, const veh_world_t *world, veh_forces_t *f, float throttle )
{
	static const float world_up[3] = { 0.0f, 0.0f, 1.0f };
	float power = veh_clamp( p->base_power + v->power, 0.0f, 1.0f );
	float eff, local[3], rt[3], g, align, input_mult, t;

	if( power <= 0.1f )
		return;
	eff = power;
	input_mult = v->engine_on ? 1.0f : 0.0f;
	veh_to_local( v, v->velocity, local );
	veh_right( v, rt );

	// drag: forward and sideways capped, vertical not
	veh_ma( f->lin, -veh_clamp( local[0], -p->max_fwd_drag, p->max_fwd_drag ) * p->drag[0] * eff, v->fwd, f->lin );
	veh_ma( f->lin, veh_clamp( local[1], -p->max_side_drag, p->max_side_drag ) * p->drag[1] * eff, rt, f->lin );
	veh_ma( f->lin, -local[2] * p->drag[2] * eff, v->up, f->lin );

	// lift holds the weight along the rotor axis, with a bit more when tilted
	g = world->gravity * v->p->c.gravity_scale;
	align = veh_dot( v->up, world_up );
	veh_ma( f->lin, g * eff, v->up, f->lin );
	veh_ma( f->lin, g * ( 0.75f - veh_clamp( veh_absf( align ), 0.0f, 0.75f )) * eff, world_up, f->lin );
	veh_ma( f->lin, p->push_up_force * throttle * input_mult * eff, v->up, f->lin );

	// cyclic and tail rotor
	veh_add_torque( v, f, 0, v->in_roll * p->roll_force * input_mult * eff );
	veh_add_torque( v, f, 1, v->in_pitch * p->pitch_force * input_mult * eff );
	veh_add_torque( v, f, 2, -v->in_yaw * p->yaw_force * input_mult * eff );

	// keep upright, less so while the stick is held over
	veh_add_torque( v, f, 0, veh_dot( rt, world_up ) * p->upright_force * ( 1.0f - veh_absf( v->in_roll ) * 0.8f ) * eff );
	veh_add_torque( v, f, 1, veh_dot( v->fwd, world_up ) * p->upright_force * ( 1.0f - veh_absf( v->in_pitch ) * 0.8f ) * eff );

	if( local[0] < p->max_speed && local[0] > -p->max_speed )
		veh_ma( f->lin, v->in_pitch * p->push_fwd_force * eff, v->fwd, f->lin );

	if( v->altitude < 25.0f && throttle < 0.1f )
		veh_ma( f->lin, -200.0f * eff, world_up, f->lin );	// stick to the ground
	else
	{
		// turbulence
		t = (float)v->steps * VEH_STEP_DT;
		veh_add_torque( v, f, 0, cosf( t * 2.0f ) * p->turbulence );
		veh_add_torque( v, f, 1, cosf( t * 1.5f ) * p->turbulence * 0.5f );
	}
}

void Veh_HeliStep( veh_t *v, const veh_input_t *in, const veh_world_t *world, veh_forces_t *f, float dt )
{
	const veh_heli_params_t *p = &v->p->heli;
	float throttle;

	if( p->mouse_aim > 0.5f )
		throttle = veh_clamp( in->throttle + in->lift, -1.0f, 1.0f );
	else
		throttle = veh_clamp( in->lift, -1.0f, 1.0f );
	if( !in->driven )
		throttle = 0.0f;

	v->altitude = veh_altitude( v, world );
	veh_heli_think( v, p, in, throttle, dt );
	veh_wheels_think( v, dt );
	veh_wheels_physics( v, f, world, dt );
	veh_heli_forces( v, p, world, f, throttle );
	veh_anti_slide( v, f, dt );
}

#endif // XASH_VEHICLES
