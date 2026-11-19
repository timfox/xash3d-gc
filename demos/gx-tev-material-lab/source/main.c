/*---------------------------------------------------------------------------------
	GX / TEV Material Lab — Flipper shader exploration tech demo

	Production presentation: gallery title cards, idle camera orbit, soft
	material transitions. START toggles Gallery ↔ Lab (engineer HUD).
	Any pad input cancels the capture reel so the player stays in control.

	Hero maps: real TextureCom / Desktop PBR sets (albedo AO metal normal rough).
	D-Pad UP cycles HDRI skyboxes (gxbake equirect→faces + IBL matcaps).
	D-Pad DOWN toggles mirror floor; L toggles bloom glow; Z toggles fog FX.
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
#include "stone_albedo_tpl.h"
#include "stone_albedo.h"
#include "stone_ao_tpl.h"
#include "stone_ao.h"
#include "stone_metallic_tpl.h"
#include "stone_metallic.h"
#include "stone_normal_tpl.h"
#include "stone_normal.h"
#include "stone_roughness_tpl.h"
#include "stone_roughness.h"
#include "height_stone_tpl.h"
#include "height_stone.h"
#include "height_brick_tpl.h"
#include "height_brick.h"
#include "height_concrete_tpl.h"
#include "height_concrete.h"
#include "lab_sky_tex.h"

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
	FEAT_INDTEX_BUMP,	/* IndTex normal → env warp (active material) */
	FEAT_HW_KEY,		/* GX_LIGHT0 key diffuse × albedo (diffuse-only A/B) */
	FEAT_HW_FILL,		/* key + cool fill */
	FEAT_HW_RIM,		/* key + rim backlight */
	FEAT_EMBOSS_LIT,	/* Lost Coast dual-sample height emboss under HW key */
	FEAT_HW_SPEC,		/* GX_AF_SPEC lobe; shininess from roughness */
	/* --- 2026-style Flipper approximations (still pure TEV / IndTex / HW lights) --- */
	FEAT_CLEARCOAT,		/* dual-lobe: base albedo + sharp clearcoat env */
	FEAT_IRIDESCENT,	/* thin-film: animated hue-tint × IBL */
	FEAT_SHEEN_CLOTH,	/* soft wrap diffuse + edge sheen env */
	FEAT_SSS_WRAP,		/* subsurface wrap (lit + ADDHALF ambient) */
	FEAT_HEAT_WARP,		/* IndTex animated heat-haze warp of IBL */
	FEAT_TOON_RAMP,		/* stylized NPR: hard lit + rim (cel look) */
	FEAT_HOLOGRAM,		/* futuristic scan + Fresnel cyan shell */
	FEAT_ANISO_FLAKE,	/* stretched IndTex flake in sharp IBL */
	FEAT_CHROMA_IBL,	/* chromatic IBL: RGB-split env via IndTex */
	FEAT_GLASS_REFRACT,	/* glass: IndTex refraction warp + Fresnel coat */
	FEAT_NEON_EMISSIVE,	/* neon / emissive: additive glow konst + soft IBL */
	FEAT_PARALLAX_LITE,	/* parallax-ish: dual height offset on albedo */
	FEAT_CAR_PAINT,		/* MatCap car paint: mid body + sharp flake coat */
	FEAT_CRT_SCAN,		/* CRT scanline overlay × albedo */
	FEAT_FORCE_FIELD,	/* energy shield: inverse-ish Fresnel rim shell */
	FEAT_WET_SURFACE,	/* rain-wet: darkened albedo + sharp Fresnel glaze */
	FEAT_OXIDE_PATINA,	/* weathered: detail patina × albedo + metal glints */
	FEAT_PORCELAIN,		/* ceramic: soft wrap body + sharp dielectric coat */
	FEAT_GOLD_LEAF,		/* gilded foil: warm gold × metal map + sharp IndTex */
	FEAT_FROST_ICE,		/* frost: cool tint + crystalline Fresnel coat */
	FEAT_CAUSTICS,		/* underwater caustics: scrolled light pattern × PBR */
	FEAT_VELVET,		/* nap cloth: soft wrap body + rim sheen IndTex */
	FEAT_CARBON,		/* carbon fiber: dual-scale weave × metal IndTex */
	FEAT_PEARL,		/* pearl / nacre: soft body + iridescent Fresnel coat */
	FEAT_CONCRETE,		/* cast concrete: cool aggregate + rough IndTex */
	FEAT_WOOD_PLANK,	/* wood plank: warm grain scroll + soft sheen */
	FEAT_BRICK,		/* fired brick: clay tint + mortar AO + IndTex */
	FEAT_CERAMIC_TILE,	/* ceramic tile: grid glaze + sharp dielectric coat */
	FEAT_OLED_VOID,		/* near-black OLED: dense IndTex × dual dark KONST */
	FEAT_ANODIZED_MG,	/* Mg anodize: scrolled IndTex × dual cool-green KONST */
	FEAT_FROST_POLY,	/* frost polycarb: open IndTex × dual cool KONST */
	FEAT_HOLO_VINYL,	/* holo vinyl: scrolled IndTex × dual chroma KONST */
	FEAT_TUNGSTEN_FOAM,	/* tungsten foam: open IndTex × dual cool gray KONST */
	FEAT_ALGAE_GLASS,	/* algae glass: scrolled IndTex × dual kelp KONST */
	FEAT_OBSIDIAN_WOOL,	/* obsidian wool: dense IndTex × dual deep KONST */
	FEAT_TOPAZ_DUST,	/* topaz dust: fine IndTex × dual amber KONST */
	FEAT_MOD2X_DETAIL,
	FEAT_SPEC_MASK,
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
	MAPS_STONE,
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
static GXTexObj texHeightStone, texHeightBrick, texHeightConcrete;
static TPLFile tplEnvSharp, tplEnvMid, tplEnvBlur, tplChecker, tplShadow;
static TPLFile tplHudTL, tplHudFPS, tplHudBottom;
static TPLFile tplFont, tplDetail, tplSpec;
static TPLFile tplHeightStone, tplHeightBrick, tplHeightConcrete;
static PbrSet pbrSets[MAPS_COUNT];

/* HDRI skybox + IBL matcaps (gxbake equirect → 512² sky / 256² IBL CMPR). */
#define LAB_SKY_COUNT 3
#define LAB_SKY_SIDES 6
typedef struct {
	const char *name;
	GXTexObj face[LAB_SKY_SIDES]; /* rt bk lf ft up dn */
	GXTexObj iblSharp, iblMid, iblBlur;
	TPLFile tplFace[LAB_SKY_SIDES];
	TPLFile tplIblS, tplIblM, tplIblB;
} SkyPack;
static SkyPack skyPacks[LAB_SKY_COUNT];
static int skyIndex = 0;
static int mirrorFloor = 0; /* off — flipped sphere read as a second shell */
static int bloomEnabled = 0; /* off — outer shell read as a force field */
static int fogFxEnabled = 0;

/* Lost Coast cliff emboss: sun-tangent UV epsilon (see r_gx_worlddl.c). */
#define LAB_EMBOSS_EPS		(6.0f / 64.0f)
static const f32 lab_emboss_ds = -0.7206f * LAB_EMBOSS_EPS;
static const f32 lab_emboss_dt = -0.6716f * LAB_EMBOSS_EPS;

static SphereVert sphereVerts[(SPHERE_STACKS + 1) * (SPHERE_SLICES + 1)];
static u16 sphereIdx[SPHERE_STACKS * SPHERE_SLICES * 6];
static int sphereIdxCount = 0;

/*
 * A-cycle = TextureCom PBR sets only. Every material ships albedo + AO +
 * metallic + normal + roughness maps (no tinted MAPS_NONE stand-ins).
 */
static Material materials[] = {
	{ "RUST",       "Oxidized iron - full PBR maps",        { 255, 255, 255, 255 }, 0.08f, 0.85f, MAPS_RUST },
	{ "DAMASCUS",   "Folded steel - metal/normal/rough",    { 255, 255, 255, 255 }, 1.00f, 0.15f, MAPS_DAMASCUS },
	{ "CORRUGATED", "Weathered sheet - ridge normals",      { 255, 255, 255, 255 }, 0.90f, 0.55f, MAPS_CORRUGATED },
	{ "STEEL",      "Brushed stainless - mid gloss",        { 255, 255, 255, 255 }, 0.85f, 0.50f, MAPS_STEEL },
	{ "CLADDING",   "Architectural panel - soft metal",     { 255, 255, 255, 255 }, 0.75f, 0.28f, MAPS_CLADDING },
	{ "CONCRETE",   "Cast dielectric - metal/normal/rough", { 255, 255, 255, 255 }, 0.00f, 0.85f, MAPS_CONCRETE },
	{ "BRICK",      "Fired clay - metal/normal/rough",      { 255, 255, 255, 255 }, 0.00f, 0.80f, MAPS_BRICK },
	{ "WOOD",       "End-grain timber - metal/normal/rough",{ 255, 255, 255, 255 }, 0.00f, 0.78f, MAPS_WOOD },
	{ "PAINT",      "Automotive clearcoat - dual lobe",     { 255, 255, 255, 255 }, 0.05f, 0.35f, MAPS_PAINT },
	{ "SOLAR",      "Photovoltaic grid - sharp glints",     { 255, 255, 255, 255 }, 0.95f, 0.28f, MAPS_SOLAR },
	{ "STONE",      "Carved wall - metal/normal/rough",     { 255, 255, 255, 255 }, 0.00f, 0.82f, MAPS_STONE },
	/* Named Flipper-path cards (reuse TCom maps; material title matches Y path) */
	{ "GOLD LEAF",  "Gilded foil - metal/normal/rough",     { 255, 255, 255, 255 }, 1.00f, 0.18f, MAPS_DAMASCUS },
	{ "FROST ICE",  "Crystalline frost - metal/normal/rough",{ 255, 255, 255, 255 }, 0.00f, 0.30f, MAPS_STONE },
	{ "CAUSTICS",   "Underwater pools - metal/normal/rough",{ 255, 255, 255, 255 }, 0.00f, 0.45f, MAPS_BRICK },
	{ "VELVET",     "Nap cloth - metal/normal/rough",       { 255, 255, 255, 255 }, 0.00f, 0.75f, MAPS_WOOD },
	{ "CARBON",     "Carbon weave - metal/normal/rough",    { 255, 255, 255, 255 }, 0.95f, 0.35f, MAPS_CORRUGATED },
	{ "PEARL",      "Nacre coat - metal/normal/rough",      { 255, 255, 255, 255 }, 0.15f, 0.28f, MAPS_PAINT },
	{ "CONCRETE",   "Cast aggregate - metal/normal/rough",  { 255, 255, 255, 255 }, 0.00f, 0.85f, MAPS_CONCRETE },
	{ "WOOD PLANK", "Plank grain - metal/normal/rough",     { 255, 255, 255, 255 }, 0.00f, 0.78f, MAPS_WOOD },
	{ "BRICK",      "Fired clay - metal/normal/rough",      { 255, 255, 255, 255 }, 0.00f, 0.80f, MAPS_BRICK },
	{ "CERAMIC TILE","Glazed tile - metal/normal/rough",    { 255, 255, 255, 255 }, 0.00f, 0.28f, MAPS_STONE },
	{ "OLED VOID",  "Near-black OLED - metal/normal/rough",{ 255, 255, 255, 255 }, 0.20f, 0.25f, MAPS_STEEL },
	{ "ANODIZED MG","Dyed Mg metal - metal/normal/rough",  { 255, 255, 255, 255 }, 0.92f, 0.40f, MAPS_CLADDING },
	{ "FROST POLY", "Frosted polycarb - metal/normal/rough",{ 255, 255, 255, 255 }, 0.00f, 0.58f, MAPS_STONE },
	{ "HOLO VINYL", "Peel holo vinyl - metal/normal/rough",{ 255, 255, 255, 255 }, 0.80f, 0.22f, MAPS_DAMASCUS },
	{ "TUNGSTEN FOAM","Dense porous W - metal/normal/rough",{ 255, 255, 255, 255 }, 0.88f, 0.55f, MAPS_STEEL },
	{ "ALGAE GLASS","Kelp translucency - metal/normal/rough",{ 255, 255, 255, 255 }, 0.00f, 0.42f, MAPS_STONE },
	{ "OBSIDIAN WOOL","Near-black loft - metal/normal/rough",{ 255, 255, 255, 255 }, 0.15f, 0.72f, MAPS_CORRUGATED },
	{ "TOPAZ DUST","Warm gold grit - metal/normal/rough",{ 255, 255, 255, 255 }, 0.70f, 0.48f, MAPS_DAMASCUS },
};
static const int materialCount = (int)(sizeof(materials) / sizeof(materials[0]));
static int materialIndex = 0; /* open on RUST — full map set */
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
static Mtx s_view; /* latest look-at; HW lights in view space */

