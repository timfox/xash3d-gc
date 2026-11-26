/*
cl_frame.c - client world snapshot
Copyright (C) 2008 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "net_encode.h"
#include "entity_types.h"
#include "pm_local.h"
#include "cl_tent.h"
#include "studio.h"
#include "sound.h"
#include "input.h"
#if XASH_GAMECUBE
#include "server.h"
#include "mod_local.h"
#include "platform/platform.h"
#endif

// #define STUDIO_INTERPOLATION_FIX

/*
=========================================================================

FRAME INTERPOLATION

=========================================================================
*/
/*
==================
CL_UpdatePositions

Store another position into interpolation circular buffer
==================
*/
static void CL_UpdatePositions( cl_entity_t *ent )
{
	position_history_t *prev = &ent->ph[ent->current_position & HISTORY_MASK];

	ent->current_position = (ent->current_position + 1) & HISTORY_MASK;
	position_history_t *ph = &ent->ph[ent->current_position];
	VectorCopy( ent->curstate.origin, ph->origin );
	VectorCopy( ent->curstate.angles, ph->angles );

	ph->animtime = ent->curstate.animtime;

	// a1ba: for some reason, this sometimes still may happen
	// at this time, I'm not sure whether this bug happens in delta readwrite code
	// or server just decides to go backwards and really sends these values
	if( ph->animtime < prev->animtime )
	{
		// try to deduce real animtime by looking up the difference between
		// server messages (cl.mtime is never modified ny the interpolation code)
		float diff = Q_max( 0, ent->curstate.msg_time - ent->prevstate.msg_time );

		ph->animtime = prev->animtime + diff;
	}
}

/*
==================
CL_ResetPositions

Interpolation init or reset after teleporting
==================
*/
static void CL_ResetPositions( cl_entity_t *ent )
{
	if( !ent )
		return;

	position_history_t store = ent->ph[ent->current_position & HISTORY_MASK];
	ent->current_position = 1;

	memset( ent->ph, 0, sizeof( position_history_t ) * HISTORY_MAX );
	ent->ph[1] = ent->ph[0] = store;
}

/*
==================
CL_EntityTeleported

check for instant movement in case
we don't want interpolate this
==================
*/
static qboolean CL_EntityTeleported( cl_entity_t *ent )
{
	vec3_t	delta;

	VectorSubtract( ent->curstate.origin, ent->prevstate.origin, delta );

	// compute potential max movement in units per frame and compare with entity movement
	float maxlen = ( clgame.movevars.maxvelocity * ( 1.0f / GAME_FPS ));
	float len = VectorLength( delta );

	return (len > maxlen);
}

/*
==================
CL_CompareTimestamps

round-off floating errors
==================
*/
static qboolean CL_CompareTimestamps( float t1, float t2 )
{
	int	iTime1 = t1 * 1000;
	int	iTime2 = t2 * 1000;

	return (( iTime1 - iTime2 ) <= 1 );
}

/*
==================
CL_EntityIgnoreLerp

some ents will be ignore lerping
==================
*/
static qboolean CL_EntityIgnoreLerp( cl_entity_t *e )
{
	if( cl_nointerp.value > 0.f )
		return true;

	if( e->model && e->model->type == mod_alias )
		return false;

	return (e->curstate.movetype == MOVETYPE_NONE) ? true : false;
}

/*
==================
CL_EntityCustomLerp

==================
*/
static qboolean CL_EntityCustomLerp( cl_entity_t *e )
{
	switch( e->curstate.movetype )
	{
	case MOVETYPE_NONE:
	case MOVETYPE_STEP:
	case MOVETYPE_WALK:
	case MOVETYPE_FLY:
	case MOVETYPE_COMPOUND:
		return false;
	}

	return true;
}

/*
==================
CL_ParametricMove

check for parametrical moved entities
==================
*/
static qboolean CL_ParametricMove( cl_entity_t *ent )
{
	vec3_t	delta;

	if( ent->curstate.starttime == 0.0f || ent->curstate.impacttime == 0.0f )
		return false;

	VectorSubtract( ent->curstate.endpos, ent->curstate.startpos, delta );
	float dt = ent->curstate.impacttime - ent->curstate.starttime;

	if( dt != 0.0f )
	{
		float t;

		if( ent->lastmove > cl.time )
			t = ent->lastmove;
		else t = cl.time;

		float frac = ( t - ent->curstate.starttime ) / dt;
		frac = bound( 0.0f, frac, 1.0f );
		VectorMA( ent->curstate.startpos, frac, delta, ent->curstate.origin );

		ent->lastmove = t;
	}

	VectorNormalize( delta );
	if( VectorLength( delta ) > 0.0f )
		VectorAngles( delta, ent->curstate.angles ); // re-aim projectile

	return true;
}

/*
====================
CL_UpdateLatchedVars

====================
*/
static void CL_UpdateLatchedVars( cl_entity_t *ent )
{
	if( !ent->model || ( ent->model->type != mod_alias && ent->model->type != mod_studio ))
		return; // below fields used only for alias and studio interpolation

	VectorCopy( ent->prevstate.origin, ent->latched.prevorigin );
	VectorCopy( ent->prevstate.angles, ent->latched.prevangles );

	if( ent->model->type == mod_alias )
		ent->latched.prevframe = ent->prevstate.frame;
	ent->latched.prevanimtime = ent->prevstate.animtime;

	if( ent->curstate.sequence != ent->prevstate.sequence )
	{
		memcpy( ent->latched.prevseqblending, ent->prevstate.blending, sizeof( ent->latched.prevseqblending ));
		ent->latched.prevsequence = ent->prevstate.sequence;
		ent->latched.sequencetime = ent->curstate.animtime;
	}

	memcpy( ent->latched.prevcontroller, ent->prevstate.controller, sizeof( ent->latched.prevcontroller ));
	memcpy( ent->latched.prevblending, ent->prevstate.blending, sizeof( ent->latched.prevblending ));

	// update custom latched vars
	if( clgame.drawFuncs.CL_UpdateLatchedVars != NULL )
		clgame.drawFuncs.CL_UpdateLatchedVars( ent, false );
}

/*
====================
CL_GetStudioEstimatedFrame

====================
*/
static float CL_GetStudioEstimatedFrame( cl_entity_t *ent )
{
	if( ent->model != NULL && ent->model->type == mod_studio )
	{
		studiohdr_t *pstudiohdr = (studiohdr_t *)Mod_StudioExtradata( ent->model );

		if( pstudiohdr && pstudiohdr->numseq > 0 )
		{
			int sequence = bound( 0, ent->curstate.sequence, pstudiohdr->numseq - 1 );
			mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)pstudiohdr + pstudiohdr->seqindex) + sequence;
			return ref.dllFuncs.R_StudioEstimateFrame( ent, pseqdesc, cl.time );
		}
	}

	return 0;
}

/*
====================
CL_ResetLatchedVars

====================
*/
void CL_ResetLatchedVars( cl_entity_t *ent, qboolean full_reset )
{
	if( !ent->model || ( ent->model->type != mod_alias && ent->model->type != mod_studio ))
		return; // below fields used only for alias and studio interpolation

	if( full_reset )
	{
		// don't modify for sprites to avoid broke sprite interp
		memcpy( ent->latched.prevblending, ent->curstate.blending, sizeof( ent->latched.prevblending ));
		ent->latched.sequencetime = ent->curstate.animtime;
		memcpy( ent->latched.prevcontroller, ent->curstate.controller, sizeof( ent->latched.prevcontroller ));
		if( ent->model->type == mod_studio )
			ent->latched.prevframe = CL_GetStudioEstimatedFrame( ent );
		else if( ent->model->type == mod_alias )
			ent->latched.prevframe = ent->curstate.frame;
		ent->prevstate = ent->curstate;
	}

	ent->latched.prevanimtime = ent->curstate.animtime = cl.mtime[0];
	VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
	VectorCopy( ent->curstate.angles, ent->latched.prevangles );
	ent->latched.prevsequence = ent->curstate.sequence;

	// update custom latched vars
	if( clgame.drawFuncs.CL_UpdateLatchedVars != NULL )
		clgame.drawFuncs.CL_UpdateLatchedVars( ent, true );
}

/*
==================
CL_ProcessEntityUpdate

apply changes since new frame received
==================
*/
static void CL_ProcessEntityUpdate( cl_entity_t *ent )
{
	ent->model = CL_ModelHandle( ent->curstate.modelindex );
	ent->index = ent->curstate.number;

	if( FBitSet( ent->curstate.entityType, ENTITY_NORMAL ))
		COM_NormalizeAngles( ent->curstate.angles );

	// a1ba: follow entities are sent with null origin, grab their aiment origin here
	//
	// null origin leads to triggered entity teleport check and subsequent reset of position history
	// and empty position history doesn't allow entity to render correctly
	//
	// it's probably should be done somewhere else, as goldsrc doesn't do this here
	// it has MoveAiments function but it's called after LinkPacketEntities :shrug:
	if( ent->curstate.movetype == MOVETYPE_FOLLOW && VectorIsNull( ent->curstate.origin ) && ent->curstate.aiment )
	{
		cl_entity_t *aiment = CL_GetEntityByIndex( ent->curstate.aiment );

		if( aiment )
			VectorCopy( aiment->origin, ent->curstate.origin );
	}

	qboolean parametric = ent->curstate.starttime != 0.0f && ent->curstate.impacttime != 0.0f;

	// allow interpolation on bmodels too
	if( ent->model && ent->model->type == mod_brush )
		ent->curstate.animtime = ent->curstate.msg_time;

	if( CL_EntityCustomLerp( ent ) && !parametric )
		ent->curstate.animtime = ent->curstate.msg_time;

	if( !CL_CompareTimestamps( ent->curstate.animtime, ent->prevstate.animtime ) || CL_EntityIgnoreLerp( ent ))
	{
		CL_UpdateLatchedVars( ent );
		CL_UpdatePositions( ent );
	}

	// g-cont. it should be done for all the players?
	if( ent->player && !FBitSet( host.features, ENGINE_COMPUTE_STUDIO_LERP ))
		ent->curstate.angles[PITCH] /= -3.0f;

	VectorCopy( ent->curstate.origin, ent->origin );
	VectorCopy( ent->curstate.angles, ent->angles );

	// initialize attachments for now
	VectorCopy( ent->origin, ent->attachment[0] );
	VectorCopy( ent->origin, ent->attachment[1] );
	VectorCopy( ent->origin, ent->attachment[2] );
	VectorCopy( ent->origin, ent->attachment[3] );
}

/*
==================
CL_FindInterpolationUpdates

find two timestamps
==================
*/
static qboolean CL_FindInterpolationUpdates( cl_entity_t *ent, double targettime, position_history_t **ph0, position_history_t **ph1 )
{
	qboolean	extrapolate = true;
	uint		imod = ent->current_position;
	uint		i0 = (imod - 0) & HISTORY_MASK;	// curpos (lerp end)
	uint		i1 = (imod - 1) & HISTORY_MASK;	// oldpos (lerp start)

	for( uint i = 1; i < HISTORY_MAX - 1; i++ )
	{
		double at = ent->ph[( imod - i ) & HISTORY_MASK].animtime;

		if( at == 0.0f )
			break;

		if( targettime > at )
		{
			// found it
			i0 = (( imod - i ) + 1 ) & HISTORY_MASK;
			i1 = (( imod - i ) + 0 ) & HISTORY_MASK;
			extrapolate = false;
			break;
		}
	}

	*ph0 = &ent->ph[i0];
	*ph1 = &ent->ph[i1];

	return extrapolate;
}

/*
==================
CL_PureOrigin

non-local players interpolation
==================
*/
static void CL_PureOrigin( cl_entity_t *ent, double t, vec3_t outorigin, vec3_t outangles )
{
	position_history_t	*ph0, *ph1;

	// NOTE: ph0 is next, ph1 is a prev
	CL_FindInterpolationUpdates( ent, t, &ph0, &ph1 );

	double t0 = ph0->animtime;
	double t1 = ph1->animtime;

	if( t0 != 0.0 )
	{
		vec4_t	q, q1, q2;
		vec3_t	delta;
		double	frac;

		VectorSubtract( ph0->origin, ph1->origin, delta );

		if( !Q_equal( t0, t1 ))
			frac = ( t - t1 ) / ( t0 - t1 );
		else frac = 1.0;

		frac = bound( 0.0, frac, 1.2 );

		VectorMA( ph1->origin, frac, delta, outorigin );

		AngleQuaternion( ph0->angles, q1, false );
		AngleQuaternion( ph1->angles, q2, false );
		QuaternionSlerp( q2, q1, frac, q );
		QuaternionAngle( q, outangles );
	}
	else
	{
		// no backup found
		VectorCopy( ph1->origin, outorigin );
		VectorCopy( ph1->angles, outangles );
	}
}

/*
==================
CL_InterpolateModel

non-players interpolation
==================
*/
static int CL_InterpolateModel( cl_entity_t *e )
{
	position_history_t  *ph0 = NULL, *ph1 = NULL;
	vec3_t		origin, angles, delta;
	vec4_t		q, q1, q2;

	VectorCopy( e->curstate.origin, e->origin );
	VectorCopy( e->curstate.angles, e->angles );

	if( cls.timedemo || !e->model )
		return 1;

	if( cls.demoplayback == DEMO_QUAKE1 )
	{
		// quake lerping is easy
		VectorLerp( e->prevstate.origin, cl.lerpFrac, e->curstate.origin, e->origin );
		AngleQuaternion( e->prevstate.angles, q1, false );
		AngleQuaternion( e->curstate.angles, q2, false );
		QuaternionSlerp( q1, q2, cl.lerpFrac, q );
		QuaternionAngle( q, e->angles );
		return 1;
	}

	if( cl.maxclients <= 1 )
		return 1;

	if( e->model->type == mod_brush && !cl_bmodelinterp.value )
		return 1;

	if( cl.local.moving && cl.local.onground == e->index )
		return 1;

	double t = cl.time - cl_interp.value;
	CL_FindInterpolationUpdates( e, t, &ph0, &ph1 );

	double t1 = ph1->animtime;
	double t2 = ph0->animtime;

	if( t - t1 < 0.0f )
		return 0;

	if( t1 == 0.0f )
	{
		VectorCopy( ph0->origin, e->origin );
		VectorCopy( ph0->angles, e->angles );
		return 0;
	}

	if( Q_equal( t2, t1 ))
	{
		VectorCopy( ph0->origin, e->origin );
		VectorCopy( ph0->angles, e->angles );
		return 1;
	}

	VectorSubtract( ph0->origin, ph1->origin, delta );
	double frac = (t - t1) / (t2 - t1);

	if( frac < 0.0f )
		return 0;

	if( frac > 1.0f )
		frac = 1.0f;

	VectorMA( ph1->origin, frac, delta, origin );

	AngleQuaternion( ph0->angles, q1, false );
	AngleQuaternion( ph1->angles, q2, false );
	QuaternionSlerp( q2, q1, frac, q );
	QuaternionAngle( q, angles );

	VectorCopy( origin, e->origin );
	VectorCopy( angles, e->angles );

	return 1;
}

/*
=============
CL_ComputePlayerOrigin

interpolate non-local clients
=============
*/
void CL_ComputePlayerOrigin( cl_entity_t *ent )
{
	vec3_t	origin;
	vec3_t	angles;

	if( !ent->player )
		return;

	if( cl_nointerp.value > 0.f )
	{
		VectorCopy( ent->curstate.angles, ent->angles );
		VectorCopy( ent->curstate.origin, ent->origin );
		return;
	}

	if( cls.demoplayback == DEMO_QUAKE1 )
	{
		vec4_t	q, q1, q2;

		// quake lerping is easy
		VectorLerp( ent->prevstate.origin, cl.lerpFrac, ent->curstate.origin, ent->origin );
		AngleQuaternion( ent->prevstate.angles, q1, false );
		AngleQuaternion( ent->curstate.angles, q2, false );
		QuaternionSlerp( q1, q2, cl.lerpFrac, q );
		QuaternionAngle( q, ent->angles );
		return;
	}

	double targettime = cl.time - cl_interp.value;
	CL_PureOrigin( ent, targettime, origin, angles );

	VectorCopy( angles, ent->angles );
	VectorCopy( origin, ent->origin );
}

/*
=================
CL_ProcessPlayerState

process player states after the new packet has received
=================
*/
static void CL_ProcessPlayerState( int playerindex, entity_state_t *state )
{
	entity_state_t *ps = &cl.frames[cl.parsecountmod].playerstate[playerindex];
	ps->number = state->number;
	ps->messagenum = cl.parsecount;
	ps->msg_time = cl.mtime[0];

	clgame.dllFuncs.pfnProcessPlayerState( ps, state );
}

/*
=================
CL_ResetLatchedState

reset latched state if this frame entity was teleported
or just EF_NOINTERP was set
=================
*/
static void CL_ResetLatchedState( int pnum, frame_t *frame, cl_entity_t *ent )
{
	if( CHECKVISBIT( frame->flags, pnum ))
	{
		VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
		VectorCopy( ent->curstate.angles, ent->latched.prevangles );

		CL_ResetLatchedVars( ent, true );
		CL_ResetPositions( ent );

		// parametric interpolation will starts at this point
		if( ent->curstate.starttime != 0.0f && ent->curstate.impacttime != 0.0f )
			ent->lastmove = cl.time;
	}
}

/*
=================
CL_ProcessPacket

process player states after the new packet has received
=================
*/
void CL_ProcessPacket( frame_t *frame )
{
	for( int pnum = 0; pnum < frame->num_entities; pnum++ )
	{
		// request the entity state from circular buffer
		entity_state_t *state = &cls.packet_entities[(frame->first_entity+pnum) % cls.num_client_entities];
		state->messagenum = cl.parsecount;
		state->msg_time = cl.mtime[0];

		// mark all the players
		cl_entity_t *ent = &clgame.entities[state->number];
		ent->player = CL_IsPlayerIndex( state->number );

		if( state->number == ( cl.playernum + 1 ))
			clgame.dllFuncs.pfnTxferLocalOverrides( state, &frame->clientdata );

		// shuffle states
		ent->prevstate = ent->curstate;
		ent->curstate = *state;

		CL_ProcessEntityUpdate( ent );
		CL_ResetLatchedState( pnum, frame, ent );
		if( !ent->player ) continue;

		CL_ProcessPlayerState(( state->number - 1 ), state );

		if( state->number == ( cl.playernum + 1 ))
			CL_CheckPredictionError();
	}
}

/*
=========================================================================

FRAME PARSING

=========================================================================
*/
static qboolean CL_ParseEntityNumFromPacket( sizebuf_t *msg, int *newnum, connprotocol_t proto )
{
	*newnum = MSG_ReadUBitLong( msg, MAX_ENTITY_BITS );
	if( *newnum == LAST_EDICT )
		return false;

	return true;
}

