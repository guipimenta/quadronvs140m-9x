# Hardware 3D on the G86 (native OpenGL/Direct3D acceleration) - plan

Goal: render OpenGL (and later D3D) on the Quadro NVS 140M's own 3D engine
instead of on the CPU (Mesa llvmpipe). This is the largest piece of the whole
project and a multi-stage hardware bring-up on the real T61, in the same style
as the EVO display bring-up (docs/EVO_PLAN.md, builds 3001-3010).

## CURRENT STATUS - RESUME HERE (as of build 3074, 2026-10-24)

The GPU bring-up and the winsys foundation are DONE and verified on the T61.
What is built, in order, each released and tested:

- M5.1  (v3061): the 2D engine (0x502d) executes commands and fills VRAM.
- M5.2a (v3064): channel VIRTUAL MEMORY - page directory @RAMIN+0x200 + a VRAM
  page table translate a GPU virtual address to physical VRAM.
- M5.2b (v3067): a VRAM HEAP allocator + a GPU FENCE (a 2D write of a sequence
  value to a heap BO via the VM; CPU polls it).
- M5.3a (v3074): a PERSISTENT CHANNEL that RING-3 submits command streams to,
  using nv50 INDIRECT-BUFFER (IB) mode. `nvprobe submit` proves user space drives
  the GPU (two fills, different colors, both land).

All the mechanism facts (register values, instance layout, IB entry format, the
DMA-pusher dead end, VM/PTE/PDE formats, the grctx) are in the auto-memory file
hw3d-gpu-accel.md - READ IT before continuing.

Test commands on the T61 (opt-in, touch only PGRAPH/PFIFO):
  nvprobe gpu2d | vmtest | fence | submit

### NEXT: M5.3b - the libdrm_nouveau shim (the task to start)
Implement the ~25-function libdrm_nouveau contract in a NEW ring-3 source compiled
into mesa3d.dll, built on the VxD ioctls we now have (OP_NV_SUBMIT 0x5013,
OP_NV_VRAM_READ 0x5014) plus new ones to add (BO alloc from the heap + CPU map via
BAR1, VM-map, fence emit/wait). Functions Mesa's nv50 winsys calls:
  nouveau_device_*, nouveau_object_new/del, nouveau_bo_new/ref/map/unmap/wait,
  nouveau_pushbuf_new/space/data/refn/kick/validate, nouveau_bufctx_*,
  nouveau_fence_new/ref/signalled/wait, nouveau_client_new.
Reference contracts: /home/guilherme/projects/mesa9x-build/mesa9x/mesa-24.1.x/src/
gallium/{winsys,drivers}/nouveau (nouveau_fence.c, nv50_screen.c fence.emit uses the
3D QUERY method - see M5.4 note). The persistent channel's command encoding is
header=(count<<18)|(subc<<13)|method; submit via OP_NV_SUBMIT.
Then M5.3c: wire Mesa's nv50 screen create to the shim and get it to initialise.
Then M5.4: full NV84_3D (0x8297) bring-up (the 3D QUERY fence TRAPs today, needs the
~15 DMA-object + register setup nv50_screen does) and wire Mesa as the GL ICD ->
Half-Life on the GPU.

Build/test loop: edit under driver/vmdisp9x-nv; build VxD with /home/guilherme/watcom,
DRV with /home/guilherme/watcom-2025-08, nvprobe with i686-w64-mingw32-gcc into
tools/; run tools/nerelocs.py on the DRV; sh tools/make_dist.sh; bump NV_BUILD in
vxd_nv.h + init.c. User flashes dist/driver to the T61, runs nvprobe, confirms;
then commit + push + `gh release create vNNNN <zip> --title ... --notes-file ...
--target main`. Commit attribution: Co-Authored-By: Claude Opus 4.8.

## Why it is big, and why it is feasible

The G86 is an NV50-family (Tesla) GPU. Its 3D engine is class 0x8297
(G84_3D / "NV84_3D"), fed - like the 2D (0x502d) and M2MF copy (0x5039)
engines - through a PFIFO DMA pushbuffer channel (PGRAPH), which is a
different channel type from the EVO *display* channel we already drive.

