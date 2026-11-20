#!/usr/bin/env python3
"""Generate GX/TEV Material Lab textures (PNG) for gxtexconv."""
from __future__ import annotations

import math
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageEnhance

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
TEX.mkdir(parents=True, exist_ok=True)

# Reference-matched GameCube purple (measured from Material Lab ref @640)
PURPLE = (91, 82, 200, 255)
PURPLE_DARK = (58, 42, 114, 255)
PURPLE_MID = (88, 70, 170, 255)
PURPLE_LIGHT = (150, 130, 220, 255)
PURPLE_LINE = (89, 82, 236, 255)
DARK = (18, 17, 20, 255)
BTN_A = (46, 180, 70, 255)
BTN_B = (210, 48, 48, 255)
BTN_X = (120, 90, 200, 255)
BTN_Y = (230, 220, 80, 255)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf" if bold else "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-B.ttf" if bold else "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def save_rgba(img: Image.Image, name: str, size: tuple[int, int] | None = None) -> None:
    if size is not None:
        img = img.resize(size, Image.Resampling.LANCZOS)
    # pad to power-of-two already expected by callers
    out = TEX / name
    img.save(out)
    print(f"wrote {out} {img.size}")


def gen_checker(n: int = 512, tiles: int = 8) -> None:
    """Warm mid-grey checkers closer to the copper reference floor."""
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cell = n // tiles
    dark = (58, 48, 48, 255)
    mid = (118, 102, 100, 255)
    for y in range(n):
        for x in range(n):
            a[y, x] = dark if ((x // cell) + (y // cell)) % 2 == 0 else mid
    Image.fromarray(a, "RGBA").save(TEX / "checker.png")
    print("wrote checker", n)


def gen_shadow(n: int = 256) -> None:
    """Darker, tighter contact shadow (reference oval reads almost black)."""
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cx = cy = (n - 1) * 0.5
    for y in range(n):
        for x in range(n):
            # Slightly elliptical like the reference contact shadow
            dx = (x - cx) / (n * 0.46)
            dy = (y - cy) / (n * 0.40)
            d = math.hypot(dx, dy)
            if d >= 1.0:
                continue
            t = (1.0 - d) ** 1.6
            a[y, x, 3] = int(235 * t)
            a[y, x, 0] = a[y, x, 1] = a[y, x, 2] = 0
    Image.fromarray(a, "RGBA").filter(ImageFilter.GaussianBlur(radius=3.5)).save(
        TEX / "shadow.png"
    )
    print("wrote shadow", n)


def gen_envmap(n: int = 256) -> None:
    """Sphere-map env: soft golden-hour courtyard (reference copper reflections).

    Drawn procedurally — no screenshot crop. Softer buildings, warmer sky,
    diffused sun, and a light plaza so copper tint reads peach/orange not magenta.
    """
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cx = cy = (n - 1) * 0.5
    for y in range(n):
        for x in range(n):
            nx = (x - cx) / cx
            ny = (y - cy) / cy
            r2 = nx * nx + ny * ny
            if r2 > 1.02:
                a[y, x] = (18, 16, 20, 255)
                continue
            z = math.sqrt(max(0.0, 1.0 - min(r2, 1.0)))
            elev = (-ny) * 0.92 + z * 0.08
            ang = math.atan2(nx, max(z, 1e-4))

            if elev > 0.22:
                # Warm twilight sky (less blue/purple than older map)
                t = min(1.0, (elev - 0.22) / 0.78)
                r = int(95 + 55 * t)
                g = int(70 + 40 * t)
                b = int(88 + 55 * t)
                # Diffused sun disc (upper-right) — soft for R≈0.25 copper
                sun = math.hypot(nx - 0.36, ny + 0.38)
                if sun < 0.14:
                    w = (0.14 - sun) / 0.14
                    w = w * w
                    r = min(255, int(r + 160 * w))
                    g = min(255, int(g + 145 * w))
                    b = min(255, int(b + 110 * w))
                elif sun < 0.34:
                    w = (0.34 - sun) / 0.20
                    r = min(255, int(r + 55 * w))
                    g = min(255, int(g + 40 * w))
                    b = min(255, int(b + 22 * w))
                # Soft cloud streaks
                clouds = 0.5 + 0.5 * math.sin(nx * 6.5 + ny * 3.2)
                if clouds > 0.78 and elev > 0.45:
                    w = (clouds - 0.78) / 0.22
                    r = min(255, int(r + 28 * w))
                    g = min(255, int(g + 24 * w))
                    b = min(255, int(b + 30 * w))
            elif elev > -0.08:
                # Soft distant architecture — low contrast so copper doesn't get a dark belt
                h = (
                    0.03
                    + 0.12 * abs(math.sin(ang * 3.6))
                    + 0.07 * abs(math.sin(ang * 7.5 + 0.7))
                    + 0.04 * abs(math.sin(ang * 13.0 + 1.1))
                )
                if elev < h:
                    shade = 0.45 + 0.20 * abs(math.sin(ang * 2.1))
                    r = int(72 + 28 * shade)
                    g = int(55 + 18 * shade)
                    b = int(50 + 14 * shade)
                    strip = 0.5 + 0.5 * math.sin(ang * 14.0) * math.sin((h - elev) * 28.0)
                    if strip > 0.88:
                        w = (strip - 0.88) / 0.12
                        r = min(255, int(r + 35 * w))
                        g = min(255, int(g + 22 * w))
                        b = min(255, int(b + 10 * w))
                else:
                    r, g, b = 96, 80, 78
            else:
                # Soft plaza — lightly blurred checkers (less literal floor mirror)
                gx = (nx * 0.5 + 0.5) * 10.0
                gz = ((z * 0.65 + elev) * 0.5 + 0.5) * 10.0
                # Smooth checker via fractional blend
                fx, fz = gx - math.floor(gx), gz - math.floor(gz)
                edge = min(fx, 1.0 - fx, fz, 1.0 - fz)
                soft = 0.35 + 0.65 * min(1.0, edge * 6.0)
                base = 0.55 + 0.20 * ((int(gx) + int(gz)) % 2)
                val = base * soft
                fade = 0.60 + 0.40 * max(0.0, z)
                r = int((95 + 55 * val) * fade)
                g = int((82 + 48 * val) * fade)
                b = int((76 + 42 * val) * fade)
            a[y, x] = (min(255, r), min(255, g), min(255, b), 255)

    img = Image.fromarray(a, "RGBA")
    # Stronger soften so building mass / plaza don't read as hard belts on copper
    sharp_rgb = img.filter(ImageFilter.GaussianBlur(radius=1.6)).convert("RGB")

    def mip_blur(src: Image.Image, factor: int, radius: float, contrast: float = 1.0) -> Image.Image:
        w, h = src.size
        small = src.resize((max(1, w // factor), max(1, h // factor)), Image.Resampling.BOX)
        back = small.resize((w, h), Image.Resampling.BILINEAR)
        if radius > 0:
            back = back.filter(ImageFilter.GaussianBlur(radius=radius))
        if contrast != 1.0:
            back = ImageEnhance.Contrast(back).enhance(contrast)
        return back

    sharp = sharp_rgb
    mid = mip_blur(sharp, 8, 3.0)
    blur = mip_blur(sharp, 32, 8.0, contrast=0.55)
    blur = ImageEnhance.Brightness(blur).enhance(1.05)
    sharp.save(TEX / "env_sharp.png")
    mid.save(TEX / "env_mid.png")
    blur.save(TEX / "env_blur.png")
    sharp.save(TEX / "envmap.png")
    mid.save(TEX / "envmap_r25.png")
    blur.save(TEX / "envmap_r50.png")
    print("wrote env_sharp/mid/blur", n)


def gen_logo(n: int = 96) -> None:
    """Draw the isometric GameCube 'G' cube mark (polygons, not a crop).

    Geometry approximates the classic GC emblem: a cube with a G-shaped
    channel cut through the top / front faces.
    """
    # Draw large then downscale for smoother edges
    S = 256
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = S * 0.50, S * 0.52
    sx, sy = S * 0.38, S * 0.22

    def P(x, y, z):
        # isometric: x right, y up, z depth
        return (cx + (x - z) * sx, cy - y * sy * 1.55 + (x + z) * sy * 0.55)

    # Outer cube
    top = [P(0, 1, 0), P(1, 1, 0), P(1, 1, 1), P(0, 1, 1)]
    right = [P(1, 1, 0), P(1, 1, 1), P(1, 0, 1), P(1, 0, 0)]
    left = [P(0, 1, 1), P(1, 1, 1), P(1, 0, 1), P(0, 0, 1)]
    d.polygon(top, fill=(168, 150, 230, 255))
    d.polygon(right, fill=(100, 78, 185, 255))
    d.polygon(left, fill=(70, 48, 145, 255))

    # Inner G channel (lighter purple inset on top)
    # Path: from left-front, across, down the G spine, across bottom arm
    g_top = [
        P(0.18, 1.001, 0.18),
        P(0.82, 1.001, 0.18),
        P(0.82, 1.001, 0.38),
        P(0.40, 1.001, 0.38),
        P(0.40, 1.001, 0.62),
        P(0.82, 1.001, 0.62),
        P(0.82, 1.001, 0.82),
        P(0.18, 1.001, 0.82),
    ]
    d.polygon(g_top, fill=(210, 195, 245, 255))

    # Front face of the G cut (visible step on left/front)
    g_front = [
        P(0.18, 1.0, 0.82),
        P(0.82, 1.0, 0.82),
        P(0.82, 0.55, 0.82),
        P(0.18, 0.55, 0.82),
    ]
    d.polygon(g_front, fill=(130, 110, 205, 255))
    # Spine wall of the G
    g_spine = [
        P(0.40, 1.0, 0.38),
        P(0.40, 1.0, 0.62),
        P(0.40, 0.72, 0.62),
        P(0.40, 0.72, 0.38),
    ]
    d.polygon(g_spine, fill=(90, 70, 170, 255))

    # Edge highlights
    d.line([P(0, 1, 0), P(1, 1, 0)], fill=(235, 230, 255, 255), width=2)
    d.line([P(1, 1, 0), P(1, 1, 1)], fill=(235, 230, 255, 255), width=2)
    d.line([P(1, 1, 1), P(0, 1, 1)], fill=(200, 185, 240, 255), width=2)

    out = img.resize((n, n), Image.Resampling.LANCZOS)
    out.save(TEX / "logo.png")
    print("wrote logo", n)


def gen_banner(w: int = 420, h: int = 36) -> None:
    """Solid purple title plate matching the reference bar."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=PURPLE)
    f = font(18, bold=True)
    text = "GX / TEV MATERIAL LAB"
    bbox = d.textbbox((0, 0), text, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((w - tw) // 2, (h - th) // 2 - 1), text, font=f, fill=(255, 255, 255, 255))
    img.save(TEX / "banner.png")
    print("wrote banner", w, h)


def gen_wordmark(w: int = 280, h: int = 48) -> None:
    """NINTENDO GAMECUBE™ wordmark drawn as sprites (not cropped)."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = font(20, bold=True)
    d.text((0, 12), "NINTENDO GAMECUBE", font=f, fill=(255, 255, 255, 255))
    f3 = font(9, bold=True)
    d.text((248, 8), "TM", font=f3, fill=(230, 230, 230, 255))
    img.save(TEX / "wordmark.png")
    print("wrote wordmark")


def gen_font_atlas() -> None:
    """Simple 16x6 atlas for ASCII 32..127 monospace."""
    cols, rows = 16, 6
    cw, ch = 8, 16
    img = Image.new("RGBA", (cols * cw, rows * ch), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = font(12)
    for i, ch_i in enumerate(range(32, 128)):
        x = (i % cols) * cw
        y = (i // cols) * ch
        d.text((x + 1, y + 1), chr(ch_i), font=f, fill=(255, 255, 255, 255))
    out = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    out.paste(img, (0, 0))
    out.save(TEX / "font.png")
    print("wrote font atlas")


def _draw_gc_button(d, x: int, y: int, kind: str, size: int = 28) -> None:
    """GameCube face-button sprites: A/B circles, X/Y ovals."""
    f = font(16, bold=True)
    if kind == "A":
        col, label = BTN_A, "A"
        d.ellipse([x, y, x + size, y + size], fill=col)
    elif kind == "B":
        col, label = BTN_B, "B"
        d.ellipse([x, y, x + size, y + size], fill=col)
    elif kind == "X":
        col, label = BTN_X, "X"
        # Horizontal oval like the real GC X
        d.ellipse([x - 2, y + 4, x + size + 2, y + size - 4], fill=col)
    else:
        col, label = BTN_Y, "Y"
        d.ellipse([x + 4, y - 2, x + size - 4, y + size + 2], fill=col)
    bbox = d.textbbox((0, 0), label, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text((x + (size - tw) / 2, y + (size - th) / 2 - 1), label, font=f, fill=(255, 255, 255, 255))


def gen_glyphs(n: int = 48) -> None:
    """Standalone A/B/X/Y button sprite strip."""
    img = Image.new("RGBA", (n * 4, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, kind in enumerate("ABXY"):
        _draw_gc_button(d, i * n + 8, 8, kind, size=n - 16)
    img.save(TEX / "glyphs.png")
    print("wrote glyphs")


def gen_ui_panels() -> None:
    """Drawn HUD chrome matching the Material Lab reference layout.

    Logo / banner / buttons are procedurally drawn sprites — not screenshot crops.
    Material stats stay live in the DOL (see draw_hud in main.c).
    """
    # Top-left: logo + wordmark + purple hairline with diagonal tab + title plate
    panel = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(panel)
    logo = Image.open(TEX / "logo.png").convert("RGBA").resize((52, 52), Image.Resampling.LANCZOS)
    word = Image.open(TEX / "wordmark.png").convert("RGBA")
    ban = Image.open(TEX / "banner.png").convert("RGBA")
    panel.paste(logo, (6, 6), logo)
    panel.paste(word, (64, 14), word)
    # Purple rule with diagonal kick (reference tab)
    d.rectangle([6, 58, 340, 61], fill=PURPLE_LINE)
    d.polygon([(340, 58), (368, 42), (372, 42), (344, 61)], fill=PURPLE_LINE)
    d.rectangle([372, 42, 500, 45], fill=PURPLE_LINE)
    panel.paste(ban, (6, 68), ban)
    panel.save(TEX / "hud_tl.png")

    # FPS
    fps = Image.new("RGBA", (128, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(fps)
    d.text((4, 8), "FPS: 60.00", font=font(16, bold=True), fill=(255, 255, 255, 255))
    fps.save(TEX / "hud_fps.png")

    # Bottom control bar — A/B/X (+ Y) drawn sprites, centered
    bot = Image.new("RGBA", (1024, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(bot)
    d.rectangle([0, 0, 639, 2], fill=PURPLE_LINE)
    d.rectangle([0, 2, 639, 47], fill=(8, 8, 10, 235))
    f = font(14)
    items = [
        ("A", "material"),
        ("B", "roughness"),
        ("X", "env map"),
        ("Y", "TEV"),
    ]
    # Measure centered cluster
    gap = 36
    widths = []
    for kind, lab in items:
        tw = d.textbbox((0, 0), f": {lab}", font=f)[2]
        widths.append(28 + 6 + tw)
    total = sum(widths) + gap * (len(items) - 1)
    x = (640 - total) // 2
    for (kind, lab), w in zip(items, widths):
        _draw_gc_button(d, x, 10, kind, size=28)
        d.text((x + 34, 16), f": {lab}", font=f, fill=(255, 255, 255, 255))
        x += w + gap
    bot.save(TEX / "hud_bottom.png")
    print("wrote HUD panels (drawn sprites)")


def gen_bump_detail_spec() -> None:
    """Extras used by Flipper feature paths (IndTex / mod2x / spec mask)."""
    n = 128
    bump = np.zeros((n, n, 4), dtype=np.uint8)
    for y in range(n):
        for x in range(n):
            fx = x / n * 2 * math.pi
            fy = y / n * 2 * math.pi
            hx = 0.35 * math.sin(fx * 4) + 0.2 * math.sin(fx * 9 + fy * 3)
            hy = 0.35 * math.sin(fy * 4) + 0.2 * math.sin(fy * 7 + fx * 2)
            bump[y, x, 0] = int(128 + max(-1.0, min(1.0, hx)) * 100)
            bump[y, x, 1] = int(128 + max(-1.0, min(1.0, hy)) * 100)
            bump[y, x, 2] = 200
            bump[y, x, 3] = 255
    Image.fromarray(bump, "RGBA").save(TEX / "bump.png")

    rng = np.random.default_rng(1)
    noise = rng.integers(90, 165, (128, 128))
    det = np.zeros((128, 128, 4), dtype=np.uint8)
    det[:, :, 0] = det[:, :, 1] = det[:, :, 2] = noise.astype(np.uint8)
    det[:, :, 3] = 255
    Image.fromarray(det, "RGBA").save(TEX / "detail.png")

    spec = np.zeros((256, 256, 4), dtype=np.uint8)
    for y in range(256):
        for x in range(256):
            sx = (x - 128) / 128.0
            sy = (128 - y) / 128.0
            if sx * sx + sy * sy > 1.0:
                continue
            sun = math.hypot(sx - 0.35, sy - 0.40)
            w = max(0.0, 1.0 - sun / 0.35) ** 2
            v = int(255 * w)
            spec[y, x] = (v, v, v, 255)
    Image.fromarray(spec, "RGBA").save(TEX / "specmask.png")
    print("wrote bump/detail/specmask")


def main() -> None:
    gen_checker()
    gen_shadow()
    # Always refresh synthetic env LODs (roughness proof). Keep HUD if present.
    gen_envmap()
    gen_logo()
    gen_banner()
    gen_wordmark()
    gen_font_atlas()
    gen_glyphs()
    # Always redraw HUD chrome (never keep screenshot-cropped panels)
    gen_ui_panels()
    gen_bump_detail_spec()
    # SCF stubs for gxtexconv
    for name, src in [
        ("env_sharp", "env_sharp.png"),
        ("env_mid", "env_mid.png"),
        ("env_blur", "env_blur.png"),
        ("envmap", "envmap.png"),
        ("checker", "checker.png"),
        ("shadow", "shadow.png"),
        ("hud_tl", "hud_tl.png"),
        ("hud_fps", "hud_fps.png"),
        ("hud_bottom", "hud_bottom.png"),
        ("font", "font.png"),
        ("bump", "bump.png"),
        ("detail", "detail.png"),
        ("specmask", "specmask.png"),
    ]:
        (TEX / f"{name}.scf").write_text(
            f'<filepath="{src}" id="{name}" colfmt=6 />\n'
        )
    print("done", TEX)


if __name__ == "__main__":
    main()
