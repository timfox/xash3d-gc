# GameCube Memory Evidence

This is an evidence report, not a hand-authored memory budget. Run
`python3 scripts/gamecube-memory-evidence.py --repo .` after a build and after
each representative map probe. The generated report is the source of truth;
missing measurements are reported as `UNAVAILABLE` and must not be replaced by
estimates.

## Memory model used by this project

The release model currently treats approximately **24 MiB of MEM1/main
memory** and **16 MiB of MEM2/auxiliary memory** as the relevant operating
assumptions. These are capacity assumptions, not allocations available to the
engine. The report must account for executable sections, runtime arenas,
stacks, libraries, caches, and temporary staging before claiming headroom.

No fixed pool split is authorized by this document. In particular, this report
does not assume a 32 MiB VRAM budget. GPU-visible memory, embedded framebuffers,
texture residency, and auxiliary-memory use must be identified from linker,
runtime, or hardware evidence separately.

## Required generated evidence

| Evidence | Source | Required result |
|---|---|---|
| ELF/DOL section sizes | ELF section table and DOL header | text/data/BSS and every DOL load section, in bytes and address range |
| Linker map | linker map emitted by the build | largest symbols/sections and placement in MEM1/MEM2; `UNAVAILABLE` if absent |
| MEM1/MEM2 high-water | Dolphin/hardware log telemetry | base, peak, free-at-peak, stage, map, and route; never infer MEM2 from MEM1 |
| Per-map peak usage | map compatibility/campaign/probe logs | one row per map and route, including failures |
| Texture/lightmap/audio/cache | tagged allocation telemetry or allocator report | subsystem, requested bytes, resident peak, and owner; `UNAVAILABLE` if untagged |
| Largest failed allocation | `mem FAIL`, allocator, or guest-fatal marker | requested bytes, subsystem, map, total/HWM, and source location |

## Current evidence state

Run the generator to refresh this section’s companion artifact:

```sh
python3 scripts/gamecube-memory-evidence.py \
  --repo . \
  --output .ai/state/gamecube-memory-evidence.json \
  --markdown .ai/state/gamecube-memory-evidence.md
```

The checked-in interpretation is intentionally conservative:

- MEM1 capacity: approximately 24 MiB project assumption; measured available
  capacity and free-at-peak: **UNAVAILABLE until a current runtime report is
  generated**.
- MEM2 capacity: approximately 16 MiB project assumption; measured use and
  high-water: **UNAVAILABLE unless explicitly emitted by the runtime**.
- ELF/DOL sections: **read from the current artifact by the generator**; a DOL
  file size is not a RAM-usage measurement.
- Linker map: **UNAVAILABLE when the build does not preserve one**. Add map
  output to the build before making placement or BSS claims.
- Per-map peaks: **only accepted from fresh logs** containing map and stage
  markers. Historical prose does not establish a current peak.
- Texture, lightmap, audio, and cache peaks: **UNAVAILABLE unless allocations
  carry subsystem tags**. Renderer source size or a guessed cache capacity is
  not residency evidence.
- Largest failed allocation: **taken from the largest parsed failure marker**;
  if no marker exists, it is `UNAVAILABLE`, not zero.


## Day of Defeat network build: measured budget

Build measured: engine master `d23b9c58a`, dod-gc `7bf7420`, libogc stack,
`XASH_GC_DEFAULT_SLOTS=3`, `XASH_GAMECUBE_LINK_BBA=1`, devkitPPC 16.1.0.
**M** = measured from the ELF or a Dolphin log; **D** = derived.
Regenerate the symbol tables with `scripts/gamecube-symbol-owners.py OUT/bin/xash-dod`
(the build emits no linker map; this reads DWARF line info through `nm -S -l`).

### MEM1 static image (M)

| Region | Range | Size |
|---|---|--:|
| .init + .text | 0x80003100 - 0x805d2440 | 5.81 MiB |
| .rodata, .eh_frame, .data | 0x805d2440 - 0x806eb1a0 | 1.10 MiB |
| .sbss + .bss | 0x806eb1a0 - 0x81659920 | 15.43 MiB |
| **Static image end** | 0x81659920 | **22.35 MiB** |
| Tail to MEM1 top (D) | 0x81659920 - 0x81800000 | 1.65 MiB |

The tail holds the malloc heap, the XFB (one 320x480 buffer, 300 KiB; dual XFB is
unavailable under BBA) and the stack. Runtime Arena1Lo/Hi are logged only with
`XASH_GC_MEMWATCH_DEBUG`, so the exact arena bounds are **D**, not **M**.

### BSS by owner (M, top entries; 16.18 MB section, 15.43 MiB in named symbols)

