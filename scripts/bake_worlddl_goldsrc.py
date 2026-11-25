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
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[1]

GX_DRAW_TRIFAN = 0xA0
GX_VTXFMT = 6
ST_FRAC = 9
LM_FRAC = 8
WD_VERSION = 2
# Header flags: GoldSrc lightmaps are applied at x1 (Lost Coast pages use x2).
WD_FLAG_LIGHT_X1 = 0x0001
# Lightmap pages are LZ4 blocks (see pack_lm_pages_lz4). Opt in with DODGC_WD_LZ4=1 or bake_worlddl_goldsrc.py --lz4.
WD_FLAG_LM_LZ4 = 0x0010
# The PVS rows are a random-access LZ4 region of PVS_BLOCK-byte blocks (pack_blocks_lz4); row offsets stay raw offsets.
WD_FLAG_PVS_LZ4 = 0x0020
PVS_BLOCK = 4096
LM_PAGES_MAX = 1024
# CMPR pages are a quarter of the size, and a cell there owns whole 4x4 blocks, so a page holds at most 63
# one-luxel faces: with one page a leaf the crowded leaves had faces left with no cell at all (flat grey).
# dod_caen alone has more than 2000 lit leaves.  4096 CMPR pages are the bytes of the 1024 RGB565 pages before
# (2 MB of the ARAM stream at most), and the runtime's page hint (GC_WD_LM_HINT) is exact to 4096.
LM_PAGES_MAX_CMPR = 4096
# A crowded leaf takes up to this many pages; a batch names its page in the low bits of its pad word
# (3 bits: r_gx_worlddl.c reads pad & 7). Four pages hold at most 252 CMPR cells, and converted MOHAA
# leaves have more lit faces than that (m5l2b: 2817 faces flat grey); eight hold 504.
LM_CHUNK_PAGES = 8
WD_FLAG_LM_MULTI = 0x0040
# Lightmap pages are 32x32 CMPR (512 bytes) instead of RGB565 (2048): four times the pages resident in the same memory.
WD_FLAG_LM_CMPR = 0x0080
LM_CMPR_BYTES = 512
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

	def pack(self, dims):
		"""Cells for faces (retail lw, lh each) on this one page; see lm_pack_pages."""
		return [c[1] if c else None for c in lm_pack_pages(dims, 1)]

	def blit(self, rect, samples, align: int = 1) -> None:
		"""Resample the face lightmap into its cell: corner luxels stay on the
		cell's corner texels (cell_st), box-filtered when the cell is smaller."""
		x, y, w, h = rect
		lh = len(samples)
		lw = len(samples[0]) if lh else 1

		def taps(n_dst: int, n_src: int, i: int):
			if n_dst == 1:
				return [(j, 1.0 / n_src) for j in range(n_src)]
			c = i * (n_src - 1) / (n_dst - 1)
			r = max(0.5, (n_src - 1) / (n_dst - 1) * 0.5)
			out = []
			for j in range(n_src):
				wgt = max(0.0, 1.0 - abs(j - c) / (2.0 * r)) if r > 0.5 else max(0.0, 1.0 - abs(j - c))
				if wgt > 0.0:
					out.append((j, wgt))
			tot = sum(wg for _j, wg in out)
			return [(j, wg / tot) for j, wg in out]

		xt = [taps(w, lw, px) for px in range(w)]
		yt = [taps(h, lh, py) for py in range(h)]
		for py in range(h):
			for px in range(w):
				acc = [0.0, 0.0, 0.0]
				for sy, wy in yt[py]:
					row = samples[sy]
					for sx, wx in xt[px]:
						c = row[sx]
						wgt = wx * wy
						acc[0] += c[0] * wgt
						acc[1] += c[1] * wgt
						acc[2] += c[2] * wgt
				self.pix[y + py][x + px] = tuple(int(max(0, min(255, round(v)))) for v in acc)
		# the cell owns its blocks: repeat its edge into the rest of them
		if align > 1:
			x1 = min(32, -(-(x + w) // align) * align)
			y1 = min(32, -(-(y + h) // align) * align)
			for py in range(y, y1):
				for px in range(x, x1):
					if px >= x + w or py >= y + h:
						self.pix[py][px] = self.pix[min(py, y + h - 1)][min(px, x + w - 1)]


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


def _c565(c) -> int:
	return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def _c888(v: int):
	r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
	return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def cmpr_block(px) -> bytes:
	"""One 4x4 block (16 RGB tuples, row order) as a GX CMPR block: two RGB565 end colours and 2 bits a texel.
	Lightmaps are smooth, so the ends are the block's darkest and brightest texel by luma; the two middle
	colours are the hardware's 5/8 and 3/8 blends."""
	lum = [c[0] * 2 + c[1] * 5 + c[2] for c in px]
	hi = _c565(px[max(range(16), key=lambda i: lum[i])])
	lo = _c565(px[min(range(16), key=lambda i: lum[i])])
	if hi == lo:
		# c0 <= c1 is the three-colour mode; every texel takes colour 0
		return struct.pack(">HHI", hi, lo, 0)
	c0, c1 = max(hi, lo), min(hi, lo)
	a, b = _c888(c0), _c888(c1)
	pal = (a, b, tuple((5 * a[k] + 3 * b[k]) >> 3 for k in range(3)), tuple((3 * a[k] + 5 * b[k]) >> 3 for k in range(3)))
	bits = 0
	for c in px:
		best = min(range(4), key=lambda i: sum((c[k] - pal[i][k]) ** 2 for k in range(3)))
		bits = (bits << 2) | best
	return struct.pack(">HHI", c0, c1, bits)


def cmpr_page(pix) -> bytes:
	"""32x32 CMPR: 8x8 tiles in row order, each four 4x4 blocks (top left, top right, bottom left, bottom right)."""
	out = bytearray()
	for ty in range(0, 32, 8):
		for tx in range(0, 32, 8):
			for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
				out += cmpr_block([pix[ty + by + y][tx + bx + x] for y in range(4) for x in range(4)])
	return bytes(out)


def cmpr_page_decode(data: bytes):
	"""Texels of a cmpr_page, as the hardware shows them (tests, and judging the loss)."""
	pix = [[(0, 0, 0)] * 32 for _ in range(32)]
	o = 0
	for ty in range(0, 32, 8):
		for tx in range(0, 32, 8):
			for by, bx in ((0, 0), (0, 4), (4, 0), (4, 4)):
				c0, c1, bits = struct.unpack_from(">HHI", data, o)
				o += 8
				a, b = _c888(c0), _c888(c1)
				if c0 > c1:
					pal = (a, b, tuple((5 * a[k] + 3 * b[k]) >> 3 for k in range(3)), tuple((3 * a[k] + 5 * b[k]) >> 3 for k in range(3)))
				else:
					pal = (a, b, tuple((a[k] + b[k]) >> 1 for k in range(3)), (0, 0, 0))
				for i in range(16):
					pix[ty + by + i // 4][tx + bx + i % 4] = pal[(bits >> (30 - 2 * i)) & 3]
	return pix


def lm_cell_sizes(dims, k: int):
	"""Cell sizes of faces (retail lw, lh) at k sixteenths of retail size, never under 2x2."""
	return [(max(2, min(30, -(-lw * k // 16))), max(2, min(30, -(-lh * k // 16)))) for lw, lh in dims]


def lm_shelf_pages(sizes, npages: int, align: int = 1):
	"""Shelf-pack sizes into npages 32x32 pages, tallest first, each page keeping the grey 2x2 at (0,0).
	Returns (page, rect) per size, or None when they do not all fit.  With align 4 (CMPR pages) every cell
	starts on a 4x4 block and owns the blocks it touches, so one face's light never shares a block's two
	end colours with its neighbour's."""
	def up(v: int) -> int:
		return -(-v // align) * align

	first = up(2)
	order = sorted(range(len(sizes)), key=lambda i: (-up(sizes[i][1]), -up(sizes[i][0])))
	out = [None] * len(sizes)
	pg, x, y, rh = 0, first, 0, first
	for i in order:
		w, h = sizes[i]
		aw, ah = up(w), up(h)
		if aw > 32 - first or ah > 32:
			return None
		while True:
			if x + aw > 32:
				x = 0
				y += rh
				rh = 0
			if y + ah <= 32:
				break
			pg, x, y, rh = pg + 1, first, 0, first
			if pg >= npages:
				return None
		out[i] = (pg, (x, y, w, h))
		x += aw
		rh = max(rh, ah)
	return out


def lm_pages_needed(dims, align: int = 1) -> int:
	"""Pages a chunk needs to give every lit face its retail lightmap size (capped at LM_CHUNK_PAGES)."""
	if not dims:
		return 0
	sizes = lm_cell_sizes(dims, 16)
	for n in range(1, LM_CHUNK_PAGES + 1):
		if lm_shelf_pages(sizes, n, align):
			return n
	return LM_CHUNK_PAGES


def lm_grant_pages(needs, budget: int) -> dict:
	"""Pages per chunk from a map-wide budget.  needs is [(chunk, pages wanted)] in order of priority.
	Every chunk gets its first page before any gets a second, every second before any third: a crowded
	leaf late in the order is not left with faces that have no cell while a large one takes four pages."""
	grant: dict = {}
	for level in range(1, LM_CHUNK_PAGES + 1):
		for cl, need in needs:
			if budget < 1:
				return grant
			if need >= level and grant.get(cl, 0) == level - 1:
				grant[cl] = level
				budget -= 1
	return grant


def lm_pack_pages(dims, npages: int, align: int = 1):
	"""(page, rect) for every lit face of a chunk in npages pages.  All faces scale by the largest common
	factor that fits; past 2x2 the smallest faces drop to one averaged luxel; None entries only when even
	that does not fit."""
	if not dims or npages < 1:
		return [None] * len(dims)
	for k in range(16, 0, -1):
		cells = lm_shelf_pages(lm_cell_sizes(dims, k), npages, align)
		if cells:
			return cells
	by_area = sorted(range(len(dims)), key=lambda i: -(dims[i][0] * dims[i][1]))
	for keep in range(len(dims) - 1, -1, -1):
		sizes = [(1, 1)] * len(dims)
		for i in by_area[:keep]:
			sizes[i] = (2, 2)
		cells = lm_shelf_pages(sizes, npages, align)
		if cells:
			return cells
	# More faces than the pages have cells even at one luxel: the largest faces keep a luxel and only the
	# rest go flat grey (before, every face of the chunk did).
	lo, hi = 0, len(dims)	# lo faces fit, hi do not
	while hi - lo > 1:
		mid = (lo + hi) // 2
		if lm_shelf_pages([(1, 1)] * mid, npages, align):
			lo = mid
		else:
			hi = mid
	out = [None] * len(dims)
	cells = lm_shelf_pages([(1, 1)] * lo, npages, align) if lo else None
	for i, cell in zip(by_area[:lo], cells or ()):
		out[i] = cell
	return out


LM_SHARE_SPAN = 8	# a chunk's pages are its first page plus the 3-bit page number of a batch
LM_SHARE_FRESH_Y = 20	# a chunk that does not fit what is left of a page this far down starts a new one


class LmSharedPacker:
	"""Shelf packer over one run of pages for the whole map: chunks are placed in stream order and a chunk
	starts where the one before it stopped, so neighbouring leaves share a page.  One page a leaf left the
	pages 12 percent full at the median (dod_charlie: 2091 pages, 388 pages' worth of luxels) and a beach
	view asked for about 1000 of them, four times what the page store holds."""

	def __init__(self, align: int = 1) -> None:
		self.align = align
		self.first = -(-2 // align) * align	# the grey 2x2 at (0,0) of every page
		self.pg, self.x, self.y, self.rh = 0, self.first, 0, self.first
		self.used = False	# a cell is on page self.pg

	def _up(self, v: int) -> int:
		return -(-v // self.align) * self.align

	def _run(self, sizes, state):
		"""Place sizes (None = no cell) from state; returns (cells, state) with cells as (page, rect)."""
		pg, x, y, rh = state
		order = sorted((i for i, sz in enumerate(sizes) if sz), key=lambda i: (-self._up(sizes[i][1]), -self._up(sizes[i][0])))
		out = [None] * len(sizes)
		for i in order:
			w, h = sizes[i]
			aw, ah = self._up(w), self._up(h)
			if aw > 32 - self.first or ah > 32:
				continue
			while True:
				if x + aw > 32:
					x = 0
					y += rh
					rh = 0
				if y + ah <= 32:
					break
				pg, x, y, rh = pg + 1, self.first, 0, self.first
			out[i] = (pg, (x, y, w, h))
			x += aw
			rh = max(rh, ah)
		return out, (pg, x, y, rh)

	def place(self, sizes):
		"""Cells for one chunk: (first page, [(page - first page, rect) or None per size]).  The pages of a
		chunk stay within LM_SHARE_SPAN of its first."""
		if not any(sizes):
			return 0, [None] * len(sizes)
		state = (self.pg, self.x, self.y, self.rh)
		fresh = (self.pg + 1, self.first, 0, self.first) if self.used else state
		cells, end = self._run(sizes, state)
		pages = sorted({c[0] for c in cells if c})
		if pages and self.used and (pages[-1] - pages[0] >= LM_SHARE_SPAN
				or (pages[-1] > self.pg and self.y + self.rh >= LM_SHARE_FRESH_Y)):
			# straddling the end of a nearly full page would cost the chunk a second page (and batch) for
			# a few luxels of room; and a span past the batch field cannot be addressed at all
			cells, end = self._run(sizes, fresh)
			pages = sorted({c[0] for c in cells if c})
		if not pages:
			return 0, [None] * len(sizes)
		if pages[-1] - pages[0] >= LM_SHARE_SPAN:
			# more than a chunk is ever granted (LM_CHUNK_PAGES); drop what lies past the span
			cells = [c if c and c[0] - pages[0] < LM_SHARE_SPAN else None for c in cells]
		self.pg, self.x, self.y, self.rh = end
		self.used = True
		return pages[0], [(c[0] - pages[0], c[1]) if c else None for c in cells]

	def npages(self) -> int:
		return self.pg + 1 if self.used else 0


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
		self.batches: dict[tuple[int, int], bytearray] = {}	# (material, lightmap page of the chunk)
		self.mins = [32767, 32767, 32767]
		self.maxs = [-32768, -32768, -32768]
		self.tris = 0
		self.lm_pages: list[LmPage] = []
		self.lm_keep = False
		self.lm_shared = False	# lm_pages is a view of the map's shared pages from lm_index on
		self.lm_index = 0
		self.pending: list = []

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

	def emit(self, mat: int, prim: int, verts: list, page: int = 0) -> None:
		"""verts: (pos, rgba, nrm, s, t, lm_u, lm_v) - file version 2 layout:
		POS idx16, NRM s8x3 (6 fraction bits), CLR0 RGBA8, TEX0 s16 ST,
		TEX1 u8 lightmap ST (8 fraction bits)."""
		dl = self.batches.setdefault((mat, page), bytearray())
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
			struct.pack_into(">HHII", out, 16 + 12 * i, m[0], m[1], len(out), len(dl))
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


SB_CLASSES = ("func_wall", "func_illusionary")
SB_MAX = 256
SB_MODEL_MAX = 1024
SB_SOLID = 0x8000


def parse_entities(bsp_bytes: bytes) -> list[dict]:
	text = lump(bsp_bytes, 0).decode("latin1", "replace")
	ents, cur = [], None
	for line in text.splitlines():
		line = line.strip()
		if line == "{":
			cur = {}
		elif line == "}":
			if cur is not None:
				ents.append(cur)
			cur = None
		elif cur is not None and line.startswith('"'):
			parts = line.split('"')
			if len(parts) >= 4:
				cur[parts[1]] = parts[3]
	return ents


def _zero(val: str | None) -> bool:
	if not val:
		return True
	try:
		return all(float(x) == 0.0 for x in val.split())
	except ValueError:
		return False


def static_brushes(bsp: "Bsp") -> list[tuple[int, bool]]:
	"""Brush entities that never change: (model number, solid).  An unnamed
	func_wall or func_illusionary drawn opaque (or masked, render mode 4 at
	full amount) at the lump origin cannot be
	triggered, moved, hidden or faded, so its faces are baked with the world
	and the server keeps it out of the packets (GC_StaticBrushes, model.c).
	The rule reads the entity lump only, so every machine with the same disc
	agrees on the set."""
	ents = parse_entities(bsp.data)
	uses: dict[str, int] = {}
	for e in ents:
		m = e.get("model", "")
		if m.startswith("*"):
			uses[m] = uses.get(m, 0) + 1
	out: list[tuple[int, bool]] = []
	for e in ents:
		cls = e.get("classname", "")
		model = e.get("model", "")
		if cls not in SB_CLASSES or not model.startswith("*") or uses.get(model) != 1:
			continue
		try:
			num = int(model[1:])
		except ValueError:
			continue
		if num < 1 or num >= min(len(bsp.models), SB_MODEL_MAX):
			continue
		if e.get("targetname") or e.get("globalname") or e.get("master"):
			continue
		if not all(_zero(e.get(k)) for k in ("renderfx", "origin", "angles", "effects")):
			continue
		# Render mode 4 at full amount is "solid" with the texture's mask cut
		# out: fences, wire, netting, masts.  It is as fixed as an opaque
		# wall, and the texture pack already carries the mask, so its faces
		# bake too (dod_charlie's flag 3 stands on such a mast, which was a
		# packet entity and dropped out when the ring was full).
		masked = False
		if not _zero(e.get("rendermode")):
			try:
				masked = (float(e.get("rendermode", "0")) == 4.0
					and float(e.get("renderamt", "0")) == 255.0)
			except ValueError:
				masked = False
			if not masked:
				continue
		if cls == "func_illusionary" and e.get("skin", "0").strip() not in ("", "0", "-1"):
			continue
		first, count = bsp.models[num][14], bsp.models[num][15]
		plain = True
		for fi in range(first, first + count):
			miptex = bsp.texinfos[bsp.faces[fi][4]][2]
			name = bsp.mip_names[miptex].lower() if 0 <= miptex < len(bsp.mip_names) else ""
			if name[:1] == "{" and masked:
				continue
			if name[:1] in ("{", "!", "*", "+", "-") or name.startswith("scroll") or "water" in name:
				plain = False
				break
		if not plain:
			continue
		out.append((num, cls == "func_wall"))
		if len(out) >= SB_MAX:
			break
	return out


WATER_CLASSES = ("func_water",)


def static_water(bsp: "Bsp") -> list[int]:
	"""func_water brushes whose faces bake with the world (model numbers).  The
	lean route has no live surface table for brush models, so a func_water drawn
	as an entity never shows: Omaha's whole sea (dod_charlie *139) was missing.
	An unnamed one at the lump origin never moves, so its faces bake like the
	world's own water.  It stays a packet entity (it is not in the static list)
	because the game still needs its water volume for swimming and splashes."""
	ents = parse_entities(bsp.data)
	uses: dict[str, int] = {}
	for e in ents:
		m = e.get("model", "")
		if m.startswith("*"):
			uses[m] = uses.get(m, 0) + 1
	out: list[int] = []
	for e in ents:
		model = e.get("model", "")
		if e.get("classname", "") not in WATER_CLASSES or not model.startswith("*") or uses.get(model) != 1:
			continue
		try:
			num = int(model[1:])
		except ValueError:
			continue
		if num < 1 or num >= min(len(bsp.models), SB_MODEL_MAX):
			continue
		if e.get("targetname") or e.get("globalname") or e.get("master"):
			continue
		if not all(_zero(e.get(k)) for k in ("renderfx", "origin", "angles", "effects", "rendermode")):
			continue
		out.append(num)
	return out


def face_clusters(bsp: "Bsp", f, pts) -> list[int]:
	"""PVS clusters a brush entity face is seen from.  Its faces are not cut
	by the world tree, so sample in front of the middle and of each corner;
	the face is emitted into every cluster found (same depth, drawn once per
	visible chunk)."""
	pl = bsp.planes[f[0]]
	sign = -1.0 if f[1] else 1.0
	n = len(pts)
	c = [sum(p[a] for p in pts) / n for a in range(3)]
	found: list[int] = []
	for dist in (2.0, 8.0):
		samples = [c] + [[p[a] + (c[a] - p[a]) * 0.15 for a in range(3)] for p in pts]
		for q in samples:
			cl = bsp.cluster_at(tuple(q[a] + pl[a] * sign * dist for a in range(3)))
			if cl >= 0 and cl not in found:
				found.append(cl)
		if found:
			break
	return found[:6]


def write_static_brushes(out_path: Path, statics: list[tuple[int, bool]], wd_bytes: int) -> None:
	"""media/wd_<map>_sb.gcd, big-endian: "GCsb", version, count, byte size of
	the world DL these faces were baked into (the runtime ignores the list if
	the pack on disc is another size), then one u16 per entity: model number,
	bit 15 set when solid."""
	body = bytearray(struct.pack(">4sHHI", b"GCsb", 1, len(statics), wd_bytes))
	for num, solid in statics:
		body += struct.pack(">H", num | (SB_SOLID if solid else 0))
	out_path.parent.mkdir(parents=True, exist_ok=True)
	out_path.write_bytes(body)


LM_PAGE_BYTES = 2048


def pack_blocks_lz4(data: bytes, block: int) -> bytes:
	"""Random-access LZ4 region: data cut into `block`-byte blocks, each LZ4-compressed when that is smaller.

	Layout: u32 n_blocks, u32 offset[n_blocks + 1] (bytes from the region start; a blob start is 32-aligned and its low
	5 bits hold the zero padding bytes AFTER the LZ4 stream; the last offset is the region size), padded to 32; then
	the blobs, each padded to 32. A block stored raw has no padding count and a size of its raw length rounded up to
	32 (a compressed block is always smaller than that). The last block may be short. The engine stages a blob and
	decodes it into a cache buffer (in-place decode of an exact-fit buffer is impossible)."""
	import gc_lz4
	n = (len(data) + block - 1) // block
	blobs = []	# (stored bytes incl. padding, padding count)
	for i in range(n):
		raw = bytes(data[i * block:(i + 1) * block])
		rawsz = (len(raw) + 31) & ~31
		cand = gc_lz4.compress(raw)
		size = (len(cand) + 31) & ~31
		best = (cand + bytes(size - len(cand)), size - len(cand)) if size < rawsz else (raw + bytes(rawsz - len(raw)), 0)
		blobs.append(best)
	head_len = (4 + 4 * (n + 1) + 31) & ~31
	region = bytearray(struct.pack(">I", n))
	ofs = head_len
	offs = []
	for blob, pad in blobs:
		offs.append(ofs | pad)
		ofs += len(blob)
	offs.append(ofs)
	region += b"".join(struct.pack(">I", o) for o in offs)
	region += bytes(head_len - len(region))
	for blob, _pad in blobs:
		region += blob
	return bytes(region)


def pack_lm_pages_lz4(lm_pages: bytes, page_bytes: int = LM_PAGE_BYTES) -> bytes:
	"""Lightmap region with every 2 KiB page LZ4-compressed (header flag WD_FLAG_LM_LZ4); see pack_blocks_lz4.
	Pages compress about 3.5:1 (1 MiB of pages per map -> about 290 KiB)."""
	return pack_blocks_lz4(lm_pages, page_bytes)


LF_BLOCK = 1024


def bake(map_bsp: Path, out_path: Path, lz4: bool = False, lm_cmpr: bool | None = None) -> None:
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

	statics = static_brushes(bsp)
	face_list = [(fi, False) for fi in range(world_first, world_first + world_num)]
	for num, _solid in statics:
		first, count = bsp.models[num][14], bsp.models[num][15]
		face_list += [(fi, True) for fi in range(first, first + count)]
	waters = static_water(bsp)
	for num in waters:
		first, count = bsp.models[num][14], bsp.models[num][15]
		face_list += [(fi, True) for fi in range(first, first + count)]
	n_static_faces = 0

	for fi, is_static in face_list:
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

		if is_static:
			clusters = face_clusters(bsp, f, pts)
		else:
			cluster = face_cluster.get(fi, -1)
			if cluster < 0:
				cx = sum(p[0] for p in pts) / len(pts)
				cy = sum(p[1] for p in pts) / len(pts)
				cz = sum(p[2] for p in pts) / len(pts)
				pl = bsp.planes[f[0]]
				sign = -1.0 if f[1] else 1.0
				cluster = bsp.cluster_at((cx + pl[0] * sign * 4, cy + pl[1] * sign * 4, cz + pl[2] * sign * 4))
			clusters = [cluster] if cluster >= 0 else []
		if not clusters:
			n_orphans += 1
			continue

		sts = st_for(ti0, ti1, tw, th, pts)
		pl = bsp.planes[f[0]]
		nrm = (-pl[0], -pl[1], -pl[2]) if f[1] else (pl[0], pl[1], pl[2])
		for cluster in clusters:
			chunk = chunks.setdefault(cluster, Chunk())
			lw = lh = 1
			bmins = (0, 0)
			samples = None
			if not water and not (flags & TEX_SPECIAL) and f[6] >= 0 and bsp.lighting:
				bmins, (lw, lh) = face_extents(ti0, ti1, pts)
				samples = face_lightmap(bsp, f[6], lw, lh)
			verts = []
			for p, (s, t) in zip(pts, sts):
				col = (96, 132, 140) if water else light_at(bsp, f, ti0, ti1, flags, pts, p)
				u = (ti0[0] * p[0] + ti0[1] * p[1] + ti0[2] * p[2] + ti0[3]) / 16.0 - bmins[0]
				v = (ti1[0] * p[0] + ti1[1] * p[1] + ti1[2] * p[2] + ti1[3]) / 16.0 - bmins[1]
				verts.append((p, col, nrm, s, t, u, v))
			chunk.pending.append((mat, verts, lw, lh, samples))
		n_faces += 1
		if is_static:
			n_static_faces += 1

	# Lightmap cells are packed per chunk once all of its faces are known.  A chunk takes as many pages as
	# its faces need at retail size (up to LM_CHUNK_PAGES), largest chunks first while the page budget lasts;
	# after that one page each, and past the budget none (vertex colour lights the chunk).
	if lm_cmpr is None:
		lm_cmpr = os.environ.get("DODGC_WD_LM_RGB565", "") != "1"
	lm_align = 4 if lm_cmpr else 1
	n_lm_soft = n_lm_flat = 0
	lit_of: dict[int, list[int]] = {}
	spans = []
	for cl, chunk in chunks.items():
		lit_of[cl] = [i for i, rec in enumerate(chunk.pending) if rec[4]]
		if not lit_of[cl]:
			continue
		lo = [min(v[0][a] for rec in chunk.pending for v in rec[1]) for a in range(3)]
		hi = [max(v[0][a] for rec in chunk.pending for v in rec[1]) for a in range(3)]
		spans.append((max(hi[a] - lo[a] for a in range(3)), cl))
	spans.sort(reverse=True)
	needs = []
	for _span, cl in spans:
		dims = [(chunks[cl].pending[i][2], chunks[cl].pending[i][3]) for i in lit_of[cl]]
		needs.append((cl, max(1, lm_pages_needed(dims, lm_align))))
	lm_grant = lm_grant_pages(needs, LM_PAGES_MAX_CMPR if lm_cmpr else LM_PAGES_MAX)
	# CMPR pages are shared: the cell sizes come from the chunk's own grant as before, their places from one
	# packer that runs through the chunks in stream order.
	lm_share = LmSharedPacker(lm_align) if lm_cmpr and os.environ.get("DODGC_WD_LM_SHARE", "") != "0" else None
	lm_shared_pages: list[LmPage] = []
	for cl in sorted(chunks):
		chunk = chunks[cl]
		lit = lit_of[cl]
		npages = lm_grant.get(cl, 0)
		placed = lm_pack_pages([(chunk.pending[i][2], chunk.pending[i][3]) for i in lit], npages, lm_align)
		if lm_share:
			chunk.lm_index, placed = lm_share.place([(c[1][2], c[1][3]) if c else None for c in placed])
			while len(lm_shared_pages) < lm_share.npages():
				lm_shared_pages.append(LmPage())
			chunk.lm_pages = lm_shared_pages[chunk.lm_index:]	# page n of the chunk is chunk.lm_pages[n]
			chunk.lm_keep = any(placed)
			chunk.lm_shared = True
		else:
			chunk.lm_pages = [LmPage() for _ in range(max((c[0] for c in placed if c), default=-1) + 1)]
			chunk.lm_keep = bool(chunk.lm_pages)
		cells = dict(zip(lit, placed))
		for i, (mat, verts, lw, lh, samples) in enumerate(chunk.pending):
			page, cell = cells.get(i) or (0, None)
			if cell:
				chunk.lm_pages[page].blit(cell, samples, lm_align)
				if cell[2] < min(lw, 30) or cell[3] < min(lh, 30):
					n_lm_soft += 1
			elif samples and npages:
				n_lm_flat += 1
			out = []
			for p, col, nrm, s, t, u, v in verts:
				lu, lv = cell_st(cell, u, v, lw, lh) if cell else GREY_UV
				out.append((p, col, nrm, s, t, lu, lv))
			chunk.emit(mat, GX_DRAW_TRIFAN, out, page)
		chunk.pending = []

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
	# Lightmap pages are a stream after the chunks, a chunk's pages in a row; the runtime fetches one when a
	# batch on it is drawn (MEM1 page cache, ref/gx/r_gx_worlddl.c GC_WD_LM_SLOTS).
	lm_page_bytes = LM_CMPR_BYTES if lm_cmpr else LM_PAGE_BYTES
	lm_pages = bytearray()
	lm_multi = 0
	for pg in lm_shared_pages:
		lm_pages += cmpr_page(pg.pix)
	for cl in order:
		if chunks[cl].lm_keep and chunks[cl].lm_shared:
			lm_multi += any(page for _mat, page in chunks[cl].batches)
		elif chunks[cl].lm_keep:
			chunks[cl].lm_index = len(lm_pages) // lm_page_bytes
			for pg in chunks[cl].lm_pages:
				lm_pages += cmpr_page(pg.pix) if lm_cmpr else rgb565_page(pg.pix)
			lm_multi += len(chunks[cl].lm_pages) > 1
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
	lz4 = lz4 or os.environ.get("DODGC_WD_LZ4", "") == "1"
	for cl, blob in zip(order, chunk_blobs):
		ch = chunks[cl]
		body += struct.pack(">II3h3hHH", run, len(blob), *ch.mins, *ch.maxs, cl, 0)
		run += len(blob)
	pad32(body)
	ofs_pvsdir = len(body)
	body += pvs_dir
	pad32(body)
	ofs_pvs = len(body)
	if lz4 and pvs_blob:
		# whole blocks only: the engine decodes every block to PVS_BLOCK bytes
		pvs_blob += bytes(-len(pvs_blob) % PVS_BLOCK)
		body += pack_blocks_lz4(bytes(pvs_blob), PVS_BLOCK)
	else:
		body += pvs_blob
	pad32(body)
	ofs_chunks = len(body)
	for blob in chunk_blobs:
		body += blob
	pad32(body)
	body += pack_lm_pages_lz4(lm_pages, lm_page_bytes) if lz4 and lm_pages else lm_pages
	max_chunk = max((len(b) for b in chunk_blobs), default=0)
	struct.pack_into(
		">4sHH13I",
		body,
		0,
		b"GCwd",
		WD_VERSION,
		WD_FLAG_LIGHT_X1 | (WD_FLAG_LM_MULTI if lm_multi else 0) | (WD_FLAG_LM_CMPR if lm_cmpr and lm_pages else 0) | (WD_FLAG_LM_LZ4 if lz4 and lm_pages else 0) | (WD_FLAG_PVS_LZ4 if lz4 and pvs_blob else 0),
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
	write_static_brushes(out_path.with_name(out_path.stem + "_sb.gcd"), statics, len(body))

	tris = sum(c.tris for c in chunks.values())
	batches = sum(len(c.batches) for c in chunks.values())
	print(f"world DL bake {stem} (GoldSrc BSP30):")
	print(f"  faces={n_faces} skip={n_skip} orphans={n_orphans} tris={tris}")
	print(f"  static brush entities={len(statics)} (solid {sum(1 for _n, so in statics if so)}) faces={n_static_faces}")
	print(f"  static func_water baked={len(waters)}")
	print(f"  clusters={bsp.numclusters} chunks={len(order)} batches={batches} mats={len(mat_recs)}")
	print(f"  tree nodes={len(bsp.nodes)} planes={len(plane_remap)}")
	print(f"  MEM1 resident={ofs_pvsdir // 1024} KB  ARAM chunks={run // 1024} KB (max {max_chunk // 1024} KB)")
	print(f"  lightmap pages={len(lm_pages) // lm_page_bytes} ({len(lm_pages) // 1024} KB, {'CMPR' if lm_cmpr else 'RGB565'}), {lm_multi} chunks with more than one")
	print(f"  lightmap cells: {n_lm_soft} faces below retail size, {n_lm_flat} flat grey (no room)")
	print(f"  wrote {out_path} ({len(body) // 1024} KB)")


LF_FLAG_SKY = 1
LF_FLAG_TILED = 2
LF_NO_LIGHT = 0xFFFFFF


def bake_lightfaces(map_bsp: Path, out_path: Path, lz4: bool = False) -> None:
	"""media/wd_<map>_lf.gcd: what GoldSrc's light point needs from each face
	when the world is drawn from the world DL and no msurface table exists
	(runtime ref/common/ref_light.c).  Header (32 bytes, big-endian):
	"GClf", version, faces, record offset, lighting offset, lighting bytes.
	Record (56 bytes): texinfo vecs[2][4] floats, texturemins[2] and
	extents[2] ints (Mod_CalcSurfaceExtents, 16-unit luxels), flags << 24 |
	lightofs (0xFFFFFF: none), styles[4].  Then the BSP lighting lump."""
	bsp = Bsp(map_bsp)
	recs = bytearray()
	for f in bsp.faces:
		_pn, _side, _first, _num, ti, styles, lightofs = f
		vecs0, vecs1, miptex, tflags = bsp.texinfos[ti]
		name = bsp.mip_names[miptex].lower() if 0 <= miptex < len(bsp.mip_names) else ""
		flags = 0
		if name.startswith("sky"):
			flags |= LF_FLAG_SKY
		if tflags & TEX_SPECIAL:
			flags |= LF_FLAG_TILED
		pts = bsp.face_pts(f)
		mins = [1e30, 1e30]
		maxs = [-1e30, -1e30]
		for p in pts:
			for j, v in enumerate((vecs0, vecs1)):
				val = v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[3]
				mins[j] = min(mins[j], val)
				maxs[j] = max(maxs[j], val)
		if not pts:
			mins = maxs = [0.0, 0.0]
		bmins = [math.floor(mins[j] / 16) for j in range(2)]
		bmaxs = [math.ceil(maxs[j] / 16) for j in range(2)]
		tmins = [bmins[j] * 16 for j in range(2)]
		ext = [(bmaxs[j] - bmins[j]) * 16 for j in range(2)]
		lo = lightofs if 0 <= lightofs < len(bsp.lighting) else LF_NO_LIGHT
		recs += struct.pack(">8f4iI4s", *vecs0, *vecs1, *tmins, *ext, (flags << 24) | (lo & LF_NO_LIGHT), styles)
	lz4 = lz4 or os.environ.get("DODGC_WD_LZ4", "") == "1"
	if lz4:
		# Version 3: the records stay raw, the lighting lump is a random-access LZ4 region (pack_blocks_lz4,
		# LF_BLOCK-byte blocks). The light-point walk reads the faces of every node along a ray (about 25 record
		# blocks a frame): LZ4 records thrashed the 1-block cache at 0.5 ms a frame (Dolphin counters), so only the
		# lighting is packed. Header: "GClf", 3, faces, record offset (32), lighting region offset, lighting raw bytes,
		# file size, block size. (Version 2 packed the records too; the engine still reads it.)
		lit_region = pack_blocks_lz4(bytes(bsp.lighting), LF_BLOCK) if bsp.lighting else b""
		body = bytearray(32)
		body += recs
		pad32(body)
		lit_ofs = len(body)
		body += lit_region
		pad32(body)
		struct.pack_into(">4s7I", body, 0, b"GClf", 3, len(bsp.faces), 32, lit_ofs, len(bsp.lighting), len(body), LF_BLOCK)
	else:
		body = bytearray(32)
		body += recs
		pad32(body)
		lit_ofs = len(body)
		body += bsp.lighting
		pad32(body)
		struct.pack_into(">4s5I", body, 0, b"GClf", 1, len(bsp.faces), 32, lit_ofs, len(bsp.lighting))
	out_path.parent.mkdir(parents=True, exist_ok=True)
	out_path.write_bytes(body)
	print(f"  light faces: {len(bsp.faces)} faces, lighting {len(bsp.lighting) // 1024} KB, wrote {out_path} ({len(body) // 1024} KB)")


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--map", type=Path, default=ROOT / "content/dod/maps/dod_avalanche.bsp")
	ap.add_argument(
		"--out",
		type=Path,
		default=None,
		help="Output .gcd path (default: content/dod/media/wd_<map>.gcd)",
	)
	ap.add_argument("--lz4", action="store_true", help="LZ4-compress each lightmap page (also DODGC_WD_LZ4=1)")
	args = ap.parse_args()
	out = args.out or (ROOT / "content/dod/media" / f"wd_{args.map.stem}.gcd")
	bake(args.map, out, lz4=args.lz4)
	bake_lightfaces(args.map, out.with_name(out.stem + "_lf.gcd"), lz4=args.lz4)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