/*
=================
CL_FlushEntityPacket

Read and ignore whole entity packet.
=================
*/
static void CL_FlushEntityPacket( sizebuf_t *msg, connprotocol_t proto )
{
	entity_state_t	from, to;

	memset( &from, 0, sizeof( from ));

	cl.frames[cl.parsecountmod].valid = false;
	cl.validsequence = 0; // can't render a frame

	// read it all, but ignore it
	while( 1 )
	{
		int newnum;

		if( !CL_ParseEntityNumFromPacket( msg, &newnum, proto ))
			break; // done

		if( MSG_CheckOverflow( msg ))
			Host_Error( "%s: overflow\n", __func__ );

		MSG_ReadDeltaEntity( msg, &from, &to, newnum, CL_IsPlayerIndex( newnum ) ? DELTA_PLAYER : DELTA_ENTITY, cl.mtime[0] );
	}
}

qboolean CL_ValidateDeltaPacket( uint oldpacket, frame_t *oldframe )
{
	int subtracted = ( cls.netchan.incoming_sequence - oldpacket ) & 0xFF;

	if( subtracted == 0 )
	{
		Con_NPrintf( 2, "^3Warning:^1 update too old\n^7\n" );
		return false;
	}

	if( subtracted >= CL_UPDATE_MASK )
	{
		// we can't use this, it is too old
		Con_NPrintf( 2, "^3Warning:^1 delta frame is too old^7\n" );
		return false;
	}

	if(( cls.next_client_entities - oldframe->first_entity ) > ( cls.num_client_entities - NUM_PACKET_ENTITIES ))
	{
		Con_NPrintf( 2, "^3Warning:^1 delta frame is too old^7\n" );
		return false;
	}

	return true;
}

int CL_UpdateOldEntNum( int oldindex, frame_t *oldframe, entity_state_t **oldent )
{
	if( !oldframe )
	{
		*oldent = NULL;
		return MAX_ENTNUMBER;
	}

	if( oldindex >= oldframe->num_entities )
		return MAX_ENTNUMBER;

	*oldent = &cls.packet_entities[(oldframe->first_entity + oldindex) % cls.num_client_entities];
	return (*oldent)->number;
}

/*
=================
CL_DeltaEntity

processing delta update
=================
*/
static void CL_DeltaEntity( sizebuf_t *msg, frame_t *frame, int newnum, entity_state_t *old, qboolean has_update )
{
	cl_entity_t	*ent;
	entity_state_t	*state;
	qboolean		newent = (old) ? false : true;
	int		pack = frame->num_entities;
	int		delta_type = DELTA_ENTITY;
	qboolean		alive = true;

	// alloc next slot to store update
	state = &cls.packet_entities[cls.next_client_entities % cls.num_client_entities];
	if( CL_IsPlayerIndex( newnum )) delta_type = DELTA_PLAYER;

	if(( newnum < 0 ) || ( newnum >= clgame.maxEntities ))
	{
#if XASH_GAMECUBE
		/* Direct-map New Game keeps the 2-slot bootstrap entity table and
		 * draws entities from server edicts; every snapshot lands here. */
		if( clgame.maxEntities > 2 )
#endif
		Con_DPrintf( S_ERROR "%s: invalid newnum: %d\n", __func__, newnum );
#if XASH_GAMECUBE
		/* No cl_entity_t for it, but its state still belongs in this frame:
		 * the server deltas the next frame against it. Leaving it out made
		 * the client parse the next delta of that entity as a new entity and
		 * misread the stream ("CL_ParsePacketEntities: overflow") once the
		 * history ring was large enough for real delta compression. */
		if( has_update )
		{
			entity_state_t base = { 0 };
			/* Read with a valid index; OOB numbers reject without reading. */
			alive = MSG_ReadDeltaEntity( msg, old ? old : &base, state, 1, delta_type, cl.mtime[0] );
		}
		else if( old )
			*state = *old;
		else
			alive = false;
		if( alive )
		{
			state->number = newnum;
			cls.next_client_entities++;
			frame->num_entities++;
		}
#else
		if( has_update )
		{
			entity_state_t discarded = { 0 };
			/* Consume bits with a valid index — OOB numbers reject without reading. */
			MSG_ReadDeltaEntity( msg, old ? old : &discarded, &discarded, 1, delta_type, cl.mtime[0] );
		}
#endif
		return;
	}

	ent = CL_EDICT_NUM( newnum );
	ent->index = newnum; // enumerate entity index
	if( newent ) old = &ent->baseline;

	if( has_update )
		alive = MSG_ReadDeltaEntity( msg, old, state, newnum, delta_type, cl.mtime[0] );
	else *state = *old;

	if( !alive )
	{
		CL_KillDeadBeams( ent ); // release dead beams
#if 0
		// this is for reference
		if( state->number == -1 )
			Con_DPrintf( "Entity %i was removed from server\n", newnum );
		else Con_Dprintf( "Entity %i was removed from delta-message\n", newnum );
#endif
		return;
	}

	if( newent )
	{
		// interpolation must be reset
		SETVISBIT( frame->flags, pack );

		// release beams from previous entity

		// a1ba: check that this entity number was never used on client
		// as beams can be transferred before this entity was sent to client
		// (for example, beam was sent over during beam entity spawn
		// but referenced start point entity hasn't been sent over due to PVS)
		if( ent->curstate.messagenum != 0 )
			CL_KillDeadBeams( ent );
	}

	// add entity to packet
	cls.next_client_entities++;
	frame->num_entities++;
}

/*
==================
CL_ParsePacketEntities

An svc_packetentities has just been parsed, deal with the
rest of the data stream.
==================
*/
int CL_ParsePacketEntities( sizebuf_t *msg, qboolean delta, connprotocol_t proto )
{
	frame_t		*oldframe;
	int		oldindex, newnum, oldnum;
	int		playerbytes = 0;
	entity_state_t	*oldent;

	// save first uncompressed packet as timestamp
	if( cls.changelevel && !delta && cls.demorecording )
		CL_WriteDemoJumpTime();

	// sentinel count. save it for debug checking
	int count = MSG_ReadUBitLong( msg, MAX_VISIBLE_PACKET_BITS ) + 1;

	frame_t *newframe = &cl.frames[cl.parsecountmod];

	// allocate parse entities
	memset( newframe->flags, 0, sizeof( newframe->flags ));
	newframe->first_entity = cls.next_client_entities;
	newframe->num_entities = 0;
	newframe->valid = true; // assume valid

	if( delta )
	{
		uint oldpacket = MSG_ReadByte( msg );
		oldframe = &cl.frames[oldpacket & CL_UPDATE_MASK];

		if( !CL_ValidateDeltaPacket( oldpacket, oldframe ))
		{
			CL_FlushEntityPacket( msg, proto );
			return playerbytes;
		}
	}
	else
	{
		// this is a full update that we can start delta compressing from now
		oldframe = NULL;
		cls.demowaiting = false;	// we can start recording now
	}

	// mark current delta state
	cl.validsequence = cls.netchan.incoming_sequence;

	oldent = NULL;
	oldindex = 0;
	oldnum = CL_UpdateOldEntNum( oldindex, oldframe, &oldent );

	while( 1 )
	{
		if( !CL_ParseEntityNumFromPacket( msg, &newnum, proto ))
			break; // done

		if( MSG_CheckOverflow( msg ))
			Host_Error( "%s: overflow\n", __func__ );

		qboolean player = CL_IsPlayerIndex( newnum );

		while( oldnum < newnum )
		{
			// one or more entities from the old packet are unchanged
			CL_DeltaEntity( msg, newframe, oldnum, oldent, false );
			oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
		}

		if( oldnum == newnum )
		{
			// delta from previous state
			int bufStart = MSG_GetNumBytesRead( msg );
			CL_DeltaEntity( msg, newframe, newnum, oldent, true );
			if( player ) playerbytes += MSG_GetNumBytesRead( msg ) - bufStart;
			oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
			continue;
		}

		if( oldnum > newnum )
		{
			// delta from baseline ?
			int bufStart = MSG_GetNumBytesRead( msg );
			CL_DeltaEntity( msg, newframe, newnum, NULL, true );
			if( player ) playerbytes += MSG_GetNumBytesRead( msg ) - bufStart;
			continue;
		}
	}

	// any remaining entities in the old frame are copied over
	while( oldnum != MAX_ENTNUMBER )
	{
		// one or more entities from the old packet are unchanged
		CL_DeltaEntity( msg, newframe, oldnum, oldent, false );
		oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
	}

	if( newframe->num_entities != count && newframe->num_entities != 0
#if XASH_GAMECUBE
		&& clgame.maxEntities > 2
#endif
		)
		Con_Reportf( S_WARN "%s%s: (%i should be %i)\n", __func__, delta ? "Delta" : "", newframe->num_entities, count );

	if( !newframe->valid )
		return playerbytes; // frame is not valid but message was parsed

	// now process packet.
	CL_ProcessPacket( newframe );

	// add new entities into physic lists
	CL_SetSolidEntities();

	// first update is the final signon stage where we actually receive an entity (i.e., the world at least)
	if( cls.signon == ( SIGNONS - 1 ))
	{
		// we are done with signon sequence.
		cls.signon = SIGNONS;

		// Clear loading plaque.
		CL_SignonReply( proto );
	}

	return playerbytes;
}

/*
==========================================================================

INTERPOLATE BETWEEN FRAMES TO GET RENDERING PARMS

==========================================================================
*/
/*
=============
CL_AddVisibleEntity

all the visible entities should pass this filter
=============
*/
qboolean CL_AddVisibleEntity( cl_entity_t *ent, int entityType )
{
	qboolean draw_player = true;
	#if XASH_GAMECUBE
	static unsigned gc_g317_seen;
	static unsigned gc_g317_rejected;
	#endif

	if( !ent || !ent->model )
		return false;
	#if XASH_GAMECUBE
	if(( Sys_CheckParm( "-gcnewgame" ) || Sys_CheckParm( "-gcmenuplaystart" )) && gc_g317_seen < 16 )
	{
		Con_Reportf( "Xash3D GameCube: G317 client entity index=%d model=%s type=%d\n",
			ent->index, ent->model->name, entityType );
		gc_g317_seen++;
	}
	#endif

	// don't add the player in firstperson mode
	if( RP_LOCALCLIENT( ent ))
	{
		cl.local.apply_effects = true;

		if( !CL_IsThirdPerson( ) && ( ent->index == cl.viewentity ))
		{
			// we don't draw player in default renderer in firstperson mode
			// but let the client.dll know about player entity anyway
			// for use in custom renderers
			draw_player = false;
		}
	}

	// check for adding this entity
	if( !clgame.dllFuncs.pfnAddEntity( entityType, ent, ent->model->name ))
	{
		#if XASH_GAMECUBE
		if(( Sys_CheckParm( "-gcnewgame" ) || Sys_CheckParm( "-gcmenuplaystart" )) && gc_g317_rejected < 8 )
		{
			Con_Reportf( "Xash3D GameCube: G317 client entity rejected index=%d model=%s\n",
				ent->index, ent->model->name );
			gc_g317_rejected++;
		}
		#endif
		// local player was reject by game code, so ignore any effects
		if( RP_LOCALCLIENT( ent ))
			cl.local.apply_effects = false;
		return false;
	}

	if( !draw_player )
		return false;

	if( !ref.dllFuncs.R_AddEntity( ent, entityType ))
		return false;

	// because pTemp->entity.curstate.effects
	// is already occupied by FTENT_FLICKER
	if( entityType != ET_TEMPENTITY && !RP_LOCALCLIENT( ent ) )
	{
		// apply client-side effects
		CL_AddEntityEffects( ent );

		// alias & studiomodel efefcts
		CL_AddModelEffects( ent );
	}

	return true;
}

/*
=============
CL_LinkCustomEntity

Add server beam to draw list
=============
*/
static void CL_LinkCustomEntity( cl_entity_t *ent, entity_state_t *state )
{
	ent->curstate.movetype = state->modelindex; // !!!

#if XASH_GAMECUBE
	/* The menu route lends server models, and the server's sprite is a stub:
	 * the beam needs the client's own sprite. */
	if( ent->model->type != mod_sprite && GC_MenuRunsServer( ))
	{
		model_t *sprite = CL_LoadClientSprite( ent->model->name );

		if( sprite && sprite->type == mod_sprite )
		{
			if( state->modelindex > 0 && state->modelindex < MAX_MODELS )
				cl.models[state->modelindex] = sprite;
			ent->model = sprite;
		}
	}
#endif
	if( ent->model->type != mod_sprite )
	{
#if XASH_GAMECUBE
		static const model_t *warned; /* OSReport per frame is not free */

		if( warned != ent->model )
#endif
		Con_Reportf( S_WARN "bad model on beam ( %s )\n", ent->model->name );
#if XASH_GAMECUBE
		warned = ent->model;
#endif
	}

	ent->latched.prevsequence = ent->curstate.sequence;
	VectorCopy( ent->origin, ent->latched.prevorigin );
	VectorCopy( ent->angles, ent->latched.prevangles );
	ent->prevstate = ent->curstate;

	CL_AddVisibleEntity( ent, ET_BEAM );
}

/*
=============
CL_LinkPlayers

Create visible entities in the correct position
for all current players
=============
*/
static void CL_LinkPlayers( frame_t *frame )
{
	cl_entity_t	*ent = CL_GetLocalPlayer();
	entity_state_t	*state;
	int		i;

	// apply muzzleflash to weaponmodel
	if( ent && FBitSet( ent->curstate.effects, EF_MUZZLEFLASH ))
		SetBits( clgame.viewent.curstate.effects, EF_MUZZLEFLASH );

	// check all the clients but add only visible
	for( i = 0, state = frame->playerstate; i < MAX_CLIENTS; i++, state++ )
	{
		if( state->messagenum != cl.parsecount )
			continue;	// not present this frame

		if( !state->modelindex || FBitSet( state->effects, EF_NODRAW ))
			continue;

		ent = &clgame.entities[i + 1];

		// fixup the player indexes...
		if( ent->index != ( i + 1 )) ent->index = (i + 1);

		if( i == cl.playernum )
		{
			if( cls.demoplayback != DEMO_QUAKE1 )
			{
				VectorCopy( state->origin, ent->origin );
				VectorCopy( state->origin, ent->prevstate.origin );
				VectorCopy( state->origin, ent->curstate.origin );
			}
			VectorCopy( ent->curstate.angles, ent->angles );
		}

		if( FBitSet( ent->curstate.effects, EF_NOINTERP ))
			CL_ResetLatchedVars( ent, false );

		if( CL_EntityTeleported( ent ))
		{
			VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
			VectorCopy( ent->curstate.angles, ent->latched.prevangles );
			CL_ResetPositions( ent );
		}

		if ( i == cl.playernum )
		{
			// using interpolation only for local player angles
			CL_ComputePlayerOrigin( ent );

			if( cls.demoplayback == DEMO_QUAKE1 )
				VectorLerp( ent->prevstate.origin, cl.lerpFrac, ent->curstate.origin, cl.simorg );
			VectorCopy( cl.simorg, ent->origin );
		}
		else
		{
			VectorCopy( ent->curstate.origin, ent->origin );
			VectorCopy( ent->curstate.angles, ent->angles );

			// interpolate non-local clients
			CL_ComputePlayerOrigin( ent );
		}

		VectorCopy( ent->origin, ent->attachment[0] );
		VectorCopy( ent->origin, ent->attachment[1] );
		VectorCopy( ent->origin, ent->attachment[2] );
		VectorCopy( ent->origin, ent->attachment[3] );

		CL_AddVisibleEntity( ent, ET_PLAYER );
	}

	// apply local player effects if entity is not added
	if( cl.local.apply_effects ) CL_AddEntityEffects( CL_GetLocalPlayer( ));
}

