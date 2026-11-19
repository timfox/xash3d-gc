"""Host test: the hi-res text atlas carries the eight controller button glyphs (original art)."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FONT = Path("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf")


class HitextButtonTests(unittest.TestCase):
    def test_button_glyphs(self):
        try:
            import PIL  # noqa: F401
        except ImportError:
            self.skipTest("Pillow unavailable")
        if not FONT.exists():
            self.skipTest("Noto Sans unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "gc_hitext.bin"
            subprocess.run([sys.executable, str(ROOT / "scripts/bake-hitext.py"), "--out", str(out)],
                           check=True, capture_output=True)
            data = out.read_bytes()
        magic, ver, tw, th, line_h, first, count = struct.unpack(">4sHHHHHH", data[:16])
        self.assertEqual(magic, b"HTXT")
        self.assertEqual((first, count), (32, 104))  # 32..135, contiguous
        self.assertLessEqual(count, 104)  # the engine's glyph table size
        self.assertEqual(len(data), 16 + count * 14 + tw * th // 2)
        glyphs = [struct.unpack(">HHHHhhH", data[16 + i * 14:30 + i * 14]) for i in range(count)]
        for code in range(128, 136):
            x, y, w, h, xoff, yoff, adv = glyphs[code - first]
            self.assertGreater(w, 8, code)
            self.assertGreater(h, 8, code)
            self.assertLessEqual(x + w, tw)
            self.assertLessEqual(y + h, th)
            self.assertGreater(adv, w, code)
        # DEL stays an empty cell so ASCII indices do not move
        self.assertEqual(glyphs[127 - first][2:4], (0, 0))


if __name__ == "__main__":
    unittest.main()
