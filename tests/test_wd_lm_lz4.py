"""LZ4 lightmap pages of the world DL pack: the baker's region decodes with the ENGINE's decoder."""
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import bake_worlddl_goldsrc as bake  # noqa: E402
import gc_lz4  # noqa: E402

PAGE = 2048
REAL = Path("/home/tim/xash3d-gc-master54/build/gc-wd-cache/wd_dod_caen.gcd")


def engine_decoder_source() -> str:
    text = (ROOT / "engine/common/mod_studio.c").read_text()
    start = text.index("qboolean Mod_GCLZ4Decode(")
    return text[start:text.index("\n}\n", start) + 3]


# Same steps as R_GXWorldDLLmFetch (ref/gx/r_gx_worlddl.c) with the real decoder: stage the blob, decode into the
# 2048-byte slot, print the page. Usage: harness <region file> <page>
HARNESS = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef int qboolean;
typedef unsigned u32;
#define false 0
#define true 1
%s
static u32 be( const byte *p ) { return ( (u32)p[0] << 24 ) | ( p[1] << 16 ) | ( p[2] << 8 ) | p[3]; }
int main( int argc, char **argv )
{
	FILE *f = fopen( argv[1], "rb" );
	long n; byte *reg, slot[2048]; u32 page = atoi( argv[2] ), w0, w1, ofs, pad, stored;
	fseek( f, 0, SEEK_END ); n = ftell( f ); fseek( f, 0, SEEK_SET );
	reg = malloc( n ); fread( reg, 1, n, f );
	if( page >= be( reg )) { puts( "RANGE" ); return 1; }
	w0 = be( reg + 4 + 4 * page ); w1 = be( reg + 8 + 4 * page );
	ofs = w0 & ~31u; pad = w0 & 31u; stored = ( w1 & ~31u ) - ofs;
	if( !stored || stored > 2048 || pad >= stored ) { puts( "BAD" ); return 1; }
	if( stored == 2048 && !pad ) memcpy( slot, reg + ofs, 2048 );
	else
	{
		byte stage[2048];
		memcpy( stage, reg + ofs, stored );
		if( !Mod_GCLZ4Decode( stage, stage + stored - pad, slot, slot + 2048 )) { puts( "FAIL" ); return 1; }
	}
	fwrite( slot, 1, 2048, stdout );
	return 0;
}
"""


class LmLz4Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "h.c"
        src.write_text(HARNESS % engine_decoder_source())
        cls.exe = Path(cls.tmp.name) / "h"
        subprocess.run(["gcc", "-O1", "-w", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def decode(self, region: bytes, page: int) -> bytes:
        path = Path(self.tmp.name) / "region.bin"
        path.write_bytes(region)
        r = subprocess.run([str(self.exe), str(path), str(page)], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stdout[:20])
        return r.stdout

    def pages(self):
        rnd = random.Random(3)
        out = [bytes(PAGE), bytes([0x84, 0x10] * (PAGE // 2))]
        smooth = bytearray()
        v = 100
        while len(smooth) < PAGE:
            v = max(0, min(255, v + rnd.randrange(-2, 3)))
            smooth += struct.pack(">H", (v << 8) | v)
        out.append(bytes(smooth[:PAGE]))
        out.append(bytes(rnd.randrange(256) for _ in range(PAGE)))  # incompressible: stored raw
        return out

    def test_every_page_round_trips_through_engine_decoder(self):
        pages = self.pages()
        region = bake.pack_lm_pages_lz4(b"".join(pages))
        self.assertEqual(struct.unpack(">I", region[:4])[0], len(pages))
        self.assertLess(len(region), len(pages) * PAGE, "nothing shrank")
        for i, page in enumerate(pages):
            self.assertEqual(self.decode(region, i), page, f"page {i}")

    def test_pages_are_32_aligned_and_padding_counted(self):
        region = bake.pack_lm_pages_lz4(b"".join(self.pages()))
        n = struct.unpack(">I", region[:4])[0]
        offs = struct.unpack(">%dI" % (n + 1), region[4:4 + 4 * (n + 1)])
        self.assertEqual(offs[-1], len(region))
        for o in offs[:-1]:
            self.assertEqual((o & ~31) % 32, 0)
        self.assertEqual(offs[-1] % 32, 0)

    def test_corrupt_page_is_refused_or_differs(self):
        pages = self.pages()
        region = bytearray(bake.pack_lm_pages_lz4(b"".join(pages)))
        n = struct.unpack(">I", region[:4])[0]
        w0 = struct.unpack(">I", region[8:12])[0]
        region[(w0 & ~31) + 1] ^= 0xFF
        path = Path(self.tmp.name) / "bad.bin"
        path.write_bytes(bytes(region))
        r = subprocess.run([str(self.exe), str(path), "1"], capture_output=True)
        self.assertTrue(r.returncode != 0 or r.stdout != pages[1])

    @unittest.skipUnless(REAL.exists(), "no baked world DL")
    def test_real_caen_lightmaps(self):
        d = REAL.read_bytes()
        h = struct.unpack_from(">4sHH13I", d, 0)
        n_chunks, ofs_cd, ofs_chunks = h[4], h[11], h[14]
        last = 0
        for i in range(n_chunks):
            o, b = struct.unpack_from(">II", d, ofs_cd + 24 * i)
            last = max(last, o + b)
        lm = d[ofs_chunks + ((last + 31) & ~31):]
        region = bake.pack_lm_pages_lz4(lm)
        self.assertLess(len(region), len(lm) // 2)
        for i in range(0, len(lm) // PAGE, 37):
            self.assertEqual(self.decode(region, i), lm[i * PAGE:(i + 1) * PAGE])


if __name__ == "__main__":
    unittest.main()
