"""Host test: GCMP sidecar baked by the disc builder round-trips through gcmp.h."""
import importlib.util
from pathlib import Path
import shutil
import struct
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GC = ROOT / "engine/platform/gamecube"


def load_builder():
    spec = importlib.util.spec_from_file_location("gc_disc", ROOT / "scripts/build-gamecube-disc.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gc_disc"] = mod
    spec.loader.exec_module(mod)
    return mod


class GcmpTests(unittest.TestCase):
    def test_header_layout(self):
        try:
            builder = load_builder()
        except ImportError:
            self.skipTest("builder deps unavailable")
        head = builder.gcmp_header(160, 120, b"source-bytes")
        self.assertEqual(len(head), 32)
        self.assertEqual(head[:4], b"GCMP")
        w, h, fmt, mips, ver, size, crc = struct.unpack(">HHBBHII", head[4:20])
        self.assertEqual((w, h, fmt, mips, ver, size), (160, 120, 0, 1, builder.GCMP_VERSION, 12))
        self.assertEqual(head[20:], bytes(12))

    def test_bake_and_decode(self):
        try:
            import numpy as np
            from PIL import Image
            builder = load_builder()
        except ImportError:
            self.skipTest("numpy/Pillow unavailable")
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            # smooth gradient plus a hard edge, 320x240 like loading.tga
            yy, xx = np.mgrid[0:240, 0:320]
            img = np.stack([xx * 255 // 319, yy * 255 // 239, (xx + yy) % 256], -1).astype(np.uint8)
            img[:, 160:] = 255 - img[:, 160:]
            Image.fromarray(img).save(tmp / "loading.tga")
            self.assertTrue(builder.bake_gcmp_sidecar(tmp / "loading.tga", tmp / "loading.cmp", 160, 120))
            self.assertFalse(builder.bake_gcmp_sidecar(tmp / "loading.tga", tmp / "bad.cmp", 161, 120))
            self.assertFalse((tmp / "bad.cmp").exists())
            ref = np.asarray(Image.fromarray(img).convert("RGB").resize((160, 120), Image.LANCZOS)).astype(np.int32)
            r565 = ((ref[..., 0] >> 3) << 11) | ((ref[..., 1] >> 2) << 5) | (ref[..., 2] >> 3)
            (tmp / "ref.raw").write_bytes(r565.astype("<u2").tobytes())
            binary = tmp / "gcmp_test"
            subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(GC),
                            str(ROOT / "tests/fixtures/gamecube_gcmp_test.c"), "-o", str(binary)], check=True)
            out = subprocess.run([str(binary), str(tmp / "loading.cmp"), str(tmp / "ref.raw")],
                                 check=True, capture_output=True, text=True)
            self.assertIn("gcmp ok", out.stdout)


if __name__ == "__main__":
    unittest.main()