The whole **userspace** stack already exists and we already compile it:
Mesa's nouveau nv50 Gallium driver (`src/gallium/drivers/nouveau/nv50`, ~20k
lines) turns OpenGL state into nv50 hardware methods, and Mesa's nv50 codegen
compiles GLSL to the Tesla shader ISA. We do not write any of that.

What does NOT exist for Windows 9x is the **kernel half**. On Linux, nouveau's
Mesa winsys calls libdrm_nouveau, which calls the kernel nouveau DRM for GPU
channels, memory, command submission and fences. On the T61 there is no such
kernel, so our VxD must provide those services, and a small Win9x winsys must
map libdrm_nouveau onto them. That winsys contract is ~25 functions:

    device/client:  nouveau_drm_new, nouveau_device_new, nouveau_client_new (+del)
    objects:        nouveau_object_new/del      (channel + engine classes via RAMHT)
    GPU memory:     nouveau_bo_new/ref/map/wait/wrap   (VRAM heap + GART)
    bufctx:         nouveau_bufctx_new/del/refn        (per-draw BO reference bins)
    submission:     nouveau_pushbuf_new/space/data/reloc/refn/kick/validate
    fences:         nouveau_fence_new/ref/wait/kick     (semaphore write + poll)

We already know a lot of the hardware from EVO: RAMHT hashing, ctxdma objects,
instance memory (RAMIN) layout, channel bring-up and notifier-polling for
sync (we have no serviceable display IRQ, so 3D sync is by polling too).
The genuinely new VxD pieces are (a) a GPU memory manager (VRAM heap + a GART
page table so the GPU can read system-memory buffers) and (b) a PFIFO DMA
channel with pushbuffer submission + relocation patching.

## Milestones

- **M5.1 - PFIFO channel + the 2D engine (first PGRAPH command).** Bring up a
  nv50 PFIFO DMA pushbuffer channel, bind the 2D class (0x502d) through RAMHT,
  and submit a solid-fill / copy of a rectangle in VRAM. Verify by reading the
  region back with `nvprobe`. Proves we can drive PGRAPH at all; reuses the EVO
  RAMHT/ctxdma/instance-memory/channel-init machinery. Opt-in IOCTL, no effect
  on the desktop. **This is the first testable step.**
- **M5.2 - GPU memory manager + fences.** A VRAM heap allocator and a GART
  (system pages mapped into the GPU address space) in the VxD, plus
  semaphore-based fence writes polled from the CPU. Enough to back
  nouveau_bo_* and nouveau_fence_*.
- **M5.3 - Win9x nouveau winsys.** Implement the ~25-function contract against
  the VxD services; compile Mesa's nv50 Gallium driver into mesa3d.dll
  (selected instead of llvmpipe when the G86 is present).
- **M5.4 - 3D class bring-up + wire as the ICD.** NV84_3D (0x8297): render
  target, clear, one shaded triangle through the real pipeline; then hand the
  hardware ICD to opengl32 and run Half-Life on the GPU.

Each milestone is cold-boot tested on the T61 and can black-screen the 3D
engine (never the display channel, which stays independent), so every step is
opt-in and reversible. Hardware references: Mesa's nv50 *.xml.h method headers
(in the Mesa tree we build), envytools NV50 docs, nouveau, and the XP
nv4_mini.sys disassembly.

## Honest expectation

This is weeks of staged, iterative bring-up on real hardware, not a single
change. The payoff is real GPU rendering. The fallback (software llvmpipe)
stays the default until M5.4 proves out, so nothing that works today regresses.

## M5.1 recipe (derived from nouveau v2.6.36 + envytools + reused EVO machinery)

References pulled 2026-10-23 via `gh api` (nouveau Linux v2.6.36: nv50_fifo.c,
nv50_instmem.c, nv50_graph.c, nouveau_channel.c, nouveau_object.c,
nouveau_reg.h) and the envytools method headers now in analysis/nv50_ref/
(nv_object.xml.h, nv_m2mf.xml.h, nv50_2d.xml.h, nv50_3d.xml.h). Kept in the
scratchpad, NOT committed (GPL kernel source); only our derived recipe and the
MIT envytools headers live in the tree.

Goal of M5.1: bring up one PFIFO DMA channel, bind the 2D engine (0x502d),
solid-fill a rectangle in VRAM, read it back with nvprobe. Opt-in IOCTL
(OP_NV_GPU_2DFILL), never touches the display channel. Reuses our existing
RAMHT (ramht_add/ramht_hash), ctxdma (build_ctxdma), instance memory and
NV_vram_* helpers from nv50_evo.c.

