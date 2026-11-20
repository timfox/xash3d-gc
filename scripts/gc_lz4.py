"""Pure-python LZ4 block codec for the GameCube bakers.

The reference decoder here mirrors the engine's Mod_GCLZ4Decode (engine/common/mod_studio.c): the same bounds
checks, and the same failures. Streams must end in at least 5 literals and no match may start in the last 12 bytes
(the standard LZ4 end rules), which compress() honours.

In-place decoding (stream at the end of the output buffer) is NOT possible for an exact-fit buffer: the last match
can end at most one byte past the read pointer however the stream is cut, so loaders stage the stream elsewhere.
"""
from __future__ import annotations

MINMATCH = 4
LAST_LITERALS = 5      # a block must end with at least 5 literals
MFLIMIT = 12           # no match may start in the last 12 bytes


def compress(src: bytes) -> bytes:
    n = len(src)
    out = bytearray()
    table: dict[bytes, int] = {}
    anchor = 0
    i = 0
    limit = n - MFLIMIT
    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key, -1)
        table[key] = i
        if cand >= 0 and i - cand <= 0xFFFF:
            m = MINMATCH
            maxm = n - LAST_LITERALS - i
            while m < maxm and src[cand + m] == src[i + m]:
                m += 1
            _emit(out, src, anchor, i, i - cand, m)
            i += m
            anchor = i
        else:
            i += 1
    _emit(out, src, anchor, n, 0, 0)
    return bytes(out)


def _len_bytes(out: bytearray, v: int) -> None:
    while v >= 255:
        out.append(255)
        v -= 255
    out.append(v)


def _emit(out: bytearray, src: bytes, a: int, b: int, off: int, mlen: int) -> None:
    lit = b - a
    ml = mlen - MINMATCH if mlen else 0
    out.append((min(lit, 15) << 4) | (min(ml, 15) if mlen else 0))
    if lit >= 15:
        _len_bytes(out, lit - 15)
    out += src[a:b]
    if mlen:
        out += bytes((off & 255, off >> 8))
        if ml >= 15:
            _len_bytes(out, ml - 15)


def decompress(comp: bytes, raw_len: int) -> bytes:
    """Reference decoder with the engine's checks (same failure cases return ValueError)."""
    buf = bytearray(raw_len)
    _decode(memoryview(bytearray(comp)), 0, len(comp), buf, 0, raw_len, shared=False)
    return bytes(buf)


def _decode(src, ip, iend, dst, op, oend, shared) -> None:
    obase = op
    while ip < iend:
        token = src[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if ip >= iend: raise ValueError("truncated literal length")
                s = src[ip]; ip += 1; lit += s
                if s != 255: break
        if lit > oend - op or lit > iend - ip: raise ValueError("literal overrun")
        for _ in range(lit):          # forward byte copy (matters when src and dst overlap)
            dst[op] = src[ip]; op += 1; ip += 1
        if ip >= iend: break
        if iend - ip < 2: raise ValueError("truncated offset")
        off = src[ip] | (src[ip + 1] << 8); ip += 2
        ml = token & 15
        if ml == 15:
            while True:
                if ip >= iend: raise ValueError("truncated match length")
                s = src[ip]; ip += 1; ml += s
                if s != 255: break
        ml += 4
        if not off or off > op - obase or ml > oend - op: raise ValueError("bad match")
        for _ in range(ml):
            dst[op] = dst[op - off]; op += 1
    if op != oend: raise ValueError("short output")
