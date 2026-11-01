#!/usr/bin/env python3
"""profsym.py - summarise a t61prof.txt (t61.py prof) by module and by function.

  profsym.py t61prof.txt [--sym MODULE=unstripped.dll ...] [--top N] [--thread T]
                         [--callers SUBSTR ...]

--callers shows, for the samples whose function name contains SUBSTR, the most common
chains of return addresses the profiler found on the stack ("s" lines; a stack scan,
so a chain may include a stale address).

Modules given with --sym are resolved to function names with the PE's symbol table
(i686-w64-mingw32-nm); a stripped module (the shipped mesa3d.dll) needs an unstripped
link of the same objects (same section addresses). Without symbols a module is
broken down by its exports, else by 4 KiB page.
"""
import argparse, bisect, collections, re, subprocess, sys


def load_syms(path, base_hint):
    out = subprocess.run(["i686-w64-mingw32-nm", "-C", "--defined-only", path],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prof")
    ap.add_argument("--sym", action="append", default=[], help="MODULE=path of an unstripped copy")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--thread", type=int, default=None)
    ap.add_argument("--callers", action="append", default=[])
    ap.add_argument("--depth", type=int, default=3)
    a = ap.parse_args()

    mods, samples, total, hdr, stacks = [], [], 0, "", []
    for line in open(a.prof, encoding="latin1"):
        line = line.strip()
        if line.startswith("samples"):
            hdr = line
        elif line.startswith("module "):
            _, base, size, name = line.split(" ", 3)
            mods.append((int(base, 16), int(size, 16), name))
        elif line.startswith("s "):
            p = line.split()
            if a.thread is not None and int(p[1]) != a.thread:
                continue
            stacks.append([int(x, 16) for x in p[2:]])
        elif re.match(r"^\d+ [0-9a-f]{8} \d+ ", line):
            t, eip, n, _ = line.split(" ", 3)
            if a.thread is not None and int(t) != a.thread:
                continue
            samples.append((int(eip, 16), int(n)))
            total += int(n)
    symtab = {}
    for s in a.sym:
        m, p = s.split("=", 1)
        symtab[m.lower()] = load_syms(p, 0)

    def module_of(eip):
        for b, sz, nm in mods:
            if b <= eip < b + sz:
                return nm, b
        return "?", 0

    def name(eip):
        nm, b = module_of(eip)
        st = symtab.get(nm.lower())
        if st:
            i = bisect.bisect_right(st[0], eip) - 1
            return f"{nm.split('.')[0]}!{st[1][i] if i >= 0 else '?'}"
        return f"{nm}+{eip - b:x}" if nm != "?" else f"{eip:08x}"

    per_mod = collections.Counter()
    per_fn = collections.Counter()
    for eip, n in samples:
        nm, b = module_of(eip)
        per_mod[nm] += n
        st = symtab.get(nm.lower())
        if st:
            i = bisect.bisect_right(st[0], eip) - 1
            fn = st[1][i] if i >= 0 else "?"
            per_fn[f"{nm}!{fn}"] += n
        elif nm == "?":
            per_fn[f"?!{eip >> 12 << 12:08x}"] += n
        else:
            per_fn[f"{nm}+{(eip - b) >> 8 << 8:x}"] += n

    print(hdr)
    print(f"{total} samples")
    print("\nby module:")
    for nm, n in per_mod.most_common():
        print(f"  {100.0 * n / total:5.1f}%  {n:6d}  {nm}")
    print(f"\ntop {a.top} functions (unsymbolised modules by 256-byte block):")
    for fn, n in per_fn.most_common(a.top):
        print(f"  {100.0 * n / total:5.1f}%  {n:6d}  {fn}")

    for want in a.callers:
        sel = [st for st in stacks if want in name(st[0])]
        chains = collections.Counter(" <- ".join(name(r) for r in st[1:1 + a.depth] if r) for st in sel)
        first = collections.Counter(name(st[1]) if st[1] else "-" for st in sel)
        print(f"\ncallers of *{want}* ({len(sel)} samples), first caller:")
        for c, n in first.most_common(15):
            print(f"  {n:6d}  {c}")
        print(f"chains:")
        for c, n in chains.most_common(15):
            print(f"  {n:6d}  {c}")


if __name__ == "__main__":
    sys.exit(main())
