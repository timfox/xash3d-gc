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
GC_PR_MAX_INST_BIG = 4096  # a converted map's pack: the engine moves its tables to the map arena
GC_PR_FIXED_INST = 512  # the engine's fixed tables; past them the pack is written nearest a player start first
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

	def sequence_index(self, name: str) -> int:
		"""LookupSequence: the index of a sequence by its label, or -1."""
		want = name.strip().lower()
		for i in range(self.numseq):
			so = self.seqindex + i * SEQ_SZ
			label = self.data[so:so + 32].split(b"\0", 1)[0].decode("latin1", "replace")
			if label.lower() == want:
				return i
		return -1

	def bone_matrices(self, sequence: int):
		"""World matrices of the bones at frame 0 of a sequence (the bind
		pose when the sequence is not in this file)."""
		d = self.data
		anim = None
		motiontype, motionbone = 0, -1
		if 0 <= sequence < self.numseq:
			so = self.seqindex + sequence * SEQ_SZ
			# The engine holds the motion bone at zero on the axes the
			# sequence moves along (R_StudioCalcRotations): that travel is
			# the entity's, not the mesh's.
			motiontype, motionbone = struct.unpack_from("<ii", d, so + 68)
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
			if b == motionbone:
				for j in range(3):
					if motiontype & (1 << j):
						value[j] = 0.0
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
	named_ord = -1
	# A dod_object whose object_group a dod_capture_area asks for is a satchel a player carries: the game spawns it as an
	# entity (SV_GCFunctionalObject, sv_game.c), so it is not part of the prop pack.
	area_groups = {e.get("area_object_group") for e in ents
		if e.get("classname") == "dod_capture_area" and e.get("area_object_group")}
	for ent_index, e in enumerate(ents):
		if e.get("classname") not in PROP_CLASSES:
			continue
		if e.get("classname") == "dod_object" and e.get("object_group") in area_groups:
			continue
		model = e.get("model", "").replace("\\", "/")
		if not model.lower().endswith(".mdl"):
			continue
		# Named props keep their edict (Use, env_render, killtarget) and the
		# pack draws them while that edict is drawable.  The ordinal counts
		# every named prop in lump order, as the server counts its edicts
		# (SV_GCIndexNamedProps, sv_game.c).
		ordinal = 0
		if e.get("targetname"):
			named_ord += 1
			ordinal = named_ord + 1
			if ordinal > 255:
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
		spawnflags = int(_floats(e.get("spawnflags", "0"), 1)[0])
		if rendermode != 0 and renderamt <= 0 and not ordinal:
			continue	# invisible in the retail game too (env_render may show a named one later)
		out.append({
			"model": model.lower(),
			"origin": _floats(e.get("origin", "0 0 0"), 3),
			"angles": angles,
			# The engine draws a studio entity at its model's own size
			# (R_StudioSetUpTransform passes 1.0), as GoldSrc does: "scale"
			# is a sprite key. Baking it made charlie's hedgehogs 2.5x too big.
			"scale": 1.0,
			"sequence": int(_floats(e.get("sequence", "0"), 1)[0]),
			# env_model names its sequence: m_iszSequence_On, or _Off when
			# spawnflag 1 starts it off (the retail class follows Spirit's
			# env_model). ouitz_tree1 has one sequence per origin height
			# ("tree_high_origin"), so sequence 0 left those trees in the air.
			"sequence_name": (e.get("m_iszSequence_Off", "") if spawnflags & 1
				else e.get("m_iszSequence_On", "")) if e.get("classname") == "env_model" else "",
			"skin": int(_floats(e.get("skin", "0"), 1)[0]),
			"body": int(_floats(e.get("body", "0"), 1)[0]),
			# A named prop's rendermode 5 without an amount is its hidden state.
			"additive": rendermode == 5 and not ordinal,
			# env_model spawnflag 2: the game drops the model onto the floor
			# at spawn (charlie's hedgehogs and wire sit 16-36 units up).
			"drop": bool(spawnflags & 2) and e.get("classname") == "env_model",
			# dod_object is the item a player picks up: it never calls the
			# floor drop, it falls like any tossed item once the map runs
			# (every unnamed one on charlie and caen hung 5 to 32 units up).
			"toss": e.get("classname") == "dod_object",
			# Lump position: the game spawns entities in this order, and a
			# prop dropping to the floor only meets what already spawned.
			"ent": ent_index,
			"named": ordinal,
		})
	return out


