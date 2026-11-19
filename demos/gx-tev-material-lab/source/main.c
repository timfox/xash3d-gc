/*---------------------------------------------------------------------------------
	GX / TEV Material Lab
	PBR-like copper sphere via Flipper TEV: base metal * env map + Fresnel approx + specular.
---------------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/tpl.h>

#include "envmap_tpl.h"
#include "envmap.h"
#include "checker_tpl.h"
#include "checker.h"
#include "shadow_tpl.h"
#include "shadow.h"
#include "hud_tl_tpl.h"
#include "hud_tl.h"
#include "hud_fps_tpl.h"
#include "hud_fps.h"
#include "hud_bottom_tpl.h"
#include "hud_bottom.h"

#define DEFAULT_FIFO_SIZE	(256 * 1024)
#define SPHERE_SLICES		48
#define SPHERE_STACKS		32
#define PI					3.14159265358979323846f

typedef struct {
	f32 x, y, z;
	f32 nx, ny, nz;
	f32 u, v;       /* sphere-map / reflection UV */
	f32 fu, fv;     /* fresnel 1D lookup (u) + unused */
	u8 r, g, b, a;  /* copper * fresnel in alpha */
} SphereVert;

typedef struct {
	const char *name;
	GXColor base;
	f32 metallic;
	f32 roughness;
} Material;

static GXRModeObj *rmode = NULL;
static void *frameBuffer[2] = { NULL, NULL };

static GXTexObj texEnv, texChecker, texShadow;
static GXTexObj texHudTL, texHudFPS, texHudBottom;
static TPLFile tplEnv, tplChecker, tplShadow;
static TPLFile tplHudTL, tplHudFPS, tplHudBottom;

static SphereVert sphereVerts[(SPHERE_STACKS + 1) * (SPHERE_SLICES + 1)];
static u16 sphereIdx[SPHERE_STACKS * SPHERE_SLICES * 6];
static int sphereIdxCount = 0;

static Material materials[] = {
	{ "COPPER",  { 184, 115,  51, 255 }, 1.00f, 0.25f },
	{ "GOLD",    { 212, 175,  55, 255 }, 1.00f, 0.20f },
	{ "IRON",    { 140, 140, 145, 255 }, 1.00f, 0.40f },
	{ "PLASTIC", { 200,  60,  50, 255 }, 0.00f, 0.45f },
};
static int materialIndex = 0;
static int envMapEnabled = 1;
static f32 roughnessOverride = -1.0f; /* <0 means use material default */

