#!/usr/bin/env python3
"""Decode C:\\nvkick.txt (the shim's kick-stream dump, build 3088+) into nv50 method names,
and optionally CHECK the streams the way the PFIFO puller would react to them.

Usage: python3 -I tools/nvkick_decode.py nvkick.txt [MESA_NV50_DIR] [--kick N]
                                        [--check] [--log nvshim.log] [--tail nvtail.txt] [--quiet]

--check walks every kick with the channel's object set and reports what would stall the
puller (and, with --log, fails on the shim's own KICK WARN / wedge / failed-kick / trap /
heap-full lines - 3094): a SET_OBJECT with a handle that was never bound, a method on a subchannel with no
object, a DMA_* method (0x180-0x1fc) naming an unbound handle, a bad header type, a
truncated packet, a 3D QUERY address outside the VM window. The bound set comes from
"bind handle/class/engine" lines in nvshim.log (3089b+), or "NV_gpu_bind: handle=" lines
in nvtail.txt (older builds), plus the VxD's fixed bindings. This is the same check the
shim's NVSHIM_FAKE=1 backend performs in-process; --quiet prints only the check result.

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
        m = re.match(r"---- kick (\d+) cnt=(\d+)( data)?", line)
        if m:
            # 3094: 'data' = a nouveau_pushbuf_data payload (index data the 3D engine
            # consumes as the data of a VB_ELEMENT header in the previous kick), not methods
            cur = {"idx": int(m.group(1)), "cnt": int(m.group(2)), "dw": [], "data": bool(m.group(3))}
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
    out(f"==== kick {kick['idx']}: {kick['cnt']} dwords (dumped {n})" + (" - pushbuf_data payload (index data), not decoded" if kick.get("data") else ""))
    if kick.get("data"):
        return
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

VM_BASE, HEAP_BASE, HEAP_END = 0x40000000, 0x04000000, 0x08000000
FIXED_HANDLES = {0xbeef502d, 0xbeef0002, 0xbeef0003}   # nv_chan_init's own RAMHT entries

def bound_handles(logpath, tailpath):
    """Handles the channel's RAMHT holds, from the shim log and/or the VxD ring copy."""
    hs = set(FIXED_HANDLES)
    for path, rx in ((logpath, r"bind handle/class/engine ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})"),
                     (tailpath, r"NV_gpu_bind: handle=([0-9A-Fa-f]+) class=([0-9A-Fa-f]+) -> inst=[0-9A-Fa-f]+ engine=(\d+)")):
        if not path or not os.path.exists(path):
            continue
        for line in open(path, errors="replace"):
            m = re.search(rx, line)
            if m and m.group(3) != "ffffffff":
                hs.add(int(m.group(1), 16))
    return hs

def check(kicks, handles, out):
    """Return the number of faults; the channel state (subchannel objects) persists across kicks."""
    sub = [0] * 8
    faults = 0
    for kick in kicks:
        if kick.get("data"):
            continue                       # index payload: nothing to walk
        dw = kick["dw"]; n = len(dw); i = 0
        while i < n:
            h = dw[i]; typ = h >> 29; subc = (h >> 13) & 7; mthd = h & 0x1ffc; cnt = (h >> 18) & 0x7ff
            if typ not in (0, 2):
                out(f"CHECK: kick {kick['idx']} dword {i}: bad header {h:08x} (type {typ}) - puller would stop"); faults += 1; break
            if i + 1 + cnt > n:
                out(f"CHECK: kick {kick['idx']} dword {i}: header {h:08x} wants {cnt} dwords, {n - i - 1} left"); faults += 1; break
            if mthd == 0 and cnt == 1:
                hd = dw[i + 1]
                if hd not in handles:
                    out(f"CHECK: kick {kick['idx']} dword {i}: SET_OBJECT subch {subc} handle {hd:08x} is NOT bound - puller would stall here"); faults += 1
                sub[subc] = hd
                i += 2; continue
            if sub[subc] == 0:
                out(f"CHECK: kick {kick['idx']} dword {i}: method {mthd:04x} on subch {subc} which has no object"); faults += 1
            qaddr = None
            for k in range(cnt):
                mm = mthd + 4 * k if typ == 0 else mthd
                v = dw[i + 1 + k]
                if 0x180 <= mm < 0x200 and v not in handles:
                    out(f"CHECK: kick {kick['idx']} dword {i + 1 + k}: DMA method {mm:04x} on subch {subc} names unbound handle {v:08x}"); faults += 1
                if subc == 3:
                    if mm == 0x1b04: qaddr = v
                    elif mm == 0x1b0c and qaddr is not None:
                        if not (VM_BASE + HEAP_BASE <= qaddr < VM_BASE + HEAP_END):
                            out(f"CHECK: kick {kick['idx']} dword {i + 1 + k}: QUERY address {qaddr:08x} outside the VM window"); faults += 1
            i += 1 + cnt
    return faults

LOG_BAD = re.compile(r"KICK WARN|FAKE WEDGE|kick FAIL|GPU DEAD|bo_new FAIL|kick TRAP")

def log_faults(logpath, out):
    """3094: lines the shim writes only when a kick went wrong - KICK WARN (a DMA source
    that is not a live BO or still holds unflushed CPU writes: the 3093 black textures),
    a fake-GPU wedge, a failed/trapped kick, a dead GPU, a full heap."""
    n = 0
    if logpath and os.path.exists(logpath):
        for line in open(logpath, errors="replace"):
            if LOG_BAD.search(line):
                if n < 12:
                    out("LOG: " + line.rstrip())
                n += 1
    return n

def main():
    argv = sys.argv[1:]
    def take(opt):
        if opt in argv:
            j = argv.index(opt); v = argv[j + 1]; del argv[j:j + 2]; return v
        return None
    only = take("--kick"); logpath = take("--log"); tailpath = take("--tail")
    do_check = "--check" in argv; quiet = "--quiet" in argv
    args = [a for a in argv if not a.startswith("--")]
    only = int(only) if only is not None else None
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
    if not quiet:
        for k in kicks:
            if only is not None and k["idx"] != only:
                continue
            decode(k, lookups, print)
    if do_check:
        hs = bound_handles(logpath, tailpath)
        print(f"CHECK: {len(kicks)} kicks against {len(hs)} bound handles: " + " ".join(f"{h:08x}" for h in sorted(hs)))
        faults = check(kicks, hs, print)
        nlog = log_faults(logpath, print)
        if nlog:
            print(f"CHECK: {nlog} bad line(s) in {logpath} (KICK WARN / wedge / failed kick / trap / heap full)")
        faults += nlog
        print(f"CHECK: {faults} fault(s)" if faults else "CHECK: clean - every object/handle resolves, every packet is well-formed, no warnings in the log")
        sys.exit(2 if faults else 0)

if __name__ == "__main__":
    main()