/*
===============
CL_LinkPacketEntities

===============
*/
static void CL_LinkPacketEntities( frame_t *frame )
{
	for( int i = 0; i < frame->num_entities; i++ )
	{
		entity_state_t *state = &cls.packet_entities[(frame->first_entity + i) % cls.num_client_entities];

		// clients are should be done in CL_LinkPlayers
		if( state->number >= 1 && state->number <= cl.maxclients )
			continue;

		// if set to invisible, skip
		if( !state->modelindex || FBitSet( state->effects, EF_NODRAW ))
			continue;

		cl_entity_t *ent = CL_GetEntityByIndex( state->number );

		if( !ent )
		{
			Con_Reportf( S_ERROR "%s: bad entity %i\n", __func__, state->number );
			continue;
		}

		// animtime must keep an actual
		ent->curstate.animtime = state->animtime;
		ent->curstate.frame = state->frame;
		qboolean interpolate = false;

#if XASH_GAMECUBE
		/* The lean menu route never registers the client model precache. On
		 * the listen server the brush submodels (*N: the c0a0 tram, doors,
		 * walls) are the same world-owned model_t, so lend the server handle,
		 * as CL_GameCubeLeanEmitBrushEntities does for -gcnewgame. */
		if( !ent->model && Sys_CheckParm( "-gcmenuplaystart" ) && SV_Active()
			&& state->modelindex > 0 && state->modelindex < MAX_MODELS )
		{
			model_t *mod = SV_ModelHandle( state->modelindex );

			/* Studio actors too: their resident mesh is the server's. */
			if( mod && ( mod->type == mod_brush
				|| ( mod->type == mod_studio && mod->cache.data )))
			{
				cl.models[state->modelindex] = mod;
				ent->model = mod;
			}
			/* The server's sprite is a stub: use the client's own. */
			else if( mod && mod->name[0] && Q_stristr( mod->name, ".spr" ))
			{
				model_t *sprite = CL_LoadClientSprite( mod->name );

				if( sprite && sprite->type == mod_sprite )
				{
					cl.models[state->modelindex] = sprite;
					ent->model = sprite;
				}
			}
		}
		if( !ent->model && Sys_CheckParm( "-gcmenuplaystart" ))
		{
			static int nomodel_logs;

			/* Bounded: packet entities the client cannot draw for lack of a model. */
			if( nomodel_logs < 12 )
			{
				nomodel_logs++;
				model_t *svmod = SV_Active() ? SV_ModelHandle( state->modelindex ) : NULL;

				Con_Reportf( "Xash3D GameCube: packet entity %d modelindex=%d has no client model (server %s type=%d data=%d)\n",
					state->number, state->modelindex, svmod ? svmod->name : "-",
					svmod ? svmod->type : -1, svmod && svmod->cache.data ? 1 : 0 );
			}
		}
#endif
		if( !ent->model ) continue;

		if( ent->curstate.rendermode == kRenderNormal )
		{
			// auto 'solid' faces
			if( FBitSet( ent->model->flags, MODEL_TRANSPARENT ) && Host_IsQuakeCompatible( ))
			{
				ent->curstate.rendermode = kRenderTransAlpha;
				ent->curstate.renderamt = 255;
			}
		}

		qboolean parametric = ( ent->curstate.impacttime != 0.0f && ent->curstate.starttime != 0.0f );

		if( !parametric && ent->curstate.movetype != MOVETYPE_COMPOUND )
		{
			if( ent->curstate.animtime == ent->prevstate.animtime && !VectorCompare( ent->curstate.origin, ent->prevstate.origin ))
				ent->lastmove = cl.time + 0.2;

			if( FBitSet( ent->curstate.eflags, EFLAG_SLERP ))
			{
				if( ent->curstate.animtime != 0.0f && ( ent->model->type == mod_alias || ent->model->type == mod_studio ))
				{
#ifdef STUDIO_INTERPOLATION_FIX
					if( ent->lastmove >= cl.time )
						VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
					if( FBitSet( host.features, ENGINE_COMPUTE_STUDIO_LERP ))
						interpolate = true;
					else ent->curstate.movetype = MOVETYPE_STEP;
#else
					if( ent->lastmove >= cl.time )
					{
						float at = ent->curstate.animtime;

						CL_ResetLatchedVars( ent, true );

						if( cl_fixmodelinterpolationartifacts.value )
							ent->latched.prevanimtime = ent->curstate.animtime = at;

						VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
						VectorCopy( ent->curstate.angles, ent->latched.prevangles );

						if( !FBitSet( host.features, ENGINE_COMPUTE_STUDIO_LERP ))
						{
							// disable step interpolation in client.dll
							ent->curstate.movetype = MOVETYPE_NONE;
						}
					}
					else
					{
						if( FBitSet( host.features, ENGINE_COMPUTE_STUDIO_LERP ))
						{
							interpolate = true;
						}
						else
						{
							// restore step interpolation in client.dll
							ent->curstate.movetype = MOVETYPE_STEP;
						}
					}
#endif
				}
			}
		}

		if( ent->model->type == mod_brush )
		{
#if XASH_GAMECUBE
			/* Menu New Game takes the view straight from the server origin
			 * (no prediction), so a pusher lerped ex_interp behind drew the
			 * c0a0 tram ~30 units behind its rider: the cabin's front wall
			 * fell behind the camera. Draw pushers at the same timebase. */
			if( ent->curstate.movetype == MOVETYPE_PUSH && Sys_CheckParm( "-gcmenuplaystart" ))
			{
				VectorCopy( ent->curstate.origin, ent->origin );
				VectorCopy( ent->curstate.angles, ent->angles );
			}
			else
#endif
			CL_InterpolateModel( ent );
		}
		else
		{
			if( parametric )
			{
				CL_ParametricMove( ent );

				VectorCopy( ent->curstate.origin, ent->origin );
				VectorCopy( ent->curstate.angles, ent->angles );
			}
			else if( CL_EntityCustomLerp( ent ))
			{
				if ( !CL_InterpolateModel( ent ))
					continue;
			}
			// a1ba: in GoldSrc this is done for cstrike and czero
			// but let modders use this as an engine feature
			else if( FBitSet( host.features, ENGINE_STEP_POSHISTORY_LERP ) &&
				ent->curstate.movetype == MOVETYPE_STEP && !NET_IsLocalAddress( cls.netchan.remote_address ))
			{
				if( !CL_InterpolateModel( ent ))
					continue;
			}
			else
			{
				// no interpolation right now
				VectorCopy( ent->curstate.origin, ent->origin );
				VectorCopy( ent->curstate.angles, ent->angles );
			}

			if( ent->model->type == mod_studio )
			{
				if( interpolate && FBitSet( host.features, ENGINE_COMPUTE_STUDIO_LERP ))
					ref.dllFuncs.R_StudioLerpMovement( ent, cl.time, ent->origin, ent->angles );
			}
		}

		if( !FBitSet( state->entityType, ENTITY_NORMAL ))
		{
			CL_LinkCustomEntity( ent, state );
			continue;
		}

		if( ent->model->type != mod_brush )
		{
			// NOTE: never pass sprites with rendercolor '0 0 0' it's a stupid Valve Hammer Editor bug
			if( !ent->curstate.rendercolor.r && !ent->curstate.rendercolor.g && !ent->curstate.rendercolor.b )
				ent->curstate.rendercolor.r = ent->curstate.rendercolor.g = ent->curstate.rendercolor.b = 255;
		}

		// XASH SPECIFIC
		if( ent->curstate.rendermode == kRenderNormal && ent->curstate.renderfx == kRenderFxNone )
			ent->curstate.renderamt = 255.0f;

		if( ent->curstate.aiment != 0 && ent->curstate.movetype != MOVETYPE_COMPOUND )
			ent->curstate.movetype = MOVETYPE_FOLLOW;

		if( FBitSet( ent->curstate.effects, EF_NOINTERP ))
			CL_ResetLatchedVars( ent, false );

		if( CL_EntityTeleported( ent ))
		{
			VectorCopy( ent->curstate.origin, ent->latched.prevorigin );
			VectorCopy( ent->curstate.angles, ent->latched.prevangles );
			CL_ResetPositions( ent );
		}

		VectorCopy( ent->origin, ent->attachment[0] );
		VectorCopy( ent->origin, ent->attachment[1] );
		VectorCopy( ent->origin, ent->attachment[2] );
		VectorCopy( ent->origin, ent->attachment[3] );

		CL_AddVisibleEntity( ent, ET_NORMAL );
	}
}

/*
===============
CL_MoveThirdpersonCamera

think thirdperson
===============
*/
void CL_MoveThirdpersonCamera( void )
{
	if( cls.state == ca_disconnected || cls.state == ca_cinematic )
		return;

	// think thirdperson camera
	clgame.dllFuncs.CAM_Think ();
}

/*
===============
CL_EmitEntities

add visible entities to refresh list
process frame interpolation etc
===============
*/
#if XASH_GAMECUBE
#if XASH_GAMECUBE
/*
=============
Remote client static props

The server sends its static props (unnamed env_model / dod_object, no edict)
in the signon as svc_spawnstatic. The listen host already draws them from
gc_static_props, so it drops these messages. A remote client fills its own
gc_static_props from them and links the nearest ones each frame.
=============
*/
#define GC_REMOTE_STATIC_DRAW	16

static qboolean CL_GCStaticPropOnServer( const entity_state_t *state )
{
	int i;

	for( i = 0; i < gc_num_static_props; i++ )
	{
		const gc_static_prop_t *sp = &gc_static_props[i];

		if( sp->modelindex == state->modelindex && VectorCompareEpsilon( sp->origin, state->origin, 0.5f ))
			return true;
	}
	return false;
}

qboolean CL_GCStaticPropFromSignon( const entity_state_t *state )
{
	gc_static_prop_t *sp;

	if( SV_Active( ))
		return CL_GCStaticPropOnServer( state );
	if( gc_num_static_props >= GC_STATIC_PROPS_MAX )
		return true; /* over the cap: drop it rather than spend a cl_entity_t */
	sp = &gc_static_props[gc_num_static_props++];
	memset( sp, 0, sizeof( *sp ));
	VectorCopy( state->origin, sp->origin );
	VectorCopy( state->angles, sp->angles );
	sp->scale = state->scale;
	sp->framerate = state->framerate;
	sp->modelindex = (short)state->modelindex;
	sp->sequence = (short)state->sequence;
	sp->skin = state->skin;
	sp->body = (short)state->body;
	sp->rendermode = (byte)state->rendermode;
	sp->renderamt = (byte)state->renderamt;
	sp->renderfx = (byte)state->renderfx;
	sp->rendercolor = state->rendercolor;
	return true;
}

void CL_GCClearRemoteStaticProps( void )
{
	if( !SV_Active( ))
		gc_num_static_props = 0;
}

void CL_GCLinkRemoteStaticProps( void )
{
	static cl_entity_t ents[GC_REMOTE_STATIC_DRAW];
	static int logged_count = -1;
	int picks[GC_REMOTE_STATIC_DRAW];
	float dists[GC_REMOTE_STATIC_DRAW];
	int n = 0, i, j, linked = 0;

	if( SV_Active() || gc_num_static_props <= 0 )
		return;
	for( i = 0; i < gc_num_static_props; i++ )
	{
		const gc_static_prop_t *sp = &gc_static_props[i];
		vec3_t delta;
		float d;

		VectorSubtract( sp->origin, refState.vieworg, delta );
		d = DotProduct( delta, delta );
		if( d > 12000.0f * 12000.0f )
			continue;
		if( n < GC_REMOTE_STATIC_DRAW )
			n++;
		else if( d >= dists[n - 1] )
			continue;
		for( j = n - 1; j > 0 && dists[j - 1] > d; j-- )
		{
			dists[j] = dists[j - 1];
			picks[j] = picks[j - 1];
		}
		dists[j] = d;
		picks[j] = i;
	}
	for( i = 0; i < n; i++ )
	{
		const gc_static_prop_t *sp = &gc_static_props[picks[i]];
		cl_entity_t *ent = &ents[linked];
		model_t *mod = CL_ModelHandle( sp->modelindex );

		if( !mod || mod->type != mod_studio || mod->needload != NL_PRESENT || !mod->cache.data )
			continue;
		memset( ent, 0, sizeof( *ent ));
		ent->model = mod;
		VectorCopy( sp->origin, ent->origin );
		VectorCopy( sp->angles, ent->angles );
		VectorCopy( sp->origin, ent->curstate.origin );
		VectorCopy( sp->angles, ent->curstate.angles );
		ent->curstate.modelindex = sp->modelindex;
		ent->curstate.sequence = sp->sequence;
		ent->curstate.body = sp->body;
		ent->curstate.skin = sp->skin;
		ent->curstate.scale = sp->scale > 0.0f ? sp->scale : 1.0f;
		ent->curstate.framerate = sp->framerate != 0.0f ? sp->framerate : 1.0f;
		ent->curstate.rendermode = sp->rendermode;
		ent->curstate.renderamt = sp->renderamt;
		ent->curstate.renderfx = sp->renderfx;
		ent->curstate.rendercolor = sp->rendercolor;
		ent->curstate.entityType = ENTITY_NORMAL;
		ent->prevstate = ent->curstate;
		VectorCopy( ent->origin, ent->latched.prevorigin );
		VectorCopy( ent->angles, ent->latched.prevangles );
		if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
			linked++;
	}
	if( logged_count != gc_num_static_props )
	{
		logged_count = gc_num_static_props;
		Con_Reportf( "Xash3D GameCube: remote static props %d, linked %d nearest\n",
			gc_num_static_props, linked );
	}
}
#endif

/*
===============
CL_GameCubeLeanEmitBrushEntities

Direct-map lean has no client netchan/validsequence, so EmitEntities never
fills the draw list (G315 solid=0). Pull nearby server brush edicts plus at
most one already-resident world studio (not v_*) into the Flipper list.
===============
*/
/* DoD animation token for a carried weapon, from its p_ model name
 * (models/p_stg44.mdl -> "stg44"); the body's sequences are stand_<token>_aim. */
static const char *CL_GCLeanDodAimToken( const char *wpath )
{
	static const struct { const char *model; const char *token; } map[] = {
		{ "p_stg44", "stg44" }, { "p_mp40", "mp40" }, { "p_k98", "bolt" }, { "p_garand", "rifle" },
		{ "p_tommy", "tommy" }, { "p_thompson", "tommy" }, { "p_spring", "bolt" }, { "p_amerk", "knife" },
		{ "p_colt", "pistol" }, { "p_luger", "pistol" }, { "p_bar", "mg" }, { "p_mg42", "mg" },
		{ "p_30cal", "30cal" }, { "p_bren", "bren" }, { "p_bazooka", "bazooka" }, { "p_pschreck", "pschreck" },
		{ "p_spade", "spade" }, { "p_grenade", "gren" }, { "p_stick", "stick" },
		/* BAR, British and para guns, on the body's nearest aim pose. */
		{ "p_barbu", "mg" }, { "p_enfield", "bolt" }, { "p_enfields", "bolt" }, { "p_sten", "mp40" },
		{ "p_piat", "bazooka" }, { "p_webley", "pistol" }, { "p_fairbairn", "knife" }, { "p_paraknife", "knife" },
		{ "p_mills", "gren" }, { "p_m1carb", "rifle" }, { "p_grease", "tommy" }, { "p_fg42", "stg44" },
		/* Bipod deployed / up variants (p_*bd, p_*bu) not covered by a prefix above. */
		{ "p_scopedfg42", "stg44" }, { "p_mg34", "mg" },
	};
	const char *name;
	int i;

	if( !wpath || !wpath[0] )
		return NULL;
	name = Q_strrchr( wpath, '/' );
	name = name ? name + 1 : wpath;
	for( i = 0; i < (int)( sizeof( map ) / sizeof( map[0] )); i++ )
	{
		if( !Q_strnicmp( name, map[i].model, Q_strlen( map[i].model )))
			return map[i].token;
	}
	return NULL;
}

extern void GC_ProfMark( const char *name );
void R_GXStudioInvalidateBindings( void );

/* First sequence of a studio header whose label contains `want`, or -1. */
static int CL_GCLeanFindSeqScan( const studiohdr_t *hdr, const char *want );

/* Every drawn player looks up up to six labels a frame, and a miss walks all
 * of a DoD body's sequences four times with case-blind compares. Remember the
 * answers per header; the header's size fields guard against a reused address. */
#define CL_GC_SEQ_CACHE 64
typedef struct
{
	const studiohdr_t *hdr;
	int numseq;
	int seqindex;
	int seq;
	char want[40];
} cl_gc_seq_cache_t;

static int CL_GCLeanFindSeq( const studiohdr_t *hdr, const char *want )
{
	static cl_gc_seq_cache_t cache[CL_GC_SEQ_CACHE];
	static int next;
	cl_gc_seq_cache_t *c;
	int i;

	if( !hdr || !want || Q_strlen( want ) >= (int)sizeof( cache[0].want ))
		return CL_GCLeanFindSeqScan( hdr, want );
	for( i = 0; i < CL_GC_SEQ_CACHE; i++ )
	{
		c = &cache[i];
		if( c->hdr == hdr && c->numseq == hdr->numseq && c->seqindex == hdr->seqindex
			&& !Q_strcmp( c->want, want ))
			return c->seq;
	}
	c = &cache[next];
	next = ( next + 1 ) % CL_GC_SEQ_CACHE;
	c->hdr = hdr;
	c->numseq = hdr->numseq;
	c->seqindex = hdr->seqindex;
	Q_strncpy( c->want, want, sizeof( c->want ));
	c->seq = CL_GCLeanFindSeqScan( hdr, want );
	return c->seq;
}

static int CL_GCLeanFindSeqScan( const studiohdr_t *hdr, const char *want )
{
	const mstudioseqdesc_t *sd;
	int i;

	if( !hdr || hdr->numseq <= 0 || hdr->seqindex <= 0 )
		return -1;
	sd = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex );
	/* Exact label first, then a label starting with it, then any animated
	 * (numframes > 1) label containing it: DoD's first "idle" match is
	 * look_idle, a single-frame aim pose. */
	for( i = 0; i < hdr->numseq; i++ )
	{
		if( !Q_stricmp( sd[i].label, want ))
			return i;
	}
	for( i = 0; i < hdr->numseq; i++ )
	{
		if( !Q_strnicmp( sd[i].label, want, Q_strlen( want )) && sd[i].numframes > 1 )
			return i;
	}
	for( i = 0; i < hdr->numseq; i++ )
	{
		if( Q_stristr( sd[i].label, want ) && sd[i].numframes > 1 )
			return i;
	}
	for( i = 0; i < hdr->numseq; i++ )
	{
		if( Q_stristr( sd[i].label, want ))
			return i;
	}
	return -1;
}

/* Keep the nearest `cap` candidates: id is an edict number, or -(prop + 1)
 * for a static prop. */
/*
 * The model index of a third-person weapon (p_*.mdl) for curstate.weaponmodel, or 0
 * while it is not resident. Not resident yet: load it (lean mesh + skin companion,
 * then from the ARAM asset cache on later maps). A failed load waits before it tries
 * again, and gives up after a few. Players and scripted film actors both carry one.
 */
static int CL_GCLeanWeaponIndex( const char *wpath, qboolean film )
{
	extern qboolean Mod_GCPromoteStudioPath( const char *path );
	static double next_try[64];	/* by model index & 63: BSS is MEM1 */
	static byte fails[64];
	static int fails_spawn = -1;
	static int noload = -1;
	int widx = ( wpath && wpath[0] ) ? SV_ModelIndex( wpath ) : 0;
	model_t *wm = ( widx > 0 && widx < MAX_MODELS ) ? SV_ModelHandle( widx ) : NULL;

	if( widx <= 0 || widx >= MAX_MODELS )
		return 0;
	if( !( wm && wm->type == mod_studio && wm->needload == NL_PRESENT && wm->cache.data ))
	{
		const int wk = widx & 63;

		if( fails_spawn != svs.spawncount )
		{
			memset( next_try, 0, sizeof( next_try ));
			memset( fails, 0, sizeof( fails ));
			fails_spawn = svs.spawncount;
		}
		if( noload < 0 )
			noload = Sys_CheckParm( "-gcnopweaponload" ) ? 1 : 0;
		/* A scripted cast of 24 asked for every class's gun at once: a 198 KB
		 * read failed in the file pool and every later shot's cast file with it
		 * (ceres short-charlie-a). That cap was two guns per map, which left every
		 * German, BAR and .30 cal actor empty-handed (ceres ep02 part 1: only the
		 * Garand and Thompson loaded). Now film actors load guns of 128 KB or less
		 * up to a byte budget per map (-gcfilmgunkb, default 512: the seven DoD guns
		 * of one map's two teams, about 440 KB on dod_avalanche), one per frame so
		 * the file pool never takes two reads at once. */
		if( film && !noload )
		{
			static int film_spawn = -1;
			static long film_bytes, film_budget = -1;
			static double film_last;
			static int film_logs;
			const fs_offset_t size = FS_FileSize( wpath, false );

			if( film_budget < 0 )
			{
				char kb[16];

				film_budget = ( Sys_GetParmFromCmdLine( "-gcfilmgunkb", kb ) && Q_atoi( kb ) > 0 ? Q_atoi( kb ) : 512 ) * 1024L;
			}
			if( film_spawn != svs.spawncount )
			{
				film_spawn = svs.spawncount;
				film_bytes = 0;
				film_last = -1.0;
			}
			if( size <= 0 || size > 131072 || film_bytes + size > film_budget )
			{
				if( film_logs < 16 && fails[wk] < 3 )
				{
					film_logs++;
					Con_Reportf( S_WARN "Xash3D GameCube: film actor weapon %s (%ld bytes) not loaded: %ld of %ld KB used\n",
						wpath, (long)size, film_bytes / 1024, film_budget / 1024 );
				}
				fails[wk] = 3;
				return 0;
			}
			if( film_last == host.realtime )
				return 0;	/* one gun read per frame */
			if( fails[wk] < 3 && host.realtime >= next_try[wk] )
			{
				film_bytes += (long)size;
				film_last = host.realtime;
				Con_Reportf( "Xash3D GameCube: film actor weapon %s (%ld bytes) loading, %ld of %ld KB used\n",
					wpath, (long)size, film_bytes / 1024, film_budget / 1024 );
			}
		}
		if( !noload && fails[wk] < 3 && host.realtime >= next_try[wk] )
		{
			if( Mod_GCPromoteStudioPath( wpath ))
			{
				wm = Mod_FindName( wpath, false );
				if( wm )
					sv.models[widx] = wm;
			}
			else
			{
				fails[wk]++;
				next_try[wk] = host.realtime + 5.0;
			}
		}
	}
	if( wm && wm->type == mod_studio && wm->needload == NL_PRESENT && wm->cache.data )
	{
		cl.models[widx] = wm;
		return widx;
	}
	return 0;
}

