# Handoff from maintainer → maintainer (2026-11-06)

Status: received by maintainer. Act on this; maintainer cannot reach this session directly.

## Retail cold-boot freeze (owner: maintainer)

The retail cold boot freezes right after the first intro frame. The bisect points to **56f7be4**: the BSS edict/baseline/static tables take ~350 KB of boot headroom (main-loop free **487 KB → 129 KB**). **1b27580** boots to the menu fine.

Alternative approach: `docs/notes/gamecube-edict-hold-shelved.md`.

### Probe / process rules

- Pull **master**.
- Use the shared Dolphin probe lock: `/dev/shm/xash3d-gc-dolphin-probe.lock`, `DOLPHIN_PROBE_LOCK_WAIT=<sec>`.
- **Do not** `pkill` / `killall` by pattern — that has been killing the maintainer menu session’s runs.

### ARAM question (needs maintainer answer)

Is it OK for the menu to hold a **3.6 MB ARAM** block (animated `logo.avi` banner), reserved after the intro video window is freed and released at play start before the map window? Any conflict with maintainer’s menu-idle prefetch?

Until that’s sorted, the banner stays a still frame, and maintainer is testing the **c0a0 → c1a1** level chain (no menu needed).

## maintainer parallel status (do not block on these)

- Playthrough c0a0 → c1a1 as level hops (no menu) — finding blockers
- Animated title banner — still frame until ARAM answer / boot fix
- New Game from the retail menu on master — **waiting on maintainer’s boot fix**
- World display lists behind a toggle — parked
