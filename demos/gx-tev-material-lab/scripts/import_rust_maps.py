#!/usr/bin/env python3
"""Import Tim's PBR rust set from ~/Desktop/pbr-gc/rust into textures/."""
from __future__ import annotations
# GC-safe default 256² (512² blows the DOL past MEM1).

import os
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "textures"
SRC = Path(os.environ.get("PBR_RUST_DIR", str(Path.home() / "Desktop/pbr-gc/rust")))

MAPS = {
	"albedo": "TCom_Rust_Plain1A_0.5x0.5_1K_albedo.tif",
	"ao": "TCom_Rust_Plain1A_0.5x0.5_1K_ao.tif",
	"metallic": "TCom_Rust_Plain1A_0.5x0.5_1K_metallic.tif",
	"normal": "TCom_Rust_Plain1A_0.5x0.5_1K_normal.tif",
	"roughness": "TCom_Rust_Plain1A_0.5x0.5_1K_roughness.tif",
}


def main() -> None:
	if not SRC.is_dir():
		raise SystemExit(f"rust map dir not found: {SRC}")
	TEX.mkdir(parents=True, exist_ok=True)
	for name, fn in MAPS.items():
		src = SRC / fn
		im = Image.open(src).convert("RGB").resize((256, 256), Image.Resampling.LANCZOS)
		arr = np.asarray(im)
		out = np.dstack([arr, np.full((256, 256), 255, np.uint8)])
		png = TEX / f"rust_{name}.png"
		Image.fromarray(out).save(png)
		(TEX / f"rust_{name}.scf").write_text(
			f'<filepath="rust_{name}.png" id="rust_{name}" colfmt=6 />\n'
		)
		print("wrote", png, "mean", arr.mean(axis=(0, 1)))


if __name__ == "__main__":
	main()
