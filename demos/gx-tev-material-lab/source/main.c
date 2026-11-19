/*---------------------------------------------------------------------------------
	GX / TEV Material Lab
	Hero: real PBR rust maps from ~/Desktop/pbr-gc/rust (albedo/AO/metallic/
	normal/roughness) — NOT a reference-extracted matcap.
	Showcase: Flipper multi-stage TEV paths with live HUD status.
---------------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/tpl.h>

#include "env_sharp_tpl.h"
#include "env_sharp.h"
#include "env_mid_tpl.h"
#include "env_mid.h"
#include "env_blur_tpl.h"
#include "env_blur.h"
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
#include "font_tpl.h"
#include "font.h"
#include "detail_tpl.h"
#include "detail.h"
#include "specmask_tpl.h"
#include "specmask.h"
#include "rust_albedo_tpl.h"
#include "rust_albedo.h"
#include "rust_ao_tpl.h"
#include "rust_ao.h"
#include "rust_metallic_tpl.h"
#include "rust_metallic.h"
#include "rust_normal_tpl.h"
#include "rust_normal.h"
#include "rust_roughness_tpl.h"
#include "rust_roughness.h"

#define DEFAULT_FIFO_SIZE	(256 * 1024)
#define SPHERE_SLICES		72
#define SPHERE_STACKS		56
#define PI					3.14159265358979323846f

#define LAB_FOV_DEG			36.0f
#define LAB_CAM_DIST		5.15f
#define LAB_LOOK_Y			0.00f
#define LAB_CAM_Y			0.28f

enum {
	FEAT_HERO_RUST_PBR = 0,
	FEAT_FRESNEL_KONST,
	FEAT_MOD2X_DETAIL,
	FEAT_SPEC_MASK,
	FEAT_INDTEX_BUMP,
	FEAT_FOG_HAZE,
	FEAT_SCALE2_LIGHT,
	FEAT_ALPHA_CLIP,
	FEAT_COUNT
};

typedef struct {
	f32 x, y, z;
	f32 nx, ny, nz;
	f32 u, v;		/* spherical UV — albedo / AO / normal / metal / rough */
	f32 u2, v2;		/* matcap UV — env reflections */
	f32 fu;			/* Schlick (1-N·V)^5 */
	u8 r, g, b, a;
} SphereVert;

typedef struct {
	const char *name;
	GXColor base;
	f32 metallic;
	f32 roughness;
	int use_rust_maps;	/* hero uses imported PBR set */
} Material;

static GXRModeObj *rmode = NULL;
static void *frameBuffer[2] = { NULL, NULL };

static GXTexObj texEnvSharp, texEnvMid, texEnvBlur;
static GXTexObj texChecker, texShadow;
static GXTexObj texHudTL, texHudFPS, texHudBottom;
static GXTexObj texFont, texDetail, texSpec;
static GXTexObj texRustAlbedo, texRustAO, texRustMetal, texRustNormal, texRustRough;
static TPLFile tplEnvSharp, tplEnvMid, tplEnvBlur, tplChecker, tplShadow;
static TPLFile tplHudTL, tplHudFPS, tplHudBottom;
static TPLFile tplFont, tplDetail, tplSpec;
static TPLFile tplRustAlbedo, tplRustAO, tplRustMetal, tplRustNormal, tplRustRough;

static SphereVert sphereVerts[(SPHERE_STACKS + 1) * (SPHERE_SLICES + 1)];
static u16 sphereIdx[SPHERE_STACKS * SPHERE_SLICES * 6];
static int sphereIdxCount = 0;

/*
 * Rust maps (TextureCom / Tim's Desktop set): albedo is the surface color;
 * metallic ~low, roughness ~high — dielectric rust with metal flecks.
 */
