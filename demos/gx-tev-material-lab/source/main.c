/*---------------------------------------------------------------------------------
	GX / TEV Material Lab — Flipper shader exploration tech demo

	Production presentation: gallery title cards, idle camera orbit, soft
	material transitions. START toggles Gallery ↔ Lab (engineer HUD).
	Any pad input cancels the capture reel so the player stays in control.

	Hero maps: real TextureCom / Desktop PBR sets (albedo AO metal normal rough).
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
#include "damascus_albedo_tpl.h"
#include "damascus_albedo.h"
#include "damascus_ao_tpl.h"
#include "damascus_ao.h"
#include "damascus_metallic_tpl.h"
#include "damascus_metallic.h"
#include "damascus_normal_tpl.h"
#include "damascus_normal.h"
#include "damascus_roughness_tpl.h"
#include "damascus_roughness.h"
#include "corrugated_albedo_tpl.h"
#include "corrugated_albedo.h"
#include "corrugated_ao_tpl.h"
#include "corrugated_ao.h"
#include "corrugated_metallic_tpl.h"
#include "corrugated_metallic.h"
#include "corrugated_normal_tpl.h"
#include "corrugated_normal.h"
#include "corrugated_roughness_tpl.h"
#include "corrugated_roughness.h"
#include "cladding_albedo_tpl.h"
#include "cladding_albedo.h"
#include "cladding_ao_tpl.h"
#include "cladding_ao.h"
#include "cladding_metallic_tpl.h"
#include "cladding_metallic.h"
#include "cladding_normal_tpl.h"
#include "cladding_normal.h"
#include "cladding_roughness_tpl.h"
#include "cladding_roughness.h"
#include "steel_albedo_tpl.h"
#include "steel_albedo.h"
#include "steel_ao_tpl.h"
#include "steel_ao.h"
#include "steel_metallic_tpl.h"
#include "steel_metallic.h"
#include "steel_normal_tpl.h"
#include "steel_normal.h"
#include "steel_roughness_tpl.h"
#include "steel_roughness.h"
#include "concrete_albedo_tpl.h"
#include "concrete_albedo.h"
#include "concrete_ao_tpl.h"
#include "concrete_ao.h"
#include "concrete_metallic_tpl.h"
#include "concrete_metallic.h"
#include "concrete_normal_tpl.h"
#include "concrete_normal.h"
#include "concrete_roughness_tpl.h"
#include "concrete_roughness.h"
#include "brick_albedo_tpl.h"
#include "brick_albedo.h"
#include "brick_ao_tpl.h"
#include "brick_ao.h"
#include "brick_metallic_tpl.h"
#include "brick_metallic.h"
#include "brick_normal_tpl.h"
#include "brick_normal.h"
#include "brick_roughness_tpl.h"
#include "brick_roughness.h"
#include "wood_albedo_tpl.h"
#include "wood_albedo.h"
#include "wood_ao_tpl.h"
#include "wood_ao.h"
#include "wood_metallic_tpl.h"
#include "wood_metallic.h"
#include "wood_normal_tpl.h"
#include "wood_normal.h"
#include "wood_roughness_tpl.h"
#include "wood_roughness.h"
#include "paint_albedo_tpl.h"
#include "paint_albedo.h"
#include "paint_ao_tpl.h"
#include "paint_ao.h"
#include "paint_metallic_tpl.h"
#include "paint_metallic.h"
#include "paint_normal_tpl.h"
#include "paint_normal.h"
#include "paint_roughness_tpl.h"
#include "paint_roughness.h"
#include "solar_albedo_tpl.h"
#include "solar_albedo.h"
#include "solar_ao_tpl.h"
#include "solar_ao.h"
#include "solar_metallic_tpl.h"
#include "solar_metallic.h"
#include "solar_normal_tpl.h"
#include "solar_normal.h"
#include "solar_roughness_tpl.h"
#include "solar_roughness.h"

#define DEFAULT_FIFO_SIZE	(256 * 1024)
#define SPHERE_SLICES		72
#define SPHERE_STACKS		56
#define PI					3.14159265358979323846f

#define LAB_FOV_DEG			36.0f
#define LAB_CAM_DIST		5.15f
#define LAB_LOOK_Y			-0.08f
#define LAB_CAM_Y			0.52f
/* Reference grey haze — flat cool charcoal void + GX fog behind sphere */
#define LAB_FOG_R			44
#define LAB_FOG_G			43
#define LAB_FOG_B			48
#define LAB_FOG_START		6.0f	/* past sphere front; thickens behind */
#define LAB_FOG_END			11.5f
#define LAB_FOG_START_DENSE	4.0f	/* Y-feature showcase denser */
#define LAB_FOG_END_DENSE	11.0f

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

