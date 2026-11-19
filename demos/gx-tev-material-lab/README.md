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

python3 scripts/import_rust_maps.py       # ~/Desktop/pbr-gc/rust → textures/rust_*.png
python3 scripts/import_damascus_maps.py   # ~/Desktop/pbr-gc/metal_damascus → damascus_*.png
python3 scripts/import_tcom_sets.py       # Storage/Extra/Stream TCom packs → textures/<slug>_*.png
python3 scripts/gen_assets.py             # synthetic env + **drawn** HUD sprites (no ref crops)
make -j$(nproc)
```

Produces `gx-tev-material-lab.dol`.

HUD chrome (logo, purple banner, GC A/B/X/Y buttons, FPS) is generated as
sprites in `gen_assets.py` — do **not** run `bake_from_reference.py` (retired).

## Presentation

Opens in **Gallery** mode (title card, soft letterbox, idle camera orbit).
**START** flips to **Lab** (full Flipper engineer HUD). Any pad input cancels
the DumpFrames capture reel so you stay in control.

```bash
./scripts/run_dolphin.sh          # interactive Flatpak Dolphin
```

## Controls

| Button | Action |
|--------|--------|
| A / D-pad / L R | Cycle material |
| B | Step roughness |
| X | Toggle env map |
| Y | Cycle TEV / lighting feature path |
| START | Gallery ↔ Lab |
| Z | Toggle idle auto-showcase |
| Stick X | Orbit camera |

### TEV / lighting paths (Y)

| Path | What Flipper is doing |
|------|------------------------|
| PBR MAPS + ENV | albedo × AO + synthetic env × Fresnel |
| FRESNEL+KONST | albedo + env × konst F0 |
| NORMAL INDTEX | active-material normal IndTex-warps env into albedo |
| HW KEY LIGHT | `GX_LIGHT0` key diffuse × albedo |
| HW KEY+FILL | key + cool fill (`GX_LIGHT0\|1`) |
| HW KEY+RIM | key + fill + rim backlight |
| MOD2X DETAIL | albedo × detail `GX_CS_SCALE_2` |
| SPEC MASK ADD | albedo + specular-lobe add |
| FOG HAZE | denser `GX_FOG_PERSP_LIN` |
| SCALE2 LIGHT | albedo × vtx `GX_CS_SCALE_2` |
| ALPHA CLIP | `GX_SetAlphaCompare` |

Maps import at **192²** (TCom) / **256²** (rust·damascus) — GC-safe DOL size.

## Roughness on Flipper

B steps roughness. The demo swaps discrete env LODs:

| Roughness | Env texture | Look |
|-----------|-------------|------|
| under 0.20 | `env_sharp` | mirror chrome |
| under 0.55 | `env_mid` | soft reflections |
| 0.55+ | `env_blur` | matte / frosted |

Dolphin auto-reel (first ~5s) locks **CHROME** and walks 0→1 so DumpFrames
can prove the path. Capture script also writes `shots/rough-*.png`.

## Capture

```bash
./scripts/run_dolphin_capture.sh /tmp/gx-tev-material-lab-cap
```