static f32 clampf(f32 v, f32 lo, f32 hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static void sphere_map_uv(f32 nx, f32 ny, f32 nz, f32 *u, f32 *v)
{
	/* Classic sphere-map from view-space normal (camera looks -Z). */
	f32 r = sqrtf(nx * nx + ny * ny + (nz + 1.0f) * (nz + 1.0f));
	if (r < 1e-5f) r = 1e-5f;
	*u = 0.5f * (nx / r + 1.0f);
	*v = 0.5f * (-ny / r + 1.0f);
}

static void build_sphere(f32 radius)
{
	int stack, slice, i = 0;
	sphereIdxCount = 0;

	for (stack = 0; stack <= SPHERE_STACKS; ++stack) {
		f32 phi = PI * ((f32)stack / (f32)SPHERE_STACKS);
		f32 sy = cosf(phi);
		f32 sr = sinf(phi);
		for (slice = 0; slice <= SPHERE_SLICES; ++slice) {
			f32 theta = 2.0f * PI * ((f32)slice / (f32)SPHERE_SLICES);
			f32 sx = sr * cosf(theta);
			f32 sz = sr * sinf(theta);
			SphereVert *v = &sphereVerts[i++];
			v->nx = sx; v->ny = sy; v->nz = sz;
			v->x = sx * radius;
			v->y = sy * radius;
			v->z = sz * radius;
			/* View-ish normal for sphere-map (camera above/+Z). */
			{
				f32 vx = sx * 0.85f + 0.10f;
				f32 vy = sy * 0.85f + 0.20f;
				f32 vz = sz * 0.85f + 0.45f;
				f32 inv = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
				sphere_map_uv(vx * inv, vy * inv, vz * inv, &v->u, &v->v);
			}
			/* Store (1-N·V)^5; tint_sphere applies Schlick F0 per material. */
			{
				f32 vx = 0.08f, vy = 0.22f, vz = 1.0f;
				f32 inv = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
				vx *= inv; vy *= inv; vz *= inv;
				f32 ndotv = clampf(sx * vx + sy * vy + sz * vz, 0.0f, 1.0f);
				v->fu = powf(1.0f - ndotv, 5.0f);
				v->fv = 0.0f;
				v->a = 255;
				v->r = v->g = v->b = 255;
			}
		}
	}

	for (stack = 0; stack < SPHERE_STACKS; ++stack) {
		for (slice = 0; slice < SPHERE_SLICES; ++slice) {
			u16 i0 = (u16)(stack * (SPHERE_SLICES + 1) + slice);
			u16 i1 = (u16)(i0 + SPHERE_SLICES + 1);
			sphereIdx[sphereIdxCount++] = i0;
			sphereIdx[sphereIdxCount++] = i1;
			sphereIdx[sphereIdxCount++] = (u16)(i0 + 1);
			sphereIdx[sphereIdxCount++] = (u16)(i0 + 1);
			sphereIdx[sphereIdxCount++] = i1;
			sphereIdx[sphereIdxCount++] = (u16)(i1 + 1);
		}
	}
}

static void tint_sphere_vertices(const Material *mat)
{
	int n = (SPHERE_STACKS + 1) * (SPHERE_SLICES + 1);
	int i;
	for (i = 0; i < n; ++i) {
		SphereVert *v = &sphereVerts[i];
		v->r = mat->base.r;
		v->g = mat->base.g;
		v->b = mat->base.b;
		/* Schlick: F = F0 + (1-F0)*(1-N·V)^5 — metals use high F0. */
		f32 m = mat->metallic;
		f32 f0 = (m > 0.5f) ? 0.92f : 0.04f;
		f32 fres = f0 + (1.0f - f0) * clampf(v->fu, 0.0f, 1.0f);
		v->a = (u8)(clampf(fres, 0.0f, 1.0f) * 255.0f);
	}
}

static void setup_pbr_tev(const Material *mat, int use_env)
{
	f32 roughness = (roughnessOverride >= 0.0f) ? roughnessOverride : mat->roughness;

	GX_SetNumChans(1);
	GX_SetNumTexGens(use_env ? 1 : 0);
	GX_SetNumTevStages(use_env ? 3 : 1);

	/*
	 * Pass vertex RGB/A straight through as raster color.
	 * RGB = metal/base tint, A = Fresnel weight. Env map supplies IBL + sun.
	 */
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 0, 0, 0, 0 });
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });

	if (!use_env) {
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		return;
	}

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texEnv, GX_TEXMAP0);

	/* Softness from roughness via LOD bias on the env sample. */
	{
		f32 bias = roughness * 1.5f;
		GX_InitTexObjLOD(&texEnv, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, bias,
			GX_FALSE, GX_TRUE, GX_ANISO_1);
	}

	/*
	 * TEV recipe (metallic PBR approx):
	 *  S0: sample env map
	 *  S1: tint env by vertex RGB (copper)
	 *  S2: C = lerp(copperFill, envTint, fresnelA) ≈ envTint*A + copper*(small)
	 *      implemented as: CPREV * APREV + RASC*(1-ish via konst scale)
	 *      Practical: C = envTint * fresnel + (vertexRGB >> 3) ambient metal
	 */

	GX_SetTevKColor(GX_KCOLOR0, mat->base);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);

	/* S0: env map */
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* S1: tint env by vertex copper RGB; keep fresnel in A */
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* S2: Fresnel-weighted tinted env + small metal fill from konst */
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	/* C = CPREV * APREV + KONST*0  → then we want a little konst add.
	   Use: (CPREV * APREV) + KONST  with konst pre-darkened via material choice,
	   then clamp. For metals konst is copper; scale down by using TB_SUB on a portion.
	   Simpler: C = CPREV * APREV + CPREV>>1 via scale trick:
	   C = CPREV * APREV + KONST (and KONST set to dim copper ambient). */
	{
		GXColor fill = mat->base;
		if (mat->metallic > 0.5f) {
			fill.r = (u8)(fill.r / 5);
			fill.g = (u8)(fill.g / 5);
			fill.b = (u8)(fill.b / 5);
		} else {
			fill.r = (u8)(fill.r / 2);
			fill.g = (u8)(fill.g / 2);
			fill.b = (u8)(fill.b / 2);
		}
		GX_SetTevKColor(GX_KCOLOR0, fill);
	}
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