static Material materials[] = {
	{ "RUST",    { 255, 255, 255, 255 }, 0.08f, 0.85f, 1 },
	/* Warm copper tuned toward the Material Lab reference look */
	{ "COPPER",  { 235, 140,  72, 255 }, 1.00f, 0.25f, 0 },
	{ "GOLD",    { 255, 220, 120, 255 }, 1.00f, 0.20f, 0 },
	{ "IRON",    { 180, 185, 195, 255 }, 1.00f, 0.40f, 0 },
	{ "CHROME",  { 235, 240, 255, 255 }, 1.00f, 0.08f, 0 },
	{ "PLASTIC", { 255, 110,  90, 255 }, 0.00f, 0.45f, 0 },
};
static const int materialCount = (int)(sizeof(materials) / sizeof(materials[0]));
static int materialIndex = 1; /* start on COPPER for reference UI parity */
static int envMapEnabled = 1;
static int featureIndex = FEAT_HERO_RUST_PBR;
static f32 roughnessOverride = -1.0f;
static f32 animTime = 0.0f;

static const char *featureNames[FEAT_COUNT] = {
	"RUST PBR MAPS",
	"FRESNEL+KONST",
	"MOD2X DETAIL",
	"SPEC MASK ADD",
	"INDTEX BUMP",
	"FOG HAZE",
	"SCALE2 LIGHT",
	"ALPHA CLIP",
};

