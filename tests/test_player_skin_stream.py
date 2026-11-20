"""GC_PLAYER_SKIN_STREAM companions: 256 px CMPR texels plus a half-size low mip that equals the plain 128 px bake."""
import importlib.util
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOD = Path("/home/tim/snap/steam/common/.local/share/Steam/steamapps/common/Half-Life/dod/models/player")
SRC = DOD / "us-inf" / "us-infT.mdl"

spec = importlib.util.spec_from_file_location("disc_stream_test", ROOT / "scripts/build-gamecube-disc.py")
disc = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = disc
spec.loader.exec_module(disc)


def textures(blob: bytes):
    n = struct.unpack_from("<i", blob, 180)[0]
    base = struct.unpack_from("<i", blob, 184)[0]
    out = []
    for i in range(n):
        o = base + 80 * i
        name = blob[o:o + 64].split(b"\0")[0].decode()
        flags, w, h, idx = struct.unpack_from("<iiii", blob, o + 64)
        out.append((name, flags, w, h, idx))
    return out


@unittest.skipUnless(SRC.exists(), "no retail DoD player models")
class PlayerSkinStream(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plain = disc.gc_cmpr_skin_companion_bytes(SRC, max_edge=128, part_edge=64)
        cls.stream = disc.gc_cmpr_skin_companion_bytes(SRC, max_edge=256, part_edge=64, stream_edge=256)

    def test_flag_constants_agree_with_the_engine_header(self):
        text = (ROOT / "engine/studio.h").read_text()
        self.assertIn("#define STUDIO_NF_GC_CMPR_STREAM\t(1U<<27)", text)
        self.assertEqual(disc.STUDIO_NF_GC_CMPR_STREAM, 1 << 27)

    def test_streamed_bodies_keep_256_in_the_header_and_append_a_low_mip(self):
        plain = {t[0]: t for t in textures(self.plain)}
        n_streamed = 0
        for name, flags, w, h, idx in textures(self.stream):
            if flags & disc.STUDIO_NF_GC_CMPR_STREAM:
                n_streamed += 1
                self.assertEqual((w, h), (256, 256), name)
                self.assertEqual(idx % 32, 0)
                full = w * h // 2
                low = self.stream[idx + full:idx + full + (w // 2) * (h // 2) // 2]
                p = plain[name]
                self.assertEqual((p[2], p[3]), (128, 128), name)
                ref = self.plain[p[4]:p[4] + 128 * 128 // 2]
                self.assertEqual(len(low), len(ref))
                # The low mip is cut from the 256 px rgb, the plain bake from the source: the same picture, so most
                # CMPR blocks match exactly and no block is far off.
                same = sum(1 for k in range(0, len(ref), 8) if low[k:k + 8] == ref[k:k + 8])
                self.assertGreater(same, len(ref) // 8 * 0.5, f"{name}: low mip differs from the plain 128 px bake")
            else:
                self.assertEqual((w, h), (plain[name][2], plain[name][3]), name)   # helmets etc. unchanged
        self.assertGreater(n_streamed, 0)

    def test_engine_streamed_reader_sizes_match_the_layout(self):
        # Mod_GCStudioTexDataBytes (engine/common/mod_studio.c): the texture-by-texture reader of big companions reads
        # exactly this many bytes at each index, so it must end at the file end for the last texture and never cross the
        # next texture's start.
        def data_bytes(flags, w, h):
            if not flags & disc.STUDIO_NF_GC_CMPR:
                return w * h + 768
            n = ((w + 7) // 8) * ((h + 7) // 8) * 32
            if flags & disc.STUDIO_NF_GC_CMPR_STREAM:
                n += (((w >> 1) + 7) // 8) * (((h >> 1) + 7) // 8) * 32
            return n
        for blob in (self.stream, self.plain):
            tex = textures(blob)
            for k, (name, flags, w, h, idx) in enumerate(tex):
                end = idx + data_bytes(flags, w, h)
                self.assertLessEqual(end, len(blob), name)
                if k + 1 < len(tex):
                    self.assertLessEqual(end, tex[k + 1][4], name)
            last = tex[-1]
            self.assertEqual(last[4] + data_bytes(last[1], last[2], last[3]), len(blob))
        src = (ROOT / "engine/common/mod_studio.c").read_text()
        self.assertIn("STUDIO_NF_GC_CMPR_STREAM", src[src.index("Mod_GCStudioTexDataBytes"):][:600])

    def test_default_bake_has_no_stream_flag(self):
        for _n, flags, *_ in textures(self.plain):
            self.assertFalse(flags & disc.STUDIO_NF_GC_CMPR_STREAM)


if __name__ == "__main__":
    unittest.main()