def prop_sequence(mdl, prop: dict) -> int:
	"""The sequence the game poses a prop in: the named one when the model
	has it (CEnvModel::SetSequence falls back to 0), else the number."""
	name = prop.get("sequence_name", "")
	if name:
		return max(0, mdl.sequence_index(name))
	return prop["sequence"]


def drop_like_the_game(floor, prop: dict):
	"""CEnvModel::Spawn with spawnflag 2: lift the origin one unit, then
	DROP_TO_FLOOR. A prop resting less than a unit inside the floor is lifted
	out and lands on it; when the drop finds nothing, or still starts in
	solid, the lift stays. Moves prop["origin"]; returns the floor z or None."""
	x, y, z = prop["origin"][0], prop["origin"][1], prop["origin"][2] + 1.0
	hit = floor.drop((x, y, z), before=prop.get("ent"))
	prop["origin"] = [x, y, z if hit is None else hit]
	return hit


def fall_like_the_game(floor, prop: dict):
	"""A tossed item (dod_object) falls until it meets solid. That happens
	in play, after every entity spawned, so all solid brush entities count."""
	x, y, z = prop["origin"]
	hit = floor.drop((x, y, z), reach=2048.0, before=None)
	if hit is not None:
		prop["origin"] = [x, y, hit]
	return hit


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
		# Hull-0 roots a point trace tests: the world, then solid brush
		# entities as (head node, origin offset, mins, maxs).
		self.heads = [(0, (0.0, 0.0, 0.0), None, None, -1)] if self.bsp.nodes else []
		first, count = 0, len(self.bsp.faces)
		if self.bsp.models:
			first, count = self.bsp.models[0][-2], self.bsp.models[0][-1]
		self._add_floors(first, count)

	# Brush entities a dropped prop passes through in the game.
	NOT_SOLID = frozenset((
		"func_illusionary", "func_water", "func_ladder", "func_friction",
		"func_monsterclip", "func_mortar_field", "func_bomb_target",
		"func_buyzone", "func_points_brush", "func_detail_illusionary",
	))

	def add_brush_entities(self, ents: list[dict]) -> int:
		"""Solid brush entities (func_wall and the like) are ground too, for
		the props that spawn after them. dod_charlie's invisible func_wall
		over the beach comes after its hedgehogs in the entity lump, so they
		fall through to the world sand under it."""
		added = 0
		for ent_index, e in enumerate(ents):
			model = e.get("model", "")
			cls = e.get("classname", "")
			if not model.startswith("*") or not cls.startswith("func_") or cls in self.NOT_SOLID:
				continue
			try:
				mi = int(model[1:])
			except ValueError:
				continue
			if mi <= 0 or mi >= len(self.bsp.models):
				continue
			vals = self.bsp.models[mi]
			org = tuple(_floats(e.get("origin", "0 0 0"), 3))
			self.heads.append((vals[9], org, vals[0:3], vals[3:6], ent_index))
			if org == (0.0, 0.0, 0.0):
				self._add_floors(vals[-2], vals[-1])
			added += 1
		return added

	def _add_floors(self, first: int, count: int) -> None:
		for fi in range(first, min(len(self.bsp.faces), first + count)):
			f = self.bsp.faces[fi]
			nx, ny, nz, dist = self.bsp.planes[f[0]]
			if f[1]:
				nx, ny, nz, dist = -nx, -ny, -nz, -dist
			if nz < 0.2 or f[6] < 0:
				continue
			ti0, ti1, _mip, flags = self.bsp.texinfos[f[4]]
			if flags & getattr(self.wd, "TEX_SPECIAL", 1):
				continue
			pts = self.bsp.face_pts(f)
			if len(pts) < 3:
				continue
			xs, ys = [p[0] for p in pts], [p[1] for p in pts]
			self.floors.append((min(xs), min(ys), max(xs), max(ys), (nx, ny, nz, dist), pts, f, ti0, ti1, flags))

	def solid(self, p, before=None) -> bool:
		"""Point contents of the world hull: true inside solid. With before,
		only brush entities that spawn earlier in the entity lump count."""
		bsp = self.bsp
		for head, org, mins, maxs, owner in getattr(self, "heads", [(0, (0.0, 0.0, 0.0), None, None, -1)]):
			if before is not None and owner >= before:
				continue
			q = (p[0] - org[0], p[1] - org[1], p[2] - org[2])
			if mins is not None and any(q[a] < mins[a] - 1.0 or q[a] > maxs[a] + 1.0 for a in range(3)):
				continue
			idx = head
			if idx >= len(bsp.nodes):
				continue
			while idx >= 0:
				node = bsp.nodes[idx]
				nx, ny, nz, dist = bsp.planes[node[0]]
				idx = node[1] if nx * q[0] + ny * q[1] + nz * q[2] - dist >= 0.0 else node[2]
			leaf = -1 - idx
			if 0 <= leaf < len(bsp.leaves) and bsp.leaves[leaf][0] == -2:
				return True
		return False

	def drop(self, p, reach: float = 256.0, before=None):
		"""DROP_TO_FLOOR for a point entity: the z where a trace straight
		down from p first meets solid within reach, or None when p starts in
		solid or nothing is hit (the game leaves the entity where it is)."""
		x, y, z0 = p[0], p[1], p[2]
		if self.solid((x, y, z0), before):
			return None
		step, lo = 1.0, z0
		while lo - step >= z0 - reach:
			if self.solid((x, y, lo - step), before):
				hi, bot = lo, lo - step
				for _ in range(12):
					mid = 0.5 * (hi + bot)
					if self.solid((x, y, mid), before):
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

	def footprint(self, p, radius: float):
		"""The floor light under a prop's footprint: the mean of the brighter
		half of a 5x5 grid of samples. One sample under the origin lands on
		a single lightmap texel, and under a tree trunk that texel is often
		a dark patch (dod_caen: 17 where the ground 30 units away is 140),
		which drew the whole tree black."""
		got = []
		for iy in range(-2, 3):
			for ix in range(-2, 3):
				c = self.at((p[0] + ix * radius * 0.5, p[1] + iy * radius * 0.5, p[2]))
				if c is not None:
					got.append(c)
		if not got:
			return None
		got.sort(key=lambda c: c[0] * 30 + c[1] * 59 + c[2] * 11, reverse=True)
		top = got[:max(1, (len(got) + 1) // 2)]
		return tuple(int(round(sum(c[a] for c in top) / len(top))) for a in range(3))


# ---- textures -----------------------------------------------------------

def encode_texture(tex: dict, encode_blocks) -> tuple[int, int, int, bytes]:
	"""An 8-bit studio texture as a CMPR mip chain with power-of-two sides,
	at most TEX_MAX_SIDE. Masked textures cut out palette index 255."""
	import numpy as np

	w, h = tex["w"], tex["h"]
	masked = bool(tex["flags"] & STUDIO_NF_MASKED)
	pal = np.frombuffer(tex["palette"], np.uint8).reshape(256, 3)
	if not masked and tex.get("texgamma", True):
		# GoldSrc's texture gamma, as the stock loader applies to an
		# unmasked studio skin (imagelib LUMP_TEXGAMMA, texgamma 2.0 over
		# gamma 2.5).
		pal = np.minimum(255, (np.power(pal / 255.0, 0.8) * 255.0).astype(np.int64)).astype(np.uint8)
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


def _a16(deg: float) -> int:
	return ((int(round(deg * 65536.0 / 360.0)) + 32768) & 0xFFFF) - 32768


def instance_record(floor, mi: int, tris: int, mins, maxs, origin, angles, named: int = 0) -> tuple[bytes, bytes]:
	"""One 32-byte instance and its 12-byte light, as R_GXPropLoad reads them.
	angles are the draw angles (pitch already as the renderer wants it)."""
	# GoldSrc lights a studio model from the floor under it: an ambient
	# part on every face and the rest by N.L.
	c = None
	if floor is not None:
		# Half the footprint's shorter side, between 16 and 48 units.
		reach = max(16.0, min(48.0, 0.25 * min(maxs[0] - mins[0], maxs[1] - mins[1])))
		# The floor under the origin. Only when there is none there, or
		# only a face with no light at all, try the middle and then the
		# top of the bounds: dod_caen's far King Tiger stands over
		# nothing but its own unlit collision brush. Taking the
		# brightest of the three instead lit indoor sandbags from the
		# sunlit top of their collision brush.
		mid = (0.5 * (mins[0] + maxs[0]), 0.5 * (mins[1] + maxs[1]), 0.5 * (mins[2] + maxs[2]))
		for at in (
			(origin[0], origin[1], origin[2] + 8.0),
			mid,
			(mid[0], mid[1], maxs[2]),
		):
			g = floor.footprint(at, reach)
			if g is not None and max(g) >= 4:
				c = g
				break
	if c is None:
		c = (150, 150, 150)
	# Props read darker than the lightmapped walls beside them at 0.6 +
	# 0.4 (Dolphin, charlie beach): the shaded side keeps 0.75 and the
	# lit side may reach 1.2, which GX clamps per channel.
	c = [max(40, v) for v in c]
	base = [int(v * 0.75) for v in c]
	direct = [min(255, int(v * 0.45)) for v in c]
	light = bytes(base + [0, 0, 0] + direct + [0, 0, 0])
	rec = struct.pack(
		">HH3h3h3h3hI",
		mi | (named << 8), min(65535, tris),
		*[_clamp16(math.floor(v)) for v in mins], *[_clamp16(math.ceil(v)) for v in maxs],
		*[_clamp16(v) for v in origin],
		_a16(angles[0]), _a16(angles[1]), _a16(angles[2]),
		(c[0] << 24) | (c[1] << 16) | (c[2] << 8) | 1,
	)
	return rec, light


def write_pack(out_gcp: Path, out_gct: Path, model_recs, chains, inst_blob: bytearray, light_blob: bytearray,
		sun, n_inst: int, piece_blob: bytearray) -> bytearray:
	"""Write media/gp_<map>.gcp and media/pt_<map>.gct. Returns the texture blob."""
	inst_blob = bytearray(inst_blob)
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

	return blob


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
		floor.add_brush_entities(ents)
		for prop in props:
			if prop.get("toss"):
				if fall_like_the_game(floor, prop) is not None:
					dropped += 1
				continue
			if not prop["drop"]:
				continue
			z = drop_like_the_game(floor, prop)
			if z is not None:
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
		key = (prop["model"], prop["body"], prop["skin"], prop["sequence"], prop.get("sequence_name", "").lower(),
			round(prop["scale"], 3), prop["additive"])
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
			cmds, textures = model_triangles(mdl, prop["body"], prop["skin"], prop_sequence(mdl, prop), prop["scale"])
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
		rec, light = instance_record(floor, mi, model_recs[mi][2], mins, maxs, prop["origin"], (pitch, yaw, roll),
			prop.get("named", 0))
		inst_blob += rec
		light_blob += light
		n_inst += 1
		placed_tris += model_recs[mi][2]
	if not n_inst:
		for rel, n in sorted(missing.items()):
			log(f"  props: {bsp_path.name}: no model file for {rel} ({n} placed)")
		return None

	blob = write_pack(out_gcp, out_gct, model_recs, chains, inst_blob, light_blob, sun, n_inst, piece_blob)

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


# ---- MOHAA static models (.mprop from timfox/mohaa-gc tools/props_models.py) ----

MPROP_MAGIC = b"MPRP"
# A prop's scale is baked into its model (an instance has no scale), so
# instances share a model per 5 % step of scale.
MPROP_SCALE_STEP = 1.05


def read_mprop(data: bytes) -> dict:
	"""{"textures": [{name, w, h, flags, palette (RGB 768), pixels}], "models": [{name, mins, maxs,
	surfaces: [(texture, verts [(pos, normal, s, t)], tris)]}], "instances": [(model, origin, angles, scale)]}."""
	magic, ver, ntex, nmod, ninst = struct.unpack_from("<4s4i", data, 0)
	if magic != MPROP_MAGIC or ver != 2:
		raise ValueError(f"not an MPRP v2 pack (got {magic!r} v{ver})")
	at = 20
	textures, models, instances = [], [], []
	for _ in range(ntex):
		name = data[at:at + 32].split(b"\0", 1)[0].decode("latin-1")
		w, h, flags = struct.unpack_from("<2HI", data, at + 32)
		rgba = data[at + 40:at + 40 + 1024]
		pixels = data[at + 1064:at + 1064 + w * h]
		pal = b"".join(rgba[i * 4:i * 4 + 3] for i in range(256))
		textures.append({"name": name, "w": w, "h": h, "flags": STUDIO_NF_MASKED if flags & 1 else 0,
			"palette": pal, "pixels": pixels, "texgamma": False})
		at += 1064 + w * h
	for _ in range(nmod):
		name = data[at:at + 64].split(b"\0", 1)[0].decode("latin-1")
		vals = struct.unpack_from("<6fi", data, at + 64)
		at += 64 + 28
		surfs = []
		for _ in range(vals[6]):
			ti, nv, nt = struct.unpack_from("<3i", data, at)
			at += 12
			verts = []
			for k in range(nv):
				x, y, z, nx, ny, nz, s_, t_ = struct.unpack_from("<8f", data, at + 32 * k)
				verts.append(((x, y, z), (nx, ny, nz), s_, t_))
			at += 32 * nv
			tris = [struct.unpack_from("<3H", data, at + 6 * k) for k in range(nt)]
			at += 6 * nt
			surfs.append((ti, verts, tris))
		models.append({"name": name, "mins": vals[0:3], "maxs": vals[3:6], "surfaces": surfs})
	for _ in range(ninst):
		mi, *f = struct.unpack_from("<i7f", data, at)
		instances.append((mi, tuple(f[0:3]), tuple(f[3:6]), f[6]))
		at += 32
	if at != len(data):
		raise ValueError(f"MPRP pack has {len(data) - at} bytes past its end")
	return {"textures": textures, "models": models, "instances": instances}


def bake_mprop(mprop_path: Path, out_gcp: Path, out_gct: Path, encode_blocks, bsp_path: Path | None = None,
		log=print) -> dict | None:
	"""The prop pack for a converted MOHAA map, from its .mprop. With the
	converted BSP the props are lit from the floor under them and, past the
	engine's instance limit, the ones nearest a player start are kept."""
	pack = read_mprop(Path(mprop_path).read_bytes())
	ents = parse_entities(Path(bsp_path).read_bytes()) if bsp_path else []
	floor = None
	if bsp_path:
		try:
			floor = FloorLight(bsp_path)
			floor.add_brush_entities(ents)
		except (Exception, SystemExit) as error:  # lighting is optional; geometry is not
			log(f"  props: {Path(bsp_path).name}: no floor light ({error})")
			floor = None
	sun = sun_direction(ents)
	insts = list(pack["instances"])
	dropped = 0
	if len(insts) > GC_PR_FIXED_INST:
		starts = [_floats(e.get("origin", "0 0 0"), 3) for e in ents
			if e.get("classname", "").startswith("info_player")]
		if starts:  # if the engine can't grow its tables it keeps the head
			insts.sort(key=lambda i: min(sum((i[1][a] - s_[a]) ** 2 for a in range(3)) for s_ in starts))
		if len(insts) > GC_PR_MAX_INST_BIG:
			dropped = len(insts) - GC_PR_MAX_INST_BIG
			insts = insts[:GC_PR_MAX_INST_BIG]
			log(f"  props: {Path(mprop_path).name}: {dropped} of {len(pack['instances'])} instances past the engine's "
				f"{GC_PR_MAX_INST_BIG} left out" + (" (kept the ones nearest a player start)" if starts else ""))

	chains, mat_of = [], {}

	def material(ti: int) -> int:
		if ti in mat_of:
			return mat_of[ti]
		if len(chains) >= GC_PR_MAX_MAT:
			return 0
		if 0 <= ti < len(pack["textures"]):
			tex = pack["textures"][ti]
			tw, th, lv, body = encode_texture(tex, encode_blocks)
			flag = GC_WT_FLAG_ALPHA if tex["flags"] & STUDIO_NF_MASKED else 0
		else:
			tw, th, lv, body = grey_texture(encode_blocks)
			flag = 0
		mat_of[ti] = len(chains)
		chains.append((f"p{len(chains):03d}", tw, th, lv, flag, body))
		return mat_of[ti]

	models, model_recs, model_bounds, steps_of = {}, [], [], {}
	piece_blob = bytearray()

	def model_index(mi: int, scale: float) -> int | None:
		step = int(round(math.log(max(scale, 1e-4)) / math.log(MPROP_SCALE_STEP)))
		if (mi, step) not in models and len(model_recs) >= GC_PR_MAX_MODELS and steps_of.get(mi):
			step = min(steps_of[mi], key=lambda k: abs(k - step))	# out of models: the nearest scale baked
		key = (mi, step)
		if key in models:
			return models[key]
		models[key] = None
		if len(model_recs) >= GC_PR_MAX_MODELS or not 0 <= mi < len(pack["models"]):
			return None
		q_scale = MPROP_SCALE_STEP ** step
		by_mat, ext = {}, 1.0
		lo, hi = [1e30] * 3, [-1e30] * 3
		for ti, verts, tris in pack["models"][mi]["surfaces"]:
			mid = material(ti)
			runs = by_mat.setdefault(mid, [])
			for tri in tris:
				vs = []
				for k in tri:
					pos, nrm, s_, t_ = verts[k]
					p = (pos[0] * q_scale, pos[1] * q_scale, pos[2] * q_scale)
					vs.append((p, nrm, s_, t_))
					for a in range(3):
						lo[a] = min(lo[a], p[a])
						hi[a] = max(hi[a], p[a])
						ext = max(ext, abs(p[a]))
				runs.append((False, vs))	# a 3-vertex strip is one triangle
		if not any(by_mat.values()):
			return None
		frac = 0
		while frac < 6 and ext * (1 << (frac + 1)) <= 32000.0:
			frac += 1
		got = emit_pieces({k: v for k, v in by_mat.items() if v}, float(1 << frac), piece_blob)
		if got is None:
			return None
		models[key] = len(model_recs)
		steps_of.setdefault(mi, []).append(step)
		model_recs.append((got[0], got[1], got[2], frac))
		model_bounds.append((lo, hi))
		return models[key]

	inst_blob, light_blob = bytearray(), bytearray()
	n_inst = placed_tris = missing = 0
	for mi, origin, angles, scale in insts:
		gi = model_index(mi, scale)
		if gi is None:
			missing += 1
			continue
		# Drawn like a studio model at these entity angles: pitch negated.
		pitch, yaw, roll = -angles[0], angles[1], angles[2]
		m = angle_matrix(pitch, yaw, roll)
		lo, hi = model_bounds[gi]
		mins, maxs = [1e30] * 3, [-1e30] * 3
		for cx in (lo[0], hi[0]):
			for cy in (lo[1], hi[1]):
				for cz in (lo[2], hi[2]):
					w = _rotate(m, (cx, cy, cz))
					for a in range(3):
						mins[a] = min(mins[a], w[a] + origin[a])
						maxs[a] = max(maxs[a], w[a] + origin[a])
		rec, light = instance_record(floor, gi, model_recs[gi][2], mins, maxs, origin, (pitch, yaw, roll))
		inst_blob += rec
		light_blob += light
		n_inst += 1
		placed_tris += model_recs[gi][2]
	if not n_inst:
		return None
	blob = write_pack(out_gcp, out_gct, model_recs, chains, inst_blob, light_blob, sun, n_inst, piece_blob)
	stats = {
		"instances": n_inst, "models": len(model_recs), "materials": len(chains),
		"model_tris": sum(r[2] for r in model_recs), "placed_tris": placed_tris,
		"missing": missing, "dropped": dropped, "chain_bytes": len(piece_blob), "tex_bytes": len(blob),
	}
	log(f"  props: {out_gcp.name} instances={n_inst} models={len(model_recs)} tris={stats['model_tris']} "
		f"(placed {placed_tris}) chains={len(piece_blob)} B, {out_gct.name} materials={len(chains)} {len(blob)} B")
	return stats


def main() -> int:
	import argparse
	import importlib.util

	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("--map", type=Path, required=True, help="the map's BSP (with --mprop: the converted MOHAA map, for light)")
	ap.add_argument("--src", type=Path, action="append", help="gamedir root with models/ (repeatable)")
	ap.add_argument("--mprop", type=Path, help="a converted MOHAA map's static props (mohaa-gc tools/props_models.py)")
	ap.add_argument("--out", type=Path, required=True, help="output directory (media/)")
	args = ap.parse_args()
	spec = importlib.util.spec_from_file_location("build_gamecube_disc", Path(__file__).with_name("build-gamecube-disc.py"))
	disc = importlib.util.module_from_spec(spec)
	sys.modules["build_gamecube_disc"] = disc
	spec.loader.exec_module(disc)
	stem = args.map.stem.lower()
	if args.mprop:
		got = bake_mprop(args.mprop, args.out / f"gp_{stem}.gcp", args.out / f"pt_{stem}.gct", disc.gx_cmpr_encode_blocks,
			args.map if args.map.is_file() else None)
		return 0 if got else 1
	if not args.src:
		ap.error("--src is needed without --mprop")
	got = bake(args.map, args.src, args.out / f"gp_{stem}.gcp", args.out / f"pt_{stem}.gct", disc.gx_cmpr_encode_blocks)
	return 0 if got else 1


if __name__ == "__main__":
	raise SystemExit(main())