static const char *featureNames[FEAT_COUNT] = {
	"PBR MAPS + ENV",
	"FRESNEL+KONST",
	"NORMAL INDTEX",
	"HW KEY LIGHT",
	"HW KEY+FILL",
	"HW KEY+RIM",
	"EMBOSS LIT",
	"HW SPEC",
	"CLEARCOAT",
	"IRIDESCENT",
	"SHEEN CLOTH",
	"SSS WRAP",
	"HEAT WARP",
	"TOON RAMP",
	"HOLOGRAM",
	"ANISO FLAKE",
	"CHROMA IBL",
	"GLASS REFRACT",
	"NEON EMISSIVE",
	"PARALLAX LITE",
	"CAR PAINT",
	"CRT SCAN",
	"FORCE FIELD",
	"WET SURFACE",
	"OXIDE PATINA",
	"PORCELAIN",
	"GOLD LEAF",
	"FROST ICE",
	"CAUSTICS",
	"VELVET",
	"CARBON",
	"PEARL",
	"CONCRETE",
	"WOOD PLANK",
	"BRICK",
	"CERAMIC TILE",
	"OLED VOID",
	"ANODIZED MG",
	"FROST POLY",
	"HOLO VINYL",
	"TUNGSTEN FOAM",
	"ALGAE GLASS",
	"OBSIDIAN WOOL",
	"TOPAZ DUST",
	"MOD2X DETAIL",
	"SPEC MASK ADD",
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

/* Discrete Flipper env LODs — active HDRI IBL matcaps (sharp/mid/blur). */
static GXTexObj *select_env_tex(f32 roughness)
{
	SkyPack *sky = &skyPacks[skyIndex];
	if (roughness < 0.35f)
		return &sky->iblSharp;
	if (roughness < 0.70f)
		return &sky->iblMid;
	return &sky->iblBlur;
}

static const char *env_lod_name(f32 roughness)
{
	if (roughness < 0.35f)
		return "IBL LOD SHARP";
	if (roughness < 0.70f)
		return "IBL LOD MID";
	return "IBL LOD BLUR";
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
			/* Metals: flat/low Fresnel — matcap limbs must not read as a glowing rim */
			f32 w = clampf(0.10f + 0.22f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		} else {
			/* Dielectrics / rust: Fresnel rim only, still roughness-scaled */
			f32 w = clampf(0.04f + 0.96f * v->fu, 0.0f, 1.0f) * spec_scale;
			v->a = (u8)(w * 255.0f);
		}
	}
}

/*
 * Hero PBR: albedo × AO + metal map + roughness map + normal-IndTex env.
 * Conductors: env × metal map. Dielectrics: env × Fresnel. Roughness picks
 * env LOD (sharp/mid/blur) and attenuates metal weight via (1−rough)×metal.
 * TEX0=albedo TEX1=AO TEX2=env TEX3=normal(ind) TEX4=metal TEX5=rough.
 */
static void setup_tev_hero_pbr(const Material *mat)
{
	f32 roughness = current_roughness(mat);
	PbrSet *set = &pbrSets[mat->maps];
	int metal = (mat->metallic > 0.5f);
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(4);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.55f;
	im[1][1] = 0.55f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_LoadTexObj(&set->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&set->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_LoadTexObj(&set->normal, GX_TEXMAP3);
	GX_LoadTexObj(&set->metal, GX_TEXMAP4);
	GX_LoadTexObj(&set->rough, GX_TEXMAP5);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);
	GX_InitTexObjWrapMode(&set->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&set->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&set->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&set->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&set->rough, GX_REPEAT, GX_REPEAT);

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: albedo */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* st1: × AO; Schlick Fresnel in vertex alpha */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: metal map → REG0; metals also fold quiet Fresnel (APREV) into weight.
	 * REG0 = metal × KONST(~0.40) so limbs stay shy of a glowing rim. */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	if (metal) {
		GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 100, 100, 100, 255 });
		GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	} else {
		GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	}
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	(void)set->rough; /* loaded above; LOD via select_env_tex(roughness) */

	/* st3: normal-warped env × (metal×KONST×Fresnel | Fresnel) + body */
	if (envMapEnabled) {
		GX_SetTevIndirect(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
			GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
		GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
		if (metal) {
			/* env × min(C0, APREV) approx: use APREV (quiet Fresnel) — metal in REG0
			 * already scaled; multiply env by APREV so limb tracks Fresnel not full map */
			GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
		} else {
			GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
		}
		/* DIVIDE_2 + quiet Fresnel/KONST: steel/damascus limbs no longer glow */
		GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	} else {
		GX_SetTevDirect(GX_TEVSTAGE3);
		GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
		GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE3, GX_TEV_KASEL_1);
	/* rough map still loaded — drives LOD via select_env_tex; visible with B */
	(void)roughness;
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


static PbrSet *active_pbr(const Material *mat)
{
	if (mat->maps > MAPS_NONE && mat->maps < MAPS_COUNT)
		return &pbrSets[mat->maps];
	return &pbrSets[MAPS_RUST];
}

/* World → view (s_view). Specular dirs use the 3×3 only. */
static void view_xform_pos(f32 x, f32 y, f32 z, f32 *ox, f32 *oy, f32 *oz)
{
	*ox = s_view[0][0] * x + s_view[0][1] * y + s_view[0][2] * z + s_view[0][3];
	*oy = s_view[1][0] * x + s_view[1][1] * y + s_view[1][2] * z + s_view[1][3];
	*oz = s_view[2][0] * x + s_view[2][1] * y + s_view[2][2] * z + s_view[2][3];
}

static void view_xform_dir(f32 x, f32 y, f32 z, f32 *ox, f32 *oy, f32 *oz)
{
	*ox = s_view[0][0] * x + s_view[0][1] * y + s_view[0][2] * z;
	*oy = s_view[1][0] * x + s_view[1][1] * y + s_view[1][2] * z;
	*oz = s_view[2][0] * x + s_view[2][1] * y + s_view[2][2] * z;
}

/* mode: 0=key, 1=key+fill, 2=key+rim — lights in view space.
 * with_spec: GX_AF_SPEC + shininess from roughness (FEAT_HW_SPEC). */
static void load_hw_lights(int mode, int with_spec, f32 roughness)
{
	GXLightObj lo;
	GXColor amb = { 28, 28, 38, 255 };
	GXColor matc = { 255, 255, 255, 255 };
	GXColor key = { 255, 236, 210, 255 };
	GXColor fill = { 72, 88, 140, 255 };
	GXColor rim = { 160, 190, 255, 255 };
	/* Key — upper-right front (matches emboss sun offset) */
	const f32 kx = 3.5f, ky = 4.5f, kz = 5.0f;
	f32 px, py, pz, dx, dy, dz, klen, shininess;

	GX_SetChanAmbColor(GX_COLOR0A0, amb);
	GX_SetChanMatColor(GX_COLOR0A0, matc);

	GX_InitLightColor(&lo, key);
	if (with_spec) {
		/* Roughness 0 → tight lobe; 1 → broad / dim (min shininess ~4). */
		shininess = 4.0f + (1.0f - clampf(roughness, 0.0f, 1.0f)) * 60.0f;
		view_xform_dir(kx, ky, kz, &dx, &dy, &dz);
		klen = sqrtf(dx * dx + dy * dy + dz * dz);
		if (klen < 1e-4f) klen = 1.0f;
		/* Specular parallel light only — InitLightPos would destroy this. */
		GX_InitSpecularDir(&lo, dx / klen, dy / klen, dz / klen);
		GX_InitLightShininess(&lo, shininess);
	} else {
		view_xform_pos(kx, ky, kz, &px, &py, &pz);
		GX_InitLightPos(&lo, px, py, pz);
		GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	}
	GX_LoadLightObj(&lo, GX_LIGHT0);

	if (mode >= 1) {
		view_xform_pos(-4.0f, 1.0f, 3.0f, &px, &py, &pz);
		GX_InitLightPos(&lo, px, py, pz);
		GX_InitLightColor(&lo, fill);
		GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
		GX_LoadLightObj(&lo, GX_LIGHT1);
	} else {
		GX_InitLightColor(&lo, (GXColor){0, 0, 0, 255});
		GX_LoadLightObj(&lo, GX_LIGHT1);
	}

	if (mode >= 2) {
		view_xform_pos(-1.5f, 2.5f, -5.5f, &px, &py, &pz);
		GX_InitLightPos(&lo, px, py, pz);
		GX_InitLightColor(&lo, rim);
		GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
		GX_LoadLightObj(&lo, GX_LIGHT2);
	} else {
		GX_InitLightColor(&lo, (GXColor){0, 0, 0, 255});
		GX_LoadLightObj(&lo, GX_LIGHT2);
	}
}

static GXTexObj *select_height_tex(const Material *mat)
{
	switch (mat->maps) {
	case MAPS_BRICK:    return &texHeightBrick;
	case MAPS_CONCRETE: return &texHeightConcrete;
	case MAPS_STONE:
	default:            return &texHeightStone;
	}
}

static void setup_tev_hw_lit(const Material *mat, int mode)
{
	PbrSet *pbr = active_pbr(mat);
	u32 lights = GX_LIGHT0;

	if (mode >= 1) lights |= GX_LIGHT1;
	if (mode >= 2) lights |= GX_LIGHT2;

	load_hw_lights(mode, 0, current_roughness(mat));

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		lights, GX_DF_CLAMP, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		/* tinted materials: solid via env mid as cheap albedo stand-in */
		GX_LoadTexObj(&texEnvMid, GX_TEXMAP0);
	}

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
}

