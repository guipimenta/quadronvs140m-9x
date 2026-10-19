# EVO bring-up plan (M3 native scanout)

Goal: scan out our own linear framebuffer so Windows renders a real high-color
1024×768 desktop, replacing the VESA/VGA path that falls back to 640×480 / 16
colors. Method map in `nv50_evo.h`; MMIO/clock/SOR map in `nv50_disp.h`.

## How NV50 display works (the constraint)

The G86 display is driven by **EVO DMA channels** (MMIO `CHANNEL[0..4]` at
0x610200). The *core* (master) channel does modeset and per-head scanout. You
don't poke scanout registers directly; you:

1. Give the channel a **pushbuffer** in VRAM and a **DMA control block** (the
   `DMA_CB` register at 0x610204 for ch0: bit0 VALID, bit1 LOCATION 0=VRAM).
2. Give the channel **DMA context objects** ("ctxdmas") that describe the
   framebuffer and notifier memory; head methods reference them by *handle*
   (`DMA_ISO` 0x874, `DMA_NOTIFIER` 0x88). These live in display instance memory.
3. Write method/data pairs into the pushbuffer, advance the channel PUT pointer,
   and commit with `UPDATE` (method 0x80 = 0). Poll the notifier for completion.

The ctxdma/instance-memory setup is the hard, error-prone part; a bad method or
handle makes the channel trap (`PDISPLAY TRAPPED_ADDR/DATA` 0x610080/84).

## Current live state (decoded from the c1/c2/c3 dumps, build 2006)

- `0x610010` OBJECTS = `0x00000001` → display instance-memory base = 0, enabled.
  (nouveau encodes this reg as `(ramin_vram_addr >> 8) | 9`; the VBIOS used
  flags `1`, base 0 → its display instance memory sits at ~VRAM offset 0.)
- CHANNEL array `0x610200`: **ch0 (core) STAT = 0x49070009 — bit4 DMA_ENABLED is
  CLEAR.** `DMA_CB`=1 (VALID,VRAM) but DMA is NOT running. ch1-4 STAT=0x40000008
  (present, idle). PUT/GET (`0x640000`/`4`) = 0/0.
  → **The VBIOS drove its modeset in PIO mode, not via a DMA pushbuffer.** There
  is no live DMA channel to piggy-back on; E3 must bring up the core channel
  itself (exactly what nouveau does on init — it never reuses VBIOS channel state).
- Head0 active: raster 1344×806, 1024×768 visible, pixel clock 64.8 MHz, SOR0.

## The E3 recipe is now fully specified (authoritative)

Reverse-derived in full from nouveau Linux v3.4 (MIT), cross-checked against the
envytools rnndb and our live dumps. Source files (for provenance, not copied in):
`nv50_evo.c`, `nv50_display.c`, `nouveau_object.c`, `nouveau_ramht.c`,
`nouveau_display.c`, `nouveau_reg.h`, `nv50_evo.h`, `nv50_crtc.c`.

**Objects to build in VRAM** (place the instance block HIGH in VRAM, away from
the VBIOS region near 0 and away from our scanout fb; 64 KiB-aligned; zero it):

1. **ramin** (display instance memory), ≥0x8000 bytes. RAMHT at ramin+0.
2. **RAMHT** (4096 B, zeroed, 512 × 8-byte slots → `bits`=9).
3. **ctxdma** (one linear surface covering all VRAM), 6 dwords at e.g. ramin+0x1000.
   For G86 (chipset 0x86, <0xc0), handle **NvEvoVRAM_LP = 0x01000003**, class 0x3d:
   - `+0x00 flags0 = 0x0019003d`  (class 0x3d | RW 0x80000 | VRAM 0x10000 | 0x100000)
   - `+0x04 limit_lo = vram_size-1`  (read `RAM_AMOUNT` 0x610384 = vram_size-1)
   - `+0x08 base_lo  = 0`
   - `+0x0c = 0`  (hi_limit<<24 | hi_base, both 0 for <4 GiB)
   - `+0x10 = 0`
   - `+0x14 flags5 = 0x00010000`
