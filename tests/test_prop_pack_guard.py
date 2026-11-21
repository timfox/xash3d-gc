import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_disc():
    sys.path.insert(0, str(ROOT / "scripts"))
    spec = importlib.util.spec_from_file_location("disc_under_test", ROOT / "scripts/build-gamecube-disc.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["disc_under_test"] = module
    spec.loader.exec_module(module)
    return module


class PropPackGuardTests(unittest.TestCase):
    def setUp(self):
        self.disc = load_disc()
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        (base / "src/maps").mkdir(parents=True)
        (base / "src/maps/dod_fake.bsp").write_bytes(b"\0" * 64)
        self.src, self.out, self.cache = base / "src", base / "out", base / "cache"

    def tearDown(self):
        self.tmp.cleanup()

    def test_failed_bake_stops_the_build(self):
        import bake_props_goldsrc

        with mock.patch.object(bake_props_goldsrc, "bake", side_effect=RuntimeError("boom")):
            with mock.patch.dict(os.environ, {"DODGC_ALLOW_MISSING_PROPPACK": ""}):
                with self.assertRaises(SystemExit) as ctx:
                    self.disc.build_prop_packs(self.src, self.out, self.cache)
        self.assertIn("dod_fake", str(ctx.exception))

    def test_override_ships_without_the_pack(self):
        import bake_props_goldsrc

        with mock.patch.object(bake_props_goldsrc, "bake", side_effect=RuntimeError("boom")):
            with mock.patch.dict(os.environ, {"DODGC_ALLOW_MISSING_PROPPACK": "1"}):
                self.assertEqual(self.disc.build_prop_packs(self.src, self.out, self.cache), [])


if __name__ == "__main__":
    unittest.main()
