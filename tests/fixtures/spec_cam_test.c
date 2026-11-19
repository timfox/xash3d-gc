#include "spec_cam.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static int near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void test_passthrough_outside_chase(void)
{
	DodCamState s;
	const float org[3] = { 1, 2, 3 }, ang[3] = { 10, 20, 0 };
	float o[3], a[3];
	memset(&s, 0, sizeof(s));
	DodCam_Smooth(&s, 4 /* in-eye */, 5, org, ang, 0.016f, o, a);
	assert(o[0] == 1 && o[2] == 3 && a[1] == 20 && !s.primed);
	DodCam_Smooth(&s, 3 /* roaming */, 5, org, ang, 0.016f, o, a);
	assert(!s.primed);
	DodCam_Smooth(NULL, 2, 5, org, ang, 0.016f, o, a);	/* no state: pass through */
	assert(o[1] == 2);
}

static void test_pull_in(void)
{
	const float start[3] = { 0, 0, 0 }, end[3] = { 100, 0, 0 };
	float o[3];
	DodCam_PullIn(start, end, 1.0f, 8.0f, o);	/* free path: untouched */
	assert(near(o[0], 100.0f, 0.001f));
	DodCam_PullIn(start, end, 0.5f, 8.0f, o);	/* wall at 50: stop 8 short */
	assert(near(o[0], 42.0f, 0.001f) && o[1] == 0 && o[2] == 0);
	DodCam_PullIn(start, end, 0.05f, 8.0f, o);	/* wall right in front: stay at the start */
	assert(near(o[0], 0.0f, 0.001f));
	DodCam_PullIn(start, start, 0.0f, 8.0f, o);	/* zero-length ray */
	assert(o[0] == 0.0f);
}

static void test_look_at(void)
{
	const float from[3] = { 0, 0, 100 }, below[3] = { 100, 0, 0 }, left[3] = { 0, 50, 100 };
	float a[3];
	DodCam_LookAt(from, below, a);
	assert(near(a[1], 0.0f, 0.01f) && near(a[0], 45.0f, 0.01f) && a[2] == 0.0f);	/* down is +pitch */
	DodCam_LookAt(from, left, a);
	assert(near(a[1], 90.0f, 0.01f) && near(a[0], 0.0f, 0.01f));
}

static void test_shots_frame_the_target(void)
{
	const float feet[3] = { 1000, -500, 64 };
	int shot, i;
	for (shot = 0; shot < DODCAM_SHOTS; shot++) {
		for (i = 0; i <= 10; i++) {
			float o[3], a[3], dx, dy, d, yawto;
			DodCam_ShotPose(shot, i / 10.0f, feet, 30.0f, shot, o, a);
			dx = feet[0] - o[0];
			dy = feet[1] - o[1];
			d = sqrtf(dx * dx + dy * dy);
			assert(d > 60.0f && d < 260.0f);		/* never inside or far from the soldier */
			assert(o[2] >= feet[2] && o[2] < feet[2] + 260.0f);
			yawto = atan2f(dy, dx) * (180.0f / 3.14159265f);
			assert(fabsf(DodCam_AngleDelta(a[1], yawto)) < 0.1f);	/* aimed at the target */
			assert(a[0] > -60.0f && a[0] < 80.0f && a[2] == 0.0f);
		}
	}
}

static void test_shots_move_smoothly(void)
{
	const float feet[3] = { 0, 0, 0 };
	int shot, i;
	for (shot = 0; shot < DODCAM_SHOTS; shot++) {
		float prev[3], o[3], a[3];
		DodCam_ShotPose(shot, 0.0f, feet, 0.0f, 0, prev, a);
		for (i = 1; i <= 360; i++) {	/* 6 s at 60 fps */
			float dx, dy, dz;
			DodCam_ShotPose(shot, i / 360.0f, feet, 0.0f, 0, o, a);
			dx = o[0] - prev[0]; dy = o[1] - prev[1]; dz = o[2] - prev[2];
			assert(sqrtf(dx * dx + dy * dy + dz * dz) < 3.0f);	/* no jump inside a shot */
			memcpy(prev, o, sizeof(prev));
		}
	}
}

static void test_cine_cuts(void)
{
	DodCamCine c;
	float t;
	memset(&c, 0, sizeof(c));
	assert(DodCam_CineStep(&c, 3, 10.0f, &t) && t == 0.0f && c.shot == 0);
	assert(!DodCam_CineStep(&c, 3, 13.0f, &t) && near(t, 0.5f, 0.001f));
	assert(DodCam_CineStep(&c, 3, 16.0f, &t) && c.shot == 1);	/* shot time over: next shot */
	assert(DodCam_CineStep(&c, 4, 16.5f, &t) && c.shot == 2 && t == 0.0f);	/* new target: cut */
	assert(DodCam_CineStep(&c, 4, 1.0f, &t));	/* clock went back (new map): restart */
	for (int i = 0; i < 10; i++)
		DodCam_CineStep(&c, 4, 100.0f + i * 7.0f, &t);
	assert(c.shot >= 0 && c.shot < DODCAM_SHOTS);
}

