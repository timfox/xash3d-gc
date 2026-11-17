"""Host tests for the GameCube full-resolution HUD cells."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GC = ROOT / "engine/platform/gamecube"


class GameCubeHudHiresTests(unittest.TestCase):
    def test_hud_hires(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "hud_hires"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(GC),
                 str(GC / "aram_core.c"), str(GC / "hud_hires.c"),
                 str(ROOT / "tests/fixtures/gamecube_hud_hires_test.c"),
                 "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("hud hires ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
