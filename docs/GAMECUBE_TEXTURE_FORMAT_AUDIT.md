# GameCube texture upload audit (ref/gx)

Static audit of which texture classes the GX renderer uploads in which format, and
what compressing the others to CMPR would save. Not measured on hardware or Dolphin.
Disc byte totals come from scanning the local DoD and Valve content trees; resident
costs come from the constants in `ref/gx/r_gx_world.c`, `r_gx_worlddl.c`, `r_image.c`.

CMPR = GX_TF_CMPR (S3TC/DXT1 style): 4 bits per texel, 1-bit alpha, 8x8 tile
granularity. RGB565 / RGB5A3 = 16 bits per texel. I8 = 8 bits.

## Per class

| Class | Where | Format today | Resident cost | CMPR candidate? |
|---|---|---|---|---|
| World textures | `R_GXPackedTexture` (pack `.gcp`, ARAM, `gc_packed*`) | **CMPR** with mips | 24 KiB bounce slots (PR #48), hot window | already |
| Menu tiles and backgrounds | `R_GXBindMenuTileCmpr` (`.cmp`, `GCMP` header), `R_GXBindMenuBackground` | tiles **CMPR**; bg RGB5A3 | tiles `w*h/2`; bg RGB5A3 up to 320 px | bg yes (opaque); loading/background TGAs yes |
| Sky | `r_gx_sky_obj`, 128x128 | **CMPR** | 8 KiB per face | already |
| Brush props | `gc_prop_slot_obj` | **CMPR** | slots | already |
| Studio skins (NPC) | `R_GXNativeTexture`, in place in the studio skin region | RGB565 / RGB5A3 (masked) tiled, <=96x96 | 2 B/texel (96x96 = 18 KiB) | yes for opaque; masked `{` skins keep RGB5A3 or CMPR with 1-bit alpha |
| Studio skins (viewmodel) | same, plus ARAM skin stream (`r_gx_skins.c`) | RGB565 (gun) / RGB5A3+ATE (hands) | 64x64 low mip 8 KiB resident, 256x256 full = 128 KiB in ARAM/window | yes: 256x256 CMPR = 32 KiB (4x smaller window and ARAM) |
| HUD / sprites / fonts | `R_GXBindTexnum` slot pool (`GC_GX_TEX_SLOTS` 32, max dim 128 world / 512 HUD) | RGB5A3 (HUD) / RGB565 tiled copies | `GC_GX_TEX_HUD_HEAP_BUDGET` 64 KiB + slots | sprites with hard edges yes; fonts, crosshairs, masks, smooth alpha no (keep IA/I/RGB5A3) |
| Lightmaps | `r_gx_lm_atlas_obj`, `gc_wd_lm_obj` (32x32 pages), live lm | RGB565 | 2 B/texel | no (gradients band under CMPR) |
| Cinematic / video | `r_gx_cinematic_tex` 320x240 | RGB565 | 150 KiB | no (per frame) |
| Bloom | `gc_bloom_tex` | I8 | small | no |
| TEV spec/normal/sun | `gc_spec_obj`, `gc_nrm_obj`, `gc_sun_obj` | CMPR 64x64 / 32x32 | tiny | already |

## Bytes on the disc (what a CMPR bake would shrink)

Counted from the TGA/SPR headers in `content/`:

| Set | Files | Texels | As 16-bit | As CMPR |
|---|---:|---:|---:|---:|
| dod `resource/**/*.tga` | 16 | 0.51 M | 997 KiB | 249 KiB |
| dod `gfx/vgui/**/*.tga` | 62 | 1.47 M | 2880 KiB | 720 KiB |
| dod `sprites/*.spr` (all frames) | 128 | 5.87 M | 11472 KiB | 2868 KiB |
| valve `resource/**/*.tga` | 155 | 7.88 M | 15381 KiB | 3845 KiB |
| valve `gfx/vgui/**/*.tga` | 107 | 5.09 M | 9935 KiB | 2483 KiB |
| valve `sprites/*.spr` | 170 | 7.14 M | 13944 KiB | 3486 KiB |

These are totals across all assets; only what a map or screen uses is resident. Several
`gfx/vgui/fonts/*.tga` are wide strips (6144x27) that must stay uncompressed.
The 225 KiB `resource/gc_menu/loading.tga` that fails in the FileSystem Pool on
Avalanche is 320x240; as a baked `.cmp` it is 37.5 KiB and loads without the
decoder.

## Existing precedent to reuse

`scripts/build-gamecube-disc.py` already bakes CMPR with `gx_cmpr_encode()` and a
32-byte header: `b"GCMP" + >HH(w, h) + 24 zero bytes` for `logo_banner.cmp` and
`bg_<row>_<col>.cmp`; `R_GXBindMenuTileCmpr` loads `<name>.cmp` next to `<name>.tga`
and uploads with no decode. The sky and world packs use the same encoder. The next
step is to generalise that header (add format, mip count, `cache_version`, source
size) and bake more classes with it; the engine keeps today's path when the header
is missing or its version differs.

## Recommendation (order)

1. Menu/loading TGAs (opaque): cheapest, removes the FileSystem Pool failure.
2. Viewmodel skins: 4x smaller ARAM store and MEM1 window; needs per-skin `.cmp`
   baked from the MDL palette at disc build time.
3. NPC skins: opaque ones; masked skins only if a 1-bit alpha CMPR looks right.
4. HUD sprites: hard-edged only; allow-list, not blanket.
