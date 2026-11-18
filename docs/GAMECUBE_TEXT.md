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
