#!/usr/bin/env python3
"""OPTIONAL: bake HUD chrome from the Material Lab reference.

Hero surface maps come from scripts/import_rust_maps.py (real PBR TIFs),
NOT from extracting the copper sphere out of the reference screenshot.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"

STORE_CANDIDATES = [
	Path(os.environ["MATERIAL_LAB_STORE"]) / "media/gx-tev-material-lab-reference.png"
	if os.environ.get("MATERIAL_LAB_STORE")
	else None,
	Path(
		"/home/tim/.local/state/cursor/agent-stores/cursor_agent_stores/"
		"bc-a17341b7-2d15-4aa2-bb4e-7303d0988f0a/files/media/gx-tev-material-lab-reference.png"
	),
	ROOT.parents[2] / "media/gx-tev-material-lab-reference.png",
]


def find_ref() -> Path:
	for p in STORE_CANDIDATES:
		if p is not None and p.is_file():
			return p
	raise SystemExit("reference PNG not found; set MATERIAL_LAB_STORE")


def bake_matcap(ref_hi: np.ndarray, n: int = 512) -> None:
	# Circle fit constants measured on 640×480 downscale of the reference.
	cx, cy, r = 318.47, 238.31, 144.18
	sx_hi = ref_hi.shape[1] / 640.0
	sy_hi = ref_hi.shape[0] / 480.0
	mat = np.zeros((n, n, 4), dtype=np.uint8)
	for j in range(n):
		for i in range(n):
			u = (i + 0.5) / n
			v = (j + 0.5) / n
			sx = u * 2 - 1
			sy = 1 - v * 2  # matches v = -sy*0.5+0.5 in the demo
			if sx * sx + sy * sy > 1.0:
				mat[j, i] = (14, 12, 18, 255)
				continue
			x640 = cx + sx * r
			y640 = cy - sy * r
			xi = int(round(x640 * sx_hi))
			yi = int(round(y640 * sy_hi))
			if 0 <= xi < ref_hi.shape[1] and 0 <= yi < ref_hi.shape[0]:
				mat[j, i] = (*ref_hi[yi, xi], 255)
			else:
				mat[j, i] = (14, 12, 18, 255)
	img = Image.fromarray(mat)
	img.save(TEX / "envmap_sharp.png")
	img.save(TEX / "envmap.png")
	img.filter(ImageFilter.GaussianBlur(0.9)).save(TEX / "envmap_r25.png")
	img.filter(ImageFilter.GaussianBlur(3.0)).save(TEX / "envmap_r50.png")
	print("baked matcap", n)


def bake_hud(ref640: Image.Image) -> None:
	tl = ref640.crop((0, 0, 380, 175)).convert("RGBA")
	arr = np.asarray(tl).copy()
	out = np.zeros_like(arr)
	for y in range(arr.shape[0]):
		for x in range(arr.shape[1]):
			r, g, b, a = arr[y, x]
			keep = False
			if r > 170 and g > 170 and b > 170:
				keep = True
			elif b > 85 and b > g + 10 and b > r and r > 30:
				keep = True
			elif r > 70 and b > 120 and g < 140:
				keep = True
			elif b > 60 and r > 40 and b > r and abs(int(g) - int(r)) < 40 and y < 70:
				keep = True
			if keep:
				out[y, x] = (r, g, b, 255)
	panel = Image.new("RGBA", (512, 256), (0, 0, 0, 0))
	panel.paste(Image.fromarray(out), (0, 0), Image.fromarray(out))
	panel.save(TEX / "hud_tl.png")

	fps = ref640.crop((500, 0, 640, 40)).convert("RGBA")
	fa = np.asarray(fps).copy()
	o = np.zeros_like(fa)
	for y in range(fa.shape[0]):
		for x in range(fa.shape[1]):
			r, g, b, a = fa[y, x]
			if r > 180 and g > 180 and b > 180:
				o[y, x] = (255, 255, 255, 255)
	fp = Image.new("RGBA", (128, 64), (0, 0, 0, 0))
	fp.paste(Image.fromarray(o), (0, 0), Image.fromarray(o))
	fp.save(TEX / "hud_fps.png")

	bot = ref640.crop((0, 436, 640, 480)).convert("RGBA")
	ba = np.asarray(bot).copy()
	for y in range(ba.shape[0]):
		for x in range(ba.shape[1]):
			r, g, b, a = ba[y, x]
			if r > 20 or g > 20 or b > 25:
				ba[y, x, 3] = 255
			else:
				ba[y, x] = (0, 0, 0, 0)
	bp = Image.fromarray(ba)
	d = ImageDraw.Draw(bp)
	try:
		f = ImageFont.truetype(
			"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12
		)
	except OSError:
		f = ImageFont.load_default()
	d.text((520, 16), "Y: TEV", font=f, fill=(255, 255, 255, 255))
	outb = Image.new("RGBA", (1024, 64), (0, 0, 0, 0))
	outb.paste(bp, (0, 0), bp)
	outb.save(TEX / "hud_bottom.png")
	print("baked HUD from reference")


def main() -> None:
	TEX.mkdir(parents=True, exist_ok=True)
	ref_path = find_ref()
	print("ref", ref_path)
	ref_hi = np.asarray(Image.open(ref_path).convert("RGB"))
	bake_matcap(ref_hi)
	ref640 = Image.fromarray(ref_hi).resize((640, 480), Image.Resampling.LANCZOS)
	bake_hud(ref640)


if __name__ == "__main__":
	main()
