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
LM_FRAC = 8
WD_VERSION = 2
# Header flags: GoldSrc lightmaps are applied at x1 (Lost Coast pages use x2).
WD_FLAG_LIGHT_X1 = 0x0001
LM_PAGES_MAX = 512
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


class LmPage:
	"""32x32 RGB565 lightmap page per chunk (runtime TEXMAP2, TEV x2): each lit
	face gets a square cell holding its retail lightmap.  Cell (0,0) is a
	mid-grey 2x2 (1x light) for faces that found no room."""

	def __init__(self) -> None:
		self.pix = [[(128, 128, 128) for _ in range(32)] for _ in range(32)]
		self.x = 2
		self.y = self.row_h = 0
		self.row_h = 2
		self.used = False
		self.failed = False

	def _try(self, w: int, h: int):
		x, y, rh = self.x, self.y, self.row_h
		if x + w > 32:
			x = 0
			y += rh
			rh = 0
		if y + h > 32:
			return None
		self.x, self.y, self.row_h = x + w, y, max(rh, h)
		return (x, y, w, h)

	def alloc(self, w: int, h: int):
		if self.failed:
			return None
		side = max(2, min(16, max(w, h)))
		for sz in (side, 8, 4, 2):
			if sz > side:
				continue
			rect = self._try(sz, sz)
			if rect:
				self.used = True
				return rect
		self.failed = True
		return None

	def blit(self, rect, samples) -> None:
		x, y, w, h = rect
		lh = len(samples)
		lw = len(samples[0]) if lh else 1
		for py in range(h):
			for px in range(w):
				sx = min(lw - 1, px * lw // w)
				sy = min(lh - 1, py * lh // h)
				self.pix[y + py][x + px] = samples[sy][sx]


def rgb565_page(pix) -> bytes:
	"""32x32 RGB565 in GX 4x4 tiles."""
	out = bytearray()
	for ty in range(0, 32, 4):
		for tx in range(0, 32, 4):
			for y in range(4):
				for x in range(4):
					r, g, b = pix[ty + y][tx + x]
					out += struct.pack(">H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
	return bytes(out)


def cell_st(cell, u: float, v: float, lw: int, lh: int) -> tuple[float, float]:
	"""Page UV (0..1) of luxel coordinate (u, v) of an lw x lh face lightmap."""
	x, y, w, h = cell
	un = min(1.0, max(0.0, u / max(lw - 1, 1)))
	vn = min(1.0, max(0.0, v / max(lh - 1, 1)))
	return (x + un * max(w - 1, 0) + 0.5) / 32.0, (y + vn * max(h - 1, 0) + 0.5) / 32.0


GREY_UV = (1.0 / 32.0, 1.0 / 32.0)


class Chunk:
	def __init__(self) -> None:
		self.pos: dict[tuple[int, int, int], int] = {}
		self.batches: dict[int, bytearray] = {}
		self.mins = [32767, 32767, 32767]
		self.maxs = [-32768, -32768, -32768]
		self.tris = 0
		self.lm = LmPage()
		self.lm_keep = False
		self.lm_index = 0

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
		"""verts: (pos, rgba, nrm, s, t, lm_u, lm_v) - file version 2 layout:
		POS idx16, NRM s8x3 (6 fraction bits), CLR0 RGBA8, TEX0 s16 ST,
		TEX1 u8 lightmap ST (8 fraction bits)."""
		dl = self.batches.setdefault(mat, bytearray())
		dl += struct.pack(">BH", prim | GX_VTXFMT, len(verts))
		for p, c, nrm, s, t, lu, lv in verts:
			dl += struct.pack(
				">Hbbb4BhhBB",
				self.pos_index(p),
				*(int(max(-127, min(127, round(n * 64.0)))) for n in nrm),
				max(0, min(255, c[0])), max(0, min(255, c[1])), max(0, min(255, c[2])), 255,
				int(max(-32768, min(32767, round(s * (1 << ST_FRAC))))),
				int(max(-32768, min(32767, round(t * (1 << ST_FRAC))))),
				int(max(0, min(255, round(lu * (1 << LM_FRAC))))),
				int(max(0, min(255, round(lv * (1 << LM_FRAC))))),
			)
		self.tris += len(verts) - 2

	def encode(self) -> bytes:
		mats = sorted(self.batches)
		out = bytearray(16 + 12 * len(mats))
		pad32(out)
		pos_ofs = len(out)
		for key in self.pos:
			out += struct.pack(">hhh", *key)
		pad32(out)
		for i, m in enumerate(mats):
			dl = bytearray(self.batches[m])
			pad32(dl)
			struct.pack_into(">HHII", out, 16 + 12 * i, m, 0, len(out), len(dl))
			out += dl
		# Page index + 1 (0: no page, vertex colour lights the chunk).
		struct.pack_into(">HHII", out, 0, len(self.pos), len(mats), pos_ofs,
			self.lm_index + 1 if self.lm_keep else 0)
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


def face_lightmap(bsp: Bsp, lightofs: int, lw: int, lh: int):
	"""Style-0 retail lightmap of a face as rows of RGB, or None."""
	lit = bsp.lighting
	if lightofs < 0 or lw < 1 or lh < 1 or lw * lh > 65536 or lightofs + lw * lh * 3 > len(lit):
		return None
	return [[tuple(lit[lightofs + (y * lw + x) * 3: lightofs + (y * lw + x) * 3 + 3]) for x in range(lw)] for y in range(lh)]


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
		chunk = chunks.setdefault(cluster, Chunk())
		pl = bsp.planes[f[0]]
		nrm = (-pl[0], -pl[1], -pl[2]) if f[1] else (pl[0], pl[1], pl[2])
		cell = None
		lw = lh = 1
		bmins = (0, 0)
		if not water and not (flags & TEX_SPECIAL) and f[6] >= 0 and bsp.lighting:
			bmins, (lw, lh) = face_extents(ti0, ti1, pts)
			samples = face_lightmap(bsp, f[6], lw, lh)
			if samples:
				cell = chunk.lm.alloc(lw, lh)
				if cell:
					chunk.lm.blit(cell, samples)
		verts = []
		for p, (s, t) in zip(pts, sts):
			col = (96, 132, 140) if water else light_at(bsp, f, ti0, ti1, flags, pts, p)
			if cell:
				u = (ti0[0] * p[0] + ti0[1] * p[1] + ti0[2] * p[2] + ti0[3]) / 16.0 - bmins[0]
				v = (ti1[0] * p[0] + ti1[1] * p[1] + ti1[2] * p[2] + ti1[3]) / 16.0 - bmins[1]
				lu, lv = cell_st(cell, u, v, lw, lh)
			else:
				lu, lv = GREY_UV
			verts.append((p, col, nrm, s, t, lu, lv))
		chunk.emit(mat, GX_DRAW_TRIFAN, verts)
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
	# Lightmap pages: largest chunks first, up to the page budget.  The pages
	# are a stream after the chunks; the runtime fetches one when its chunk
	# is drawn (MEM1 page cache, ref/gx/r_gx_worlddl.c GC_WD_LM_SLOTS).
	ranked = sorted(
		((max(ch.maxs[a] - ch.mins[a] for a in range(3)), cl) for cl, ch in chunks.items() if ch.pos and ch.lm.used),
		reverse=True,
	)
	lm_pages = bytearray()
	for _span, cl in ranked[:LM_PAGES_MAX]:
		chunks[cl].lm_keep = True
	for cl in order:
		if chunks[cl].lm_keep:
			chunks[cl].lm_index = len(lm_pages) // 2048
			lm_pages += rgb565_page(chunks[cl].lm.pix)
	chunk_blobs = [chunks[c].encode() for c in order]
	mats_blob = b"".join(struct.pack(">IIBBH", h, 0, tid, fl, 0) for h, tid, fl in mat_recs)
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
	pad32(body)
	body += lm_pages
	max_chunk = max((len(b) for b in chunk_blobs), default=0)
	struct.pack_into(
		">4sHH13I",
		body,
		0,
		b"GCwd",
		WD_VERSION,
		WD_FLAG_LIGHT_X1,
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
	print(f"  lightmap pages={len(lm_pages) // 2048} ({len(lm_pages) // 1024} KB)")
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
