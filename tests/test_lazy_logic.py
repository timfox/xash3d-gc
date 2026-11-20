"""sv_lazy_logic: deferred logic entities spawn on first lookup by name and fire in the same order as if spawned at load."""
import ctypes
import random
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32)


class LazyLogic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        so = Path(cls.tmp.name) / "lazy.so"
        subprocess.run(["gcc", "-shared", "-fPIC", "-O1", "-o", str(so), str(ROOT / "engine/server/sv_lazy_logic.c"),
                        f"-I{ROOT / 'engine/server'}"], check=True)
        L = ctypes.CDLL(str(so))
        L.LazyLogic_Hash.restype = ctypes.c_uint32
        L.LazyLogic_Hash.argtypes = [ctypes.c_char_p]
        L.LazyLogic_Add.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
        L.LazyLogic_Materialize.argtypes = [ctypes.c_uint32, CB, ctypes.c_void_p]
        L.LazyLogic_Materialize.restype = ctypes.c_int
        cls.L = L

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.L.LazyLogic_Reset()

    # --- a tiny model of the engine: edicts in spawn order, FireTargets scans them ascending -------------------------
    def make_lump(self, rnd, n):
        names = [f"n{i}" for i in range(max(3, n // 2))]
        lump = []
        for i in range(n):
            nm = rnd.choice(names)
            # a target always points to a later name index so chains are acyclic
            k = names.index(nm)
            tgt = rnd.choice(names[k + 1:]) if k + 1 < len(names) and rnd.random() < 0.8 else None
            lump.append({"i": i, "name": nm, "target": tgt, "delay": rnd.choice([0, 0, 1, 2])})
        return lump

    def fire(self, edicts, name, t, out, lazy_hook=None):
        """FireTargets: every edict with this targetname, ascending, then what each one targets (after its delay)."""
        if lazy_hook:
            lazy_hook(name)
        for ed in list(edicts):                      # ascending edict index
            if ed["name"] == name:
                out.append((t + ed["delay"], ed["i"]))
                if ed["target"]:
                    self.fire(edicts, ed["target"], t + ed["delay"], out, lazy_hook)

    def run_full(self, lump, entry):
        out = []
        self.fire(list(lump), entry, 0, out)
        return sorted(out, key=lambda e: e[0])  # stable: ties keep fire order

    def run_lazy(self, lump, entry, keep):
        L = self.L
        L.LazyLogic_Reset()
        edicts = list(lump[:keep])
        by_ofs = {}
        for ed in lump[keep:]:
            by_ofs[ed["i"] * 100] = ed
            L.LazyLogic_Add(L.LazyLogic_Hash(ed["name"].encode()), ed["i"] * 100, 50)

        def spawn(_ctx, ofs, _len):
            ed = by_ofs[ofs]
            edicts.append(ed)
            return 1

        cb = CB(spawn)
        hook = lambda name: L.LazyLogic_Materialize(L.LazyLogic_Hash(name.encode()), cb, None)
        out = []
        self.fire(edicts, entry, 0, out, hook)
        return sorted(out, key=lambda e: e[0]), len(edicts) - keep

    def test_fire_order_matches_the_all_spawned_path(self):
        rnd = random.Random(2026)
        checked = 0
        for _ in range(300):
            n = rnd.randrange(6, 40)
            lump = self.make_lump(rnd, n)
            keep = rnd.randrange(0, n + 1)
            entry = rnd.choice([e["name"] for e in lump])
            full = self.run_full(lump, entry)
            lazy, spawned_late = self.run_lazy(lump, entry, keep)
            self.assertEqual(lazy, full, (n, keep, entry))
            checked += 1
        self.assertEqual(checked, 300)

    def test_only_what_is_looked_up_spawns(self):
        L = self.L
        for i, name in enumerate(["a", "b", "c", "a"]):
            L.LazyLogic_Add(L.LazyLogic_Hash(name.encode()), i * 10, 5)
        seen = []
        cb = CB(lambda _c, ofs, _l: seen.append(ofs) or 1)
        self.assertEqual(L.LazyLogic_Materialize(L.LazyLogic_Hash(b"a"), cb, None), 2)
        self.assertEqual(seen, [0, 30], "same-name entries spawn together, in lump order")
        self.assertEqual(L.LazyLogic_Pending(), 2)
        self.assertEqual(L.LazyLogic_Materialize(L.LazyLogic_Hash(b"a"), cb, None), 0, "spawned once only")
        self.assertEqual(L.LazyLogic_Materialize(L.LazyLogic_Hash(b"zzz"), cb, None), 0)
        self.assertEqual(L.LazyLogic_Spawned(), 2)

    def test_collision_and_no_room_stay_pending(self):
        L = self.L
        h = L.LazyLogic_Hash(b"x")
        L.LazyLogic_Add(h, 0, 5)     # not really "x": the callback says so (hash collision)
        L.LazyLogic_Add(h, 10, 5)    # real, but no edict free this time
        L.LazyLogic_Add(h, 20, 5)    # real
        answers = {0: 0, 10: -1, 20: 1}
        cb = CB(lambda _c, ofs, _l: answers[ofs])
        self.assertEqual(L.LazyLogic_Materialize(h, cb, None), 1)
        self.assertEqual(L.LazyLogic_Pending(), 2)
        answers[10] = 1
        self.assertEqual(L.LazyLogic_Materialize(h, cb, None), 1, "a later lookup retries the one that had no room")

    def test_no_reentry_while_spawning(self):
        L = self.L
        h = L.LazyLogic_Hash(b"r")
        L.LazyLogic_Add(h, 0, 5)
        calls = []

        def spawn(_c, ofs, _l):
            calls.append(ofs)
            # the entity's own Spawn looks its name up again
            L.LazyLogic_Materialize(h, cbref[0], None)
            return 1
        cbref = [CB(spawn)]
        self.assertEqual(L.LazyLogic_Materialize(h, cbref[0], None), 1)
        self.assertEqual(calls, [0])

    def test_table_limit(self):
        L = self.L
        for i in range(192):
            self.assertGreaterEqual(L.LazyLogic_Add(5, i, 1), 0)
        self.assertEqual(L.LazyLogic_Add(5, 999, 1), -1)

    def test_engine_hooks_are_wired_and_default_off(self):
        src = (ROOT / "engine/server/sv_game.c").read_text()
        self.assertIn('Cvar_FindVar( "sv_gc_lazy_logic" )', src)
        self.assertIn("return cv && cv->value != 0.0f;", src)       # off unless the cvar is set
        self.assertIn("gc_lazy_active && e == 0 && LazyLogic_Pending() > 0", src)
        self.assertIn("LazyLogic_Add( LazyLogic_Hash( tn )", src)


if __name__ == "__main__":
    unittest.main()