/*
 * FEAT_HW_SPEC — hardware specular channel (GX_DF_CLAMP + GX_AF_SPEC).
 * Shininess from roughness; clearly brighter lobe than diffuse-only HW KEY.
 */
static void setup_tev_hw_spec(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 roughness = current_roughness(mat);

	load_hw_lights(0, 1, roughness);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	/* Specular-only light → DF_NONE + AF_SPEC (actor pattern). */
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0, GX_DF_NONE, GX_AF_SPEC);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(&texEnvMid, GX_TEXMAP0);
	}

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	/* SCALE_2 so the roughness→shininess lobe reads hot on chrome/damascus */
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
}

/*
 * FEAT_EMBOSS_LIT — Lost Coast cliff emboss under HW key light.
 * lit × (0.5 + h(uv) − h(uv+d)) × 2  (r_gx_worlddl.c dual-sample height).
 */
static void setup_tev_emboss_lit(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	GXTexObj *height = select_height_tex(mat);
	Mtx tm;
	u8 tc0, tc1, st;

	load_hw_lights(1, 0, current_roughness(mat)); /* key+fill so carved relief reads */

	GX_SetNumChans(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_NONE);

	/* TEXCOORD0 = albedo UV; TEXCOORD1/2 = height at uv and uv+d */
	memset(tm, 0, sizeof(tm));
	tm[0][0] = 1.0f; tm[1][1] = 1.0f; tm[2][2] = 1.0f;
	GX_LoadTexMtxImm(tm, GX_TEXMTX1, GX_MTX2x4);
	tm[0][3] = lab_emboss_ds;
	tm[1][3] = lab_emboss_dt;
	GX_LoadTexMtxImm(tm, GX_TEXMTX2, GX_MTX2x4);

	tc0 = GX_TEXCOORD1;
	tc1 = GX_TEXCOORD2;
	GX_SetNumTexGens(3);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(tc0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1);
	GX_SetTexCoordGen(tc1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2);

	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(&texEnvMid, GX_TEXMAP0);
	}
	GX_LoadTexObj(height, GX_TEXMAP3);
	GX_InitTexObjWrapMode(height, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjFilterMode(height, GX_LINEAR, GX_LINEAR);

	/* Stage 0: albedo × HW diffuse → PREV (SCALE_2 for proof readability) */
	GX_SetNumTevStages(5);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* Stage 1: save lit → REG0 */
	st = GX_TEVSTAGE1;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* Stage 2: h(uv) → PREV */
	st = GX_TEVSTAGE2;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, tc0, GX_TEXMAP3, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* Stage 3: h0 − h(uv+d) + 0.5  (TB_ADDHALF avoids 8-bit mid-grey overflow) */
	st = GX_TEVSTAGE3;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, tc1, GX_TEXMAP3, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV);
	GX_SetTevColorOp(st, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* Stage 4: lit × factor × 2 */
	st = GX_TEVSTAGE4;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C0, GX_CC_ZERO);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void setup_tev_indtex_bump(const Material *mat)
{
	f32 roughness = current_roughness(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.18f;
	PbrSet *pbr = active_pbr(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	/* Stronger warp so high-res normals read clearly on Flipper */
	im[0][0] = 0.70f;
	im[1][1] = 0.70f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = phase * 0.7f - (f32)(int)(phase * 0.7f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(roughness), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjLOD(select_env_tex(roughness), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		roughness * 0.75f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	/* IndTex: active material normal warps env sample into albedo */
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

/* Cheap HSV→RGB for thin-film hue cycling (0..1 hue). */
static void lab_hsv_to_rgb(f32 h, f32 s, f32 v, u8 *r, u8 *g, u8 *b)
{
	f32 c = v * s;
	f32 x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
	f32 m = v - c;
	f32 rf = 0, gf = 0, bf = 0;
	int sector = (int)(h * 6.0f) % 6;
	if (sector < 0) sector += 6;
	switch (sector) {
	case 0: rf = c; gf = x; break;
	case 1: rf = x; gf = c; break;
	case 2: gf = c; bf = x; break;
	case 3: gf = x; bf = c; break;
	case 4: rf = x; bf = c; break;
	default: rf = c; bf = x; break;
	}
	*r = (u8)clampf((rf + m) * 255.0f, 0.0f, 255.0f);
	*g = (u8)clampf((gf + m) * 255.0f, 0.0f, 255.0f);
	*b = (u8)clampf((bf + m) * 255.0f, 0.0f, 255.0f);
}

/*
 * FEAT_CLEARCOAT — modern automotive dual-lobe on Flipper:
 * base (albedo×vtx or mid IBL) + sharp coat env × Fresnel (Schlick A).
 */
static void setup_tev_clearcoat(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 roughness = current_roughness(mat);
	u8 coat = 210; /* high dielectric F0 for lacquer */

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(roughness > 0.55f ? roughness : 0.55f), GX_TEXMAP0);
	}
	GX_LoadTexObj(select_env_tex(0.08f), GX_TEXMAP1); /* sharp coat */
	GX_InitTexObjLOD(select_env_tex(0.08f), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		0.05f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ coat, coat, coat, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);

	/* st0: base albedo × vertex color */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: sharp coat × F0 → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: base + coat * FresnelA */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_C0, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

/*
 * FEAT_IRIDESCENT — thin-film / soap-bubble look: IBL × animated hue konst.
 */
static void setup_tev_iridescent(const Material *mat)
{
	u8 r, g, b;
	f32 hue = fmodf(animTime * 0.12f, 1.0f);
	if (hue < 0.0f) hue += 1.0f;
	lab_hsv_to_rgb(hue, 0.85f, 1.0f, &r, &g, &b);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(0.12f), GX_TEXMAP0);
	GX_InitTexObjLOD(select_env_tex(0.12f), GX_LINEAR, GX_LINEAR, 0.0f, 0.0f,
		0.10f, GX_FALSE, GX_TRUE, GX_ANISO_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ r, g, b, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, mat->base);
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);

	/* st0: sharp IBL × hue */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: lerp toward metal base with FresnelA */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_KONST, GX_CC_APREV, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

/*
 * FEAT_SHEEN_CLOTH — soft wrap body + edge sheen (velvet / fabric).
 * lit × albedo with ADDHALF wrap, then + mid IBL × Fresnel.
 */
static void setup_tev_sheen_cloth(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);

	load_hw_lights(0, 0, current_roughness(mat));
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 48, 36, 52, 255 });
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.70f), GX_TEXMAP0);
	}
	GX_LoadTexObj(select_env_tex(0.55f), GX_TEXMAP1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 180, 120, 160, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);

	/* st0: albedo × HW lit, ADDHALF soft wrap */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: sheen lobe = mid IBL × tint → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: body + sheen * FresnelA */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_C0, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

/*
 * FEAT_SSS_WRAP — cheap subsurface: albedo × (HW lit with soft ambient),
 * then ADDHALF konst scatter tint (modern skin/wax look without a G-buffer).
 */
static void setup_tev_sss_wrap(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	GXColor scatter = { 255, 90, 70, 255 };

	load_hw_lights(0, 0, current_roughness(mat));
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 70, 40, 35, 255 });
	GX_SetChanMatColor(GX_COLOR0A0, mat->base);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.65f), GX_TEXMAP0);
	}

	GX_SetTevKColor(GX_KCOLOR0, scatter);
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* Wrap/scatter: CPREV + konst with ADDHALF (pushes into the dark side) */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_KONST, GX_CC_ONE, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

/*
 * FEAT_HEAT_WARP — "heat haze" / refraction vibe: animated IndTex warp of IBL
 * using the detail map (modern screen-space distortion feel on Flipper).
 */
static void setup_tev_heat_warp(const Material *mat)
{
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.55f;
	PbrSet *pbr = active_pbr(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 1.15f;
	im[1][1] = 1.15f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -2);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = phase * 1.3f - (f32)(int)(phase * 1.3f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.40f), GX_TEXMAP0);
	}
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(0.20f), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

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
}

/*
 * FEAT_TOON_RAMP — NPR / cel: flat saturated konst × hard DF_CLAMP lit.
 * Amb = visible cool fill (not black). Mat/amb from REG (never VTX — that
 * double-multiplied plastic RGB into darkness). No Fresnel ink stage.
 */
static void setup_tev_toon_ramp(const Material *mat)
{
	GXColor cel = mat->base;

	load_hw_lights(0, 0, 0.5f);
	/* Low cool fill → hard lit/shadow cut; white mat so RASC = amb + N·L */
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 22, 16, 38, 255 });
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });

	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);

	GX_SetTevKColor(GX_KCOLOR0, cel);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);

	/* Flat cel × hard lit — SCALE_2 punches the terminator */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_KONST, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
}

/*
 * FEAT_HOLOGRAM — cyber shell (retuned): dark plate + visible scan bands + cyan rim.
 * No full IBL add (that washed to white).
 */
static void setup_tev_hologram(const Material *mat)
{
	Mtx scroll;
	f32 phase = animTime * 1.4f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 10.0f; /* dense horizontal bands */
	scroll[1][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&texDetail, GX_TEXMAP0);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 20, 90, 110, 255 }); /* mid cyan body */
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, mat->base); /* bright rim */
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);

	/* st0: cyan body × scroll detail (CRT-style bands cut valleys) + Fresnel A */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: dielectric Fresnel rim (CYBER metallic=0 so A is rim-only) */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_KONST, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

