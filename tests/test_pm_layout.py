import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PmLayoutTests(unittest.TestCase):
    def test_layout_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "pm-layout"
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 str(ROOT / "tests/pm_layout_test.c"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
