# Test maps

Small original maps for testing the GameCube port. They use retail Half-Life
textures and entities only. Textures are referenced from `halflife.wad`, never
embedded, so the BSPs carry no Valve art and need the retail game data to run.

| Map | What it is |
| --- | --- |
| `gc_crabhall` | Black Mesa hallway (c1a1 cream walls, tiled floor, fifties ceiling lights) that turns into an Anomalous Materials style lab arena with crates, a rising double door and 15 headcrabs. Spawn hall has the HEV suit, crowbar, 9mm, shotgun, MP5, ammo, health and batteries. |

## Play one in Dolphin

    scripts/play-dolphin.sh --map gc_crabhall

This copies `maps/gc_crabhall.bsp` into the play disc's build cache (the
retail `Half-Life/valve` tree is not touched), builds
`OUT/xash3d-gc-play-gc_crabhall.iso` with `--play --play-map gc_crabhall`, and
starts it. Pick New Game in the retail menu as usual and it loads the test map
instead of c0a0.

## Edit or rebuild

`src/gen_<map>.py` writes the `.map` (Valve 220 format, opens in J.A.C.K.,
Hammer or TrenchBroom). Compile with any ZHLT/VHLT/SDHLT toolchain with
`halflife.wad` on the WAD path:

    python3 maps/src/gen_gc_crabhall.py > maps/src/gc_crabhall.map
    hlcsg -wadautodetect gc_crabhall
    hlbsp gc_crabhall
    hlvis -full gc_crabhall
    hlrad -extra gc_crabhall