/* ---- batch 3: glass / neon / parallax / car paint / CRT / force field ---- */

static void setup_tev_glass_refract(const Material *mat)
{
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.15f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.55f;
	im[1][1] = 0.55f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -2);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = 0.03f * sinf(phase * 6.28f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(select_env_tex(0.40f), GX_TEXMAP0); /* softer body */
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(0.08f), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 12, 28, 36, 255 }); /* deep bottle tint */
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 160, 200, 230, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K1);

	/* st0: deep tinted body (volume, not flood) */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: IndTex-warped sharp IBL (refraction) — Fresnel-weighted only */
	GX_SetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: thin Fresnel coat (DIVIDE_2 so edges don't blow) */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_KONST, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
	(void)mat;
}

static void setup_tev_neon_emissive(const Material *mat)
{
	f32 pulse = 0.55f + 0.45f * (0.5f + 0.5f * sinf(animTime * 4.0f));
	u8 g = (u8)(mat->base.g * pulse);
	u8 r = (u8)(mat->base.r * pulse);
	u8 b = (u8)(mat->base.b * pulse);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(0.55f), GX_TEXMAP0);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ r, g, b, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 255, 180, 220, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);

	/* st0: soft IBL × neon tint (body glow) */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: hot Fresnel rim */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_KONST, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

/*
 * Parallax-lite = emboss cousin with larger UV offset: lit × (0.5 + h0 − h1) × 2.
 */
static void setup_tev_parallax_lite(const Material *mat)
{
	Mtx tm;
	PbrSet *pbr = active_pbr(mat);
	GXTexObj *ht = select_height_tex(mat);
	u8 tc0, tc1, st;

	load_hw_lights(1, 0, current_roughness(mat));

	GX_SetNumChans(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_NONE);

	memset(tm, 0, sizeof(tm));
	tm[0][0] = 1.0f; tm[1][1] = 1.0f; tm[2][2] = 1.0f;
	GX_LoadTexMtxImm(tm, GX_TEXMTX1, GX_MTX2x4);
	tm[0][3] = lab_emboss_ds * 2.2f;
	tm[1][3] = lab_emboss_dt * 2.2f;
	GX_LoadTexMtxImm(tm, GX_TEXMTX2, GX_MTX2x4);

	tc0 = GX_TEXCOORD1;
	tc1 = GX_TEXCOORD2;
	GX_SetNumTexGens(3);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(tc0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1);
	GX_SetTexCoordGen(tc1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2);

	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.60f), GX_TEXMAP0);
	}
	GX_LoadTexObj(ht, GX_TEXMAP3);
	GX_InitTexObjWrapMode(ht, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjFilterMode(ht, GX_LINEAR, GX_LINEAR);

	GX_SetNumTevStages(5);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* save lit → REG0 */
	st = GX_TEVSTAGE1;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* h(uv) → PREV */
	st = GX_TEVSTAGE2;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, tc0, GX_TEXMAP3, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* h0 − h1 + 0.5 */
	st = GX_TEVSTAGE3;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, tc1, GX_TEXMAP3, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV);
	GX_SetTevColorOp(st, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* lit × factor × 2 */
	st = GX_TEVSTAGE4;
	GX_SetTevDirect(st);
	GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C0, GX_CC_ZERO);
	GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void setup_tev_car_paint(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.55f), GX_TEXMAP0);
	}
	GX_LoadTexObj(select_env_tex(0.10f), GX_TEXMAP1);

	GX_SetTevKColor(GX_KCOLOR0, mat->base);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 255, 255, 255, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);

	/* st0: body MatCap / albedo × paint tint */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: sharp MatCap flake → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: body + flake * Fresnel */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_C0, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

static void setup_tev_crt_scan(const Material *mat)
{
	Mtx scroll;
	f32 phase = animTime * 0.9f;
	PbrSet *pbr = active_pbr(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 12.0f;
	scroll[1][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.40f), GX_TEXMAP0);
	}
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* CRT lines darken bands */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	(void)mat;
}

static void setup_tev_force_field(const Material *mat)
{
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(0.30f), GX_TEXMAP0);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 2, 8, 6, 255 }); /* near-black core */
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, mat->base); /* shield color on rim */
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);

	/* st0: near-black core (no IBL flood) */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: strong energy rim (Fresnel) — force-field silhouette */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_KONST, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

/*
 * FEAT_WET_SURFACE — rain-wet PBR: darkened albedo×AO + sharp glaze × Fresnel.
 * Samples albedo / AO / metal / normal; roughness drives env LOD (wet prefers sharp).
 */
static void setup_tev_wet_surface(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	f32 wetRough = current_roughness(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(6);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.25f;
	im[1][1] = 0.25f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(wetRough < 0.4f ? 0.05f : wetRough), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough; /* LOD via select_env_tex(wetRough) */

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 140, 150, 165, 255 }); /* cool wet darken */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	/* st0: albedo; Fresnel A from vtx */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: × cool wet tint */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: metal → REG0 (conductor weight under the water sheet) */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: water sheet — normal-warped env × Fresnel (DIVIDE_2 = no force-field limb) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st5: conductor glints under the wet sheet — env × metal / 2 */
	GX_SetTevIndirect(GX_TEVSTAGE5, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE5, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE5, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE5, GX_TEV_KASEL_1);
}

/*
 * FEAT_OXIDE_PATINA — weathered metal: scrolled detail patina on albedo,
 * then metal-map-weighted IndTex env glints through the oxide.
 */
static void setup_tev_oxide_patina(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	Mtx scroll;
	f32 phase = animTime * 0.07f;
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(4);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 2.0f;
	scroll[1][1] = 2.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.40f;
	im[1][1] = 0.40f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: albedo */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × scrolled detail (oxide grain) */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: metal map → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: normal-warped matcap env × metal (bare metal through oxide) */
	GX_SetTevIndirect(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD2, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE3, GX_TEV_KASEL_1);
}

/*
 * FEAT_PORCELAIN — ceramic / enamel: soft wrap body + sharp dielectric coat.
 * Albedo×AO under HW wrap, then sharp env × Fresnel (no metal shell).
 */
static void setup_tev_porcelain(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];

	load_hw_lights(0, 0, current_roughness(mat));
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 70, 68, 80, 255 });
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(4);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.18f;
	im[1][1] = 0.18f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	/* Ceramic coat stays glossy but still tracks B/rough map via clamped LOD */
	GX_LoadTexObj(select_env_tex(current_roughness(mat) < 0.35f
		? current_roughness(mat) * 0.5f : 0.18f), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: albedo × (lit + soft wrap) */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO; keep Fresnel A from vtx (dielectric rim) */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: CPREV*(1-metal) — ceramic metal≈0 keeps body; metallic flecks mute */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: normal-warped sharp glaze × Fresnel */
	GX_SetTevIndirect(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE3, GX_TEV_KASEL_1);
}

/*
 * FEAT_GOLD_LEAF — gilded foil: warm gold tint × albedo, metal-weighted
 * sharp IndTex env (foil glints). Samples metal / normal / rough.
 */
static void setup_tev_gold_leaf(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.35f;
	im[1][1] = 0.35f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat) < 0.3f ? 0.05f : current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 196, 72, 255 }); /* gold leaf */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	/* st0: albedo */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* st1: × AO; Fresnel A from vtx */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: × gold tint */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: metal → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: normal-warped foil env × metal (DIVIDE_2 — no force-field limb) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_FROST_ICE — frost / ice: cool-tinted albedo×AO + crystalline Fresnel
 * coat via normal IndTex. Metal/rough still sampled.
 */
static void setup_tev_frost_ice(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.22f;
	im[1][1] = 0.22f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat) < 0.4f ? 0.08f : current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 170, 205, 230, 255 }); /* frost blue */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	/* st0: albedo; Fresnel A */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: × frost tint */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: metal → REG0 (ice is dielectric; mute metal flecks into body) */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: crystalline glaze × Fresnel */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_CAUSTICS — underwater light patterns: scrolled detail as caustic
 * overlay × albedo×AO, then metal-weighted IndTex env glints.
 */
static void setup_tev_caustics(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	Mtx scroll;
	f32 phase = animTime * 0.35f;
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(6);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 3.0f;
	scroll[1][1] = 3.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = phase * 0.7f - (f32)(int)(phase * 0.7f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.30f;
	im[1][1] = 0.30f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&texDetail, GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 160, 195, 205, 255 }); /* soft aqua wash */
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K0);

	/* st0: albedo; Fresnel A */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: × scrolled caustic detail (SCALE_1 — SCALE_2 washed out as a haze shell) */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: × aqua tint */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: CPREV*(1-metal) — conductors mute under water sheet */
	GX_SetTevDirect(GX_TEVSTAGE4);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st5: IndTex env × Fresnel — soft glints under water */
	GX_SetTevIndirect(GX_TEVSTAGE5, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE5, GX_TEXCOORD2, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE5, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE5, GX_TEV_KASEL_1);
}

/*
 * FEAT_VELVET — nap cloth: soft wrap-lit albedo×AO + rim sheen via Fresnel IndTex.
 * Distinct from SHEEN_CLOTH: uses PBR maps + muted metal + soft env LOD.
 */
static void setup_tev_velvet(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];

	load_hw_lights(0, 0, current_roughness(mat));
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ 40, 32, 48, 255 });
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(4);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.15f;
	im[1][1] = 0.15f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: albedo × soft wrap lighting */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: CPREV*(1-metal) — velvet is dielectric */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: soft rim sheen × Fresnel (DIVIDE_2) */
	GX_SetTevIndirect(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE3, GX_TEV_KASEL_1);
}

/*
 * FEAT_CARBON — carbon fiber weave: albedo×AO × dual-scale scrolled detail,
 * metal-weighted IndTex env (weave glints).
 */
