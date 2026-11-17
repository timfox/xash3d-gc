"""scripts/bake_props_goldsrc.py: a map's static studio props as a GX prop
pack (media/gp_<map>.gcp + media/pt_<map>.gct), read by ref/gx/r_gx_world.c."""
import importlib.util
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

	def test_only_unnamed_props_with_a_model_file_are_instanced(self):
		stats, _gcp, _gct, logs = self.bake()
		self.assertEqual(stats["instances"], 2)
		self.assertEqual(stats["models"], 1)	# one model, placed twice
		self.assertEqual(stats["missing"], 1)
		self.assertTrue(any("not_on_disc" in line for line in logs))

	def test_pack_layout_matches_the_engine_loader(self):
		"""R_GXPropLoad: header words, 32-byte alignment, instance and model tables."""
		stats, gcp, _gct, _logs = self.bake()
		hdr = struct.unpack_from(">8I", gcp, 0)
		self.assertEqual(gcp[:4], b"GCpr")
		self.assertEqual(hdr[1] >> 16, 7)
		self.assertEqual(hdr[1] & 0xFFFF, 2)
		self.assertEqual(hdr[2] >> 16, 1)
		self.assertEqual(hdr[3], 32)
		for word in (hdr[4], hdr[5], hdr[7], len(gcp)):
			self.assertEqual(word % 32, 0)
		self.assertEqual(hdr[7], len(gcp))	# no far region
		model, count, *rest = struct.unpack_from(">HH3h3h3h3hI", gcp, 32)
		mins, maxs, org, ang, lit = rest[0:3], rest[3:6], rest[6:9], rest[9:12], rest[12]
		self.assertEqual((model, count), (0, 2))
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
		light.heads = [(0, (0.0, 0.0, 0.0), None, None)]
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

	def test_spawnflag_2_marks_a_prop_for_the_drop(self):
		ents = [
			{"classname": "env_model", "model": "models/a.mdl", "origin": "0 0 4", "spawnflags": "2"},
			{"classname": "env_model", "model": "models/a.mdl", "origin": "0 0 4"},
		]
		props = self.props.static_props(ents)
		self.assertEqual([p["drop"] for p in props], [True, False])

	def test_drop_lands_on_the_first_solid_below(self):
		z = self.floor().drop((10.0, 20.0, 4.0))
		self.assertAlmostEqual(z, -40.0, delta=0.01)

	def test_no_drop_from_inside_solid_or_past_the_reach(self):
		light = self.floor()
		self.assertIsNone(light.drop((0.0, 0.0, -60.0)))
		self.assertIsNone(light.drop((0.0, 0.0, 300.0)))


if __name__ == "__main__":
	unittest.main()