### Registers (BAR0), from nouveau_reg.h
- PFIFO channel USER control:  NV50_USER(c)      = 0x00c00000 + c*0x2000
    DMA_PUT = 0x00c00040 + c*0x2000,  DMA_GET = 0x00c00044 + c*0x2000
- Context table (per channel):  NV50_PFIFO_CTX_TABLE(c) = 0x2600 + c*4
    bit31 CHANNEL_ENABLED; value = ramfc_instance >> 8 (G84/G86) | enabled
- PFIFO caches/init: 0x2500 (CACHES: 0 off, 0x101 on), 0x250c=0x6f3cfc34,
    0x3250/0x3220/0x3204/0x3210/0x3270 = 0, INTR_0 0x2100 = 0xFFFFFFFF,
    INTR_EN_0 0x2140 = 0xFFFFFFFF.
- PMC enable 0x200: ensure PFIFO (bit8) + PGRAPH (bit12) on (already on after
    the EVO path runs, but assert it).

### Instance memory layout (extend our RAMIN region in VRAM)
We already own RAMIN at NVFB_RAMIN_VRAM_OFFSET with RAMHT + two ctxdmas for
EVO. Add, in free RAMIN space, for the GPU channel (all offsets are byte
offsets into the channel's RAMIN block; nv50 chan RAMIN is 0x1000-aligned):
- channel RAMFC at +0x0000 (0x100 bytes), RAMFC fields (dword offsets):
    0x48 = pushbuf_ctxdma_instance >> 4      (pushbuffer ctxdma, class 0x3d)
    0x80 = (0<<27)|(4<<24)|(ramht_instance >> 4)   /* 4 KiB, SEARCH_FULL */
    0x44 = 0x2101ffff
    0x60 = 0x7fffffff
    0x40 = 0
    0x7c = 0x30000001
    0x78 = 0
    0x3c = 0x403f6078
    0x50 = pushbuf_base (byte offset of the first method in the pushbuf DMA)
    0x54 = order(ib_max+1)<<16   (0 if not using an indirect buffer)
  G84/G86 ALSO:  ramin[0]=chan_id, ramin[1]=ramfc_instance>>8,
    ramfc 0x88 = cache_instance>>10, ramfc 0x98 = ramin_instance>>12
- FIFO cache at +0x0400 (4 KiB)
- RAMHT: reuse ours (methods are bound by handle through it, like EVO).
- pushbuffer: a buffer in VRAM; a ctxdma (class 0x3d) spanning it (or all
    VRAM, like NvEvoVRAM_LP) is referenced by RAMFC 0x48. We use DIRECT DMA
    (no IB): write methods into the pushbuffer, set DMA_PUT = byte length,
    hardware runs GET->PUT, poll GET==PUT. Same model as our EVO ring.

### Channel bring-up (direct-DMA)
1. Assert PMC 0x200 bits for PFIFO+PGRAPH. nv50_graph must be initialised
   (nv50_graph.c nv50_graph_init: PGRAPH reset via 0x200, ctxprog upload) -
   for M5.1 the 2D engine needs PGRAPH context; confirm from nv50_graph.c how
   much init the 2D object needs (may need the ctxctl microcode load; if so,
   M5.1 grows - verify before coding).
2. Build RAMFC (above) + pushbuffer ctxdma over VRAM.
3. Bind engine objects through RAMHT (ramht_add), like EVO:
     handle 0xbeef502d -> ctx for 2D class 0x502d
     (later 0xbeef5039 M2MF 0x5039, 0xbeef8297 3D)
   nv50 object ctx value in RAMHT entry for an engine object encodes the
   oclass + engine; derive the exact ctx dword from nouveau_object.c
   (nv50_gpuobj_class / the RAMHT entry format) - TODO confirm.
4. CTX_TABLE(c) = (ramfc_instance>>8) | CHANNEL_ENABLED; playlist update
   (nv50_fifo_playlist_update writes the list of active chids to a VRAM
   buffer and pokes 0x32f4/0x32ec - get exact regs from nv50_fifo.c).
5. CACHES on: 0x2500 = 0x101.

### 2D solid fill method stream (nv50_2d.xml.h), written to the pushbuffer
Header per batch = (count<<18)|(subchannel<<13)|method, subchannel 0 bound to
the 2D object via a BIND (0x0000 = SET_OBJECT, data = handle 0xbeef502d).
  0x0000 SET_OBJECT        = 0xbeef502d
  0x0184 DMA_DST           = dst ctxdma handle (VRAM)
  0x0200 DST_FORMAT        = e.g. 0x000000e5 (A8R8G8B8) or a 16bpp fmt
  0x0204 DST_LINEAR        = 1
  0x0214 DST_PITCH         = pitch bytes
  0x0218 DST_WIDTH         = w
  0x021c DST_HEIGHT        = h
  0x0220 DST_ADDRESS_HIGH  = (dst>>32)
  0x0224 DST_ADDRESS_LOW   = dst (byte offset in the VRAM ctxdma)
  0x02ac OPERATION         = SRCCOPY (0x3) for an opaque fill
  0x0580 DRAW_SHAPE        = 4 (RECTANGLES)
  0x0584 DRAW_COLOR_FORMAT = same family as DST_FORMAT
  0x0588 DRAW_COLOR        = fill colour
  0x0600 DRAW_POINT32_X(0) = x0 ; 0x0604 DRAW_POINT32_Y(0) = y0
  0x0608 DRAW_POINT32_X(1) = x1 ; 0x060c DRAW_POINT32_Y(1) = y1
Then DMA_PUT = stream_bytes; poll DMA_GET==DMA_PUT; read back VRAM.

### RESOLVED: PGRAPH needs the chip-specific context microcode (the big one)

On NV50 every data-moving/rendering engine - 2D (0x502d), M2MF (0x5039) and
3D (0x8297) - is a PGRAPH object, and PGRAPH executes NOTHING until:
  (a) nv50_graph_init_ctxctl uploads a "ctxprog" context-switch microcode to
      PGRAPH_CTXCTL (NV40_PGRAPH_CTXCTL_UCODE_INDEX/_DATA), and
  (b) each channel has a per-channel graphics context (ramin_grctx) seeded with
      chip-specific "ctxvals", referenced from the channel RAMIN header.
Both come from nouveau's nv50_grctx.c (nv50_grctx_init) - 2383 lines,
table-driven, and it explicitly handles chipset 0x86 (`case 0x84: case 0x86:`).
No shortcut: even a 2D fill or an M2MF copy needs it, because the engine's
thousands of state registers are set up by loading the initial context. So:

  **M5.1a - port nv50_grctx.c for the G86 (chipset 0x86).** Reimplement the
  ctxprog + ctxvals generator in the VxD (or precompute both on the host from
  the ported generator and ship them as blobs). ~2.4k lines of portable C with
  little kernel dependency; cross-check the generated G86 context against what
  the XP nv4_mini.sys loads. This is the crux of nv50 bring-up.
  **M5.1b** - then the PFIFO channel + 2D fill recipe above.

### Other open items (smaller)
- Exact RAMHT ctx-dword format for an nv50 ENGINE object (nouveau_object.c).
- nv50_fifo_playlist_update exact registers (0x32f4 / 0x32ec area).
- DST_FORMAT/DRAW_COLOR_FORMAT enum values (g80_defs.xml.h).

The single biggest unknown is now de-risked: the context generator exists as
portable C in nouveau and covers the G86, so M5.1a is a porting job, not a
research dead-end.

## M5.1a DONE + full M5.1b recipe (2026-10-23)

M5.1a (context microcode) is DONE. nouveau v2.6.36 nv50_grctx.c was ported to
run on the host unmodified (scratchpad tools/grctx_host: drmP.h/nouveau_drv.h
stubs + main.c; `gcc main.c nv50_grctx.c`). The T61's G86 reports
0x1540 = 0xF3010001 (read with `nvprobe rd 1540`). Generated for
chipset 0x86 + that units value: ctxprog = 110 dwords, grctx_size = 378112
bytes (94528 dwords, 660 non-zero). Baked into driver/vmdisp9x-nv/
nv50_grctx_g86.h (derived data, like the EVO streams). Regenerate for another
board by reading its 0x1540 and re-running the host tool.

### M5.1b - PGRAPH init + channel + 2D fill (all registers derived)

PGRAPH init (BAR0), from nv50_graph.c, do once:
- reset: PMC_ENABLE(0x200) &= ~((1<<12)|(1<<21)); then |= it (toggle PGRAPH).
- init_regs__nv: 0x400804=0xc0000000, 0x406800=0xc0000000, 0x400c04=.., 0x401800,
  0x405018, 0x402000 all 0xc0000000; per-unit (chipset<0xa0, i in enabled set)
  0x408900+(i<<12),0x408e08+(i<<12),0x408314+(i<<12)=0xc0000000 and
  0x408600+(i<<11),0x408708+(i<<11),0x40831c+(i<<11)=0xc0000000; then
  0x400108=0xffffffff, 0x400824=0x00004000, 0x400500=0x00010001.
- init_regs: 0x400be0(NV04_PGRAPH_DEBUG_3)=(1<<2) HW_CONTEXT_SWITCH_ENABLED;
  0x402ca8=0x800.
- init_ctxctl: NV40_PGRAPH_CTXCTL_UCODE_INDEX=0; loop NV40_PGRAPH_CTXCTL_UCODE_DATA
  = nv50_ctxprog[i] (110 of them); 0x400320=4; CTXCTL_CUR=0;
  CHANNEL_CTX_POINTER=0.  (CTXCTL_UCODE_INDEX/_DATA, CTXCTL_CUR regs from
  nouveau_reg.h - grab exact addrs.)

Channel (reuse EVO ramht_hash/build_ctxdma/NV_vram_* ; own RAMIN block):
- pushbuffer ctxdma (class 0x3d) over VRAM (like NvEvoVRAM_LP).
- RAMFC (dword offsets): 0x48=pushbuf_ctxdma_inst>>4; 0x80=(0<<27)|(4<<24)|
  (ramht_inst>>4); 0x44=0x2101ffff; 0x60=0x7fffffff; 0x40=0; 0x7c=0x30000001;
  0x78=0; 0x3c=0x403f6078; 0x50=pushbuf_base(byte off of 1st method); 0x54=0
  (no IB). G86 also: ramin[0]=chid(0), ramin[1]=ramfc_inst>>8, ramfc 0x88=
  cache_inst>>10, ramfc 0x98=ramin_inst>>12.
- grctx: 378112-byte VRAM block, 0x1000-aligned, ZEROED, then apply the 660
  nv50_ctxvals pairs; grctx[0]=ramin_inst>>12. Channel RAMIN header at byte
  0x20: +0x00=0x00190002; +0x04=grctx_inst+grctx_size-1; +0x08=grctx_inst;
  +0x0c=0; +0x10=0; +0x14=0x00010000.
- 2D object instance (class 0x502d): small block (class_instmem_size; use 6
  dwords), obj[0]=0x502d, obj[5]=0x00010000, engine=GR(1). RAMHT entry:
  handle=0xbeef502d, ctx=(obj_inst>>4)|(1<<20)  [NV40_RAMHT_CONTEXT_ENGINE_SHIFT
  =20, graphics engine=1]. hash = fold(handle, ramht_bits)<<3 (NO channel xor on
  nv50) - our ramht_hash already does this.
- enable: CTX_TABLE(0)=(ramfc_inst>>8)|(1<<31); playlist update (write active
  chids to a VRAM list, poke the playlist regs - get exact from nv50_fifo.c
  nv50_fifo_playlist_update); CACHES 0x2500=0x101.

2D solid fill into the pushbuffer (header=(count<<18)|(subc<<13)|method,
subc 0): 0x0000 SET_OBJECT=0xbeef502d; 0x0184 DMA_DST=pushbuf/VRAM ctxdma
handle; 0x0200 DST_FORMAT; 0x0204 DST_LINEAR=1; 0x0214 PITCH; 0x0218 W; 0x021c
H; 0x0220/0x0224 DST_ADDR hi/lo; 0x02ac OPERATION=SRCCOPY; 0x0580 DRAW_SHAPE=4;
0x0584 DRAW_COLOR_FORMAT; 0x0588 DRAW_COLOR; 0x0600/0x0604 POINT32(0)=x0,y0;
0x0608/0x060c POINT32(1)=x1,y1. Then USER_DMA_PUT(0)(0xc00040)=bytes; poll
USER_DMA_GET(0)(0xc00044)==PUT; read back the VRAM rect with nvprobe.

Opt-in IOCTL OP_NV_GPU_2DFILL; nvprobe `gpu2d`. Never touches the EVO display
channel. A fault parks PGRAPH, recover by cold boot (like EVO bring-up).

## M5.2 recipe - the NV50 GPU virtual memory (derived 2026-10-24)

M5.1 proved the 2D engine with a PHYSICAL ctxdma (no VM). Mesa's nv50 3D driver
addresses all buffers by GPU VIRTUAL address, so M5.2 brings up the channel VM.
Derived from nouveau v2.6.36 (nv50_instmem.c, nouveau_object.c
nouveau_gpuobj_channel_init, nouveau_mem.c nv50_mem_vm_bind_linear,
nouveau_drv.h). Constants: NV50_VM_BLOCK = 512 MiB (coverage per PDE),
NV50_VM_MAX_VRAM = 2 GiB, 64 KiB pages, 8-byte PTEs.

VM address layout (one PDE = 512 MiB):
  PDE 0  (VM 0)        = PRAMIN/instance   (nouveau "cheat": instance at VM 0)
  PDE 2  (VM 512 MiB)  = GART   (vm_gart_base)      [skip for M5.2a]
  PDE 4  (VM 1 GiB)    = VRAM PT[0] (vm_vram_base); PDE 6/8/10 = PT[1..3] for 2 GiB
  So a VRAM BO at physical P maps 1:1 to VM address (1 GiB + P).

Page directory (per channel): 0x4000 bytes (2048 PDEs x 8). Lives INSIDE the
channel RAMIN at offset 0x200 (G84/G86; 0x1400 on G80). Init every PDE to
{0, 0xdeadcafe}. Then:
  PDE dword (vm_vram_base/512MiB)*2 = 4  ->  nv_wo32(pd, 4, vram_pt_inst | 0x61)
                                             nv_wo32(pd, 5, 0)
  (GART PDE dword 2 = gart_pt | 0x03, when GART is added.)

VRAM page table: covers 512 MiB => 512MiB/64KiB = 8192 PTEs x 8 = 64 KiB.
Map VM page -> physical VRAM page P (nv50_mem_vm_bind_linear, VRAM case):
  pte_index (dword) = ((VM_addr - vm_vram_base) >> 16) << 1
  pte_lo = (P & 0xffffffff) | 1          (bit0 = present; flags 0 for linear)
  pte_hi = upper_32_bits(P)              (0 for our <4 GiB VRAM)
  (GART/system pages instead OR in 0x30 and add vram_sys_base.)

ctxdma that targets the VM (for engine DMA_DST/SRC etc. to use VM addresses),
vs the physical VIDMEM ctxdma M5.1 used:
  VM/AGP target:  dword0 = 0x7fc00000 | class(0x3d),  dword5 = 0x00080000
  VIDMEM target:  dword0 = 0x00190000 | 0x3d,         dword5 = 0x00010000  (M5.1)
The engine translates a VM ctxdma's offset through the channel's PD at RAMIN+0x200.

VM config / BAR regs (nouveau_reg.h): NV50_PUNK_BAR0_PRAMIN 0x1700 (CPU PRAMIN
window, =pramin_phys>>16), NV50_PUNK_BAR_CFG_BASE 0x1704 (=ramin>>12 |VALID<<30),
NV50_PUNK_BAR1_CTXDMA 0x1708, NV50_PUNK_UNK1710 0x1710. These set up BAR1/PRAMIN
instance access for the dummy channel; a graphics channel's own VM is found via
its RAMIN PD at +0x200.

### M5.2a - first test: a VM-addressed 2D fill
Reuse the M5.1 channel. Add: a VRAM page table; the channel PD at RAMIN+0x200
with PDE 4 -> the PT; map the scratch region into the VM at (1 GiB + scratch);
a VM-target ctxdma; redo the 2D fill with DST_ADDR = the VM address and
DMA_DST = the VM ctxdma. Read back the scratch PHYSICALLY: if it holds the fill
colour, the VM translated our virtual write correctly. OPEN: whether PDE->PT and
the PD itself are referenced physically or PRAMIN-relative (M5.1 showed PGRAPH
ctx = physical, PFIFO objects = channel-relative; the hardware test decides).
Then M5.2b: a VRAM heap allocator + semaphore fences; M5.3: the Mesa winsys.