static void setup_tev_carbon(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	Mtx scroll;
	f32 phase = animTime * 0.04f;
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 4.0f;
	scroll[1][1] = 1.5f; /* anisotropic weave stretch */
	scroll[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.55f; /* stretch IndTex along weave */
	im[1][1] = 0.18f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&texDetail, GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: albedo */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);

	/* st1: × AO; Fresnel A */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: × scrolled weave detail */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: metal → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: aniso IndTex env × metal (DIVIDE_2) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD2, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_PEARL — nacre / pearl: soft albedo×AO body + animated hue Fresnel coat
 * via IndTex. Metal map weights the coat; rough drives LOD.
 */
static void setup_tev_pearl(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	f32 hue = animTime * 0.55f;
	u8 r, g, b;

	/* Slow RGB cycle for nacre interference */
	r = (u8)(180 + (int)(60.0f * sinf(hue)));
	g = (u8)(170 + (int)(50.0f * sinf(hue + 2.1f)));
	b = (u8)(200 + (int)(55.0f * sinf(hue + 4.2f)));

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.20f;
	im[1][1] = 0.20f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat) < 0.35f ? 0.12f : current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ r, g, b, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE4, GX_TEV_KCSEL_K0);

	/* st0: albedo; Fresnel A */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: × AO */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st2: metal → REG0 */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st3: IndTex env × Fresnel into PREV */
	GX_SetTevIndirect(GX_TEVSTAGE3, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st4: nacre hue × metal weight folded lightly onto coat */
	GX_SetTevDirect(GX_TEVSTAGE4);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	/* CPREV + KONST * C0 / via: lerp — soft add of hue*metal: CPREV + KONST*C0 with DIVIDE conceptually
	   use CPREV*(1) + KONST*C0: a=KONST,b=0,c=C0,d=CPREV */
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_KONST, GX_CC_ZERO, GX_CC_C0, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}


/*
 * FEAT_CONCRETE — cast concrete: cool grey aggregate tint × albedo×AO,
 * low-frequency scrolled detail (pebble), rough IndTex env. Distinct from
 * asphalt (lighter body, aggregate scroll) and stone hero PBR.
 */
static void setup_tev_concrete(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.04f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.14f;
	im[1][1] = 0.14f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -5);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.6f;
	scroll[1][1] = 1.6f;
	scroll[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;
	(void)pbr->metal;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 168, 170, 166, 255 }); /* cool concrete */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* cool aggregate tint */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* pebble detail × body (MOD2X-ish DIVIDE keeps it subtle) */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* rough IndTex env glints — matte concrete */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD2, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_WOOD_PLANK — plank timber: warm tint × albedo×AO × anisotropic grain
 * scroll (U stretched), soft IndTex sheen. Distinct from VELVET nap / leather.
 */
static void setup_tev_wood_plank(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.03f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.20f;
	im[1][1] = 0.08f; /* stretch — plank grain anisotropy */
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 3.2f; /* long plank grain */
	scroll[1][1] = 0.85f;
	scroll[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;
	(void)pbr->metal;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 176, 128, 72, 255 }); /* warm oak */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* grain detail */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* soft plank sheen */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD2, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_BRICK — fired clay brick: warm red tint × albedo×AO, mortar from AO
 * emphasis, mid IndTex bumps. Distinct from leather (no scrolled grain hide).
 */
static void setup_tev_brick(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.22f;
	im[1][1] = 0.22f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(current_roughness(mat)), GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP5);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;
	(void)pbr->metal;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 186, 92, 68, 255 }); /* fired clay */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* AO ×2 — mortar recesses read darker */
	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* sample metal (dielectric ≈0) into REG0 for map-aware completeness */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0, GX_TEXMAP4, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_CERAMIC_TILE — glazed floor/wall tile: cool glaze tint × albedo×AO ×
 * tiled detail grid, sharp dielectric IndTex coat. Distinct from PORCELAIN
 * (no soft wrap body — hard glaze over grid).
 */
static void setup_tev_ceramic_tile(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx tile;
	f32 phase = animTime * 0.02f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(3);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.28f;
	im[1][1] = 0.28f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	memset(tile, 0, sizeof(tile));
	tile[0][0] = 4.0f; /* tile grid repeat */
	tile[1][1] = 4.0f;
	tile[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(tile, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(current_roughness(mat) < 0.40f
		? current_roughness(mat) * 0.45f : 0.22f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;
	(void)pbr->metal;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 220, 226, 232, 255 }); /* cool glaze */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* cool glaze tint */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* tile grid detail (grout lines) */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* sharp glaze × Fresnel — not porcelain wrap */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD2, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
}

/*
 * FEAT_OLED_VOID — near-black OLED panel: dense IndTex × dual dark KONST.
 * MAPS ON; no force-field shell; bloom stays off. Avoid pale KONST+ADDHALF.
 */
static void setup_tev_oled_void(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.028f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.10f; /* tight OLED pixel grain */
	im[1][1] = 0.12f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -6);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 14.0f; /* dense void grain */
	scroll[1][1] = 15.5f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.30f) - (f32)(int)(phase * 0.30f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.18f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 18, 20, 28, 255 }); /* dual dark void body */
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 42, 52, 78, 255 }); /* dual cool pin fleck */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* near-black OLED body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* sparse cool pin flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* dense IndTex void (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_ANODIZED_MG — magnesium anodize: scrolled IndTex × dual cool-green KONST.
 * MAPS ON; no force-field shell; bloom stays off.
 */
static void setup_tev_anodized_mg(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.046f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.22f; /* anodize pore scroll */
	im[1][1] = 0.10f; /* stretch — brushed Mg grain */
	GX_SetIndTexMatrix(GX_ITM_0, im, -5);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 6.2f;
	scroll[1][1] = 2.4f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.42f) - (f32)(int)(phase * 0.42f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.36f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 68, 96, 88, 255 }); /* dual cool Mg dye */
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 98, 128, 112, 255 }); /* dual cool fleck */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* dyed Mg body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* anodize pore flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* scrolled IndTex metal (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_FROST_POLY — frosted polycarbonate: open IndTex × dual cool KONST.
 * Soft scatter dielectric; no ADDHALF pale washout; bloom stays off.
 */
static void setup_tev_frost_poly(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.038f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.34f; /* open frost scatter */
	im[1][1] = 0.38f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -5);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 3.6f; /* soft poly frost density */
	scroll[1][1] = 4.1f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.48f) - (f32)(int)(phase * 0.48f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.62f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* mid cool frost — not pale washout */
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 118, 138, 152, 255 });
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 148, 168, 178, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* frost poly body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* soft scatter flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* open IndTex frost (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_HOLO_VINYL — peelable holo vinyl: scrolled IndTex × dual chroma KONST.
 * Distinct from HOLO FOIL; no force-field shell; bloom stays off.
 */
static void setup_tev_holo_vinyl(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.055f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.16f; /* vinyl foil scroll */
	im[1][1] = 0.20f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 5.4f;
	scroll[1][1] = 7.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.62f) - (f32)(int)(phase * 0.62f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.16f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 92, 42, 128, 255 }); /* dual chroma vinyl */
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 42, 118, 148, 255 }); /* dual cyan fleck */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* holo vinyl body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* scrolled chroma flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* scrolled IndTex vinyl (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_TUNGSTEN_FOAM — dense dark porous W foam: open IndTex × dual cool gray KONST.
 * MAPS ON; mid-dark body; no pale KONST+ADDHALF; bloom stays off.
 */
static void setup_tev_tungsten_foam(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.032f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.28f; /* open pore foam */
	im[1][1] = 0.32f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -5);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 8.5f; /* dense tungsten cells */
	scroll[1][1] = 9.2f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.36f) - (f32)(int)(phase * 0.36f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.28f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* mid-dark cool gray — not pale washout */
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 58, 62, 72, 255 });
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 88, 94, 108, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* tungsten foam body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* cool pore flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* open IndTex foam (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_ALGAE_GLASS — soft green translucency: scrolled IndTex × dual kelp KONST.
 * MAPS ON; no force-field shell; bloom stays off.
 */
static void setup_tev_algae_glass(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.044f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.20f; /* soft kelp scroll */
	im[1][1] = 0.26f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -5);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 4.2f;
	scroll[1][1] = 5.6f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.52f) - (f32)(int)(phase * 0.52f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.55f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 48, 96, 62, 255 }); /* dual kelp body */
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 78, 132, 88, 255 }); /* dual algae fleck */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* algae glass body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* scrolled kelp flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* scrolled IndTex glass (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_OBSIDIAN_WOOL — near-black loft: dense IndTex × dual deep KONST.
 * MAPS ON; mid-dark; no pale KONST+ADDHALF; bloom stays off.
 */
static void setup_tev_obsidian_wool(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.026f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.12f; /* dense loft grain */
	im[1][1] = 0.14f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -6);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 12.0f; /* dense wool loft */
	scroll[1][1] = 13.5f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.28f) - (f32)(int)(phase * 0.28f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.22f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* dual deep mid-dark — not black crush or pale wash */
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 22, 20, 28, 255 });
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 48, 44, 58, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* obsidian wool body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* dense loft flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* dense IndTex loft (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_TOPAZ_DUST — warm gold grit: fine IndTex × dual amber KONST.
 * MAPS ON; no pale KONST+ADDHALF; bloom stays off.
 */
static void setup_tev_topaz_dust(const Material *mat)
{
	PbrSet *pbr = active_pbr(mat);
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.050f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(5);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.18f; /* fine grit */
	im[1][1] = 0.22f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -4);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 7.8f;
	scroll[1][1] = 6.4f;
	scroll[0][3] = phase - (f32)(int)phase;
	scroll[1][3] = (phase * 0.58f) - (f32)(int)(phase * 0.58f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
	GX_LoadTexObj(&pbr->ao, GX_TEXMAP1);
	GX_LoadTexObj(&texDetail, GX_TEXMAP2);
	GX_LoadTexObj(&pbr->normal, GX_TEXMAP3);
	GX_LoadTexObj(&pbr->metal, GX_TEXMAP4);
	GX_LoadTexObj(select_env_tex(0.24f), GX_TEXMAP5);
	GX_LoadTexObj(&pbr->rough, GX_TEXMAP6);
	GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->ao, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->normal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->metal, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&pbr->rough, GX_REPEAT, GX_REPEAT);
	(void)pbr->rough;

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP3);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* dual amber — mid warm, not pale chrome grit */
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 148, 98, 36, 255 });
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 188, 142, 58, 255 });
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
	GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevDirect(GX_TEVSTAGE1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* topaz dust body */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* fine amber grit flecks */
	GX_SetTevDirect(GX_TEVSTAGE3);
	GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_ZERO, GX_CC_KONST, GX_CC_TEXC, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* fine IndTex grit (no shell) */
	GX_SetTevIndirect(GX_TEVSTAGE4, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP5, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
	(void)pbr->metal;
}

/*
 * FEAT_ANISO_FLAKE — brushed / flake metal: asymmetric IndTex stretches sharp IBL.
 */
static void setup_tev_aniso_flake(const Material *mat)
{
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.08f;

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(2);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 1.60f; /* stretch U — anisotropic sparkle */
	im[1][1] = 0.35f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -3);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = phase - (f32)(int)phase;
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
	GX_LoadTexObj(select_env_tex(0.45f), GX_TEXMAP0); /* body */
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);            /* flake ind */
	GX_LoadTexObj(select_env_tex(0.08f), GX_TEXMAP2); /* sharp flakes */
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetTevKColor(GX_KCOLOR0, mat->base);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	/* st0: mid IBL × metal tint */
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* st1: anisotropic sharp flakes via IndTex */
	GX_SetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
}

