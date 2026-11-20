#ifndef XASH_GC_SPEC_CAM_H
/* Copy of dod-gc src/client/dod_cam.h: the GameCube routes build the spectator camera in the
 * engine (GC_SpectatorEye) and never run the client V_CalcRefdef, so the same shot and
 * smoothing math lives here too. Keep the two in step. */
#define XASH_GC_SPEC_CAM_H

/* Smooth spectator chase camera. Header-only so the client DLL and the host
 * tests share one implementation (no libm beyond expf).
 *
 * The chase cameras place the view straight from the followed player's
 * entity origin, which only changes when a network update arrives, so the
 * picture steps (worse on the GameCube, where updates are slow). This low
 * passes the camera origin and angles with a time constant of about a tenth
 * of a second, and snaps instead of swooping whenever the camera should
 * jump: a new target, a mode change, a teleport or respawn, a long frame. */

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DODCAM_CHASE_LOCKED 1
#define DODCAM_CHASE_FREE 2
#define DODCAM_ROAMING 3

#define DODCAM_ORG_RATE 9.0f		/* 1/s: origin catches up in about 0.11 s */
#define DODCAM_ANG_RATE 14.0f		/* 1/s: angles a little tighter, so aiming feels direct */
#define DODCAM_SNAP_DIST 300.0f		/* units: further than this is a jump, not a move */
#define DODCAM_MAX_DT 0.25f		/* seconds: after a hitch, do not blend over the gap */

typedef struct DodCamState {
	float org[3];
	float ang[3];
	int mode;
	int target;
	int primed;
} DodCamState;

static inline float DodCam_AngleDelta(float from, float to)
{
	float d = fmodf(to - from, 360.0f);
	if (d >= 180.0f)
		d -= 360.0f;
	else if (d < -180.0f)
		d += 360.0f;
	return d;
}

/* Fraction of the remaining distance to cover this frame. */
static inline float DodCam_Blend(float rate, float dt)
{
	if (dt <= 0.0f)
		return 0.0f;
	return 1.0f - expf(-rate * dt);
}

static inline void DodCam_Reset(DodCamState *s)
{
	if (s)
		s->primed = 0;
}

/* Keep the horizon level. The free cameras take their angles from the spectator's
 * own view angles, which can carry a roll (strafe lean, damage and punch kicks) or
 * a pitch past the poles; the chase and roaming views want neither. In-eye keeps
 * the followed player's own view. Roll is zeroed and pitch clamped to +-89. */
static inline void DodCam_Level(int mode, float ang[3])
{
	if (mode != DODCAM_CHASE_LOCKED && mode != DODCAM_CHASE_FREE && mode != DODCAM_ROAMING)
		return;
	ang[2] = 0.0f;
	if (ang[0] > 89.0f)
		ang[0] = 89.0f;
	else if (ang[0] < -89.0f)
		ang[0] = -89.0f;
}

/* A camera that a trace stopped early sits on the wall it hit. Back it off the surface
 * by `margin` units along the ray so the near plane does not cut into the wall, and never
 * past the start. start/end are the ray, fraction is the trace's clear fraction (1 = free). */
static inline void DodCam_PullIn(const float start[3], const float end[3], float fraction,
	float margin, float out[3])
{
	float d[3], len, f;
	int i;

	for (i = 0; i < 3; i++)
		d[i] = end[i] - start[i];
	len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	f = fraction;
	if (f > 1.0f)
		f = 1.0f;
	if (f < 1.0f && len > 0.001f)
		f -= margin / len;
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	for (i = 0; i < 3; i++)
		out[i] = start[i] + d[i] * f;
}

/* Cinematic spectating: instead of a fixed chase offset, the camera runs a sequence of
 * moving shots around the followed player and cuts to the next every DODCAM_SHOT_TIME
 * seconds (or at once when the director picks someone else). Each shot is a smooth path
 * relative to the target, always aimed at the target's chest, and the caller clips it
 * against the world from the target's eye so a shot never looks through a wall. */
#define DODCAM_SHOT_TIME 6.0f
#define DODCAM_SHOTS 5

typedef enum {
	DODCAM_SHOT_ORBIT = 0,	/* slow circle at mid distance, eye height */
	DODCAM_SHOT_TRACK,	/* three-quarter from behind, low, drifting round */
	DODCAM_SHOT_CRANE,	/* starts high and close, rises and pulls back */
	DODCAM_SHOT_FLYBY,	/* ahead and to the side, slides past */
	DODCAM_SHOT_LOW		/* low hero angle from the front quarter, slow push in */
} DodCamShot;

static inline float DodCam_Lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* Ease in and out over a shot so moves start and stop gently. */
static inline float DodCam_Ease(float t)
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

/* Aim from `from` at `to` in GoldSrc view angles: looking down is a positive pitch. */
static inline void DodCam_LookAt(const float from[3], const float to[3], float ang[3])
{
	const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
	const float flat = sqrtf(dx * dx + dy * dy);
	ang[1] = atan2f(dy, dx) * (180.0f / 3.14159265f);
	ang[0] = -atan2f(dz, flat > 0.001f ? flat : 0.001f) * (180.0f / 3.14159265f);
	ang[2] = 0.0f;
}

