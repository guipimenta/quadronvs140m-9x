#!/usr/bin/env python3
"""Extract the G86 PGRAPH context data from the NVIDIA Windows XP miniport (nv4_mini.sys)
and write driver/vmdisp9x-nv/nv50_grctx_g86_xp.h (DERIVED DATA, like the nouveau-generated
nv50_grctx_g86*.h) plus a human-readable listing next to this script.

Where the data comes from (objdump VAs, image base 0x10000 - see analysis/reverse/XP_nv4_mini.asm):
  - the G86 chip object is registered by the constructor at 0xe0280 (predicate 0xe0230 accepts
    BOOT0 0x086xxxxx except revision A1); its engine-method table is 0x44ba18 (0x50c pairs).
  - engine 6 (PGRAPH) method 0x0600002f = 0x2506c0: per-chip PGRAPH init (0x40008c=4, ctxctl
    flags 0x400824/82c/830 = 0, 0x402ca8=0x800, zcull regions zeroed, TPC 0x408000=0x30 and the
    unit clock-gating registers = 0xf0c242 family).
  - method 0x06000030 = 0x250ae0: after a PGRAPH signature check (0x400800 == 0x5049e8ac or
    0x7ee17cc6) it installs: ctxprog = 224 dwords at 0x47a2b8, context size 0x5bca0, golden
    context values = 723 {byte offset, value} pairs at 0x3aeaf0, the MMIO list = 512
    {register, grctx byte offset, init value} triples at 0x3b0188, and 230 {method>>2, value}
    pairs at 0x47a638 that the engine init (method 0x1f = 0x2a8b40) feeds to PGRAPH through
    ICMD (0x400200/0x400204) after writing the MMIO list to the registers.
  - method 0x1f (0x2a8b40) writes every MMIO-list register (TPC registers gated by the TPC
    mask) and then the ICMD methods; context create (0x2a1780) zeroes 0x5bca0 bytes and writes
    the 723 pairs; a channel's first bind (0x2a8760) sets 0x400824 |= 0x800 and CTXCTL_CUR
    directly, later switches (0x2a38c0) use 0x400824 |= 0x20 (save) / 0x40 (load) + 0x400304 |= 1.

Usage: python3 -I analysis/xp_grctx/extract_xp_g86.py [nv4_mini.sys] [out.h] [listing.txt]
"""
import struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "analysis/xp_extracted/nv4_mini.sys")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "driver/vmdisp9x-nv/nv50_grctx_g86_xp.h")
LST = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "analysis/xp_grctx/g86_xp_context.txt")

IB = 0x10000                      # image base; objdump VA = file offset + IB for this image
VA_CTXPROG, N_CTXPROG = 0x47a2b8, 224
VA_CTXVALS, N_CTXVALS = 0x3aeaf0, 723
VA_MMIO,    N_MMIO    = 0x3b0188, 512
VA_ICMD,    N_ICMD    = 0x47a638, 230
GRCTX_SIZE = 0x5bca0

d = open(SYS, "rb").read()
def rd(va): return struct.unpack_from("<I", d, va - IB)[0]

prog = [rd(VA_CTXPROG + 4 * i) for i in range(N_CTXPROG)]
vals = [(rd(VA_CTXVALS + 8 * i), rd(VA_CTXVALS + 8 * i + 4)) for i in range(N_CTXVALS)]
mmio = [(rd(VA_MMIO + 12 * i), rd(VA_MMIO + 12 * i + 4), rd(VA_MMIO + 12 * i + 8)) for i in range(N_MMIO)]
icmd = [(rd(VA_ICMD + 8 * i), rd(VA_ICMD + 8 * i + 4)) for i in range(N_ICMD)]

# sanity: the data must look like what the disassembly says it is
assert prog[0] == 0x0070008e and prog[-1] == 0x0060000c, "ctxprog signature"
assert all(o % 4 == 0 and o < GRCTX_SIZE for o, v in vals), "ctxvals offsets"
assert len(set(o for o, v in vals)) == len(vals), "ctxvals duplicate offsets"
assert mmio[0][0] == 0x400808 and all(0x400000 <= r < 0x410000 for r, o, v in mmio), "mmio list"
t1 = dict(vals)
assert all(t1.get(o) == v for r, o, v in mmio if v), "mmio init values must be in the golden context"
assert all(0x38 <= m <= 0x83c for m, v in icmd), "icmd methods"

def fmt_rows(rows, per):
    out = []
    for i in range(0, len(rows), per):
        out.append("\t" + " ".join(rows[i:i + per]) + "\n")
    return "".join(out)

h = []
h.append("/* nv50 PGRAPH context for the G86 taken from NVIDIA's Windows XP miniport nv4_mini.sys\n"
         " * (analysis/xp_extracted/, the driver that ran this exact chip on XP) by\n"
         " * analysis/xp_grctx/extract_xp_g86.py - DERIVED DATA, not source; regenerate, do not edit.\n"
         " * Grctx variant 3 (build 3102): the XP context microcode goes to PGRAPH_CTXCTL_UCODE, the\n"
         " * golden context values into each channel's grctx (byte layout: MMIO part at 0x100, xfer\n"
         " * areas at 0xa00 and 0x52d00 - nouveau's layout + 0x100), the MMIO list is written to the\n"
         " * registers at PGRAPH init the way XP's engine init does. See docs/HW3D_PLAN.md step 11. */\n"
         "#ifndef NV50_GRCTX_G86_XP_H\n#define NV50_GRCTX_G86_XP_H\n\n")
