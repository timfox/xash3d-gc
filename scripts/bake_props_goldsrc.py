#!/usr/bin/env python3
"""Bake a GoldSrc map's static studio props into a GX prop pack.

Day of Defeat maps place their scenery (beach hedgehogs, barbed wire,
sandbags, bunkers, trees...) as env_model / dod_object entities. On the
GameCube each loaded studio model costs MEM1, so the engine could keep six
prop models per map and dropped the rest (dod_charlie: 87 of 144 props).

This writes the same pack the Lost Coast props use (ref/gx/r_gx_world.c,
R_GXProp*): media/gp_<map>.gcp holds every model once, as GX display lists
in its posed shape, and one 32-byte instance per placement; media/pt_<map>.gct
holds the textures as CMPR mip chains. Both stay in ARAM. The renderer draws
the instances through the position matrix, so no studio model is loaded, no
edict is used and nothing is sent over the network: every client has the pack.

An entity goes into the pack when the engine's static prop rule takes it
(sv_game.c SV_GCTryStaticProp): classname env_model or dod_object, no
targetname.
"""
from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

PROP_CLASSES = ("env_model", "dod_object")
GC_PR_MAGIC = b"GCpr"
GC_PR_VERSION = 7
GC_PR_MAX_INST = 512
GC_PR_MAX_MODELS = 192
GC_PR_MAX_MAT = 127
GC_PR_PIECE_BYTES = 15360	# piece body cap: header + body fit the engine's 16 KB bounce
GC_PR_CMD_VERTS = 1200
GC_WT_ENTRY = struct.Struct(">16sHHBBHI")
GC_WT_FLAG_ALPHA = 1
TEX_MAX_SIDE = 128

STUDIO_NF_CHROME = 0x0002
STUDIO_NF_ADDITIVE = 0x0020
STUDIO_NF_MASKED = 0x0040
BONE_SZ, BODYPART_SZ, TEXTURE_SZ, MODEL_SZ, MESH_SZ, SEQ_SZ = 112, 76, 80, 112, 20, 176


def oct_normal_table():
	"""The 256 directions a prop vertex normal can take: a 16 x 16 octahedral
	grid. The engine builds the same table (r_gx_world.c, R_GXPropLoad)."""
	out = []
	for q in range(256):
		u = ((q & 15) + 0.5) * (2.0 / 16.0) - 1.0
		v = ((q >> 4) + 0.5) * (2.0 / 16.0) - 1.0
		x, y, z = u, v, 1.0 - abs(u) - abs(v)
		if z < 0.0:
			x = (1.0 - abs(v)) * (-1.0 if u < 0.0 else 1.0)
			y = (1.0 - abs(u)) * (-1.0 if v < 0.0 else 1.0)
		l = math.sqrt(x * x + y * y + z * z)
		out.append((x / l, y / l, z / l))
	return out


_OCT = oct_normal_table()
_OCT_CACHE: dict[tuple[int, int, int], int] = {}


def oct_index(n) -> int:
	key = (int(round(n[0] * 31)), int(round(n[1] * 31)), int(round(n[2] * 31)))
	got = _OCT_CACHE.get(key)
	if got is None:
		best, bd = 0, -2.0
		for i, t in enumerate(_OCT):
			d = n[0] * t[0] + n[1] * t[1] + n[2] * t[2]
			if d > bd:
				best, bd = i, d
		got = _OCT_CACHE[key] = best
	return got


# ---- studio model -------------------------------------------------------

def _quat(angles):
	"""AngleQuaternion( ..., studio = true ): bone rotations are radians."""
	sy, cy = math.sin(angles[2] * 0.5), math.cos(angles[2] * 0.5)
	sp, cp = math.sin(angles[1] * 0.5), math.cos(angles[1] * 0.5)
	sr, cr = math.sin(angles[0] * 0.5), math.cos(angles[0] * 0.5)
	return (
		sr * cp * cy - cr * sp * sy,
		cr * sp * cy + sr * cp * sy,
		cr * cp * sy - sr * sp * cy,
		cr * cp * cy + sr * sp * sy,
	)


def _mat_quat_origin(q, o):
	x, y, z, w = q
	return [
		[1.0 - 2.0 * y * y - 2.0 * z * z, 2.0 * x * y - 2.0 * w * z, 2.0 * x * z + 2.0 * w * y, o[0]],
		[2.0 * x * y + 2.0 * w * z, 1.0 - 2.0 * x * x - 2.0 * z * z, 2.0 * y * z - 2.0 * w * x, o[1]],
		[2.0 * x * z - 2.0 * w * y, 2.0 * y * z + 2.0 * w * x, 1.0 - 2.0 * x * x - 2.0 * y * y, o[2]],
	]