/*
 * FEAT_CHROMA_IBL — chromatic aberration on reflections: warp IBL then tint R/B.
 */
static void setup_tev_chroma_ibl(const Material *mat)
{
	f32 im[2][3];
	Mtx scroll;
	f32 phase = animTime * 0.25f;
	PbrSet *pbr = active_pbr(mat);

	GX_SetNumChans(1);
	GX_SetNumTexGens(2);
	GX_SetNumTevStages(3);
	GX_SetNumIndStages(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	memset(im, 0, sizeof(im));
	im[0][0] = 0.55f;
	im[1][1] = 0.55f;
	GX_SetIndTexMatrix(GX_ITM_0, im, -2);

	memset(scroll, 0, sizeof(scroll));
	scroll[0][0] = 1.0f;
	scroll[1][1] = 1.0f;
	scroll[0][3] = 0.02f * sinf(phase * 6.28f);
	scroll[1][3] = 0.02f * cosf(phase * 6.28f);
	GX_LoadTexMtxImm(scroll, GX_TEXMTX0, GX_MTX2x4);

	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_TEXMTX0);
	if (mat->maps != MAPS_NONE) {
		GX_LoadTexObj(&pbr->albedo, GX_TEXMAP0);
		GX_InitTexObjWrapMode(&pbr->albedo, GX_REPEAT, GX_REPEAT);
	} else {
		GX_LoadTexObj(select_env_tex(0.50f), GX_TEXMAP0);
	}
	GX_LoadTexObj(&texDetail, GX_TEXMAP1);
	GX_LoadTexObj(select_env_tex(0.12f), GX_TEXMAP2);
	GX_InitTexObjWrapMode(&texDetail, GX_REPEAT, GX_REPEAT);

	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 60, 40, 255 }); /* red fringe */
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
	GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 40, 90, 255, 255 }); /* blue fringe */
	GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K1);

	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU,
		GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP2, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	/* Blue fringe × Fresnel on the opposite side of the lobe */
	GX_SetTevDirect(GX_TEVSTAGE2);
	GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_KONST, GX_CC_APREV, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_1);
}

static void setup_pbr_tev(const Material *mat, int use_env)
{
	if (!use_env && featureIndex != FEAT_MOD2X_DETAIL && featureIndex != FEAT_SCALE2_LIGHT
		&& featureIndex != FEAT_HERO_RUST_PBR
		&& featureIndex != FEAT_HW_KEY && featureIndex != FEAT_HW_FILL
		&& featureIndex != FEAT_HW_RIM && featureIndex != FEAT_INDTEX_BUMP
		&& featureIndex != FEAT_EMBOSS_LIT && featureIndex != FEAT_HW_SPEC
		&& featureIndex != FEAT_CLEARCOAT && featureIndex != FEAT_IRIDESCENT
		&& featureIndex != FEAT_SHEEN_CLOTH && featureIndex != FEAT_SSS_WRAP
		&& featureIndex != FEAT_HEAT_WARP
		&& featureIndex != FEAT_TOON_RAMP && featureIndex != FEAT_HOLOGRAM
		&& featureIndex != FEAT_ANISO_FLAKE && featureIndex != FEAT_CHROMA_IBL
		&& featureIndex != FEAT_GLASS_REFRACT && featureIndex != FEAT_NEON_EMISSIVE
		&& featureIndex != FEAT_PARALLAX_LITE && featureIndex != FEAT_CAR_PAINT
		&& featureIndex != FEAT_CRT_SCAN && featureIndex != FEAT_FORCE_FIELD
		&& featureIndex != FEAT_WET_SURFACE && featureIndex != FEAT_OXIDE_PATINA
		&& featureIndex != FEAT_PORCELAIN && featureIndex != FEAT_GOLD_LEAF
		&& featureIndex != FEAT_FROST_ICE && featureIndex != FEAT_CAUSTICS
		&& featureIndex != FEAT_VELVET && featureIndex != FEAT_CARBON
		&& featureIndex != FEAT_PEARL
		&& featureIndex != FEAT_CONCRETE && featureIndex != FEAT_WOOD_PLANK
		&& featureIndex != FEAT_BRICK && featureIndex != FEAT_CERAMIC_TILE
		&& featureIndex != FEAT_OLED_VOID && featureIndex != FEAT_ANODIZED_MG
		&& featureIndex != FEAT_FROST_POLY && featureIndex != FEAT_HOLO_VINYL
		&& featureIndex != FEAT_TUNGSTEN_FOAM && featureIndex != FEAT_ALGAE_GLASS
		&& featureIndex != FEAT_OBSIDIAN_WOOL && featureIndex != FEAT_TOPAZ_DUST) {
		setup_tev_flat_lit(mat);
		return;
	}

	switch (featureIndex) {
	case FEAT_FRESNEL_KONST: setup_tev_fresnel_konst(mat); break;
	case FEAT_INDTEX_BUMP:   setup_tev_indtex_bump(mat); break;
	case FEAT_HW_KEY:        setup_tev_hw_lit(mat, 0); break;
	case FEAT_HW_FILL:       setup_tev_hw_lit(mat, 1); break;
	case FEAT_HW_RIM:        setup_tev_hw_lit(mat, 2); break;
	case FEAT_EMBOSS_LIT:    setup_tev_emboss_lit(mat); break;
	case FEAT_HW_SPEC:       setup_tev_hw_spec(mat); break;
	case FEAT_CLEARCOAT:     setup_tev_clearcoat(mat); break;
	case FEAT_IRIDESCENT:    setup_tev_iridescent(mat); break;
	case FEAT_SHEEN_CLOTH:   setup_tev_sheen_cloth(mat); break;
	case FEAT_SSS_WRAP:      setup_tev_sss_wrap(mat); break;
	case FEAT_HEAT_WARP:     setup_tev_heat_warp(mat); break;
	case FEAT_TOON_RAMP:     setup_tev_toon_ramp(mat); break;
	case FEAT_HOLOGRAM:      setup_tev_hologram(mat); break;
	case FEAT_ANISO_FLAKE:   setup_tev_aniso_flake(mat); break;
	case FEAT_CHROMA_IBL:    setup_tev_chroma_ibl(mat); break;
	case FEAT_GLASS_REFRACT: setup_tev_glass_refract(mat); break;
	case FEAT_NEON_EMISSIVE: setup_tev_neon_emissive(mat); break;
	case FEAT_PARALLAX_LITE: setup_tev_parallax_lite(mat); break;
	case FEAT_CAR_PAINT:     setup_tev_car_paint(mat); break;
	case FEAT_CRT_SCAN:      setup_tev_crt_scan(mat); break;
	case FEAT_FORCE_FIELD:   setup_tev_force_field(mat); break;
	case FEAT_WET_SURFACE:   setup_tev_wet_surface(mat); break;
	case FEAT_OXIDE_PATINA:  setup_tev_oxide_patina(mat); break;
	case FEAT_PORCELAIN:     setup_tev_porcelain(mat); break;
	case FEAT_GOLD_LEAF:     setup_tev_gold_leaf(mat); break;
	case FEAT_FROST_ICE:     setup_tev_frost_ice(mat); break;
	case FEAT_CAUSTICS:      setup_tev_caustics(mat); break;
	case FEAT_VELVET:        setup_tev_velvet(mat); break;
	case FEAT_CARBON:        setup_tev_carbon(mat); break;
	case FEAT_PEARL:         setup_tev_pearl(mat); break;
	case FEAT_CONCRETE:      setup_tev_concrete(mat); break;
	case FEAT_WOOD_PLANK:    setup_tev_wood_plank(mat); break;
	case FEAT_BRICK:         setup_tev_brick(mat); break;
	case FEAT_CERAMIC_TILE:  setup_tev_ceramic_tile(mat); break;
	case FEAT_OLED_VOID:     setup_tev_oled_void(mat); break;
	case FEAT_ANODIZED_MG:   setup_tev_anodized_mg(mat); break;
	case FEAT_FROST_POLY:    setup_tev_frost_poly(mat); break;
	case FEAT_HOLO_VINYL:    setup_tev_holo_vinyl(mat); break;
	case FEAT_TUNGSTEN_FOAM: setup_tev_tungsten_foam(mat); break;
	case FEAT_ALGAE_GLASS:   setup_tev_algae_glass(mat); break;
	case FEAT_OBSIDIAN_WOOL: setup_tev_obsidian_wool(mat); break;
	case FEAT_TOPAZ_DUST:    setup_tev_topaz_dust(mat); break;
	case FEAT_MOD2X_DETAIL:  setup_tev_mod2x(mat); break;
	case FEAT_SPEC_MASK:     setup_tev_spec_mask(mat); break;
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
	Mtx nrm;

	GX_LoadPosMtxImm(modelview, GX_PNMTX0);
	/* HW lights need view-space normals (match view_xform_* lights) */
	guMtxInvXpose(modelview, nrm);
	GX_LoadNrmMtxImm(nrm, GX_PNMTX0);

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

/* One sky cube face: GoldSrc-oriented textures remapped to Y-up lab axes. */
static void draw_sky_face(GXTexObj *tex, f32 x0, f32 y0, f32 z0,
	f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, f32 x3, f32 y3, f32 z3)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_InitTexObjWrapMode(tex, GX_CLAMP, GX_CLAMP);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(x0, y0, z0); GX_TexCoord2f32(0, 0);
		GX_Position3f32(x1, y1, z1); GX_TexCoord2f32(1, 0);
		GX_Position3f32(x2, y2, z2); GX_TexCoord2f32(1, 1);
		GX_Position3f32(x3, y3, z3); GX_TexCoord2f32(0, 1);
	GX_End();
}

static void draw_skybox(const Mtx view)
{
	Mtx skyview, modelview;
	SkyPack *sky = &skyPacks[skyIndex];
	const f32 s = 80.0f;
	int i;

	/* Infinite sky: rotation only — drop camera translation so faces don't
	 * look like a nearby cube / warped dome. */
	guMtxCopy(view, skyview);
	skyview[0][3] = 0.0f;
	skyview[1][3] = 0.0f;
	skyview[2][3] = 0.0f;
	guMtxIdentity(modelview);
	guMtxConcat(skyview, modelview, modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0, 0, 0, 0});
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
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

	for (i = 0; i < LAB_SKY_SIDES; ++i) {
		GX_InitTexObjFilterMode(&sky->face[i], GX_LINEAR, GX_LINEAR);
		GX_InitTexObjLOD(&sky->face[i], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
			GX_FALSE, GX_FALSE, GX_ANISO_1);
	}

	/* Y-up remap of GoldSrc sides: ±X=rt/lf, ±Y=up/dn, ±Z=bk/ft */
	draw_sky_face(&sky->face[0],  s,-s,-s,  s,-s, s,  s, s, s,  s, s,-s); /* rt +X */
	draw_sky_face(&sky->face[2], -s,-s, s, -s,-s,-s, -s, s,-s, -s, s, s); /* lf -X */
	draw_sky_face(&sky->face[4], -s, s,-s,  s, s,-s,  s, s, s, -s, s, s); /* up +Y */
	draw_sky_face(&sky->face[5], -s,-s, s,  s,-s, s,  s,-s,-s, -s,-s,-s); /* dn -Y */
	draw_sky_face(&sky->face[1], -s,-s, s,  s,-s, s,  s, s, s, -s, s, s); /* bk +Z */
	draw_sky_face(&sky->face[3],  s,-s,-s, -s,-s,-s, -s, s,-s,  s, s,-s); /* ft -Z */

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

/* Soft additive bloom glow — scaled chrome shell around the sphere. */
static void draw_bloom_glow(Mtx modelview)
{
	Mtx glow, scl;
	f32 pulse = 1.08f + 0.03f * sinf(animTime * 2.2f);

	if (!bloomEnabled)
		return;

	guMtxScale(scl, pulse, pulse, pulse);
	guMtxConcat(modelview, scl, glow);
	GX_LoadPosMtxImm(glow, GX_PNMTX0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	GX_SetNumIndStages(0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_LoadTexObj(select_env_tex(0.15f), GX_TEXMAP0);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASA, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetCullMode(GX_CULL_FRONT); /* outer shell */
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);

	{
		int i;
		/* Dim vertex alpha for soft bloom contribution */
		for (i = 0; i < sphereIdxCount; ++i) {
			SphereVert *v = &sphereVerts[sphereIdx[i]];
			/* temporarily push low alpha via color — use a=40-ish via konst path */
			(void)v;
		}
	}
	/* Use konst alpha instead of mutating verts */
	GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 255, 255, 48 });
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);

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

	{
		int i;
		GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)sphereIdxCount);
		for (i = 0; i < sphereIdxCount; ++i) {
			SphereVert *v = &sphereVerts[sphereIdx[i]];
			GX_Position3f32(v->x, v->y, v->z);
			GX_Normal3f32(v->nx, v->ny, v->nz);
			GX_Color4u8(255, 255, 255, 48);
			GX_TexCoord2f32(v->u, v->v);
			GX_TexCoord2f32(v->u2, v->v2);
		}
		GX_End();
	}

	GX_SetCullMode(GX_CULL_BACK);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

