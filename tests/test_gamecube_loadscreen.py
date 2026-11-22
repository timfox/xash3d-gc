"""Host test: a full-screen loading screen file (gc_loadscreen.h) parses, and its bar plate cuts out right.

The file is written here the way mohaa-gc tools/mohaa_loading.py writes MOHAA's retail screens: a GCMP image
640 x (480 + fill rows) whose reserved header bytes carry the bar layout."""
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GC = ROOT / "engine/platform/gamecube"


def loadscreen_file(bar=(194, 456, 252, 8), plate=(24, 56, 32, 3), fill_rows=8, seed=1):
    rnd = random.Random(seed)
    body = bytes(rnd.getrandbits(8) for _ in range(640 * (480 + fill_rows) // 2))
    head = b"GCMP" + struct.pack(">HHBBH", 640, 480 + fill_rows, 0, 1, 0) + bytes(8)
    head += struct.pack(">HHHHBBBB", *bar, *plate)
    assert len(head) == 32
    return head + body


class LoadScreenTests(unittest.TestCase):
    def test_parse_and_cut_plate(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "m1l1.cmp").write_bytes(loadscreen_file())
            binary = tmp / "loadscreen_test"
            subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(GC),
                            str(ROOT / "tests/fixtures/gamecube_loadscreen_test.c"), "-o", str(binary)], check=True)
            out = subprocess.run([str(binary), str(tmp / "m1l1.cmp"), "194", "456", "252", "8", "24", "56", "32", "3"],
                                 check=True, capture_output=True, text=True)
            self.assertIn("loadscreen ok", out.stdout)


if __name__ == "__main__":
    unittest.main()