def _mat_concat(a, b):
	out = [[0.0] * 4 for _ in range(3)]
	for i in range(3):
		for j in range(3):
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]
		out[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3]
	return out


def _xform(m, v):
	return (
		m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3],
		m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3],
		m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3],
	)


def _rotate(m, v):
	return (
		m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
		m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
		m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
	)


def angle_matrix(pitch, yaw, roll):
	"""AngleMatrix (degrees), the matrix R_GXPropEmitModel builds."""
	p, y, r = math.radians(pitch), math.radians(yaw), math.radians(roll)
	sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
	return [
		[cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy, 0.0],
		[cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy, 0.0],
		[-sp, sr * cp, cr * cp, 0.0],
	]


class StudioModel:
	"""The parts of a GoldSrc studio model (version 10) a static bake needs."""

	def __init__(self, path: Path):
		d = self.data = path.read_bytes()
		if d[:4] != b"IDST" or struct.unpack_from("<i", d, 4)[0] != 10:
			raise ValueError(f"{path}: not a version 10 studio model")
		(
			self.flags, self.numbones, self.boneindex, _nbc, _bci, _nhb, _hbi,
			self.numseq, self.seqindex, _nsg, _sgi,
			self.numtextures, self.textureindex, _tdi,
			self.numskinref, self.numskinfamilies, self.skinindex,
			self.numbodyparts, self.bodypartindex,
		) = struct.unpack_from("<19i", d, 136)
		# Textures in a companion <name>t.mdl when the model has none.
		self.tex = self
		if self.numtextures == 0:
			comp = path.with_name(path.stem + "t" + path.suffix)
			if not comp.is_file():
				comp = path.with_name(path.stem + "T" + path.suffix)
			if comp.is_file():
				self.tex = StudioModel(comp)

	def bone_matrices(self, sequence: int):
		"""World matrices of the bones at frame 0 of a sequence (the bind
		pose when the sequence is not in this file)."""
		d = self.data
		anim = None
		if 0 <= sequence < self.numseq:
			so = self.seqindex + sequence * SEQ_SZ
			animindex, = struct.unpack_from("<i", d, so + 124)
			seqgroup, = struct.unpack_from("<i", d, so + 156)
			if seqgroup == 0 and 0 < animindex < len(d):
				anim = animindex
		mats = []
		for b in range(self.numbones):
			bo = self.boneindex + b * BONE_SZ
			parent, = struct.unpack_from("<i", d, bo + 32)
			value = list(struct.unpack_from("<6f", d, bo + 64))
			scale = struct.unpack_from("<6f", d, bo + 88)
			if anim is not None:
				ao = anim + b * 12
				if ao + 12 <= len(d):
					offs = struct.unpack_from("<6H", d, ao)
					for j in range(6):
						vo = ao + offs[j]
						if offs[j] and vo + 4 <= len(d) and d[vo] > 0:
							value[j] += struct.unpack_from("<h", d, vo + 2)[0] * scale[j]
			local = _mat_quat_origin(_quat(value[3:6]), value[0:3])
			mats.append(local if parent < 0 or parent >= len(mats) else _mat_concat(mats[parent], local))
		return mats

	def textures(self):
		t = self.tex
		out = []
		for i in range(t.numtextures):
			off = t.textureindex + i * TEXTURE_SZ
			name = t.data[off:off + 64].split(b"\0", 1)[0].decode("latin1", "replace")
			flags, w, h, index = struct.unpack_from("<iiii", t.data, off + 64)
			pixels = palette = b""
			if w > 0 and h > 0 and 0 < index and index + w * h + 768 <= len(t.data):
				pixels = t.data[index:index + w * h]
				palette = t.data[index + w * h:index + w * h + 768]
			out.append({"name": name, "flags": flags, "w": max(1, w), "h": max(1, h),
				"pixels": pixels, "palette": palette})
		return out

	def skinref(self, family: int):
		t = self.tex
		if t.numskinref <= 0 or t.skinindex <= 0:
			return list(range(max(1, t.numtextures)))
		fam = max(0, min(family, max(0, t.numskinfamilies - 1)))
		return list(struct.unpack_from(f"<{t.numskinref}h", t.data, t.skinindex + fam * t.numskinref * 2))

	def body_models(self, body: int):
		"""The sub-model each body part draws for a GoldSrc body value."""
		d = self.data
		out = []
		for i in range(self.numbodyparts):
			off = self.bodypartindex + i * BODYPART_SZ
			nmodels, base, modelindex = struct.unpack_from("<iii", d, off + 64)
			if nmodels <= 0:
				continue
			pick = (body // max(1, base)) % nmodels
			mo = modelindex + pick * MODEL_SZ
			(nmesh, meshindex, nverts, vertinfoindex, vertindex,
				nnorms, norminfoindex, normindex) = struct.unpack_from("<8i", d, mo + 72)
			out.append({
				"meshes": [struct.unpack_from("<iiiii", d, meshindex + k * MESH_SZ) for k in range(nmesh)],
				"verts": [struct.unpack_from("<3f", d, vertindex + k * 12) for k in range(nverts)],
				"vbone": d[vertinfoindex:vertinfoindex + nverts],
				"norms": [struct.unpack_from("<3f", d, normindex + k * 12) for k in range(nnorms)],
				"nbone": d[norminfoindex:norminfoindex + nnorms],
			})
		return out


def model_triangles(mdl: StudioModel, body: int, skin: int, sequence: int, scale: float):
	"""Posed geometry as draw commands per texture:
	{texture index: [(is_fan, [(pos, normal, s, t), ...]), ...]}, plus the textures."""
	mats = mdl.bone_matrices(sequence)
	if not mats:
		return {}, []
	textures = mdl.textures()
	sref = mdl.skinref(skin)
	d = mdl.data
	out: dict[int, list] = {}
	for sub in mdl.body_models(body):
		pos = [tuple(c * scale for c in _xform(mats[min(b, len(mats) - 1)], v))
			for v, b in zip(sub["verts"], sub["vbone"])]
		nrm = []
		for n, b in zip(sub["norms"], sub["nbone"]):
			r = _rotate(mats[min(b, len(mats) - 1)], n)
			l = math.sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) or 1.0
			nrm.append((r[0] / l, r[1] / l, r[2] / l))
		for _ntris, triindex, skinref, _nn, _u in sub["meshes"]:
			tex_i = sref[skinref] if 0 <= skinref < len(sref) else skinref
			if not 0 <= tex_i < len(textures):
				tex_i = 0
			tex = textures[tex_i] if textures else {"w": 64, "h": 64, "flags": 0}
			su, sv = 1.0 / tex["w"], 1.0 / tex["h"]
			p = triindex
			while p + 2 <= len(d):
				cmd, = struct.unpack_from("<h", d, p)
				p += 2
				if cmd == 0:
					break
				n = abs(cmd)
				verts = []
				for _ in range(n):
					vi, ni, s, t = struct.unpack_from("<hhhh", d, p)
					p += 8
					if 0 <= vi < len(pos):
						verts.append((pos[vi], nrm[ni] if 0 <= ni < len(nrm) else (0.0, 0.0, 1.0), s * su, t * sv))
				if len(verts) >= 3:
					out.setdefault(tex_i, []).append((cmd < 0, verts))
	return out, textures


