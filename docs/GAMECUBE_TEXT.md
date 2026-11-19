# GameCube text: where resolution is lost

The DoD route renders into a 320x240 frame (`-width 320 -height 240`) that Dolphin/the TV scales 2x to 640x480.
HUD and menu art is laid out for 640x480 and scaled by 0.5. Every text path starts from a small bitmap, so none of
them can be sharper than the 320x240 frame; the table lists what each path starts from.

| Text | Source | Size at 320x240 | Notes |
|---|---|---|---|
| Main menu labels (New Game, Find Servers, Options) | baked button strips `resource/gc_menu/btn_*.tga` | as baked | unchanged by design |
| Menu pages (Options, Create Server, lists, hints) | mainui `CBitmapFont`: every glyph is `pfnDrawCharacter(0, ...)` of the engine **credits font atlas** scaled to 18 px (default) / smaller for help text | 18 px | soft: a 128 or 256 px wide atlas scaled up |
| Console, chat history, `DrawConsoleString` | the same credits font atlas | ~9-12 px | |
| HUD digits, health, ammo | HUD sprite sheets (hi-res cells from ARAM where they fit) | | |
| On-screen keyboard, chat lines, build stamp | built-in 5x8 bitmap font, drawn as rectangles (`in_osk.c`) | 1 real px per font px (keys 2) | pixel-exact at 320x240, small |

## What changed
`CL_GCLoadLeanHudFont` kept `force_lean = true`, i.e. the 128 px wide atlas, "for memory". The full atlas
(`gfx/gc_creditsfont256.fnt`, 256x64 RGBA = 64 KiB texture) is loaded instead; the lean bake stays as the fallback when
the full one cannot be allocated. Measured in Dolphin: boots to the menu and into a dod_charlie match without a
`mem FAIL` or a font fallback log line.

## What is still soft
- Everything is bounded by the 320x240 frame. Real sharpening needs a 640x480 text layer (EFB 640x480 is available
  in hardware; the cost is the software RGB565 2D buffer, 600 KiB instead of 150 KiB) or drawing text as GX quads
  from a 640-sized atlas instead of into the soft buffer.
- The keyboard font is exact at 320x240 but only 5x8; a larger bitmap (8x14) at 2x would need 640 output.

## Hi-res menu text (phase 1, implemented)
- `scripts/bake-hitext.py` bakes `scripts/gc_hitext.bin`: ASCII 32..126 from **Noto Sans Regular** (SIL Open Font
  License 1.1, free to ship; the licence text must accompany the font data) into a 512x256 I4 atlas (64 KiB) with
  glyph metrics. The disc builder stores it as `gfx/gc_hitext.bin`.
- `engine/platform/gamecube/hitext_gamecube.c`: mainui glyphs (`pfnDrawCharacter`, hFont 0) are queued and drawn as
  GX quads at the EFB resolution after the soft frame, right before `CopyDisp` (soft present and Flipper present).
  A filled box drawn after text (`pfnFillRGBA`, alpha >= 160) drops the glyphs under it (dialogs). The queue and the
  atlas are not used while the on-screen keyboard is open, in a match (the atlas is freed once `ca_active`), or when
  `gc_hitext 0`. The main menu draws its labels the old way (mainui sets `gc_hitext_pause` around its `Draw`).
- The network-build video mode used a 320-wide XFB (`GC_NetworkVideoMode`), so the EFB was 320x480 and quads could not
  be sharper than the soft buffer. It is now 640 wide. **Cost:** +300 KiB: the network build allocates ONE XFB (`GC_AllocXFB` skips index != 0 under `XASH_DODGC_NETWORK`), 320x480x2 = 300 KiB becomes 640x480x2 = 600 KiB, taken from the SYS arena (`SYS_AllocateFramebuffer`), not the game heap.
  A dod_charlie match still loads without `mem FAIL` in Dolphin; hardware memory headroom is NOT verified.
## Hi-res text: what the code says (read from vid_gamecube.c / ref_gx)

