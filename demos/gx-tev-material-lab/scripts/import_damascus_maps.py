#!/usr/bin/env python3
"""Import Tim's Rainbow Damascus PBR set into textures/damascus_*.png."""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
SRC = Path(
	os.environ.get(
		"PBR_DAMASCUS_DIR",
		str(Path.home() / "Desktop/pbr-gc/metal_damascus"),
	)
)

MAPS = {
	"albedo": "TCom_Metal_RainbowDamascus_1K_albedo.tif",
	"metallic": "TCom_Metal_RainbowDamascus_1K_metallic.tif",
	"normal": "TCom_Metal_RainbowDamascus_1K_normal.tif",
	"roughness": "TCom_Metal_RainbowDamascus_1K_roughness.tif",
}


def write_rgba(name: str, rgb: np.ndarray) -> None:
	h, w, _ = rgb.shape
	out = np.dstack([rgb, np.full((h, w), 255, np.uint8)])
	png = TEX / f"damascus_{name}.png"
	Image.fromarray(out).save(png)
	(TEX / f"damascus_{name}.scf").write_text(
		f'<filepath="damascus_{name}.png" id="damascus_{name}" colfmt=6 />\n'
	)
	print("wrote", png, "mean", rgb.mean(axis=(0, 1)))


def main() -> None:
	if not SRC.is_dir():
		raise SystemExit(f"damascus map dir not found: {SRC}")
	TEX.mkdir(parents=True, exist_ok=True)
	for name, fn in MAPS.items():
		im = Image.open(SRC / fn).convert("RGB").resize(
			(512, 512), Image.Resampling.LANCZOS
		)
		write_rgba(name, np.asarray(im))
	# No AO in the TextureCom set — flat white so the rust TEV path still works
	white = np.full((512, 512, 3), 255, np.uint8)
	write_rgba("ao", white)


if __name__ == "__main__":
	main()