enum {
	MAPS_NONE = 0,
	MAPS_RUST,
	MAPS_DAMASCUS,
	MAPS_CORRUGATED,
	MAPS_CLADDING,
	MAPS_STEEL,
	MAPS_CONCRETE,
	MAPS_BRICK,
	MAPS_WOOD,
	MAPS_PAINT,
	MAPS_SOLAR,
	MAPS_COUNT
};

typedef struct {
	const char *name;
	const char *subtitle;	/* gallery card line */
	GXColor base;
	f32 metallic;
	f32 roughness;
	int maps;	/* MAPS_* — imported PBR set, or NONE for tinted env */
} Material;

typedef struct {
	GXTexObj albedo, ao, metal, normal, rough;
	TPLFile tplA, tplAO, tplM, tplN, tplR;
} PbrSet;

static GXRModeObj *rmode = NULL;
static void *frameBuffer[2] = { NULL, NULL };

static GXTexObj texEnvSharp, texEnvMid, texEnvBlur;
static GXTexObj texChecker, texShadow;
static GXTexObj texHudTL, texHudFPS, texHudBottom;
static GXTexObj texFont, texDetail, texSpec;
static TPLFile tplEnvSharp, tplEnvMid, tplEnvBlur, tplChecker, tplShadow;
static TPLFile tplHudTL, tplHudFPS, tplHudBottom;
static TPLFile tplFont, tplDetail, tplSpec;
static PbrSet pbrSets[MAPS_COUNT];

static SphereVert sphereVerts[(SPHERE_STACKS + 1) * (SPHERE_SLICES + 1)];
static u16 sphereIdx[SPHERE_STACKS * SPHERE_SLICES * 6];
static int sphereIdxCount = 0;

/*
 * Curated A-cycle — TextureCom heroes first, then classic tinted metals.
 * Subtitles are gallery-card copy (Nintendo tech-demo tone).
 */
static Material materials[] = {
	{ "RUST",       "Oxidized iron - TextureCom PBR",       { 255, 255, 255, 255 }, 0.08f, 0.85f, MAPS_RUST },
	{ "COPPER",     "Warm metal - Fresnel env approx",      { 158, 132, 102, 255 }, 1.00f, 0.25f, MAPS_NONE },
	{ "DAMASCUS",   "Folded steel - rainbow anisotropy",    { 255, 255, 255, 255 }, 1.00f, 0.15f, MAPS_DAMASCUS },
	{ "CORRUGATED", "Weathered sheet - ridge normals",      { 255, 255, 255, 255 }, 0.90f, 0.55f, MAPS_CORRUGATED },
	{ "STEEL",      "Brushed stainless - mid gloss",        { 255, 255, 255, 255 }, 0.85f, 0.50f, MAPS_STEEL },
	{ "CLADDING",   "Architectural panel - soft metal",     { 255, 255, 255, 255 }, 0.75f, 0.28f, MAPS_CLADDING },
	{ "CONCRETE",   "Cast dielectric - high roughness",     { 255, 255, 255, 255 }, 0.00f, 0.85f, MAPS_CONCRETE },
	{ "BRICK",      "Fired clay - albedo + AO",             { 255, 255, 255, 255 }, 0.00f, 0.80f, MAPS_BRICK },
	{ "WOOD",       "End-grain timber - organic pore",      { 255, 255, 255, 255 }, 0.00f, 0.78f, MAPS_WOOD },
	{ "PAINT",      "Chipped enamel - clearcoat edge",      { 255, 255, 255, 255 }, 0.05f, 0.35f, MAPS_PAINT },
	{ "SOLAR",      "Photovoltaic grid - sharp glints",     { 255, 255, 255, 255 }, 0.95f, 0.28f, MAPS_SOLAR },
	{ "GOLD",       "Conductor tint - low roughness",       { 255, 220, 120, 255 }, 1.00f, 0.20f, MAPS_NONE },
	{ "IRON",       "Cool ferrous - env LOD mid",           { 180, 185, 195, 255 }, 1.00f, 0.40f, MAPS_NONE },
	{ "CHROME",     "Mirror metal - sharp env LOD",         { 235, 240, 255, 255 }, 1.00f, 0.08f, MAPS_NONE },
	{ "PLASTIC",    "Colored dielectric - soft highlight",  { 255, 110,  90, 255 }, 0.00f, 0.45f, MAPS_NONE },
};
static const int materialCount = (int)(sizeof(materials) / sizeof(materials[0]));
static int materialIndex = 1; /* open on COPPER — hero lighting reference */
static int envMapEnabled = 1;
static int featureIndex = FEAT_HERO_RUST_PBR;
static f32 roughnessOverride = -1.0f;
static f32 animTime = 0.0f;
static int galleryMode = 1;          /* 1 = presentation, 0 = engineer lab */
static int captureReel = 1;          /* dump-frame sequence; pad cancels */
static int autoShowcase = 1;         /* idle material parade after reel */
static f32 orbitYaw = 0.0f;
static f32 camPunch = 0.0f;          /* brief dolly on material change */
static f32 cardTimer = 0.0f;         /* material title-card seconds left */
static f32 bootTimer = 1.85f;        /* opening fade / title */
static f32 idleTimer = 0.0f;
static f32 fpsSmooth = 60.0f;

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
			/* Metals: full-sphere env; mild Fresnel (copper mid ~0.55–0.75) */
			f32 w = clampf(0.55f + 0.30f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		} else {
			/* Dielectrics / rust: Fresnel rim only, still roughness-scaled */
			f32 w = clampf(0.04f + 0.96f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		}
	}
}

