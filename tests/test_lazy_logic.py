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


class LazyLogicLifecycle(LazyLogic):
    """Restart and level change: the table belongs to one map load; nothing leaks into the next."""

    def test_report_entries_and_failure_counter(self):
        L = self.L
        L.LazyLogic_Failed.restype = ctypes.c_int
        L.LazyLogic_Entry.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                                      ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
        h = L.LazyLogic_Hash(b"x")
        L.LazyLogic_Add(h, 100, 7)
        L.LazyLogic_Add(L.LazyLogic_Hash(b"y"), 200, 9)
        fail = CB(lambda _c, _o, _l: -1)
        self.assertEqual(L.LazyLogic_Materialize(h, fail, None), 0)
        self.assertEqual(L.LazyLogic_Failed(), 1)
        ok = CB(lambda _c, _o, _l: 1)
        self.assertEqual(L.LazyLogic_Materialize(h, ok, None), 1)
        L.LazyLogic_NoteEdict(100, 77)
        ofs, ln, sp, ed = ctypes.c_uint32(), ctypes.c_uint32(), ctypes.c_int(), ctypes.c_int()
        self.assertEqual(L.LazyLogic_Entry(0, ofs, ln, sp, ed), 1)
        self.assertEqual((ofs.value, ln.value, sp.value, ed.value), (100, 7, 1, 77))
        self.assertEqual(L.LazyLogic_Entry(1, ofs, ln, sp, ed), 1)
        self.assertEqual((sp.value, ed.value), (0, 0), "an entry nobody asked for is still pending, no edict")
        self.assertEqual(L.LazyLogic_Entry(2, ofs, ln, sp, ed), 0)

    def test_reset_for_the_next_map_load_leaves_nothing_behind(self):
        L = self.L
        names = [b"a", b"b", b"c"]
        for i, nm in enumerate(names):
            L.LazyLogic_Add(L.LazyLogic_Hash(nm), i * 10, 4)
        cb = CB(lambda _c, _o, _l: 1)
        L.LazyLogic_Materialize(L.LazyLogic_Hash(b"a"), cb, None)
        L.LazyLogic_NoteEdict(0, 12)
        self.assertEqual((L.LazyLogic_Total(), L.LazyLogic_Spawned(), L.LazyLogic_Pending()), (3, 1, 2))
        # level change / restart: SV_LoadFromFile resets before the new lump is scanned
        L.LazyLogic_Reset()
        self.assertEqual((L.LazyLogic_Total(), L.LazyLogic_Spawned(), L.LazyLogic_Pending(), L.LazyLogic_Failed()),
                         (0, 0, 0, 0))
        self.assertEqual(L.LazyLogic_Materialize(L.LazyLogic_Hash(b"b"), cb, None), 0, "old names do not spawn in the new map")
        # the same lump again (restart of the same map): everything is deferred afresh, including what was spawned before
        for i, nm in enumerate(names):
            L.LazyLogic_Add(L.LazyLogic_Hash(nm), i * 10, 4)
        self.assertEqual(L.LazyLogic_Materialize(L.LazyLogic_Hash(b"a"), cb, None), 1, "spawns again after a reload")

    def test_a_fire_once_entity_that_freed_itself_is_not_respawned(self):
        # Spawn, the DLL removes the entity after it fired (fire-once relay): the entry stays spawned, so a later lookup by the
        # same name finds nothing, exactly as in the all-spawned path where that edict is gone for the rest of the map.
        L = self.L
        h = L.LazyLogic_Hash(b"game_playerjoin")
        L.LazyLogic_Add(h, 0, 4)
        calls = []
        cb = CB(lambda _c, o, _l: calls.append(o) or 1)
        self.assertEqual(L.LazyLogic_Materialize(h, cb, None), 1)
        self.assertEqual(L.LazyLogic_Materialize(h, cb, None), 0)
        self.assertEqual(calls, [0])

    def test_round_restart_does_not_touch_the_table(self):
        # DoD's round restart (dod_runtime.cpp) respawns players and resets objectives; DodGC_TrimEdicts only frees gibs and
        # weapon boxes. The engine reloads the entity lump (SV_LoadFromFile) only on a map load. So nothing in the lazy
        # table changes at a round restart: spawned entities live on, pending ones stay pending.
        src = (ROOT / "engine/server/sv_game.c").read_text()
        self.assertEqual(src.count("LazyLogic_Reset();"), 1)
        self.assertIn('SV_GCLazyReport( "previous map table"', src)
        self.assertIn("gc_lazy_report", src)
        self.assertIn('Cvar_FindVar( "sv_gc_lazy_log" )', src)


class LazyEffects(unittest.TestCase):
    def test_named_effects_are_eligible_and_lights_are_not(self):
        src = (ROOT / "engine/server/sv_game.c").read_text()
        body = src[src.index("static qboolean SV_GCLazyEligibleClass"):][:1500]
        for cls in ("trigger_relay", "trigger_changetarget", "info_target", "ambient_generic", "env_sprite", "env_shake",
                    "env_shooter"):
            self.assertIn(f'"{cls}"', body)
        self.assertNotIn('"light"', body)       # a named light sets its light style in Spawn: must happen at load
        self.assertNotIn('"light_spot"', body)
        # only entities with a targetname can be looked up, so only they are deferred
        self.assertIn("ent->v.targetname && gc_blk && SV_GCLazyEligibleClass", src)

    def test_class_counters_are_in_the_report_and_reset_per_map(self):
        src = (ROOT / "engine/server/sv_game.c").read_text()
        self.assertIn("spawned by class: relay %d changetarget %d", src)
        self.assertIn("memset( gc_lazy_class_spawned, 0", src)