# ---- BSP ----------------------------------------------------------------

def parse_entities(bsp_bytes: bytes) -> list[dict]:
	off, length = struct.unpack_from("<ii", bsp_bytes, 4)
	text = bsp_bytes[off:off + length].decode("latin1", "replace")
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


def _floats(s: str, n: int):
	try:
		vals = [float(x) for x in s.split()]
	except ValueError:
		vals = []
	return (vals + [0.0] * n)[:n]


def static_props(ents: list[dict]) -> list[dict]:
	"""The entities SV_GCTryStaticProp consumes, with the keys it reads."""
	out = []
	for e in ents:
		if e.get("classname") not in PROP_CLASSES or e.get("targetname"):
			continue
		model = e.get("model", "").replace("\\", "/")
		if not model.lower().endswith(".mdl"):
			continue
		angles = _floats(e.get("angles", "0 0 0"), 3)
		if "angle" in e and "angles" not in e:
			a = _floats(e["angle"], 1)[0]
			if a == -1.0:
				angles[0] = -90.0
			elif a == -2.0:
				angles[0] = 90.0
			else:
				angles[1] = a
		rendermode = int(_floats(e.get("rendermode", "0"), 1)[0])
		renderamt = int(_floats(e.get("renderamt", "255"), 1)[0])
		if rendermode != 0 and renderamt <= 0:
			continue	# invisible in the retail game too
		out.append({
			"model": model.lower(),
			"origin": _floats(e.get("origin", "0 0 0"), 3),
			"angles": angles,
			# The engine draws a studio entity at its model's own size
			# (R_StudioSetUpTransform passes 1.0), as GoldSrc does: "scale"
			# is a sprite key. Baking it made charlie's hedgehogs 2.5x too big.
			"scale": 1.0,
			"sequence": int(_floats(e.get("sequence", "0"), 1)[0]),
			"skin": int(_floats(e.get("skin", "0"), 1)[0]),
			"body": int(_floats(e.get("body", "0"), 1)[0]),
			"additive": rendermode == 5,
			# env_model spawnflag 2: the game drops the model onto the floor
			# at spawn (charlie's hedgehogs and wire sit 16-36 units up).
			"drop": bool(int(_floats(e.get("spawnflags", "0"), 1)[0]) & 2),
		})
	return out