/* Flat charcoal backdrop removed — HDRI skybox fills the void. */

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
	 * world origin under the sphere. Mirror mode: semi-transparent so the
	 * reflected sphere (drawn first) reads through the tiles.
	 */
	Mtx model, modelview;
	const f32 y = -1.12f;
	const f32 s = 6.0f;
	const int cells = 12; /* even → origin on a 4-way tile corner */
	const f32 cell = (2.0f * s) / (f32)cells;
	const GXColor dark = { 58, 48, 48, 255 };
	const GXColor mid = { 118, 102, 100, 255 };
	u8 a = mirrorFloor ? 160 : 255;
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
	if (mirrorFloor)
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	else
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
			GX_Position3f32(x0, y, z0); GX_Color4u8(c.r, c.g, c.b, a);
			GX_Position3f32(x1, y, z0); GX_Color4u8(c.r, c.g, c.b, a);
			GX_Position3f32(x1, y, z1); GX_Color4u8(c.r, c.g, c.b, a);
			GX_Position3f32(x0, y, z1); GX_Color4u8(c.r, c.g, c.b, a);
		}
	}
	GX_End();
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
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
			snprintf(line, sizeof(line), "METAL %.2f  ROUGH %.2f  MAPS %s",
				mat->metallic, rough,
				mat->maps != MAPS_NONE ? "ON" : "OFF");
			draw_text_centered(320, 384, line, 1.0f, 170, 168, 185, ca);
		}

		/* Quiet mode hint + active Flipper path / FX */
		draw_text_col(12, 112, "GALLERY", 1.0f, 180, 170, 210, 180);
		snprintf(line, sizeof(line), "SKY %s  IBL %s",
			skyPacks[skyIndex].name, envMapEnabled ? "ON" : "OFF");
		draw_text_col(12, 128, line, 1.0f, 150, 150, 165, 160);
		snprintf(line, sizeof(line), "Y:%s", featureNames[featureIndex]);
		draw_text_col(12, 144, line, 1.0f, 170, 165, 190, 200);
		snprintf(line, sizeof(line), "MIRROR %s  BLOOM %s  FOG %s",
			mirrorFloor ? "ON" : "OFF",
			bloomEnabled ? "ON" : "OFF",
			fogFxEnabled ? "ON" : "OFF");
		draw_text_col(12, 160, line, 1.0f, 150, 148, 165, 170);
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
		draw_text_col(12, 202, "UP sky  DOWN mirror  L bloom  Z fog", 1.0f, 180, 180, 190, 220);
		draw_text_col(12, 220, "START: gallery mode", 1.0f, 160, 160, 170, 200);
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
	LOAD_PBR_SET(MAPS_STONE, stone);

	TPL_OpenTPLFromMemory(&tplHeightStone, (void *)height_stone_tpl, height_stone_tpl_size);
	TPL_GetTexture(&tplHeightStone, height_stone, &texHeightStone);
	TPL_OpenTPLFromMemory(&tplHeightBrick, (void *)height_brick_tpl, height_brick_tpl_size);
	TPL_GetTexture(&tplHeightBrick, height_brick, &texHeightBrick);
	TPL_OpenTPLFromMemory(&tplHeightConcrete, (void *)height_concrete_tpl, height_concrete_tpl_size);
	TPL_GetTexture(&tplHeightConcrete, height_concrete, &texHeightConcrete);

