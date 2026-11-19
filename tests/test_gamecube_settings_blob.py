"""Host test: the saved-settings blob round-trips and rejects damage."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GC = ROOT / "engine/platform/gamecube"


class SettingsBlobTests(unittest.TestCase):
    def test_blob(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "settings_blob"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(GC),
                 str(GC / "settings_blob.c"), str(ROOT / "tests/fixtures/gamecube_settings_blob_test.c"),
                 "-o", str(binary)],
                check=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("settings blob ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
