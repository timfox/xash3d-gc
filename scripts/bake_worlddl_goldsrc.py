#!/usr/bin/env python3
"""Bake a GoldSrc BSP30 map into GX display lists grouped by PVS cluster.

Same media/wd_<map>.gcd layout as lostcoast-gc tools/bake_worlddl.py, but
reads Half-Life / Day of Defeat BSP30 instead of Source VBSP. The runtime
(ref/gx/r_gx_worlddl.c) walks the baked tree for the eye cluster, loads that
cluster's Quake RLE PVS row, frustum-culls chunk boxes, and GX_CallDispList
per material batch.

media/wd_<map>.gcd  (big-endian) — see lostcoast bake_worlddl.py header doc.

Shared copy for the disc build (scripts/build-gamecube-disc.py bakes every
staged HL1 map); originally tools/bake_worlddl_goldsrc.py in dod-gc.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GX_DRAW_TRIFAN = 0xA0
GX_VTXFMT = 6
ST_FRAC = 9
MAT_F_WATER = 0x01
MAT_F_NOLIGHT = 0x02
TEX_SPECIAL = 1

SKIP_TEX = {
	"sky",
	"clip",
	"hint",
	"skip",
	"aaatrigger",
	"origin",
	"null",
	"nodraw",
	"trigger",
	"black",
	"white",
}


def fnv1a32(s: str) -> int:
	h = 2166136261
	for ch in s.lower():
		h ^= ord(ch) & 0xFF
		h = (h * 16777619) & 0xFFFFFFFF
	return h


def lump(data: bytes, i: int) -> bytes:
	off, ln = struct.unpack_from("<ii", data, 4 + i * 8)
	return data[off : off + ln]


def pad32(b: bytearray) -> None:
	b += b"\0" * ((-len(b)) % 32)


def rgb565(c) -> int:
	return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


class Chunk:
	def __init__(self) -> None:
		self.pos: dict[tuple[int, int, int], int] = {}
		self.batches: dict[int, bytearray] = {}
		self.mins = [32767, 32767, 32767]
		self.maxs = [-32768, -32768, -32768]
		self.tris = 0

	def pos_index(self, p) -> int:
		key = (
			int(max(-32767, min(32767, round(p[0])))),
			int(max(-32767, min(32767, round(p[1])))),
			int(max(-32767, min(32767, round(p[2])))),
		)
		idx = self.pos.get(key)
		if idx is None:
			idx = self.pos[key] = len(self.pos)
			for a in range(3):
				self.mins[a] = min(self.mins[a], key[a])
				self.maxs[a] = max(self.maxs[a], key[a])
		return idx

	def emit(self, mat: int, prim: int, verts: list) -> None:
		dl = self.batches.setdefault(mat, bytearray())
		dl += struct.pack(">BH", prim | GX_VTXFMT, len(verts))
		for p, c, s, t in verts:
			dl += struct.pack(
				">HHhh",
				self.pos_index(p),
				rgb565(c),
				int(max(-32768, min(32767, round(s * (1 << ST_FRAC))))),
				int(max(-32768, min(32767, round(t * (1 << ST_FRAC))))),
			)
		self.tris += len(verts) - 2

	def encode(self) -> bytes:
		mats = sorted(self.batches)
		out = bytearray(8 + 12 * len(mats))
		pad32(out)
		pos_ofs = len(out)
		for key in self.pos:
			out += struct.pack(">hhh", *key)
		pad32(out)
		for i, m in enumerate(mats):
			dl = bytearray(self.batches[m])
			pad32(dl)
			struct.pack_into(">HHII", out, 8 + 12 * i, m, 0, len(out), len(dl))
			out += dl
		struct.pack_into(">HHI", out, 0, len(self.pos), len(mats), pos_ofs)
		return bytes(out)


class Bsp:
	def __init__(self, path: Path):
		d = self.data = path.read_bytes()
		ver = struct.unpack_from("<I", d, 0)[0]
		if ver != 30:
			raise SystemExit(f"expected BSP30, got version {ver}")
		raw = lump(d, 1)
		self.planes = [struct.unpack_from("<ffffi", raw, i)[:4] for i in range(0, len(raw), 20)]
		raw = lump(d, 3)
		self.verts = [struct.unpack_from("<fff", raw, i) for i in range(0, len(raw), 12)]
		raw = lump(d, 12)
		self.edges = [struct.unpack_from("<HH", raw, i) for i in range(0, len(raw), 4)]
		raw = lump(d, 13)
		self.surfedges = struct.unpack_from(f"<{len(raw) // 4}i", raw)
		raw = lump(d, 6)
		self.texinfos = []
		for i in range(0, len(raw), 40):
			vecs = struct.unpack_from("<8f", raw, i)
			miptex, flags, _faceinfo = struct.unpack_from("<ihh", raw, i + 32)
			self.texinfos.append((vecs[0:4], vecs[4:8], miptex, flags))
		tex = lump(d, 2)
		n_mip = struct.unpack_from("<i", tex, 0)[0]
		self.mip_names: list[str] = []
		self.mip_size: list[tuple[int, int]] = []
		for i in range(n_mip):
			off = struct.unpack_from("<i", tex, 4 + i * 4)[0]
			if off < 0:
				self.mip_names.append("")
				self.mip_size.append((64, 64))
				continue
			name = tex[off : off + 16].split(b"\0", 1)[0].decode("latin-1")
			w, h = struct.unpack_from("<ii", tex, off + 16)
			self.mip_names.append(name)
			self.mip_size.append((w or 64, h or 64))
		raw = lump(d, 7)
		self.faces = []
		for i in range(0, len(raw), 20):
			planenum, side, firstedge, numedges, texinfo, styles, lightofs = struct.unpack_from(
				"<Hhihh4si", raw, i
			)
			self.faces.append((planenum, side, firstedge, numedges, texinfo, styles, lightofs))
		self.lighting = lump(d, 8)
		raw = lump(d, 5)
		# planenum, children[2], mins[3], maxs[3], firstface, numfaces
		self.nodes = [struct.unpack_from("<i8h2H", raw, i) for i in range(0, len(raw), 24)]
		raw = lump(d, 10)
		self.leaves = []
		for i in range(0, len(raw), 28):
			contents, visofs = struct.unpack_from("<ii", raw, i)
			mins = struct.unpack_from("<hhh", raw, i + 8)
			maxs = struct.unpack_from("<hhh", raw, i + 14)
			firstms, numms = struct.unpack_from("<HH", raw, i + 20)
			self.leaves.append((contents, visofs, mins, maxs, firstms, numms))
		raw = lump(d, 11)
		self.marksurfaces = struct.unpack_from(f"<{len(raw) // 2}H", raw)
		raw = lump(d, 14)
		self.models = []
		for i in range(0, len(raw), 64):
			# 9 floats + headnode[4] + visleafs + firstface + numfaces
			vals = struct.unpack_from("<9f4iiii", raw, i)
			self.models.append(vals)
		self.vis = lump(d, 4)
		self.numclusters = max(0, len(self.leaves) - 1)

	def face_pts(self, f) -> list:
		_planenum, _side, first, num, _ti, _st, _lo = f
		pts = []
		for k in range(num):
			se = self.surfedges[first + k]
			v = self.edges[se][0] if se >= 0 else self.edges[-se][1]
			pts.append(self.verts[v])
		return pts

	def cluster_at(self, p) -> int:
		idx = 0
		while idx >= 0:
			planenum, c0, c1 = self.nodes[idx][0], self.nodes[idx][1], self.nodes[idx][2]
			nx, ny, nz, dist = self.planes[planenum]
			idx = c0 if nx * p[0] + ny * p[1] + nz * p[2] - dist >= 0.0 else c1
		leaf = -1 - idx
		return -1 if leaf <= 0 else leaf - 1


def st_for(ti0, ti1, tw, th, pts) -> list[tuple[float, float]]:
	st = [
		(
			(ti0[0] * p[0] + ti0[1] * p[1] + ti0[2] * p[2] + ti0[3]) / tw,
			(ti1[0] * p[0] + ti1[1] * p[1] + ti1[2] * p[2] + ti1[3]) / th,
		)
		for p in pts
	]
	s0 = math.floor(min(s for s, _ in st))
	t0 = math.floor(min(t for _, t in st))
	return [(s - s0, t - t0) for s, t in st]


def face_extents(ti0, ti1, pts):
	mins = [1e30, 1e30]
	maxs = [-1e30, -1e30]
	for p in pts:
		s = ti0[0] * p[0] + ti0[1] * p[1] + ti0[2] * p[2] + ti0[3]
		t = ti1[0] * p[0] + ti1[1] * p[1] + ti1[2] * p[2] + ti1[3]
		mins[0] = min(mins[0], s)
		mins[1] = min(mins[1], t)
		maxs[0] = max(maxs[0], s)
		maxs[1] = max(maxs[1], t)
	bmins = [math.floor(mins[0] / 16), math.floor(mins[1] / 16)]
	bmaxs = [math.ceil(maxs[0] / 16), math.ceil(maxs[1] / 16)]
	return bmins, (bmaxs[0] - bmins[0] + 1, bmaxs[1] - bmins[1] + 1)


def light_at(bsp: Bsp, f, ti0, ti1, flags, pts, p) -> tuple[int, int, int]:
	lightofs = f[6]
	if lightofs < 0 or (flags & TEX_SPECIAL) or not bsp.lighting:
		return (192, 192, 192)
	bmins, (lw, lh) = face_extents(ti0, ti1, pts)
	if lw < 1 or lh < 1:
		return (192, 192, 192)
	s = ti0[0] * p[0] + ti0[1] * p[1] + ti0[2] * p[2] + ti0[3]
	t = ti1[0] * p[0] + ti1[1] * p[1] + ti1[2] * p[2] + ti1[3]
	u = max(0.0, min(lw - 1.0, s / 16.0 - bmins[0]))
	v = max(0.0, min(lh - 1.0, t / 16.0 - bmins[1]))
	x0, y0 = int(u), int(v)
	x1, y1 = min(lw - 1, x0 + 1), min(lh - 1, y0 + 1)
	fx, fy = u - x0, v - y0
	acc = [0.0, 0.0, 0.0]
	lit = bsp.lighting
	for x, y, w in (
		(x0, y0, (1 - fx) * (1 - fy)),
		(x1, y0, fx * (1 - fy)),
		(x0, y1, (1 - fx) * fy),
		(x1, y1, fx * fy),
	):
		o = lightofs + (y * lw + x) * 3
		if o + 3 > len(lit):
			continue
		acc[0] += lit[o] * w
		acc[1] += lit[o + 1] * w
		acc[2] += lit[o + 2] * w
	return (
		int(max(0, min(255, round(acc[0])))),
		int(max(0, min(255, round(acc[1])))),
		int(max(0, min(255, round(acc[2])))),
	)


def bake(map_bsp: Path, out_path: Path) -> None:
	bsp = Bsp(map_bsp)
	stem = map_bsp.stem
	mat_index: dict[str, int] = {}
	mat_recs: list[tuple[int, int, int]] = []

	def mat_for(miptex: int, flags: int) -> int | None:
		if miptex < 0 or miptex >= len(bsp.mip_names):
			return None
		name = bsp.mip_names[miptex]
		key = name.lower()
		if not key or key in SKIP_TEX or key.startswith("sky"):
			return None
		if key not in mat_index:
			water = key.startswith("*") or key.startswith("!") or "water" in key
			mflags = (MAT_F_WATER if water else 0) | (MAT_F_NOLIGHT if flags & TEX_SPECIAL else 0)
			mat_index[key] = len(mat_recs)
			mat_recs.append((fnv1a32(key), 0, mflags))
		return mat_index[key]

	face_cluster: dict[int, int] = {}
	for li, leaf in enumerate(bsp.leaves):
		if li == 0:
			continue
		cluster = li - 1
		_contents, _visofs, _mins, _maxs, firstms, numms = leaf
		for k in range(numms):
			fi = bsp.marksurfaces[firstms + k]
			face_cluster.setdefault(fi, cluster)

	chunks: dict[int, Chunk] = {}
	# dmodel: 9f + headnode[4] + visleafs + firstface + numfaces
	world_first = bsp.models[0][14]
	world_num = bsp.models[0][15]
	n_faces = n_orphans = n_skip = 0

	for fi in range(world_first, world_first + world_num):
		f = bsp.faces[fi]
		ti0, ti1, miptex, flags = bsp.texinfos[f[4]]
		mat = mat_for(miptex, flags)
		if mat is None:
			n_skip += 1
			continue
		pts = bsp.face_pts(f)
		if len(pts) < 3:
			continue
		tw, th = bsp.mip_size[miptex]
		name = bsp.mip_names[miptex].lower()
		water = name.startswith("*") or name.startswith("!") or "water" in name

		cluster = face_cluster.get(fi, -1)
		if cluster < 0:
			cx = sum(p[0] for p in pts) / len(pts)
			cy = sum(p[1] for p in pts) / len(pts)
			cz = sum(p[2] for p in pts) / len(pts)
			pl = bsp.planes[f[0]]
			sign = -1.0 if f[1] else 1.0
			cluster = bsp.cluster_at((cx + pl[0] * sign * 4, cy + pl[1] * sign * 4, cz + pl[2] * sign * 4))
		if cluster < 0:
			n_orphans += 1
			continue

		sts = st_for(ti0, ti1, tw, th, pts)
		verts = []
		for p, (s, t) in zip(pts, sts):
			col = (96, 132, 140) if water else light_at(bsp, f, ti0, ti1, flags, pts, p)
			verts.append((p, col, s, t))
		chunks.setdefault(cluster, Chunk()).emit(mat, GX_DRAW_TRIFAN, verts)
		n_faces += 1

	# BSP tree for point-in-leaf → cluster (child encoding matches lostcoast)
	plane_remap: dict[int, int] = {}
	node_blob = bytearray()

	def child(c: int) -> int:
		if c >= 0:
			return c
		leaf = -1 - c
		if leaf <= 0:
			return -1
		return -(leaf - 1 + 2)  # -(cluster + 2)

	if len(bsp.nodes) >= 32768 or bsp.numclusters >= 32000:
		raise SystemExit(f"tree too large: nodes={len(bsp.nodes)} clusters={bsp.numclusters}")
	for n in bsp.nodes:
		planenum, c0, c1 = n[0], n[1], n[2]
		pi = plane_remap.setdefault(planenum, len(plane_remap))
		node_blob += struct.pack(">Hhh", pi, child(c0), child(c1))
	plane_blob = bytearray(16 * len(plane_remap))
	for planenum, pi in plane_remap.items():
		struct.pack_into(">ffff", plane_blob, pi * 16, *bsp.planes[planenum])

	# Quake RLE PVS rows (same encoding the runtime decompresses)
	visbytes = (bsp.numclusters + 7) // 8
	pvs_blob = bytearray()
	pvs_dir = bytearray()
	for cl in range(bsp.numclusters):
		visofs = bsp.leaves[cl + 1][1]
		if visofs < 0 or not bsp.vis:
			pvs_dir += struct.pack(">I", 0xFFFFFFFF)
			continue
		pos = visofs
		done = 0
		while done < visbytes:
			if pos >= len(bsp.vis):
				break
			if bsp.vis[pos]:
				pos += 1
				done += 1
			else:
				if pos + 1 >= len(bsp.vis):
					break
				done += bsp.vis[pos + 1]
				pos += 2
		pvs_dir += struct.pack(">I", len(pvs_blob))
		pvs_blob += bsp.vis[visofs:pos]

	order = sorted(chunks)
	chunk_blobs = [chunks[c].encode() for c in order]
	mats_blob = b"".join(struct.pack(">IBBH", h, tid, fl, 0) for h, tid, fl in mat_recs)
	body = bytearray(64)
	ofs_mats = len(body)
	body += mats_blob
	pad32(body)
	ofs_nodes = len(body)
	body += node_blob
	pad32(body)
	ofs_planes = len(body)
	body += plane_blob
	pad32(body)
	ofs_chunkdir = len(body)
	run = 0
	for cl, blob in zip(order, chunk_blobs):
		ch = chunks[cl]
		body += struct.pack(">II3h3hHH", run, len(blob), *ch.mins, *ch.maxs, cl, 0)
		run += len(blob)
	pad32(body)
	ofs_pvsdir = len(body)
	body += pvs_dir
	pad32(body)
	ofs_pvs = len(body)
	body += pvs_blob
	pad32(body)
	ofs_chunks = len(body)
	for blob in chunk_blobs:
		body += blob
	max_chunk = max((len(b) for b in chunk_blobs), default=0)
	struct.pack_into(
		">4sHH13I",
		body,
		0,
		b"GCwd",
		1,
		0,
		bsp.numclusters,
		len(order),
		len(mat_recs),
		len(bsp.nodes),
		len(plane_remap),
		ofs_mats,
		ofs_nodes,
		ofs_planes,
		ofs_chunkdir,
		ofs_pvsdir,
		ofs_pvs,
		ofs_chunks,
		len(body),
	)
	struct.pack_into(">I", body, 60, max_chunk)
	out_path.parent.mkdir(parents=True, exist_ok=True)
	out_path.write_bytes(body)

	tris = sum(c.tris for c in chunks.values())
	batches = sum(len(c.batches) for c in chunks.values())
	print(f"world DL bake {stem} (GoldSrc BSP30):")
	print(f"  faces={n_faces} skip={n_skip} orphans={n_orphans} tris={tris}")
	print(f"  clusters={bsp.numclusters} chunks={len(order)} batches={batches} mats={len(mat_recs)}")
	print(f"  tree nodes={len(bsp.nodes)} planes={len(plane_remap)}")
	print(f"  MEM1 resident={ofs_pvsdir // 1024} KB  ARAM chunks={run // 1024} KB (max {max_chunk // 1024} KB)")
	print(f"  wrote {out_path} ({len(body) // 1024} KB)")


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--map", type=Path, default=ROOT / "content/dod/maps/dod_avalanche.bsp")
	ap.add_argument(
		"--out",
		type=Path,
		default=None,
		help="Output .gcd path (default: content/dod/media/wd_<map>.gcd)",
	)
	args = ap.parse_args()
	out = args.out or (ROOT / "content/dod/media" / f"wd_{args.map.stem}.gcd")
	bake(args.map, out)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