def sun_direction(ents: list[dict]):
	"""Unit vector toward the light_environment sun, straight up without one."""
	for e in ents:
		if e.get("classname") != "light_environment":
			continue
		ang = _floats(e.get("angles", "0 0 0"), 3)
		pitch = _floats(e["pitch"], 1)[0] if "pitch" in e else ang[0]
		yaw = ang[1]
		if "angle" in e and "angles" not in e:
			yaw = _floats(e["angle"], 1)[0]
		p, y = math.radians(pitch), math.radians(yaw)
		# The light travels along (cos p cos y, cos p sin y, sin p); pitch is negative downward.
		d = (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p))
		return (-d[0], -d[1], -d[2]) if d[2] < 0.0 else (d[0], d[1], d[2])
	return (0.0, 0.0, 1.0)


class FloorLight:
	"""Light under a point, as the engine lights a studio model: the lightmap
	of the world face straight below it."""

	def __init__(self, bsp_path: Path):
		sys.path.insert(0, str(Path(__file__).resolve().parent))
		import bake_worlddl_goldsrc as wd  # type: ignore

		self.wd = wd
		self.bsp = wd.Bsp(bsp_path)
		self.floors = []
		first, count = 0, len(self.bsp.faces)
		if self.bsp.models:
			first, count = self.bsp.models[0][-2], self.bsp.models[0][-1]
		for fi in range(first, min(len(self.bsp.faces), first + count)):
			f = self.bsp.faces[fi]
			nx, ny, nz, dist = self.bsp.planes[f[0]]
			if f[1]:
				nx, ny, nz, dist = -nx, -ny, -nz, -dist
			if nz < 0.2 or f[6] < 0:
				continue
			ti0, ti1, _mip, flags = self.bsp.texinfos[f[4]]
			if flags & getattr(wd, "TEX_SPECIAL", 1):
				continue
			pts = self.bsp.face_pts(f)
			if len(pts) < 3:
				continue
			xs, ys = [p[0] for p in pts], [p[1] for p in pts]
			self.floors.append((min(xs), min(ys), max(xs), max(ys), (nx, ny, nz, dist), pts, f, ti0, ti1, flags))

	def solid(self, p) -> bool:
		"""Point contents of the world hull: true inside solid."""
		bsp = self.bsp
		if not bsp.nodes:
			return False
		idx = 0
		while idx >= 0:
			node = bsp.nodes[idx]
			nx, ny, nz, dist = bsp.planes[node[0]]
			idx = node[1] if nx * p[0] + ny * p[1] + nz * p[2] - dist >= 0.0 else node[2]
		leaf = -1 - idx
		return 0 <= leaf < len(bsp.leaves) and bsp.leaves[leaf][0] == -2

	def drop(self, p, reach: float = 256.0):
		"""DROP_TO_FLOOR for a point entity: the z where a trace straight
		down from p first meets solid within reach, or None when p starts in
		solid or nothing is hit (the game leaves the entity where it is)."""
		x, y, z0 = p[0], p[1], p[2]
		if self.solid((x, y, z0)):
			return None
		step, lo = 1.0, z0
		while lo - step >= z0 - reach:
			if self.solid((x, y, lo - step)):
				hi, bot = lo, lo - step
				for _ in range(12):
					mid = 0.5 * (hi + bot)
					if self.solid((x, y, mid)):
						bot = mid
					else:
						hi = mid
				return hi
			lo -= step
		return None

	def at(self, p):
		best, bz = None, -1e30
		x, y = p[0], p[1]
		for x0, y0, x1, y1, (nx, ny, nz, dist), pts, f, ti0, ti1, flags in self.floors:
			if x < x0 or x > x1 or y < y0 or y > y1:
				continue
			z = (dist - nx * x - ny * y) / nz
			if z > p[2] + 1.0 or z <= bz:
				continue
			sign = 0
			inside = True
			for k in range(len(pts)):
				a, b = pts[k], pts[(k + 1) % len(pts)]
				c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
				if abs(c) < 1e-6:
					continue
				s = 1 if c > 0 else -1
				if sign and s != sign:
					inside = False
					break
				sign = s
			if inside:
				best, bz = (f, ti0, ti1, flags, pts, (x, y, z)), z
		if best is None:
			return None
		f, ti0, ti1, flags, pts, hit = best
		return self.wd.light_at(self.bsp, f, ti0, ti1, flags, pts, hit)


