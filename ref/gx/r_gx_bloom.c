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
 * ( scale / baked )^( 1 / 2.2 ). The sky (depth at the far plane) is left
 * alone: its art is authored for the screen, not in light units.
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
		int gx, gy, n = 0, acc, hi = 31, med = 0;
		float lum_hi, lum_med, t1, t2;

		memset( hist, 0, sizeof( hist ));
		for( gy = 0; gy < 12; gy++ )
		{
			const int y = (int)(( 0.075f + 0.85f * ( (float)gy + 0.5f ) / 12.0f ) * (float)bh );

			for( gx = 0; gx < 20; gx++ )
			{
				const int x = (int)(( 0.05f + 0.9f * ( (float)gx + 0.5f ) / 20.0f ) * (float)bw );

				hist[buf[(( y >> 2 ) * ( bw >> 3 ) + ( x >> 3 )) * 32 + ( y & 3 ) * 8 + ( x & 7 )] >> 3]++;
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
		if( hist[0] == n )
			hi = -1;
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
		gEngfuncs.Con_Reportf( "Xash3D GameCube: exposure scale=%.2f goal=%.2f limits %.2f..%.2f\n",
			gc_expo.scale, goal, gc_expo.lim_min, gc_expo.lim_max );
	return powf( gc_expo.scale / gc_expo.baked, 1.0f / 2.2f );
}

/* One fullscreen multiply by the gain, over everything nearer than the far plane. */
static void R_GXExposureApply( GXRModeObj *rmode, float gain )
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
	GX_SetZMode( GX_TRUE, GX_GREATER, GX_FALSE );
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
	R_GXExposureApply( rmode, R_GXExposureUpdate( gc_bloom_buf, bw, bh ));

	/* 1) EFB -> half-res luma through the copy box filter. */
	GX_SetTexCopySrc( 0, 0, efb_w, efb_h );
	GX_SetTexCopyDst( bw, bh, GX_TF_I8, GX_TRUE );
	DCInvalidateRange( gc_bloom_buf, (u32)bw * bh );
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

void R_GXFogBegin( void )
{
	const GXColor fog = { 151, 166, 181, 255 };
	f32 znear = 4.0f;
	f32 zfar;
	static qboolean logged;

	if( gc_fog_on )
		return;
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

void R_GXFogEnd( void )
{
	const GXColor none = { 0, 0, 0, 0 };

	if( !gc_fog_on )
		return;
	GX_SetFog( GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, none );
	gc_fog_on = false;
}

#endif /* XASH_GAMECUBE */
