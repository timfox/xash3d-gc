#!/usr/bin/env python3
"""Count entity classes in GoldSrc BSP v30 maps: dod-entity-census.py MAP.bsp..."""
import collections, re, struct, sys

for path in sys.argv[1:]:
    with open(path, 'rb') as f:
        data = f.read()
    version, off, size = struct.unpack_from('<iii', data, 0)
    text = data[off:off + size].decode('latin-1')
    counts = collections.Counter()
    named = collections.Counter()
    # Tokens: quoted strings or bare braces ("{CRACK4" values hold braces).
    blocks, cur = [], None
    for tok in re.findall(r'"[^"]*"|[{}]', text):
        if tok == '{':
            cur = []
        elif tok == '}':
            if cur is not None:
                blocks.append(cur)
            cur = None
        elif cur is not None:
            cur.append(tok[1:-1])
    for toks in blocks:
        kv = dict(zip(toks[0::2], toks[1::2]))
        cls = kv.get('classname')
        if not cls:
            continue
        counts[cls] += 1
        if kv.get('targetname'):
            named[cls] += 1
    print('%s: %d entities' % (path, sum(counts.values())))
    for cls, n in counts.most_common():
        print('  %-28s %4d  (named %d)' % (cls, n, named[cls]))