# ---- textures -----------------------------------------------------------

def encode_texture(tex: dict, encode_blocks) -> tuple[int, int, int, bytes]:
	"""An 8-bit studio texture as a CMPR mip chain with power-of-two sides,
	at most TEX_MAX_SIDE. Masked textures cut out palette index 255."""
	import numpy as np

	w, h = tex["w"], tex["h"]
	masked = bool(tex["flags"] & STUDIO_NF_MASKED)
	pal = np.frombuffer(tex["palette"], np.uint8).reshape(256, 3)
	idx = np.frombuffer(tex["pixels"], np.uint8).reshape(h, w)
	rgb = pal[idx].astype(np.float32)
	cov = (idx != 255).astype(np.float32) if masked else np.ones((h, w), np.float32)
	tw = min(TEX_MAX_SIDE, 1 << max(3, (w - 1).bit_length()))
	th = min(TEX_MAX_SIDE, 1 << max(3, (h - 1).bit_length()))
	if (tw, th) != (w, h):
		# Area resample through an integer oversample, weighted by coverage so
		# a cut-out's colour does not bleed into its edge.
		ys = np.minimum(((np.arange(th * 4) + 0.5) * h / (th * 4)).astype(np.int64), h - 1)
		xs = np.minimum(((np.arange(tw * 4) + 0.5) * w / (tw * 4)).astype(np.int64), w - 1)
		big_c, big_a = rgb[ys][:, xs], cov[ys][:, xs]
		a = big_a.reshape(th, 4, tw, 4).sum((1, 3))
		c = (big_c * big_a[..., None]).reshape(th, 4, tw, 4, 3).sum((1, 3))
		plain = big_c.reshape(th, 4, tw, 4, 3).mean((1, 3))
		rgb = np.where(a[..., None] > 0, c / np.maximum(a[..., None], 1e-6), plain)
		cov = a / 16.0
	body = bytearray()
	levels = 0
	while True:
		body += encode_blocks(np.clip(rgb + 0.5, 0, 255).astype(np.uint8), cov >= 0.5)
		levels += 1
		ch, cw = cov.shape
		if min(ch, cw) <= 8:
			break
		a = cov.reshape(ch // 2, 2, cw // 2, 2).sum((1, 3))
		c = (rgb * cov[..., None]).reshape(ch // 2, 2, cw // 2, 2, 3).sum((1, 3))
		plain = rgb.reshape(ch // 2, 2, cw // 2, 2, 3).mean((1, 3))
		rgb = np.where(a[..., None] > 0, c / np.maximum(a[..., None], 1e-6), plain)
		cov = a / 4.0
	return tw, th, levels, bytes(body)


def grey_texture(encode_blocks) -> tuple[int, int, int, bytes]:
	import numpy as np

	return 8, 8, 1, encode_blocks(np.full((8, 8, 3), 150, np.uint8), np.ones((8, 8), bool))


# ---- pack ---------------------------------------------------------------

def _clamp16(v: float) -> int:
	return int(max(-32767, min(32767, round(v))))


def emit_pieces(cmds_by_mat: dict[int, list], q: float, blob: bytearray):
	"""Append a model's piece chain: per material, GX draw commands (strips
	and fans as the model stores them) of 11-byte vertices. Returns
	(offset, bytes of the first piece, triangles) or None."""
	pieces = []
	for mid in sorted(cmds_by_mat):
		body, nv, nt = bytearray(), 0, 0
		for fan, verts in cmds_by_mat[mid]:
			su = math.floor(min(v[2] for v in verts))
			sv = math.floor(min(v[3] for v in verts))
			# A strip longer than a command holds is cut on an even vertex so
			# the winding carries on; a fan restarts from its hub.
			runs = []
			if len(verts) <= GC_PR_CMD_VERTS:
				runs.append(verts)
			elif fan:
				k = 1
				while k + 1 < len(verts):
					runs.append([verts[0]] + verts[k:k + GC_PR_CMD_VERTS - 1])
					k += GC_PR_CMD_VERTS - 2
			else:
				k = 0
				while k + 2 < len(verts):
					runs.append(verts[k:k + GC_PR_CMD_VERTS])
					k += GC_PR_CMD_VERTS - 2
			for run in runs:
				cb = bytearray(struct.pack(">BH", 0xA0 if fan else 0x98, len(run)))
				for pos, nrm, s, t in run:
					cb += struct.pack(
						">hhhBhh",
						_clamp16(pos[0] * q), _clamp16(pos[1] * q), _clamp16(pos[2] * q),
						oct_index(nrm),
						_clamp16((s - su) * 256.0), _clamp16((t - sv) * 256.0),
					)
				if body and len(body) + len(cb) > GC_PR_PIECE_BYTES:
					body += b"\0" * (-len(body) % 32)
					pieces.append((mid, nv, bytes(body), nt))
					body, nv, nt = bytearray(), 0, 0
				body += cb
				nv += len(run)
				nt += len(run) - 2
		if body:
			body += b"\0" * (-len(body) % 32)
			pieces.append((mid, nv, bytes(body), nt))
	pieces = [p for p in pieces if len(p[2]) <= GC_PR_PIECE_BYTES + 32]
	if not pieces:
		return None
	ofs, total = len(blob), 0
	for n, (mid, nverts, body, ntris) in enumerate(pieces):
		nxt = (32 + len(pieces[n + 1][2])) if n + 1 < len(pieces) else 0
		blob += struct.pack(">BBHIIH", mid, 0, min(65535, nverts), len(body), nxt, min(65535, ntris)) + b"\0" * 18
		blob += body
		total += ntris
	return ofs, 32 + len(pieces[0][2]), total


def bake(bsp_path: Path, model_roots: list[Path], out_gcp: Path, out_gct: Path, encode_blocks, log=print) -> dict | None:
	"""Write the prop pack and its textures for one map. Returns counts, or
	None when the map has no static props that can be drawn."""
	raw = Path(bsp_path).read_bytes()
	ents = parse_entities(raw)
	props = static_props(ents)
	if not props:
		return None
	try:
		floor = FloorLight(bsp_path)
	except (Exception, SystemExit) as error:  # lighting is optional; geometry is not
		log(f"  props: {bsp_path.name}: no floor light ({error})")
		floor = None
	dropped = 0
	if floor is not None:
		for prop in props:
			if not prop["drop"]:
				continue
			z = floor.drop(prop["origin"])
			if z is not None:
				prop["origin"] = [prop["origin"][0], prop["origin"][1], z]
				dropped += 1
		if dropped:
			log(f"  props: {bsp_path.name}: {dropped} dropped to the floor")
	sun = sun_direction(ents)

	def find_model(rel: str) -> Path | None:
		for root in model_roots:
			cand = root / rel
			if cand.is_file():
				return cand
			parent = (root / rel).parent
			if parent.is_dir():
				want = Path(rel).name.lower()
				for p in parent.iterdir():
					if p.name.lower() == want:
						return p
		return None

	chains: list[tuple[str, int, int, int, int, bytes]] = []
	mat_of: dict[tuple, int] = {}
	grey = None
	models: dict[tuple, int | None] = {}
	model_recs = []
	model_bounds = []
	piece_blob = bytearray()
	studio_cache: dict[str, StudioModel | None] = {}
	missing: dict[str, int] = {}

	def material(rel: str, ti: int, tex: dict) -> int:
		nonlocal grey
		key = (rel, ti)
		if key in mat_of:
			return mat_of[key]
		if len(chains) >= GC_PR_MAX_MAT:
			return 0
		if tex.get("pixels") and len(tex["pixels"]) == tex["w"] * tex["h"] and len(tex["palette"]) == 768:
			tw, th, lv, body = encode_texture(tex, encode_blocks)
			flag = GC_WT_FLAG_ALPHA if tex["flags"] & STUDIO_NF_MASKED else 0
		else:
			if grey is None:
				grey = grey_texture(encode_blocks)
			tw, th, lv, body = grey
			flag = 0
		mat_of[key] = len(chains)
		chains.append((f"p{len(chains):03d}", tw, th, lv, flag, body))
		return mat_of[key]

	def model_index(prop: dict) -> int | None:
		key = (prop["model"], prop["body"], prop["skin"], prop["sequence"], round(prop["scale"], 3), prop["additive"])
		if key in models:
			return models[key]
		models[key] = None
		rel = prop["model"]
		if rel not in studio_cache:
			path = find_model(rel)
			try:
				studio_cache[rel] = StudioModel(path) if path else None
			except (ValueError, struct.error, OSError) as error:
				log(f"  props: {rel}: {error}")
				studio_cache[rel] = None
		mdl = studio_cache[rel]
		if mdl is None:
			return None
		if len(model_recs) >= GC_PR_MAX_MODELS:
			return None
		try:
			cmds, textures = model_triangles(mdl, prop["body"], prop["skin"], prop["sequence"], prop["scale"])
		except (struct.error, IndexError) as error:
			log(f"  props: {rel}: {error}")
			return None
		by_mat: dict[int, list] = {}
		ext = 1.0
		lo, hi = [1e30] * 3, [-1e30] * 3
		for ti, runs in cmds.items():
			tex = textures[ti] if ti < len(textures) else {"flags": 0, "w": 8, "h": 8, "pixels": b"", "palette": b""}
			if tex["flags"] & STUDIO_NF_CHROME:
				continue	# chrome wants view-dependent coordinates
			mid = material(rel, ti, tex)
			if prop["additive"] or tex["flags"] & STUDIO_NF_ADDITIVE:
				mid |= 0x80
			by_mat.setdefault(mid, []).extend(runs)
			for _fan, verts in runs:
				for pos, _n, _s, _t in verts:
					for a in range(3):
						lo[a] = min(lo[a], pos[a])
						hi[a] = max(hi[a], pos[a])
						ext = max(ext, abs(pos[a]))
		if not by_mat:
			return None
		frac = 0
		while frac < 6 and ext * (1 << (frac + 1)) <= 32000.0:
			frac += 1
		got = emit_pieces(by_mat, float(1 << frac), piece_blob)
		if got is None:
			return None
		models[key] = len(model_recs)
		model_recs.append((got[0], got[1], got[2], frac))
		model_bounds.append((lo, hi))
		return models[key]

	inst_blob = bytearray()
	light_blob = bytearray()
	n_inst = placed_tris = 0
	a16 = lambda deg: ((int(round(deg * 65536.0 / 360.0)) + 32768) & 0xFFFF) - 32768
	for prop in props:
		if n_inst >= GC_PR_MAX_INST:
			log(f"  props: {bsp_path.name}: more than {GC_PR_MAX_INST} instances, rest dropped")
			break
		mi = model_index(prop)
		if mi is None:
			missing[prop["model"]] = missing.get(prop["model"], 0) + 1
			continue
		# A studio entity is drawn with its pitch negated (R_StudioSetUpTransform).
		pitch, yaw, roll = -prop["angles"][0], prop["angles"][1], prop["angles"][2]
		m = angle_matrix(pitch, yaw, roll)
		lo, hi = model_bounds[mi]
		mins, maxs = [1e30] * 3, [-1e30] * 3
		for cx in (lo[0], hi[0]):
			for cy in (lo[1], hi[1]):
				for cz in (lo[2], hi[2]):
					w = _rotate(m, (cx, cy, cz))
					for a in range(3):
						mins[a] = min(mins[a], w[a] + prop["origin"][a])
						maxs[a] = max(maxs[a], w[a] + prop["origin"][a])
		# GoldSrc lights a studio model from the floor under it: an ambient
		# part on every face and the rest by N.L.
		c = None
		if floor is not None:
			c = floor.at((prop["origin"][0], prop["origin"][1], prop["origin"][2] + 8.0))
			if c is None:
				c = floor.at((0.5 * (mins[0] + maxs[0]), 0.5 * (mins[1] + maxs[1]), maxs[2]))
		if c is None:
			c = (150, 150, 150)
		# Props read darker than the lightmapped walls beside them at 0.6 +
		# 0.4 (Dolphin, charlie beach): the shaded side keeps 0.75 and the
		# lit side may reach 1.2, which GX clamps per channel.
		c = [max(40, v) for v in c]
		base = [int(v * 0.75) for v in c]
		direct = [min(255, int(v * 0.45)) for v in c]
		light_blob += bytes(base + [0, 0, 0] + direct + [0, 0, 0])
		inst_blob += struct.pack(
			">HH3h3h3h3hI",
			mi, min(65535, model_recs[mi][2]),
			*[_clamp16(math.floor(v)) for v in mins], *[_clamp16(math.ceil(v)) for v in maxs],
			*[_clamp16(v) for v in prop["origin"]],
			a16(pitch), a16(yaw), a16(roll),
			(c[0] << 24) | (c[1] << 16) | (c[2] << 8) | 1,
		)
		n_inst += 1
		placed_tris += model_recs[mi][2]
	if not n_inst:
		for rel, n in sorted(missing.items()):
			log(f"  props: {bsp_path.name}: no model file for {rel} ({n} placed)")
		return None

	model_blob = bytearray()
	for dl_ofs, first_bytes, tris, frac in model_recs:
		# No far level: a far instance draws the full chain (the engine
		# keeps it resident), so the far fields stay empty.
		model_blob += struct.pack(">IIHBBIIHH", dl_ofs, first_bytes, min(65535, tris), frac, 0, 0, 0, 0, 0)
	model_blob += b"\0" * (-len(model_blob) % 32)
	inst_blob += struct.pack(">3h", *[int(round(max(-1.0, min(1.0, v)) * 32767.0)) for v in sun]).ljust(32, b"\0")
	inst_blob += light_blob
	inst_blob += b"\0" * (-len(inst_blob) % 32)
	inst_ofs = 32
	models_ofs = inst_ofs + len(inst_blob)
	dl_base = models_ofs + len(model_blob)
	hdr = struct.pack(
		">4sHHHHIIII",
		GC_PR_MAGIC, GC_PR_VERSION, n_inst, len(model_recs), 0,
		inst_ofs, models_ofs, dl_base, 0xFFFFFFFF,
	)
	hdr += struct.pack(">I", dl_base + len(piece_blob))	# far region: empty, at the end
	hdr = hdr.ljust(32, b"\0")
	out_gcp.parent.mkdir(parents=True, exist_ok=True)
	out_gcp.write_bytes(hdr + inst_blob + model_blob + piece_blob)

	entries, blob = [], bytearray()
	for name, tw, th, lv, flag, body in chains:
		entries.append(GC_WT_ENTRY.pack(name.encode("latin-1"), tw, th, lv, flag, 0, len(blob)))
		blob += body
		blob += bytes(-len(blob) & 31)
	head = b"GCWT" + struct.pack(">II", len(entries), len(blob)) + b"".join(entries)
	out_gct.write_bytes(head + bytes(-len(head) & 31) + bytes(blob))

	for rel, n in sorted(missing.items()):
		log(f"  props: {bsp_path.name}: no model file for {rel} ({n} placed)")
	stats = {
		"instances": n_inst, "models": len(model_recs), "materials": len(chains),
		"model_tris": sum(r[2] for r in model_recs), "placed_tris": placed_tris,
		"missing": sum(missing.values()), "chain_bytes": len(piece_blob), "tex_bytes": len(blob),
	}
	log(
		f"  props: {out_gcp.name} instances={n_inst} models={len(model_recs)} "
		f"tris={stats['model_tris']} (placed {placed_tris}) chains={len(piece_blob)} B, "
		f"{out_gct.name} materials={len(chains)} {len(blob)} B"
		+ (f", {stats['missing']} placements without a model file" if missing else "")
	)
	return stats


def main() -> int:
	import argparse
	import importlib.util

	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("--map", type=Path, required=True)
	ap.add_argument("--src", type=Path, action="append", required=True, help="gamedir root with models/ (repeatable)")
	ap.add_argument("--out", type=Path, required=True, help="output directory (media/)")
	args = ap.parse_args()
	spec = importlib.util.spec_from_file_location("build_gamecube_disc", Path(__file__).with_name("build-gamecube-disc.py"))
	disc = importlib.util.module_from_spec(spec)
	sys.modules["build_gamecube_disc"] = disc
	spec.loader.exec_module(disc)
	stem = args.map.stem.lower()
	got = bake(args.map, args.src, args.out / f"gp_{stem}.gcp", args.out / f"pt_{stem}.gct", disc.gx_cmpr_encode_blocks)
	return 0 if got else 1


if __name__ == "__main__":
	raise SystemExit(main())