static void CL_GCLeanPick( int *picks, float *dists, int *n, int cap, int id, float dist )
{
	int slot, worst = 0;

	if( *n < cap )
	{
		picks[*n] = id;
		dists[*n] = dist;
		( *n )++;
		return;
	}
	for( slot = 1; slot < *n; slot++ )
		if( dists[slot] > dists[worst] )
			worst = slot;
	if( *n > 0 && dist < dists[worst] )
	{
		picks[worst] = id;
		dists[worst] = dist;
	}
}

/* World DL visibility memo keys for static props (edicts use their number). */
#define GC_STATIC_PROP_KEY0	4096

/* Death pose for a dead player or a body-queue corpse. The server's player
 * model is a hull stub, so the game cannot always look the sequence up: keep
 * the game's sequence when it is a valid one-shot of this body, else use the
 * body's first one-shot death label. */
static int CL_GCLeanDeathSeq( const studiohdr_t *hdr, int seq )
{
	static const char *const labels[] = { "death", "die", "dead" };
	const mstudioseqdesc_t *sd;
	int i, k;

	if( !hdr || hdr->numseq <= 0 || hdr->seqindex <= 0 )
		return seq;
	sd = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex );
	if( seq > 0 && seq < hdr->numseq && !FBitSet( sd[seq].flags, STUDIO_LOOPING ))
		return seq;
	for( k = 0; k < (int)( sizeof( labels ) / sizeof( labels[0] )); k++ )
	{
		for( i = 0; i < hdr->numseq; i++ )
		{
			if( Q_stristr( sd[i].label, labels[k] ) && !FBitSet( sd[i].flags, STUDIO_LOOPING ))
				return i;
		}
	}
	return ( seq >= 0 && seq < hdr->numseq ) ? seq : 0;
}

/* Team body of player slot e (1-based) from its "model" userinfo, if resident
 * (loads it once per slot otherwise). */
static model_t *CL_GCLeanPlayerBody( int e, qboolean *tried )
{
	const char *body;
	char path[MAX_QPATH];
	model_t *mod;

	if( e < 1 || e > svs.maxclients || !svs.clients )
		return NULL;
	body = Info_ValueForKey( svs.clients[e - 1].userinfo, "model" );
	if( !body[0] || Q_snprintf( path, sizeof( path ), "models/player/%s/%s.mdl", body, body ) <= 0 )
		return NULL;
	mod = Mod_FindName( path, false );
	if(( !mod || mod->needload != NL_PRESENT ) && tried && !*tried )
	{
		*tried = true;
		mod = Mod_ForName( path, false, false );
		Con_Reportf( "Xash3D GameCube: lean emit player %d fallback body %s %s\n",
			e, path, mod && mod->cache.data ? "loaded" : "load failed" );
	}
	if( !mod || mod->type != mod_studio || mod->needload != NL_PRESENT || !mod->cache.data )
		return NULL;
	return mod;
}

/* A studio prop the game puts in a film (dod-gc's film actors: the soldiers of a
 * cine_play shot, props wearing the team player models) carries this in
 * pev->iuser4. The lean emitter's model allowlist leaves out player bodies, so
 * these are admitted by the tag, ahead of the map's props. Keep the value in step
 * with DODGC_FILM_ACTOR_TAG in dod-gc's src/dod_film.h. */
#define GC_FILM_ACTOR_TAG	0x46494C4D	/* 'FILM' */
/* Studio slots in capture mode: dod-gc's 64-actor cast, the players and props. */
#define GC_LEAN_STUDIO_MAX	72
/* Brush entities (func_wall, doors, breakables) in capture mode, was 6. */
#define GC_LEAN_BRUSH_MAX	64

/* The lean emitter rebuilds each studio entity from its edict every frame, so
 * the client-side latch that crossfades a sequence change (0.2 s in
 * R_StudioSetupBones) never survives. Keep it here, per edict. */
typedef struct
{
	int	index;
	int	seq, prevseq;
	float	frame, prevframe;	/* edict frames, 0..255 */
	float	changetime;
} gc_seqlatch_t;

static gc_seqlatch_t gc_seqlatch[GC_LEAN_STUDIO_MAX + 24];	/* every studio slot: a crowd evicting latches lost its fades */

static void GC_LeanSeqLatch( cl_entity_t *ent, const edict_t *ed, model_t *mod )
{
	const studiohdr_t *hdr = mod ? (const studiohdr_t *)mod->cache.data : NULL;
	gc_seqlatch_t *l = NULL, *spare = &gc_seqlatch[0];
	int i;

	if( !hdr || ent->index <= 0 )
		return;
	for( i = 0; i < (int)ARRAYSIZE( gc_seqlatch ); i++ )
	{
		if( gc_seqlatch[i].index == ent->index )
		{
			l = &gc_seqlatch[i];
			break;
		}
		if( !gc_seqlatch[i].index || gc_seqlatch[i].changetime < spare->changetime )
			spare = &gc_seqlatch[i];
	}
	if( !l )
	{
		l = spare;
		l->index = ent->index;
		l->seq = l->prevseq = ent->curstate.sequence;
		l->changetime = 0.0f;
	}
	else if( l->seq != ent->curstate.sequence )
	{
		l->prevseq = l->seq;
		l->prevframe = l->frame;
		l->seq = ent->curstate.sequence;
		l->changetime = (float)cl.time;
	}
	l->frame = ed->v.frame;
	if( l->changetime > 0.0f && (float)cl.time - l->changetime < 0.2f
		&& l->prevseq >= 0 && l->prevseq < hdr->numseq )
	{
		const mstudioseqdesc_t *sd = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex ) + l->prevseq;

		ent->latched.prevsequence = l->prevseq;
		ent->latched.sequencetime = l->changetime;
		ent->latched.prevframe = l->prevframe * ( sd->numframes - 1 ) / 256.0f;
	}
}

/* The same for movement. A walking monster (MOVETYPE_STEP) moves and turns
 * when it thinks, ten times a second, and the renderer carries it from the
 * step before to this one (R_StudioLerpMovement) between its animtimes. The
 * lean emitter latched "before" to "now" every frame, so nothing was carried:
 * the Lost Coast fisherman walked and turned in ten jumps a second. Few
 * things walk at once on this route, and static data is heap here: four. */
typedef struct
{
	int	index;
	float	animtime, prevanimtime;
	vec3_t	origin, prevorigin;
	vec3_t	angles, prevangles;
} gc_movelatch_t;

static gc_movelatch_t gc_movelatch[4];

static void GC_LeanMoveLatch( cl_entity_t *ent )
{
	gc_movelatch_t *l = NULL, *spare = &gc_movelatch[0];
	int i;

	if( ent->index <= 0 || ent->curstate.movetype != MOVETYPE_STEP || ent->curstate.animtime <= 0.0f )
		return;
	for( i = 0; i < (int)ARRAYSIZE( gc_movelatch ); i++ )
	{
		if( gc_movelatch[i].index == ent->index )
		{
			l = &gc_movelatch[i];
			break;
		}
		if( !gc_movelatch[i].index || gc_movelatch[i].animtime < spare->animtime )
			spare = &gc_movelatch[i];
	}
	if( !l || ent->curstate.animtime < l->animtime || ent->curstate.animtime > l->animtime + 1.0f )
	{
		/* new here, a new level, or back after a pause: nothing to carry */
		l = l ? l : spare;
		l->index = ent->index;
		l->animtime = l->prevanimtime = ent->curstate.animtime;
		VectorCopy( ent->origin, l->origin );
		VectorCopy( ent->origin, l->prevorigin );
		VectorCopy( ent->angles, l->angles );
		VectorCopy( ent->angles, l->prevangles );
	}
	else if( ent->curstate.animtime != l->animtime )
	{
		l->prevanimtime = l->animtime;
		VectorCopy( l->origin, l->prevorigin );
		VectorCopy( l->angles, l->prevangles );
		l->animtime = ent->curstate.animtime;
		VectorCopy( ent->origin, l->origin );
		VectorCopy( ent->angles, l->angles );
	}
	ent->latched.prevanimtime = l->prevanimtime;
	VectorCopy( l->prevorigin, ent->latched.prevorigin );
	VectorCopy( l->prevangles, ent->latched.prevangles );
}

/*
=============
CL_GCLeanPlayerEffects

The New Game routes run a lean client frame without CL_EmitEntities, where
retail turns the local player's effects into dynamic lights
(CL_LinkPlayers -> CL_AddEntityEffects: the flashlight beam, bright light).
Do that step here from the listen server's player edict.
=============
*/
void CL_GCLeanPlayerEffects( void )
{
	edict_t *player;
	cl_entity_t *lp;

	if( !SV_Active() || cls.state != ca_active || svs.maxclients < 1 )
		return;
	player = SV_EdictNum( 1 );
	if( !player || !SV_IsValidEdict( player )
		|| !FBitSet( player->v.effects, EF_DIMLIGHT | EF_BRIGHTLIGHT ))
		return;
	lp = CL_GetLocalPlayer();
	if( !lp )
		return;
	VectorCopy( player->v.origin, lp->origin );
	VectorCopy( player->v.angles, lp->angles );
	lp->curstate.effects = player->v.effects;
	lp->player = true;
	lp->index = cl.playernum + 1;
	if( VectorIsNull( cl.viewheight ))
		VectorSet( cl.viewheight, 0.0f, 0.0f, player->v.view_ofs[2] );
	CL_AddEntityEffects( lp );
}

#define GC_LEAN_WALL_POOL 40	/* static wall draw slots per frame: PVS + frustum visible, reported when exceeded */

/* The wall draw pool (40 cl_entity_t, ~66 KB) is taken from the heap the first time a map has static wall records, not
 * kept as static image: a build or map without gc_static_walls (Lost Coast, HL1) never pays for it. Kept for the session;
 * NULL (walls not drawn, reported once) when the heap cannot give it. */
static cl_entity_t *CL_GCLeanWallPool( void )
{
	static cl_entity_t *pool;
	static qboolean failed;

	if( !pool && !failed )
	{
		pool = (cl_entity_t *)calloc( GC_LEAN_WALL_POOL, sizeof( cl_entity_t ));
		failed = !pool;
		Con_Reportf( "Xash3D GameCube: static wall draw pool %s %s\n", Q_memprint( GC_LEAN_WALL_POOL * sizeof( cl_entity_t )),
			pool ? "from the heap" : "REFUSED, static walls not drawn" );
	}
	return pool;
}

extern byte sv_gc_muzzleflash[MAX_CLIENTS + 1];