| Owner | Size | Note |
|---|--:|---|
| `vid` (ref/gx/r_context.c) | 3.69 MiB | map-load arena (colormap, alphamap 1.5 MB, pad 1.2 MB) |
| `gc_world_array_spill` | 900 KiB | |
| `gc_gcmap_bootstrap_entities` | 744 KiB | |
| `gc_studio_bss` | 648 KiB | |
| `cl` | 638 KiB | |
| `sv_` | 623 KiB | |
| `gc_singleplayer_edicts` | 361 KiB | |
| `r_images` | 352 KiB | |
| `gpMove` x2 (cl_game.c, sv_game.c) | 635 KiB | 317 KiB each |
| `gx_fifo`, `gc_static_clipnodes` | 256 KiB each | |
| lwip `memp_memory` + `ram`, `gc_sv_netbuf` | 538 KiB | network build only |

### Tail and heap by stage (M, log of dod_charlie)

| Stage | heap_used | heap_free | unclaimed |
|---|--:|--:|--:|
| pre-Con_Init | 376 KiB | 35 KiB | 1.83 MiB |
| post-Platform_Init | 458 KiB | 49 KiB | 1.73 MiB |
| pre-SV_Init (boot reserve release) | 830 KiB | 293 KiB | 1.13 MiB |
| map active | 1.83-2.01 MiB | 219-365 KiB | 8-48 KiB |

Per map at "map active" (M): charlie heap_free 219 KiB / unclaimed 8 KiB;
avalanche 168 KiB / 0; caen 159 KiB / 56 KiB; zalec 143 KiB / 0.
These logs predate the current master image, which is 0.69 MB larger than the
build they came from, so re-measure after any change.

### ARAM in a DoD match (M from the dod_caen / avalanche / zalec / charlie logs, D where marked)

