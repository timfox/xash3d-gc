"""scripts/bake_props_goldsrc.py: a map's static studio props as a GX prop
pack (media/gp_<map>.gcp + media/pt_<map>.gct), read by ref/gx/r_gx_world.c."""
import importlib.util
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, relative):
	spec = importlib.util.spec_from_file_location(name, ROOT / relative)
	module = importlib.util.module_from_spec(spec)
	sys.modules[name] = module
	spec.loader.exec_module(module)
	return module


def make_mdl(masked=False):
	"""One bone, one body part, one mesh: a quad as a 4-vertex strip, a
	16x16 texture."""
	hdr = bytearray(244)
	hdr[0:4] = b"IDST"
	struct.pack_into("<i", hdr, 4, 10)
	out = bytearray(hdr)

	def at():
		while len(out) % 4:
			out.append(0)
		return len(out)

	bone = at()
	b = bytearray(112)
	b[0:4] = b"root"
	struct.pack_into("<i", b, 32, -1)
	struct.pack_into("<6f", b, 64, 0, 0, 10, 0, 0, 0)	# lifted 10 units
	struct.pack_into("<6f", b, 88, 1, 1, 1, 1, 1, 1)
	out += b
	tex = at()
	t = bytearray(80)
	t[0:8] = b"skin.bmp"
	pix_at = tex + 80
	struct.pack_into("<iiii", t, 64, 0x40 if masked else 0, 16, 16, pix_at)
	out += t
	out += bytes([255 if (i % 16) < 4 else 1 for i in range(256)])
	out += bytes([200, 100, 50] * 256)
	skin = at()
	out += struct.pack("<h", 0)
	verts = at()
	for v in ((0, 0, 0), (32, 0, 0), (0, 32, 0), (32, 32, 0)):
		out += struct.pack("<3f", *v)
	vinfo = at()
	out += bytes(4)
	norms = at()
	out += struct.pack("<3f", 0, 0, 1)
	ninfo = at()
	out += bytes(1)
	tris = at()
	out += struct.pack("<h", 4)
	for vi, (s, tt) in enumerate(((0, 0), (16, 0), (0, 16), (16, 16))):
		out += struct.pack("<hhhh", vi, 0, s, tt)
	out += struct.pack("<h", 0)
	mesh = at()
	out += struct.pack("<iiiii", 2, tris, 0, 1, 0)
	model = at()
	m = bytearray(112)
	struct.pack_into("<8i", m, 72, 1, mesh, 4, vinfo, verts, 1, ninfo, norms)
	out += m
	body = at()
	bp = bytearray(76)
	struct.pack_into("<iii", bp, 64, 1, 1, model)
	out += bp
	struct.pack_into("<19i", out, 136, 0, 1, bone, 0, 0, 0, 0, 0, 0, 0, 0, 1, tex, 0, 1, 1, skin, 1, body)
	return bytes(out)


def make_bsp(entities: str):
	text = entities.encode("latin1") + b"\0"
	head = bytearray(4 + 15 * 8)
	struct.pack_into("<i", head, 0, 30)
	struct.pack_into("<ii", head, 4, len(head), len(text))
	return bytes(head) + text


ENTS = """{
"classname" "worldspawn"
}
{
"classname" "env_model"
"model" "models/mapmodels/hedgehog.mdl"
"origin" "100 200 0"
"angles" "0 90 0"
}
{
"classname" "dod_object"
"model" "models/mapmodels/hedgehog.mdl"
"origin" "-300 50 16"
}
{
"classname" "env_model"
"targetname" "switched"
"model" "models/mapmodels/hedgehog.mdl"
"origin" "0 0 0"
}
{
"classname" "env_model"
"model" "models/mapmodels/not_on_disc.mdl"
"origin" "0 0 0"
}
"""