void CL_GameCubeLeanEmitBrushEntities( void )
{
	static int log_n;
	static cl_entity_t lean_brush_static[6];
	cl_entity_t *lean_brush_ents = lean_brush_static;
	/* WorldDL listen: emit every nearby map prop (avalanche has ~dozen
	 * env_model instances across 5 unique MDLs). Was 4 — only sandbags. */
	static cl_entity_t lean_studio_static[40];	/* 24 film actors plus map props */
	/* -gccapture: a 64-actor cast plus props, from the spill or heap per map. */
	static cl_entity_t *lean_studio_big;
	cl_entity_t *lean_studio_ents = lean_studio_static;
	int studio_budget = (int)( sizeof( lean_studio_static ) / sizeof( lean_studio_static[0] ));
	static qboolean lean_player_seen;
	edict_t *player;
	vec3_t view_org;
	int e, added = 0, studios = 0;
	int scan_brush = 0, scan_far = 0, scan_nomod = 0, scan_nosurf = 0, brush_beyond_budget = 0;
	int scan_studio = 0, scan_studio_skip = 0;
	int budget = (int)( sizeof( lean_brush_static ) / sizeof( lean_brush_static[0] ));
	qboolean capture_brush_pick = false;
	int brush_picks[GC_LEAN_BRUSH_MAX];
	int n_brush_picks = 0;
	const float max_dist = CL_GCCaptureMode() ? 32000.0f : 8000.0f;
	/* G371: denser AM NPC near dump/landmark eye. Xen bake eye→Nihilanth is
	 * ~450u; keep headroom for companions. WorldDL props: whole-map radius. */
	const float studio_max_dist = CL_GCCaptureMode() ? 32000.0f : ( Sys_CheckParm( "-gcfullphysics" )
		|| Sys_CheckParm( "-gcworldrender" )) ? 12000.0f : 1600.0f;

/* Spill left for the world DL (its tables, ~55 KiB on dod_charlie, and the
 * smallest chunk cache) when capture mode takes big blocks before it loads. */
#ifndef GC_CAPTURE_WORLD_SPILL_KEEP
#define GC_CAPTURE_WORLD_SPILL_KEEP	( 224u * 1024u )
#endif
	if( CL_GCCaptureMode( ))
	{
		extern size_t GC_MemHeapFreeBytes( void );
		extern size_t Mod_GCWorldArraySpillFreeLasting( void );
		extern void *Mod_GCWorldArraySpillTryAlloc( size_t bytes, const char *what );
		extern qboolean Mod_GCWorldArraySpillHolds( const void *ptr, size_t bytes );
		const size_t big_bytes = sizeof( cl_entity_t ) * ( GC_LEAN_STUDIO_MAX + GC_LEAN_BRUSH_MAX );
		static qboolean big_in_spill;
		static int big_tried_spawn = -1;

		/* A spill block dies with its map (the spill rewinds). */
		if( lean_studio_big && big_in_spill && !Mod_GCWorldArraySpillHolds( lean_studio_big, big_bytes ))
		{
			lean_studio_big = NULL;
			big_in_spill = false;
		}
		/* Once per map, in game: the world-array spill first (MEM1 is down to
		 * a few KiB by then), the heap only with a margin, since Mem_Calloc
		 * does not come back on failure. ~1.6 KiB per slot. */
		if( !lean_studio_big && big_tried_spawn != svs.spawncount && cls.state == ca_active )
		{
			const char *where = "spill";

			big_tried_spawn = svs.spawncount;
			/* The world DL loads its tables and chunk cache from the spill on
			 * the first frame drawn, after this. Taking the slots first left it
			 * "no spill for tables" and capture runs drew no world at all (sky,
			 * actors and smoke only). Leave it room, else use the heap. */
			if( Mod_GCWorldArraySpillFreeLasting() >= big_bytes + GC_CAPTURE_WORLD_SPILL_KEEP )
				lean_studio_big = Mod_GCWorldArraySpillTryAlloc( big_bytes, "capture studio slots" );
			if( lean_studio_big )
			{
				memset( lean_studio_big, 0, big_bytes );
				big_in_spill = true;
			}
			else if( GC_MemHeapFreeBytes() > big_bytes + 192 * 1024 )
			{
				lean_studio_big = Mem_Calloc( host.mempool, big_bytes );
				where = "heap";
			}
			Con_Reportf( "Xash3D GameCube: capture mode studio slots %d, brush slots %d %s in %s (%s)\n",
				GC_LEAN_STUDIO_MAX, GC_LEAN_BRUSH_MAX, lean_studio_big ? "allocated" : "not allocated",
				lean_studio_big ? where : "spill or heap", Q_memprint( big_bytes ));
		}
		if( lean_studio_big )
		{
			lean_studio_ents = lean_studio_big;
			studio_budget = GC_LEAN_STUDIO_MAX;
			lean_brush_ents = lean_studio_big + GC_LEAN_STUDIO_MAX;
			budget = GC_LEAN_BRUSH_MAX;
			capture_brush_pick = true;
		}
	}

	/* CapFaces listen (-gcworldrender) also lacks a filled Flipper entity
	 * list (solid=0) — env_model / mapmodels need the same lean emit path. */
	if( !Sys_CheckParm( "-gcnewgame" ) && !Sys_CheckParm( "-gcworldrender" ))
		return;
	/* Fullphysics direct-map still uses bootstrap tables (maxEntities=2) so
	 * CL_EmitEntities never fills the Flipper list (c4a3 20261105-082525:
	 * G315/G318 solid=0). Keep lean emit so Xen bosses can draw. */
	if( !SV_Active() || !GC_IsNewGameWorldReady() )
		return;
	if( cls.state != ca_active )
		return;

	ref.dllFuncs.R_ClearScene();

	player = ( svs.maxclients >= 1 ) ? SV_EdictNum( 1 ) : NULL;
	/* Prefer dump/bake eye so Xen boss picks use the CapFaces camera, not
	 * info_player_start (c4a3 EmitBrush view=(436,-376) while dump eye was
	 * (240,-396)). */
	/* A cinematic camera (cine_play) looks from far across the map: what is
	 * near it is what the shot sees, not what is near the player. */
	if( !VectorIsNull( refState.vieworg )
		&& ( Sys_CheckParm( "-gcdumpframes" ) || Sys_CheckParm( "-gcdump" ) || CL_CineActive( )))
		VectorCopy( refState.vieworg, view_org );
	else if( player && SV_IsValidEdict( player ) && !VectorIsNull( player->v.origin ))
		VectorCopy( player->v.origin, view_org );
	else
		VectorCopy( refState.vieworg, view_org );

	/* -gccapture: the brush slots go to the brush entities that look
	 * biggest from the camera, not the lowest edict numbers. dod_charlie has well over 16 drawn brush
	 * entities, and the first-come pick left out the radio tent's column
	 * (func_wall *178), so the control-point flag on it hung in the air. */
	if( capture_brush_pick )
	{
		float brush_dists[GC_LEAN_BRUSH_MAX];
		vec3_t fwd;

		/* Rank by how big the box looks from the camera (distance over
		 * size), and leave out what is behind it: nearest-first gave every
		 * slot to the beach obstacles in front of a shot looking up at the
		 * bluff, and the radio tent's column still missed out. */
		/* Every map prop, not the first 64 in edict order: dod_charlie has
		 * more than that before its control points. Static: 3 KB. */
		static vec3_t feet[256];
		int n_feet = 0;

		AngleVectors( refState.viewangles, fwd, NULL, NULL );
		n_brush_picks = 0;
		/* A brush that a studio prop stands on (the control-point flag on
		 * its column) always gets a slot: without it the prop floats. */
		for( e = svs.maxclients + 1; e < svgame.numEntities && n_feet < (int)ARRAYSIZE( feet ); e++ )
		{
			edict_t *ed = SV_EdictNum( e );
			model_t *mod;

			if( !SV_IsValidEdict( ed ) || !ed->v.modelindex || FBitSet( ed->v.effects, EF_NODRAW ))
				continue;
			if( ed->v.iuser4 == GC_FILM_ACTOR_TAG )
				continue;
			mod = SV_ModelHandle( ed->v.modelindex );
			if( !mod || mod->type != mod_studio )
				continue;
			VectorSet( feet[n_feet], ed->v.origin[0], ed->v.origin[1], ed->v.origin[2] - 8.0f );
			n_feet++;
		}
		for( e = svs.maxclients + 1; e < svgame.numEntities; e++ )
		{
			edict_t *ed = SV_EdictNum( e );
			model_t *mod;
			vec3_t center, delta;

			if( !SV_IsValidEdict( ed ) || !ed->v.modelindex || FBitSet( ed->v.effects, EF_NODRAW ))
				continue;
			mod = SV_ModelHandle( ed->v.modelindex );
			if( !mod || mod->type != mod_brush )
				continue;
			if( mod->nummodelsurfaces <= 0 && !( mod->name[0] == '*' ))
				continue;
			VectorAverage( ed->v.absmin, ed->v.absmax, center );
			VectorSubtract( center, view_org, delta );
			{
				vec3_t half;
				float radius;

				VectorSubtract( ed->v.absmax, center, half );
				radius = VectorLength( half );
				float score = VectorLength( delta ) / ( radius + 16.0f );
				int f;

				/* The prop's origin need not touch the brush: the control-point
				 * flag's origin sits about 26 units above its pole's foot, and
				 * the pole is sunk into the top of the column (dod_charlie cp3
				 * on func_wall *178). So: over the box's footprint (8 units of
				 * slack), and between its bottom and 64 units over its top. */
				for( f = 0; f < n_feet; f++ )
				{
					if( feet[f][0] >= ed->v.absmin[0] - 8.0f && feet[f][0] <= ed->v.absmax[0] + 8.0f
						&& feet[f][1] >= ed->v.absmin[1] - 8.0f && feet[f][1] <= ed->v.absmax[1] + 8.0f
						&& feet[f][2] >= ed->v.absmin[2] && feet[f][2] <= ed->v.absmax[2] + 64.0f )
					{
						static const model_t *told[32];
						static int ntold;
						int ti;

						score = 0.0f;
						for( ti = 0; ti < ntold && told[ti] != mod; ti++ )
							;
						if( ti == ntold && ntold < (int)ARRAYSIZE( told ))
						{
							told[ntold++] = mod;
							Con_Reportf( "Xash3D GameCube: capture prop support %s rm=%d amt=%.0f top=%.0f prop feet=%.0f %.0f %.0f\n",
								mod->name, ed->v.rendermode, ed->v.renderamt, ed->v.absmax[2],
								feet[f][0], feet[f][1], feet[f][2] );
						}
						break;
					}
				}
				if( score > 0.0f && DotProduct( delta, fwd ) < -radius )
					continue;	/* behind the camera */
				CL_GCLeanPick( brush_picks, brush_dists, &n_brush_picks, budget, e, score );
			}
		}
	}

	for( int bi = svs.maxclients + 1; ( capture_brush_pick ? bi - ( svs.maxclients + 1 ) < n_brush_picks
		: bi < svgame.numEntities ) && added < budget; bi++ )
	{
		edict_t *ed;
		model_t *mod;
		cl_entity_t *ent;
		vec3_t delta, center;
		float dist;

		e = capture_brush_pick ? brush_picks[bi - ( svs.maxclients + 1 )] : bi;
		ed = SV_EdictNum( e );

		if( !SV_IsValidEdict( ed ))
			continue;
		if( !ed->v.modelindex )
			continue;
		if( FBitSet( ed->v.effects, EF_NODRAW ))
			continue;

		mod = SV_ModelHandle( ed->v.modelindex );
		if( !mod || mod->type != mod_brush )
		{
			scan_nomod++;
			continue;
		}
		scan_brush++;
		if( added >= budget )
		{
			brush_beyond_budget++;
			continue;
		}
		/* Inline *submodels may report surfaces via firstmodelsurface. */
		if( mod->nummodelsurfaces <= 0 && !( mod->name[0] == '*' ))
		{
			scan_nosurf++;
			continue;
		}

		VectorSubtract( ed->v.origin, view_org, delta );
		dist = VectorLength( delta );
		if( dist > max_dist )
		{
			VectorAverage( ed->v.absmin, ed->v.absmax, center );
			VectorSubtract( center, view_org, delta );
			dist = VectorLength( delta );
			if( dist > max_dist )
			{
				scan_far++;
				continue;
			}
		}

		if( ed->v.modelindex > 0 && ed->v.modelindex < MAX_MODELS )
			cl.models[ed->v.modelindex] = mod;

		/* Direct-map bootstrap keeps maxEntities=2 — do not use CL_EDICT_NUM.
		 * Stable per-frame slots are enough for R_AddEntity → DrawEntities. */
		ent = &lean_brush_ents[added];
		memset( ent, 0, sizeof( *ent ));
		ent->index = e;
		ent->player = 0;
		ent->model = mod;
		VectorCopy( ed->v.origin, ent->origin );
		VectorCopy( ed->v.angles, ent->angles );
		VectorCopy( ed->v.origin, ent->curstate.origin );
		VectorCopy( ed->v.angles, ent->curstate.angles );
		VectorCopy( ed->v.mins, ent->curstate.mins );
		VectorCopy( ed->v.maxs, ent->curstate.maxs );
		ent->curstate.modelindex = ed->v.modelindex;
		ent->curstate.solid = (int)ed->v.solid;
		ent->curstate.movetype = (int)ed->v.movetype;
		ent->curstate.rendermode = ed->v.rendermode;
		ent->curstate.renderamt = (int)ed->v.renderamt;
		ent->curstate.renderfx = ed->v.renderfx;
		ent->curstate.effects = ed->v.effects;
		ent->curstate.entityType = ENTITY_NORMAL;
		ent->prevstate = ent->curstate;
		VectorCopy( ent->origin, ent->latched.prevorigin );
		VectorCopy( ent->angles, ent->latched.prevangles );

		{
			const qboolean took = ref.dllFuncs.R_AddEntity( ent, ET_NORMAL );

			/* -gccapture: each brush model handed to the renderer, once
			 * (settles whether a support brush reaches the draw list). */
			if( capture_brush_pick )
			{
				static const model_t *handed[96];
				static int nhanded;
				int hi;

				for( hi = 0; hi < nhanded && handed[hi] != mod; hi++ )
					;
				if( hi == nhanded && nhanded < (int)ARRAYSIZE( handed ))
				{
					handed[nhanded++] = mod;
					Con_Reportf( "Xash3D GameCube: capture emit brush %s e=%d took=%d rm=%d amt=%d dist=%.0f slot=%d of %d\n",
						mod->name, e, took ? 1 : 0, ent->curstate.rendermode, ent->curstate.renderamt, dist,
						added, budget );
				}
			}
			if( took )
			{
				added++;
				if( FBitSet( ent->curstate.effects, EF_BRIGHTLIGHT | EF_DIMLIGHT | EF_LIGHT ))
					CL_AddEntityEffects( ent );
			}
		}
	}

	/* Static func_wall records (gc_static_walls 1): no edict, so the scan above never sees them. Draw the nearest
	 * visible ones from the record list, the way the static props are drawn. */
	if( SV_GCStaticWallCount() > 0 && SV_GCStaticWallsMode() == 1 && CL_GCLeanWallPool() )
	{
		cl_entity_t *lean_wall_ents = CL_GCLeanWallPool();
		extern qboolean R_GXWorldDLEntityVisible( int key, const float *mins, const float *maxs );
		int wpick[GC_LEAN_WALL_POOL], wn = 0, wi, wj, drawn = 0, wvisible = 0;
		float wdist[GC_LEAN_WALL_POOL];
		static int wmax_visible;

		for( wi = 0; wi < SV_GCStaticWallCount(); wi++ )
		{
			vec3_t wo, wmin, wmax, wabsmin, wabsmax, wc, wd;
			int wmi;
			float d;

			SV_GCStaticWallInfo( wi, wo, wmin, wmax, &wmi );
			SV_GCStaticWallBounds( wi, wabsmin, wabsmax, &wmi );
			VectorAverage( wabsmin, wabsmax, wc );
			VectorSubtract( wc, view_org, wd );
			d = VectorLength( wd );
			if( d > max_dist )
				continue;
			if( !R_GXWorldDLEntityVisible( 6144 + wi, wabsmin, wabsmax ))
				continue;
			wvisible++;
			if( wn < GC_LEAN_WALL_POOL )
				wn++;
			else if( d >= wdist[wn - 1] )
				continue;
			for( wj = wn - 1; wj > 0 && wdist[wj - 1] > d; wj-- )
			{
				wdist[wj] = wdist[wj - 1];
				wpick[wj] = wpick[wj - 1];
			}
			wdist[wj] = d;
			wpick[wj] = wi;
		}
		for( wi = 0; wi < wn; wi++ )
		{
			vec3_t wo, wmin, wmax;
			int wmi;
			cl_entity_t *went = &lean_wall_ents[drawn];
			model_t *wmod;

			SV_GCStaticWallInfo( wpick[wi], wo, wmin, wmax, &wmi );
			wmod = SV_ModelHandle( wmi );
			if( !wmod || wmod->type != mod_brush )
				continue;
			if( wmi > 0 && wmi < MAX_MODELS )
				cl.models[wmi] = wmod;
			memset( went, 0, sizeof( *went ));
			went->index = 0;
			went->model = wmod;
			VectorCopy( wo, went->origin );
			VectorCopy( wo, went->curstate.origin );
			VectorCopy( wmin, went->curstate.mins );
			VectorCopy( wmax, went->curstate.maxs );
			went->curstate.modelindex = wmi;
			went->curstate.solid = SOLID_BSP;
			went->curstate.movetype = MOVETYPE_PUSH;
			went->curstate.rendermode = kRenderNormal;
			went->curstate.entityType = ENTITY_NORMAL;
			went->prevstate = went->curstate;
			VectorCopy( went->origin, went->latched.prevorigin );
			if( ref.dllFuncs.R_AddEntity( went, ET_NORMAL ))
				drawn++;
		}
		{
			static int wall_logs;

			if( wvisible > wmax_visible )
			{
				wmax_visible = wvisible;
				Con_Reportf( "Xash3D GameCube: static walls: new max visible %d (pool %d, drawn %d, records %d)%s\n", wvisible,
					GC_LEAN_WALL_POOL, drawn, SV_GCStaticWallCount(), wvisible > GC_LEAN_WALL_POOL ? " TRUNCATED" : "" );
			}
			if( wall_logs++ < 3 || !( wall_logs % 600 ))
				Con_Reportf( "Xash3D GameCube: static walls drawn %d of %d records (%d visible, view %.0f %.0f %.0f)\n",
					drawn, SV_GCStaticWallCount(), wvisible, view_org[0], view_org[1], view_org[2] );
		}
	}

	{
		static int beyond_max, beyond_logs;

		if( brush_beyond_budget > beyond_max && beyond_logs++ < 12 )
		{
			beyond_max = brush_beyond_budget;
			Con_Reportf( "Xash3D GameCube: lean brush scan: %d brush edicts in range NOT drawn (budget %d, drew %d, scanned %d)\n",
				brush_beyond_budget, budget, added, scan_brush );
		}
	}
	GC_ProfMark( "render: lean emit brush scan" );

	/* Already-resident world studios (w_* weapons + allowlisted NPCs +
	 * CapFaces mapmodels). No Mod_ForName — G371 promotes after present≥24.
	 * CapFaces listen admits studios once densify has presented (≥2). */
	if( ( GC_GetNewGamePresentCount() >= ( Sys_CheckParm( "-gcworldrender" ) ? 2u : 16u )
			|| ( Sys_CheckParm( "-gcfullphysics" )
				&& ( Sys_CheckParm( "-gcdumpframes" ) || Sys_CheckParm( "-gcdump" ))))
		&& studio_budget > 0 )
	{
		int studio_slots = 0;
		int bodies[4];
		int n_bodies = 0;
		int picks[GC_LEAN_STUDIO_MAX];
		float pick_dists[GC_LEAN_STUDIO_MAX];
		/* Leave room for the other players and bots (emitted after). */
		const int pick_cap = Q_max( 1, studio_budget - Q_max( 0, svs.maxclients - 1 ));
		int n_picks = 0;
		int slot;

		for( e = svs.maxclients + 1; e < svgame.numEntities; e++ )
		{
			edict_t *ed = SV_EdictNum( e );
			model_t *mod;
			vec3_t delta;
			float dist;
			qboolean allow;
			qboolean is_film;
			qboolean mohaa_script_asset;

			if( !SV_IsValidEdict( ed ) || !ed->v.modelindex )
				continue;
			if( FBitSet( ed->v.effects, EF_NODRAW ))
				continue;
			/* Body-queue corpse (CopyToBodyQue: renderfx kRenderFxDeadPlayer,
			 * renderamt = the dead player's slot). Drawn after the players. */
			if( ed->v.renderfx == kRenderFxDeadPlayer )
			{
				if( n_bodies < (int)( sizeof( bodies ) / sizeof( bodies[0] )))
					bodies[n_bodies++] = e;
				continue;
			}
			{
				extern qboolean SV_GCEdictIsPackNamed( const edict_t *ent );

				/* Named map props are drawn from the prop pack: no studio load. */
				if( SV_GCEdictIsPackNamed( ed ))
					continue;
			}
			mod = SV_ModelHandle( ed->v.modelindex );
			/* Before the type test: an unloaded handle is not mod_studio yet. */
			if( mod && ( mod->needload != NL_PRESENT || !mod->cache.data ) && mod->name[0]
				&& Q_stristr( mod->name, "mapmodels/" ))
			{
				/* A map model precached by the game at spawn (DoD control
				 * point flags) stays a registered, never-loaded handle on this
				 * route: nothing promotes it the way the static props are.
				 * Point the handle at a resident copy, or load it (lean mesh,
				 * ARAM cache later), retrying a failed load every few seconds. */
				extern qboolean Mod_GCPromoteStudioPath( const char *path );
				static double next_try[32];
				static byte fails[32];
				static int fails_spawn = -1;
				const int mk = ed->v.modelindex & 31;
				model_t *found;

				if( fails_spawn != svs.spawncount )
				{
					memset( next_try, 0, sizeof( next_try ));
					memset( fails, 0, sizeof( fails ));
					fails_spawn = svs.spawncount;
				}
				found = Mod_FindName( mod->name, false );
				if(( !found || found->needload != NL_PRESENT || !found->cache.data )
					&& fails[mk] < 3 && host.realtime >= next_try[mk] )
				{
					if( Mod_GCPromoteStudioPath( mod->name ))
						found = Mod_FindName( mod->name, false );
					else
					{
						fails[mk]++;
						next_try[mk] = host.realtime + 5.0;
					}
					Con_Reportf( "Xash3D GameCube: lean promote map model %s (index %d) %s\n", mod->name,
						ed->v.modelindex, found && found->needload == NL_PRESENT && found->cache.data ? "resident" : "not loaded" );
				}
				if( found && found->type == mod_studio && found->needload == NL_PRESENT && found->cache.data )
				{
					if( ed->v.modelindex > 0 && ed->v.modelindex < MAX_MODELS )
						sv.models[ed->v.modelindex] = found;
					mod = found;
				}
			}
			if( mod && mod->type == mod_studio && mod->needload == NL_PRESENT && mod->cache.data
				&& Q_stristr( mod->name, "mapmodels/" ))
			{
				/* A promoted map model has its lean mesh only; attach its *t
				 * skin atlas too, or it draws flat grey (ceres 3282517bd).
				 * Bound skins return at once; stop checking after that. */
				extern qboolean Mod_GCAttachStudioSkins( model_t *m );
				static byte skin_state[32];	/* 0..2 fails, 0xff bound */
				static double skin_next[32];
				static int skin_spawn = -1;
				const int sk = ed->v.modelindex & 31;

				if( skin_spawn != svs.spawncount )
				{
					memset( skin_state, 0, sizeof( skin_state ));
					memset( skin_next, 0, sizeof( skin_next ));
					skin_spawn = svs.spawncount;
				}
				if( skin_state[sk] < 3 && host.realtime >= skin_next[sk] )
				{
					if( Mod_GCAttachStudioSkins( mod ))
					{
						skin_state[sk] = 0xff;
						R_GXStudioInvalidateBindings();
						Con_Reportf( "Xash3D GameCube: lean map model skins '%s' bound\n", mod->name );
					}
					else
					{
						skin_state[sk]++;
						skin_next[sk] = host.realtime + 5.0;
						Con_Reportf( "Xash3D GameCube: lean map model skins '%s' failed (%d)\n", mod->name, skin_state[sk] );
					}
				}
			}
			is_film = ( ed->v.iuser4 == GC_FILM_ACTOR_TAG );
			if( is_film && ( !mod || mod->needload != NL_PRESENT || !mod->cache.data )
				&& ed->v.modelindex > 0 && ed->v.modelindex < MAX_MODELS && sv.model_precache[ed->v.modelindex][0] )
			{
				/* The team body is precached by the game while the map loads,
				 * which on this route registers the name without loading it
				 * (the bots' bodies go through CL_GCLeanPlayerBody the same way).
				 * Load it once per model index and map. */
				static byte film_tried[MAX_MODELS / 8];
				static int film_spawn = -1;
				const int mi = ed->v.modelindex;

				if( film_spawn != svs.spawncount )
				{
					memset( film_tried, 0, sizeof( film_tried ));
					film_spawn = svs.spawncount;
				}
				if( !CHECKVISBIT( film_tried, mi ))
				{
					model_t *found;

					SETVISBIT( film_tried, mi );
					found = Mod_ForName( sv.model_precache[mi], false, false );
					Con_Reportf( "Xash3D GameCube: lean film actor body %s %s\n", sv.model_precache[mi],
						found && found->cache.data ? "loaded" : "load failed" );
					if( found && found->type == mod_studio && found->needload == NL_PRESENT && found->cache.data )
					{
						sv.models[mi] = found;
						mod = found;
					}
				}
				else
				{
					model_t *found = Mod_FindName( sv.model_precache[mi], false );

					if( found && found->type == mod_studio && found->needload == NL_PRESENT && found->cache.data )
						mod = found;
				}
			}
			if( !mod || mod->type != mod_studio )
				continue;
#if defined( XASH_MOHAA_GAMECUBE ) && XASH_MOHAA_GAMECUBE
			mohaa_script_asset = Q_stristr( mod->name, "models/vehicles/" )
				|| Q_stristr( mod->name, "models/human/" )
				|| Q_stristr( mod->name, "models/animal/" );
#else
			mohaa_script_asset = false;
#endif
			/* A weapon a player carries keeps its w_ model at the player's
			 * origin; it drew as a white rifle lying at the bot's feet
			 * (ceres 18e0d6f). Carried items belong to the player model. */
			if(( ed->v.owner && SV_IsValidEdict( ed->v.owner )
					&& NUM_FOR_EDICT( ed->v.owner ) >= 1 && NUM_FOR_EDICT( ed->v.owner ) <= svs.maxclients )
				|| ed->v.movetype == MOVETYPE_FOLLOW
				|| ( ed->v.aiment && SV_IsValidEdict( ed->v.aiment )))
			{
				static int carried_logs;

				if( carried_logs < 4 )
				{
					carried_logs++;
					Con_Reportf( "Xash3D GameCube: lean skip carried %s (entity %d, owner %d)\n",
						mod->name, e, NUM_FOR_EDICT( ed->v.owner ));
				}
				continue;
			}
			scan_studio++;
			if( mod->needload != NL_PRESENT || !mod->name[0] || !mod->cache.data )
			{
				scan_studio_skip++;
				continue;
			}
			/* Weapons + lean NPCs + Xen bosses + CapFaces mapmodels.
			 * Skip v_* / player. The answer only depends on the model name,
			 * and it took up to twenty case-insensitive substring searches
			 * per studio entity per frame: remember it per model. */
			if( is_film )
				allow = true;
			else
			{
				static struct { const model_t *mod; char name[40]; signed char ok; } memo[8];
				const unsigned mi = ( (unsigned)(size_t)mod >> 4 ) & 7u;

				if( memo[mi].mod == mod && memo[mi].ok >= 0 && !Q_strcmp( memo[mi].name, mod->name ))
					allow = memo[mi].ok != 0;
				else
				{
				allow = false;
				if( Q_stristr( mod->name, "w_" ) && !Q_stristr( mod->name, "v_" ))
					allow = true;
				else if( Q_stristr( mod->name, "mapmodels/" )
					|| mohaa_script_asset
					|| Q_stristr( mod->name, "hk_satchel" )
					|| Q_stristr( mod->name, "sandbags" )
					|| Q_stristr( mod->name, "by_rubble" )
					|| Q_stristr( mod->name, "horse_statue" )
					|| Q_stristr( mod->name, "by_canopy" )
					|| Q_stristr( mod->name, "arc_xer_tree" )
					|| Q_stristr( mod->name, "furniture" )
					|| Q_stristr( mod->name, "bulb" ))
					allow = true;
				else if( Q_stristr( mod->name, "scientist" )
					|| Q_stristr( mod->name, "barney" )
					|| Q_stristr( mod->name, "headcrab" )
					|| Q_stristr( mod->name, "zombie" )
					|| Q_stristr( mod->name, "roach" )
					|| Q_stristr( mod->name, "nihilanth" )
					|| Q_stristr( mod->name, "controller" )
					|| Q_stristr( mod->name, "islave" )
					|| Q_stristr( mod->name, "models/lc/" ))	/* Lost Coast cast */
					allow = true;
					if( Q_stristr( mod->name, "player.mdl" ))
						allow = false;
					if( Q_strlen( mod->name ) < sizeof( memo[mi].name ))
					{
						memo[mi].mod = mod;
						Q_strncpy( memo[mi].name, mod->name, sizeof( memo[mi].name ));
						memo[mi].ok = allow ? 1 : 0;
					}
				}
			}
			if( !allow )
			{
				scan_studio_skip++;
				continue;
			}
			VectorSubtract( ed->v.origin, view_org, delta );
			dist = VectorLength( delta );
			if( dist > studio_max_dist )
			{
				scan_studio_skip++;
				continue;
			}
			/* World DL maps carry their PVS: leave out what the eye cannot
			 * see instead of animating and skinning the whole map's cast. */
			{
				extern qboolean R_GXWorldDLEntityVisible( int key, const float *mins, const float *maxs );
				vec3_t bmin, bmax;

				VectorCopy( ed->v.absmin, bmin );
				VectorCopy( ed->v.absmax, bmax );
				if( bmax[0] - bmin[0] < 1.0f || bmax[2] - bmin[2] < 1.0f )
				{
					VectorSet( bmin, ed->v.origin[0] - 48.0f, ed->v.origin[1] - 48.0f, ed->v.origin[2] );
					VectorSet( bmax, ed->v.origin[0] + 48.0f, ed->v.origin[1] + 48.0f, ed->v.origin[2] + 80.0f );
				}
				/* These are moving MOHAA script models. Their GoldSrc edict bounds
				 * are placeholder-sized (2x2x2 for the m1l1 GMC), so WorldDL's
				 * static-world PVS rejects the truck even with the rider inside it.
				 * Keep the distance and renderer budgets, but let the GX frustum test
				 * the authored vehicle/actor model itself. */
				if( !mohaa_script_asset
					&& !R_GXWorldDLEntityVisible( e, bmin, bmax ))
				{
					scan_studio_skip++;
					continue;
				}
			}
			{
				static int pick_logs;

				if( pick_logs < 12 )
				{
					pick_logs++;
					Con_Reportf( "Xash3D GameCube: lean studio candidate %d %s class=%s owner=%d aiment=%d movetype=%d org=(%.0f %.0f %.0f)\n",
						e, mod->name, SV_GetString( ed->v.classname ),
						ed->v.owner ? NUM_FOR_EDICT( ed->v.owner ) : 0,
						ed->v.aiment ? NUM_FOR_EDICT( ed->v.aiment ) : 0,
						ed->v.movetype, ed->v.origin[0], ed->v.origin[1], ed->v.origin[2] );
				}
			}
			/* Insert into nearest-N picks. A film actor is what the shot is
			 * about: it goes ahead of every map prop. */
			CL_GCLeanPick( picks, pick_dists, &n_picks, pick_cap, e, is_film ? dist * 0.001f : dist );
		}
		/* Static props (unnamed env_model / dod_object, no edict). */
		for( e = 0; e < gc_num_static_props; e++ )
		{
			const gc_static_prop_t *sp = &gc_static_props[e];
			extern qboolean R_GXWorldDLEntityVisible( int key, const float *mins, const float *maxs );
			model_t *mod = SV_ModelHandle( sp->modelindex );
			vec3_t delta, bmin, bmax;
			float dist;

			if( !mod || mod->type != mod_studio || mod->needload != NL_PRESENT || !mod->cache.data )
				continue;
			VectorSubtract( sp->origin, view_org, delta );
			dist = VectorLength( delta );
			if( dist > studio_max_dist )
				continue;
			VectorSet( bmin, sp->origin[0] - 48.0f, sp->origin[1] - 48.0f, sp->origin[2] );
			VectorSet( bmax, sp->origin[0] + 48.0f, sp->origin[1] + 48.0f, sp->origin[2] + 80.0f );
			if( !R_GXWorldDLEntityVisible( GC_STATIC_PROP_KEY0 + e, bmin, bmax ))
				continue;
			CL_GCLeanPick( picks, pick_dists, &n_picks, pick_cap, -( e + 1 ), dist );
		}
		for( slot = 0; slot < n_picks && studio_slots < studio_budget; slot++ )
		{
			edict_t *ed;
			model_t *mod;
			cl_entity_t *ent = &lean_studio_ents[studio_slots];

			if( picks[slot] < 0 )
			{
				const gc_static_prop_t *sp = &gc_static_props[-picks[slot] - 1];

				mod = SV_ModelHandle( sp->modelindex );
				if( !mod )
					continue;
				if( sp->modelindex > 0 && sp->modelindex < MAX_MODELS )
					cl.models[sp->modelindex] = mod;
				memset( ent, 0, sizeof( *ent ));
				ent->index = 0;
				ent->model = mod;
				VectorCopy( sp->origin, ent->origin );
				VectorCopy( sp->angles, ent->angles );
				VectorCopy( sp->origin, ent->curstate.origin );
				VectorCopy( sp->angles, ent->curstate.angles );
				ent->curstate.modelindex = sp->modelindex;
				ent->curstate.sequence = sp->sequence;
				ent->curstate.body = sp->body;
				ent->curstate.skin = sp->skin;
				ent->curstate.scale = sp->scale > 0.0f ? sp->scale : 1.0f;
				ent->curstate.framerate = sp->framerate != 0.0f ? sp->framerate : 1.0f;
				ent->curstate.rendermode = sp->rendermode;
				ent->curstate.renderamt = sp->renderamt;
				ent->curstate.renderfx = sp->renderfx;
				ent->curstate.rendercolor = sp->rendercolor;
				ent->curstate.entityType = ENTITY_NORMAL;
				ent->prevstate = ent->curstate;
				VectorCopy( ent->origin, ent->latched.prevorigin );
				VectorCopy( ent->angles, ent->latched.prevangles );
				if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
				{
					studios++;
					studio_slots++;
				}
				continue;
			}
			ed = SV_EdictNum( picks[slot] );
			mod = SV_ModelHandle( ed->v.modelindex );
			if( !mod )
				continue;
			if( ed->v.modelindex > 0 && ed->v.modelindex < MAX_MODELS )
				cl.models[ed->v.modelindex] = mod;
			memset( ent, 0, sizeof( *ent ));
			ent->index = picks[slot];
			ent->model = mod;
			VectorCopy( ed->v.origin, ent->origin );
			VectorCopy( ed->v.angles, ent->angles );
			VectorCopy( ed->v.origin, ent->curstate.origin );
			VectorCopy( ed->v.angles, ent->curstate.angles );
			VectorCopy( ed->v.mins, ent->curstate.mins );
			VectorCopy( ed->v.maxs, ent->curstate.maxs );
			ent->curstate.modelindex = ed->v.modelindex;
			ent->curstate.solid = (int)ed->v.solid;
			ent->curstate.movetype = (int)ed->v.movetype;
			ent->curstate.sequence = ed->v.sequence;
			ent->curstate.frame = ed->v.frame;
			/* Bone controllers travel with every studio entity in GoldSrc. Left
			 * at the memset 0 they sit at the controller's start angle: the
			 * Lost Coast fisherman's four look joints held him bent over. */
			ent->curstate.controller[0] = ed->v.controller[0];
			ent->curstate.controller[1] = ed->v.controller[1];
			ent->curstate.controller[2] = ed->v.controller[2];
			ent->curstate.controller[3] = ed->v.controller[3];
			/* And the blends: a player model's aim grid (torso yaw, pitch). Left at 0
			 * the grid's corner had every scripted film actor looking up. */
			ent->curstate.blending[0] = ed->v.blending[0];
			ent->curstate.blending[1] = ed->v.blending[1];
			ent->curstate.body = ed->v.body;
			ent->curstate.skin = ed->v.skin;
			ent->curstate.scale = ed->v.scale > 0.0f ? ed->v.scale : 1.0f;
			ent->curstate.framerate = ed->v.framerate != 0.0f ? ed->v.framerate : 1.0f;
			/* A scripted film actor carries its class's p_ weapon like a player; the
			 * renderer merges it onto the hand bones (R_StudioDrawModel). */
			/* The renderer cross-fades a film actor's every change of sequence and gait
			 * (r_studio.c R_GXFilmBlend); it knows one by this tag. */
			ent->curstate.iuser4 = ed->v.iuser4 == GC_FILM_ACTOR_TAG ? GC_FILM_ACTOR_TAG : 0;
			if( ed->v.iuser4 == GC_FILM_ACTOR_TAG && ed->v.weaponmodel )
			{
				static int film_wlogs;

				ent->curstate.weaponmodel = CL_GCLeanWeaponIndex( SV_GetString( ed->v.weaponmodel ), true );
				if( film_wlogs < 4 )
				{
					film_wlogs++;
					Con_Reportf( "Xash3D GameCube: film actor %d weapon %s index %d\n", picks[slot],
						SV_GetString( ed->v.weaponmodel ), ent->curstate.weaponmodel );
				}
			}
			/* G377: edict animtime is 0 under lean StrikeThink. Snapping to
			 * cl.time every emit made EstimateFrame dfdt=0 (frozen pose). */
			ent->curstate.animtime = ed->v.animtime > 0.0f ? ed->v.animtime : 0.0f;
			ent->curstate.rendermode = ed->v.rendermode;
			ent->curstate.renderamt = (int)ed->v.renderamt;
			ent->curstate.effects = ed->v.effects;
			ent->curstate.entityType = ENTITY_NORMAL;
			/* G377: keep in-base group-0 sequences so GetAnim never opens
			 * scientist0N.mdl. Remap only seqgroup 1+ / OOB to a group-0 idle.
			 * Center look blends; skip latched seq crossfade. */
			if( Q_stristr( mod->name, "scientist" )
				|| Q_stristr( mod->name, "barney" )
				|| Q_stristr( mod->name, "headcrab" )
				|| Q_stristr( mod->name, "zombie" ))
			{
				studiohdr_t *hdr = (studiohdr_t *)mod->cache.data;
				int seq = ent->curstate.sequence;
				qboolean remap = ( seq < 0 );

				if( hdr && hdr->numseq > 0 )
				{
					if( seq >= hdr->numseq )
						remap = true;
					else
					{
						mstudioseqdesc_t *sd = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex ) + seq;
						if( sd->seqgroup != 0 )
							remap = true;
					}
					if( remap )
					{
						int i, idle = -1, any0 = -1;

						for( i = 0; i < hdr->numseq; i++ )
						{
							mstudioseqdesc_t *sd = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex ) + i;
							if( sd->seqgroup != 0 )
								continue;
							if( any0 < 0 )
								any0 = i;
							if( idle < 0 && Q_stristr( sd->label, "idle" ))
								idle = i;
						}
						ent->curstate.sequence = idle >= 0 ? idle : ( any0 >= 0 ? any0 : 0 );
					}
				}
				else if( seq < 0 || seq >= 41 )
					ent->curstate.sequence = 13;
				ent->curstate.blending[0] = 127;
				ent->curstate.blending[1] = 127;
				ent->curstate.controller[0] = ed->v.controller[0];
				ent->curstate.controller[1] = ed->v.controller[1];
				ent->curstate.controller[2] = ed->v.controller[2];
				ent->curstate.controller[3] = ed->v.controller[3];
				ent->latched.sequencetime = 0.0f;
				ent->angles[0] = 0.0f;
				ent->angles[2] = 0.0f;
				VectorCopy( ent->angles, ent->curstate.angles );
				ent->curstate.body = 0;
				ent->curstate.skin = 0;
			}
			ent->prevstate = ent->curstate;
			VectorCopy( ent->origin, ent->latched.prevorigin );
			VectorCopy( ent->angles, ent->latched.prevangles );
			ent->latched.prevanimtime = ent->curstate.animtime;
			memcpy( ent->latched.prevcontroller, ent->curstate.controller, sizeof( ent->latched.prevcontroller ));
			GC_LeanSeqLatch( ent, ed, mod );
			GC_LeanMoveLatch( ent );
			if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
			{
				studios++;
				studio_slots++;
			}
		}
		GC_ProfMark( "render: lean emit studio scan" );
		/* Other players and bots. The scan above starts after the client slots
		 * and skips player.mdl, so on this route nobody else was ever drawn.
		 * Emit them as players (ent->player) so the renderer swaps in the team
		 * body from their "model" userinfo (models/player/<model>/<model>.mdl). */
		{
			static int player_logs;
			static int player_reject_logs;
			static int player_last_reject[MAX_CLIENTS + 1];
			static qboolean player_body_tried[MAX_CLIENTS + 1];
			static qboolean player_scan_logged;
			static convar_t *gc_facebot;
			int near_e = 0;
			float near_d = 0.0f;
			vec3_t near_org = { 0.0f, 0.0f, 0.0f };

			/* gc_facebot 1 (console or listenserver.cfg): turn the local view
			 * toward the nearest drawn player each frame, so a fixed test
			 * run can capture a bot without steering. */
			if( !gc_facebot )
				gc_facebot = Cvar_FindVar( "gc_facebot" );

			if( !player_scan_logged )
			{
				player_scan_logged = true;
				Con_Reportf( "Xash3D GameCube: lean emit player scan maxclients=%d local=%d slots=%d/%d\n",
					svs.maxclients, cl.playernum + 1, studio_slots, studio_budget );
			}

			for( e = 1; e <= svs.maxclients && e <= MAX_CLIENTS && studio_slots < studio_budget; e++ )
			{
				edict_t *ed = SV_EdictNum( e );
				model_t *mod = NULL;
				cl_entity_t *ent;
				vec3_t delta;
				int reject = 0;
				qboolean dead;

				if( e == cl.playernum + 1 )
					continue;
				/* Spectator in-eye (OBS_IN_EYE 4): the view sits inside the
				 * followed player, so drawing its body puts the head and
				 * shoulders in front of the camera. iuser2 is the target. */
				if( SV_IsValidEdict( SV_EdictNum( cl.playernum + 1 )))
				{
					edict_t *self = SV_EdictNum( cl.playernum + 1 );

					if( self->v.iuser1 == 4 && self->v.iuser2 == e )
						continue;
				}
				if( !SV_IsValidEdict( ed ))
					reject = 1;
				else if( FBitSet( ed->v.effects, EF_NODRAW ))
					reject = 2;
				else if( ed->v.iuser1 != 0 )
					reject = 3;
				else
				{
					if( ed->v.modelindex )
						mod = SV_ModelHandle( ed->v.modelindex );
					if( !mod || mod->type != mod_studio || mod->needload != NL_PRESENT || !mod->cache.data )
					{
						/* The lean player.mdl may be a spill stub (no cache.data)
						 * or the bot may have no modelindex on this route. Any
						 * resident studio will do as the entity's model: the
						 * renderer swaps in the team body from the "model"
						 * userinfo anyway. Load that body once per slot. */
						mod = CL_GCLeanPlayerBody( e, &player_body_tried[e] );
						if( !mod || mod->type != mod_studio || mod->needload != NL_PRESENT || !mod->cache.data )
							reject = ed->v.modelindex ? 5 : 6;
					}
				}
				if( !reject )
				{
					VectorSubtract( ed->v.origin, view_org, delta );
					if( VectorLength( delta ) > studio_max_dist )
						reject = 7;
				}
				if( reject )
				{
					/* Log each slot's reason when it changes, so one run shows
					 * why a bot is not drawn: 1 invalid/free, 2 nodraw,
					 * 3 spectator, 5 model not resident, 6 no
					 * modelindex and no body, 7 out of range. */
					if( reject != 1 && player_last_reject[e] != reject && player_reject_logs < 32 )
					{
						model_t *sm = ( SV_IsValidEdict( ed ) && ed->v.modelindex ) ? SV_ModelHandle( ed->v.modelindex ) : NULL;

						player_reject_logs++;
						Con_Reportf( "Xash3D GameCube: lean emit player %d reject=%d modelindex=%d model=%s type=%d needload=%d cache=%s\n",
							e, reject, SV_IsValidEdict( ed ) ? ed->v.modelindex : -1,
							sm ? sm->name : "-", sm ? (int)sm->type : -1, sm ? (int)sm->needload : -1,
							sm && sm->cache.data ? "yes" : "no" );
					}
					player_last_reject[e] = reject;
					continue;
				}
				player_last_reject[e] = 0;
				dead = ed->v.deadflag != DEAD_NO;
				if( ed->v.modelindex > 0 && ed->v.modelindex < MAX_MODELS && SV_ModelHandle( ed->v.modelindex ) == mod )
					cl.models[ed->v.modelindex] = mod;
				ent = &lean_studio_ents[studio_slots];
				memset( ent, 0, sizeof( *ent ));
				ent->index = e;
				ent->player = true;
				ent->model = mod;
				VectorCopy( ed->v.origin, ent->origin );
				VectorCopy( ed->v.angles, ent->angles );
				VectorCopy( ent->origin, ent->curstate.origin );
				VectorCopy( ent->angles, ent->curstate.angles );
				VectorCopy( ed->v.mins, ent->curstate.mins );
				VectorCopy( ed->v.maxs, ent->curstate.maxs );
				ent->curstate.number = e;
				ent->curstate.modelindex = ed->v.modelindex;
				ent->curstate.solid = (int)ed->v.solid;
				ent->curstate.movetype = (int)ed->v.movetype;
				ent->curstate.sequence = ed->v.sequence;
				ent->curstate.gaitsequence = ed->v.gaitsequence;
				ent->curstate.frame = ed->v.frame;
				ent->curstate.body = ed->v.body;
				ent->curstate.skin = ed->v.skin;
				ent->curstate.scale = 1.0f;
				ent->curstate.framerate = ed->v.framerate != 0.0f ? ed->v.framerate : 1.0f;
				/* The server's player model is a hull-only stub, so the game
				 * code cannot look up sequences and leaves sequence 0 with
				 * animtime 0 (ceres acf6eef: the bot held one pose). Pick the
				 * body's own sequences by label from its speed, and keep a
				 * fixed animtime so the frame advances with cl.time (stamping
				 * cl.time every emit froze it at frame 0). */
				/* DoD's server stamps animtime = time every think, but with the
				 * hull stub its frame never advances, so animtime == time and
				 * the pose stuck at frame 0 (ceres 2ee8fa7). Use a per-slot
				 * base time, reset when the chosen sequence changes. */
				{
					static float anim_base[MAX_CLIENTS + 1];
					static int anim_seq[MAX_CLIENTS + 1];

					/* Dying restarts the clock even when the game's sequence
					 * number did not change, so the death plays from frame 0. */
					const int key = ent->curstate.sequence + 1 + ( dead ? 0x10000 : 0 );

					if( anim_base[e] <= 0.0f || anim_base[e] > (float)cl.time || anim_seq[e] != key )
					{
						anim_base[e] = (float)cl.time > 0.0f ? (float)cl.time : 0.001f;
						anim_seq[e] = key;
					}
					ent->curstate.animtime = anim_base[e];
					ent->curstate.frame = 0.0f;
				}
				{
					static int seq_logs;
					const studiohdr_t *bh = (const studiohdr_t *)mod->cache.data;
					float speed = sqrtf( ed->v.velocity[0] * ed->v.velocity[0] + ed->v.velocity[1] * ed->v.velocity[1] );
					int idle = CL_GCLeanFindSeq( bh, "idle" );
					int gait;
					/* One-shots on top of the aim/gait choice below: a shot (shoot
					 * pose of the carried weapon, torso only) and whole-body poses
					 * (stance change, jump, swim). anim clocks are per slot. */
					static float shoot_base[MAX_CLIENTS + 1], shoot_until[MAX_CLIENTS + 1];
					static float body_base[MAX_CLIENTS + 1], body_until[MAX_CLIENTS + 1], air_since[MAX_CLIENTS + 1];
					static int body_hold[MAX_CLIENTS + 1], prev_prone[MAX_CLIENTS + 1];
					qboolean shooting = false;
					int body_seq = -1;
					float body_t = 0.0f;

					if( speed > 150.0f )
						gait = CL_GCLeanFindSeq( bh, "run" );
					else if( speed > 10.0f )
						gait = CL_GCLeanFindSeq( bh, "walk" );
					else
						gait = idle;
					/* DoD bodies have no "idle"/"run" HL labels: sequence 0 is look_idle (arm held
					 * out). Use the DoD ones: stand_/crouch_<weapon>_aim for the carried weapon,
					 * dod_idle1, and dod_walk / dod_jog / dod_sprint for the legs. */
					{
						const char *tok = ed->v.weaponmodel ? CL_GCLeanDodAimToken( SV_GetString( ed->v.weaponmodel )) : NULL;
						char label[48];
						int aim = -1, dgait = -1;

						/* iuser3 bit 0 is prone, bit 1 deployed (dod-gc
						 * docs/DOD_ENGINE_REQUESTS.md). */
						const qboolean prone = ( ed->v.iuser3 & 1 ) != 0;

						if( tok )
						{
							Q_snprintf( label, sizeof( label ), "%s_%s_aim",
								prone ? "prone" : FBitSet( ed->v.flags, FL_DUCKING ) ? "crouch" : "stand", tok );
							aim = CL_GCLeanFindSeq( bh, label );
						}
						if( aim < 0 && prone )
							aim = CL_GCLeanFindSeq( bh, "prone_idle" );
						if( aim < 0 )
							aim = CL_GCLeanFindSeq( bh, "dod_idle1" );
						/* Crouched legs: dod_crawl when moving, dod_crouch_idle
						 * when still (standing legs under a crouched torso
						 * before). */
						if( prone )
							dgait = speed > 10.0f ? CL_GCLeanFindSeq( bh, "prone_forward" ) : aim;
						else if( FBitSet( ed->v.flags, FL_DUCKING ))
							dgait = CL_GCLeanFindSeq( bh, speed > 10.0f ? "dod_crawl" : "dod_crouch_idle" );
						else if( speed > 200.0f )
							dgait = CL_GCLeanFindSeq( bh, "dod_sprint" );
						else if( speed > 120.0f )
							dgait = CL_GCLeanFindSeq( bh, "dod_jog" );
						else if( speed > 10.0f )
							dgait = CL_GCLeanFindSeq( bh, "dod_walk" );
						else
						{
							/* Standing still: idle legs. The HL "idle" lookup
							 * finds look_idle, sequence 0, which the renderer
							 * reads as "no gait": the body then ignored the aim
							 * pitch whenever a bot stood still to shoot. */
							dgait = CL_GCLeanFindSeq( bh, "dod_idle1" );
							if( dgait <= 0 )
								dgait = CL_GCLeanFindSeq( bh, "standing_idle" );
						}
						/* Sprinting: the carried weapon's sprint aim pose. */
						if( tok && !prone && !FBitSet( ed->v.flags, FL_DUCKING ) && speed > 200.0f )
						{
							int sa;

							Q_snprintf( label, sizeof( label ), "sprint_%s_aim", tok );
							sa = CL_GCLeanFindSeq( bh, label );
							if( sa >= 0 )
								aim = sa;
						}
						/* A shot: the weapon sets EF_MUZZLEFLASH, which the server
						 * clears at the start of its next frame. Play the shoot pose
						 * of this stance for a third of a second. */
						if( FBitSet( ed->v.effects, EF_MUZZLEFLASH ) || sv_gc_muzzleflash[e] )
						{
							sv_gc_muzzleflash[e] = 0;
							shoot_base[e] = (float)cl.time;
							shoot_until[e] = (float)cl.time + 0.35f;
						}
						if( tok && !dead && cl.time < shoot_until[e] )
						{
							int ss;

							Q_snprintf( label, sizeof( label ), "%s_%s_shoot",
								prone ? "prone" : FBitSet( ed->v.flags, FL_DUCKING ) ? "crouch" : "stand", tok );
							ss = CL_GCLeanFindSeq( bh, label );
							if( ss >= 0 )
							{
								aim = ss;
								shooting = true;
							}
						}
						/* Whole-body poses, in priority order: a stance change,
						 * swimming, jumping. They replace torso and legs. */
						if( prone != prev_prone[e] && !dead )
						{
							const int ts = CL_GCLeanFindSeq( bh, prone ? "get_down" : "get_up" );

							prev_prone[e] = prone;
							if( ts > 0 )
							{
								body_hold[e] = ts;
								body_base[e] = (float)cl.time;
								body_until[e] = (float)cl.time + 0.9f;
							}
						}
						if( cl.time < body_until[e] && body_hold[e] > 0 )
						{
							body_seq = body_hold[e];
							body_t = body_base[e];
						}
						else if( ed->v.waterlevel > 1 )
						{
							body_seq = CL_GCLeanFindSeq( bh, speed > 10.0f ? "swim" : "swim_idle" );
							body_t = 0.0f;
						}
						else
						{
							if( FBitSet( ed->v.flags, FL_ONGROUND ))
								air_since[e] = 0.0f;
							else if( air_since[e] <= 0.0f )
								air_since[e] = (float)cl.time;
							if( air_since[e] > 0.0f && cl.time - air_since[e] > 0.2f )
							{
								body_seq = CL_GCLeanFindSeq( bh, "jump" );
								body_t = air_since[e];
							}
						}
						if( aim >= 0 )
							idle = aim;
						if( dgait >= 0 )
							gait = dgait;
					}
					if( dead )
					{
						/* One-shot death on the whole body: no legs overlay, and
						 * the renderer clamps a non-looping sequence on its last
						 * frame, so the corpse holds the final pose. */
						static int death_logs;

						ent->curstate.sequence = CL_GCLeanDeathSeq( bh, ed->v.sequence );
						ent->curstate.gaitsequence = 0;
						if( death_logs < 8 )
						{
							const mstudioseqdesc_t *sd = (const mstudioseqdesc_t *)((const byte *)bh + bh->seqindex );

							death_logs++;
							Con_Reportf( "Xash3D GameCube: lean player %d dead (deadflag %d) game seq %d -> %d '%s'\n",
								e, (int)ed->v.deadflag, ed->v.sequence, ent->curstate.sequence,
								( ent->curstate.sequence >= 0 && ent->curstate.sequence < bh->numseq ) ? sd[ent->curstate.sequence].label : "?" );
						}
					}
					else
					{
						if( ed->v.sequence == 0 && idle >= 0 )
							ent->curstate.sequence = idle;
						if( ed->v.gaitsequence == 0 && gait >= 0 )
							ent->curstate.gaitsequence = gait;
						if( shooting )
							ent->curstate.animtime = shoot_base[e];
						if( body_seq >= 0 )
						{
							/* No legs overlay on a whole-body pose, so the renderer
							 * keeps the root yaw-only and the pose plays as authored. */
							ent->curstate.sequence = body_seq;
							ent->curstate.gaitsequence = 0;
							if( body_t > 0.0f )
								ent->curstate.animtime = body_t;
						}
						{
							/* Which sequences the game actually plays: the first time each one is chosen,
							 * and every shot latch (EF_MUZZLEFLASH seen). */
							static byte an_seen[512];
							static int an_shots;
							const mstudioseqdesc_t *asd = (const mstudioseqdesc_t *)((const byte *)bh + bh->seqindex );
							const int qs = ent->curstate.sequence, qg = ent->curstate.gaitsequence;
							int k;

							for( k = 0; k < 2; k++ )
							{
								const int q = k ? qg : qs;

								if( q > 0 && q < bh->numseq && q < 512 && !an_seen[q] )
								{
									an_seen[q] = 1;
									Con_Reportf( "Xash3D GameCube: lean anim new seq %d '%s' (p%d t=%.1f speed %.0f)\n", q, asd[q].label, e, cl.time, speed );
								}
							}
							if( shooting && an_shots < 8 && ent->curstate.animtime == shoot_base[e] && cl.time - shoot_base[e] < 0.1f )
							{
								an_shots++;
								Con_Reportf( "Xash3D GameCube: lean anim shot p%d t=%.1f '%s'\n", e, cl.time, asd[qs].label );
							}
						}
					}
					if( seq_logs < 6 )
					{
						const mstudioseqdesc_t *sd = bh ? (const mstudioseqdesc_t *)((const byte *)bh + bh->seqindex ) : NULL;

						seq_logs++;
						int sq = ent->curstate.sequence, gq = ent->curstate.gaitsequence;
						qboolean sok = sd && sq >= 0 && sq < bh->numseq;
						qboolean gok = sd && gq >= 0 && gq < bh->numseq;

						Con_Reportf( "Xash3D GameCube: lean player %d seq %d '%s' (%d frames %.0f fps) gait %d '%s' (%d frames) speed %.0f numseq %d\n",
							e, sq, sok ? sd[sq].label : "?", sok ? sd[sq].numframes : -1, sok ? sd[sq].fps : 0.0f,
							gq, gok ? sd[gq].label : "?", gok ? sd[gq].numframes : -1,
							speed, bh ? bh->numseq : -1 );
					}
				}
				/* Keep the aim pitch (angles.x = -v_angle.x / 3): with a gait
				 * sequence the renderer turns it into the aim blend and
				 * leaves the root yaw-only (R_StudioPlayerBlend), as stock
				 * GoldSrc does. Decide on the gait this emit chose above:
				 * DoD's server leaves v.gaitsequence 0, so testing the edict
				 * zeroed the pitch of every bot (held at the baked mid
				 * blend, looking slightly up). Without a gait the pitch
				 * would tilt the whole body, so it stays 0 then. */
				if( dead || !ent->curstate.gaitsequence )
					ent->angles[0] = 0.0f;
				ent->curstate.angles[0] = ent->angles[0];
				{
					/* Aim check: what pitch each player hands the renderer. */
					static double next_aim_log[MAX_CLIENTS + 1];

					if( e <= MAX_CLIENTS && host.realtime >= next_aim_log[e] )
					{
						next_aim_log[e] = host.realtime + 2.0;
						Con_Reportf( "Xash3D GameCube: lean aim player %d v_angle.x %.1f angles.x %.1f gait %d seq %d -> pitch %.1f%s\n",
							e, ed->v.v_angle[0], ed->v.angles[0], ent->curstate.gaitsequence, ent->curstate.sequence,
							ent->angles[0], dead ? " (dead)" : "" );
					}
				}
				/* v.weaponmodel is a string_t (the p_ model path); the renderer
				 * wants a model index (ceres fpa: weaponmodel=28156). Attach it
				 * only when that model is resident. */
				ent->curstate.weaponmodel = 0;
				if( ed->v.weaponmodel && !dead )
				{
					const char *wpath = SV_GetString( ed->v.weaponmodel );
					static int wlogs;

					ent->curstate.weaponmodel = CL_GCLeanWeaponIndex( wpath, false );
					if( wlogs < 4 )
					{
						wlogs++;
						Con_Reportf( "Xash3D GameCube: lean player %d weapon %s index %d %s\n",
							e, wpath ? wpath : "?", ent->curstate.weaponmodel, ent->curstate.weaponmodel ? "attached" : "not resident" );
					}
				}
				ent->curstate.rendermode = ed->v.rendermode;
				ent->curstate.renderamt = (int)ed->v.renderamt;
				ent->curstate.effects = ed->v.effects;
				ent->curstate.entityType = ENTITY_NORMAL;
				ent->curstate.blending[0] = 127;
				ent->curstate.blending[1] = 127;
				ent->prevstate = ent->curstate;
				VectorCopy( ent->origin, ent->latched.prevorigin );
				VectorCopy( ent->angles, ent->latched.prevangles );
				ent->latched.prevanimtime = ent->curstate.animtime;
				if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
				{
					float d = VectorLength( delta );

					studios++;
					studio_slots++;
					lean_player_seen = true;
					/* gc_facebot 10+N: face player N instead of the nearest
					 * (a test aid: e.g. 12 = move to and aim at player 2). */
					const int facebot_pick = gc_facebot && gc_facebot->value >= 10.0f
						? (int)gc_facebot->value - 10 : 0;

					if( facebot_pick ? e == facebot_pick : ( !near_e || d < near_d ))
					{
						near_e = e;
						near_d = d;
						VectorCopy( ed->v.origin, near_org );
					}
					if( player_logs < 8 )
					{
						player_logs++;
						Con_Reportf( "Xash3D GameCube: lean emit player %d model=%s userinfo=%s seq=%d dist=%.0f org=(%.0f %.0f %.0f) yaw=%.0f\n",
							e, mod->name, cl.players[e - 1].model, ed->v.sequence, d,
							ed->v.origin[0], ed->v.origin[1], ed->v.origin[2],
							RAD2DEG( atan2f( delta[1], delta[0] )));
					}
				}
			}
			if( near_e && (( gc_facebot && gc_facebot->value != 0.0f ) || Sys_CheckParm( "-gcfacebot" )))
			{
				static int facebot_logs;
				static qboolean facebot_moved;
				vec3_t dir, ang;

				/* gc_facebot 2: also move the local player, once, to a free
				 * spot next to the bot (a hull trace out from the bot in 8
				 * directions, longest clear one), so walls between the spawn
				 * and the bot cannot hide it. */
				if( gc_facebot && gc_facebot->value >= 2.0f && !facebot_moved
					&& player && SV_IsValidEdict( player ))
				{
					float best = 0.0f;
					vec3_t best_pos;
					int k;

					for( k = 0; k < 8; k++ )
					{
						float a = (float)k * ( M_PI_F / 4.0f );
						vec3_t end;
						trace_t tr;
						float clear;

						end[0] = near_org[0] + cosf( a ) * 240.0f;
						end[1] = near_org[1] + sinf( a ) * 240.0f;
						end[2] = near_org[2];
						tr = SV_MoveNoEnts( near_org, player->v.mins, player->v.maxs, end, MOVE_NOMONSTERS, player );
						if( tr.allsolid || tr.startsolid )
							continue;
						clear = tr.fraction * 240.0f;
						if( clear > best )
						{
							best = clear;
							VectorLerp( near_org, Q_min( clear - 16.0f, 200.0f ) / 240.0f, end, best_pos );
						}
					}
					facebot_moved = true;
					if( best >= 96.0f )
					{
						VectorCopy( best_pos, player->v.origin );
						VectorClear( player->v.velocity );
						SV_LinkEdict( player, false );
						VectorCopy( best_pos, view_org );
					}
					Con_Reportf( "Xash3D GameCube: gc_facebot move %s to (%.0f %.0f %.0f), clear %.0f from player %d\n",
						best >= 96.0f ? "done" : "skipped", best >= 96.0f ? best_pos[0] : 0.0f,
						best >= 96.0f ? best_pos[1] : 0.0f, best >= 96.0f ? best_pos[2] : 0.0f, best, near_e );
				}
				VectorSubtract( near_org, view_org, dir );
				VectorAngles( dir, ang );
				if( ang[0] > 180.0f )
					ang[0] -= 360.0f;
				cl.viewangles[PITCH] = -ang[PITCH];
				cl.viewangles[YAW] = ang[YAW];
				cl.viewangles[ROLL] = 0.0f;
				/* Some lean camera paths read the local edict's v_angle. */
				if( player && SV_IsValidEdict( player ))
					VectorCopy( cl.viewangles, player->v.v_angle );
				if( facebot_logs < 4 )
				{
					facebot_logs++;
					Con_Reportf( "Xash3D GameCube: gc_facebot aim at player %d dist=%.0f angles=(%.0f %.0f)\n",
						near_e, near_d, cl.viewangles[PITCH], cl.viewangles[YAW] );
				}
			}
		}
		/* Body-queue corpses, drawn as their player (body from that slot's
		 * userinfo) holding the last frame of the death sequence. */
		for( slot = 0; slot < n_bodies && studio_slots < studio_budget; slot++ )
		{
			static qboolean body_tried[MAX_CLIENTS + 1];
			static int body_logs;
			edict_t *ed = SV_EdictNum( bodies[slot] );
			const int owner = (int)ed->v.renderamt;
			model_t *mod;
			cl_entity_t *ent;
			vec3_t delta, bmin, bmax;
			extern qboolean R_GXWorldDLEntityVisible( int key, const float *mins, const float *maxs );

			if( owner < 1 || owner > svs.maxclients || owner > MAX_CLIENTS )
				continue;
			VectorSubtract( ed->v.origin, view_org, delta );
			if( VectorLength( delta ) > studio_max_dist )
				continue;
			VectorSet( bmin, ed->v.origin[0] - 48.0f, ed->v.origin[1] - 48.0f, ed->v.origin[2] - 36.0f );
			VectorSet( bmax, ed->v.origin[0] + 48.0f, ed->v.origin[1] + 48.0f, ed->v.origin[2] + 36.0f );
			if( !R_GXWorldDLEntityVisible( bodies[slot], bmin, bmax ))
				continue;
			mod = CL_GCLeanPlayerBody( owner, &body_tried[owner] );
			if( !mod )
				continue;
			ent = &lean_studio_ents[studio_slots];
			memset( ent, 0, sizeof( *ent ));
			ent->index = bodies[slot];
			ent->player = true;
			ent->model = mod;
			VectorCopy( ed->v.origin, ent->origin );
			VectorCopy( ed->v.angles, ent->angles );
			ent->angles[0] = 0.0f;
			VectorCopy( ent->origin, ent->curstate.origin );
			VectorCopy( ent->angles, ent->curstate.angles );
			ent->curstate.number = owner;	/* renderer picks the body by slot */
			ent->curstate.modelindex = ed->v.modelindex;
			ent->curstate.movetype = MOVETYPE_NONE;
			ent->curstate.sequence = CL_GCLeanDeathSeq( (const studiohdr_t *)mod->cache.data, ed->v.sequence );
			ent->curstate.gaitsequence = 0;
			ent->curstate.frame = 255.0f;
			ent->curstate.framerate = 0.0f;
			ent->curstate.animtime = (float)cl.time;
			ent->curstate.body = ed->v.body;
			ent->curstate.skin = ed->v.skin;
			ent->curstate.scale = 1.0f;
			ent->curstate.rendermode = kRenderNormal;
			ent->curstate.renderamt = 255;
			ent->curstate.entityType = ENTITY_NORMAL;
			ent->curstate.blending[0] = 127;
			ent->curstate.blending[1] = 127;
			ent->prevstate = ent->curstate;
			VectorCopy( ent->origin, ent->latched.prevorigin );
			VectorCopy( ent->angles, ent->latched.prevangles );
			ent->latched.prevanimtime = ent->curstate.animtime;
			if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
			{
				studios++;
				studio_slots++;
				if( body_logs < 8 )
				{
					body_logs++;
					Con_Reportf( "Xash3D GameCube: lean corpse %d of player %d %s seq %d at (%.0f %.0f %.0f)\n",
						bodies[slot], owner, mod->name, ent->curstate.sequence,
						ed->v.origin[0], ed->v.origin[1], ed->v.origin[2] );
				}
			}
		}
		GC_ProfMark( "render: lean emit players" );
		/* Once a player body has been drawn the stub is not needed, and it
		 * showed as a white plank on the floor near the bot (ceres fld). */
