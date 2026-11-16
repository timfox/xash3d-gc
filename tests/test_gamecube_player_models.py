"""Host test: the disc build splits DoD team bodies into core + animation pack."""
from pathlib import Path
import importlib.util
import io
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DODGC = ROOT.parent / "dod-gc"


def load(name, path):
	spec = importlib.util.spec_from_file_location(name, path)
	module = importlib.util.module_from_spec(spec)
	sys.modules[name] = module
	spec.loader.exec_module(module)
	return module


@unittest.skipUnless((DODGC / "tools/split_studio_mdl.py").is_file(), "dod-gc checkout not beside the engine")
class PlayerModelStagingTests(unittest.TestCase):
	def test_core_and_pack(self):
		disc = load("build_gamecube_disc", ROOT / "scripts/build-gamecube-disc.py")
		splitter = load("split_studio_mdl", DODGC / "tools/split_studio_mdl.py")
		fixture = load("split_fixture", DODGC / "tests/test_split_studio_mdl.py")
		model = bytes(fixture.build_model()[0])
		with tempfile.TemporaryDirectory() as tmp:
			data = Path(tmp)
			folder = data / "models/player/axis-inf"
			folder.mkdir(parents=True)
			(folder / "axis-inf.mdl").write_bytes(model)
			buf = io.BytesIO()
			with zipfile.ZipFile(buf, "w") as archive:
				count = disc.inject_gc_player_models(archive, data, splitter)
				names = set(archive.namelist())
				core = archive.read("gc_studio/axis-inf.mdl")
			self.assertGreaterEqual(count, 1)
			self.assertIn("gc_studio/axis-inf.mdl", names)
			packs = list((data / "models/player").glob("anim_*.gca"))
			self.assertEqual(len(packs), 1)
			self.assertEqual(packs[0].read_bytes()[:4], b"GCAP")
			# Sequence group 1 of the core names the pack by its in-game path.
			numgroups, groupindex = struct.unpack_from("<ii", core, 172)
			self.assertEqual(numgroups, 2)
			name = core[groupindex + 104 + 32 : groupindex + 104 + 96].split(b"\0")[0].decode()
			self.assertEqual(name, "models/player/" + packs[0].name)
			self.assertLess(len(core), len(model))

	def test_meshes_streamed_to_pack(self):
		disc = load("build_gamecube_disc", ROOT / "scripts/build-gamecube-disc.py")
		splitter = load("split_studio_mdl", DODGC / "tools/split_studio_mdl.py")
		fixture = load("split_fixture", DODGC / "tests/test_split_studio_mdl.py")
		if not hasattr(splitter, "stream_meshes") or not hasattr(fixture, "build_body_model"):
			self.skipTest("dod-gc splitter without mesh streaming")
		model, want, _ = fixture.build_body_model()
		with tempfile.TemporaryDirectory() as tmp:
			data = Path(tmp)
			folder = data / "models/player/us-inf"
			folder.mkdir(parents=True)
			(folder / "us-inf.mdl").write_bytes(model)
			with zipfile.ZipFile(io.BytesIO(), "w") as archive:
				self.assertEqual(disc.inject_gc_player_models(archive, data, splitter), 1)
				core = archive.read("gc_studio/us-inf.mdl")
			meshes = list((data / "models/player").glob("mesh_*.gcm"))
			self.assertEqual(len(meshes), 1)
			self.assertEqual(meshes[0].read_bytes()[:4], b"GCAP")
			numgroups, groupindex = struct.unpack_from("<ii", core, 172)
			last = groupindex + 104 * (numgroups - 1)
			self.assertEqual(core[last + 32 : last + 96].split(b"\0")[0].decode(), "models/player/" + meshes[0].name)
			bodypart = struct.unpack_from("<i", core, 140 + 4 * splitter.HDR_FIELDS.index("bodypartindex"))[0]
			models = struct.unpack_from("<i", core, bodypart + 72)[0]
			self.assertEqual(struct.unpack_from("<i", core, models + 64)[0], splitter.MESH_STREAMED)



def make_tex_model(textures):
	"""A v10 *T.mdl holding only textures: [(name, flags, w, h, index bytes, palette)]."""
	table = 244
	out = bytearray(table + 80 * len(textures))
	out[0:4] = b"IDST"
	struct.pack_into("<i", out, 4, 10)
	for i, (name, flags, w, h, pixels, palette) in enumerate(textures):
		off = table + i * 80
		out[off : off + 64] = name.encode().ljust(64, b"\0")
		struct.pack_into("<iiii", out, off + 64, flags, w, h, len(out))
		out += pixels + palette
	struct.pack_into("<i", out, 72, len(out))
	struct.pack_into("<i", out, 180, len(textures))
	struct.pack_into("<i", out, 184, table)
	struct.pack_into("<i", out, 188, table + 80 * len(textures))
	return bytes(out)


