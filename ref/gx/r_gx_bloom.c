/*
r_gx_bloom.c - Flipper post bloom (Twilight Princess style)

Cheap, hardware-native glow for the Lost Coast CapFaces path:

  1. GX_CopyTex the finished EFB through the copy unit's 2x box filter into
     a half-res I8 (luma) texture. 320x240 I8 = 75 KiB, borrowed from the idle soft Z
     scratch: no heap, no new BSS.
  2. Draw one fullscreen quad. Each tap is thresholded, then the survivors
     are averaged and tinted. Taps stay within a few texels so the glow
     sits on the surface. A tap 14 texels away stamped a second copy of
     the boats and the towers.
  3. Additive blend (ONE, ONE) back onto the EFB, Z off.

No scratch EFB regions, no extra copies, one texture: well inside Flipper's
budget (one EFB copy + one fullscreen fill).

Enabled for -gccapfacessmoke (Lost Coast) or -gcbloom; -gcnobloom disables.
*/

#include "r_local.h"

#if XASH_GAMECUBE

#include <gccore.h>
#include <ogc/gx.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define GC_BLOOM_MAX_BYTES ( 96u * 1024u )
#define GC_BLOOM_TAPS 6

/* The copy lands in the idle soft Z scratch (r_gcmap.c) — no new BSS. */
extern void *R_GcmapBorrowSoftZ( unsigned offset, unsigned bytes );
static GXTexObj gc_bloom_tex;

/* Floor sits under the sky and pale hulls, above sand and water.
 * A floor of 112 let the sunlit ground through and the add clipped it white.
 * Scale stays 1 so the halo does not reprint the mesh. */
#define GC_BLOOM_THRESHOLD 172 /* 0..255 luma floor. 188 suited the old clipped lighting; sunlit plaster now sits near 190 */
#define GC_BLOOM_TINT_R 255
#define GC_BLOOM_TINT_G 236
#define GC_BLOOM_TINT_B 196
#define GC_BLOOM_TINT_A 120    /* lift tower/sky glow; taps stay tight */

static int gc_bloom_enabled = -1;

qboolean R_GXBloomEnabled( void )
{
	if( gc_bloom_enabled < 0 )
	{
		gc_bloom_enabled = 0;
		if( !gEngfuncs.Sys_CheckParm( "-gcnobloom" )
			&& ( gEngfuncs.Sys_CheckParm( "-gccapfacessmoke" )
				|| gEngfuncs.Sys_CheckParm( "-gcbloom" )
				|| gEngfuncs.Sys_CheckParm( "-gcworldrender" )
				|| gEngfuncs.Sys_CheckParm( "-gcvst" )))
			gc_bloom_enabled = 1;
	}
	return gc_bloom_enabled ? true : false;
}

/*
 * Auto-exposure, as Source's HDR path does it, with the limits the map sets
 * (media/tm_d2_lostcoast.bin, lostcoast-gc tools/bake_lostcoast_tonemap.py).
 *
 * The lighting is baked at one exposure. Source moves a tonemap scale every
 * frame: it finds the luminance that only 2 % of the frame exceeds and aims
 * to put it at 60 % of white, never letting the median fall under 3 %
 * (mat_tonemap_percent_bright_pixels / _percent_target / _min_avglum), then
 * clamps the scale to [min, max]. The map's env_tonemap_controller takes
 * those limits from trigger volumes: 0.5 to 2 at the start, up to 10 inside
 * the monastery. The camera stands in for the player.
 *
 * The frame is measured on last frame's half-res luma copy (the bloom's), and
 * the scale is applied as one fullscreen multiply before the bloom:
 * display = gamma( light x scale ), so the gain over the baked exposure is
 * ( scale / baked )^( 1 / 2.2 ).
 *
 * The sky (depth at the far plane) follows the scale too when the bake says
 * its textures are light: the map's HDR sky x sky_baked (the last header word;
 * 0 = screen art, leave it alone). Source draws the sky as HDR x tonemap
 * scale, which is why it blows out white behind dark cliffs and goes deep
 * blue over a sunlit yard.
 *
 * The sky and whatever is blown out count in the top bucket: Source's frame
 * is clamped at white before it is measured, so a bright sky holds the scale
 * down. That is retail's look in the cove (a white sky over dark ground).
 */
#define GC_EXPO_MAX_VOLUMES 16

typedef struct
{
	float	mins[3], maxs[3];
	float	enter_max, enter_min, leave_max, leave_min;
	u32	flags;
} gc_expo_vol_t;

static struct
{
	s8	state;		/* 0 untried, 1 ready, -1 none */
	u8	n, inside[GC_EXPO_MAX_VOLUMES], fired[GC_EXPO_MAX_VOLUMES];
	float	baked, lim_min, lim_max, scale, last_time, last_org[3];
	float	sky_baked;	/* tonemap scale the sky textures were baked at, 0 = none */
	int	frames;
	gc_expo_vol_t	vol[GC_EXPO_MAX_VOLUMES];
} gc_expo;

static void R_GXExposureLoad( void )
{
	file_t *f;
	u32 hdr[6];

	gc_expo.state = -1;
	f = gEngfuncs.fsapi->Open( "media/tm_d2_lostcoast.bin", "rb", false );
	if( !f )
		return;
	if( gEngfuncs.fsapi->Read( f, hdr, 24 ) == 24 && hdr[0] == 0x4743746du && ( hdr[1] >> 16 ) == 1
		&& ( hdr[1] & 0xffffu ) <= GC_EXPO_MAX_VOLUMES )
	{
		gc_expo.n = (u8)( hdr[1] & 0xffffu );
		memcpy( &gc_expo.baked, &hdr[2], 12 );
		memcpy( &gc_expo.sky_baked, &hdr[5], 4 );
		if( !( gc_expo.sky_baked > 0.0f && gc_expo.sky_baked < 100.0f ))
			gc_expo.sky_baked = 0.0f;
		if( gc_expo.baked > 0.0f && gc_expo.lim_max >= gc_expo.lim_min && gc_expo.lim_min > 0.0f
			&& gEngfuncs.fsapi->Read( f, gc_expo.vol, gc_expo.n * 44 ) == (fs_offset_t)( gc_expo.n * 44 ))
		{
			/* Start where the baked look was, inside the limits. */
			gc_expo.scale = gc_expo.baked > gc_expo.lim_max ? gc_expo.lim_max : gc_expo.baked;
			gc_expo.state = 1;
		}
	}
	gEngfuncs.fsapi->Close( f );
	gEngfuncs.Con_Reportf( "Xash3D GameCube: map exposure volumes=%d limits %.2f..%.2f baked %.2f\n",
		gc_expo.state > 0 ? gc_expo.n : -1, gc_expo.lim_min, gc_expo.lim_max, gc_expo.baked );
}

