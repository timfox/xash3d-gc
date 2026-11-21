"""Static brush entities baked into the world DL (scripts/bake_worlddl_goldsrc.py)."""
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_bake():
	spec = importlib.util.spec_from_file_location("bake_worlddl_goldsrc", ROOT / "scripts/bake_worlddl_goldsrc.py")
	mod = importlib.util.module_from_spec(spec)
	sys.modules[spec.name] = mod
	spec.loader.exec_module(mod)
	return mod


def entity_lump(ents) -> bytes:
	text = ""
	for e in ents:
		text += "{\n" + "".join(f'"{k}" "{v}"\n' for k, v in e.items()) + "}\n"
	return text.encode("latin1") + b"\0"


class FakeBsp:
	"""Only what static_brushes reads: entity lump, models, faces, textures."""

	def __init__(self, ents, tex_of_model):
		lump0 = entity_lump(ents)
		head = bytearray(4 + 15 * 8)
		struct.pack_into("<I", head, 0, 30)
		struct.pack_into("<ii", head, 4, len(head), len(lump0))
		self.data = bytes(head) + lump0
		self.mip_names = sorted(set(tex_of_model.values()) | {"wall"})
		self.texinfos = [((1, 0, 0, 0), (0, 1, 0, 0), i, 0) for i in range(len(self.mip_names))]
		self.faces = []
		self.models = [(0.0,) * 9 + (0, 0, 0, 0, 0, 0, 0)]
		count = max(tex_of_model) + 1 if tex_of_model else 1
		for num in range(1, count):
			tex = tex_of_model.get(num, "wall")
			self.models.append((0.0,) * 9 + (0, 0, 0, 0, 0, len(self.faces), 1))
			self.faces.append((0, 0, 0, 4, self.mip_names.index(tex), b"\0\xff\xff\xff", -1))


class StaticBrushTest(unittest.TestCase):
	def setUp(self):
		self.bake = load_bake()

	def pick(self, ents, tex=None):
		tex = tex or {}
		for e in ents:
			m = e.get("model", "")
			if m.startswith("*"):
				tex.setdefault(int(m[1:]), "wall")
		return self.bake.static_brushes(FakeBsp(ents, tex))

	def test_plain_walls_are_static(self):
		got = self.pick([
			{"classname": "worldspawn"},
			{"classname": "func_wall", "model": "*1"},
			{"classname": "func_illusionary", "model": "*2", "skin": "-1"},
			{"classname": "func_wall", "model": "*3", "rendermode": "0", "origin": "0 0 0"},
		])
		self.assertEqual(got, [(1, True), (2, False), (3, True)])

	def test_anything_that_can_change_stays_an_entity(self):
		got = self.pick([
			{"classname": "func_wall", "model": "*1", "targetname": "gate"},
			{"classname": "func_wall", "model": "*2", "rendermode": "4", "renderamt": "128"},
			{"classname": "func_wall", "model": "*3", "origin": "0 0 16"},
			{"classname": "func_door", "model": "*4"},
			{"classname": "func_wall", "model": "*5", "renderfx": "14"},
			{"classname": "func_illusionary", "model": "*6", "skin": "-16"},
			{"classname": "func_wall", "model": "*7", "master": "ms"},
			{"classname": "func_wall", "model": "*8"},
			{"classname": "func_wall", "model": "*8"},
		])
		self.assertEqual(got, [])

	def test_special_textures_stay_entities(self):
		ents = [{"classname": "func_wall", "model": f"*{i}"} for i in range(1, 6)]
		got = self.pick(ents, {1: "{fence", 2: "!water", 3: "+0button", 4: "scroll_a", 5: "brick"})
		self.assertEqual(got, [(5, True)])

	def test_masked_walls_at_full_amount_are_static(self):
		# Render mode 4, amount 255: the mask is cut out and nothing fades.
		ents = [
			{"classname": "func_wall", "model": "*1", "rendermode": "4", "renderamt": "255"},
			{"classname": "func_illusionary", "model": "*2", "rendermode": "4", "renderamt": "255"},
			{"classname": "func_wall", "model": "*3", "rendermode": "4"},
			{"classname": "func_wall", "model": "*4", "rendermode": "5", "renderamt": "255"},
			{"classname": "func_wall", "model": "*5", "rendermode": "2", "renderamt": "255"},
			{"classname": "func_wall", "model": "*6", "rendermode": "4", "renderamt": "255", "targetname": "net"},
			{"classname": "func_wall", "model": "*7", "rendermode": "4", "renderamt": "255"},
		]
		got = self.pick(ents, {1: "{tension6", 2: "{fence", 3: "{fence", 4: "{fence", 5: "glass", 6: "{fence", 7: "!water"})
		self.assertEqual(got, [(1, True), (2, False)])

	def test_list_file(self):
		with tempfile.TemporaryDirectory() as tmp:
			out = Path(tmp) / "wd_x_sb.gcd"
			self.bake.write_static_brushes(out, [(1, True), (700, False)], 123456)
			raw = out.read_bytes()
		self.assertEqual(struct.unpack_from(">4sHHI", raw, 0), (b"GCsb", 1, 2, 123456))
		self.assertEqual(struct.unpack_from(">2H", raw, 12), (0x8001, 700))

	def test_runtime_reads_the_same_layout(self):
		src = (ROOT / "engine/common/model.c").read_text()
		self.assertIn('"media/wd_%s_sb.gcd"', src)
		self.assertIn("GC_SB_SOLID\t0x8000", src)
		self.assertIn("#define GC_SB_MAX\t256", src)
		self.assertEqual(self.bake.SB_MAX, 256)
		self.assertEqual(self.bake.SB_MODEL_MAX, 1024)


def load_pair_check():
	"""world_dl_pair_ok from the disc builder, without importing the whole script."""
	import ast

	src = (ROOT / "scripts/build-gamecube-disc.py").read_text()
	fn = next(n for n in ast.parse(src).body if isinstance(n, ast.FunctionDef) and n.name == "world_dl_pair_ok")
	scope = {"Path": Path}
	exec(compile(ast.Module(body=[fn], type_ignores=[]), "build-gamecube-disc.py", "exec"), scope)
	return scope["world_dl_pair_ok"]


class StalePrebakeTest(unittest.TestCase):
	def test_disc_builder_takes_only_a_pack_its_list_was_written_with(self):
		ok = load_pair_check()
		bake = load_bake()
		with tempfile.TemporaryDirectory() as tmp:
			wd = Path(tmp) / "wd_dod_test.gcd"
			wd.write_bytes(b"x" * 1000)
			self.assertFalse(ok(wd))  # no list at all: an old prebake
			sb = Path(tmp) / "wd_dod_test_sb.gcd"
			bake.write_static_brushes(sb, [(3, True)], 1000)
			self.assertTrue(ok(wd))
			bake.write_static_brushes(sb, [(3, True)], 2000)
			self.assertFalse(ok(wd))  # list from another bake


if __name__ == "__main__":
	unittest.main()
