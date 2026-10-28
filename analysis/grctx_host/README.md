# grctx_host - nouveau's nv50 PGRAPH context generator, run on the host

`driver/vmdisp9x-nv/nv50_grctx_g86.h` (ctxprog microcode + initial context values the VxD
loads at channel start) is derived data: the output of nouveau's `ctxnv50.c` for chipset
0x86 and units register 0x1540 = 0xF3010001 (the T61's NVS 140M).

- `fetch_build_run.sh [chip] [units]` fetches the current `ctxnv50.c`/`ctxnv40.h` from
  torvalds/linux with `gh`, builds them against the tiny kernel stubs in `stubs/` (nvkm_rd32
  answers 0x1540 with the units value; nvkm_wo32 writes the context image), runs the
  generator in PROG and VALS mode and rewrites the header through `gen_header.py`.
- `g86_F3010001.txt` is the listing the 3100 header was made from.

History: the first generation (2026-10-23, build 3091+) used nouveau v2.6.36's
`nv50_grctx.c` through the same kind of harness. Running both generators with a register
mapping hook showed, for the G86, three real differences besides a reordered ctxprog:
the per-TP registers 0x40821c/0x40829c (0x018c0000 now, 0x008c0000 then - a G86-specific
fix), the clipping strand's `FP_CTRL_UNK196C`/`UNK1968` defaults (the 3 sat one slot late),
and the 2D engine's `IFC_CLIP_W/H` defaults. Build 3100 switched to the current generator
while chasing the 1024x768-only corner holes (pixels of huge triangles left uncovered).
