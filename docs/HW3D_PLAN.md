# Hardware 3D on the G86 (native OpenGL/Direct3D acceleration) - plan

Goal: render OpenGL (and later D3D) on the Quadro NVS 140M's own 3D engine
instead of on the CPU (Mesa llvmpipe). This is the largest piece of the whole
project and a multi-stage hardware bring-up on the real T61, in the same style
as the EVO display bring-up (docs/EVO_PLAN.md, builds 3001-3010).

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

### Open items to confirm before coding (from the fetched sources)
- Exact RAMHT ctx-dword format for an nv50 ENGINE object (nouveau_object.c).
- Whether the 2D/PGRAPH engine needs the ctxctl microcode (nv50_graph.c /
  the external nv50 grctx) just to run 2D - if yes, M5.1 must upload it.
- nv50_fifo_playlist_update exact registers.
- DST_FORMAT/DRAW_COLOR_FORMAT enum values (g80_defs.xml.h).

If 2D needs the full grctx microcode, fall back to M2MF (0x5039) for the first
"does PGRAPH execute our methods" test - M2MF is a simpler copy engine.
