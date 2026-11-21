"""Host test for engine/common/mcol.c, the .mcol brush sweep for converted MOHAA maps.

The fixtures come from timfox/mohaa-gc tools/mesh_collision.py; the expected values are
what its reference trace_box() returns for the same traces.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIX = ROOT / "tests/fixtures/mcol"
PLAYER = "-16 -16 -36 16 16 36"
EPS = 0.03125


class McolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "mcol-test"
        subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             str(ROOT / "tests/mcol_test.c"), str(ROOT / "engine/common/mcol.c"), "-o", str(cls.binary)],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def trace(self, fixture, *lines):
        out = subprocess.run([str(self.binary), str(fixture)], input="\n".join(lines) + "\n",
                             capture_output=True, text=True, check=True).stdout.split("\n")
        rows = []
        for row in out[:len(lines)]:
            f, nx, ny, nz, ss, al = row.split()
            rows.append((float(f), (float(nx), float(ny), float(nz)), int(ss), int(al)))
        return rows

    def test_box_hit_and_miss(self):
        hit, miss = self.trace(FIX / "box.mcol", f"-100 32 32 100 32 32 {PLAYER}", f"-100 200 32 100 200 32 {PLAYER}")
        # box face at x=0 and player half-width 16: stops at x = -16 - epsilon
        self.assertAlmostEqual(-100 + 200 * hit[0], -16 - EPS, places=2)
        self.assertEqual(hit[1], (-1.0, 0.0, 0.0))
        self.assertEqual(hit[2:], (0, 0))
        self.assertEqual(miss[0], 1.0)

    def test_inside_is_allsolid(self):
        (inside,) = self.trace(FIX / "box.mcol", f"32 32 32 32 32 32 {PLAYER}")
        self.assertEqual((inside[0], inside[2], inside[3]), (0.0, 1, 1))
        (leaving,) = self.trace(FIX / "box.mcol", f"32 32 32 32 32 300 {PLAYER}")
        self.assertEqual((leaving[2], leaving[3]), (1, 0))

    def test_ramp_is_exact(self):
        (drop,) = self.trace(FIX / "ramp.mcol", f"128 0 400 128 0 0 {PLAYER}")
        # the box spans x 112..144, so the ramp's highest point under it carries the feet at 144
        self.assertAlmostEqual(400 - 400 * drop[0] - 36, 144, delta=0.2)
        self.assertAlmostEqual(drop[1][2], 2 ** -0.5, places=4)

    def test_bvh_many_brushes(self):
        rows = self.trace(FIX / "many.mcol", *[f"{k * 100 + 16} 16 300 {k * 100 + 16} 16 0 {PLAYER}" for k in (0, 17, 49)],
                          f"1766 16 300 1766 16 0 {PLAYER}")
        for r in rows[:3]:
            self.assertLess(r[0], 1.0)
        self.assertEqual(rows[3][0], 1.0)

    def test_malformed_files_are_rejected(self):
        data = (FIX / "box.mcol").read_bytes()
        cases = {
            "truncated": data[:-4],
            "magic": b"XCOL" + data[4:],
            "version": data[:4] + (2).to_bytes(4, "little") + data[8:],
        }
        nplanes = int.from_bytes(data[8:12], "little")
        nbrushes = int.from_bytes(data[12:16], "little")
        side0 = 24 + 16 * nplanes + 32 * nbrushes
        cases["side"] = data[:side0] + (nplanes).to_bytes(4, "little") + data[side0 + 4:]
        with tempfile.TemporaryDirectory() as tmp:
            for name, blob in cases.items():
                p = Path(tmp) / f"{name}.mcol"
                p.write_bytes(blob)
                r = subprocess.run([str(self.binary), str(p)], input="", capture_output=True)
                self.assertEqual(r.returncode, 2, name)


if __name__ == "__main__":
    unittest.main()