static void test_first_frame_snaps_then_follows(void)
{
	DodCamState s;
	float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, o[3], a[3];
	int i;
	memset(&s, 0, sizeof(s));
	DodCam_Smooth(&s, DODCAM_CHASE_FREE, 7, org, ang, 0.016f, o, a);
	assert(o[0] == 0 && s.primed);
	org[0] = 100.0f;	/* target steps 100 units in one network update */
	DodCam_Smooth(&s, DODCAM_CHASE_FREE, 7, org, ang, 0.016f, o, a);
	assert(o[0] > 5.0f && o[0] < 25.0f);	/* partial, not a step */
	for (i = 0; i < 90; i++)		/* 1.5 s later it has caught up */
		DodCam_Smooth(&s, DODCAM_CHASE_FREE, 7, org, ang, 0.016f, o, a);
	assert(near(o[0], 100.0f, 0.5f));
}

static void test_frame_rate_independent(void)
{
	DodCamState s30, s60;
	float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, o30[3], o60[3], a[3];
	int i;
	memset(&s30, 0, sizeof(s30));
	memset(&s60, 0, sizeof(s60));
	DodCam_Smooth(&s30, 2, 1, org, ang, 0.0333f, o30, a);
	DodCam_Smooth(&s60, 2, 1, org, ang, 0.0167f, o60, a);
	org[0] = 100.0f;
	DodCam_Smooth(&s30, 2, 1, org, ang, 0.0333f, o30, a);
	for (i = 0; i < 2; i++)
		DodCam_Smooth(&s60, 2, 1, org, ang, 0.0167f, o60, a);
	assert(near(o30[0], o60[0], 0.5f));
}

static void test_snaps(void)
{
	DodCamState s;
	float org[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, o[3], a[3];
	memset(&s, 0, sizeof(s));
	DodCam_Smooth(&s, 2, 1, org, ang, 0.016f, o, a);
	org[0] = 50.0f;
	DodCam_Smooth(&s, 2, 9, org, ang, 0.016f, o, a);	/* new target */
	assert(o[0] == 50.0f);
	org[0] = 60.0f;
	DodCam_Smooth(&s, 1, 9, org, ang, 0.016f, o, a);	/* mode change */
	assert(o[0] == 60.0f);
	org[0] = 700.0f;
	DodCam_Smooth(&s, 1, 9, org, ang, 0.016f, o, a);	/* teleport / respawn */
	assert(o[0] == 700.0f);
	org[0] = 710.0f;
	DodCam_Smooth(&s, 1, 9, org, ang, 1.0f, o, a);	/* long hitch */
	assert(o[0] == 710.0f);
	org[0] = 720.0f;
	DodCam_Smooth(&s, 1, 9, org, ang, 0.0f, o, a);	/* paused frame: hold */
	assert(o[0] == 710.0f);
}

static void test_angle_wrap(void)
{
	DodCamState s;
	float org[3] = { 0, 0, 0 }, ang[3] = { 0, 170, 0 }, o[3], a[3];
	int i;
	memset(&s, 0, sizeof(s));
	DodCam_Smooth(&s, 2, 1, org, ang, 0.016f, o, a);
	ang[1] = -170.0f;	/* 20 degrees the short way, across the seam */
	DodCam_Smooth(&s, 2, 1, org, ang, 0.016f, o, a);
	assert(a[1] > 170.0f && a[1] < 180.0f + 5.0f);	/* moved forward through 180, not back through 0 */
	for (i = 0; i < 60; i++)
		DodCam_Smooth(&s, 2, 1, org, ang, 0.016f, o, a);
	assert(near(DodCam_AngleDelta(a[1], -170.0f), 0.0f, 0.5f));
	assert(near(DodCam_AngleDelta(350.0f, 10.0f), 20.0f, 0.001f));
}

static void test_level(void)
{
	float a[3] = { 120.0f, 30.0f, 25.0f };
	DodCam_Level(DODCAM_CHASE_FREE, a);
	assert(a[0] == 89.0f && a[1] == 30.0f && a[2] == 0.0f);
	a[0] = -100.0f; a[2] = -40.0f;
	DodCam_Level(DODCAM_ROAMING, a);
	assert(a[0] == -89.0f && a[2] == 0.0f);
	a[0] = 10.0f; a[2] = 12.0f;
	DodCam_Level(DODCAM_CHASE_LOCKED, a);
	assert(a[0] == 10.0f && a[2] == 0.0f);
	a[2] = 12.0f;
	DodCam_Level(4 /* in-eye keeps the player's view */, a);
	assert(a[2] == 12.0f);
}

int main(void)
{
	test_passthrough_outside_chase();
	test_pull_in();
	test_look_at();
	test_shots_frame_the_target();
	test_shots_move_smoothly();
	test_cine_cuts();
	test_first_frame_snaps_then_follows();
	test_frame_rate_independent();
	test_snaps();
	test_angle_wrap();
	test_level();
	return 0;
}