16 MiB pool (15.98 MiB usable above the DSP's 16 KiB). Session reserves are carved from
the bottom, streams from the top; the free gap in the middle is what `largest=` reports.

| Owner | Class | avalanche | caen | zalec | Idle in a match? |
|---|---|--:|--:|--:|---|
| `audio` | session | 1.00 MiB | 1.00 | 1.00 | no |
| `prefetch` | session | 1.03 MiB | 1.03 | 1.03 | **yes**: the menu-idle first-map prefetch, filled before the match |
| `wtstore` | session | 6.00 MiB | 6.00 | 6.00 | **mostly**: a ring sized 6 MiB, but the whole map pack is 1.39 (charlie), 2.23 (chemille), 2.34 (avalanche), 2.73 (anzio), 3.22 MiB (caen) (M, `media/wt_*.gct`) |
| `worlddl` | stream | 1.50 MiB | 3.00 | none logged | no |
| `lightfaces` | stream | 2.10 MiB | 3.01 | none logged | no |
| `props` + `proptex` | session | 0.12 MiB | 0.27 | n/a | no |
| `animpack` mesh packs (.gcm) | stream | 0.35 MiB | 0.27 | 0.29 | no |
| `animpack` animation packs (.gca) | stream | **refused x2** | **refused x2** | 1 of 2 | |
| free after the above | | 2.32 MiB | 1.17 | 2.48 | |

`saves` (4 MiB, changelevel transition mode), `sfx` and `lstyles` do **not** appear as
reserves in any DoD log, so they are not holding ARAM in a match. `prefetch` and
`wtstore` are the reserves that are held but largely unused.

**Animation packs.** Each player animation pack is 3.55 MiB, 345 sequences, one per
team class (`models/player/anim_<hash>.gca`), and a match with both teams needs two:
**7.1 MiB** to hold both in ARAM. A refused pack is not an error: sequences are read from
disc through the 256 KiB MEM1 ring (`Mod_GCAnimPackFetch`), about 10 KiB per sequence on
average (3.55 MiB / 345), so the cost is one disc seek and read the first time each bot
body shows a sequence that is not already in the ring (D: 24 sequences fit the ring; a
real-disc seek is tens of milliseconds, Dolphin's is much less). Measured: on avalanche
and caen both packs stream from disc; on zalec one fits in ARAM and one streams.

**Cheapest hole (D, not applied).** `wtstore` is a ring that evicts the oldest entries,
so shrinking it costs only more re-streaming for a pack larger than the ring. Every DoD
pack measured fits in 3.5 MiB, which would free **2.5 MiB**; dropping `prefetch` once
the match starts would free another **1.0 MiB**. Together that is about 3.5 MiB of
contiguous gap, enough for one more 3.55 MiB animation pack (both only if the layout also
shrinks `worlddl`/`lightfaces`). A change needs the ring sized once per session
(`gc_wt_store_bytes`, `mod_bmodel.c`, already sized per map for Lost Coast) with a bound
that covers the largest pack plus slack, and a Dolphin run on caen to check for restreaming.
This changes the ARAM layout, so it has not been made.

### Known refusals (M, every map in the logs)

- `mem FAIL subsystem=FileSystem Pool at=../filesystem/io.c:56`, 65-193 KiB:
  charlie 5, avalanche 1, caen 4, zalec 8.
- `ARAM animpack refused stream need=3.55 Mb`, largest free chunk 1.1-2.3 MiB.
- `CMPR slots skipped: budget 0 < 8 slots` (avalanche).
- The `ARAM <stage> session= stream= free=` report line is not in these logs;
  the build that produced them predates it.

## One memory plan: full detail (A), network (B), hi-res HUD and text (C)

Goal (Tim, 2026-11-18): all three fit together, with named pools and ARAM streaming, without
dropping content. Fixed inputs from the menu/HUD thread: output frame 640x480, one XFB of
600 KiB in the SYS arena, HUD cells at full resolution, 640-wide menus. Those are not traded.
M = measured (ELF or Dolphin log), R = read from code, D = derived, P = planned (not built).

### MEM1 (24 MiB = 24,576 KiB)

Static image on master after the playermove caps (PR 240): 22.35 MiB measured at d23b9c58a
minus the 360.7 KiB saved = about **22,525 KiB (22.0 MiB)** (M, D). The remaining tail is about
**2,050 KiB** and holds the malloc heap, the XFB and the stack.

| Pool | KiB | Group | Basis |
|---|--:|---|---|
| XFB (one, 640x480 RGB) | 600 | all | M (malloc trace: `GC_AllocXFB` 600.0 KiB) |
| FileSystem Pool arena | 192 | all | M (`Mem_InitPool` 192 KiB); peak use about 104 KiB |
| ISO9660 mount | 34 | all | M |
| `_Mem_Alloc` pool tables | 80 | all | M (32 + 48 KiB) |
| Durable hot window | 96 | A | M (`R_GCEnsureDurableHotWindow`) |
| HUD texture pool (3 slots of 128x128) | 96 | C | R (`GC_GX_TEX_HUD_RESERVE` = 3, 32 KiB each) |
| Hi-res HUD cell arena | up to 64 | C | R (`GC_HUDHI_ARENA_MAX`) |
| Menu glyph atlas 512x256 I4 | 64 | C, menus only | menu thread |
| In-match glyph atlas | 64 (ceiling 128) | C | P (branch `hitext-ingame`) |
| 256 px view-model skin window | 192 | A | R (`GC_SKIN_WINDOW_BYTES`); **gated on free heap >= 704 KiB, so it is never allocated at map active** |
| World reserves held at Host_Main | 400 | boot | M; released before SV_Init, not resident in a match |
| Server slot table (5 slots) | about 45 | B | R (about 9 KiB per slot) |

Heap at map active (M, earlier logs, which predate PRs 240/241): heap_free **140-220 KiB**,
unclaimed 0-56 KiB, on charlie, avalanche, caen and zalec. PR 240 added 360.7 KiB of tail to
this, but those logs were taken on an image 0.69 MB smaller than master, so the net is not
yet re-measured.

### BSS by group (M, ELF)

| Group | BSS | Notes |
|---|--:|---|
| Map-load arena (`vid`) | 3.69 MiB | holds the BSP, world arrays, texture pool tail |
| Client entities | 744 KiB + cl 638 KiB | 460 edicts, not idle |
| Server (`sv_`, edicts, baselines, clipnodes) | about 1.5 MiB | 460 edicts |
| Studio (`gc_studio_bss`) | 648 KiB | |
| Network (lwIP memp 218 + mem 192 + `gc_sv_netbuf` 128) | 538 KiB | **B**: another thread is slimming lwIP |
| Effects pixels (`gc_fx_pixels`) | 208 KiB | **C** (effect sprites) |
| FIFO, soft 2D buffers, view/Z buffers | about 0.9 MiB | Z is borrowed by bloom |
| playermove_t x2 | 274 KiB (was 635) | PR 240 |

### ARAM (16 MiB = 15.98 MiB usable)

| Owner | MiB | Group | Basis |
|---|--:|---|---|
| audio | 1.00 | A | M |
| wtstore (world texture ring) | 1.4-3.5 | A | M logs show 6.0; the layout branch sizes it to pack + margin (1.84 on charlie), packs are 1.39-3.22 |
| world display lists (`worlddl`) | 1.5-3.0 | A | M |
| light faces (`lightfaces`) | 1.9-3.0 | A | M |
| props + proptex | 0.12-0.27 | A | M |
| player animation packs, 2 team classes | 7.1 | A | M (3.55 each; refused today on avalanche and caen, streamed from disc) |
| player mesh packs (.gcm) | 0.27-0.35 | A | M |
| asset cache (hi-res HUD cells, skins, studio, HUD sprites) | up to 4.0 | A, C | M ceiling; 1.49 MiB in use at map-ready on charlie |
| prefetch (menu idle) | 1.03 | menu | M; held for the session on the menu route |
| 256 px skins (CMPR 256x256, 32 KiB each) | under 1 | A | D, about 10 bodies + viewmodels |
| sfx / lstyles / saves | 2 / 1 / 4 | not seen in DoD logs | design doc |

**Sum on the worst measured map (caen), full detail:** audio 1.0 + wtstore 3.5 + worlddl 3.0 +
lightfaces 3.0 + props 0.27 + mesh 0.27 + animation packs 7.1 + cache ceiling 4.0 =
**22.1 MiB against 15.98**, so about **6.1 MiB over** at the ceiling and about 3.6 MiB over with the
cache at its measured 1.5 MiB (D). That is why both animation packs cannot be resident today and
why the 256 px skins have no guaranteed room.

### What does not fit, and the cheapest ways to make it fit (D, ranked by KiB per risk)

MEM1, needed beyond today's free heap: 192 KiB (256 px skin window) + 64-128 KiB (in-match glyph
atlas) = **256-320 KiB**, against 140-220 KiB free at map active plus the 360.7 KiB from PR 240.

1. **PR 240 BSS saving, 360.7 KiB, done.** Give it to the skin window and the glyph atlas as named
   pools (a static or boot-arena block, not general heap). Risk: low.
2. **FS arena 192 to 112-128 KiB: 64-80 KiB.** The pool peaks near 104 KiB. Risk: low to medium
   (pool setup belongs to the boot-sizing thread).
3. **Skin window from the map-load arena or the tail instead of a free-heap gate: 192 KiB made
   reachable.** The current gate (704 KiB of free heap) can never pass in a match. Risk: medium.
4. **Server packet entities and client slot table trims: 0-45 KiB.** Risk: low, small.
5. **lwIP buffers (538 KiB static):** owned by the network thread; any cut is added to the pool above.

ARAM, over by 3.6-6.1 MiB, cheapest first:

1. **Cache ceiling 4 MiB to a sized budget (about 2 MiB): 2 MiB.** The cache already evicts. Risk: low.
2. **Stream one animation pack from disc (keep the other in ARAM): 3.55 MiB.** Already the fallback;
   costs a seek per new bot sequence. Risk: low for correctness, medium for frame time (unmeasured).
3. **wtstore sized per map: 2.5-4.6 MiB. In the layout PR (`gc-mem-aram-layout`).** Risk: low; the
   changelevel realloc path is new and still needs one Dolphin run.
4. **Drop the menu prefetch after load: 1.03 MiB island.** In the same PR. Risk: low.
5. **Lightfaces and worlddl partial residency (stream the half not in view): 1-2 MiB each.** Risk:
   high (render path), not started.
6. **Share one animation pack across teams where the sequences are identical:** unknown; needs the
   split tool to report duplicate sequences. Risk: medium.

Per-bot cost (R, M): about 3 edicts per bot (limit is the 460 edicts, not memory), about 9 KiB of
server slot, and about 0 ARAM per extra bot because packs and meshes are shared by name. Streams
from disc: animation sequences when a pack is not in ARAM, the 193 KiB sprite that does not fit
the staging buffer, the world texture pack entries beyond the ring.

## Runtime markers currently consumed

The existing MEM1 telemetry is useful and is consumed without changing its
meaning:

```text
Xash3D GameCube: mem stage=<stage> total=<size> delta=<size> hwm=<size> map=<map>
Xash3D GameCube: map-load pressure stage=<stage> peak=<size> delta=<size> base=<size>
Xash3D GameCube: mem FAIL subsystem=<name> size=<size> map=<map> at=<file>:<line> total=<size> hwm=<size>
```

These lines describe the allocator/accounting path currently instrumented.
They do not prove that all allocations are in MEM1, do not expose MEM2 unless
the platform allocator reports it, and do not partition renderer/audio/cache
usage without subsystem tags.

## Acceptance rules

Memory pressure is a secondary hypothesis for the map failure, not a license to
hide the filesystem failure or skip `Delta_Init()`. A memory claim is accepted
only when it includes:

1. artifact identity and ELF/DOL section data;
2. linker-map evidence or an explicit missing-map result;
3. fresh runtime high-water data for the named map and route;
4. the largest failed request and owning subsystem, when a failure occurred;
5. separate MEM1 and MEM2 fields, with `UNAVAILABLE` where the runtime did not
   measure the region.

Until those fields exist, use the wording **“memory pressure is plausible but
unproven”** and continue debugging the first failing filesystem/map ladder
gate.
