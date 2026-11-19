#!/usr/bin/env python3
"""Bake MEM1-safe height maps from material normals for FEAT_EMBOSS_LIT.

Integrates tangent-space normal XY into a grayscale height field (Lost Coast
GCeh-style dual-sample emboss). Output is 64² RGBA (R=G=B=height) for gxtexconv.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
SIZE = 64
SLUGS = ("stone", "brick", "concrete")


def bake(slug: str) -> None:
	src = TEX / f"{slug}_normal.png"
	img = Image.open(src).convert("RGBA")
	a = np.asarray(img, dtype=np.float32)
	nx = a[:, :, 0] / 255.0 * 2.0 - 1.0
	ny = a[:, :, 1] / 255.0 * 2.0 - 1.0
	hx = np.cumsum(-nx, axis=1)
	hy = np.cumsum(-ny, axis=0)
	h = 0.5 * (hx + hy)
	h -= h.mean()
	std = float(h.std())
	if std < 1e-6:
		std = 1.0
	h = np.clip(0.5 + h / (std * 3.0), 0.0, 1.0)
	v = (h * 255.0).astype(np.uint8)
	pil = Image.fromarray(np.dstack([v, v, v, np.full_like(v, 255)]), "RGBA")
	pil = pil.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
	dest = TEX / f"height_{slug}.png"
	pil.save(dest)
	(TEX / f"height_{slug}.scf").write_text(
		f'<filepath="height_{slug}.png" id="height_{slug}" colfmt=6 />\n'
	)
	print(f"wrote {dest} {pil.size}")


def main() -> None:
	for slug in SLUGS:
		bake(slug)


if __name__ == "__main__":
	main()
