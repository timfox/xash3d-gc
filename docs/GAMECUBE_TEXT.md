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