def cmpr_texel(blob, w, x, y):
	"""Decode one texel of a GX CMPR image: (r, g, b, opaque)."""
	tile = (y // 8) * (w // 8) + x // 8
	blk = blob[tile * 32 + (((y & 7) >> 2) * 2 + ((x & 7) >> 2)) * 8 :][:8]
	c0, c1 = struct.unpack(">HH", blk[:4])
	idx = (blk[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3

	def rgb(c):
		r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
		return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))

	e0, e1 = rgb(c0), rgb(c1)
	if idx == 0:
		return e0 + (True,)
	if idx == 1:
		return e1 + (True,)
	if c0 > c1:
		k = (2, 1) if idx == 2 else (1, 2)
		return tuple((k[0] * a + k[1] * b) // 3 for a, b in zip(e0, e1)) + (True,)
	if idx == 2:
		return tuple((a + b) // 2 for a, b in zip(e0, e1)) + (True,)
	return (0, 0, 0, False)


class PlayerSkinBakeTests(unittest.TestCase):
	def test_cmpr_companion(self):
		try:
			import numpy  # noqa: F401
			import PIL  # noqa: F401
		except ImportError:
			self.skipTest("numpy and Pillow needed for the bake")
		disc = load("build_gamecube_disc", ROOT / "scripts/build-gamecube-disc.py")
		palette = bytearray(768)
		palette[3:6] = bytes((200, 40, 40))      # index 1: red
		palette[765:768] = bytes((0, 0, 255))    # index 255: key colour
		palette = bytes(palette)
		body = bytes([1]) * (96 * 60)            # 96x60 flat red -> 128x64
		mask = bytearray([1]) * (16 * 16)
		for y in range(8):
			for x in range(8):
				mask[y * 16 + x] = 255             # top-left quarter cut out
		remap = bytes([1]) * (8 * 8)
		model = make_tex_model([
			("body.bmp", 0, 96, 60, body, palette),
			("net.bmp", 0x40 | 0x2, 16, 16, bytes(mask), palette),
			("DM_Base.bmp", 0, 8, 8, remap, palette),
		])
		with tempfile.TemporaryDirectory() as tmp:
			src = Path(tmp) / "axis-infT.mdl"
			src.write_bytes(model)
			out = disc.gc_cmpr_skin_companion_bytes(src, max_edge=128)
		self.assertIsNotNone(out)
		self.assertEqual(struct.unpack_from("<i", out, 72)[0], len(out))
		self.assertEqual(struct.unpack_from("<i", out, 180)[0], 3)
		table = struct.unpack_from("<i", out, 184)[0]
		flags, w, h, index = struct.unpack_from("<iiii", out, table + 64)
		self.assertEqual((flags, w, h), (disc.STUDIO_NF_GC_CMPR, 128, 64))
		self.assertEqual(index % 32, 0)
		r, g, b, opaque = cmpr_texel(out[index : index + w * h // 2], w, 50, 30)
		self.assertTrue(opaque)
		self.assertTrue(abs(r - 200) < 10 and abs(g - 40) < 10 and abs(b - 40) < 10, (r, g, b))
		flags, w, h, index = struct.unpack_from("<iiii", out, table + 80 + 64)
		self.assertEqual((flags, w, h), (disc.STUDIO_NF_GC_CMPR | 0x40, 16, 16))
		blob = out[index : index + w * h // 2]
		self.assertFalse(cmpr_texel(blob, w, 2, 2)[3])
		self.assertTrue(cmpr_texel(blob, w, 12, 12)[3])
		self.assertTrue(abs(cmpr_texel(blob, w, 12, 12)[0] - 200) < 10)
		# The colour-remap texture stays palettised.
		flags, w, h, index = struct.unpack_from("<iiii", out, table + 160 + 64)
		self.assertEqual((flags, w, h), (0, 8, 8))
		self.assertEqual(out[index : index + 64], remap)
		self.assertEqual(disc.gc_cmpr_skin_edge(68, 128), 64)
		self.assertEqual(disc.gc_cmpr_skin_edge(512, 128), 128)


if __name__ == "__main__":
	unittest.main()
