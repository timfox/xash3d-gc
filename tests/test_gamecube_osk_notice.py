"""Host test: the on-screen notice panel stays inside the title-safe area."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OskNoticeTests(unittest.TestCase):
    def test_notice_rect(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "osk_notice"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "engine/client/input"),
                 str(ROOT / "tests/fixtures/gamecube_osk_notice_test.c"), "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("osk notice ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