#if XASH_GC_LOSTCOAST
		/* Not on Lost Coast: with no character in view (the monastery stairs,
		 * the church) the stub sat 48 units in front of the eye, a crowbar
		 * that looked ten times too large. */
		if( 0 )
#else
		if( studios == 0 && !lean_player_seen )
#endif
		{
			/* Prefer tiny lean w_crowbar stub — v_crowbar hung Flipper
			 * (probe 20260807-171034: studios=1 then silence). */
			model_t *mod = Mod_FindName( "models/w_crowbar.mdl", false );
			cl_entity_t *ent = &lean_studio_ents[0];

			if( mod && mod->type == mod_studio && mod->needload == NL_PRESENT
				&& mod->cache.data )
			{
				memset( ent, 0, sizeof( *ent ));
				ent->index = 0;
				ent->model = mod;
				VectorCopy( view_org, ent->origin );
				ent->origin[0] += 48.0f;
				ent->origin[2] += 16.0f;
				VectorCopy( ent->origin, ent->curstate.origin );
				ent->curstate.modelindex = 0;
				ent->curstate.solid = SOLID_NOT;
				ent->curstate.movetype = MOVETYPE_NONE;
				ent->curstate.entityType = ENTITY_NORMAL;
				ent->prevstate = ent->curstate;
				VectorCopy( ent->origin, ent->latched.prevorigin );
				if( ref.dllFuncs.R_AddEntity( ent, ET_NORMAL ))
				{
					static int stub_logs;

					studios++;
					if( stub_logs < 4 )
					{
						stub_logs++;
						Con_Reportf( "Xash3D GameCube: lean stub w_crowbar at (%.0f %.0f %.0f)\n",
							ent->origin[0], ent->origin[1], ent->origin[2] );
					}
				}
			}
		}

		/* This route never runs V_RenderView/V_SetupViewModel, so the held
		 * weapon was never bound (ceres lt3: vm=0, no "viewmodel index"
		 * line). Bind the server's viewmodel here, loading it on demand. */
		{
			int vmi = cl.local.viewmodel ? cl.local.viewmodel
				: cl.frames[cl.parsecountmod].clientdata.viewmodel;
			model_t *held = vmi > 0 ? CL_ModelHandle( vmi ) : NULL;
			static int held_logged = -1, held_logs;
			char vpath[MAX_QPATH];

			vpath[0] = '\0';
			if( held && held->name[0] )
				Q_strncpy( vpath, held->name, sizeof( vpath ));
			/* Listen server: a late precache (game Deploy after connect) may
			 * not have reached cl.models yet (ceres: index=294 model=(null)).
			 * The name is in our own server's precache table. */
			else if( vmi > 0 && vmi < MAX_MODELS && sv.model_precache[vmi][0] )
				Q_strncpy( vpath, sv.model_precache[vmi], sizeof( vpath ));

			/* A cinematic shot that hides the weapon (cine_viewmodel 0) never
			 * draws it, so it does not load it either: the 30-120 KB studio
			 * and its skins stay free for the scripted actors and effects. */
			if( vpath[0] && Q_stristr( vpath, ".mdl" ) && !CL_CineHideViewModel()
				&& ( !held || held->type != mod_studio || !held->cache.data ))
			{
				/* A failed load is retried once a second, not every frame:
				 * each attempt clears the FS miss cache and probes the disc
				 * (ceres b302741eb ran the budget-skip loop until exit). */
				static int held_retry_vmi = -1;
				static double held_retry_time;

				if( vmi != held_retry_vmi || cl.time < held_retry_time
					|| cl.time - held_retry_time >= 1.0 )
				{
					held_retry_vmi = vmi;
					held_retry_time = cl.time;
					if( Mod_GCEnsureLandmarkViewModel( vpath ))
						held = Mod_FindName( vpath, false );
				}
			}
			/* No held weapon (spectator, dead, holstered): drop the last
			 * one, or the old gun stayed bound and drawn for good. */
			if( vmi <= 0 && clgame.viewent.model )
			{
				clgame.viewent.model = NULL;
				clgame.viewent.curstate.modelindex = 0;
			}
			if( held && held->type == mod_studio && held->cache.data
				&& clgame.viewent.model != held )
			{
				/* A gun the server precache already made resident skipped
				 * the ensure call above, so it never got its *t skins (flat
				 * pink v_enfield) or became the protected held slot. */
				Mod_GCEnsureLandmarkViewModel( vpath );
				clgame.viewent.model = held;
				clgame.viewent.curstate.modelindex = vmi;
				clgame.viewent.curstate.animtime = (float)cl.time;
				clgame.viewent.curstate.framerate = 1.0f;
				clgame.viewent.curstate.rendermode = kRenderNormal;
				VectorCopy( view_org, clgame.viewent.origin );
				VectorCopy( clgame.viewent.origin, clgame.viewent.curstate.origin );
			}
			if( vmi != held_logged && held_logs < 8 )
			{
				held_logs++;
				held_logged = vmi;
				Con_Reportf( "Xash3D GameCube: lean held viewmodel index=%d name=%s model=%s bound=%d\n",
					vmi, vpath[0] ? vpath : "(none)", held ? held->name : "(null)",
					held && clgame.viewent.model == held ? 1 : 0 );
			}
		}

		/* Landmark viewmodel for Flipper G155 gun pass (G105 crowbar).
		 * Bind even when no world NPC studios made the lean list — tip-safe
		 * c0a0 often has studios=0 while crowbar is already G105-ready. */
