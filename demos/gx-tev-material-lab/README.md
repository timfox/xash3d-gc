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
python3 scripts/bake_height_from_normals.py  # stone/brick/concrete height for EMBOSS LIT
# HDRI skyboxes + IBL matcaps (needs gxbake on PYTHONPATH):
PYTHONPATH=/path/to/gxbake python3 scripts/bake_hdri_skyboxes.py
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
| A / D-pad L/R | Cycle material |
| B | Step roughness |
| X | Toggle IBL env |
| Y | Cycle TEV / lighting feature path |
| D-pad UP | Cycle HDRI skybox (GOLDEN / NOON / DUSK) |
| D-pad DOWN | Toggle mirror floor |
| L | Toggle bloom glow |
| Z | Toggle fog FX |
| START | Gallery ↔ Lab |
| Stick X | Orbit camera |

### TEV / lighting paths (Y)

| Path | What Flipper is doing |
|------|------------------------|
| PBR MAPS + ENV | albedo × AO + synthetic env × Fresnel |
| FRESNEL+KONST | albedo + env × konst F0 |
| NORMAL INDTEX | active-material normal IndTex-warps env into albedo |
| HW KEY LIGHT | `GX_LIGHT0` key diffuse × albedo (diffuse-only A/B) |
| HW KEY+FILL | key + cool fill (`GX_LIGHT0\|1`) |
| HW KEY+RIM | key + fill + rim backlight |
| EMBOSS LIT | Lost Coast dual-sample height emboss under HW key (`lit×(0.5+h0−h1)×2`) |
| HW SPEC | `GX_DF_CLAMP` + `GX_AF_SPEC`; shininess from roughness |
| CLEARCOAT | Dual-lobe lacquer: base albedo + sharp IBL × Fresnel |
| IRIDESCENT | Thin-film: IBL × animated hue konst |
| SHEEN CLOTH | Wrap diffuse (`TB_ADDHALF`) + edge sheen IBL |
| SSS WRAP | Subsurface-ish: lit albedo + scatter konst wrap |
| HEAT WARP | IndTex animated detail warps IBL (heat-haze vibe) |
| PARALLAX LITE | Emboss-cousin dual-height relief on stone |
| CAR PAINT | MatCap body + sharp flake × Fresnel (PAINT maps) |
| CRT SCAN | Dense scrolled scanline overlay |
| FORCE FIELD | Inverse Fresnel energy shell (lab only; bloom/mirror default OFF) |
| WET SURFACE | Darkened albedo×AO + sharp Fresnel glaze (metal/normal/rough) |
| OXIDE PATINA | Scrolled detail × albedo + metal IndTex glints |
| PORCELAIN | Soft wrap body + normal-warped dielectric coat |
| GOLD LEAF | Warm gold tint × metal IndTex foil glints |
| FROST ICE | Cool frost tint + crystalline Fresnel coat |
| CAUSTICS | Scrolled underwater light pools × PBR maps |
| VELVET | Soft wrap nap + rim sheen IndTex |
| CARBON | Dual-scale weave × metal IndTex glints |
| PEARL | Soft body + animated nacre Fresnel coat |
| CONCRETE | Cool aggregate tint × scrolled pebble + rough IndTex |
| WOOD PLANK | Warm oak tint × anisotropic grain scroll + soft sheen |
| BRICK | Fired clay tint × AO×2 mortar + IndTex bumps |
| CERAMIC TILE | Cool glaze × tile grid detail + sharp dielectric coat |
| OLED VOID | Near-black OLED × dual dark KONST IndTex |
| ANODIZED MG | Dyed Mg metal × dual cool-green KONST IndTex |
| FROST POLY | Frosted polycarb × dual cool KONST IndTex |
| HOLO VINYL | Peel holo vinyl × dual chroma KONST IndTex |
| ASPHALT | Dark road body + sparse wet IndTex glints |
| RUBBER | Soft wrap tire rubber, muted Fresnel |
| PLASTIC ABS | Cool-tinted injection plastic + soft dielectric coat |
| LEATHER | Warm hide grain + soft sheen IndTex |
| NEON RIM | Solid albedo×AO body + thin KONST Fresnel accent (not force-field) |
| THIN FILM OIL | Dark body + low-sat rainbow on Fresnel only (not IRIDESCENT bloom) |
| BRUSHED TITANIUM | Cool Ti tint + directional IndTex brush streaks (not ANISO FLAKE) |
| STAINED GLASS | Cobalt-tinted panes × AO + sharp Fresnel IndTex coat |
| LED MATRIX | Pixel-grid panel × metal-gated KONST emissive (no force-field shell) |
| OIL SLICK | Dark wet body + rainbow Fresnel film (≠ IRIDESCENT / thin-film oil) |
| TOON INK RIM | Cel body + KONST ink rim (≠ TOON RAMP / force field) |
| LIQUID MERCURY | Cool chrome fluid × metal IndTex |
| JADE GLAZE | Soft wrap jade body + dielectric glaze |
| HOLO FOIL | Foil base + metal IndTex × rainbow KONST |
| MOD2X DETAIL | albedo × detail `GX_CS_SCALE_2` |
| SPEC MASK ADD | albedo + specular-lobe add |
| FOG HAZE | denser `GX_FOG_PERSP_LIN` |
| SCALE2 LIGHT | albedo × vtx `GX_CS_SCALE_2` |
| ALPHA CLIP | `GX_SetAlphaCompare` |

Maps import at **192²** (TCom) / **256²** (rust·damascus); emboss heights **64²**;
HDRI sky faces **512² CMPR** + IBL matcaps **256²** (gxbake equirect→sides) — DOL ~≤16MB MEM1-safe.

### HDRI / FX

Three IBL-driving skies baked from local equirect panoramas via **gxbake**
(`scripts/bake_hdri_skyboxes.py`): GOLDEN, NOON, DUSK. Each packs six cube
faces plus sharp/mid/blur matcaps that replace the old synthetic env LODs.
Mirror floor draws a Y-flipped sphere under a translucent plaza; bloom is an
additive outer shell.

### Dolphin proofs (emboss / specular)

Capture reel frames ~570–700 lock **STONE + EMBOSS LIT** then **DAMASCUS/CHROME + HW SPEC**.
If this environment has no Dolphin/devkit, build on a machine with `DEVKITPRO` and run:

```bash
./scripts/run_dolphin_capture.sh /tmp/gx-tev-material-lab-cap
# → shots/emboss-lit.png, shots/hw-spec.png
```

Sibling self-hosted worker [GX TEV Material Lab](bc-bf36fe5e-1557-50c8-9093-12a29316f14e) can Dolphin-prove when available.

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