static void draw_sphere(Mtx modelview)
{
	int i;
	Mtx nrm;
	Mtx mvi;

	GX_LoadPosMtxImm(modelview, GX_PNMTX0);
	guMtxInverse(modelview, mvi);
	guMtxTranspose(mvi, nrm);
	GX_LoadNrmMtxImm(nrm, GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetCullMode(GX_CULL_BACK);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);

	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)sphereIdxCount);
	for (i = 0; i < sphereIdxCount; ++i) {
		SphereVert *v = &sphereVerts[sphereIdx[i]];
		GX_Position3f32(v->x, v->y, v->z);
		GX_Normal3f32(v->nx, v->ny, v->nz);
		GX_Color4u8(v->r, v->g, v->b, v->a);
		GX_TexCoord2f32(v->u, v->v);
	}
	GX_End();
}

static void draw_floor(const Mtx view)
{
	Mtx model, modelview;
	f32 y = -1.05f;
	f32 s = 8.0f;

	guMtxIdentity(model);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texChecker, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(-s, y, -s); GX_TexCoord2f32(0, 0);
		GX_Position3f32( s, y, -s); GX_TexCoord2f32(6, 0);
		GX_Position3f32( s, y,  s); GX_TexCoord2f32(6, 6);
		GX_Position3f32(-s, y,  s); GX_TexCoord2f32(0, 6);
	GX_End();
}

static void draw_shadow(const Mtx view)
{
	Mtx model, modelview;
	f32 y = -1.049f;
	f32 s = 1.15f;

	guMtxIdentity(model);
	guMtxTransApply(model, model, 0.05f, 0.0f, 0.10f);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texShadow, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(-s, y, -s); GX_TexCoord2f32(0, 0);
		GX_Position3f32( s, y, -s); GX_TexCoord2f32(1, 0);
		GX_Position3f32( s, y,  s); GX_TexCoord2f32(1, 1);
		GX_Position3f32(-s, y,  s); GX_TexCoord2f32(0, 1);
	GX_End();
}

static void draw_hud_quad(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(x0, y0, 0); GX_TexCoord2f32(0, 0);
		GX_Position3f32(x1, y0, 0); GX_TexCoord2f32(1, 0);
		GX_Position3f32(x1, y1, 0); GX_TexCoord2f32(1, 1);
		GX_Position3f32(x0, y1, 0); GX_TexCoord2f32(0, 1);
	GX_End();
}