class PropPackTest(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.baker = load("bake_props_goldsrc", "scripts/bake_props_goldsrc.py")
		cls.disc = load("build_gamecube_disc_props", "scripts/build-gamecube-disc.py")

	def bake(self, masked=False):
		tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))
		(tmp / "models/mapmodels").mkdir(parents=True)
		(tmp / "models/mapmodels/hedgehog.mdl").write_bytes(make_mdl(masked))
		(tmp / "maps").mkdir()
		bsp = tmp / "maps/dod_test.bsp"
		bsp.write_bytes(make_bsp(ENTS))
		logs = []
		stats = self.baker.bake(bsp, [tmp], tmp / "gp_dod_test.gcp", tmp / "pt_dod_test.gct",
			self.disc.gx_cmpr_encode_blocks, log=logs.append)
		return stats, (tmp / "gp_dod_test.gcp").read_bytes(), (tmp / "pt_dod_test.gct").read_bytes(), logs

	def test_props_with_a_model_file_are_instanced_named_ones_too(self):
		stats, _gcp, _gct, logs = self.bake()
		self.assertEqual(stats["instances"], 3)	# two unnamed, one named
		self.assertEqual(stats["models"], 1)	# one model, placed twice
		self.assertEqual(stats["missing"], 1)
		self.assertTrue(any("not_on_disc" in line for line in logs))

	def test_pack_layout_matches_the_engine_loader(self):
		"""R_GXPropLoad: header words, 32-byte alignment, instance and model tables."""
		stats, gcp, _gct, _logs = self.bake()
		hdr = struct.unpack_from(">8I", gcp, 0)
		self.assertEqual(gcp[:4], b"GCpr")
		self.assertEqual(hdr[1] >> 16, 7)
		self.assertEqual(hdr[1] & 0xFFFF, 3)
		self.assertEqual(hdr[2] >> 16, 1)
		self.assertEqual(hdr[3], 32)
		for word in (hdr[4], hdr[5], hdr[7], len(gcp)):
			self.assertEqual(word % 32, 0)
		self.assertEqual(hdr[7], len(gcp))	# no far region
		model, count, *rest = struct.unpack_from(">HH3h3h3h3hI", gcp, 32)
		mins, maxs, org, ang, lit = rest[0:3], rest[3:6], rest[6:9], rest[9:12], rest[12]
		self.assertEqual((model, count), (0, 2))
		# The named prop: same model in the low byte, its ordinal + 1 above it
		# (R_GXPropEmitReal asks SV_GCNamedPropVisible for ordinal 0).
		words = [struct.unpack_from(">H", gcp, 32 + i * 32)[0] for i in range(3)]
		self.assertEqual(sorted(words), [0, 0, 0x0100])
		self.assertEqual(tuple(org), (100, 200, 0))
		self.assertEqual(ang[1], 16384)	# 90 degrees of yaw
		self.assertEqual(lit & 0xFF, 1)
		# Yawed 90 degrees: the quad's +X edge runs along +Y, lifted by the bone.
		self.assertEqual(tuple(mins), (68, 200, 10))
		self.assertEqual(tuple(maxs), (100, 232, 10))
		dl_ofs, first_bytes, tris, frac = struct.unpack_from(">IIHB", gcp, hdr[4])
		self.assertEqual((dl_ofs, tris), (0, 2))
		self.assertGreaterEqual(first_bytes, 64)
		self.assertEqual(first_bytes % 32, 0)

	def test_piece_is_a_gx_display_list_the_engine_accepts(self):
		_stats, gcp, _gct, _logs = self.bake()
		hdr = struct.unpack_from(">8I", gcp, 0)
		_dl_ofs, first_bytes, _tris, frac = struct.unpack_from(">IIHB", gcp, hdr[4])
		piece = gcp[hdr[5]:hdr[5] + first_bytes]
		mid, _flags, nverts, body, nxt, ntris = struct.unpack_from(">BBHIIH", piece, 0)
		self.assertEqual((mid, nverts, nxt, ntris), (0, 4, 0, 2))
		self.assertEqual(body + 32, first_bytes)
		self.assertLessEqual(nverts * 11 + 3, body)	# the engine's bound
		cmd, n = struct.unpack_from(">BH", piece, 32)
		self.assertEqual((cmd, n), (0x98, 4))
		x, y, z, _nrm, s, t = struct.unpack_from(">hhhBhh", piece, 35 + 11 * 3)
		q = 1 << frac
		self.assertEqual((x, y, z), (32 * q, 32 * q, 10 * q))
		self.assertEqual((s, t), (256, 256))
		self.assertEqual(piece[35 + 44:], bytes(len(piece) - 35 - 44))	# NOP padding

	def test_normal_index_matches_the_engine_table(self):
		table = self.baker.oct_normal_table()
		up = self.baker.oct_index((0.0, 0.0, 1.0))
		self.assertGreater(table[up][2], 0.97)

	def test_texture_pack_layout(self):
		for masked in (False, True):
			_stats, _gcp, gct, _logs = self.bake(masked)
			self.assertEqual(gct[:4], b"GCWT")
			count, blob = struct.unpack_from(">II", gct, 4)
			self.assertEqual(count, 1)
			_name, w, h, levels, flags, _tag, ofs = self.baker.GC_WT_ENTRY.unpack_from(gct, 12)
			self.assertEqual((w, h, levels, ofs), (16, 16, 2, 0))
			self.assertEqual(flags, 1 if masked else 0)
			data = (12 + 28 + 31) & ~31
			self.assertEqual(len(gct), data + blob)
			self.assertEqual(blob, (16 * 16 // 2 + 8 * 8 // 2 + 31) & ~31)

	def test_engine_reads_the_pack_per_map(self):
		src = (ROOT / "ref/gx/r_gx_world.c").read_text()
		self.assertIn('"media/gp_%s.gcp"', src)
		self.assertIn('"media/pt_%s.gct"', src)
		server = (ROOT / "engine/server/sv_game.c").read_text()
		self.assertIn('"media/gp_%s.gcp"', server)


class DropToFloorTests(unittest.TestCase):
	"""env_model spawnflag 2 is the game's DROP_TO_FLOOR."""

	@classmethod
	def setUpClass(cls):
		cls.props = load("bake_props_goldsrc", "scripts/bake_props_goldsrc.py")

	def floor(self):
		import types

		# One node: the plane z = -40, empty above (leaf 1), solid below (leaf 0).
		light = self.props.FloorLight.__new__(self.props.FloorLight)
		light.bsp = types.SimpleNamespace(
			planes=[(0.0, 0.0, 1.0, -40.0)],
			nodes=[(0, -2, -1)],
			leaves=[(-2,), (-1,)],
		)
		return light

	def test_a_solid_brush_entity_is_ground_too(self):
		import types

		# World floor at z = -160; a func_wall slab (model 1) with its top at
		# z = -88, like the sand on dod_charlie's beach. Model 2 is a trigger.
		light = self.props.FloorLight.__new__(self.props.FloorLight)
		model = lambda head, lo, hi: (*lo, *hi, 0.0, 0.0, 0.0, head, 0, 0, 0, 0, 0, 0)
		light.bsp = types.SimpleNamespace(
			planes=[(0.0, 0.0, 1.0, -160.0), (0.0, 0.0, 1.0, -88.0), (0.0, 0.0, 1.0, -20.0)],
			nodes=[(0, -2, -1), (1, -2, -1), (2, -2, -1)],
			leaves=[(-2,), (-1,)],
			faces=[],
			models=[
				model(0, (-4096.0,) * 3, (4096.0,) * 3),
				model(1, (-500.0, -500.0, -120.0), (500.0, 500.0, -88.0)),
				model(2, (-500.0, -500.0, -60.0), (500.0, 500.0, -20.0)),
			],
		)
		light.floors = []
		light.heads = [(0, (0.0, 0.0, 0.0), None, None, -1)]
		self.assertAlmostEqual(light.drop((0.0, 0.0, -10.0)), -160.0, delta=0.01)
		added = light.add_brush_entities([
			{"classname": "func_wall", "model": "*1"},
			{"classname": "trigger_multiple", "model": "*2"},
			{"classname": "func_illusionary", "model": "*2"},
		])
		self.assertEqual(added, 1)
		self.assertAlmostEqual(light.drop((0.0, 0.0, -10.0)), -88.0, delta=0.01)
		# Outside the slab the world floor still counts.
		self.assertAlmostEqual(light.drop((900.0, 0.0, -10.0)), -160.0, delta=0.01)
		# The game spawns entities in lump order: a prop that comes before
		# the slab (entity 0) falls through it, and one that starts inside
		# it is not stuck. A later prop lands on it.
		self.assertAlmostEqual(light.drop((0.0, 0.0, -10.0), before=0), -160.0, delta=0.01)
		self.assertAlmostEqual(light.drop((0.0, 0.0, -100.0), before=0), -160.0, delta=0.01)
		self.assertAlmostEqual(light.drop((0.0, 0.0, -10.0), before=1), -88.0, delta=0.01)

	def test_spawnflag_2_marks_a_prop_for_the_drop(self):
		ents = [
			{"classname": "env_model", "model": "models/a.mdl", "origin": "0 0 4", "spawnflags": "2"},
			{"classname": "env_model", "model": "models/a.mdl", "origin": "0 0 4"},
		]
		props = self.props.static_props(ents)
		self.assertEqual([p["drop"] for p in props], [True, False])

	def test_env_model_is_posed_in_its_named_sequence(self):
		import types

		ents = [
			{"classname": "env_model", "model": "models/tree.mdl",
				"m_iszSequence_On": "tree_high_origin", "m_iszSequence_Off": "idle"},
			{"classname": "env_model", "model": "models/tree.mdl", "spawnflags": "1",
				"m_iszSequence_On": "tree_high_origin", "m_iszSequence_Off": "idle"},
			{"classname": "env_model", "model": "models/tree.mdl", "sequence": "2"},
			# only env_model has these keys
			{"classname": "dod_object", "model": "models/tree.mdl", "m_iszSequence_On": "tree_high_origin"},
		]
		props = self.props.static_props(ents)
		self.assertEqual([p["sequence_name"] for p in props], ["tree_high_origin", "idle", "", ""])
		labels = ["idle", "tree_low_origin", "Tree_High_Origin"]
		mdl = types.SimpleNamespace(sequence_index=lambda name: next(
			(i for i, label in enumerate(labels) if label.lower() == name.lower()), -1))
		self.assertEqual([self.props.prop_sequence(mdl, p) for p in props], [2, 0, 2, 0])
		# a name the model lacks falls back to sequence 0, as the game does
		self.assertEqual(self.props.prop_sequence(mdl, {"sequence_name": "gone", "sequence": 5}), 0)

	def test_sequence_labels_are_read_from_the_model(self):
		mdl = self.props.StudioModel.__new__(self.props.StudioModel)
		size = self.props.SEQ_SZ
		mdl.seqindex = 16
		mdl.numseq = 2
		mdl.data = (b"\0" * 16 + b"idle".ljust(size, b"\0") + b"tree_high_origin".ljust(size, b"\0"))
		self.assertEqual(mdl.sequence_index("TREE_high_origin"), 1)
		self.assertEqual(mdl.sequence_index("missing"), -1)

	def test_motion_bone_is_held_on_the_axes_the_sequence_moves_along(self):
		# One bone at z=-365, no animation data. A sequence whose motion type
		# has the Z bit holds it at 0, as R_StudioCalcRotations does.
		import struct
		bp = self.props
		mdl = bp.StudioModel.__new__(bp.StudioModel)
		bone = bytearray(bp.BONE_SZ)
		struct.pack_into("<i", bone, 32, -1)
		struct.pack_into("<6f", bone, 64, 5.0, 6.0, -365.0, 0.0, 0.0, 0.0)
		seqs = bytearray(bp.SEQ_SZ * 2)
		struct.pack_into("<ii", seqs, bp.SEQ_SZ + 68, 4, 0)
		mdl.data = bytes(bone) + bytes(seqs)
		mdl.numbones, mdl.boneindex = 1, 0
		mdl.numseq, mdl.seqindex = 2, bp.BONE_SZ
		self.assertAlmostEqual(mdl.bone_matrices(0)[0][2][3], -365.0)
		held = mdl.bone_matrices(1)[0]
		self.assertAlmostEqual(held[2][3], 0.0)
		self.assertAlmostEqual(held[0][3], 5.0)

	def test_named_props_get_their_ordinal_in_lump_order(self):
		ents = [
			{"classname": "env_model", "model": "models/a.mdl"},
			{"classname": "env_model", "model": "models/a.mdl", "targetname": "good"},
			{"classname": "func_wall", "model": "*1", "targetname": "wall"},
			{"classname": "env_model", "targetname": "no_model"},
			# Hidden until env_render shows it: still baked when named.
			{"classname": "dod_object", "model": "models/b.mdl", "targetname": "bad", "rendermode": "5"},
			{"classname": "env_model", "model": "models/b.mdl", "rendermode": "5", "renderamt": "0"},
		]
		props = self.props.static_props(ents)
		self.assertEqual([p["named"] for p in props], [0, 1, 2])
		self.assertEqual([p["additive"] for p in props], [False, False, False])

	def test_a_carried_dod_object_is_not_baked_into_the_pack(self):
		# charlie's tnt satchels: a capture area wants the group, so the game spawns them; other groups stay decoration
		ents = [
			{"classname": "dod_capture_area", "area_object_group": "tnt"},
			{"classname": "dod_object", "model": "models/b.mdl", "object_group": "tnt"},
			{"classname": "dod_object", "model": "models/b.mdl", "object_group": "dev42"},
			{"classname": "dod_object", "model": "models/b.mdl"},
			{"classname": "env_model", "model": "models/a.mdl"},
		]
		props = self.props.static_props(ents)
		self.assertEqual(len(props), 3)	# the tnt object is the only one left out

	def test_footprint_light_ignores_a_dark_texel_under_the_origin(self):
		light = self.props.FloorLight.__new__(self.props.FloorLight)
		# Dark under the origin, lit ground to the east, no floor to the west.
		light.at = lambda p: None if p[0] < -1 else ((17, 16, 15) if p[0] < 1 else (140, 120, 110))
		self.assertEqual(light.footprint((0.0, 0.0, 8.0), 32.0), (140, 120, 110))
		light.at = lambda p: (17, 16, 15)
		self.assertEqual(light.footprint((0.0, 0.0, 8.0), 32.0), (17, 16, 15))
		light.at = lambda p: None
		self.assertIsNone(light.footprint((0.0, 0.0, 8.0), 32.0))

	def test_drop_lands_on_the_first_solid_below(self):
		z = self.floor().drop((10.0, 20.0, 4.0))
		self.assertAlmostEqual(z, -40.0, delta=0.01)

	def test_no_drop_from_inside_solid_or_past_the_reach(self):
		light = self.floor()
		self.assertIsNone(light.drop((0.0, 0.0, -60.0)))
		self.assertIsNone(light.drop((0.0, 0.0, 300.0)))

	def test_spawn_drop_lifts_one_unit_first(self):
		light = self.floor()
		# half a unit inside the floor: the lift frees it and it lands on top
		prop = {"origin": [0.0, 0.0, -40.5], "ent": 3}
		self.assertAlmostEqual(self.props.drop_like_the_game(light, prop), -40.0, delta=0.01)
		self.assertAlmostEqual(prop["origin"][2], -40.0, delta=0.01)
		# deep inside solid, or nothing within reach: only the lift remains
		for z in (-60.0, 300.0):
			prop = {"origin": [0.0, 0.0, z], "ent": 3}
			self.assertIsNone(self.props.drop_like_the_game(light, prop))
			self.assertEqual(prop["origin"][2], z + 1.0)

	def test_dod_object_falls_any_distance_and_is_not_a_spawn_drop(self):
		ents = [
			{"classname": "dod_object", "model": "models/a.mdl", "origin": "0 0 300", "spawnflags": "2"},
			{"classname": "env_model", "model": "models/a.mdl", "origin": "0 0 300"},
		]
		props = self.props.static_props(ents)
		self.assertEqual([(p["toss"], p["drop"]) for p in props], [(True, False), (False, False)])
		light = self.floor()
		self.assertAlmostEqual(self.props.fall_like_the_game(light, props[0]), -40.0, delta=0.01)
		self.assertAlmostEqual(props[0]["origin"][2], -40.0, delta=0.01)


if __name__ == "__main__":
	unittest.main()


def make_mprop(instances, ntex=1):
	"""An MPRP v2 pack (mohaa-gc tools/props_models.py): one 8x8 texture, one
	quad model 10 x 20 in x/z, and the given (model, origin, angles, scale)."""
	out = bytearray(b"MPRP" + struct.pack("<4i", 2, ntex, 1, len(instances)))
	for _ in range(ntex):
		pal = b"".join(bytes((i, 255 - i, 40, 255)) for i in range(256))
		out += b"textures/models/bark.tga".ljust(32, b"\0") + struct.pack("<2HI", 8, 8, 0) + pal + bytes(range(64))
	verts = [(0, 0, 0), (10, 0, 0), (10, 0, 20), (0, 0, 20)]
	out += b"static/pine.tik".ljust(64, b"\0") + struct.pack("<6fi", 0, 0, 0, 10, 0, 20, 1)
	out += struct.pack("<3i", 0, 4, 2)
	for k, v in enumerate(verts):
		out += struct.pack("<8f", *v, 0, -1, 0, k & 1, k >> 1)
	out += struct.pack("<6H", 0, 1, 2, 0, 2, 3)
	for mi, org, ang, sc in instances:
		out += struct.pack("<i7f", mi, *org, *ang, sc)
	return bytes(out)


def make_tree_mprop(instances, name=b"static/pine.tik"):
	"""An MPRP v2 pack with one masked 8x8 texture and a 48-triangle cone 40 wide and 100 high."""
	out = bytearray(b"MPRP" + struct.pack("<4i", 2, 1, 1, len(instances)))
	pal = b"".join(bytes((20, 100 + i // 3, 30, 255)) for i in range(256))
	out += b"textures/models/pine.tga".ljust(32, b"\0") + struct.pack("<2HI", 8, 8, 1) + pal
	out += bytes(255 if (i % 8 + i // 8) % 4 == 0 else 7 for i in range(64))
	verts = [(0.0, 0.0, 100.0, 0.5, 0.0)]
	for k in range(48):
		a = 2 * math.pi * k / 48
		verts.append((20 * math.cos(a), 20 * math.sin(a), 0.0, k / 48, 1.0))
	out += name.ljust(64, b"\0") + struct.pack("<6fi", -20, -20, 0, 20, 20, 100, 1)
	out += struct.pack("<3i", 0, len(verts), 48)
	for x, y, z, s_, t_ in verts:
		out += struct.pack("<8f", x, y, z, x / 20.0, y / 20.0, 0.5, s_, t_)
	for k in range(48):
		out += struct.pack("<3H", 0, 1 + k, 1 + (k + 1) % 48)
	for mi, org, ang, sc in instances:
		out += struct.pack("<i7f", mi, *org, *ang, sc)
	return bytes(out)


class MohaaPropPackTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.baker = load("bake_props_goldsrc", "scripts/bake_props_goldsrc.py")
		cls.disc = load("build_gamecube_disc_props", "scripts/build-gamecube-disc.py")

	def bake(self, instances, data=None, **kw):
		kw.setdefault("cells", False)
		with tempfile.TemporaryDirectory() as d:
			d = Path(d)
			(d / "m.mprop").write_bytes(data or make_mprop(instances))
			logs = []
			stats = self.baker.bake_mprop(d / "m.mprop", d / "gp_m.gcp", d / "pt_m.gct",
				self.disc.gx_cmpr_encode_blocks, log=logs.append, **kw)
			return stats, (d / "gp_m.gcp").read_bytes(), (d / "pt_m.gct").read_bytes(), logs

	def test_box_cluster_is_the_one_open_leaf_holding_the_box(self):
		class Bsp:	# one plane x = 0: leaf 1 (x >= 0) and leaf 2 (x < 0) open, leaf 3 solid under z = 0
			planes = [(1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0)]
			nodes = [(0, 1, -3), (1, -2, -4)]	# node 1 splits x >= 0 by z: above leaf 1, below leaf 3
			leaves = [(-2, 0), (-1, 0), (-1, 0), (-2, 0)]
		b = Bsp()
		cluster = self.baker.box_cluster
		self.assertEqual(cluster(b, (10, 0, 10), (20, 5, 30)), 0)	# leaf 1
		self.assertEqual(cluster(b, (-20, 0, 10), (-10, 5, 30)), 1)	# leaf 2
		self.assertEqual(cluster(b, (-5, 0, 10), (5, 5, 30)), self.baker.GC_PR_VIS_ALL)	# across the plane
		self.assertEqual(cluster(b, (10, 0, -5), (20, 5, 30)), 0)	# dips into solid: still the one open leaf
		self.assertEqual(cluster(None, (0, 0, 0), (1, 1, 1)), self.baker.GC_PR_VIS_ALL)

	def test_a_tree_gets_an_impostor_far_chain(self):
		stats, gcp, gct, _logs = self.bake(None, make_tree_mprop([(0, (0, 0, 0), (0, 0, 0), 1.0)]))
		self.assertEqual((stats["impostors"], stats["materials"]), (1, 2))
		hdr = struct.unpack_from(">8I", gcp, 0)
		models_ofs, dl_base, far_base = hdr[4], hdr[5], hdr[7]
		self.assertGreater(len(gcp), far_base)
		dl_ofs, first, tris, frac, _c, far_ofs, far_first, far_tris, _cm = struct.unpack_from(">IIHBBIIHH", gcp, models_ofs)
		self.assertEqual((tris, far_tris), (48, 4))
		piece = gcp[far_base + far_ofs:far_base + far_ofs + far_first]
		self.assertEqual(len(piece), far_first)
		self.assertEqual(piece[0], 1)	# the impostor's material
		self.assertEqual((piece[32], struct.unpack_from(">H", piece, 33)[0]), (0x80, 8))	# one GX quads command, 2 quads
		# the impostor texture: 64 x 64, alpha tested, some texels cut out and some kept
		name, tw, th, lv, flag, _r, _o = self.baker.GC_WT_ENTRY.unpack_from(gct, 12 + self.baker.GC_WT_ENTRY.size)
		self.assertEqual((name.rstrip(b"\0"), tw, th, flag), (b"i001", 64, 64, 1))

	def test_a_world_mesh_never_becomes_a_cutout(self):
		stats, _gcp, _gct, _logs = self.bake(None, make_tree_mprop([(0, (0, 0, 0), (0, 0, 0), 1.0)],
			b"*world/terrain_0_0"), cells=True)
		self.assertEqual((stats["impostors"], stats["cells"], stats["materials"]), (0, 0, 1))

	def test_far_trees_are_baked_into_cells(self):
		trees = [(0, (x * 256.0, y * 256.0, 0), (0, x * 37 % 360, 0), 1.0) for x in range(8) for y in range(8)]
		stats, gcp, _gct, _logs = self.bake(None, make_tree_mprop(trees), cells=True)
		hdr = struct.unpack_from(">8I", gcp, 0)
		ncells = hdr[2] & 0xFFFF
		self.assertEqual((hdr[1] >> 16, ncells, stats["cells"]), (9, stats["cells"], ncells))
		self.assertGreater(ncells, 1)
		ninst = hdr[1] & 0xFFFF
		self.assertEqual(ninst, 64 + ncells)
		at = 32 + ninst * 32
		at += (32 + ninst * 12 + 31) & ~31
		seen = []
		for c in range(ncells):
			cell_inst, first, count, _pad = struct.unpack_from(">4H", gcp, at + 8 * c)
			self.assertEqual(cell_inst, 64 + c)	# the cell instances come last
			seen += range(first, first + count)
			model, tris = struct.unpack_from(">HH", gcp, 32 + 32 * cell_inst)
			self.assertEqual(tris, 4 * count)	# two quads a tree
			self.assertGreaterEqual(model, 1)
		self.assertEqual(sorted(seen), list(range(64)))	# every tree in one cell, members contiguous
		self.assertEqual(stats["cell_members"], 64)

	def test_reads_the_mprop_pack(self):
		pack = self.baker.read_mprop(make_mprop([(0, (1, 2, 3), (0, 90, 0), 0.766)]))
		self.assertEqual(len(pack["textures"]), 1)
		self.assertEqual(pack["textures"][0]["palette"][:3], bytes((0, 255, 40)))
		self.assertEqual(len(pack["models"][0]["surfaces"][0][1]), 4)
		self.assertEqual(pack["instances"][0][0], 0)
		with self.assertRaises(ValueError):
			self.baker.read_mprop(make_mprop([])[:-1] + b"\0\0")

	def test_every_scale_shares_one_model(self):
		stats, gcp, gct, _logs = self.bake([
			(0, (100, 200, 0), (0, 90, 0), 0.766),
			(0, (300, 200, 0), (0, 0, 0), 0.77),	# same 5 % step as 0.766
			(0, (500, 200, 0), (0, 0, 0), 1.0),
		])
		# one model: v9 scales each instance at draw time (high byte: 1.05 ^ (byte - 128))
		self.assertEqual((stats["instances"], stats["models"], stats["materials"]), (3, 1, 1))
		hdr = struct.unpack_from(">8I", gcp, 0)
		self.assertEqual((gcp[:4], hdr[1] >> 16, hdr[1] & 0xFFFF, hdr[2] >> 16), (b"GCpr", 9, 3, 1))
		model, count, *rest = struct.unpack_from(">HH3h3h3h3hI", gcp, 32)
		self.assertEqual((model, count), ((128 - 5) << 8, 2))
		self.assertEqual(struct.unpack_from(">H", gcp, 64)[0], (128 - 5) << 8)	# 0.77: the same step
		self.assertEqual(struct.unpack_from(">H", gcp, 96)[0], 128 << 8)
		self.assertEqual(tuple(rest[6:9]), (100, 200, 0))
		self.assertEqual(rest[10], 16384)	# 90 degrees of yaw
		# the 0.766 quad is about 7.7 wide and 15.3 high, yawed onto +y
		self.assertEqual((rest[1], rest[4], rest[5]), (200, 208, 16))
		self.assertEqual(gct[:4], b"GCWT")

	def test_past_the_fixed_tables_every_instance_is_kept(self):
		n = self.baker.GC_PR_FIXED_INST + 88
		stats, gcp, _gct, logs = self.bake([(0, (i, 0, 0), (0, 0, 0), 1.0) for i in range(n)])
		self.assertEqual((stats["instances"], stats["dropped"]), (n, 0))
		self.assertEqual(struct.unpack_from(">I", gcp, 4)[0] & 0xFFFF, n)
		self.assertFalse(any("left out" in line for line in logs))

	def test_past_the_instance_limit_some_are_left_out(self):
		n = self.baker.GC_PR_MAX_INST_BIG + 5
		stats, gcp, _gct, logs = self.bake([(0, (i, 0, 0), (0, 0, 0), 1.0) for i in range(n)])
		self.assertEqual((stats["instances"], stats["dropped"]), (self.baker.GC_PR_MAX_INST_BIG, 5))
		self.assertTrue(any("left out" in line for line in logs))