/* Did the camera's step from a to b pass through the box (the door triggers are 8 units thin)? */
static qboolean R_GXExposureSegBox( const float *a, const float *b, const gc_expo_vol_t *v )
{
	float t0 = 0.0f, t1 = 1.0f;
	int i;

	for( i = 0; i < 3; i++ )
	{
		const float d = b[i] - a[i];

		if( d > -0.001f && d < 0.001f )
		{
			if( a[i] < v->mins[i] || a[i] > v->maxs[i] )
				return false;
		}
		else
		{
			float ta = ( v->mins[i] - a[i] ) / d, tb = ( v->maxs[i] - a[i] ) / d;

			if( ta > tb ) { const float sw = ta; ta = tb; tb = sw; }
			if( ta > t0 ) t0 = ta;
			if( tb < t1 ) t1 = tb;
			if( t0 > t1 )
				return false;
		}
	}
	return true;
}

/* Returns the display gain for this frame. buf is last frame's luma copy. */
/* Which histogram samples are sky. The retail still of the cove has the sky
 * at tonemap scale 1.55 with half the frame brighter than 60 % of white, which
 * the 2 % rule would never allow if the sky had a vote: the world sets the
 * exposure and the sky follows it. Sky = depth still at the far plane. A
 * second half-res copy takes the low byte of Z (the other half of the idle
 * soft Z buffer the luma copy borrows): 0xff there for the far plane, any
 * value for geometry. */
static const u8 *gc_expo_zbuf;
/* The copied region in half-res texels: origin and size (8 x 4 tiles). */
#define GC_EXPO_ZX 16
#define GC_EXPO_ZY 18
#define GC_EXPO_ZW 288
#define GC_EXPO_ZH 204
static int gc_expo_dbg[4];	/* samples counted, bright bin, median bin, sky samples: for the log */

static float R_GXExposureUpdate( const u8 *buf, int bw, int bh )
{
	const float *org = RI.rvp.vieworigin;
	const float now = gp_cl ? (float)gp_cl->time : 0.0f;
	float dt, goal;
	int i;

	if( !gc_expo.state )
		R_GXExposureLoad();
	if( gc_expo.state <= 0 )
		return 1.0f;
	if( gc_expo.frames == 0 )
		VectorCopy( org, gc_expo.last_org );
	/* The map's triggers. */
	for( i = 0; i < gc_expo.n; i++ )
	{
		const gc_expo_vol_t *v = &gc_expo.vol[i];
		const qboolean in = R_GXExposureSegBox( gc_expo.last_org, org, v );

		if( in && !gc_expo.inside[i] && !(( v->flags & 1u ) && gc_expo.fired[i] ))
		{
			if( v->enter_max >= 0.0f ) gc_expo.lim_max = v->enter_max;
			if( v->enter_min >= 0.0f ) gc_expo.lim_min = v->enter_min;
			gc_expo.fired[i] = 1;
		}
		else if( !in && gc_expo.inside[i] )
		{
			if( v->leave_max >= 0.0f ) gc_expo.lim_max = v->leave_max;
			if( v->leave_min >= 0.0f ) gc_expo.lim_min = v->leave_min;
		}
		gc_expo.inside[i] = in ? 1 : 0;
	}
	VectorCopy( org, gc_expo.last_org );
	dt = now - gc_expo.last_time;
	gc_expo.last_time = now;
	if( dt < 0.0f || dt > 0.1f )
		dt = 0.1f;
	goal = gc_expo.scale;
	if( buf && gc_expo.frames >= 4 )
	{
		/* Histogram of a 20 x 12 grid over the centre 90 % x 85 % of the frame
		 * (mat_exposure_center_region). I8 texels sit in 8 x 4 tiles. */
		u16 hist[32];
		int gx, gy, n = 0, acc, hi = 31, med = 0, nsky = 0;
		float lum_hi, lum_med, t1, t2;

		memset( hist, 0, sizeof( hist ));
		for( gy = 0; gy < 12; gy++ )
		{
			const int y = (int)(( 0.075f + 0.85f * ( (float)gy + 0.5f ) / 12.0f ) * (float)bh );

			for( gx = 0; gx < 20; gx++ )
			{
				const int x = (int)(( 0.05f + 0.9f * ( (float)gx + 0.5f ) / 20.0f ) * (float)bw );

				const u8 l = buf[(( y >> 2 ) * ( bw >> 3 ) + ( x >> 3 )) * 32 + ( y & 3 ) * 8 + ( x & 7 )];

				if( gc_expo.sky_baked > 0.0f )
				{
					const int zx = x - GC_EXPO_ZX, zy = y - GC_EXPO_ZY;

					if( gc_expo_zbuf && bw == 320 && zx >= 0 && zy >= 0 && zx < GC_EXPO_ZW && zy < GC_EXPO_ZH
						&& gc_expo_zbuf[(( zy >> 2 ) * ( GC_EXPO_ZW >> 3 ) + ( zx >> 3 )) * 32 + ( zy & 3 ) * 8 + ( zx & 7 )] == 0xff )
					{
						/* The sky counts, like any other pixel. Left out
						 * (as blown-out samples were too), the ground alone
						 * set the exposure and the cove sat at the map's
						 * maximum of 3: pale sand, a rusty hull at 82 of 255
						 * where retail has 33, a grey sky. Retail's frame
						 * there is a white sky over dark ground: the bright
						 * sky holds the scale down. */
						nsky++;	/* counted below, at what it is drawn at */
					}
				}
				hist[l >> 3]++;
				n++;
			}
		}
		/* The level 2 % of the samples exceed, and the median. */
		for( acc = 0, hi = 31; hi > 0; hi-- )
		{
			acc += hist[hi];
			if( acc * 50 >= n )
				break;
		}
		for( acc = 0, med = 0; med < 31; med++ )
		{
			acc += hist[med];
			if( acc * 2 >= n )
				break;
		}
		/* A copy that reads back all black is no measurement: Dolphin only
		 * writes EFB copies to memory with "Store EFB Copies to Texture Only"
		 * off, and the Null backend never does. Hold the scale then. */
		gc_expo_dbg[0] = n; gc_expo_dbg[1] = hi; gc_expo_dbg[2] = med; gc_expo_dbg[3] = nsky;
		if( hist[0] == n || n < 24 )
			hi = -1;	/* also when nearly every sample is blown out */
		/* Bin centres back to linear light, as shares of white. */
		if( hi >= 0 )
		{
			lum_hi = powf(( (float)hi + 0.5f ) * ( 1.0f / 32.0f ), 2.2f );
			lum_med = powf(( (float)med + 0.5f ) * ( 1.0f / 32.0f ), 2.2f );
			t1 = 0.60f / ( lum_hi > 0.0001f ? lum_hi : 0.0001f );
			t2 = 0.03f / ( lum_med > 0.0001f ? lum_med : 0.0001f );
			goal = gc_expo.scale * ( t2 > t1 ? t2 : t1 );
		}
	}
	if( goal > gc_expo.lim_max ) goal = gc_expo.lim_max;
	if( goal < gc_expo.lim_min ) goal = gc_expo.lim_min;
	/* Toward the goal at mat_hdr_manual_tonemap_rate (1 per second), three
	 * times as fast on the way down (mat_accelerate_adjust_exposure_down). */
	{
		float k = dt * ( goal < gc_expo.scale ? 3.0f : 1.0f );

		if( k > 1.0f ) k = 1.0f;
		gc_expo.scale += ( goal - gc_expo.scale ) * k;
	}
	gc_expo.frames++;
	if(( gc_expo.frames & 255 ) == 1 )
		gEngfuncs.Con_Reportf( "Xash3D GameCube: exposure scale=%.2f goal=%.2f limits %.2f..%.2f counted=%d sky=%d hi=%d med=%d\n",
			gc_expo.scale, goal, gc_expo.lim_min, gc_expo.lim_max, gc_expo_dbg[0], gc_expo_dbg[3], gc_expo_dbg[1], gc_expo_dbg[2] );
	return powf( gc_expo.scale / gc_expo.baked, 1.0f / 2.2f );
}

