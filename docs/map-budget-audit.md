# DoD map budget audit (2026-11-19)

Scope: all 22 shipped DoD maps. Sizes are KiB, measured from the local bake caches
(`~/xash3d-gc-master54/build/gc-{wd,gp,cf}-cache`) with `scripts/gamecube-texture-budget-audit.py`-style
tooling plus a one-off size/zlib script. "zlib" is zlib level 6 over the whole file, an upper bound on what a
per-chunk LZ4/deflate would give. Not measured here: heap-after-load and ms/frame in Dolphin (see "Not done").

## 1. Per-map cost

| map | BSP | world DL wd | wd zlib | wd_lf | prop pack gp | gp zlib | prop tex pt | pt zlib | cf cells |
|---|---|---|---|---|---|---|---|---|---|
| dod_anzio | 4440 | 2793 | 746 | 2620 | 33 | 15 | 113 | 85 | 50 |
| dod_avalanche | 3682 | 2544 | 694 | 2152 | 56 | 23 | 64 | 52 | 44 |
| dod_caen | 5295 | 2993 | 850 | 3086 | 107 | 49 | 168 | 134 | 64 |
| dod_charlie | 3934 | 2454 | 549 | 1942 | 188 | 81 | 303 | 188 | 64 |
| dod_chemille | 3782 | 2542 | 693 | 2203 | 70 | 33 | 169 | 115 | 44 |
| dod_donner | 3336 | 2199 | 568 | 1891 | 53 | 25 | 112 | 84 | 57 |
| dod_escape | 3933 | 2354 | 642 | 2581 | 168 | 75 | 518 | 368 | 60 |
| dod_falaise | 3541 | 2390 | 705 | 1864 | 227 | 106 | 570 | 412 | 64 |
| dod_flash | 3128 | 1911 | 482 | 1713 | 34 | 17 | 123 | 86 | 38 |
| dod_flugplatz | 3839 | 2354 | 638 | 1998 | 97 | 44 | 375 | 279 | 64 |
| dod_forest | 3975 | 2141 | 586 | 2392 | 75 | 38 | 185 | 131 | 64 |
| dod_glider | 3822 | 2310 | 675 | 1917 | 169 | 76 | 565 | 381 | 60 |
| dod_jagd | 4890 | 2770 | 810 | 3124 | 94 | 47 | 227 | 176 | 64 |
| dod_kalt | 3579 | 2013 | 470 | 2141 | 39 | 19 | 160 | 125 | 51 |
| dod_kraftstoff | 3439 | 2482 | 689 | 1837 | 78 | 36 | 192 | 142 | 47 |
| dod_merderet | 3599 | 2471 | 682 | 2055 | 83 | 38 | 267 | 206 | 64 |
| dod_northbound | 3993 | 2435 | 647 | 2277 | 84 | 40 | 144 | 92 | 55 |
| dod_saints | 3580 | 2216 | 591 | 2354 | 195 | 87 | 675 | 470 | 59 |
| dod_sturm | 3746 | 2344 | 670 | 2368 | 173 | 82 | 511 | 363 | 64 |
| dod_switch | 4773 | 2696 | 787 | 2813 | 96 | 43 | 267 | 193 | 45 |
| dod_vicenza | 4639 | 2640 | 723 | 2771 | 48 | 22 | 203 | 132 | 64 |
| dod_zalec | 5686 | 3118 | 976 | 3498 | 152 | 66 | 390 | 298 | 64 |

Totals (22 maps): BSP 88,631; world DL wd 54,170 -> 14,873 zlib (3.6x); prop pack gp 2,319 -> 1,062 (2.2x);
prop textures pt 6,301. `wd_*_lf` files are the lean (fallback) variant of the same DL; `cf cells` is the
CapFaces spatial cell count (cf-cache: 65 MB on disk, 2,730 -> 635 KiB zlib for avalanche, 4.3x).

## 2. What compresses and what does not

| Asset | Ratio (zlib) | Verdict |
|---|---|---|
| world DL `wd_*.gcd` (3.07 MB caen) | 0.27 (zstd-1 0.32, zstd-3 0.30) | **Best target.** Vertex/index runs are very redundant. |
| CapFaces cells `cf_*.gcp` | 0.23 | Compress per cell; load-time only. |
| prop packs `gp_*.gcp` | 0.46 | Small absolute win (about 1.2 MiB over all maps). |
| prop textures `pt_*.gct` | 0.7-0.75 | Already CMPR; modest. |
| world textures `wt` (sample of 400 cache entries) | 0.81 | CMPR is dense; gain must come from **dedup/mip reuse**, not codecs. |
| anim packs | 0.63 (done: GCAP v2, LZ4, 3.55 -> 2.25 MiB per team) | In branch gc-mem-engine-all. |

## 3. Coverage gaps found (from the task brief and code)

- falaise: prop chains want 222 KiB against the 180 KiB chain cache (r_gx_world.c chain cache); needs the cache
  to grow from the map arena tail / spill, or chains split by distance.