static f32 clampf(f32 v, f32 lo, f32 hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static void build_sphere(f32 radius)
{
	int stack, slice, i = 0;
	f32 vx = 0.01f, vy = 0.08f, vz = 1.0f;
	f32 inv = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
	vx *= inv; vy *= inv; vz *= inv;

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
			f32 ndotv;

			v->nx = sx; v->ny = sy; v->nz = sz;
			v->x = sx * radius;
			v->y = sy * radius;
			v->z = sz * radius;

			/* Spherical UV for albedo / AO / normal / metal / roughness */
			v->u = (f32)slice / (f32)SPHERE_SLICES;
			v->v = (f32)stack / (f32)SPHERE_STACKS;
			/* Matcap UV for env reflections */
			v->u2 = clampf(sx * 0.5f + 0.5f, 0.0f, 1.0f);
			v->v2 = clampf(-sy * 0.5f + 0.5f, 0.0f, 1.0f);
			ndotv = clampf(sx * vx + sy * vy + sz * vz, 0.0f, 1.0f);
			v->fu = powf(1.0f - ndotv, 5.0f);
			v->a = 255;
			v->r = v->g = v->b = 255;
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

static f32 current_roughness(const Material *mat)
{
	return (roughnessOverride >= 0.0f) ? roughnessOverride : mat->roughness;
}

/* Discrete Flipper env LODs — proves roughness by swapping pre-blurred maps.
 * R≈0.25 (copper reference) stays on sharp so the courtyard still reads. */
static GXTexObj *select_env_tex(f32 roughness)
{
	if (roughness < 0.35f)
		return &texEnvSharp;
	if (roughness < 0.70f)
		return &texEnvMid;
	return &texEnvBlur;
}

static const char *env_lod_name(f32 roughness)
{
	if (roughness < 0.35f)
		return "ENV LOD SHARP";
	if (roughness < 0.70f)
		return "ENV LOD MID";
	return "ENV LOD BLUR";
}

static void tint_sphere_vertices(const Material *mat)
{
	int n = (SPHERE_STACKS + 1) * (SPHERE_SLICES + 1);
	int i;
	f32 roughness = current_roughness(mat);
	/*
	 * Soften env with roughness, but keep copper/chrome bright at low R
	 * (reference copper ≈ 0.25 still reads as metal, not mud).
	 */
	f32 spec_scale = clampf(1.0f - roughness * 0.40f, 0.45f, 1.0f);

	for (i = 0; i < n; ++i) {
		SphereVert *v = &sphereVerts[i];
		v->r = mat->base.r;
		v->g = mat->base.g;
		v->b = mat->base.b;
		if (mat->metallic > 0.5f) {
			/* Metals: strong full-sphere env; Fresnel lifts the rim */
			f32 w = clampf(0.82f + 0.18f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		} else {
			/* Dielectrics / rust: Fresnel rim only, still roughness-scaled */
			f32 w = clampf(0.04f + 0.96f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		}
	}
}

/*
 * Hero: albedo × AO + env×Fresnel×(metal+rim), synthetic env (not ref-extracted).
 * TEX0 = albedo (spher UV), TEX1 = AO, TEX2 = env (matcap UV), TEX3 = metallic.
 */
static void setup_tev_hero_rust(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_LoadTexObj(&texRustAO, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texRustAO, GX_REPEAT, GX_REPEAT);

	/* S0: albedo */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* S1: × AO; carry Fresnel in vertex alpha */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* S2: albedo*AO + synthetic env × Fresnel (metal flecks / rim) */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	if (envMapEnabled) {
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	} else {
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
	}
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);

	(void)mat;
}

/* Non-rust materials: tinted synthetic env matcap */
static void setup_tev_env_tint(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP0);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.85f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	/* env × metal tint */
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* CPREV + CPREV*A ≈ env*tint*(1+Fresnel) — brightens metals without clipping chrome */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

static void setup_tev_fresnel_konst(const Material *mat)
{
	f32 roughness = current_roughness(mat);
	u8 f0 = (mat->metallic > 0.5f) ? 230 : 20;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP1);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ f0, f0, f0, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* CPREV + C0 * APREV */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_C0, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

static void setup_tev_mod2x(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_InitTexObjLOD(&texRustAlbedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.10f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	(void)mat;
}

static void setup_tev_spec_mask(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_LoadTexObj(&texSpec, GX_TEXMAP1);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(&texRustAlbedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.15f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 220, 180, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
	(void)mat;
}

static void setup_tev_indtex_bump(const Material *mat)
{
	f32 roughness = current_roughness(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.12f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.45f;
	im[1][1] = 0.45f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = phase * 0.7f - (f32)(int)(phase * 0.7f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_LoadTexObj(&texRustNormal, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texRustNormal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	/* IndTex: normal warps env sample */
	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
	(void)mat;
}

static void setup_tev_scale2_light(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(&texRustAlbedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.20f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	(void)mat;
}

static void setup_tev_flat_lit(const Material *mat)
{
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texRustAlbedo, GX_TEXMAP0);
	GX_InitTexObjWrapMode(&texRustAlbedo, GX_REPEAT, GX_REPEAT);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	(void)mat;
}

static void setup_pbr_tev(const Material *mat, int use_env)
{
	if (!use_env && featureIndex != FEAT_MOD2X_DETAIL && featureIndex != FEAT_SCALE2_LIGHT
		&& featureIndex != FEAT_HERO_RUST_PBR) {
		setup_tev_flat_lit(mat);
		return;
	}

	switch (featureIndex) {
	case FEAT_FRESNEL_KONST: setup_tev_fresnel_konst(mat); break;
	case FEAT_MOD2X_DETAIL:  setup_tev_mod2x(mat); break;
	case FEAT_SPEC_MASK:     setup_tev_spec_mask(mat); break;
	case FEAT_INDTEX_BUMP:   setup_tev_indtex_bump(mat); break;
	case FEAT_SCALE2_LIGHT:  setup_tev_scale2_light(mat); break;
	case FEAT_FOG_HAZE:
	case FEAT_ALPHA_CLIP:
	case FEAT_HERO_RUST_PBR:
	default:
		if (mat->use_rust_maps)
			setup_tev_hero_rust(mat);
		else if (use_env)
			setup_tev_env_tint(mat);
		else
			setup_tev_flat_lit(mat);
		break;
	}
}

static void draw_sphere(Mtx modelview)
{
	int i;

	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

	GX_SetCullMode(GX_CULL_BACK);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	if (featureIndex == FEAT_ALPHA_CLIP) {
		GX_SetAlphaCompare(GX_GREATER, 16, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
	} else {
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
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
		GX_TexCoord2f32(v->u2, v->v2);
	}
	GX_End();

	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetNumIndStages(0);
	GX_SetTevDirect(GX_TEVSTAGE0);
}

static void draw_floor(const Mtx view)
{
	Mtx model, modelview;
	f32 y = -1.12f;
	f32 s = 8.0f;

	guMtxIdentity(model);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texChecker, GX_TEXMAP0);
	GX_SetTevDirect(GX_TEVSTAGE0);
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
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(-s, y, -s); GX_TexCoord2f32(0, 0);
		GX_Position3f32( s, y, -s); GX_TexCoord2f32(3.0f, 0);
		GX_Position3f32( s, y,  s); GX_TexCoord2f32(3.0f, 3.0f);
		GX_Position3f32(-s, y,  s); GX_TexCoord2f32(0, 3.0f);
	GX_End();
}

static void draw_shadow(const Mtx view)
{
	Mtx model, modelview;
	f32 y = -1.115f;
	f32 sx = 1.45f;
	f32 sz = 1.05f;

	guMtxIdentity(model);
	guMtxTransApply(model, model, 0.05f, 0.0f, 0.12f);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_LoadTexObj(&texShadow, GX_TEXMAP0);
	GX_SetTevDirect(GX_TEVSTAGE0);
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
		GX_Position3f32(-sx, y, -sz); GX_TexCoord2f32(0, 0);
		GX_Position3f32( sx, y, -sz); GX_TexCoord2f32(1, 0);
		GX_Position3f32( sx, y,  sz); GX_TexCoord2f32(1, 1);
		GX_Position3f32(-sx, y,  sz); GX_TexCoord2f32(0, 1);
	GX_End();
}

static void draw_hud_uv(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1,
	f32 u0, f32 v0, f32 u1, f32 v1)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(x0, y0, 0); GX_TexCoord2f32(u0, v0);
		GX_Position3f32(x1, y0, 0); GX_TexCoord2f32(u1, v0);
		GX_Position3f32(x1, y1, 0); GX_TexCoord2f32(u1, v1);
		GX_Position3f32(x0, y1, 0); GX_TexCoord2f32(u0, v1);
	GX_End();
}

static void draw_char(f32 x, f32 y, char c, f32 scale)
{
	int idx, col, row;
	f32 u0, v0, u1, v1;
	f32 w = 8.0f * scale;
	f32 h = 16.0f * scale;

	if (c < 32 || c > 127) c = '?';
	idx = (int)c - 32;
	col = idx % 16;
	row = idx / 16;
	u0 = (f32)col * 8.0f / 128.0f;
	v0 = (f32)row * 16.0f / 128.0f;
	u1 = u0 + 8.0f / 128.0f;
	v1 = v0 + 16.0f / 128.0f;

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(x, y, 0); GX_TexCoord2f32(u0, v0);
		GX_Position3f32(x + w, y, 0); GX_TexCoord2f32(u1, v0);
		GX_Position3f32(x + w, y + h, 0); GX_TexCoord2f32(u1, v1);
		GX_Position3f32(x, y + h, 0); GX_TexCoord2f32(u0, v1);
	GX_End();
}

static void draw_text(f32 x, f32 y, const char *s, f32 scale)
{
	while (*s) {
		draw_char(x, y, *s, scale);
		x += 8.0f * scale;
		++s;
	}
}

static void draw_hud(f32 fb_w, f32 fb_h, const Material *mat)
{
	Mtx44 ortho;
	Mtx ident;
	char line[72];
	f32 rough = current_roughness(mat);

	guOrtho(ortho, 0, fb_h, 0, fb_w, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	guMtxIdentity(ident);
	GX_LoadPosMtxImm(ident, GX_PNMTX0);

	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0,0,0,0});

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	/* Logo + banner only (stats drawn live — no reference sphere extract) */
	draw_hud_uv(&texHudTL, 0, 0, 380, 120,
		0.0f, 0.0f, 380.0f / 512.0f, 120.0f / 256.0f);
	draw_hud_uv(&texHudFPS, 508, 4, 636, 40,
		0.0f, 0.0f, 1.0f, 36.0f / 64.0f);
	draw_hud_uv(&texHudBottom, 0, 436, 640, 480,
		0.0f, 0.0f, 640.0f / 1024.0f, 44.0f / 64.0f);

	GX_LoadTexObj(&texFont, GX_TEXMAP0);
	/* Live stats — same three-line layout as the copper reference */
	snprintf(line, sizeof(line), "MATERIAL: %s", mat->name);
	draw_text(12, 112, line, 1.0f);
	snprintf(line, sizeof(line), "METALLIC %.2f   ROUGHNESS %.2f",
		mat->metallic, rough);
	draw_text(12, 130, line, 1.0f);
	if (mat->use_rust_maps) {
		snprintf(line, sizeof(line), "ENV %s | %s",
			envMapEnabled ? "ON " : "OFF", featureNames[featureIndex]);
		draw_text(12, 148, line, 1.0f);
		snprintf(line, sizeof(line), "%s (B steps rough)", env_lod_name(rough));
		draw_text(12, 166, line, 1.0f);
		draw_text(12, 184, "MAPS: albedo ao metal normal rough", 1.0f);
	} else if (featureIndex == FEAT_HERO_RUST_PBR || featureIndex == FEAT_FRESNEL_KONST) {
		/* Copper-reference three-line status (no extra LOD line) */
		draw_text(12, 148, "ENV MAP + FRESNEL APPROX", 1.0f);
	} else {
		snprintf(line, sizeof(line), "ENV %s | %s",
			envMapEnabled ? "ON " : "OFF", featureNames[featureIndex]);
		draw_text(12, 148, line, 1.0f);
	}

	(void)fb_w;
}

static void load_textures(void)
{
	TPL_OpenTPLFromMemory(&tplEnvSharp, (void *)env_sharp_tpl, env_sharp_tpl_size);
	TPL_GetTexture(&tplEnvSharp, env_sharp, &texEnvSharp);
	TPL_OpenTPLFromMemory(&tplEnvMid, (void *)env_mid_tpl, env_mid_tpl_size);
	TPL_GetTexture(&tplEnvMid, env_mid, &texEnvMid);
	TPL_OpenTPLFromMemory(&tplEnvBlur, (void *)env_blur_tpl, env_blur_tpl_size);
	TPL_GetTexture(&tplEnvBlur, env_blur, &texEnvBlur);
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
	TPL_OpenTPLFromMemory(&tplFont, (void *)font_tpl, font_tpl_size);
	TPL_GetTexture(&tplFont, font, &texFont);
	TPL_OpenTPLFromMemory(&tplDetail, (void *)detail_tpl, detail_tpl_size);
	TPL_GetTexture(&tplDetail, detail, &texDetail);
	TPL_OpenTPLFromMemory(&tplSpec, (void *)specmask_tpl, specmask_tpl_size);
	TPL_GetTexture(&tplSpec, specmask, &texSpec);

	TPL_OpenTPLFromMemory(&tplRustAlbedo, (void *)rust_albedo_tpl, rust_albedo_tpl_size);
	TPL_GetTexture(&tplRustAlbedo, rust_albedo, &texRustAlbedo);
	TPL_OpenTPLFromMemory(&tplRustAO, (void *)rust_ao_tpl, rust_ao_tpl_size);
	TPL_GetTexture(&tplRustAO, rust_ao, &texRustAO);
	TPL_OpenTPLFromMemory(&tplRustMetal, (void *)rust_metallic_tpl, rust_metallic_tpl_size);
	TPL_GetTexture(&tplRustMetal, rust_metallic, &texRustMetal);
	TPL_OpenTPLFromMemory(&tplRustNormal, (void *)rust_normal_tpl, rust_normal_tpl_size);
	TPL_GetTexture(&tplRustNormal, rust_normal, &texRustNormal);
	TPL_OpenTPLFromMemory(&tplRustRough, (void *)rust_roughness_tpl, rust_roughness_tpl_size);
	TPL_GetTexture(&tplRustRough, rust_roughness, &texRustRough);

	GX_InitTexObjWrapMode(&texEnvSharp, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texEnvMid, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texEnvBlur, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texChecker, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texShadow, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudTL, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudFPS, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texHudBottom, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texFont, GX_CLAMP, GX_CLAMP);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texSpec, GX_CLAMP, GX_CLAMP);
}

int main(int argc, char **argv)
{
	f32 yscale;
	u32 xfbHeight;
	u32 fb = 0;
	u32 first_frame = 1;
	void *gpfifo = NULL;
	GXColor background = { 22, 20, 24, 0xff };
	Mtx view, model, modelview;
	Mtx44 perspective;
	guVector cam = { 0.02f, LAB_CAM_Y, LAB_CAM_DIST };
	guVector up = { 0.0f, 1.0f, 0.0f };
	guVector look = { 0.0f, LAB_LOOK_Y, 0.0f };
	u16 prevButtons = 0;
	u32 frame = 0;

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
	guPerspective(perspective, LAB_FOV_DEG,
		(f32)rmode->fbWidth / (f32)rmode->efbHeight, 0.1f, 100.0f);

	while (1) {
		Material *mat = &materials[materialIndex];
		u16 down;

		PAD_ScanPads();
		down = PAD_ButtonsDown(0);
		if ((down & PAD_BUTTON_A) && !(prevButtons & PAD_BUTTON_A)) {
			materialIndex = (materialIndex + 1) % materialCount;
			mat = &materials[materialIndex];
			roughnessOverride = -1.0f;
			tint_sphere_vertices(mat);
		}
		if ((down & PAD_BUTTON_B) && !(prevButtons & PAD_BUTTON_B)) {
			f32 r = current_roughness(mat);
			r += 0.25f;
			if (r > 1.01f) r = 0.0f;
			roughnessOverride = r;
			tint_sphere_vertices(mat);
		}
		if ((down & PAD_BUTTON_X) && !(prevButtons & PAD_BUTTON_X)) {
			envMapEnabled ^= 1;
		}
		if ((down & PAD_BUTTON_Y) && !(prevButtons & PAD_BUTTON_Y)) {
			featureIndex = (featureIndex + 1) % FEAT_COUNT;
		}
		prevButtons = PAD_ButtonsHeld(0);
		animTime += 1.0f / 60.0f;
		/*
		 * Capture reel:
		 *  0–150  COPPER @ R=0.25 (reference UI/material parity)
		 *  160–400 CHROME roughness 0→1 (env LOD proof)
		 */
		if (frame == 0 || frame == 1) {
			materialIndex = 1; /* COPPER */
			mat = &materials[materialIndex];
			featureIndex = FEAT_HERO_RUST_PBR;
			roughnessOverride = 0.25f;
			tint_sphere_vertices(mat);
		}
		if (frame == 160) {
			materialIndex = 4; /* CHROME */
			mat = &materials[materialIndex];
			featureIndex = FEAT_HERO_RUST_PBR;
			roughnessOverride = 0.0f;
			tint_sphere_vertices(mat);
		}
		if (frame >= 160 && frame < 460 && ((frame - 160) % 60) == 0) {
			int step = (int)((frame - 160) / 60);
			static const f32 steps[5] = { 0.00f, 0.25f, 0.50f, 0.75f, 1.00f };
			if (step < 5) {
				roughnessOverride = steps[step];
				tint_sphere_vertices(mat);
			}
		}
		if (frame >= 480 && ((frame - 480) % 90) == 0)
			featureIndex = (int)(((frame - 480) / 90) % FEAT_COUNT);
		++frame;

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

		if (featureIndex == FEAT_FOG_HAZE) {
			GXColor fogc = { 40, 36, 48, 255 };
			GX_SetFog(GX_FOG_PERSP_LIN, 4.0f, 14.0f, 0.1f, 100.0f, fogc);
		} else {
			GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0,0,0,0});
		}

		draw_floor(view);
		draw_shadow(view);

		guMtxIdentity(model);
		guMtxConcat(view, model, modelview);
		setup_pbr_tev(mat, envMapEnabled);
		draw_sphere(modelview);

		draw_hud((f32)rmode->fbWidth, (f32)rmode->efbHeight, mat);

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
	}
	return 0;
}