- The hardware frame is already native 640x480 (`TVNtsc480Prog`, EFB/XFB 640x480). Only the **soft 2D buffer** is
  320x240 (`-width 320 -height 240` in `sys_gamecube.c`: "Tip-safe soft/internal FB. Flipper EFB/XFB stay native
  640x480 via VI").
- 3D (pure-GX world frames) is drawn by GX into the 640x480 EFB and copied to the XFB (`CopyDisp`).
- Everything 2D (menu, HUD sprites, text) is rasterised by the software path in `ref/gx/r_draw.c` into the 320x240
  RGB565 buffer, and `GC_PresentBuffer` converts and upscales that to the XFB with the CPU (menu screens), or the HUD is
  drawn over a held GX EFB (match frames). So all text is soft 320x240 text, doubled.
- Real sharp text therefore means moving **text** (not the whole 2D layer) to GX quads at 640x480: a 640-sized glyph
  atlas in a GX texture and a quad per glyph, drawn after the soft layer.

## Plan (phases; each needs Dolphin proof and a frame-time check)
1. Menu screens: present the soft 320x240 buffer as one GX textured quad into the EFB (instead of the CPU
   upscale/YUY2 convert; also bilinear), then text quads, then `CopyDisp`. Cost: a 153 KiB RGB565 texture upload per
   frame, no extra RAM (the XFBs already exist). This path also removes the CPU convert from the menu frame.
2. Glyph atlas: bake a 640-scale atlas offline (TTF -> IA4 texture + metrics, shipped in the disc archive), ~128-256 KiB
   in GX/ARAM-backed memory; the credits atlas stays as the fallback.
3. Route `pfnDrawCharacter(0, ...)` (mainui), `Con_DrawString`/`DrawHudString` (HUD, chat, kill feed, scoreboard) and the
   on-screen keyboard to the quad path; text that is overdrawn by later soft content (dialogs over text) needs an
   ordering rule (queue text with the soft layer's draw order and flush after the soft blit, clipping by the dialog rects).
4. Match frames: text quads after the HUD blit over the held EFB; measure 60 FPS and heap after a dod_charlie load.

Not implemented yet: phases 1-4. Phase 1 changes the present path (`GC_PresentBuffer`, many diagnostic modes) and
should be done and verified in Dolphin before anything depends on it.

## Memory cost of the UI work (MEM1 resident / SYS arena / ARAM)
| Item | MEM1 resident | SYS arena | ARAM |
|---|---|---|---|
| Hi-res menu atlas (512x256 I4, `gfx/gc_hitext.bin`) | 64 KiB while a menu is up, freed when the match starts | | not used (streaming not implemented) |
| 640-wide XFB | | +300 KiB (one XFB) | |
| Credits font atlas 256x64 | 64 KiB (was the 128 px bake) | | |
| Menu pages, panels, build stamp | no allocations | | |
| In-game glyph atlas (planned) | 64-128 KiB target | | rest of the atlas |
Measured in Dolphin on dod_charlie / avalanche / flash / caen: no `mem FAIL` at 320 or 640 XFB; heap_free on charlie 328.9 KB vs 328.2 KB. Console hardware headroom is not measured.

## Hi-res text in a match (phase 3, behind a cvar, not yet verified in Dolphin)
`gc_hitext_ingame 1` keeps the same 64 KiB I4 atlas resident during a match and routes `CL_DrawCharacter` (HUD, chat,
kill feed, scoreboard, console and the team/class text menus all end up there) to the GX glyph queue, flushed before
`CopyDisp`. Default 0: the atlas is freed when the match starts, as before. **Memory when on: +64 KiB MEM1 resident**
(inside the 64-128 KiB target; no ARAM paging needed at this size). If the queue (900 glyphs) fills, glyphs fall back to
the soft path. Not measured: frame time with it on, heap on the heaviest maps.

## Button glyphs in hint rows
Eight original controller-button glyphs (A, B, X, Y, Start, Z, L, R: solid circle / pill shapes with the letter cut out,
drawn by `scripts/bake-hitext.py`, no console artwork) live in the hi-res atlas at codes 128..135, after an empty DEL cell,
so the glyph table is 104 entries (was 96). The caller colours them (A green, B red, X and Y light, Z violet). mainui's
`CMenuHintRow` (`{A}` `{B}` ... tokens in the hint string) draws them through `pfnDrawCharacter`, which reaches
`GC_HiTextGlyph`; with `gc_hitext 0` the token prints as `[A]`.
Memory: the atlas file grows by 112 bytes of glyph table (67 008 bytes, still one 64 KiB I4 texture); nothing else.
Host test `tests/test_gamecube_hitext_buttons.py` checks the glyph table. Not seen in Dolphin.

## Notices and the fatal panel (retail palette, inside the safe area)
- **Controller disconnected:** `OSK_DrawNotice` (in_osk.c, drawn with the keyboard) shows a centred panel when no native pad
  has been present for about 0.75 s (`GC_PadMissingNotice`, never for automated probes). Layout is `osk_notice.h`, host test
  `tests/test_gamecube_osk_notice.py` (stays inside 32 x 24 px margins). Memory: none (soft FillRGBA + the keyboard's
  bitmap glyphs, or hi-res glyphs when the atlas is up).
- **Fatal / Host_Error panel:** the magenta 160x120 panel is now ViewportBG with a ClassMenuLight border on the safe area
  (x 8..151, y 6..113 of the panel), title "DAY OF DEFEAT - ERROR". It still uses a static 38 KiB panel buffer (unchanged).
- Not seen in Dolphin: needs a pad-less run to show the notice.

## Progressive scan prompt at boot (cost: none; not seen in Dolphin)
`GC_BootProgressivePrompt` (vid_gamecube.c) runs on the early boot splash: only on an NTSC console with a component cable
(`VIDEO_HaveComponentCable`) and B held at boot, as GameCube releases do. It draws a retail-palette panel inside the safe
area ("Use 480p? ... A Yes  B No", `No in N s`); A = yes, B / Start / 10 s timeout = no. The answer lives in a static for the
session and makes `GC_ResolveVideoMode` pick `TVNtsc480Prog`.
- **XFB:** 640x480 480p and 480i frames are the same size (600 KiB), so no XFB allocation changes; the switch is refused (and
  reported) if a mode ever needed a larger XFB than the one already allocated.
- **SYS arena / ARAM:** unchanged. **.bss:** unchanged; the prompt shares the 160x120 panel buffer the fatal breadcrumb already
  had (it moved to file scope as `gc_text_panel`).
- Not verified: the 480p presentation path (GX copy filter, vfilter) beyond what `-gcvideo prog` already exercised, and the
  prompt on screen (Dolphin needs a component cable setting: Config > GameCube > progressive scan, plus B held).
