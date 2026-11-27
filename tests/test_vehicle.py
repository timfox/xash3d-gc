import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMMON = ROOT / "engine/common"
SOURCES = ["vehicle.c", "vehicle_car.c", "vehicle_tank.c", "vehicle_flight.c"]


class VehicleCoreTest(unittest.TestCase):
    """Car, tank and flight physics plus the chase camera, on a fake world."""

    def _build_and_run(self, extra):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "vehicle_test"
            subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", *extra,
                 "-I", str(COMMON), str(ROOT / "tests/fixtures/vehicle_test.c"),
                 *[str(COMMON / s) for s in SOURCES], "-o", str(binary), "-lm"],
                check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_vehicle_core_sanitized(self):
        self._build_and_run(["-O0", "-fsanitize=address,undefined"])

    def test_vehicle_core_optimized(self):
        # -O2 with single-precision contraction, the way the DOL is built
        self._build_and_run(["-O2", "-ffp-contract=fast"])


if __name__ == "__main__":
    unittest.main()