#define LOAD_SKY_FACE(pack, si, skyname, side) do { \
	TPL_OpenTPLFromMemory(&skyPacks[pack].tplFace[si], (void *)sky_##skyname##_##side##_tpl, sky_##skyname##_##side##_tpl_size); \
	TPL_GetTexture(&skyPacks[pack].tplFace[si], sky_##skyname##_##side, &skyPacks[pack].face[si]); \
	GX_InitTexObjWrapMode(&skyPacks[pack].face[si], GX_CLAMP, GX_CLAMP); \
} while (0)
#define LOAD_SKY_IBL(pack, skyname) do { \
	TPL_OpenTPLFromMemory(&skyPacks[pack].tplIblS, (void *)ibl_##skyname##_sharp_tpl, ibl_##skyname##_sharp_tpl_size); \
	TPL_GetTexture(&skyPacks[pack].tplIblS, ibl_##skyname##_sharp, &skyPacks[pack].iblSharp); \
	TPL_OpenTPLFromMemory(&skyPacks[pack].tplIblM, (void *)ibl_##skyname##_mid_tpl, ibl_##skyname##_mid_tpl_size); \
	TPL_GetTexture(&skyPacks[pack].tplIblM, ibl_##skyname##_mid, &skyPacks[pack].iblMid); \
	TPL_OpenTPLFromMemory(&skyPacks[pack].tplIblB, (void *)ibl_##skyname##_blur_tpl, ibl_##skyname##_blur_tpl_size); \
	TPL_GetTexture(&skyPacks[pack].tplIblB, ibl_##skyname##_blur, &skyPacks[pack].iblBlur); \
	GX_InitTexObjWrapMode(&skyPacks[pack].iblSharp, GX_CLAMP, GX_CLAMP); \
	GX_InitTexObjWrapMode(&skyPacks[pack].iblMid, GX_CLAMP, GX_CLAMP); \
	GX_InitTexObjWrapMode(&skyPacks[pack].iblBlur, GX_CLAMP, GX_CLAMP); \
} while (0)

	skyPacks[0].name = "GOLDEN";
	LOAD_SKY_FACE(0, 0, golden, rt); LOAD_SKY_FACE(0, 1, golden, bk);
	LOAD_SKY_FACE(0, 2, golden, lf); LOAD_SKY_FACE(0, 3, golden, ft);
	LOAD_SKY_FACE(0, 4, golden, up); LOAD_SKY_FACE(0, 5, golden, dn);
	LOAD_SKY_IBL(0, golden);
	skyPacks[1].name = "NOON";
	LOAD_SKY_FACE(1, 0, noon, rt); LOAD_SKY_FACE(1, 1, noon, bk);
	LOAD_SKY_FACE(1, 2, noon, lf); LOAD_SKY_FACE(1, 3, noon, ft);
	LOAD_SKY_FACE(1, 4, noon, up); LOAD_SKY_FACE(1, 5, noon, dn);
	LOAD_SKY_IBL(1, noon);
	skyPacks[2].name = "DUSK";
	LOAD_SKY_FACE(2, 0, dusk, rt); LOAD_SKY_FACE(2, 1, dusk, bk);
	LOAD_SKY_FACE(2, 2, dusk, lf); LOAD_SKY_FACE(2, 3, dusk, ft);
	LOAD_SKY_FACE(2, 4, dusk, up); LOAD_SKY_FACE(2, 5, dusk, dn);
	LOAD_SKY_IBL(2, dusk);

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
	GX_InitTexObjWrapMode(&texHeightStone, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texHeightBrick, GX_REPEAT, GX_REPEAT);
	GX_InitTexObjWrapMode(&texHeightConcrete, GX_REPEAT, GX_REPEAT);
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
	select_material(0, 0); /* RUST open — full PBR maps */
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
				PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN)) {
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
		if ((down & PAD_BUTTON_UP) && !(prevButtons & PAD_BUTTON_UP)) {
			skyIndex = (skyIndex + 1) % LAB_SKY_COUNT;
			cardTimer = 1.8f;
		}
		if ((down & PAD_BUTTON_DOWN) && !(prevButtons & PAD_BUTTON_DOWN))
			mirrorFloor ^= 1;
		if ((down & PAD_TRIGGER_L) && !(prevButtons & PAD_TRIGGER_L))
			bloomEnabled ^= 1;
		if ((down & PAD_TRIGGER_Z) && !(prevButtons & PAD_TRIGGER_Z))
			fogFxEnabled ^= 1;

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
		 * Capture reel — TextureCom PBR only (metal/normal/rough on every mat).
		 * Bloom stays OFF (outer shell read as a force field).
		 *  0–90     RUST hero PBR
		 *  100–160  DAMASCUS / CORRUGATED / SOLAR
		 *  170–350  STEEL roughness 0→1
		 *  360–420  CORRUGATED + NORMAL INDTEX
		 *  430–510  BRICK HW KEY / FILL / RIM
		 *  520–630  STONE INDTEX + EMBOSS
		 *  640–700  DAMASCUS / STEEL HW SPEC
		 *  740–900  sky cycle on STEEL (no bloom)
		 *  930–1200 clearcoat / sheen / SSS / heat / parallax / car paint / CRT
		 */
		if (captureReel) {
			if (frame == 0 || frame == 1) {
				select_material(0, 0); /* RUST */
				featureIndex = FEAT_HERO_RUST_PBR;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
				bloomEnabled = 0;
				cardTimer = 2.5f;
			}
			if (frame == 100) select_material(1, 1);   /* DAMASCUS */
			if (frame == 130) {
				select_material(2, 1); /* CORRUGATED */
				featureIndex = FEAT_HERO_RUST_PBR;
			}
			if (frame == 150) select_material(9, 1);  /* SOLAR */
			if (frame == 170) {
				select_material(3, 1); /* STEEL */
				featureIndex = FEAT_HERO_RUST_PBR;
				roughnessOverride = 0.0f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame >= 170 && frame < 360 && ((frame - 170) % 45) == 0) {
				int step = (int)((frame - 170) / 45);
				static const f32 steps[5] = { 0.00f, 0.25f, 0.50f, 0.75f, 1.00f };
				if (step < 5) {
					roughnessOverride = steps[step];
					tint_sphere_vertices(&materials[materialIndex]);
					cardTimer = 1.6f;
				}
			}
			if (frame == 360) {
				select_material(2, 1); /* CORRUGATED */
				featureIndex = FEAT_INDTEX_BUMP;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 430) {
				select_material(6, 1); /* BRICK */
				featureIndex = FEAT_HW_KEY;
			}
			if (frame == 460) featureIndex = FEAT_HW_FILL;
			if (frame == 490) featureIndex = FEAT_HW_RIM;
			if (frame == 520) {
				select_material(10, 1); /* STONE */
				featureIndex = FEAT_INDTEX_BUMP;
			}
			if (frame == 570) {
				select_material(10, 1); /* STONE */
				featureIndex = FEAT_EMBOSS_LIT;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 640) {
				select_material(1, 1); /* DAMASCUS */
				featureIndex = FEAT_HW_SPEC;
				roughnessOverride = 0.12f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 680) {
				select_material(3, 1); /* STEEL */
				featureIndex = FEAT_HW_SPEC;
				roughnessOverride = 0.08f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 740) {
				skyIndex = 1; /* NOON */
				mirrorFloor = 0;
				bloomEnabled = 0;
				select_material(3, 1); /* STEEL */
				featureIndex = FEAT_HERO_RUST_PBR;
				roughnessOverride = 0.20f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 800) {
				skyIndex = 2; /* DUSK */
				select_material(4, 1); /* CLADDING */
				featureIndex = FEAT_HERO_RUST_PBR;
			}
			if (frame == 860) {
				skyIndex = 0; /* GOLDEN */
				mirrorFloor = 0;
				bloomEnabled = 0;
				select_material(1, 1); /* DAMASCUS */
				featureIndex = FEAT_HERO_RUST_PBR;
			}
			if (frame == 930) {
				select_material(8, 1); /* PAINT */
				featureIndex = FEAT_CLEARCOAT;
				roughnessOverride = 0.30f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
			}
			if (frame == 990) {
				select_material(8, 1); /* PAINT */
				featureIndex = FEAT_IRIDESCENT;
				roughnessOverride = 0.15f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 1050) {
				select_material(7, 1); /* WOOD */
				featureIndex = FEAT_SHEEN_CLOTH;
				roughnessOverride = 0.70f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 1110) {
				select_material(5, 1); /* CONCRETE */
				featureIndex = FEAT_SSS_WRAP;
				roughnessOverride = 0.80f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 1170) {
				select_material(10, 1); /* STONE */
				featureIndex = FEAT_HEAT_WARP;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
			}
			if (frame == 1250) {
				select_material(10, 1); /* STONE */
				featureIndex = FEAT_PARALLAX_LITE;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 1320) {
				select_material(8, 1); /* PAINT */
				featureIndex = FEAT_CAR_PAINT;
				roughnessOverride = 0.25f;
				tint_sphere_vertices(&materials[materialIndex]);
			}
			if (frame == 1390) {
				select_material(1, 1); /* DAMASCUS */
				featureIndex = FEAT_CRT_SCAN;
				roughnessOverride = 0.20f;
				tint_sphere_vertices(&materials[materialIndex]);
				bloomEnabled = 0;
			}
			if (frame == 1460) {
				select_material(6, 1); /* BRICK — wet rain look */
				featureIndex = FEAT_WET_SURFACE;
				roughnessOverride = 0.35f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2; /* DUSK */
				bloomEnabled = 0;
			}
			if (frame == 1530) {
				select_material(0, 1); /* RUST — oxide patina */
				featureIndex = FEAT_OXIDE_PATINA;
				roughnessOverride = 0.55f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
			}
			if (frame == 1600) {
				select_material(5, 1); /* CONCRETE as porcelain stand-in */
				featureIndex = FEAT_PORCELAIN;
				roughnessOverride = 0.42f; /* keep body albedo readable under glaze */
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
			}
			if (frame == 1670) {
				select_material(11, 1); /* GOLD LEAF card + path */
				featureIndex = FEAT_GOLD_LEAF;
				roughnessOverride = 0.18f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0; /* GOLDEN */
				bloomEnabled = 0;
			}
			if (frame == 1740) {
				select_material(12, 1); /* FROST ICE card + path */
				featureIndex = FEAT_FROST_ICE;
				roughnessOverride = 0.30f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1; /* NOON */
			}
			if (frame == 1810) {
				select_material(13, 1); /* CAUSTICS card + path */
				featureIndex = FEAT_CAUSTICS;
				roughnessOverride = 0.45f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2; /* DUSK */
			}
			if (frame == 1880) {
				select_material(14, 1); /* VELVET card + path */
				featureIndex = FEAT_VELVET;
				roughnessOverride = 0.75f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
			}
			if (frame == 1950) {
				select_material(15, 1); /* CARBON card + path */
				featureIndex = FEAT_CARBON;
				roughnessOverride = 0.35f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
			}
			if (frame == 2020) {
				select_material(16, 1); /* PEARL card + path */
				featureIndex = FEAT_PEARL;
				roughnessOverride = 0.28f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
			}
			if (frame == 2090) {
				select_material(17, 1); /* CONCRETE card + path */
				featureIndex = FEAT_CONCRETE;
				roughnessOverride = 0.85f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
			}
			if (frame == 2160) {
				select_material(18, 1); /* WOOD PLANK card + path */
				featureIndex = FEAT_WOOD_PLANK;
				roughnessOverride = 0.78f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
			}
			if (frame == 2230) {
				select_material(19, 1); /* BRICK card + path */
				featureIndex = FEAT_BRICK;
				roughnessOverride = 0.80f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
			}
			if (frame == 2300) {
				select_material(20, 1); /* CERAMIC TILE card + path */
				featureIndex = FEAT_CERAMIC_TILE;
				roughnessOverride = 0.28f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
			}
			if (frame == 2370) {
				select_material(21, 1); /* OLED VOID card + path */
				featureIndex = FEAT_OLED_VOID;
				roughnessOverride = 0.25f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2440) {
				select_material(22, 1); /* ANODIZED MG card + path */
				featureIndex = FEAT_ANODIZED_MG;
				roughnessOverride = 0.40f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2510) {
				select_material(23, 1); /* FROST POLY card + path */
				featureIndex = FEAT_FROST_POLY;
				roughnessOverride = 0.58f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2580) {
				select_material(24, 1); /* HOLO VINYL card + path */
				featureIndex = FEAT_HOLO_VINYL;
				roughnessOverride = 0.22f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2650) {
				select_material(25, 1); /* TUNGSTEN FOAM card + path */
				featureIndex = FEAT_TUNGSTEN_FOAM;
				roughnessOverride = 0.55f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2720) {
				select_material(26, 1); /* ALGAE GLASS card + path */
				featureIndex = FEAT_ALGAE_GLASS;
				roughnessOverride = 0.42f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2790) {
				select_material(27, 1); /* OBSIDIAN WOOL card + path */
				featureIndex = FEAT_OBSIDIAN_WOOL;
				roughnessOverride = 0.72f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 1;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2860) {
				select_material(28, 1); /* TOPAZ DUST card + path */
				featureIndex = FEAT_TOPAZ_DUST;
				roughnessOverride = 0.48f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 2;
				bloomEnabled = 0;
				mirrorFloor = 0;
			}
			if (frame == 2930) {
				select_material(0, 1); /* RUST — back to hero PBR */
				featureIndex = FEAT_HERO_RUST_PBR;
				roughnessOverride = -1.0f;
				tint_sphere_vertices(&materials[materialIndex]);
				skyIndex = 0;
			}
			if (frame == 2980)
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
		guMtxCopy(view, s_view);

		GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

		draw_skybox(view);
		apply_scene_fog(fogFxEnabled || featureIndex == FEAT_FOG_HAZE);

		/* Mirrored sphere under the plaza (Y flip about floor) */
		if (mirrorFloor) {
			Mtx mirror, scl;
			guMtxIdentity(model);
			guMtxRotRad(rot, 'y', animTime * 0.18f + camPunch * 0.55f);
			guMtxScale(scl, 1.0f, -1.0f, 1.0f);
			guMtxConcat(scl, rot, mirror);
			/* Translate so reflection sits under floor plane y=-1.12 */
			mirror[1][3] = -2.24f;
			guMtxConcat(view, mirror, modelview);
			setup_pbr_tev(mat, envMapEnabled);
			draw_sphere(modelview);
		}

		draw_floor(view);
		draw_shadow(view);

		/* Sphere: subtle authored spin so normals / anisotropy read */
		guMtxIdentity(model);
		guMtxRotRad(rot, 'y', animTime * 0.18f + camPunch * 0.55f);
		guMtxConcat(model, rot, model);
		guMtxConcat(view, model, modelview);
		setup_pbr_tev(mat, envMapEnabled);
		draw_sphere(modelview);
		draw_bloom_glow(modelview);

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