/* For the sky box drawn into the water mirror: the mirror is part of the
 * world, which takes the world's gain afterwards, so its sky has to be in the
 * world's baked units. Returns the factor over the sky textures, 0 = none. */
float R_GXExposureMirrorSkyGain( void )
{
	if( gc_expo.state <= 0 || gc_expo.sky_baked <= 0.0f || gc_expo.baked <= 0.0f )
		return 0.0f;
	return powf( gc_expo.baked / gc_expo.sky_baked, 1.0f / 2.2f );
}

/* One fullscreen multiply by the gain: over everything nearer than the far
 * plane, or (sky) over what is left at the far plane. */
static void R_GXExposureApply( GXRModeObj *rmode, float gain, qboolean sky )
{
	const f32 w = (f32)rmode->fbWidth, h = (f32)rmode->efbHeight;
	Mtx44 ortho;
	Mtx ident;
	GXColor c;
	int v;

	if( gain > 0.995f && gain < 1.005f )
		return;
	if( gain > 2.0f ) gain = 2.0f;
	if( gain < 0.0f ) gain = 0.0f;
	v = (int)(( gain > 1.0f ? gain - 1.0f : gain ) * 255.0f + 0.5f );
	c.r = c.g = c.b = (u8)( v > 255 ? 255 : v );
	c.a = 255;
	GX_SetViewport( 0.0f, 0.0f, w, h, 0.0f, 1.0f );
	GX_SetScissor( 0, 0, rmode->fbWidth, rmode->efbHeight );
	guOrtho( ortho, 0.0f, h, 0.0f, w, 0.0f, 1.0f );
	GX_LoadProjectionMtx( ortho, GX_ORTHOGRAPHIC );
	guMtxIdentity( ident );
	GX_LoadPosMtxImm( ident, GX_PNMTX0 );
	GX_SetCurrentMtx( GX_PNMTX0 );
	/* Only where the scene wrote a depth nearer than the far plane. */
	GX_SetZMode( GX_TRUE, sky ? GX_LEQUAL : GX_GREATER, GX_FALSE );
	GX_SetZCompLoc( GX_TRUE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	/* Brighter: dst + dst x ( gain - 1 ). Darker: dst x gain. */
	if( gain > 1.0f )
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_ONE, GX_LO_NOOP );
	else
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ZERO, GX_BL_SRCCLR, GX_LO_NOOP );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_SetChanMatColor( GX_COLOR0A0, c );
	GX_SetNumTexGens( 0 );
	GX_SetNumIndStages( 0 );
	GX_SetNumTevStages( 1 );
	GX_SetTevDirect( GX_TEVSTAGE0 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	GX_Begin( GX_QUADS, GX_VTXFMT7, 4 );
	GX_Position3f32( 0.0f, 0.0f, -0.9999990f );
	GX_Position3f32( w, 0.0f, -0.9999990f );
	GX_Position3f32( w, h, -0.9999990f );
	GX_Position3f32( 0.0f, h, -0.9999990f );
	GX_End();
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
}

