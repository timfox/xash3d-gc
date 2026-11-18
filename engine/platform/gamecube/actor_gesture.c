/*
actor_gesture.c - choreography gestures playing on studio actors (GameCube)
Copyright (C) 2026 xash3d-gc contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Source plays a scene's gestures as animation layers over whatever the actor
is doing. GoldSrc entities have no layers to send, and on GameCube the game
code and the renderer are one binary, so the game code hands each gesture
event straight to this table and the renderer (ref/gx/r_gx_gesture.c) asks
what is playing on an entity when it sets up its bones.
*/

#include "common.h"
#include "client.h"
#include "server.h"

#if XASH_GAMECUBE

#define GC_GESTURE_MAX	12
#define GC_GESTURE_TAGS	4
#define GC_GESTURE_FADE	0.2f	/* mstudioseqdesc_t fadeintime / fadeouttime of every gesture */

typedef struct
{
	int	ent;
	char	name[28];
	float	t0, t1;		/* client time */
	int	ntags;
	float	play[GC_GESTURE_TAGS];	/* fraction of the event */
	float	orig[GC_GESTURE_TAGS];	/* fraction of the sequence */
} gc_gesture_t;

static gc_gesture_t gc_gestures[GC_GESTURE_MAX];

/* A pose the game code holds on an actor (Source pose parameters: where the
 * head, spine and body are turned). The game sets it at think rate; the value
 * eases to each new setting so the turn is smooth between thinks. */
#define GC_POSE_MAX	8
#define GC_POSE_EASE	0.1f

typedef struct
{
	int	ent;
	char	name[28];
	float	from, to, time;
} gc_pose_t;

static gc_pose_t gc_poses[GC_POSE_MAX];

static float GC_ActorPoseValue( const gc_pose_t *p, float time )
{
	const float t = bound( 0.0f, ( time - p->time ) / GC_POSE_EASE, 1.0f );

	return p->from + ( p->to - p->from ) * t;
}

/* cycle 0..1 across the pose's range, 0.5 at rest */
void GC_ActorPose( int ent, const char *name, float cycle )
{
	gc_pose_t *p = NULL;
	int i;

	if( ent <= 0 || !name || !name[0] )
		return;
	for( i = 0; i < GC_POSE_MAX; i++ )
	{
		if( gc_poses[i].ent == ent && !Q_strcmp( gc_poses[i].name, name ))
		{
			p = &gc_poses[i];
			break;
		}
		if( !p && !gc_poses[i].ent )
			p = &gc_poses[i];
	}
	if( !p )
		return;
	cycle = bound( 0.0f, cycle, 1.0f );
	if( p->ent != ent )
	{
		p->ent = ent;
		Q_strncpy( p->name, name, sizeof( p->name ));
		p->from = p->to = 0.5f;
	}
	if( cycle == p->to )
		return;
	p->from = GC_ActorPoseValue( p, (float)cl.time );
	p->to = cycle;
	p->time = (float)cl.time;
}

/* start and end are server times; tags are ntags pairs of (event fraction,
 * sequence fraction), in order. */
void GC_ActorGesture( int ent, const char *name, float start, float end, int ntags, const float *tags )
{
	const float shift = (float)( cl.time - sv.time );
	gc_gesture_t *g = &gc_gestures[0];
	int i;

	if( ent <= 0 || !name || !name[0] || end <= start )
		return;
	for( i = 0; i < GC_GESTURE_MAX; i++ )
	{
		gc_gesture_t *c = &gc_gestures[i];

		if( !c->ent || c->t1 < (float)cl.time )
		{
			g = c;
			break;
		}
		if( c->t1 < g->t1 )
			g = c;
	}
	memset( g, 0, sizeof( *g ));
	g->ent = ent;
	Q_strncpy( g->name, name, sizeof( g->name ));
	g->t0 = start + shift;
	g->t1 = end + shift;
	g->ntags = bound( 0, ntags, GC_GESTURE_TAGS );
	for( i = 0; i < g->ntags; i++ )
	{
		g->play[i] = tags[i * 2];
		g->orig[i] = tags[i * 2 + 1];
	}
}

/* A scene's facial track playing on an actor (brows, eyelids, cheeks): a
 * baked file of face-shape weights over time. One actor speaks at a time. */
static struct
{
	int	ent;
	char	path[64];
	float	t0, t1;	/* client time */
} gc_face;

/* start and end are server times */
void GC_ActorFace( int ent, const char *track, float start, float end )
{
	const float shift = (float)( cl.time - sv.time );

	if( ent <= 0 || !track || !track[0] || end <= start )
		return;
	gc_face.ent = ent;
	Q_strncpy( gc_face.path, track, sizeof( gc_face.path ));
	gc_face.t0 = start + shift;
	gc_face.t1 = end + shift;
}

/* The track on ent at client time and how far into it, or NULL. */
const char *GC_ActorFaceAt( int ent, float time, float *seconds )
{
	if( gc_face.ent != ent || time < gc_face.t0 || time >= gc_face.t1 )
		return NULL;
	*seconds = time - gc_face.t0;
	return gc_face.path;
}

