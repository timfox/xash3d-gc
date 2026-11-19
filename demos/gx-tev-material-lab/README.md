# GX / TEV Material Lab

Standalone GameCube (libogc) Flipper tech demo.

**Hero material** uses Tim’s real PBR rust set from `~/Desktop/pbr-gc/rust`
(albedo / AO / metallic / normal / roughness) — **not** a texture extracted
from the copper reference screenshot. Env reflections use a synthetic
sphere-map, combined in TEV with Fresnel.

## Build

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

python3 scripts/import_rust_maps.py   # from ~/Desktop/pbr-gc/rust → textures/rust_*.png
python3 scripts/gen_assets.py         # synthetic env, HUD chrome, detail/spec
make -j$(nproc)
```

Produces `gx-tev-material-lab.dol`.

## Controls

| Button | Action |
|--------|--------|
| A | Cycle material (RUST / copper / gold / iron / chrome / plastic) |
| B | Step roughness |
| X | Toggle env map |
| Y | Cycle TEV feature path |

### TEV feature paths (Y)

| Path | What Flipper is doing |
|------|------------------------|
| RUST PBR MAPS | albedo × AO + synthetic env × Fresnel |
| FRESNEL+KONST | albedo + env × konst F0 |
| MOD2X DETAIL | rust albedo × detail `GX_CS_SCALE_2` |
| SPEC MASK ADD | albedo + specular-lobe add |
| INDTEX BUMP | rust normal IndTex warps env into albedo |
| FOG HAZE | `GX_FOG_PERSP_LIN` |
| SCALE2 LIGHT | albedo × vtx `GX_CS_SCALE_2` |
| ALPHA CLIP | `GX_SetAlphaCompare` |

## Capture

```bash
./scripts/run_dolphin_capture.sh /tmp/gx-tev-material-lab-cap
```
