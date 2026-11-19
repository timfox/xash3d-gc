"""Host tests for the GameCube hi-res text glyph queue."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GC = ROOT / "engine/platform/gamecube"


class HiTextQueueTests(unittest.TestCase):
    def test_queue(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "hitext_queue"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(GC),
                 str(ROOT / "tests/fixtures/gamecube_hitext_queue_test.c"),
                 "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("hitext queue ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
