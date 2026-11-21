#!/usr/bin/env python3
"""Report how far every map prop sits above or below the ground.

For each prop the bake would place (same selection and floor drop as
bake_props_goldsrc.py) this poses the model, moves it to its baked origin
and compares its lowest vertex with the first solid found under it:

  gap > 0   the prop floats by that many units
  gap < 0   the prop is sunk by that many units

The ground is probed twice: against the world only, and against the world
plus every solid brush entity.  Props meant to hang (lamps, wall signs) show
up as large gaps; the table is for a person to read, not a pass/fail gate.

  audit_prop_grounding.py --map maps/dod_charlie.bsp --src <gamedir> [--csv out.csv]
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bake_props_goldsrc as bp  # noqa: E402


def ground_under(floor, x: float, y: float, z_from: float, before, reach: float = 512.0):
	"""First solid under (x, y) starting at z_from: its top z, or None.
	Starts above solid: when z_from is already inside solid, climbs out
	first (up to 96 units) so a sunk prop still gets a ground height."""
	z = z_from
	climbed = 0.0
	while floor.solid((x, y, z), before) and climbed < 96.0:
		z += 2.0
		climbed += 2.0
	if floor.solid((x, y, z), before):
		return None
	return floor.drop((x, y, z), reach=reach + climbed, before=before)


def audit(bsp_path: Path, model_roots: list[Path], log=print, floor=None) -> list[dict]:
	ents = bp.parse_entities(bsp_path.read_bytes())
	props = bp.static_props(ents)
	if floor is None:
		floor = bp.FloorLight(bsp_path)
		floor.add_brush_entities(ents)
	all_brush = len(ents) + 1
	cache: dict[tuple, list | None] = {}
	studio: dict[str, object] = {}
	rows = []

	def find_model(rel: str):
		for root in model_roots:
			cand = root / rel
			if cand.is_file():
				return cand
			parent = cand.parent
			if parent.is_dir():
				want = Path(rel).name.lower()
				for p in parent.iterdir():
					if p.name.lower() == want:
						return p
		return None

	def verts_of(prop: dict):
		key = (prop["model"], prop["body"], prop["sequence"], prop.get("sequence_name", "").lower())
		if key in cache:
			return cache[key]
		cache[key] = None
		rel = prop["model"]
		if rel not in studio:
			path = find_model(rel)
			try:
				studio[rel] = bp.StudioModel(path) if path else None
			except Exception as error:  # noqa: BLE001 - report and carry on
				log(f"  {rel}: {error}")
				studio[rel] = None
		mdl = studio[rel]
		if mdl is None:
			return None
		try:
			cmds, _tex = bp.model_triangles(mdl, prop["body"], prop["skin"], bp.prop_sequence(mdl, prop), 1.0)
		except Exception as error:  # noqa: BLE001
			log(f"  {rel}: {error}")
			return None
		pts = [pos for runs in cmds.values() for _fan, verts in runs for pos, _n, _s, _t in verts]
		cache[key] = pts or None
		return cache[key]

	for prop in props:
		raw = list(prop["origin"])
		origin = list(raw)
		dropped = False
		if prop.get("toss") or prop["drop"]:
			moved = dict(prop)
			fall = bp.fall_like_the_game if prop.get("toss") else bp.drop_like_the_game
			dropped = fall(floor, moved) is not None
			origin = list(moved["origin"])
		pts = verts_of(prop)
		row = {
			"ent": prop["ent"],
			"class": ents[prop["ent"]].get("classname", ""),
			"name": ents[prop["ent"]].get("targetname", ""),
			"model": prop["model"],
			"spawnflags": ents[prop["ent"]].get("spawnflags", "0"),
			"sequence": prop.get("sequence_name", "") or prop["sequence"],
			"x": round(raw[0], 1), "y": round(raw[1], 1), "z_map": round(raw[2], 1),
			"z_baked": round(origin[2], 1),
			"drop_flag": int(prop["drop"]), "dropped": int(dropped),
			"origin_in_solid": int(floor.solid(tuple(raw), prop.get("ent"))),
			"low_z": "", "mins_z": "", "gap_world": "", "gap_all": "",
		}
		if pts:
			m = bp.angle_matrix(-prop["angles"][0], prop["angles"][1], prop["angles"][2])
			low = None
			local_low = min(p[2] for p in pts)
			for p in pts:
				w = bp._rotate(m, p)
				wp = (w[0] + origin[0], w[1] + origin[1], w[2] + origin[2])
				if low is None or wp[2] < low[2]:
					low = wp
			row["low_z"] = round(low[2], 1)
			row["mins_z"] = round(local_low, 1)
			# Probe under the origin: the lowest vertex of a leaning fence or
			# a tree root can hang over an edge.
			start = max(origin[2], low[2]) + 24.0
			for col, before in (("gap_world", 0), ("gap_all", all_brush)):
				g = ground_under(floor, origin[0], origin[1], start, before)
				if g is not None:
					row[col] = round(low[2] - g, 1)
		rows.append(row)
	return rows


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--map", type=Path, required=True)
	ap.add_argument("--src", type=Path, action="append", required=True, help="gamedir root with models/ (repeatable)")
	ap.add_argument("--csv", type=Path, help="write every prop to this CSV")
	ap.add_argument("--tolerance", type=float, default=3.0, help="gap in units that counts as grounded")
	args = ap.parse_args()
	rows = audit(args.map, args.src)
	if args.csv and rows:
		with args.csv.open("w", newline="") as f:
			w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
			w.writeheader()
			w.writerows(rows)
	have = [r for r in rows if r["gap_all"] != ""]
	floats = [r for r in have if r["gap_all"] > args.tolerance]
	sunk = [r for r in have if r["gap_all"] < -args.tolerance]
	print(f"{args.map.name}: {len(rows)} props, {len(have)} measured, "
		f"{len(floats)} float, {len(sunk)} sunk (tolerance {args.tolerance})")
	by_model: dict[str, list] = {}
	for r in floats + sunk:
		by_model.setdefault(r["model"], []).append(r)
	for model, rs in sorted(by_model.items(), key=lambda kv: -len(kv[1])):
		gaps = sorted(r["gap_all"] for r in rs)
		flags = sorted({f"{r['class']} sf={r['spawnflags']} drop={r['dropped']}" for r in rs})
		print(f"  {model}: {len(rs)} off, gap_all {gaps[0]}..{gaps[-1]}, mins_z {rs[0]['mins_z']}, {'; '.join(flags)}")
		for r in rs[:4]:
			print(f"    ent {r['ent']} {r['name'] or '-'} at {r['x']} {r['y']} z_map {r['z_map']} "
				f"z_baked {r['z_baked']} low {r['low_z']} gap_world {r['gap_world']} gap_all {r['gap_all']} "
				f"in_solid {r['origin_in_solid']}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
