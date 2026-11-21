"""The static wall self test must not count records without an edict as mismatches (caen records 102/103 sit next to 6)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StaticWallSelfTest(unittest.TestCase):
    def test_edictless_records_are_left_out_of_the_whole_path_comparison(self):
        game = (ROOT / "engine/server/sv_game.c").read_text()
        world = (ROOT / "engine/server/sv_world.c").read_text()
        # remembered while the per-record loop finds no edict
        self.assertRegex(game, r"gc_sw_noedict\[i\] = 1;")
        # switched off in SV_Move for the whole-path comparison and switched back on afterwards
        self.assertIn("SV_GCStaticWallSetSkip( i, true );", game)
        self.assertIn("SV_GCStaticWallSetSkip( i, false );", game)
        self.assertLess(game.index("SV_GCStaticWallSetSkip( i, true );"), game.index("SV_GCStaticWallSetSkip( i, false );"))
        # no rays are started around such a record
        self.assertIn("if( i < 256 && gc_sw_noedict[i] )\n\t\t\t\tcontinue;", game)
        # the skip only matters when set; normal play never sets it
        self.assertIn("if( gc_static_wall_skip[i] )\n\t\t\tcontinue;", world)
        self.assertEqual(len(re.findall(r"SV_GCStaticWallSetSkip\(", game)), 2)

    def test_skip_flags_are_cleared_with_the_records(self):
        world = (ROOT / "engine/server/sv_world.c").read_text()
        body = world[world.index("void SV_GCStaticWallsClear"):][:300]
        self.assertIn("memset( gc_static_wall_skip", body)


if __name__ == "__main__":
    unittest.main()
