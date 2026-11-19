# GX / TEV Material Lab

Standalone GameCube (libogc) demo: PBR-like copper sphere via Flipper TEV
(base metal tint × env map × Fresnel approx), with Material Lab HUD chrome.

## Build

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
python3 scripts/gen_assets.py
make -j$(nproc)
```

Produces `gx-tev-material-lab.dol`.

## Controls

| Button | Action        |
|--------|---------------|
| A      | Cycle material |
| B      | Step roughness |
| X      | Toggle env map |

## Capture

```bash
./scripts/run_dolphin_capture.sh /tmp/gx-tev-material-lab-cap
```

Uses Flatpak Dolphin (`org.DolphinEmu.dolphin-emu`) with frame dumps. Prefer a
`/tmp/...` output path so Flatpak can write frames.