- dod_glider: refused by the stager at 39 studio MDLs against a cap of 32.
- 4-bot 48-59 FPS on caen/avalanche/glider is world texture reads (owned by the "Polish maps" texture cache
  thread; deliberately not duplicated here).
- Several maps have no baked cf cells in the earlier probe (charlie, chemille, donner, falaise: "dens none").

## 3b. Where the world pack's bytes really are (measured, caen; the other maps are alike)

| region of wd_*.gcd | KiB | zlib KiB | verdict |
|---|---|---|---|
| lightmap pages (512 x 2 KiB, always full) | 1024 | 115 (per page LZ4: 293) | **compress** (done, below) |
| PVS rows | 596 | 97 whole / 508 per row | per-row compression gains nothing (rows are already RLE); the zlib gain is cross-row, so it needs row dedup or delta coding, not a codec |
| display-list chunks (2981 chunks, 400 B each) | 1157 | 507 | small chunks; in-place decode impossible (see below); would need a staging buffer per fetch |
| tree, planes, chunkdir | 187 | 114 | small |

Also unmeasured but large: the `wd_<map>_lf.gcd` light-face pack (1.7-3.5 MB a map, holds the BSP lighting lump)
lives in ARAM too and is the next candidate after this.

## 4. Done: per-page LZ4 lightmaps (DODGC_WD_LZ4=1, default off)

- Baker: `scripts/bake_worlddl_goldsrc.py --lz4` (or env `DODGC_WD_LZ4=1`, which the disc builder honours and adds to
  its cache key) packs each 2 KiB page with the new `scripts/gc_lz4.py` (pure python LZ4, no dependency).
  Header flag `0x0010`; layout in `pack_lm_pages_lz4`.
- Engine: `R_GXWorldDLLmFetch` (ref/gx/r_gx_worlddl.c) stages the blob in a 2 KiB static buffer and decodes it into the
  page slot with the shared `Mod_GCLZ4Decode` (made non-static in mod_studio.c). Files without the flag load as before.
  Cost: **+2 KiB of MEM1 BSS** (the staging buffer) and one LZ4 decode of at most 2 KiB per lightmap-page miss.
- Why not decode in place or compress the 400-byte display-list chunks: for an exact-fit buffer the last match always
  ends one byte past the read pointer, however the stream is cut (token byte of the final literal run), so in-place
  needs a larger buffer than the slot. I first built a per-chunk in-place version, measured that 0 of 2,981 caen
  chunks qualify, and dropped it.
- Measured sizes (22 maps, from the local bake caches): lightmap region **22.0 MiB -> 6.0 MiB** in total, per map
  1024 KiB -> 220-325 KiB, so **about 0.7 MiB of ARAM freed per map** and a wd file 25-40% smaller
  (caen 2993 -> 2273 KiB, zalec 3118 -> 2417, flash 1911 -> 1210). Pure-python bake time about 1 s a map.
- Tests: `tests/test_wd_lm_lz4.py` (4 tests): round trip of synthetic pages (zero, flat, smooth, incompressible-stays-raw)
  and every 37th real caen page through the ENGINE's decoder source compiled with gcc, directory layout, and a
  corrupt-block case. Full host suite: 207 passed, 5 skipped.
- dod-gc: stager default caps raised from 32 models / 4 MiB to 48 models / 6 MiB (branch `mapmodel-cap-48`,
  tip 24fd9c5, off dod-gc main f269158). dod_glider refs 39 MDLs = 5.1 MiB; saints (40), falaise (35), escape (35) and
  sturm (34) were also over the old count cap.

## 4b. Done: LZ4 `_lf` light-face pack (GClf version 2, same opt-in `DODGC_WD_LZ4=1`)

- Both parts of `wd_<map>_lf.gcd` (56-byte face records, BSP lighting lump) become random-access LZ4 regions of
  1 KiB blocks (`pack_blocks_lz4`, shared with the lightmap pages). Version 1 files load unchanged.
- Engine (`R_GXLfBlock` / `R_GXLfRead2` in ref/gx/r_gx_worlddl.c): a block is staged and decoded into one of three
  1 KiB cache buffers (one for records, two for lighting because each light style is read far from the last).
  Cost **+4 KiB of MEM1 BSS** and one LZ4 decode of at most 1 KiB per cache miss in the light-point walk.
- Measured on 6 maps (baked from the BSPs): anzio 2620 -> 1239 KiB, caen 3086 -> 1740, flash 1713 -> 1082,
  zalec 3498 -> 2139, kalt 2141 -> 1167, glider 1917 -> 1308; total 14.6 -> 8.5 MiB (-42%), about 1 MiB of ARAM a map
  on top of the lightmap saving.
- Tests: `tests/test_lf_lz4.py` (random-access reads through the engine's decoder source compiled with gcc,
  short/odd lengths, and a real dod_flash bake: v2 reads equal v1 bytes). Host suite 211 passed, 5 skipped.
- Not measured: Dolphin (ms per light-point query with cache misses), heap, ms/frame.

## 4c. Done (PR pending): LZ4 PVS rows (header flag 0x20, same `DODGC_WD_LZ4=1`)