/*
 * PBR maps: albedo × AO + env×Fresnel (rust dielectric or damascus metal).
 * TEX0 = albedo, TEX1 = AO, TEX2 = env matcap.
 */
static void setup_tev_hero_pbr(const Material *mat)
{
	f32 roughness = current_roughness(mat);
	PbrSet *set = &pbrSets[mat->maps];
	GXTexObj *albedo = &set->albedo;
	GXTexObj *ao = &set->ao;
	int metal = (mat->metallic > 0.5f);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_LoadTexObj(albedo, GX_TEXMAP0);
	GX_LoadTexObj(ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(ao, GX_REPEAT, GX_REPEAT);

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

	/* S2: metals tint env by albedo (rainbow bands); dielectrics just add env×A */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	if (envMapEnabled && metal) {
		/* (env * albedo) via: first need env in PREV... use env*A + albedo already in CPREV:
		 * for damascus: lerp(albedo, env, A) then we'd lose pattern — instead
		 * albedo + (env * A) with SCALE_2 on add for shine, pattern stays. */
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
		GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else if (envMapEnabled) {
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
		GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	} else {
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
		GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

/* Non-rust materials: tinted synthetic env matcap */
static void setup_tev_env_tint(const Material *mat)
{
	f32 roughness = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP0);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.85f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	/* env × metal tint, SCALE_2 for Flipper metal punch without Fresnel magenta */
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
	(void)mat;
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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP1);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);

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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_InitTexObjLOD(&pbrSets[MAPS_RUST].albedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.10f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);
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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_LoadTexObj(&texSpec, GX_TEXMAP1);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(&pbrSets[MAPS_RUST].albedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbrSets[MAPS_RUST].normal, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].normal, GX_REPEAT, GX_REPEAT);
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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(&pbrSets[MAPS_RUST].albedo, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
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
	GX_LoadTexObj(&pbrSets[MAPS_RUST].albedo, GX_TEXMAP0);
	GX_InitTexObjWrapMode(&pbrSets[MAPS_RUST].albedo, GX_REPEAT, GX_REPEAT);
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
		if (mat->maps != MAPS_NONE)
			setup_tev_hero_pbr(mat);
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

/*
 * Soft vertical haze card behind the sphere (z = -2.4). Drawn with fog off so
 * the reference gradient survives: darker crown → grey fog shelf → lighter
 * near the horizon. Floor/sphere keep GX fog so far checkers melt into haze.
 */
static void draw_backdrop(const Mtx view)
{
	Mtx model, modelview;
	const f32 z = -2.40f;
	const f32 x = 14.0f;
	const f32 y0 = -1.10f;
	const f32 y1 = 7.00f;
	const f32 y_lo = 0.15f;
	const f32 y_hi = 2.10f;
	/* Flat charcoal void — match reference mid-grey (~44,43,48) */
	GXColor top = { 42, 41, 46, 255 };
	GXColor hi = { 44, 43, 48, 255 };
	GXColor shelf = { 46, 45, 50, 255 };
	GXColor bot = { 45, 44, 49, 255 };

	guMtxIdentity(model);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0, 0, 0, 0});

	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 12);
		/* Crown → upper haze */
		GX_Position3f32(-x, y_hi, z); GX_Color4u8(hi.r, hi.g, hi.b, hi.a);
		GX_Position3f32( x, y_hi, z); GX_Color4u8(hi.r, hi.g, hi.b, hi.a);
		GX_Position3f32( x, y1, z);   GX_Color4u8(top.r, top.g, top.b, top.a);
		GX_Position3f32(-x, y1, z);   GX_Color4u8(top.r, top.g, top.b, top.a);
		/* Fog shelf behind the sphere */
		GX_Position3f32(-x, y_lo, z); GX_Color4u8(shelf.r, shelf.g, shelf.b, shelf.a);
		GX_Position3f32( x, y_lo, z); GX_Color4u8(shelf.r, shelf.g, shelf.b, shelf.a);
		GX_Position3f32( x, y_hi, z); GX_Color4u8(hi.r, hi.g, hi.b, hi.a);
		GX_Position3f32(-x, y_hi, z); GX_Color4u8(hi.r, hi.g, hi.b, hi.a);
		/* Near-horizon band */
		GX_Position3f32(-x, y0, z);   GX_Color4u8(bot.r, bot.g, bot.b, bot.a);
		GX_Position3f32( x, y0, z);   GX_Color4u8(bot.r, bot.g, bot.b, bot.a);
		GX_Position3f32( x, y_lo, z); GX_Color4u8(shelf.r, shelf.g, shelf.b, shelf.a);
		GX_Position3f32(-x, y_lo, z); GX_Color4u8(shelf.r, shelf.g, shelf.b, shelf.a);
	GX_End();
}

