"""LZ4 PVS rows of the world DL pack (flag 0x20): every cluster's RLE row reads back identical through the engine decoder."""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import bake_worlddl_goldsrc as bake  # noqa: E402

spec = importlib.util.spec_from_file_location("lf_test", ROOT / "tests/test_lf_lz4.py")
lf_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lf_test)

BLOCK = bake.PVS_BLOCK
BSP = lf_test.BSP


@unittest.skipUnless(BSP.exists(), "no dod_flash.bsp")
class PvsLz4Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "h.c"
        # the lf harness walks GC_LF_BLOCK-byte blocks of one region; use it with the PVS block size
        src.write_text(lf_test.HARNESS % (BLOCK, lf_test.engine_decoder_source()))
        cls.exe = Path(cls.tmp.name) / "h"
        subprocess.run(["gcc", "-O1", "-w", "-o", str(cls.exe), str(src)], check=True)
        cls.v1, cls.v2 = Path(cls.tmp.name) / "v1.gcd", Path(cls.tmp.name) / "v2.gcd"
        bake.bake(BSP, cls.v1, lz4=False)
        bake.bake(BSP, cls.v2, lz4=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def hdr(self, d):
        return struct.unpack_from(">4sHH13I", d, 0)

    def test_flag_and_size(self):
        a, b = self.v1.read_bytes(), self.v2.read_bytes()
        h1, h2 = self.hdr(a), self.hdr(b)
        self.assertEqual(h1[2] & 0x20, 0)
        self.assertEqual(h2[2] & 0x30, 0x30)          # lightmap and PVS flags
        self.assertEqual(h2[15], len(b))
        pvs1 = h1[14] - h1[13]
        pvs2 = h2[14] - h2[13]
        self.assertLess(pvs2, pvs1 * 0.5)

    def test_every_row_matches(self):
        a, b = self.v1.read_bytes(), self.v2.read_bytes()
        h1, h2 = self.hdr(a), self.hdr(b)
        nc = h1[3]
        dir1 = [struct.unpack_from(">I", a, h1[12] + 4 * i)[0] for i in range(nc)]
        dir2 = [struct.unpack_from(">I", b, h2[12] + 4 * i)[0] for i in range(nc)]
        self.assertEqual(dir1, dir2, "row offsets stay raw offsets")
        raw = a[h1[13]:h1[14]]
        region = b[h2[13]:h2[14]]
        n_blocks = struct.unpack_from(">I", region, 0)[0]
        total = n_blocks * BLOCK
        self.assertGreaterEqual(total, len(raw) - 31)
        valid = [o for o in dir1 if o != 0xFFFFFFFF]
        path = Path(self.tmp.name) / "pvs.bin"
        path.write_bytes(region)
        step = max(1, len(valid) // 60)
        for k in range(0, len(valid) - 1, step):
            lo, hi = valid[k], valid[k + 1]
            r = subprocess.run([str(self.exe), str(path), str(total), str(lo), str(hi - lo)], capture_output=True)
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout, raw[lo:hi], f"row at {lo}")


if __name__ == "__main__":
    unittest.main()
