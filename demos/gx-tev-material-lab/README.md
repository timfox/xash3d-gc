# GX / TEV Material Lab

Standalone GameCube (libogc) Flipper tech demo:

1. **Hero shot** — reference copper sphere (metallic 1.0 / roughness 0.25) via
   reference-baked matcap IBL + Fresnel look.
2. **Feature showcase** — cycle real Flipper/TEV techniques (mod2x detail,
   specular mask, IndTex bump warp, fog, scale2 light, alpha compare, konst
   Fresnel) with a live HUD that names the active path.

## Build

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

# Optional: refresh matcap/HUD from the Project-store reference PNG
python3 scripts/bake_from_reference.py

python3 scripts/gen_assets.py   # procedural extras (bump/detail/font/…)
make -j$(nproc)
```

Produces `gx-tev-material-lab.dol`.

## Controls

| Button | Action |
|--------|--------|
| A | Cycle material (copper / gold / iron / chrome / brushed / plastic) |
| B | Step roughness (0 → 0.25 → 0.5 → 0.75 → 1.0) |
| X | Toggle env map |
| Y | Cycle TEV feature path |

### TEV feature paths (Y)

| Path | What Flipper is doing |
|------|------------------------|
| HERO MATCAP IBL | Reference-baked matcap sample × material tint |
| FRESNEL+KONST | Env × konst F0 + vertex Schlick rim |
| MOD2X DETAIL | Dual-tex matcap × detail with `GX_CS_SCALE_2` |
| SPEC MASK ADD | Matcap + specular-lobe texture add via konst |
| INDTEX BUMP | `GX_SetTevIndirect` bump warp into env (Lost Coast-style) |
| FOG HAZE | `GX_SetFog` perspective linear haze |
| SCALE2 LIGHT | Env × vertex with `GX_CS_SCALE_2` boost |
| ALPHA CLIP | `GX_SetAlphaCompare` cutout path |

After ~3s the demo also auto-advances feature paths so a single Dolphin
framedump run can harvest showcase stills.

## Capture

```bash
./scripts/run_dolphin_capture.sh /tmp/gx-tev-material-lab-cap
```

Uses Flatpak Dolphin (`org.DolphinEmu.dolphin-emu`) with frame dumps. Prefer a
`/tmp/...` output path so Flatpak can write frames.
