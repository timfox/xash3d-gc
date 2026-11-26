/*
gc_cam.h - camera paths (.gcam) for the Xash3D GameCube port (or any libogc program).

`gxbake camera` (and `[[camera]]` rules in a gxbake.toml) write .gcam: named clips, each
with a position track, a rotation track (unit quaternions) and a vertical-FOV track, plus
near/far planes. This file samples them and turns a pose into what the renderer wants:

  gc_cam_count / gc_cam_clip / gc_cam_find   validate the file and look a clip up
  gc_cam_sample      pose at a time: origin, rotation, fov_y, near, far
  gc_cam_angles      GoldSrc pitch/yaw/roll (refdef viewangles), and gc_cam_fov_x for fov_x
  gc_cam_axes        forward, left, up (what AngleVectors gives, with right = -left)
  gc_cam_view_mtx    a GX view matrix (Mtx, 3x4): world -> camera, camera looking down -Z
  gc_cam_proj_mtx    a GX perspective matrix (Mtx44), the same numbers as guPerspective
  gc_cam_load        (GC_CAM_GX) GX_LoadProjectionMtx with that matrix
  gc_cam_play / gc_cam_update / gc_cam_stop
                     a scripted camera: start a clip by name at the game's clock, then
                     each frame get origin, GoldSrc angles and fov_x to overwrite the
                     refdef with, until the clip ends (never, if it loops)

Coordinates are the engine's: Z up, GoldSrc units; the camera looks down +X with +Y to its
left and +Z up. The file is read in place: plain C89, byte-wise big-endian reads, bounds
checked once in gc_cam_clip, no allocation. GxBake's tests compile it on the host
(-Werror) and check that it samples exactly what GxBake's own reader does.

Curves: each track is linear or smooth (Hermite with finite-difference tangents, the same
as Catmull-Rom on evenly spaced keys). Rotations interpolate as quaternion components and
are normalised afterwards. Two keys with the same time are a cut: the pose jumps there and
no tangent reaches across it.
*/
#ifndef GC_CAM_H
#define GC_CAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_CAM_NAME 32

enum { GC_CAM_POS = 0, GC_CAM_ROT = 1, GC_CAM_FOV = 2, GC_CAM_TRACKS = 3 };

typedef struct
{
	const uint8_t *times;	/* u16 ticks (fixed) or f32 seconds, big-endian */
	const uint8_t *values;	/* s16 (fixed, value / 2^frac) or f32, big-endian, components per key */
	uint16_t keys;		/* 0 = track absent */
	uint8_t components;
	uint8_t frac;
	uint8_t smooth;		/* 1 Hermite, 0 linear */
} gc_cam_track_t;

typedef struct
{
	char name[GC_CAM_NAME];	/* NUL-terminated */
	float duration;		/* seconds */
	float near_z, far_z;
	float aspect;		/* 0 = use the screen's */
	float tick_rate;	/* Hz: fixed-point times are ticks of 1/tick_rate */
	int loop;
	int fixed;
	gc_cam_track_t track[GC_CAM_TRACKS];
} gc_cam_clip_t;

typedef struct
{
	float origin[3];
	float quat[4];		/* x, y, z, w: camera axes (forward, left, up) -> world */
	float fov_y;		/* degrees */
	float near_z, far_z;
} gc_cam_pose_t;

/* Number of clips, or -1 if this is not a .gcam (or is truncated). */
int gc_cam_count( const uint8_t *blob, size_t len );
/* Clip i, with every offset checked against len. 0 on success. */
int gc_cam_clip( const uint8_t *blob, size_t len, int i, gc_cam_clip_t *clip );
/* Index of the clip with this name (case-insensitive), or -1. */
int gc_cam_find( const uint8_t *blob, size_t len, const char *name );

/* Pose at time t seconds from the clip's start. Times before 0 give the first key; past
 * the end give the last, or wrap when the clip loops. A missing track leaves the matching
 * pose fields at origin 0, identity rotation, 90 degrees horizontal at 4:3. */
void gc_cam_sample( const gc_cam_clip_t *clip, float t, gc_cam_pose_t *pose );

void gc_cam_axes( const gc_cam_pose_t *pose, float forward[3], float left[3], float up[3] );
/* GoldSrc angles in degrees: pitch (positive looks down), yaw, roll. */
void gc_cam_angles( const gc_cam_pose_t *pose, float angles[3] );
/* Horizontal FOV for a vertical FOV at this aspect (width / height); 4:3 gives HL's fov_x. */
float gc_cam_fov_x( float fov_y, float aspect );
/* World -> camera, GX convention (camera looks down -Z, +Y up, +X right): rows are
 * -left, up, -forward with the translation in column 3. Same layout as libogc's Mtx. */
void gc_cam_view_mtx( const gc_cam_pose_t *pose, float m[3][4] );
/* Perspective projection as guPerspective( fov_y, aspect, near, far ) builds it. */
void gc_cam_proj_mtx( const gc_cam_pose_t *pose, float aspect, float m[4][4] );

/* A scripted camera. The player keeps a pointer to the blob, which must stay loaded. */
typedef struct
{
	const uint8_t *blob;
	size_t len;
	gc_cam_clip_t clip;
	double start;		/* the clock when gc_cam_play was called, seconds */
	int active;
} gc_cam_player_t;

typedef struct
{
	gc_cam_pose_t pose;
	float t;		/* seconds into the clip */
	float vieworg[3];
	float viewangles[3];	/* gc_cam_angles */
	float fov_x;		/* gc_cam_fov_x( fov_y, the clip's aspect or the caller's ) */
} gc_cam_view_t;

/* Start the named clip (case-insensitive) at clock time now (e.g. cl.time). 0 on success;
 * on failure the player is left stopped. */
int gc_cam_play( gc_cam_player_t *pl, const uint8_t *blob, size_t len, const char *name, double now );
/* The view at clock time now. Returns 1 and fills view while the clip runs: from its start
 * to its duration inclusive, or forever when it loops. Returns 0 (and stops) once a
 * one-shot clip has ended, or if the player is stopped; the caller then keeps its own view.
 * aspect is the screen's width / height, used for fov_x when the clip stores none; pass
 * 4.0f / 3.0f for HL's fov_x convention. */
int gc_cam_update( gc_cam_player_t *pl, double now, float aspect, gc_cam_view_t *view );
void gc_cam_stop( gc_cam_player_t *pl );

#ifdef GC_CAM_GX
/* Load the projection into GX (GX_PERSPECTIVE) and return the view matrix for the
 * caller's modelview set-up (guMtxConcat it with each model matrix). */
void gc_cam_load( const gc_cam_pose_t *pose, float aspect, float view[3][4] );
#endif

#ifdef __cplusplus
}
#endif

#endif /* GC_CAM_H */