h.append("#define NV50_XP_GRCTX_SIZE   0x%xUL   /* bytes, XP's context allocation for the G86 */\n" % GRCTX_SIZE)
h.append("#define NV50_XP_CTXPROG_LEN  %dUL\n" % N_CTXPROG)
h.append("static const unsigned long nv50_ctxprog_xp[%d] = {\n" % N_CTXPROG)
h.append(fmt_rows(["0x%08xUL," % v for v in prog], 6))
h.append("};\n\n/* %d non-zero golden context values: {dword offset, value} */\n" % N_CTXVALS)
h.append("#define NV50_XP_CTXVALS_CNT  %dUL\n" % N_CTXVALS)
h.append("static const unsigned long nv50_ctxvals_xp[%d][2] = {\n" % N_CTXVALS)
h.append("".join("\t{%dUL, 0x%08xUL},\n" % (o // 4, v) for o, v in vals))
h.append("};\n\n/* XP's PGRAPH MMIO list, %d registers: {register, init value} in the order XP's engine init\n"
         " * writes them (the ctxprog saves/restores the same registers; TPC 0x408xxx = TPC 0 only). */\n" % N_MMIO)
h.append("#define NV50_XP_MMIO_CNT     %dUL\n" % N_MMIO)
h.append("static const unsigned long nv50_mmio_xp[%d][2] = {\n" % N_MMIO)
h.append(fmt_rows(["{0x%06xUL,0x%08xUL}," % (r, v) for r, o, v in mmio], 3))
h.append("};\n\n#endif /* NV50_GRCTX_G86_XP_H */\n")
open(OUT, "w").write("".join(h))

# listing: ctxprog disassembly, MMIO list with offsets, ICMD methods
FL = {0: 'SWAP_DIR', 1: 'UNK01', 3: 'UNK03', 0xb: 'UNK0B', 0xe: 'XFER_SWITCH', 0x1c: 'STATE', 0x1d: 'UNK1D',
      0x20: 'UNK20', 0x40: 'STATUS', 0x44: 'AUTO_SAVE', 0x45: 'AUTO_LOAD', 0x4a: 'NEWCTX', 0x4b: 'XFER',
      0x4d: 'ALWAYS', 0x4f: 'INTR'}
def fl(f): return FL.get(f, 'F%02x' % f)
def dis(w):
    op = w & 0xf00000
    if op == 0x100000: return 'CTX  reg 0x%06x cnt %d' % (0x400000 + (w & 0x3fff) * 4, (w >> 16) & 0xf)
    if op == 0x200000: return 'LOAD_SR 0x%05x' % (w & 0xfffff)
    if op == 0x300000: return 'UNK3 0x%05x' % (w & 0xfffff)
    if op == 0x400000: return 'BRA  %s%s -> %d' % ('!' if w & 0x80 else '', fl(w & 0x7f), (w >> 8) & 0x1ff)
    if op == 0x500000: return 'WAIT %s%s' % ('' if w & 0x80 else '!', fl(w & 0x7f))
    if op == 0x600000: return 'CMD  0x%x' % (w & 0xfffff)
    if op == 0x700000: return 'SET  %s=%d' % (fl(w & 0x7f), 1 if w & 0x80 else 0)
    if op == 0x800000: return 'XFER%s n=0x%x' % ('_2' if w & 0x80000 else '_1', w & 0xff)
    if op == 0x900000: return 'DISABLE 0x%05x' % (w & 0xfffff)
    if op == 0xa00000: return 'UNKA 0x%05x' % (w & 0xfffff)
    if op == 0xc00000: return 'SEEK%s 0x%x' % ('_2' if w & 0x80000 else '_1', w & 0xff)
    return '?? %08x' % w
L = [__doc__, "\n== ctxprog (%d dwords @%x)\n" % (N_CTXPROG, VA_CTXPROG)]
L += ["%3d: %08x %s\n" % (i, w, dis(w)) for i, w in enumerate(prog)]
L.append("\n== MMIO list (%d @%x): register -> grctx byte offset = init value\n" % (N_MMIO, VA_MMIO))
L += ["%06x -> %05x = %08x\n" % (r, o, v) for r, o, v in mmio]
L.append("\n== golden context (%d @%x): byte offset = value\n" % (N_CTXVALS, VA_CTXVALS))
L += ["%05x = %08x\n" % (o, v) for o, v in vals]
L.append("\n== ICMD method init (%d @%x): 3D method = value (engine init only, not a context value)\n" % (N_ICMD, VA_ICMD))
L += ["%04x = %08x\n" % (m * 4, v) for m, v in icmd]
open(LST, "w").write("".join(L))
print("wrote", OUT, "and", LST, ": ctxprog", N_CTXPROG, "ctxvals", N_CTXVALS, "mmio", N_MMIO, "size 0x%x" % GRCTX_SIZE)
