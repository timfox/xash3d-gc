"""The lean route's own TEAM / HP / AMMO bars must stay behind the gc_hud_retail check (no second ammo readout)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CapFacesHudGateTests(unittest.TestCase):
    def test_lean_bars_are_gated(self):
        src = (ROOT / "engine/platform/gamecube/vid_gamecube.c").read_text()
        gate = 'Cvar_VariableValue( "gc_hud_retail" ) == 0.0f'
        self.assertEqual(src.count('"AMMO %d"'), 1)
        gate_at = src.index(gate)
        self.assertLess(gate_at, src.index('"AMMO %d"'))
        # the gate opens a block that contains the AMMO / HP text and closes before the combat-ready code
        block = src[gate_at:src.index("if( !gc_team_fire_logged )", gate_at)]
        self.assertIn('"AMMO %d"', block)
        self.assertIn('"HP 100"', block)
        self.assertIn('"TEAM: %s"', block)

    def test_flag_is_registered_by_the_engine(self):
        # registered at engine start so the client DLL's lookup finds it (no "non-existent cvar" warning at boot)
        src = (ROOT / "engine/client/input/in_osk.c").read_text()
        self.assertIn('CVAR_DEFINE_AUTO( gc_hud_retail, "0"', src)
        self.assertIn("Cvar_RegisterVariable( &gc_hud_retail )", src)


if __name__ == "__main__":
    unittest.main()