#if XASH_GC_LOSTCOAST
		/* Lost Coast has no view models baked yet. The HL1 crowbar this
		 * fallback binds is not on its disc as a usable model: it was bound
		 * again every frame and drew as a white mesh ten times too large in
		 * the world (the monastery stairs, the church). */
		if( 0 )
#else
		if( !clgame.viewent.model
			|| clgame.viewent.model->type != mod_studio
			|| !clgame.viewent.model->cache.data )
#endif
		{
			const char *vpath = Mod_GCLandmarkViewModelPath();
			model_t *vm = NULL;

			if( !vpath || !vpath[0] )
				vpath = "models/v_crowbar.mdl";
			if( Mod_GCEnsureLandmarkViewModel( vpath ))
				vm = Mod_FindName( vpath, false );
			if( vm && vm->type == mod_studio && vm->cache.data )
			{
				clgame.viewent.model = vm;
				VectorCopy( view_org, clgame.viewent.origin );
				VectorCopy( clgame.viewent.origin, clgame.viewent.curstate.origin );
				clgame.viewent.curstate.animtime = (float)cl.time;
				clgame.viewent.curstate.framerate = 1.0f;
				clgame.viewent.curstate.sequence = 0;
				clgame.viewent.curstate.rendermode = kRenderNormal;
				Con_Reportf( "Xash3D GameCube: lean EmitBrush bind viewmodel %s\n", vpath );
			}
		}