/*
 * The wide glow. The bloom below is six taps inside three pixels: a rim on
 * what is bright. Source's bloom is a blurred copy of the bright parts of the
 * frame added back over it, wide enough that the sky around the sun washes
 * over the cliff and the towers in front of it.
 *
 * Before the scene is drawn, last frame's half-res luma (still in the bloom
 * buffer) is drawn 64 x 48 into the corner of the EFB through the threshold,
 * four taps a texel and a quarter apart, and copied out as I8 (3 KB). Each of
 * its texels is ten pixels of the frame; R_GXBloomApply adds it over the
 * finished frame with four bilinear taps a texel apart. One frame late, which
 * a glow does not show.
 */
#define GC_GLOW_W 64
#define GC_GLOW_H 48
#define GC_GLOW_THRESHOLD 196	/* of 255: the sky by the sun, sunlit plaster, glints */
#define GC_GLOW_STRENGTH 150	/* of 255, of the tint */
static u8 gc_glow_buf[GC_GLOW_W * GC_GLOW_H] __attribute__(( aligned( 32 )));
static GXTexObj gc_glow_tex;
static qboolean gc_glow_ready;
static u8 *gc_glow_src;		/* last frame's luma copy */
static u16 gc_glow_src_w, gc_glow_src_h;

void R_GXBloomGlowPrepass( void )
{
	static const f32 taps[4][2] = { { -1.25f, -1.25f }, { 1.25f, -1.25f }, { 1.25f, 1.25f }, { -1.25f, 1.25f } };
	static const f32 corner[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Mtx44 ortho;
	Mtx ident;
	GXTexObj src;
	GXColor thresh = { GC_GLOW_THRESHOLD, GC_GLOW_THRESHOLD, GC_GLOW_THRESHOLD, 0 };
	GXColor zero = { 0, 0, 0, 0 };
	f32 du, dv;
	int i, c;

	gc_glow_ready = false;
	if( !gc_glow_src || !R_GXBloomEnabled())
		return;
	/* One glow texel covers five source texels across: step the taps so
	 * the four of them cover it. */
	du = (f32)gc_glow_src_w / (f32)GC_GLOW_W * 0.25f / (f32)gc_glow_src_w;
	dv = (f32)gc_glow_src_h / (f32)GC_GLOW_H * 0.25f / (f32)gc_glow_src_h;
	GX_InitTexObj( &src, gc_glow_src, gc_glow_src_w, gc_glow_src_h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE );
	GX_InitTexObjLOD( &src, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1 );
	GX_InvalidateTexAll();
	GX_LoadTexObj( &src, GX_TEXMAP0 );
	GX_SetViewport( 0.0f, 0.0f, (f32)GC_GLOW_W, (f32)GC_GLOW_H, 0.0f, 1.0f );
	GX_SetScissor( 0, 0, GC_GLOW_W, GC_GLOW_H );
	guOrtho( ortho, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f );
	GX_LoadProjectionMtx( ortho, GX_ORTHOGRAPHIC );
	guMtxIdentity( ident );
	GX_LoadPosMtxImm( ident, GX_PNMTX0 );
	GX_SetCurrentMtx( GX_PNMTX0 );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetNumChans( 0 );
	GX_SetNumIndStages( 0 );
	GX_SetNumTexGens( 4 );
	GX_SetTevColor( GX_TEVREG0, thresh );
	GX_SetTevColor( GX_TEVREG2, zero );
	GX_SetNumTevStages( 9 );
	for( i = 0; i < 4; i++ )
	{
		const u8 sub = (u8)( GX_TEVSTAGE0 + i * 2 ), acc = (u8)( sub + 1 );

		GX_SetTexCoordGen( GX_TEXCOORD0 + i, GX_TG_MTX2x4, GX_TG_TEX0 + i, GX_IDENTITY );
		GX_SetTevDirect( sub );
		GX_SetTevOrder( sub, GX_TEXCOORD0 + i, GX_TEXMAP0, GX_COLORNULL );
		GX_SetTevColorIn( sub, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
		GX_SetTevColorOp( sub, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( sub, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( sub, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevDirect( acc );
		GX_SetTevOrder( acc, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
		GX_SetTevKColorSel( acc, GX_TEV_KCSEL_1_4 );
		GX_SetTevColorIn( acc, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_C2 );
		GX_SetTevColorOp( acc, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG2 );
		GX_SetTevAlphaIn( acc, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( acc, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	/* What is over the threshold spans 0 to 59 of 255: x4 to use the byte. */
	GX_SetTevDirect( GX_TEVSTAGE8 );
	GX_SetTevOrder( GX_TEVSTAGE8, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE8, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C2 );
	GX_SetTevColorOp( GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_4, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE8, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	for( i = 0; i < 4; i++ )
	{
		GX_SetVtxDesc( GX_VA_TEX0 + i, GX_DIRECT );
		GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_TEX0 + i, GX_TEX_ST, GX_F32, 0 );
	}
	GX_InvVtxCache();
	GX_Begin( GX_QUADS, GX_VTXFMT7, 4 );
	for( c = 0; c < 4; c++ )
	{
		GX_Position3f32( corner[c][0], corner[c][1], 0.0f );
		for( i = 0; i < 4; i++ )
			GX_TexCoord2f32( corner[c][0] + taps[i][0] * du, corner[c][1] + taps[i][1] * dv );
	}
	GX_End();
	GX_SetTexCopySrc( 0, 0, GC_GLOW_W, GC_GLOW_H );
	GX_SetTexCopyDst( GC_GLOW_W, GC_GLOW_H, GX_TF_I8, GX_FALSE );
	DCInvalidateRange( gc_glow_buf, sizeof( gc_glow_buf ));
	GX_CopyTex( gc_glow_buf, GX_FALSE );
	GX_PixModeSync();
	GX_InitTexObj( &gc_glow_tex, gc_glow_buf, GC_GLOW_W, GC_GLOW_H, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE );
	GX_InitTexObjLOD( &gc_glow_tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1 );
	gc_glow_ready = true;
	GX_SetNumTevStages( 1 );
	GX_SetNumTexGens( 1 );
	GX_SetNumChans( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	for( i = 0; i < 9; i++ )
		GX_SetTevKColorSel( (u8)( GX_TEVSTAGE0 + i ), GX_TEV_KCSEL_1 );
	GX_ClearVtxDesc();
	GX_InvalidateTexAll();
}

/* The glow over the finished frame: four bilinear taps, a texel apart. */
static void R_GXBloomGlowAdd( f32 fb_w, f32 fb_h )
{
	static const f32 taps[4][2] = { { -1.0f, -0.5f }, { 1.0f, 0.5f }, { 0.5f, -1.0f }, { -0.5f, 1.0f } };
	static const f32 corner[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	GXColor tint;
	int i, c;

	if( !gc_glow_ready )
		return;
	tint.r = (u8)(( GC_BLOOM_TINT_R * GC_GLOW_STRENGTH ) / 255 );
	tint.g = (u8)(( GC_BLOOM_TINT_G * GC_GLOW_STRENGTH ) / 255 );
	tint.b = (u8)(( GC_BLOOM_TINT_B * GC_GLOW_STRENGTH ) / 255 );
	tint.a = 255;
	GX_LoadTexObj( &gc_glow_tex, GX_TEXMAP1 );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP );
	GX_SetNumChans( 0 );
	GX_SetNumTexGens( 4 );
	GX_SetTevColor( GX_TEVREG1, tint );
	GX_SetNumTevStages( 5 );
	for( i = 0; i < 4; i++ )
	{
		GX_SetTexCoordGen( GX_TEXCOORD0 + i, GX_TG_MTX2x4, GX_TG_TEX0 + i, GX_IDENTITY );
		GX_SetTevDirect( GX_TEVSTAGE0 + i );
		GX_SetTevOrder( GX_TEVSTAGE0 + i, GX_TEXCOORD0 + i, GX_TEXMAP1, GX_COLORNULL );
		GX_SetTevKColorSel( GX_TEVSTAGE0 + i, GX_TEV_KCSEL_1_4 );
		GX_SetTevColorIn( GX_TEVSTAGE0 + i, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, i ? GX_CC_CPREV : GX_CC_ZERO );
		GX_SetTevColorOp( GX_TEVSTAGE0 + i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0 + i, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( GX_TEVSTAGE0 + i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	GX_SetTevDirect( GX_TEVSTAGE4 );
	GX_SetTevOrder( GX_TEVSTAGE4, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C1, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	for( i = 0; i < 4; i++ )
	{
		GX_SetVtxDesc( GX_VA_TEX0 + i, GX_DIRECT );
		GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_TEX0 + i, GX_TEX_ST, GX_F32, 0 );
	}
	GX_InvVtxCache();
	GX_Begin( GX_QUADS, GX_VTXFMT7, 4 );
	for( c = 0; c < 4; c++ )
	{
		GX_Position3f32( corner[c][0] * fb_w, corner[c][1] * fb_h, 0.0f );
		for( i = 0; i < 4; i++ )
			GX_TexCoord2f32( corner[c][0] + taps[i][0] / (f32)GC_GLOW_W, corner[c][1] + taps[i][1] / (f32)GC_GLOW_H );
	}
	GX_End();
}

/*
 * Apply bloom to the current EFB. Leaves GX in a 2D ortho state; the caller
 * must restore its 3D pipe (projection, VAT/desc, TEV, blend, Z).
 * Returns true if the pass ran.
 */
qboolean R_GXBloomApply( void *rmode_ptr )
{
	GXRModeObj *rmode = (GXRModeObj *)rmode_ptr;
	static qboolean logged;
	Mtx44 ortho;
	Mtx ident;
	u16 efb_w, efb_h, bw, bh;
	u8 *gc_bloom_buf;
	f32 fb_w, fb_h, du, dv;
	GXColor thresh, tint, zero;
	int i;
	/* Half-res texels. Keep the halo inside ~3 screen pixels so bright
	 * towers glow without a shifted second hull. */
	static const f32 taps[GC_BLOOM_TAPS][2] = {
		{  1.0f,  0.0f }, { -1.0f,  0.0f }, {  0.0f,  1.0f }, {  0.0f, -1.0f },
		{  1.5f,  1.5f }, { -1.5f, -1.5f },
	};

	if( !rmode || !R_GXBloomEnabled())
		return false;

	efb_w = rmode->fbWidth & ~15u;
	efb_h = rmode->efbHeight & ~7u;
	bw = efb_w / 2;
	bh = efb_h / 2;
	if( bw == 0 || bh == 0 || (u32)bw * bh > GC_BLOOM_MAX_BYTES )
		return false;
	gc_bloom_buf = (u8 *)R_GcmapBorrowSoftZ( 0, (u32)bw * bh );
	if( !gc_bloom_buf )
		return false;

	/* Exposure first: measured on last frame's copy still in the buffer,
	 * applied before this frame's copy so the bloom sees the result. */
	R_GXExposureApply( rmode, R_GXExposureUpdate( gc_bloom_buf, bw, bh ), false );
	if( gc_expo.state > 0 && gc_expo.sky_baked > 0.0f )
		R_GXExposureApply( rmode, powf( gc_expo.scale / gc_expo.sky_baked, 1.0f / 2.2f ), true );

	/* 1) EFB -> half-res luma through the copy box filter. */
	GX_SetTexCopySrc( 0, 0, efb_w, efb_h );
	GX_SetTexCopyDst( bw, bh, GX_TF_I8, GX_TRUE );
	DCInvalidateRange( gc_bloom_buf, (u32)bw * bh );
	if( gc_expo.state > 0 && gc_expo.sky_baked > 0.0f )
	{
		/* Low byte of Z for the same picture: next frame's sky mask. */
		/* Only the part of the frame the histogram samples (the centre
		 * 90 % x 85 %): the whole frame does not fit the borrowed half. */
		u8 *zb = ( efb_w == 640 && efb_h == 480 )
			? (u8 *)R_GcmapBorrowSoftZ( (u32)bw * bh, GC_EXPO_ZW * GC_EXPO_ZH ) : NULL;

		if( zb )
		{
			GX_SetTexCopySrc( GC_EXPO_ZX * 2, GC_EXPO_ZY * 2, GC_EXPO_ZW * 2, GC_EXPO_ZH * 2 );
			GX_SetTexCopyDst( GC_EXPO_ZW, GC_EXPO_ZH, GX_CTF_Z8L, GX_TRUE );
			DCInvalidateRange( zb, GC_EXPO_ZW * GC_EXPO_ZH );
			GX_CopyTex( zb, GX_FALSE );
			GX_SetTexCopySrc( 0, 0, efb_w, efb_h );
			GX_SetTexCopyDst( bw, bh, GX_TF_I8, GX_TRUE );
		}
		gc_expo_zbuf = zb;
	}
	GX_CopyTex( gc_bloom_buf, GX_FALSE );
	GX_PixModeSync();
	GX_InitTexObj( &gc_bloom_tex, gc_bloom_buf, bw, bh, GX_TF_I8,
		GX_CLAMP, GX_CLAMP, GX_FALSE );
	GX_InitTexObjLOD( &gc_bloom_tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
		GX_FALSE, GX_FALSE, GX_ANISO_1 );
	GX_InvalidateTexAll();
	GX_LoadTexObj( &gc_bloom_tex, GX_TEXMAP0 );

	/* 2) 2D pipe: ortho over the full EFB. */
	fb_w = (f32)efb_w;
	fb_h = (f32)efb_h;
	GX_SetViewport( 0.0f, 0.0f, (f32)rmode->fbWidth, (f32)rmode->efbHeight, 0.0f, 1.0f );
	GX_SetScissor( 0, 0, rmode->fbWidth, rmode->efbHeight );
	guOrtho( ortho, 0.0f, fb_h, 0.0f, fb_w, 0.0f, 1.0f );
	GX_LoadProjectionMtx( ortho, GX_ORTHOGRAPHIC );
	guMtxIdentity( ident );
	GX_LoadPosMtxImm( ident, GX_PNMTX0 );
	GX_SetCurrentMtx( GX_PNMTX0 );

	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP );
	GX_SetNumChans( 0 );
	GX_SetNumTexGens( GC_BLOOM_TAPS );
	for( i = 0; i < GC_BLOOM_TAPS; i++ )
		GX_SetTexCoordGen( GX_TEXCOORD0 + i, GX_TG_MTX2x4, GX_TG_TEX0 + i, GX_IDENTITY );

	/* Each tap: clamp(tex - threshold), then add tap/8 into reg2.
	 * Last stage is reg2 * tint * 2. Threshold before the average so a
	 * bright hull is not cancelled by the dark water next to it. */
	thresh.r = thresh.g = thresh.b = GC_BLOOM_THRESHOLD;
	thresh.a = 0;
	tint.r = (u8)(( GC_BLOOM_TINT_R * GC_BLOOM_TINT_A ) / 255 );
	tint.g = (u8)(( GC_BLOOM_TINT_G * GC_BLOOM_TINT_A ) / 255 );
	tint.b = (u8)(( GC_BLOOM_TINT_B * GC_BLOOM_TINT_A ) / 255 );
	tint.a = 255;
	zero.r = zero.g = zero.b = zero.a = 0;
	GX_SetTevColor( GX_TEVREG0, thresh );
	GX_SetTevColor( GX_TEVREG1, tint );
	GX_SetTevColor( GX_TEVREG2, zero );
	GX_SetNumTevStages( GC_BLOOM_TAPS * 2 + 1 );
	for( i = 0; i < GC_BLOOM_TAPS; i++ )
	{
		const u8 sub = (u8)( GX_TEVSTAGE0 + i * 2 );
		const u8 acc = (u8)( sub + 1 );

		GX_SetTevOrder( sub, GX_TEXCOORD0 + i, GX_TEXMAP0, GX_COLORNULL );
		GX_SetTevColorIn( sub, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
		GX_SetTevColorOp( sub, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( sub, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( sub, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );

		GX_SetTevOrder( acc, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
		GX_SetTevKColorSel( acc, GX_TEV_KCSEL_1_8 );
		GX_SetTevColorIn( acc, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_C2 );
		GX_SetTevColorOp( acc, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG2 );
		GX_SetTevAlphaIn( acc, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( acc, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	{
		const u8 fin = (u8)( GX_TEVSTAGE0 + GC_BLOOM_TAPS * 2 );

		GX_SetTevOrder( fin, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL );
		GX_SetTevColorIn( fin, GX_CC_ZERO, GX_CC_C2, GX_CC_C1, GX_CC_ZERO );
		GX_SetTevColorOp( fin, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( fin, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO );
		GX_SetTevAlphaOp( fin, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}

	/* Vertex stream: POS + one texcoord per tap (VTXFMT7 is unused elsewhere). */
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	for( i = 0; i < GC_BLOOM_TAPS; i++ )
	{
		GX_SetVtxDesc( GX_VA_TEX0 + i, GX_DIRECT );
		GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_TEX0 + i, GX_TEX_ST, GX_F32, 0 );
	}
	GX_SetVtxAttrFmt( GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	GX_InvVtxCache();

	du = 1.0f / (f32)bw;
	dv = 1.0f / (f32)bh;
	{
		static const f32 corner[4][4] = {
			/* x, y, u, v (unit) */
			{ 0.0f, 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 1.0f, 0.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 0.0f, 1.0f, 0.0f, 1.0f },
		};
		int c, t;

		GX_Begin( GX_QUADS, GX_VTXFMT7, 4 );
		for( c = 0; c < 4; c++ )
		{
			GX_Position3f32( corner[c][0] * fb_w, corner[c][1] * fb_h, 0.0f );
			for( t = 0; t < GC_BLOOM_TAPS; t++ )
				GX_TexCoord2f32( corner[c][2] + taps[t][0] * du,
					corner[c][3] + taps[t][1] * dv );
		}
		GX_End();
	}

	R_GXBloomGlowAdd( fb_w, fb_h );
	gc_glow_src = gc_bloom_buf;
	gc_glow_src_w = bw;
	gc_glow_src_h = bh;

	/* Leave a sane default blend/TEV for whoever draws next. */
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	GX_SetNumTevStages( 1 );
	GX_SetNumTexGens( 1 );
	GX_SetNumChans( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	for( i = 0; i < GC_BLOOM_TAPS * 2 + 1; i++ )
		GX_SetTevKColorSel( (u8)( GX_TEVSTAGE0 + i ), GX_TEV_KCSEL_1 );
	GX_ClearVtxDesc();

	if( !logged )
	{
		logged = true;
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: GX bloom pass %ux%u I8 taps=%d thresh=%d tint=%d,%d,%d\n",
			bw, bh, GC_BLOOM_TAPS, GC_BLOOM_THRESHOLD, tint.r, tint.g, tint.b );
	}
	return true;
}

/*
 * Sun disc (Source env_sun) — a soft radial fan at the sun's projected
 * position, drawn into the sky before the world. Expects the caller's 2D
 * sky state: ortho over the EFB, Z off, POS+CLR0 vertex desc, PASSCLR.
 *
 * d2_lostcoast: env_sun angles "0 317 0" pitch -10 → towards the sun is
 * yaw 137, 10 degrees up; rendercolor 253 236 198.
 */
#define GC_SUN_YAW	137.0f
#define GC_SUN_ELEV	10.0f
#define GC_SUN_SEGS	20

static void R_GXSunFan( f32 cx, f32 cy, f32 rx, f32 ry, u32 inner, u32 outer )
{
	int i;

	GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, GC_SUN_SEGS + 2 );
	GX_Position3f32( cx, cy, -0.40f );
	GX_Color1u32( inner );
	for( i = 0; i <= GC_SUN_SEGS; i++ )
	{
		const f32 a = (f32)i * ( 2.0f * (f32)M_PI_F / (f32)GC_SUN_SEGS );

		GX_Position3f32( cx + cosf( a ) * rx, cy + sinf( a ) * ry, -0.40f );
		GX_Color1u32( outer );
	}
	GX_End();
}

qboolean R_GXBloomDrawSun( void *rmode_ptr )
{
	GXRModeObj *rmode = (GXRModeObj *)rmode_ptr;
	vec3_t fwd, right, up, sun;
	f32 fb_w, fb_h, df, tx, ty, cx, cy, r;
	const f32 yaw = GC_SUN_YAW * (f32)M_PI_F / 180.0f;
	const f32 el = GC_SUN_ELEV * (f32)M_PI_F / 180.0f;
	f32 fov_x = RI.rvp.fov_x > 1.0f ? RI.rvp.fov_x : 90.0f;
	f32 fov_y = RI.rvp.fov_y > 1.0f ? RI.rvp.fov_y : 90.0f;

	if( !rmode || !R_GXBloomEnabled())
		return false;
	fb_w = (f32)rmode->fbWidth;
	fb_h = (f32)rmode->efbHeight;
	sun[0] = cosf( el ) * cosf( yaw );
	sun[1] = cosf( el ) * sinf( yaw );
	sun[2] = sinf( el );
	AngleVectors( RI.rvp.viewangles, fwd, right, up );
	df = DotProduct( sun, fwd );
	if( df < 0.15f )
		return true; /* sun behind / far off-axis: nothing to draw */
	tx = DotProduct( sun, right ) / df / tanf( fov_x * (f32)M_PI_F / 360.0f );
	ty = DotProduct( sun, up ) / df / tanf( fov_y * (f32)M_PI_F / 360.0f );
	cx = fb_w * 0.5f * ( 1.0f + tx );
	cy = fb_h * 0.5f * ( 1.0f - ty );
	r = fb_h * 0.5f / tanf( fov_y * (f32)M_PI_F / 360.0f ); /* pixels per unit tan */

	/* Self-contained 2D colour state (the sky clear left ortho + identity). */
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetNumChans( 1 );
	GX_SetNumTexGens( 0 );
	GX_SetNumTevStages( 1 );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP );
	GX_SetCullMode( GX_CULL_NONE );
	/* Banding fix: a stack of thin low-alpha fans, then dither. */
	GX_SetDither( GX_TRUE );
	{
		const int layers = 14;
		int s;

		for( s = 0; s < layers; s++ )
		{
			const f32 t = (f32)s / (f32)( layers - 1 );
			const f32 rr = r * ( 0.52f - 0.48f * t * t );
			const u32 a = (u32)( 14.0f + 40.0f * t * t * t );
			const u32 rgb = ( t < 0.7f ) ? 0xFDECC6u : 0xFFF8E8u;

			R_GXSunFan( cx, cy, rr, rr, ( rgb << 8 ) | a, ( rgb << 8 ) | 0u );
		}
	}
	GX_SetDither( GX_FALSE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP );
	return true;
}

/*
 * Distance fog (Source env_fog_controller) as Flipper hardware fog.
 * d2_lostcoast / sky_camera: fogcolor 151 166 181, fogstart 1600, fogend 50000.
 * Independent of bloom — WorldDL used to stub these and kill map fog.
 */
static qboolean gc_fog_on;

/* A converted MOHAA map's far plane (worldspawn "farplane", already in map
 * units, and "farplane_color", 0..1): retail fogs to that colour and draws
 * nothing past it, which is how its forests stay dense. 0 = none. */
static char gc_mapfog_name[64];
static float gc_mapfog_end;
static GXColor gc_mapfog_col = { 51, 51, 77, 255 };

static const char *R_GXWorldspawnValue( const char *ents, const char *end, const char *key )
{
	char quoted[40];
	const char *p;
	size_t n;

	Q_snprintf( quoted, sizeof( quoted ), "\"%s\"", key );
	n = Q_strlen( quoted );
	p = Q_strstr( ents, quoted );
	if( !p || p + n >= end )
		return NULL;
	p += n;
	while( p < end && *p != '"' )
		p++;
	return p < end ? p + 1 : NULL;
}

float R_GXMapFarplane( void )
{
	const model_t *world = WORLDMODEL;

	if( !world || !world->name[0] )
		return 0.0f;
	if( Q_stricmp( world->name, gc_mapfog_name ))
	{
		const char *ents = world->entities;
		const char *end = ents ? Q_strchr( ents, '}' ) : NULL;	/* worldspawn comes first */
		const char *v;

		Q_strncpy( gc_mapfog_name, world->name, sizeof( gc_mapfog_name ));
		gc_mapfog_end = 0.0f;
		if( end && ( v = R_GXWorldspawnValue( ents, end, "farplane" )) != NULL )
		{
			const float fp = (float)strtod( v, NULL );

			if( fp >= 64.0f )
			{
				float rgb[3] = { 0.2f, 0.2f, 0.3f };
				int k;

				gc_mapfog_end = fp;
				if(( v = R_GXWorldspawnValue( ents, end, "farplane_color" )) != NULL )
				{
					char *next = (char *)v;

					for( k = 0; k < 3; k++ )
						rgb[k] = (float)strtod( next, &next );
				}
				gc_mapfog_col.r = (u8)( bound( 0.0f, rgb[0], 1.0f ) * 255.0f + 0.5f );
				gc_mapfog_col.g = (u8)( bound( 0.0f, rgb[1], 1.0f ) * 255.0f + 0.5f );
				gc_mapfog_col.b = (u8)( bound( 0.0f, rgb[2], 1.0f ) * 255.0f + 0.5f );
				gEngfuncs.Con_Reportf( "Xash3D GameCube: map farplane %.0f colour %u %u %u\n", fp,
					gc_mapfog_col.r, gc_mapfog_col.g, gc_mapfog_col.b );
			}
		}
	}
	return gc_mapfog_end;
}

/* The far plane fog colour as 0xRRGGBBAA (the clear colour past the plane). */
u32 R_GXMapFogRGBA( void )
{
	return ((u32)gc_mapfog_col.r << 24 ) | ((u32)gc_mapfog_col.g << 16 ) | ((u32)gc_mapfog_col.b << 8 ) | 0xffu;
}

/* Arms the map's far plane fog for a projection with these planes; false
 * when the map has none. */
qboolean R_GXMapFogSet( f32 z_near, f32 z_far )
{
	if( R_GXMapFarplane() <= 0.0f )
		return false;
	GX_SetFog( GX_FOG_PERSP_LIN, 0.0f, gc_mapfog_end, z_near, z_far, gc_mapfog_col );
	return true;
}

void R_GXFogBegin( void )
{
	const GXColor fog = { 151, 166, 181, 255 };
	f32 znear = 4.0f;
	f32 zfar;
	static qboolean logged;

	if( gc_fog_on )
		return;
	if( R_GXMapFarplane() > 0.0f )
	{
		/* MOHAA's far plane: linear from the eye to the plane. */
		static qboolean map_logged;

		zfar = Q_max( 256.0f, RI.farClip > 1.0f ? RI.farClip : 4096.0f );
		R_GXMapFogSet( znear, zfar );
		gc_fog_on = true;
		if( !map_logged )
		{
			map_logged = true;
			gEngfuncs.Con_Reportf( "Xash3D GameCube: fog map farplane colour=%u,%u,%u end=%.0f zfar=%.0f\n",
				gc_mapfog_col.r, gc_mapfog_col.g, gc_mapfog_col.b, gc_mapfog_end, zfar );
		}
		return;
	}
	/* nearZ/farZ MUST match guFrustum from R_GXBuildWorldProjection.
	 * fogstart/fogend stay at map values (1600/50000) even when zfar is
	 * shorter — density just never reaches 1.0 inside the frustum. */
	zfar = Q_max( 256.0f, RI.farClip > 1.0f ? RI.farClip : 4096.0f );
	/* near/far match R_GXBuildWorldProjection; fogstart/end stay map values. */
	GX_SetFog( GX_FOG_PERSP_LIN, 1600.0f, 50000.0f, znear, zfar, fog );
	gc_fog_on = true;
	if( !logged )
	{
		logged = true;
		gEngfuncs.Con_Reportf(
			"Xash3D GameCube: fog env_fog_controller color=151,166,181 start=1600 end=50000 zfar=%.0f\n",
			zfar );
	}
}

/* Held for the whole 3D scene on a map with a far plane: the world, its
 * brush entities (converted detail walls and terrain are func_walls), the
 * props and the characters all fog alike. The passes' own begin/end pairs
 * leave it on meanwhile. */
static qboolean gc_fog_scene;

void R_GXFogSceneBegin( void )
{
	if( R_GXMapFarplane() <= 0.0f )
		return;
	R_GXFogBegin();
	gc_fog_scene = true;
}

void R_GXFogSceneEnd( void )
{
	extern void R_GXFogEnd( void );

	gc_fog_scene = false;
	R_GXFogEnd();
}

void R_GXFogEnd( void )
{
	const GXColor none = { 0, 0, 0, 0 };

	if( !gc_fog_on || gc_fog_scene )
		return;
	GX_SetFog( GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, none );
	gc_fog_on = false;
}

#endif /* XASH_GAMECUBE */
