#!/usr/bin/env python3
"""Import TextureCom PBR sets from Tim's drives into textures/<slug>_*.png.

Sources discovered on Storage / Extra / Stream (see SETS below).
Resizes to 256² RGBA + writes gxtexconv .scf sidecars.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
SIZE = 128

STORAGE = Path("/run/media/tim/Storage/NewOld/New folder (3)")
EXTRA = Path("/run/media/tim/Extra")
STREAM = Path("/run/media/tim/Stream/urayandere/assets")

# slug → (dir, filename prefix, maps to pull)
# normal_alt: optional fallback filename fragment when primary normal is empty
SETS = {
	"corrugated": {
		"dir": STORAGE,
		"prefix": "TCom_Metal_Corrugated4_4K_",
		"maps": ("albedo", "ao", "metallic", "normal", "roughness"),
	},
	"cladding": {
		"dir": STORAGE,
		"prefix": "TCom_MetalCladdingSheetA_4K_",
		"maps": ("albedo", "ao", "metallic", "normal", "roughness"),
	},
	"steel": {
		"dir": STORAGE,
		"prefix": "TCom_StainlessSteelPlates_4K_",
		"maps": ("albedo", "ao", "metallic", "normal", "roughness"),
	},
	"concrete": {
		"dir": STORAGE,
		"prefix": "TCom_Concrete_Base15_2.5x2.5_4K_",
		"maps": ("albedo", "ao", "normal", "roughness"),
	},
	"brick": {
		"dir": STORAGE,
		"prefix": "TCom_Wall_BrickPlain12_5.0x2.5_4K_",
		"maps": ("albedo", "ao", "normal", "roughness"),
	},
	"wood": {
		"dir": STREAM,
		"prefix": "TCom_Wood_End01_4K_",
		"maps": ("albedo", "normal", "roughness"),
	},
	"paint": {
		"dir": STREAM,
		"prefix": "TCom_Paint_Chipped_4K_",
		"maps": ("albedo", "normal", "roughness"),
	},
	"solar": {
		"dir": STREAM,
		"prefix": "TCom_SolarCells_4K_",
		"maps": ("albedo", "ao", "metallic", "normal", "roughness"),
	},
}


def find_src(directory: Path, prefix: str, kind: str) -> Path | None:
	candidates = sorted(directory.glob(f"{prefix}{kind}*"))
	# Prefer exact .tif without .part / (1) duplicates
	good = [
		p
		for p in candidates
		if p.suffix.lower() in (".tif", ".tiff", ".png", ".jpg", ".jpeg")
		and ".part" not in p.name
		and "(1)" not in p.name
	]
	# Fallback: allow (1) normals (WoodOldRough case)
	if not good:
		good = [
			p
			for p in candidates
			if p.suffix.lower() in (".tif", ".tiff", ".png", ".jpg", ".jpeg")
			and ".part" not in p.name
		]
	for p in good:
		if p.stat().st_size > 0:
			return p
	return None


def write_rgba(slug: str, kind: str, rgb: np.ndarray) -> None:
	h, w, _ = rgb.shape
	out = np.dstack([rgb, np.full((h, w), 255, np.uint8)])
	png = TEX / f"{slug}_{kind}.png"
	Image.fromarray(out).save(png)
	(TEX / f"{slug}_{kind}.scf").write_text(
		f'<filepath="{slug}_{kind}.png" id="{slug}_{kind}" colfmt=6 />\n'
	)
	print(f"  wrote {png.name} mean={rgb.mean(axis=(0, 1)).astype(int)}")


def import_set(slug: str, cfg: dict) -> None:
	directory: Path = cfg["dir"]
	prefix: str = cfg["prefix"]
	maps: tuple[str, ...] = cfg["maps"]
	print(f"\n== {slug} ==")
	if not directory.is_dir():
		print(f"  SKIP missing dir {directory}")
		return
	have = {}
	for kind in maps:
		src = find_src(directory, prefix, kind)
		if src is None:
			print(f"  missing {kind}")
			continue
		im = Image.open(src).convert("RGB").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
		arr = np.asarray(im)
		write_rgba(slug, kind, arr)
		have[kind] = arr
	# Synthetic white AO / black metal when absent so hero TEV path stays uniform
	if "ao" not in have:
		write_rgba(slug, "ao", np.full((SIZE, SIZE, 3), 255, np.uint8))
	if "metallic" not in have:
		write_rgba(slug, "metallic", np.zeros((SIZE, SIZE, 3), np.uint8))


def main() -> None:
	TEX.mkdir(parents=True, exist_ok=True)
	for slug, cfg in SETS.items():
		import_set(slug, cfg)
	print("\ndone", TEX)


if __name__ == "__main__":
	main()
