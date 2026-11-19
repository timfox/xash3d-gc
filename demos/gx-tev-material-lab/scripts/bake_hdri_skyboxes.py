#!/usr/bin/env python3
"""Bake HDRI / equirect panoramas → Material Lab skybox faces + IBL matcaps.

Uses gxbake's skybox sampler (same orientation as R_GXDrawSkyBox) so Flipper
sky cubes match the engine. Lab embeds 256² RGB faces (CMPR TPL) — highest
res that keeps the DOL MEM1-safe with three skies.

Usage:
  PYTHONPATH=/path/to/gxbake python3 scripts/bake_hdri_skyboxes.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"

# Prefer local high-res equirects (2K PNG from proceduralsky / HDRI library).
SOURCES = [
	("golden", Path("/home/tim/proceduralsky-maya-demo/output/hdr/golden_hour.png")),
	("noon", Path("/home/tim/proceduralsky-maya-demo/output/hdr/cycle_0040.png")),
	("dusk", Path("/home/tim/proceduralsky-maya-demo/output/hdr/cycle_0056.png")),
]

SKY_SIZE = 256  # lab skybox (engine sky_*.cmp is 128; we are not that path)
IBL_SIZE = 256
SIDES = ("rt", "bk", "lf", "ft", "up", "dn")


def _ensure_gxbake():
	candidates = [
		Path("/home/tim/Desktop/fork-work/gxbake"),
		Path.home() / "Desktop/fork-work/gxbake",
		Path.home() / "gxbake",
	]
	for c in candidates:
		if (c / "gxbake" / "core" / "skybox.py").is_file():
			sys.path.insert(0, str(c))
			return
	raise SystemExit("gxbake not found — clone timfox/gxbake or set PYTHONPATH")


def bake_matcap(equirect: np.ndarray, size: int) -> np.ndarray:
	"""View-space normal matcap from equirect (matches sphere TEX1 UVs)."""
	from gxbake.core import skybox as sb

	h, w = equirect.shape[:2]
	# Build a temporary equirect source via skybox open
	yy, xx = np.mgrid[0:size, 0:size]
	# Matcap UV → disk normal in view space (z toward camera)
	u = (xx + 0.5) / size * 2.0 - 1.0
	v = 1.0 - (yy + 0.5) / size * 2.0  # flip V to match GX
	r2 = u * u + v * v
	inside = r2 <= 1.0
	nz = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
	# Reflect view=(0,0,1) about N: R = 2(N·V)N - V
	ndotv = nz  # V = (0,0,1)
	rx = 2.0 * ndotv * u
	ry = 2.0 * ndotv * v
	rz = 2.0 * ndotv * nz - 1.0
	# Direction → equirect lon/lat (Y-up)
	rn = np.stack([rx, ry, rz], axis=-1)
	rn /= np.linalg.norm(rn, axis=-1, keepdims=True) + 1e-8
	lon = np.arctan2(rn[..., 0], rn[..., 2])  # -pi..pi
	lat = np.arcsin(np.clip(rn[..., 1], -1.0, 1.0))
	sx = ((lon / (2.0 * np.pi)) + 0.5) * (w - 1)
	sy = (0.5 - lat / np.pi) * (h - 1)
	sx = np.clip(sx, 0, w - 1).astype(np.int32)
	sy = np.clip(sy, 0, h - 1).astype(np.int32)
	out = equirect[sy, sx].copy()
	out[~inside] = 0
	return out.astype(np.uint8)


def write_png_scf(path: Path, arr: np.ndarray, tex_id: str):
	Image.fromarray(arr).save(path)
	scf = path.with_suffix(".scf")
	# colfmt=14 CMPR — MEM1-friendly for many faces
	scf.write_text(f'<filepath="{path.name}" id="{tex_id}" colfmt=14 />\n')


def main() -> int:
	_ensure_gxbake()
	from gxbake.core import skybox as sb

	TEX.mkdir(exist_ok=True)
	opts = sb.SkyOptions(size=SKY_SIZE, layout="equirect", supersample=4)

	for name, src in SOURCES:
		if not src.is_file():
			print(f"skip missing {src}")
			continue
		print(f"==> {name} from {src}")
		sky = sb.build([src], opts, name=name)
		for side in SIDES:
			arr = sky.sides[side]
			tid = f"sky_{name}_{side}"
			write_png_scf(TEX / f"{tid}.png", arr, tid)
			print(f"  {tid}.png {arr.shape}")

		# IBL matcap (sharp) + mid/blur for roughness LOD
		eq = np.asarray(Image.open(src).convert("RGB"))
		sharp = bake_matcap(eq, IBL_SIZE)
		mid = np.asarray(Image.fromarray(sharp).filter(ImageFilter.GaussianBlur(radius=2)))
		blur = np.asarray(Image.fromarray(sharp).filter(ImageFilter.GaussianBlur(radius=6)))
		for tag, img in (("sharp", sharp), ("mid", mid), ("blur", blur)):
			tid = f"ibl_{name}_{tag}"
			write_png_scf(TEX / f"{tid}.png", img, tid)
			print(f"  {tid}.png")

		# Cross preview for QA
		cross = sb.cross_preview(sky.sides)
		Image.fromarray(cross).save(TEX / f"sky_{name}_cross_preview.png")

	print("done")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
