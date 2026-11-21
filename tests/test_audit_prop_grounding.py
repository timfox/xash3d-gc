import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_gamecube_prop_pack as fixtures  # noqa: E402


def load(name, relative):
	spec = importlib.util.spec_from_file_location(name, ROOT / relative)
	module = importlib.util.module_from_spec(spec)
	sys.modules[name] = module
	spec.loader.exec_module(module)
	return module


class PropGroundingAudit(unittest.TestCase):
	def test_every_prop_of_the_fixture_gets_a_row(self):
		audit = load("audit_prop_grounding", "scripts/audit_prop_grounding.py")
		with tempfile.TemporaryDirectory() as tmp_name:
			tmp = Path(tmp_name)
			(tmp / "models/mapmodels").mkdir(parents=True)
			(tmp / "models/mapmodels/hedgehog.mdl").write_bytes(fixtures.make_mdl())
			bsp = tmp / "dod_test.bsp"
			bsp.write_bytes(fixtures.make_bsp(fixtures.ENTS))
			# One node: the plane z = -40, empty above, solid below.
			floor = audit.bp.FloorLight.__new__(audit.bp.FloorLight)
			floor.bsp = types.SimpleNamespace(
				planes=[(0.0, 0.0, 1.0, -40.0)],
				nodes=[(0, -2, -1)],
				leaves=[(-2,), (-1,)],
			)
			rows = audit.audit(bsp, [tmp], log=lambda *_: None, floor=floor)
		self.assertTrue(rows)
		measured = [row for row in rows if row["gap_all"] != ""]
		self.assertTrue(measured)
		for row in measured:
			# the gap is the lowest vertex against the plane at -40
			self.assertAlmostEqual(row["gap_all"], row["low_z"] + 40.0, delta=0.2)
			self.assertEqual(row["gap_all"], row["gap_world"])


if __name__ == "__main__":
	unittest.main()
