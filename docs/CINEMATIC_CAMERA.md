# Cinematic camera

Keyframed camera paths for cut scenes, short films and documentary capture.
A shot is a `.cam` text file under the game's `cine/` folder. While a shot
plays, the path owns the view (eye, angles, field of view), the HUD and the
weapon are hidden, and game time is locked to the film frame rate so every
rendered frame is exactly one film frame, however slowly the machine or
emulator renders it.

Code: `engine/client/cine_path.c` (path parsing and sampling, pure C, host
tested by `tests/test_cine_path.py`) and `engine/client/cl_cine.c` (console
commands, time lock, frame stamp). The view is applied in `V_RenderView`
(`cl_view.c`) and, on the GameCube CapFaces/WorldDL route, in
`GC_RenderNewGameWorldFrames` (`vid_gamecube.c`), after every probe aim.

## `.cam` format

```
cam 1
# t     x      y      z    pitch  yaw  roll  fov  mode
key 0    -1200  -3000  -150  -4     80   0     70   smooth
key 4    -1100  -2400  -120  -6     85   0     70   smooth
key 8    -900   -1800  -40   -10    95   0     60   hold
key 10   300    1200   250   20     -90  0     75
```

- `t` seconds, increasing. The last key's time is the shot length.
- `pitch yaw roll` in degrees, engine convention (positive pitch looks down).
  Angles are unwrapped key to key, so 350 → 10 turns 20 degrees.
- `fov` horizontal, 1..170.
- `mode` applies to the segment that starts at this key:
  `smooth` (default) is a Catmull-Rom style curve through the neighbouring
  keys that eases in at the first key and out at the last; `linear` is
  constant speed; `hold` (or `cut`) stays on this key until the next one.
- At most 64 keys a shot (2.3 KiB of BSS on the Cube).

## Console

| Command / cvar | |
|---|---|
| `cine_play <name> [preroll]` | play `cine/<name>.cam`; starts when the world is up, after `preroll` seconds held on key 0 |
| `cine_stop` | hand the view back |
| `cine_mark [t] [mode]` | print a key for the current view and append it to `cine/marks.cam`: fly the map in noclip, mark, then set times |
| `cine_info` | shot, clock, frame |
| `cine_fps 30` | game time step while a shot plays (0 = real time) |
| `cine_hud 0` / `cine_viewmodel 0` | draw the HUD / weapon during a shot |
| `cine_stamp 1` | draw the frame stamp |
| `cine_capture 0` | desktop builds: write `cine/<name>/fNNNNN.tga` every frame |
| `cine_onend ""` | console text when a shot ends, e.g. `cine_play shot02` to chain shots |

The log prints `CINE START <name> ...` and `CINE DONE <name> frames=N` for
capture scripts.

## Frame stamp

Dolphin's DumpFrames writes every frame the console shows, which on a slow
frame means the same picture twice, or none. While a shot plays the engine
draws a 20-cell strip along the top-left edge (each cell 1/80 of the screen
width, square): cell 0 white, cells 1..17 the frame number LSB first, cell
18 even parity, cell 19 black. The assembler in dod-gc
(`tools/film/`) reads it, keeps one image per frame number, fills a missed
number with the previous image, and crops the strip away with the letterbox.
Preroll frames carry no stamp and are dropped.