/* Camera for `shot` at `t` (0..1 through the shot) around a target standing at `feet`
 * facing `yaw` degrees. `seed` varies the side a shot favours so repeats differ. */
static inline void DodCam_ShotPose(int shot, float t, const float feet[3], float yaw, int seed,
	float out_org[3], float out_ang[3])
{
	const float side = (seed & 1) ? 1.0f : -1.0f;
	const float e = DodCam_Ease(t);
	float bearing, dist, height, aim[3], rad;

	switch (shot % DODCAM_SHOTS) {
	case DODCAM_SHOT_ORBIT:
		bearing = yaw + 180.0f + side * DodCam_Lerp(-40.0f, 40.0f, t);
		dist = 150.0f;
		height = 52.0f;
		break;
	case DODCAM_SHOT_TRACK:
		bearing = yaw + 180.0f + side * DodCam_Lerp(25.0f, 50.0f, e);
		dist = 110.0f;
		height = 30.0f;
		break;
	case DODCAM_SHOT_CRANE:
		bearing = yaw + 180.0f + side * 30.0f;
		dist = DodCam_Lerp(90.0f, 200.0f, e);
		height = DodCam_Lerp(110.0f, 190.0f, e);
		break;
	case DODCAM_SHOT_FLYBY:
		bearing = yaw + side * DodCam_Lerp(30.0f, 150.0f, e);
		dist = 170.0f;
		height = 64.0f;
		break;
	default: /* DODCAM_SHOT_LOW */
		bearing = yaw + side * 35.0f;
		dist = DodCam_Lerp(170.0f, 120.0f, e);
		height = 12.0f;
		break;
	}
	rad = bearing * (3.14159265f / 180.0f);
	out_org[0] = feet[0] + cosf(rad) * dist;
	out_org[1] = feet[1] + sinf(rad) * dist;
	out_org[2] = feet[2] + height;
	aim[0] = feet[0];
	aim[1] = feet[1];
	aim[2] = feet[2] + 56.0f;	/* chest of a standing soldier */
	DodCam_LookAt(out_org, aim, out_ang);
}

typedef struct DodCamCine {
	int target;
	int shot;
	float start;	/* client time the current shot began */
	float yaw;	/* target's facing when the shot began: the path stays put while it turns */
	int primed;
} DodCamCine;

/* Picks the shot for `now`: a new target or an expired shot starts the next one. Returns
 * nonzero when this frame is a cut (the caller should not blend across it). */
static inline int DodCam_CineStep(DodCamCine *c, int target, float now, float *t_out)
{
	int cut = 0;
	if (!c->primed || c->target != target || now < c->start || now - c->start >= DODCAM_SHOT_TIME) {
		if (c->primed)
			c->shot = (c->shot + 1) % DODCAM_SHOTS;
		c->primed = 1;
		c->target = target;
		c->start = now;
		cut = 1;
	}
	*t_out = (now - c->start) / DODCAM_SHOT_TIME;
	return cut;
}

/* Smooths org/ang into out_org/out_ang (which may alias the inputs). Modes other
 * than the two chase cameras pass through untouched and reset the filter. */
static inline void DodCam_Smooth(DodCamState *s, int mode, int target,
	const float org[3], const float ang[3], float dt, float out_org[3], float out_ang[3])
{
	int i, snap;
	float dx, dy, dz, a_org, a_ang;

	if (!s) {
		for (i = 0; i < 3; i++) {
			out_org[i] = org[i];
			out_ang[i] = ang[i];
		}
		return;
	}
	if (mode != DODCAM_CHASE_LOCKED && mode != DODCAM_CHASE_FREE) {
		s->primed = 0;
		for (i = 0; i < 3; i++) {
			out_org[i] = org[i];
			out_ang[i] = ang[i];
		}
		return;
	}
	dx = org[0] - s->org[0];
	dy = org[1] - s->org[1];
	dz = org[2] - s->org[2];
	snap = !s->primed || s->mode != mode || s->target != target || dt > DODCAM_MAX_DT ||
		dx * dx + dy * dy + dz * dz > DODCAM_SNAP_DIST * DODCAM_SNAP_DIST;
	if (snap) {
		for (i = 0; i < 3; i++) {
			s->org[i] = org[i];
			s->ang[i] = ang[i];
		}
	} else {
		a_org = DodCam_Blend(DODCAM_ORG_RATE, dt);
		a_ang = DodCam_Blend(DODCAM_ANG_RATE, dt);
		for (i = 0; i < 3; i++) {
			s->org[i] += (org[i] - s->org[i]) * a_org;
			s->ang[i] += DodCam_AngleDelta(s->ang[i], ang[i]) * a_ang;
		}
	}
	s->primed = 1;
	s->mode = mode;
	s->target = target;
	for (i = 0; i < 3; i++) {
		out_org[i] = s->org[i];
		out_ang[i] = s->ang[i];
	}
}

#ifdef __cplusplus
}
#endif

#endif
