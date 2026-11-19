/*
cine_path.h - keyframed camera paths for cinematic capture

A camera path is a list of keys, each a time in seconds, an eye position,
view angles (pitch yaw roll, degrees) and a horizontal field of view. The
camera between two keys follows the mode of the earlier key:

	smooth	cubic Hermite through the neighbouring keys (Catmull-Rom with
		the real key spacing), so a dolly bends through its keys
		without stopping. The first and last key of a run of smooth
		keys have zero tangents: the camera eases in and out.
	linear	straight line, constant speed
	hold	stays on this key until the next one, which is a cut

Angles are unwrapped key to key before interpolating, so a yaw going from
350 to 10 turns 20 degrees, not 340.

Text form (.cam), one statement per line, '#' starts a comment:

	cam 1
	key <t> <x> <y> <z> <pitch> <yaw> <roll> <fov> [smooth|linear|hold]

Pure C with no engine headers, so the host tests can build it alone.
*/
#ifndef CINE_PATH_H
#define CINE_PATH_H

#include <stddef.h>

#define CINE_MAX_KEYS	64

typedef enum
{
	CINE_SMOOTH = 0,
	CINE_LINEAR,
	CINE_HOLD
} cine_mode_t;

typedef struct
{
	float	t;
	float	o[3];
	float	a[3];	// pitch yaw roll, unwrapped against the previous key at parse time
	float	fov;
	int	mode;
} cine_key_t;

typedef struct
{
	cine_key_t	keys[CINE_MAX_KEYS];
	int		count;
} cine_path_t;

// Parses a .cam text. Returns 1 on success; on failure returns 0 and writes a
// one-line reason (with the line number) into err.
int Cine_ParsePath( cine_path_t *path, const char *text, char *err, size_t errlen );

// Time of the last key, the length of the shot.
float Cine_Duration( const cine_path_t *path );

// Camera at time t (clamped to the path). Returns 0 for an empty path.
int Cine_Sample( const cine_path_t *path, float t, float o[3], float a[3], float *fov );

// One "key ..." line for the given camera, the format Cine_ParsePath reads.
int Cine_FormatKey( char *buf, size_t len, float t, const float o[3], const float a[3], float fov, int mode );

// The frame stamp: a frame number drawn as CINE_STAMP_BITS black or white
// cells, so the assembler can drop frames an emulator dumped twice. Bit i of
// the returned word is cell i; cells 0 and CINE_STAMP_BITS-1 are always
// white and black so a reader can tell a stamp from picture.
#define CINE_STAMP_BITS	20
unsigned int Cine_StampWord( unsigned int frame );
// Inverse of Cine_StampWord. Returns -1 when the guard cells or parity are wrong.
int Cine_StampFrame( unsigned int word );

#endif // CINE_PATH_H
