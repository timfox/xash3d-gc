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