static void draw_hud(f32 fb_w, f32 fb_h)
{
	Mtx44 ortho;
	Mtx ident;

	guOrtho(ortho, 0, fb_h, 0, fb_w, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	guMtxIdentity(ident);
	GX_LoadPosMtxImm(ident, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	/* Top-left HUD (logo + banner + stats) */
	draw_hud_quad(&texHudTL, 8, 8, 8 + 320, 8 + 160);
	/* FPS top-right */
	draw_hud_quad(&texHudFPS, fb_w - 136, 10, fb_w - 8, 42);
	/* Bottom bar */
	draw_hud_quad(&texHudBottom, 0, fb_h - 40, fb_w, fb_h);
}

static void load_textures(void)
{
	TPL_OpenTPLFromMemory(&tplEnv, (void *)envmap_tpl, envmap_tpl_size);
	TPL_GetTexture(&tplEnv, envmap, &texEnv);
	TPL_OpenTPLFromMemory(&tplChecker, (void *)checker_tpl, checker_tpl_size);
	TPL_GetTexture(&tplChecker, checker, &texChecker);
	TPL_OpenTPLFromMemory(&tplShadow, (void *)shadow_tpl, shadow_tpl_size);
	TPL_GetTexture(&tplShadow, shadow, &texShadow);
	TPL_OpenTPLFromMemory(&tplHudTL, (void *)hud_tl_tpl, hud_tl_tpl_size);
	TPL_GetTexture(&tplHudTL, hud_tl, &texHudTL);
	TPL_OpenTPLFromMemory(&tplHudFPS, (void *)hud_fps_tpl, hud_fps_tpl_size);
	TPL_GetTexture(&tplHudFPS, hud_fps, &texHudFPS);
	TPL_OpenTPLFromMemory(&tplHudBottom, (void *)hud_bottom_tpl, hud_bottom_tpl_size);
	TPL_GetTexture(&tplHudBottom, hud_bottom, &texHudBottom);

	GX_InitTexObjWrapMode(&texEnv, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texChecker, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texShadow, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudTL, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudFPS, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudBottom, GX_CLAMP, GX_CLAMP);
}

int main(int argc, char **argv)
{
	f32 yscale;
	u32 xfbHeight;
	u32 fb = 0;
	u32 first_frame = 1;
	void *gpfifo = NULL;
	GXColor background = { 28, 26, 30, 0xff };
	Mtx view, model, modelview;
	Mtx44 perspective;
	guVector cam = { 0.10f, 0.65f, 4.10f };
	guVector up = { 0.0f, 1.0f, 0.0f };
	guVector look = { 0.0f, 0.00f, 0.0f };
	u32 frame = 0;
	u16 prevButtons = 0;

	(void)argc; (void)argv;

	VIDEO_Init();
	PAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	gpfifo = memalign(32, DEFAULT_FIFO_SIZE);
	memset(gpfifo, 0, DEFAULT_FIFO_SIZE);

	frameBuffer[0] = SYS_AllocateFramebuffer(rmode);
	frameBuffer[1] = SYS_AllocateFramebuffer(rmode);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(frameBuffer[fb]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	fb ^= 1;

	GX_Init(gpfifo, DEFAULT_FIFO_SIZE);
	GX_SetCopyClear(background, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
		((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_BACK);
	GX_CopyDisp(frameBuffer[fb], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_InvalidateTexAll();

	load_textures();
	build_sphere(1.0f);
	tint_sphere_vertices(&materials[materialIndex]);

	guLookAt(view, &cam, &up, &look);
	guPerspective(perspective, 34.0f, (f32)rmode->fbWidth / (f32)rmode->efbHeight, 0.1f, 100.0f);

	while (1) {
		Material *mat = &materials[materialIndex];
		u16 down;
		PAD_ScanPads();
		down = PAD_ButtonsDown(0);
		if ((down & PAD_BUTTON_A) && !(prevButtons & PAD_BUTTON_A)) {
			materialIndex = (materialIndex + 1) % (int)(sizeof(materials) / sizeof(materials[0]));
			mat = &materials[materialIndex];
			roughnessOverride = -1.0f;
			tint_sphere_vertices(mat);
		}
		if ((down & PAD_BUTTON_B) && !(prevButtons & PAD_BUTTON_B)) {
			f32 r = (roughnessOverride >= 0.0f) ? roughnessOverride : mat->roughness;
			r += 0.25f;
			if (r > 1.01f) r = 0.0f;
			roughnessOverride = r;
		}
		if ((down & PAD_BUTTON_X) && !(prevButtons & PAD_BUTTON_X)) {
			envMapEnabled ^= 1;
		}
		prevButtons = PAD_ButtonsHeld(0);

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

		draw_floor(view);
		draw_shadow(view);

		guMtxIdentity(model);
		guMtxTransApply(model, model, 0.0f, 0.0f, 0.0f);
		guMtxConcat(view, model, modelview);
		setup_pbr_tev(mat, envMapEnabled);
		draw_sphere(modelview);

		draw_hud((f32)rmode->fbWidth, (f32)rmode->efbHeight);

		GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(frameBuffer[fb], GX_TRUE);
		GX_DrawDone();

		VIDEO_SetNextFramebuffer(frameBuffer[fb]);
		if (first_frame) {
			first_frame = 0;
			VIDEO_SetBlack(FALSE);
		}
		VIDEO_Flush();
		VIDEO_WaitVSync();
		fb ^= 1;
		++frame;

		(void)frame;
	}
	return 0;
}
