"""Host tests for the cinematic camera path module (engine/client/cine_path.c)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "engine/client"


class CinePathTests(unittest.TestCase):
    def test_paths_parse_sample_and_stamp(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "cine"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(CLIENT),
                 str(CLIENT / "cine_path.c"), str(ROOT / "tests/fixtures/cine_path_test.c"),
                 "-lm", "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("cine ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