- Exact duplicate rows are only 0.3% and one-level XOR delta vs a base row saves about 15%, so row dedup/delta was
  dropped. The big gain is cross-row: LZ4 over 4 KiB blocks of the PVS blob takes caen 596 -> 204 KiB, charlie 618 -> 178,
  avalanche 424 -> 128, zalec 483 -> 156 (about -66%, roughly 0.4 MiB of ARAM a map).
- Baker pads the blob to whole 4 KiB blocks and packs it with `pack_blocks_lz4`; row offsets in the pvs directory stay
  raw offsets. Engine: `R_GXWorldDLPvsByte` / `R_GXWorldDLLoadPVSLz4` read the row through one cached 4 KiB block.
  MEM1 +4 KiB for the block cache; the 4 KiB staging buffer is shared with the lightmap and light-face decoders
  (which no longer have their own, so the three LZ4 paths together cost 8 KiB + 3 KiB of caches).
- Tests: `tests/test_pvs_lz4.py` bakes dod_flash both ways and compares raw rows through the engine decoder.

## 4d. Dolphin measurements, LZ4 off vs on (headless `-v Null`, 4 bots, own user dir; DoD DOL from master)

| charlie | flag off | lightmap + lf | + PVS |
|---|---|---|---|
| world pack in ARAM (wd) | 2626 KiB | 1869 KiB | 1430 KiB |
| light-face pack in ARAM (lf) | 1989 KiB | 1142 KiB | 1142 KiB |
| ARAM total of the two | 4615 KiB | 3011 KiB | 2572 KiB (-2.0 MiB) |
| heap at the same boot points (KB) | 453 / 227 / 211 | 453 / 226 / 210 | not compared |
| `render: GX world faces` mean per frame | 12.40 ms | 11.91 ms | 12.65 ms |
| lightmap page misses in the log samples | 0 | 0 | 0 |

- Caen and avalanche: their file sizes shrink the same way (lf: avalanche 2204 -> 1221 KiB), but the world-draw times of
  the off and on runs are not comparable: the 150-235 s runs reached different points of the game (caen never reached
  world-draw frames headless, avalanche's `world faces` ranged 1.8-9.9 ms by phase). Heap at matching boot points was
  within 1-2 KB. Headless Null time is emulated-clock timing, not GPU time; treat ms/frame here as "no visible cost",
  not as a frame-time guarantee. A fixed camera path on the GPU queue would settle it.
- A second pass was invalidated (two copies of my script ran together) and discarded; the figures above are from clean runs.
- Dolphin with the PVS build: world renders, 237 visible chunks vs 230 in the earlier run, no LZ4 or PVS failure lines.
- Reasons for not doing the rest of the list: texture dedup across maps frees nothing at run time (a map loads only its
  own wt file; it would only shrink the disc image); per-chunk LZ4 sits on the hot chunk-refetch path (up to 500 KB a
  frame) so it needs a GPU-queue frame measurement before it is worth trying; gp/pt packs are about 50-70 KiB a map.
- Default: the disc builder now packs with LZ4 unless `DODGC_WD_LZ4=0` (4d PR).

## 5. Ranked next steps (not done)

1. **Rebake and measure.** Nothing here has run on a disc image or in Dolphin. Build a disc with `DODGC_WD_LZ4=1`,
   run the 12-bot recipe, check "world DL" and ARAM segment logs, and compare lightmap miss cost (`gc_wd_lm_miss`).
   Only then flip the default on.
2. ~~LZ4 the `_lf` light-face pack~~ done in 4b.
3. ~~PVS cross-row dedup/delta~~ done as block LZ4 in 4c.
4. **Falaise chain cache (222 > 180 KiB):** on DoD the cache is `min(all chains, spill room - margin)`
   (r_gx_world.c, "A map pack" branch), i.e. it is MEM1-spill bound, not a constant; the fix is more spill room or a
   working-set policy, which needs the Dolphin heap/ms measurements. Not changed.
5. **gp/pt/cf packs:** `gp` 2.3 MiB -> 1.1 MiB, `cf` cells 4.3x are load-time/disc only; not touched (no loader change
   made). **Texture dedup across maps:** not measured (wt cache entries are content-hashed, 2,639 files).

## 6. Branch state and what was not done

- Engine: branch `gc-mem-rebased-master` in /home/tim/gc-mem-wt2 (origin/master 121ec08515 + gc-mem-engine-all merge
  + audit + lightmap LZ4). **Not pushed**: the push was denied by the permission classifier. Tim runs
  `git -C /home/tim/gc-mem-wt2 push -u origin gc-mem-rebased-master` and opens the PR (body must start with the two
  attribution lines).
- dod-gc: `git -C /home/tim/gc-mem-wt-dod2 push -u origin mapmodel-cap-48` (worktree /home/tim/gc-mem-wt-dod2); branch
  gc-mem-dod-integ9 is untouched and unpushed.
- **No Dolphin run, no heap KB or ms/frame measured**; no disc image built. Boot disk and Expansion1 are at 99-100%.
  Sizes above are measured file sizes; ARAM freed is computed from them.
- Not done: gp/pt/cf compression, texture dedup, falaise chain cache, `_lf` pack, PVS dedup.