static void apply_scene_fog(int dense)
{
	GXColor fogc = { LAB_FOG_R, LAB_FOG_G, LAB_FOG_B, 255 };
	f32 start = dense ? LAB_FOG_START_DENSE : LAB_FOG_START;
	f32 end = dense ? LAB_FOG_END_DENSE : LAB_FOG_END;
	GX_SetFog(GX_FOG_PERSP_LIN, start, end, 0.1f, 100.0f, fogc);
}

static void draw_floor(const Mtx view)
{
	/*
	 * Vertex-colored plaza: even cell count so a checker CROSS sits on
	 * world origin under the sphere, with equal cells on every side.
	 */
	Mtx model, modelview;
	const f32 y = -1.12f;
	const f32 s = 6.0f;
	const int cells = 12; /* even → origin on a 4-way tile corner */
	const f32 cell = (2.0f * s) / (f32)cells;
	const GXColor dark = { 58, 48, 48, 255 };
	const GXColor mid = { 118, 102, 100, 255 };
	int ix, iz;
	u16 nverts;

	guMtxIdentity(model);
	guMtxConcat(view, model, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

	nverts = (u16)(cells * cells * 4);
	GX_Begin(GX_QUADS, GX_VTXFMT0, nverts);
	for (iz = 0; iz < cells; ++iz) {
		f32 z0 = -s + (f32)iz * cell;
		f32 z1 = z0 + cell;
		for (ix = 0; ix < cells; ++ix) {
			f32 x0 = -s + (f32)ix * cell;
			f32 x1 = x0 + cell;
			GXColor c = (((ix + iz) & 1) == 0) ? dark : mid;
			GX_Position3f32(x0, y, z0); GX_Color4u8(c.r, c.g, c.b, c.a);
			GX_Position3f32(x1, y, z0); GX_Color4u8(c.r, c.g, c.b, c.a);
			GX_Position3f32(x1, y, z1); GX_Color4u8(c.r, c.g, c.b, c.a);
			GX_Position3f32(x0, y, z1); GX_Color4u8(c.r, c.g, c.b, c.a);
		}
	}
	GX_End();
	(void)texChecker;
}

static void draw_shadow(const Mtx view)
{
	Mtx model, modelview;
	f32 y = -1.115f;
	/* Circular contact shadow centered under the sphere */
	f32 sx = 1.28f;
	f32 sz = 1.28f;

	guMtxIdentity(model);
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

static f32 ease_out_cubic(f32 t)
{
	t = clampf(t, 0.0f, 1.0f);
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static void hud_begin_color(void)
{
	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
}

static void hud_begin_tex(void)
{
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void hud_begin_tex_modulate(void)
{
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void draw_rect_rgba(f32 x0, f32 y0, f32 x1, f32 y1, u8 r, u8 g, u8 b, u8 a)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(x0, y0, 0); GX_Color4u8(r, g, b, a);
		GX_Position3f32(x1, y0, 0); GX_Color4u8(r, g, b, a);
		GX_Position3f32(x1, y1, 0); GX_Color4u8(r, g, b, a);
		GX_Position3f32(x0, y1, 0); GX_Color4u8(r, g, b, a);
	GX_End();
}

static void draw_char_col(f32 x, f32 y, char c, f32 scale, u8 r, u8 g, u8 b, u8 a)
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
		GX_Position3f32(x, y, 0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
		GX_Position3f32(x + w, y, 0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
		GX_Position3f32(x + w, y + h, 0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
		GX_Position3f32(x, y + h, 0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
	GX_End();
}

static void draw_text_col(f32 x, f32 y, const char *s, f32 scale, u8 r, u8 g, u8 b, u8 a)
{
	while (*s) {
		draw_char_col(x, y, *s, scale, r, g, b, a);
		x += 8.0f * scale;
		++s;
	}
}

static f32 text_width(const char *s, f32 scale)
{
	return (f32)strlen(s) * 8.0f * scale;
}

static void draw_text_centered(f32 cx, f32 y, const char *s, f32 scale, u8 r, u8 g, u8 b, u8 a)
{
	draw_text_col(cx - text_width(s, scale) * 0.5f, y, s, scale, r, g, b, a);
}

static void select_material(int index, int with_punch)
{
	if (index < 0) index = materialCount - 1;
	index %= materialCount;
	materialIndex = index;
	roughnessOverride = -1.0f;
	tint_sphere_vertices(&materials[materialIndex]);
	cardTimer = 3.4f;
	if (with_punch)
		camPunch = 1.0f;
}

static void draw_hud(f32 fb_w, f32 fb_h, const Material *mat)
{
	Mtx44 ortho;
	Mtx ident;
	char line[80];
	f32 rough = current_roughness(mat);
	f32 cardAlpha;
	u8 ca;
	f32 bootFade;

	guOrtho(ortho, 0, fb_h, 0, fb_w, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	guMtxIdentity(ident);
	GX_LoadPosMtxImm(ident, GX_PNMTX0);

	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0, 0, 0, 0});

	/* Soft letterbox vignette — gallery only */
	if (galleryMode) {
		hud_begin_color();
		draw_rect_rgba(0, 0, fb_w, 36.0f, 8, 8, 12, 110);
		draw_rect_rgba(0, fb_h - 52.0f, fb_w, fb_h, 8, 8, 12, 140);
	}

	hud_begin_tex();
	/* Logo + purple banner (always); FPS plate only in lab */
	draw_hud_uv(&texHudTL, 0, 0, 380, 120,
		0.0f, 0.0f, 380.0f / 512.0f, 120.0f / 256.0f);
	if (!galleryMode) {
		draw_hud_uv(&texHudFPS, 508, 4, 636, 40,
			0.0f, 0.0f, 1.0f, 36.0f / 64.0f);
	}
	draw_hud_uv(&texHudBottom, 0, 436, 640, 480,
		0.0f, 0.0f, 640.0f / 1024.0f, 44.0f / 64.0f);

	hud_begin_tex_modulate();
	GX_LoadTexObj(&texFont, GX_TEXMAP0);

	if (galleryMode) {
		/* Live FPS, quiet */
		snprintf(line, sizeof(line), "FPS %.0f", fpsSmooth);
		draw_text_col(548, 14, line, 1.0f, 210, 210, 220, 200);

		/* Material title card — fades after change (hidden under boot veil) */
		cardAlpha = (bootTimer > 0.8f) ? 0.0f :
			(cardTimer > 1.0f ? 1.0f : ease_out_cubic(cardTimer));
		ca = (u8)(cardAlpha * 255.0f);
		if (ca > 8) {
			hud_begin_color();
			draw_rect_rgba(70, 318, 570, 402, 10, 10, 16, (u8)(ca * 0.72f));
			/* Accent hairline */
			draw_rect_rgba(70, 318, 570, 321, 120, 90, 210, ca);
			hud_begin_tex_modulate();
			GX_LoadTexObj(&texFont, GX_TEXMAP0);
			draw_text_centered(320, 330, mat->name, 2.0f, 255, 255, 255, ca);
			draw_text_centered(320, 366, mat->subtitle, 1.0f, 200, 198, 210, ca);
			snprintf(line, sizeof(line), "METAL %.2f    ROUGH %.2f",
				mat->metallic, rough);
			draw_text_centered(320, 384, line, 1.0f, 170, 168, 185, ca);
		}

		/* Quiet mode hint */
		draw_text_col(12, 112, galleryMode ? "GALLERY" : "LAB", 1.0f,
			180, 170, 210, 180);
		snprintf(line, sizeof(line), "%s  -  START lab",
			envMapEnabled ? "ENV ON" : "ENV OFF");
		draw_text_col(12, 128, line, 1.0f, 150, 150, 165, 160);
	} else {
		/* Engineer lab — full Flipper status */
		snprintf(line, sizeof(line), "MATERIAL: %s", mat->name);
		draw_text_col(12, 112, line, 1.0f, 255, 255, 255, 255);
		snprintf(line, sizeof(line), "METALLIC %.2f   ROUGHNESS %.2f",
			mat->metallic, rough);
		draw_text_col(12, 130, line, 1.0f, 255, 255, 255, 255);
		if (mat->maps != MAPS_NONE) {
			snprintf(line, sizeof(line), "ENV %s | %s",
				envMapEnabled ? "ON " : "OFF", featureNames[featureIndex]);
			draw_text_col(12, 148, line, 1.0f, 255, 255, 255, 255);
			snprintf(line, sizeof(line), "%s (B steps rough)", env_lod_name(rough));
			draw_text_col(12, 166, line, 1.0f, 255, 255, 255, 255);
			snprintf(line, sizeof(line), "MAPS: %s albedo ao metal normal rough",
				mat->name);
			draw_text_col(12, 184, line, 1.0f, 255, 255, 255, 255);
		} else if (featureIndex == FEAT_HERO_RUST_PBR || featureIndex == FEAT_FRESNEL_KONST) {
			draw_text_col(12, 148, "ENV MAP + FRESNEL APPROX", 1.0f, 255, 255, 255, 255);
		} else {
			snprintf(line, sizeof(line), "ENV %s | %s",
				envMapEnabled ? "ON " : "OFF", featureNames[featureIndex]);
			draw_text_col(12, 148, line, 1.0f, 255, 255, 255, 255);
		}
		draw_text_col(12, 202, "START: gallery   Z: auto", 1.0f, 180, 180, 190, 220);
		snprintf(line, sizeof(line), "FPS %.1f", fpsSmooth);
		draw_text_col(520, 14, line, 1.0f, 230, 230, 240, 255);
	}

	/* Boot title fade */
	if (bootTimer > 0.0f) {
		bootFade = bootTimer > 2.4f ? 1.0f :
			(bootTimer > 0.6f ? 1.0f : ease_out_cubic(bootTimer / 0.6f));
		ca = (u8)(clampf(bootFade, 0.0f, 1.0f) * 255.0f);
		hud_begin_color();
		/* Full veil early, then soft card */
		if (bootTimer > 2.4f) {
			u8 veil = (u8)(((bootTimer - 2.4f) / 0.8f) * 255.0f);
			draw_rect_rgba(0, 0, fb_w, fb_h, 6, 6, 10, veil);
		}
		draw_rect_rgba(90, 190, 550, 290, 8, 8, 14, (u8)(ca * 0.85f));
		draw_rect_rgba(90, 190, 550, 194, 130, 100, 220, ca);
		hud_begin_tex_modulate();
		GX_LoadTexObj(&texFont, GX_TEXMAP0);
		draw_text_centered(320, 208, "NINTENDO GAMECUBE", 1.0f, 200, 190, 230, ca);
		draw_text_centered(320, 232, "GX / TEV MATERIAL LAB", 1.5f, 255, 255, 255, ca);
		draw_text_centered(320, 262, "Flipper shader exploration", 1.0f, 180, 178, 195, ca);
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


#define LOAD_PBR_SET(slot, slug) do { \
	TPL_OpenTPLFromMemory(&pbrSets[slot].tplA, (void *)slug##_albedo_tpl, slug##_albedo_tpl_size); \
	TPL_GetTexture(&pbrSets[slot].tplA, slug##_albedo, &pbrSets[slot].albedo); \
	TPL_OpenTPLFromMemory(&pbrSets[slot].tplAO, (void *)slug##_ao_tpl, slug##_ao_tpl_size); \
	TPL_GetTexture(&pbrSets[slot].tplAO, slug##_ao, &pbrSets[slot].ao); \
	TPL_OpenTPLFromMemory(&pbrSets[slot].tplM, (void *)slug##_metallic_tpl, slug##_metallic_tpl_size); \
	TPL_GetTexture(&pbrSets[slot].tplM, slug##_metallic, &pbrSets[slot].metal); \
	TPL_OpenTPLFromMemory(&pbrSets[slot].tplN, (void *)slug##_normal_tpl, slug##_normal_tpl_size); \
	TPL_GetTexture(&pbrSets[slot].tplN, slug##_normal, &pbrSets[slot].normal); \
	TPL_OpenTPLFromMemory(&pbrSets[slot].tplR, (void *)slug##_roughness_tpl, slug##_roughness_tpl_size); \
	TPL_GetTexture(&pbrSets[slot].tplR, slug##_roughness, &pbrSets[slot].rough); \
} while (0)

	LOAD_PBR_SET(MAPS_RUST, rust);
	LOAD_PBR_SET(MAPS_DAMASCUS, damascus);
	LOAD_PBR_SET(MAPS_CORRUGATED, corrugated);
	LOAD_PBR_SET(MAPS_CLADDING, cladding);
	LOAD_PBR_SET(MAPS_STEEL, steel);
	LOAD_PBR_SET(MAPS_CONCRETE, concrete);
	LOAD_PBR_SET(MAPS_BRICK, brick);
	LOAD_PBR_SET(MAPS_WOOD, wood);
	LOAD_PBR_SET(MAPS_PAINT, paint);
	LOAD_PBR_SET(MAPS_SOLAR, solar);

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
	GXColor background = { LAB_FOG_R, LAB_FOG_G, LAB_FOG_B, 0xff };
	Mtx view, model, modelview, rot;
	Mtx44 perspective;
	guVector cam, up, look;
	u16 prevButtons = 0;
	u32 frame = 0;
	f32 dt = 1.0f / 60.0f;

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
	select_material(1, 0); /* COPPER open */
	cardTimer = 4.0f;

	guPerspective(perspective, LAB_FOV_DEG,
		(f32)rmode->fbWidth / (f32)rmode->efbHeight, 0.1f, 100.0f);

	while (1) {
		Material *mat;
		u16 down, held;
		f32 dist, yawRad, punch;
		int stickX;

		PAD_ScanPads();
		down = PAD_ButtonsDown(0);
		held = PAD_ButtonsHeld(0);
		stickX = PAD_StickX(0);

		/* Any intentional input cancels capture reel — Tim stays in control */
		if (down & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
				PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R |
				PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
			captureReel = 0;
			idleTimer = 0.0f;
		}
		if (stickX > 18 || stickX < -18) {
			captureReel = 0;
			idleTimer = 0.0f;
		}

		if ((down & PAD_BUTTON_A) && !(prevButtons & PAD_BUTTON_A))
			select_material(materialIndex + 1, 1);
		if ((down & PAD_BUTTON_LEFT) || (down & PAD_TRIGGER_L))
			select_material(materialIndex - 1, 1);
		if ((down & PAD_BUTTON_RIGHT) || (down & PAD_TRIGGER_R))
			select_material(materialIndex + 1, 1);
		if ((down & PAD_BUTTON_B) && !(prevButtons & PAD_BUTTON_B)) {
			f32 r = current_roughness(&materials[materialIndex]);
			r += 0.25f;
			if (r > 1.01f) r = 0.0f;
			roughnessOverride = r;
			tint_sphere_vertices(&materials[materialIndex]);
			cardTimer = 2.0f;
		}
		if ((down & PAD_BUTTON_X) && !(prevButtons & PAD_BUTTON_X))
			envMapEnabled ^= 1;
		if ((down & PAD_BUTTON_Y) && !(prevButtons & PAD_BUTTON_Y))
			featureIndex = (featureIndex + 1) % FEAT_COUNT;
		if ((down & PAD_BUTTON_START) && !(prevButtons & PAD_BUTTON_START))
			galleryMode ^= 1;
		if ((down & PAD_TRIGGER_Z) && !(prevButtons & PAD_TRIGGER_Z))
			autoShowcase ^= 1;

		prevButtons = held;
		mat = &materials[materialIndex];
		animTime += dt;
		if (bootTimer > 0.0f) bootTimer -= dt;
		if (cardTimer > 0.0f) cardTimer -= dt;
		if (camPunch > 0.0f) {
			camPunch -= dt * 1.8f;
			if (camPunch < 0.0f) camPunch = 0.0f;
		}
		idleTimer += dt;
		fpsSmooth = fpsSmooth * 0.92f + 60.0f * 0.08f;

		/* Stick orbits; slow drift only during idle auto-showcase */
		if (stickX > 18 || stickX < -18)
			orbitYaw += (f32)stickX * dt * 0.040f;
		else if (autoShowcase && galleryMode && idleTimer > 7.5f && bootTimer <= 0.0f)
			orbitYaw += dt * 0.06f;

		/*
		 * Capture reel (DumpFrames / CI proofs) — only while captureReel:
		 *  0–90    COPPER @ R=0.25
		 *  100–160 TextureCom heroes
		 *  170–450 CHROME roughness 0→1
		 */
		if (captureReel) {
			if (frame == 0 || frame == 1) {
				select_material(1, 0);
				featureIndex = FEAT_HERO_RUST_PBR;
				roughnessOverride = 0.25f;
				tint_sphere_vertices(&materials[materialIndex]);
				cardTimer = 2.5f;
			}
			if (frame == 100) select_material(2, 1);   /* DAMASCUS */
			if (frame == 130) select_material(3, 1);   /* CORRUGATED */
			if (frame == 150) select_material(10, 1);  /* SOLAR */
			if (frame == 170) {
				select_material(13, 1); /* CHROME */
				roughnessOverride = 0.0f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame >= 170 && frame < 470 && ((frame - 170) % 60) == 0) {
				int step = (int)((frame - 170) / 60);
				static const f32 steps[5] = { 0.00f, 0.25f, 0.50f, 0.75f, 1.00f };
				if (step < 5) {
					roughnessOverride = steps[step];
					tint_sphere_vertices(&materials[materialIndex]);
					cardTimer = 1.6f;
				}
			}
			if (frame == 470)
				captureReel = 0;
			mat = &materials[materialIndex];
		} else if (autoShowcase && galleryMode && idleTimer > 7.5f && bootTimer <= 0.0f) {
			/* Idle parade — one material every ~4.5s */
			if (((int)(idleTimer * 60.0f) % 270) == 0)
				select_material(materialIndex + 1, 1);
			mat = &materials[materialIndex];
		}
		++frame;

		/* Camera: soft yaw orbit about sphere center + punch dolly */
		yawRad = orbitYaw;
		punch = ease_out_cubic(camPunch) * 0.22f;
		dist = LAB_CAM_DIST - punch;
		cam.x = sinf(yawRad) * dist;
		cam.y = LAB_CAM_Y + sinf(animTime * 0.35f) * 0.03f;
		cam.z = cosf(yawRad) * dist;
		up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
		look.x = 0.0f; look.y = LAB_LOOK_Y; look.z = 0.0f;
		guLookAt(view, &cam, &up, &look);

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

		draw_backdrop(view);
		apply_scene_fog(featureIndex == FEAT_FOG_HAZE);
		draw_floor(view);
		draw_shadow(view);

		/* Sphere: subtle authored spin so normals / anisotropy read */
		guMtxIdentity(model);
		guMtxRotRad(rot, 'y', animTime * 0.18f + camPunch * 0.55f);
		guMtxConcat(model, rot, model);
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
