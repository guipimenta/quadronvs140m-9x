#!/usr/bin/env python3
"""Turn the harness output (CTXPROG/CTXVALS listing) into driver/vmdisp9x-nv/nv50_grctx_g86.h."""
import sys, re
prog, vals, mode, size = [], [], None, None
for line in open(sys.argv[1]):
    t = line.split()
    if t[0] == "chipset": size = int(re.search(r'\((\d+) dwords\)', line).group(1)); continue
    if t[0] in ("CTXPROG", "CTXVALS", "END"): mode = t[0]; continue
    if mode == "CTXPROG": prog.append(int(t[1], 16))
    elif mode == "CTXVALS": vals.append((int(t[0]), int(t[1], 16)))
out = ["/* nv50 PGRAPH context for the ThinkPad T61 G86 (chipset 0x86, units 0x1540=0xF3010001).\n"
" * Generated on the host from CURRENT nouveau (torvalds/linux master,\n"
" * drivers/gpu/drm/nouveau/nvkm/engine/gr/ctxnv50.c, unmodified) by\n"
" * analysis/grctx_host/fetch_build_run.sh; DERIVED DATA (like the EVO method streams), not\n"
" * source. 3100 replaced the 2026-10-23 generation from nouveau v2.6.36 (see the file history\n"
" * and docs/HW3D_PLAN.md for the differences). ctxprog -> PGRAPH_CTXCTL_UCODE at init;\n"
" * ctxvals -> each channel grctx buffer. */\n"
"#ifndef NV50_GRCTX_G86_H\n#define NV50_GRCTX_G86_H\n\n"
"#define NV50_GRCTX_UNITS   0xF3010001UL   /* 0x1540 this was built for */\n"
"#define NV50_GRCTX_DWORDS  %dUL\n#define NV50_GRCTX_SIZE    (%dUL*4)\n"
"#define NV50_CTXPROG_LEN   %dUL\n\n" % (size, size, len(prog)),
"static const unsigned long nv50_ctxprog[%d] = {\n" % len(prog)]
for i in range(0, len(prog), 6):
    out.append("\t" + " ".join("0x%08xUL," % v for v in prog[i:i+6]) + "\n")
out.append("};\n\n/* %d non-zero context values: {dword offset, value} */\n#define NV50_CTXVALS_CNT   %dUL\n" % (len(vals), len(vals)))
out.append("static const unsigned long nv50_ctxvals[%d][2] = {\n" % len(vals))
out += ["\t{%dUL, 0x%08xUL},\n" % (off, v) for off, v in vals]
out.append("};\n\n#endif /* NV50_GRCTX_G86_H */\n")
open(sys.argv[2], "w").write("".join(out))
print("wrote", sys.argv[2], ": ctxprog", len(prog), "dwords, ctxvals", len(vals), "non-zero of", size)
