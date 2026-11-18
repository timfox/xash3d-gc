#!/usr/bin/env python3
"""Top BSS/data symbols of a GameCube ELF, grouped by owner file (DWARF line info).

  python3 scripts/gamecube-symbol-owners.py OUT/bin/xash-dod [--top 60] [--out table.md]

Needs powerpc-eabi-nm (devkitPPC) and a build with -g. No linker map is required.
"""
import argparse, collections, os, re, subprocess, sys


def nm_rows(elf, nm):
    out = subprocess.run([nm, "-S", "-l", "-C", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        loc = ""
        if "\t" in line:
            line, loc = line.split("\t", 1)
        p = line.split(None, 3)
        if len(p) < 4 or p[2] not in "bBdD":
            continue
        try:
            addr, size = int(p[0], 16), int(p[1], 16)
        except ValueError:
            continue
        f = re.sub(r":\d+$", "", loc)
        f = re.sub(r"^.*/\.\./", "", f) if "/../" in f else f
        yield addr, size, "bss" if p[2] in "bB" else "data", p[3], f or "(no line info)"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--nm", default=os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitPPC/bin/powerpc-eabi-nm"))
    ap.add_argument("--out")
    a = ap.parse_args()
    rows = list(nm_rows(a.elf, a.nm))
    md = []
    for kind in ("bss", "data"):
        sel = [r for r in rows if r[2] == kind]
        tot = sum(r[1] for r in sel)
        md.append(f"\n## Top {a.top} {kind} symbols ({tot / 1048576:.2f} MiB in {len(sel)} symbols)\n")
        md.append("| # | KiB | symbol | owner file |\n|--:|--:|---|---|")
        for i, r in enumerate(sorted(sel, key=lambda r: -r[1])[: a.top], 1):
            md.append(f"| {i} | {r[1] / 1024:.1f} | `{r[3][:70]}` | {r[4]} |")
        g = collections.defaultdict(int)
        for r in sel:
            g[r[4]] += r[1]
        md.append(f"\n### {kind} by owner file (top 25)\n\n| owner file | KiB | % |\n|---|--:|--:|")
        for f, s in sorted(g.items(), key=lambda x: -x[1])[:25]:
            md.append(f"| {f} | {s / 1024:.1f} | {100 * s / tot:.1f}% |")
    text = "\n".join(md) + "\n"
    if a.out:
        open(a.out, "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