/* Headcrab canisters in flight (the monastery gun shelling the town). Game
 * code launches one; the renderer asks where each is.
 * CEnv_HeadcrabCanisterShared, launched from within the world: constant
 * horizontal speed over the flight time, and a vertical speed that goes
 * evenly from its launch value to the impact value (the impact direction's
 * z times the flight speed), so the path is a parabola that ends on the
 * impact point at the impact angle. */
#define GC_CANISTER_MAX	2

static struct
{
	float	t0, time;	/* client time of launch, seconds of flight; time 0 = free */
	float	launch[3], impact[3];
	float	vz0, vzf;
} gc_canister[GC_CANISTER_MAX];

void GC_CanisterLaunch( const float *launch, const float *impact, float pitch, float yaw, float flightTime, float flightSpeed )
{
	int i, slot = 0;

	(void)yaw;
	if( !launch || !impact || flightTime <= 0.0f )
		return;
	for( i = 0; i < GC_CANISTER_MAX; i++ )
	{
		if( gc_canister[i].time <= 0.0f || (float)cl.time >= gc_canister[i].t0 + gc_canister[i].time )
		{
			slot = i;
			break;
		}
	}
	gc_canister[slot].t0 = (float)cl.time;
	gc_canister[slot].time = flightTime;
	VectorCopy( launch, gc_canister[slot].launch );
	VectorCopy( impact, gc_canister[slot].impact );
	/* the canister comes in along the reverse of its resting forward */
	gc_canister[slot].vzf = sinf( pitch * ( M_PI_F / 180.0f )) * flightSpeed;
	gc_canister[slot].vz0 = ( 2.0f * ( impact[2] - launch[2] ) - flightTime * gc_canister[slot].vzf ) / flightTime;
}

/* Canisters in the air at client time: position and velocity of each. */
int GC_CanisterFlights( float time, float pos[][3], float vel[][3], int max )
{
	int i, n = 0;

	for( i = 0; i < GC_CANISTER_MAX && n < max; i++ )
	{
		const float T = gc_canister[i].time;
		const float t = time - gc_canister[i].t0;
		float u, accel;

		if( T <= 0.0f || t < 0.0f || t >= T )
			continue;
		u = t / T;
		accel = ( gc_canister[i].vzf - gc_canister[i].vz0 ) / T;
		pos[n][0] = gc_canister[i].launch[0] + ( gc_canister[i].impact[0] - gc_canister[i].launch[0] ) * u;
		pos[n][1] = gc_canister[i].launch[1] + ( gc_canister[i].impact[1] - gc_canister[i].launch[1] ) * u;
		pos[n][2] = gc_canister[i].launch[2] + gc_canister[i].vz0 * t + 0.5f * accel * t * t;
		vel[n][0] = ( gc_canister[i].impact[0] - gc_canister[i].launch[0] ) / T;
		vel[n][1] = ( gc_canister[i].impact[1] - gc_canister[i].launch[1] ) / T;
		vel[n][2] = gc_canister[i].vz0 + accel * t;
		n++;
	}
	return n;
}

void GC_ActorGestureClear( int ent )
{
	int i;

	for( i = 0; i < GC_GESTURE_MAX; i++ )
	{
		if( !ent || gc_gestures[i].ent == ent )
			gc_gestures[i].ent = 0;
	}
	for( i = 0; i < GC_POSE_MAX; i++ )
	{
		if( !ent || gc_poses[i].ent == ent )
			gc_poses[i].ent = 0;
	}
}

/* What plays on ent at client time: sequence cycle (0..1) and weight of each. */
int GC_ActorGesturesAt( int ent, float time, const char **names, float *cycles, float *weights, int max )
{
	int i, k, n = 0;

	/* held poses first: Source turns the body before it layers gestures */
	for( i = 0; i < GC_POSE_MAX && n < max; i++ )
	{
		const gc_pose_t *p = &gc_poses[i];
		float v;

		if( p->ent != ent )
			continue;
		v = GC_ActorPoseValue( p, time );
		if( v > 0.495f && v < 0.505f )
			continue;	/* at rest: the middle frame is no turn */
		names[n] = p->name;
		cycles[n] = v;
		weights[n] = 1.0f;
		n++;
	}
	for( i = 0; i < GC_GESTURE_MAX && n < max; i++ )
	{
		const gc_gesture_t *g = &gc_gestures[i];
		float p, p0 = 0.0f, o0 = 0.0f, p1 = 1.0f, o1 = 1.0f, w;

		if( g->ent != ent || time < g->t0 || time >= g->t1 )
			continue;
		p = ( time - g->t0 ) / ( g->t1 - g->t0 );
		/* CChoreoEvent::GetOriginalPercentageFromPlaybackPercentage: straight
		 * between the tags on either side */
		for( k = 0; k < g->ntags; k++ )
		{
			if( p >= g->play[k] )
			{
				p0 = g->play[k];
				o0 = g->orig[k];
			}
			else
			{
				p1 = g->play[k];
				o1 = g->orig[k];
				break;
			}
		}
		names[n] = g->name;
		cycles[n] = p1 > p0 ? o0 + ( o1 - o0 ) * ( p - p0 ) / ( p1 - p0 ) : o0;
		w = Q_min( time - g->t0, g->t1 - time ) / GC_GESTURE_FADE;
		weights[n] = bound( 0.0f, w, 1.0f );
		n++;
	}
	return n;
}

#endif // XASH_GAMECUBE
