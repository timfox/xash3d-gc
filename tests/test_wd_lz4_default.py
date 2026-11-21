import importlib.util
import sys
import os
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location("disc_default_test", ROOT / "scripts/build-gamecube-disc.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


class WdLz4Default(unittest.TestCase):
    def test_on_unless_zero(self):
        mod = load()
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("DODGC_WD_LZ4", None)
            mod.apply_wd_lz4_default()
            self.assertEqual(os.environ["DODGC_WD_LZ4"], "1")
        with mock.patch.dict(os.environ, {"DODGC_WD_LZ4": "0"}):
            mod.apply_wd_lz4_default()
            self.assertEqual(os.environ["DODGC_WD_LZ4"], "0")

    def test_baker_alone_stays_opt_in(self):
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        import bake_worlddl_goldsrc as bake
        src = (ROOT / "scripts/bake_worlddl_goldsrc.py").read_text()
        self.assertIn('os.environ.get("DODGC_WD_LZ4", "") == "1"', src)


if __name__ == "__main__":
    unittest.main()
