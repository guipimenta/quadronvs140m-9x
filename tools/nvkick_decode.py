#!/usr/bin/env python3
"""Decode C:\\nvkick.txt (the shim's kick-stream dump, build 3088+) into nv50 method names.

Usage: python3 -I tools/nvkick_decode.py nvkick.txt [MESA_NV50_DIR] [--kick N]

MESA_NV50_DIR defaults to the mesa9x build tree's nv50 directory (nv50_3d.xml.h,
nv50_2d.xml.h, nv50_compute.xml.h, and the M2MF class from nv50_defs / nv50_winsys).
Subchannel assignment follows nv50_winsys.h: 3D=3, 2D=4, M2MF=5, COMPUTE=6.
"""
import re, sys, os

DEF_DIR = "/home/guilherme/projects/mesa9x-build/mesa9x/mesa-25.1.x/src/gallium/drivers/nouveau/nv50"

def load_defs(path, prefix):
    """Return list of (base, stride, count, name) for #defines of the given class prefix."""
    out = []
    if not os.path.exists(path):
        return out
    txt = open(path, errors="replace").read()
    plain = re.compile(r"#define\s+(%s_[A-Z0-9_]+)\s+0x([0-9a-fA-F]{8})\s*$" % prefix, re.M)
    arr1 = re.compile(r"#define\s+(%s_[A-Z0-9_]+)\(i0\)\s+\(0x([0-9a-fA-F]{8})\s*\+\s*0x([0-9a-fA-F]+)\*\(i0\)\)" % prefix)
    arr2 = re.compile(r"#define\s+(%s_[A-Z0-9_]+)\(i0,\s*i1\)\s+\(0x([0-9a-fA-F]{8})\s*\+\s*0x([0-9a-fA-F]+)\*\(i0\)\s*\+\s*0x([0-9a-fA-F]+)\*\(i1\)\)" % prefix)
    lens = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"#define\s+(%s_[A-Z0-9_]+)__LEN\s+(0x[0-9a-fA-F]+)" % prefix, txt)}
    for m in plain.finditer(txt):
        name = m.group(1)
        if name.endswith(("__ESIZE", "__LEN", "__MASK", "__SHIFT", "__MAX", "__MIN")):
            continue
        out.append((int(m.group(2), 16), 0, 1, name))
    for m in arr1.finditer(txt):
        name = m.group(1)
        n = lens.get(name, 16)
        out.append((int(m.group(2), 16), int(m.group(3), 16), n, name))
    for m in arr2.finditer(txt):
        name = m.group(1)
        s0 = int(m.group(3), 16); s1 = int(m.group(4), 16)
        n0 = lens.get(name, 16)
        for i in range(n0):
            out.append((int(m.group(2), 16) + s0 * i, s1, max(1, s0 // s1), f"{name}[{i}]"))
    return out

def make_lookup(defs):
    plain = {}
    arrays = []
    for base, stride, n, name in defs:
        if stride == 0:
            # keep the first (xml.h defines value constants with the same prefix too; those
            # are small numbers and never collide with method offsets >= 0x100 in practice)
            plain.setdefault(base, name)
        else:
            arrays.append((base, stride, n, name))
    def look(m):
        if m in plain:
            return plain[m]
        best = None
        for base, stride, n, name in arrays:
            if base <= m < base + stride * n and (m - base) % stride == 0:
                cand = f"{name}({(m - base) // stride})"
                if best is None or len(cand) < len(best):
                    best = cand
            elif base <= m < base + stride * n:
                # inside an element: offset within a struct-like array element
                idx = (m - base) // stride
                off = (m - base) % stride
                cand = f"{name}({idx})+0x{off:x}"
                if best is None:
                    best = cand
        return best or f"0x{m:04x}"
    return look

CLASS_NAMES = {3: "3D", 4: "2D", 5: "M2MF", 6: "COMPUTE", 0: "sub0", 1: "sub1", 2: "sub2", 7: "sub7"}
VAL_NAMES = {
    ("3D", 0x15dc): {0: "POINTS", 1: "LINES", 2: "LINE_LOOP", 3: "LINE_STRIP", 4: "TRIANGLES", 5: "TRIANGLE_STRIP",
                     6: "TRIANGLE_FAN", 7: "QUADS", 8: "QUAD_STRIP", 9: "POLYGON"},
}

def parse_kicks(path):
    kicks = []
    cur = None
    for line in open(path, errors="replace"):
        line = line.strip()
        m = re.match(r"---- kick (\d+) cnt=(\d+)", line)
        if m:
            cur = {"idx": int(m.group(1)), "cnt": int(m.group(2)), "dw": []}
            kicks.append(cur)
            continue
        if cur is None or not line:
            continue
        for tok in line.split():
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                cur["dw"].append(int(tok, 16))
    return kicks

def decode(kick, lookups, out):
    dw = kick["dw"]
    i = 0
    n = len(dw)
    out(f"==== kick {kick['idx']}: {kick['cnt']} dwords (dumped {n})")
    while i < n:
        h = dw[i]
        typ = h >> 29
        subc = (h >> 13) & 7
        mthd = h & 0x1ffc
        count = (h >> 18) & 0x7ff
        cls = CLASS_NAMES.get(subc, f"sub{subc}")
        look = lookups.get(cls)
        if typ == 0:   # incrementing
            mode = "inc"
        elif typ == 2:  # non-incrementing (0x40000000)
            mode = "ni "
        elif h == 0x00000000 and count == 0:
            out(f"  [{i:4d}] {h:08x}  NOP"); i += 1; continue
        else:
            out(f"  [{i:4d}] {h:08x}  ?? unknown header type {typ} - aborting kick decode"); return
        data = dw[i + 1:i + 1 + count]
        if mthd == 0 and count == 1:
            out(f"  [{i:4d}] {h:08x}  {cls}:SET_OBJECT handle={data[0]:08x}")
        else:
            for k, v in enumerate(data):
                m = mthd + (0 if mode == "ni " else 4 * k)
                name = look(m) if look else f"0x{m:04x}"
                extra = ""
                vn = VAL_NAMES.get((cls, m))
                if vn and v in vn:
                    extra = f"  ({vn[v]})"
                if k == 0:
                    out(f"  [{i:4d}] {h:08x}  {cls}:{name} = {v:08x}{extra}")
                elif mode == "inc":
                    out(f"  [{i + 1 + k:4d}]           {cls}:{name} = {v:08x}{extra}")
                else:
                    out(f"  [{i + 1 + k:4d}]           {cls}:{name}[{k}] = {v:08x}")
            if count == 0:
                out(f"  [{i:4d}] {h:08x}  {cls}: zero-count header")
        if len(data) < count:
            out(f"  ... stream truncated (header wants {count}, {len(data)} left)")
        i += 1 + count

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--kick"):
            only = int(a.split("=")[1]) if "=" in a else None
    if "--kick" in sys.argv:
        only = int(sys.argv[sys.argv.index("--kick") + 1])
        args = [a for a in args if a != str(only)]
    if not args:
        print(__doc__); sys.exit(1)
    path = args[0]
    d = args[1] if len(args) > 1 else DEF_DIR
    lookups = {
        "3D": make_lookup(load_defs(os.path.join(d, "nv50_3d.xml.h"), "NV50_3D")),
        "2D": make_lookup(load_defs(os.path.join(d, "nv50_2d.xml.h"), "NV50_2D")),
        "COMPUTE": make_lookup(load_defs(os.path.join(d, "nv50_compute.xml.h"), "NV50_COMPUTE")),
        "M2MF": make_lookup([(0x180, 0, 1, "DMA_NOTIFY"), (0x184, 0, 1, "DMA_BUFFER_IN"), (0x188, 0, 1, "DMA_BUFFER_OUT"),
                             (0x200, 0, 1, "LINEAR_IN"), (0x204, 0, 1, "TILING_MODE_IN"), (0x208, 0, 1, "TILING_PITCH_IN"),
                             (0x20c, 0, 1, "TILING_HEIGHT_IN"), (0x210, 0, 1, "TILING_DEPTH_IN"), (0x214, 0, 1, "TILING_POSITION_IN_Z"),
                             (0x218, 0, 1, "TILING_POSITION_IN"), (0x21c, 0, 1, "LINEAR_OUT"), (0x220, 0, 1, "TILING_MODE_OUT"),
                             (0x224, 0, 1, "TILING_PITCH_OUT"), (0x228, 0, 1, "TILING_HEIGHT_OUT"), (0x22c, 0, 1, "TILING_DEPTH_OUT"),
                             (0x230, 0, 1, "TILING_POSITION_OUT_Z"), (0x234, 0, 1, "TILING_POSITION_OUT"),
                             (0x238, 0, 1, "OFFSET_IN_HIGH"), (0x23c, 0, 1, "OFFSET_OUT_HIGH"),
                             (0x30c, 0, 1, "OFFSET_IN"), (0x310, 0, 1, "OFFSET_OUT"), (0x314, 0, 1, "PITCH_IN"), (0x318, 0, 1, "PITCH_OUT"),
                             (0x31c, 0, 1, "LINE_LENGTH_IN"), (0x320, 0, 1, "LINE_COUNT"), (0x324, 0, 1, "FORMAT"), (0x328, 0, 1, "BUFFER_NOTIFY")]),
    }
    kicks = parse_kicks(path)
    for k in kicks:
        if only is not None and k["idx"] != only:
            continue
        decode(k, lookups, print)

if __name__ == "__main__":
    main()