#if XASH_GC_LOSTCOAST
		/* However it got bound (the fallback above, or the weapon the HL1
		 * player code holds), Lost Coast draws no view model: none is baked
		 * for it, and the HL1 crowbar showed as a huge white mesh. */
		clgame.viewent.model = NULL;
		clgame.viewent.curstate.modelindex = 0;
#endif

		/* Early G291 seed (present 0/2) dies before studio admit (present≥16).
		 * Reseed tip-safe particles once so lean EFX can emit. */
		if( studios > 0 )
		{
			static qboolean lean_efx_reseeded;

			if( !lean_efx_reseeded )
			{
				CL_GCSeedFlipperEfxProof( view_org );
				lean_efx_reseeded = true;
			}
		}
	}

	/* R_ClearScene above dropped the temp entities CL_EmitEntities linked. */
	CL_GCRelinkTempEnts();

	if( log_n < 4 || ( studios > 0 && log_n < 8 ))
	{
		Con_Reportf( "Xash3D GameCube: lean EmitBrush entities=%d studios=%d ents=%d brush=%d studio_seen=%d skip=%d view=(%.0f,%.0f,%.0f)\n",
			added, studios, svgame.numEntities, scan_brush, scan_studio, scan_studio_skip,
			view_org[0], view_org[1], view_org[2] );
		log_n++;
	}
}
#endif

#if XASH_GAMECUBE
/* -gcprofile: every 5 s, the studio-model entities in the current packet:
 * model loaded or not, animation state, and how often GX drew each since
 * the last dump (R_GCStudioDrawCount). */
static void CL_GCDumpStudioEntities( frame_t *frame )
{
	extern unsigned R_GCStudioDrawCount( int index );
	static unsigned last_draws[1024];
	static double next_dump;
	static int profile = -1;
	int i, n = 0;

	if( profile < 0 )
		profile = Sys_CheckParm( "-gcprofile" ) ? 1 : 0;
	if( !profile || host.realtime < next_dump )
		return;
	next_dump = host.realtime + 5.0;

	for( i = 0; i < frame->num_entities && n < 24; i++ )
	{
		const entity_state_t *st = &cls.packet_entities[( frame->first_entity + i ) % cls.num_client_entities];
		cl_entity_t *e = CL_EDICT_NUM( st->number );
		model_t *mod = CL_ModelHandle( st->modelindex );
		unsigned draws;

		if( !mod || mod->type != mod_studio || st->number <= cl.maxclients )
			continue;
		draws = R_GCStudioDrawCount( st->number );
		Con_Reportf( "Xash3D GameCube: npc cl map=%s ent=%d model=%s loaded=%d org=%.0f %.0f %.0f seq=%d frame=%.1f fr=%.2f anim=%.1f draws=+%u\n",
			clgame.mapname, st->number, mod->name, mod->cache.data ? 1 : 0,
			e->origin[0], e->origin[1], e->origin[2], st->sequence, st->frame,
			st->framerate, st->animtime, draws - last_draws[st->number & 1023] );
		last_draws[st->number & 1023] = draws;
		n++;
	}
}
#endif

void CL_EmitEntities( void )
{
	if( cl.paused ) return; // don't waste time

	// not in server yet, no entities to redraw
	if( cls.state != ca_active || !cl.validsequence )
		return;

	// make sure we have at least one valid update
	if( !cl.frames[cl.parsecountmod].valid )
		return;

	// animate lightestyles
	ref.dllFuncs.CL_RunLightStyles( CL_GetLightStyle( 0 ));

	// decay dynamic lights
	CL_DecayLights ();

	// compute last interpolation amount
	CL_UpdateFrameLerp ();

	// set client ideal pitch when mlook is disabled
	CL_SetIdealPitch ();

	ref.dllFuncs.R_ClearScene ();

	// link all the visible clients first
	CL_LinkPlayers ( &cl.frames[cl.parsecountmod] );

	// link all the entities that actually have update
	CL_LinkPacketEntities ( &cl.frames[cl.parsecountmod] );
#if XASH_GAMECUBE
	CL_GCDumpStudioEntities( &cl.frames[cl.parsecountmod] );
	CL_GCLinkRemoteStaticProps();
#endif

	// link custom user temp entities
	clgame.dllFuncs.pfnCreateEntities();

	// evaluate temp entities
	CL_TempEntUpdate ();

	// fire events (client and server)
	CL_FireEvents ();

	// handle spectator camera movement
	CL_MoveSpectatorCamera();

	// perfomance test
	CL_TestLights();
}

/*
==========================================================================

SOUND ENGINE IMPLEMENTATION

==========================================================================
*/
qboolean CL_GetEntitySpatialization( channel_t *ch )
{
	if( ch->entnum == 0 )
	{
		SetBits( ch->flags, FL_CHAN_STATIC_SOUND );
		return true; // static sound
	}

	if(( ch->entnum - 1 ) == cl.playernum )
	{
		VectorCopy( refState.vieworg, ch->origin );
		return true;
	}

	qboolean valid_origin = VectorIsNull( ch->origin ) ? false : true;
	cl_entity_t *ent = CL_GetEntityByIndex( ch->entnum );

	// entity is not present on the client but has valid origin
	if( !ent || !ent->model || ent->curstate.messagenum != cl.parsecount )
		return valid_origin;

	// setup origin
	if( ent->model->type == mod_brush )
	{
		VectorAverage( ent->model->mins, ent->model->maxs, ch->origin );
		VectorAdd( ent->origin, ch->origin, ch->origin );
	}
	else
	{
		VectorCopy( ent->origin, ch->origin );
	}

	return true;
}

qboolean CL_GetMovieSpatialization( rawchan_t *ch )
{
	cl_entity_t	*ent;
	qboolean		valid_origin;

	valid_origin = VectorIsNull( ch->origin ) ? false : true;
	ent = CL_GetEntityByIndex( ch->entnum );

	// entity is not present on the client but has valid origin
	if( !ent || !ent->index || ent->curstate.messagenum == 0 )
		return valid_origin;

	// setup origin
	if( ent->model->type == mod_brush )
	{
		VectorAverage( ent->model->mins, ent->model->maxs, ch->origin );
		VectorAdd( ent->origin, ch->origin, ch->origin );
	}
	else
	{
		VectorCopy( ent->origin, ch->origin );
	}

	return true;
}

#if XASH_GAMECUBE
/* The name the (listen) server precached at this model index, or NULL: cl_game.c has no view of server_t. */
const char *CL_GCServerModelName( int modelindex )
{
	if( modelindex <= 0 || modelindex >= MAX_MODELS || !SV_Active() || !sv.model_precache[modelindex][0] )
		return NULL;
	return sv.model_precache[modelindex];
}
#endif
