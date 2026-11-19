#!/usr/bin/env python3
"""DEPRECATED for HUD and hero.

Hero surface maps: scripts/import_rust_maps.py (real PBR TIFs).
HUD chrome: scripts/gen_assets.py draws logo / banner / GC buttons —
do NOT crop the copper reference screenshot.

This script remains only as a historical note. Running it exits nonzero.
"""
from __future__ import annotations

import sys


def main() -> None:
	print(
		"bake_from_reference.py is retired.\n"
		"  HUD  → python3 scripts/gen_assets.py  (drawn sprites)\n"
		"  hero → python3 scripts/import_rust_maps.py\n"
		"Do not crop the Material Lab reference for env/HUD.",
		file=sys.stderr,
	)
	raise SystemExit(2)


if __name__ == "__main__":
	main()