4. **RAMHT entry** for the ctxdma. Hash(handle, bits=9): xor 9-bit folds of the
   handle, then `<<3`. For 0x01000003 → slot **0x218**. If occupied, step +8.
   - slot+0x00 = handle = 0x01000003
   - slot+0x04 = ctx = `(cinst << 10) | (id << 28) | id`, cinst = ctxdma's
     ramin-relative byte offset (0x1000), id=0 → **0x00400000**.
5. **pushbuffer**, 4096 B in VRAM (for the method stream).

**Core-channel bring-up** (`nv50_evo_channel_init`, id=0; regs are id*0x10+base):
```
0x610204 DMA_CB   = (pushbuf_vram>>8) | 0x1           ; VALID, VRAM
0x610208 UNK2     = 0x00010000
0x61020c HASH_TAG = 0x00000000                         ; = id
0x610200 CTRL    |= 0x00000010                         ; DMA_ENABLED (mask)
0x640000 USER_PUT = 0x00000000
0x610200 CTRL     = 0x01000013                         ; 0x01000003 | DMA_ENABLED
wait (0x610200 & 0x80000000) == 0                      ; channel accepts
0x610028         |= 0x00010001                         ; enable error reporting (ch0)
0x610010 OBJECTS  = (ramin_vram>>8) | 9                ; point engine at OUR objects
```

**Scanout method stream** (push into pushbuffer, head0; header=(count<<18)|method):
```
0x874 FB_DMA    (1) = 0x01000003                       ; NvEvoVRAM_LP
0x860 FB_OFFSET (2) = fb_vram_offset >> 8 , 0          ; NB: >>8, two dwords
0x868 FB_SIZE   (1) = (768<<16)|1024      = 0x03000400
0x86c FB_CONFIG (1) = 0x00100000 | pitch  = 0x00100800 ; MODE_PITCH(linear)|2048
0x870 FB_DEPTH  (1) = 0x0000e800                       ; DEPTH_16 (R5G6B5)
0x080 UPDATE    (1) = 0                                 ; commit
```
Then advance `0x640000 USER_PUT` past the stream. Inherit CLUT, raster, VPLL and
SOR0 from the VBIOS (don't push CLUT_MODE/CLOCK — that's M3b). For 32bpp use
FB_DEPTH_24 = 0x0000cf00 and pitch = 4096 → FB_CONFIG = 0x00101000.

### E2 encoding bugs this research exposed (fix before E3)

E2 "verified byte-for-byte" only against *our own* reference, which was wrong:
- **FB_OFFSET must be `byte_offset >> 8`.** E2 would write the raw byte offset
  (256× too high) for any fb not at 0.
- **FB_CONFIG = `0x00100000 | pitch_bytes`** (linear MODE bit + raw byte pitch).
  E2 used `0x8000` (pitch>>4<<8, MODE=blocklinear) — wrong for a linear surface.
- **FB_DMA handle** must be a registered ctxdma handle (0x01000003), not the
  invented 0xcafe0001.
- FB_DEPTH 0xe800 and FB_SIZE/REAL_RES 0x03000400 were correct.

## Staged, safest-first

- **E1/E2 (done, VRAM-only, safe).** Framebuffer + method stream staged in VRAM.
- **E3 (RISKY — first black-screen possibility).** Build the objects above, bring
  up ch0, push the scanout stream, UPDATE. Make it **self-diagnosing**: snapshot
  every MMIO reg before writing; after UPDATE read back `0x610080/84`
  TRAPPED_ADDR/DATA, `0x610024` INTR, `0x610200` STAT, PUT/GET, and return them
  via IOCTL. Make it **opt-in and non-persistent**: gate the flip behind an
  explicit IOCTL (not Device_Init) so a bad attempt never wedges every boot;
  recovery = reboot → Safe Mode → Standard VGA.
- **E4.** Wire the new framebuffer into DIBENG (FBHDA) at 16/32bpp; set depth in INF.

## Validate on silicon before writing E3 (read-only, zero risk)

Dump the VBIOS's live display instance memory to confirm the ctxdma/RAMHT format
on *this* G86 and to locate the region to avoid:
- `nvprobe vramrd 0x0 32`     — RAMHT slots (handle, ctx pairs) at base 0
- `nvprobe vramrd 0x1000 16`  — ctxdma objects (look for `…003d` flags0 words)
- `nvprobe rd 0x610384 1`     — RAM_AMOUNT = vram_size-1 (the ctxdma limit)
