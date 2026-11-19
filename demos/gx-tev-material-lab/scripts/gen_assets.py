#!/usr/bin/env python3
"""Generate GX/TEV Material Lab textures (PNG) for gxtexconv."""
from __future__ import annotations

import math
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
TEX.mkdir(parents=True, exist_ok=True)

PURPLE = (105, 55, 185, 255)
PURPLE_LINE = (120, 70, 200, 255)
DARK = (18, 17, 20, 255)


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
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cell = n // tiles
    dark = (42, 40, 46, 255)
    mid = (92, 88, 98, 255)
    for y in range(n):
        for x in range(n):
            a[y, x] = dark if ((x // cell) + (y // cell)) % 2 == 0 else mid
    Image.fromarray(a, "RGBA").save(TEX / "checker.png")
    print("wrote checker", n)


def gen_shadow(n: int = 256) -> None:
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cx = cy = (n - 1) * 0.5
    for y in range(n):
        for x in range(n):
            d = math.hypot(x - cx, y - cy) / (n * 0.48)
            if d >= 1.0:
                continue
            # soft blob
            t = (1.0 - d) ** 2
            a[y, x, 3] = int(200 * t)
            a[y, x, 0] = a[y, x, 1] = a[y, x, 2] = 0
    Image.fromarray(a, "RGBA").filter(ImageFilter.GaussianBlur(radius=6)).save(TEX / "shadow.png")
    print("wrote shadow", n)


def gen_envmap(n: int = 256) -> None:
    """Sphere-map style env: urban dusk / outdoor courtyard with clear structure."""
    a = np.zeros((n, n, 4), dtype=np.uint8)
    cx = cy = (n - 1) * 0.5
    for y in range(n):
        for x in range(n):
            nx = (x - cx) / cx
            ny = (y - cy) / cy
            r2 = nx * nx + ny * ny
            if r2 > 1.02:
                a[y, x] = (16, 14, 20, 255)
                continue
            z = math.sqrt(max(0.0, 1.0 - min(r2, 1.0)))
            vy = -ny
            elev = vy * 0.9 + z * 0.1
            ang = math.atan2(nx, max(z, 1e-4))
            if elev > 0.18:
                t = min(1.0, (elev - 0.18) / 0.82)
                r = int(55 + 70 * t)
                g = int(35 + 45 * t)
                b = int(95 + 120 * t)
                # sun disc upper-right
                sun = math.hypot(nx - 0.38, ny + 0.40)
                if sun < 0.10:
                    w = (0.10 - sun) / 0.10
                    r = min(255, int(r + 220 * w))
                    g = min(255, int(g + 210 * w))
                    b = min(255, int(b + 180 * w))
                elif sun < 0.26:
                    w = (0.26 - sun) / 0.16
                    r = min(255, int(r + 90 * w))
                    g = min(255, int(g + 55 * w))
                    b = min(255, int(b + 35 * w))
                # soft clouds
                clouds = 0.5 + 0.5 * math.sin(nx * 9.0 + ny * 4.0)
                if clouds > 0.75 and elev > 0.4:
                    w = (clouds - 0.75) / 0.25
                    r = min(255, int(r + 40 * w))
                    g = min(255, int(g + 35 * w))
                    b = min(255, int(b + 45 * w))
            elif elev > -0.12:
                # building skyline
                h = 0.05 + 0.18 * abs(math.sin(ang * 5.0)) + 0.10 * abs(math.sin(ang * 11.0 + 1.3))
                h += 0.06 * abs(math.sin(ang * 17.0))
                if elev < h:
                    shade = 22 + int(30 * abs(math.sin(ang * 3.0)))
                    r = shade + 18
                    g = shade + 8
                    b = shade + 28
                    # window grid
                    wx = int(abs(ang) * 55.0) % 7
                    wy = int((h - elev) * 90.0) % 5
                    if wx in (1, 4) and wy in (1, 3) and shade < 45:
                        r, g, b = 210, 160, 90
                else:
                    r, g, b = 70, 50, 95
            else:
                # checker courtyard / ground
                gx = int((nx * 0.5 + 0.5) * 18)
                gz = int(((z * 0.7 + elev) * 0.5 + 0.5) * 18)
                if (gx + gz) % 2 == 0:
                    r, g, b = 40, 36, 44
                else:
                    r, g, b = 95, 90, 100
                fade = 0.45 + 0.55 * max(0.0, z)
                r = int(r * fade)
                g = int(g * fade)
                b = int(b * fade)
            a[y, x] = (min(255, r), min(255, g), min(255, b), 255)
    img = Image.fromarray(a, "RGBA")
    # roughness 0.25: mild blur only
    img_r25 = img.filter(ImageFilter.GaussianBlur(radius=1.6))
    img.save(TEX / "envmap.png")
    img_r25.save(TEX / "envmap_r25.png")
    img.filter(ImageFilter.GaussianBlur(radius=4.5)).save(TEX / "envmap_r50.png")
    print("wrote envmap", n)


def gen_logo(n: int = 64) -> None:
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # stylized GameCube cube logo
    s = n * 0.72
    ox, oy = n * 0.14, n * 0.18
    # isometric-ish cube
    top = [(ox + s * 0.5, oy), (ox + s, oy + s * 0.22), (ox + s * 0.5, oy + s * 0.44), (ox, oy + s * 0.22)]
    left = [(ox, oy + s * 0.22), (ox + s * 0.5, oy + s * 0.44), (ox + s * 0.5, oy + s * 0.88), (ox, oy + s * 0.66)]
    right = [(ox + s, oy + s * 0.22), (ox + s * 0.5, oy + s * 0.44), (ox + s * 0.5, oy + s * 0.88), (ox + s, oy + s * 0.66)]
    d.polygon(top, fill=(160, 90, 220, 255))
    d.polygon(left, fill=(90, 40, 160, 255))
    d.polygon(right, fill=(120, 55, 190, 255))
    # inner G notch
    d.polygon(
        [
            (ox + s * 0.35, oy + s * 0.30),
            (ox + s * 0.62, oy + s * 0.30),
            (ox + s * 0.62, oy + s * 0.42),
            (ox + s * 0.48, oy + s * 0.42),
            (ox + s * 0.48, oy + s * 0.55),
            (ox + s * 0.62, oy + s * 0.55),
            (ox + s * 0.62, oy + s * 0.68),
            (ox + s * 0.35, oy + s * 0.68),
        ],
        fill=(230, 220, 255, 255),
    )
    img.save(TEX / "logo.png")
    print("wrote logo", n)


def gen_banner(w: int = 512, h: int = 64) -> None:
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # slanted purple banner
    slant = 48
    poly = [(0, 8), (w - slant, 8), (w, h - 8), (0, h - 8)]
    d.polygon(poly, fill=PURPLE)
    # thin top line extension
    d.rectangle([0, 0, w - 20, 3], fill=PURPLE_LINE)
    f = font(22, bold=True)
    text = "GX / TEV MATERIAL LAB"
    # measure
    bbox = d.textbbox((0, 0), text, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((w - slant - tw) // 2, (h - th) // 2 - 2), text, font=f, fill=(255, 255, 255, 255))
    img.save(TEX / "banner.png")
    print("wrote banner", w, h)


def gen_wordmark(w: int = 256, h: int = 64) -> None:
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f1 = font(16, bold=True)
    f2 = font(18, bold=True)
    d.text((4, 8), "NINTENDO", font=f1, fill=(255, 255, 255, 255))
    d.text((4, 30), "GAMECUBE", font=f2, fill=(255, 255, 255, 255))
    # tiny TM
    f3 = font(9)
    d.text((148, 28), "TM", font=f3, fill=(220, 220, 220, 255))
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
    # pad to 128x128
    out = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    out.paste(img, (0, 0))
    out.save(TEX / "font.png")
    print("wrote font atlas")


def gen_glyphs(n: int = 64) -> None:
    img = Image.new("RGBA", (n * 3, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = font(28, bold=True)
    colors = [(40, 180, 70, 255), (200, 40, 40, 255), (150, 90, 210, 255)]
    labels = "ABX"
    for i, (col, lab) in enumerate(zip(colors, labels)):
        x0 = i * n
        pad = 6
        d.ellipse([x0 + pad, pad, x0 + n - pad, n - pad], fill=col)
        bbox = d.textbbox((0, 0), lab, font=f)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        d.text((x0 + (n - tw) / 2, (n - th) / 2 - 2), lab, font=f, fill=(255, 255, 255, 255))
    img.save(TEX / "glyphs.png")
    print("wrote glyphs")


def gen_ui_panels() -> None:
    """Precomposed HUD strips for exact placement."""
    # top-left stack: logo+wordmark+banner+stats  (512x256)
    panel = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
    logo = Image.open(TEX / "logo.png").convert("RGBA").resize((48, 48), Image.Resampling.LANCZOS)
    word = Image.open(TEX / "wordmark.png").convert("RGBA")
    ban = Image.open(TEX / "banner.png").convert("RGBA").resize((420, 48), Image.Resampling.LANCZOS)
    panel.paste(logo, (8, 8), logo)
    panel.paste(word, (64, 8), word)
    # purple rule
    d = ImageDraw.Draw(panel)
    d.rectangle([8, 62, 400, 65], fill=PURPLE_LINE)
    panel.paste(ban, (8, 68), ban)
    f = font(14)
    lines = [
        "MATERIAL: COPPER",
        "METALLIC 1.00   ROUGHNESS 0.25",
        "ENV MAP + FRESNEL APPROX",
    ]
    y = 128
    for line in lines:
        d.text((12, y), line, font=f, fill=(255, 255, 255, 255))
        y += 18
    panel.save(TEX / "hud_tl.png")

    # FPS panel
    fps = Image.new("RGBA", (128, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(fps)
    d.text((8, 6), "FPS: 60.00", font=font(14), fill=(255, 255, 255, 255))
    fps.save(TEX / "hud_fps.png")

    # bottom bar 640x48
    bot = Image.new("RGBA", (640, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(bot)
    d.rectangle([0, 0, 640, 2], fill=PURPLE_LINE)
    d.rectangle([0, 2, 640, 48], fill=(10, 10, 12, 230))
    glyphs = Image.open(TEX / "glyphs.png").convert("RGBA").resize((96, 32), Image.Resampling.LANCZOS)
    # center cluster
    labels = ["material", "roughness", "env map"]
    f = font(13)
    total_w = 0
    chunks = []
    for i, lab in enumerate(labels):
        g = glyphs.crop((i * 32, 0, (i + 1) * 32, 32))
        tw = d.textbbox((0, 0), f": {lab}", font=f)[2]
        chunks.append((g, f": {lab}", 32 + 4 + tw))
        total_w += 32 + 4 + tw + 28
    x = (640 - total_w) // 2
    for g, text, w in chunks:
        bot.paste(g, (x, 8), g)
        d.text((x + 36, 14), text, font=f, fill=(255, 255, 255, 255))
        x += w + 28
    bot.save(TEX / "hud_bottom.png")
    print("wrote HUD panels")


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
    # Keep reference-baked matcap/HUD when present (bake_from_reference.py).
    if not (TEX / "envmap_sharp.png").is_file():
        gen_envmap()
    else:
        print("keep existing envmap_sharp / envmap_r25")
    gen_logo()
    gen_banner()
    gen_wordmark()
    gen_font_atlas()
    gen_glyphs()
    if not (TEX / "hud_tl.png").is_file() or (TEX / "hud_tl.png").stat().st_size < 1000:
        gen_ui_panels()
    else:
        print("keep existing HUD panels")
    gen_bump_detail_spec()
    # SCF stubs for gxtexconv
    for name, src in [
        ("envmap", "envmap_r25.png"),
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
