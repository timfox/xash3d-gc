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
             str(ROOT / "tests/mcol_test.c"), str(ROOT / "engine/common/mcol.c"), "-o", str(cls.binary), "-lm"],
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

    def test_terrain_heightfield(self):
        # sample map terrain: a flat patch at z 0 (x, y -1024..-512) and a 1-in-8 slope east of it
        flat, slope = self.trace(FIX / "terrain.mcol", f"-768 -768 400 -768 -768 -200 {PLAYER}",
                                 f"-256 -768 400 -256 -768 -200 {PLAYER}")
        self.assertAlmostEqual(400 - 600 * flat[0] - 36, 0.0, delta=0.1)
        self.assertEqual(flat[1], (0.0, 0.0, 1.0))
        # the box's uphill edge (16 units east) carries the feet
        self.assertAlmostEqual(400 - 600 * slope[0] - 36, (256 + 16) / 8.0, delta=0.1)
        # a box moving level above the slope's highest point passes; just below it, the slope stops it
        over, into = self.trace(FIX / "terrain.mcol", f"-600 -768 90 -100 -768 90 {PLAYER}",
                                f"-600 -768 40 -100 -768 40 {PLAYER}")
        self.assertEqual(over[0], 1.0)
        self.assertLess(into[0], 1.0)
        self.assertGreater(into[1][2], 0.9)

    def test_patch_triangles(self):
        # sample map's solid hump patch: 256 long in x from 300, z 0 -> 64 -> 0; top at x = 428
        top, nonsolid = self.trace(FIX / "terrain.mcol", f"428 364 400 428 364 -200 {PLAYER}",
                                   f"664 364 400 664 364 -200 {PLAYER}")
        self.assertAlmostEqual(400 - 600 * top[0] - 36, 64.0, delta=1.0)
        self.assertGreater(top[1][2], 0.95)
        self.assertEqual(nonsolid[0], 1.0)  # the vines patch is not solid

    def test_malformed_files_are_rejected(self):
        data = (FIX / "box.mcol").read_bytes()
        cases = {
            "truncated": data[:-4],
            "magic": b"XCOL" + data[4:],
            "version": data[:4] + (1).to_bytes(4, "little") + data[8:],
        }
        # the ramp's one stored side (its slope; axial planes come from the bounds)
        ramp = (FIX / "ramp.mcol").read_bytes()
        nplanes = int.from_bytes(ramp[8:12], "little")
        nbrushes = int.from_bytes(ramp[12:16], "little")
        self.assertEqual(int.from_bytes(ramp[16:20], "little"), 1)
        side0 = 44 + 16 * nplanes + 32 * nbrushes
        cases["side"] = ramp[:side0] + (nplanes).to_bytes(4, "little") + ramp[side0 + 4:]
        # the last node of the file (a triangle leaf) claiming more triangles than exist
        ter = (FIX / "terrain.mcol").read_bytes()
        ntris = int.from_bytes(ter[36:40], "little")
        self.assertGreater(ntris, 0)
        cases["trinode"] = ter[:-4] + (ntris + 1).to_bytes(4, "little")
        # a triangle vertex index past the last vertex: tris sit just before the trinodes
        ntrinodes = int.from_bytes(ter[40:44], "little")
        nverts = int.from_bytes(ter[32:36], "little")
        tri0 = len(ter) - 32 * ntrinodes - 12 * ntris
        cases["trivert"] = ter[:tri0] + nverts.to_bytes(4, "little") + ter[tri0 + 4:]
        with tempfile.TemporaryDirectory() as tmp:
            for name, blob in cases.items():
                p = Path(tmp) / f"{name}.mcol"
                p.write_bytes(blob)
                r = subprocess.run([str(self.binary), str(p)], input="", capture_output=True)
                self.assertEqual(r.returncode, 2, name)


if __name__ == "__main__":
    unittest.main()
