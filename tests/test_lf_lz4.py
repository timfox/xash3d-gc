"""LZ4 light-face pack (GClf version 2): random-access reads through the ENGINE's decoder match the raw data."""
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

BLOCK = bake.LF_BLOCK
BSP = Path("/home/tim/snap/steam/common/.local/share/Steam/steamapps/common/Half-Life/dod/maps/dod_flash.bsp")


def engine_decoder_source() -> str:
    text = (ROOT / "engine/common/mod_studio.c").read_text()
    start = text.index("qboolean Mod_GCLZ4Decode(")
    return text[start:text.index("\n}\n", start) + 3]


# The block walk of R_GXLfBlock / R_GXLfRead2 (ref/gx/r_gx_worlddl.c) with the real decoder.
# Usage: harness <region> <raw_total> <ofs> <len>   -> bytes on stdout
HARNESS = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef int qboolean;
typedef unsigned u32;
#define false 0
#define true 1
#define GC_LF_BLOCK %d
#define Q_min( a, b ) ( (a) < (b) ? (a) : (b) )
%s
static u32 be( const byte *p ) { return ( (u32)p[0] << 24 ) | ( p[1] << 16 ) | ( p[2] << 8 ) | p[3]; }
static byte *reg;
static int blockfetch( u32 raw_total, u32 blk, byte *out )
{
	byte stage[GC_LF_BLOCK];
	u32 raw_len, rawsz, ofs, pad, stored, w0, w1;
	if( blk * GC_LF_BLOCK >= raw_total ) return 0;
	raw_len = Q_min( GC_LF_BLOCK, raw_total - blk * GC_LF_BLOCK );
	rawsz = ( raw_len + 31u ) & ~31u;
	w0 = be( reg + 4 + 4 * blk ); w1 = be( reg + 8 + 4 * blk );
	ofs = w0 & ~31u; pad = w0 & 31u; stored = ( w1 & ~31u ) - ofs;
	if( !stored || stored > rawsz || pad >= stored ) return 0;
	if( stored == rawsz && !pad ) memcpy( out, reg + ofs, raw_len );
	else
	{
		memcpy( stage, reg + ofs, stored );
		if( !Mod_GCLZ4Decode( stage, stage + stored - pad, out, out + raw_len )) return 0;
	}
	return 1;
}
int main( int argc, char **argv )
{
	FILE *f = fopen( argv[1], "rb" );
	long n; u32 raw_total = atoi( argv[2] ), ofs = atoi( argv[3] ), len = atoi( argv[4] );
	byte b[GC_LF_BLOCK];
	fseek( f, 0, SEEK_END ); n = ftell( f ); fseek( f, 0, SEEK_SET );
	reg = malloc( n ); fread( reg, 1, n, f );
	while( len )
	{
		u32 blk = ofs / GC_LF_BLOCK, off = ofs %% GC_LF_BLOCK, c = Q_min( len, GC_LF_BLOCK - off );
		if( !blockfetch( raw_total, blk, b )) { puts( "FAIL" ); return 1; }
		fwrite( b + off, 1, c, stdout );
		ofs += c; len -= c;
	}
	return 0;
}
"""


class LfLz4Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "h.c"
        src.write_text(HARNESS % (BLOCK, engine_decoder_source()))
        cls.exe = Path(cls.tmp.name) / "h"
        subprocess.run(["gcc", "-O1", "-w", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def read(self, region: bytes, total: int, ofs: int, n: int) -> bytes:
        path = Path(self.tmp.name) / "r.bin"
        path.write_bytes(region)
        r = subprocess.run([str(self.exe), str(path), str(total), str(ofs), str(n)], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stdout[:16])
        return r.stdout

    def data(self):
        rnd = random.Random(11)
        out = bytearray()
        while len(out) < 5 * BLOCK + 300:          # short last block
            v = rnd.randrange(256)
            out += bytes([v, v ^ 3, v]) * rnd.randrange(1, 30)
            out += bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 8)))
        return bytes(out[:5 * BLOCK + 300]), bytes(rnd.randrange(256) for _ in range(BLOCK))

    def test_random_access_reads(self):
        a, rnd_block = self.data()
        data = a + rnd_block          # an incompressible block stays raw
        region = bake.pack_blocks_lz4(data, BLOCK)
        self.assertLess(len(region), len(data))
        rnd = random.Random(5)
        for _ in range(40):
            ofs = rnd.randrange(len(data) - 1)
            n = rnd.randrange(1, min(3 * BLOCK, len(data) - ofs) + 1)
            self.assertEqual(self.read(region, len(data), ofs, n), data[ofs:ofs + n], (ofs, n))
        self.assertEqual(self.read(region, len(data), len(data) - 7, 7), data[-7:])   # 3-byte samples at the end too

    def test_short_data_and_empty(self):
        for n in (1, 3, 31, 32, 33, BLOCK, BLOCK + 1):
            d = bytes(i % 251 for i in range(n))
            region = bake.pack_blocks_lz4(d, BLOCK)
            self.assertEqual(self.read(region, n, 0, n), d)

    @unittest.skipUnless(BSP.exists(), "no dod_flash.bsp")
    def test_baked_lf_v3_equals_v1(self):
        with tempfile.TemporaryDirectory() as t:
            v1, v3 = Path(t) / "a.gcd", Path(t) / "b.gcd"
            bake.bake_lightfaces(BSP, v1, lz4=False)
            bake.bake_lightfaces(BSP, v3, lz4=True)
            a, b = v1.read_bytes(), v3.read_bytes()
            h1 = struct.unpack_from(">4s5I", a, 0)
            h3 = struct.unpack_from(">4s7I", b, 0)
            self.assertEqual((h1[1], h3[1], h3[2], h3[7]), (1, 3, h1[2], BLOCK))
            self.assertEqual(h3[6], len(b))
            self.assertLess(len(b), len(a) * 0.85)
            recs1, lit1 = a[h1[3]:h1[4]], a[h1[4]:h1[4] + h1[5]]
            # version 3: records are raw, byte for byte, in front of the lighting region
            self.assertEqual(b[h3[3]:h3[3] + h1[2] * 56], recs1[:h1[2] * 56])
            self.assertGreaterEqual(h3[4], 32 + h1[2] * 56)
            lit_region = b[h3[4]:]
            self.assertEqual(h3[5], h1[5])
            for ofs in (0, 1000, 1023, h1[5] // 2, h1[5] - 3):
                self.assertEqual(self.read(lit_region, h1[5], ofs, 3), lit1[ofs:ofs + 3])


if __name__ == "__main__":
    unittest.main()
