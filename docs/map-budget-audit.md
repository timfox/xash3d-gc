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

## 4. Ranked plan (cheapest, safest first)

1. **Per-chunk LZ4 for `wd` (ARAM prefix and disc stream).** 54 MB -> about 15-27 MB of ARAM/disc across maps;
   per map about 1.5-2.1 MiB (caen 3.0 MiB -> about 1.0 with zlib-class, LZ4 will be a little worse). Decode on
   chunk fetch into the existing spill/bounce ring; the chunk dir stores compressed length. Same approach as the
   already-merged GCAP v2 decode-in-place. Frees ARAM that the memory plan lists as 3.6-6.1 MiB short.
2. **LZ4 for `gp`/`pt` and `cf` cells.** About 1.2 MiB (gp) and 2.1 MiB per big map (cf) of ARAM/disc.
3. **World-texture dedup across maps** (the wt cache is content-hashed, 2,639 entries, 73 MB): measure shared
   hashes per map pair before changing the ring.
4. **Chain-cache cap fix** for falaise (222 > 180 KiB) and **stager cap 32 -> 40** for glider (39 MDLs), both
   small constants/guards plus a disc build check.
5. Instancing/streaming of repeated studio models once the above lands.

## 5. Done in this task

- Checked master: it does **not** contain the gc-mem work (2edbfb57ba is not an ancestor of origin/master).
- Merged `gc-mem-engine-all` (82 commits: LZ4 anim packs, lazy/streamed 256 px VM skins, nav and entity text in
  the map arena, weapon box pool, packet ring sizing) onto origin/master 121ec08515 with no conflicts: branch
  `gc-mem-rebased-master`, tip 5effa689b3, worktree /home/tim/gc-mem-wt2. Host python tests: 29 test files, all pass.
- Audit table and compression measurements above.

## 6. Not done (be explicit)

- **Not pushed, no PR**: the push of `gc-mem-rebased-master` was denied by the permission classifier. Tim runs:
  `git -C /home/tim/gc-mem-wt2 push -u origin gc-mem-rebased-master`, then opens the PR (body must start with the
  two attribution lines). dod-gc branch gc-mem-dod-integ9 (/home/tim/gc-mem-wt-dod) is untouched and also unpushed;
  dod-gc local checkout is on another branch with 8 unpushed commits.
- No new compression code was written and **no heap KB or ms/frame was measured**: no Dolphin run was made, the
  boot disk (17 GB) and Expansion1 (9 GB) are at 99-100% and a combined build needs headroom. The numbers in
  section 2 are file-size measurements only; the gains in section 4 are estimates, not results.
- gamecube-heap-census.sh and the 12-bot recipe were not run.
