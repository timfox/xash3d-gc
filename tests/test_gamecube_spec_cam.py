import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GC = ROOT / "engine/platform/gamecube"


class SpectatorCameraTest(unittest.TestCase):
    def test_spec_cam(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "spec_cam"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(GC),
                 str(ROOT / "tests/fixtures/spec_cam_test.c"), "-o", str(binary), "-lm"],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
