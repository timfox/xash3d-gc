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


if __name__ == "__main__":
	unittest.main()
