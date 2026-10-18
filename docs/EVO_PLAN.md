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

## Current live state (decoded from the dumps, no new hardware trip)

- Core channel ch0 `DMA_CB` (0x610204) = 1 → VALID, VRAM: the VBIOS already set
  up a core-channel pushbuffer. Channels 1-4 read default/idle.
- Head0 active: raster 1344×806, 1024×768 visible, pixel clock 64.8 MHz, output
  SOR0. Scanout is a linear framebuffer at BAR1 offset 0 (we can write it).

## Staged, safest-first

- **E1 (read-only, safe).** Decode the running core channel: PUT/GET, its
  pushbuffer location (DMA_CB), the notifier, and the current head FB
  method state. Confirms whether we can reuse the VBIOS channel or must make our
  own. No writes. (nvprobe `rd` already covers the MMIO; add decode.)
- **E2 (VRAM-only, safe).** Allocate in VRAM: our 1024×768 framebuffer, a
  pushbuffer, a notifier. Build the method stream (SET DMA_ISO handle, FB
  offset/size/storage/params, viewport, UPDATE) in VRAM. Verify by reading it
  back. Nothing is pointed at hardware yet, so the display is untouched.
- **E3 (RISKY — first black-screen possibility).** Point the core channel at
  our pushbuffer (or inject into the running one), submit the scanout methods,
  UPDATE, poll the notifier. If the panel goes black: reboot → Safe Mode →
  Standard VGA (unchanged fallback). Keep a watchdog: if the notifier doesn't
  signal, don't touch anything else.
- **E4.** Wire the new framebuffer into the DIBENG display driver (FBHDA) so
  Windows renders into it at 16/32bpp, and set the color depth in the INF.

## First framebuffer takeover (head0, reuse raster/clock/SOR)

Minimal core-channel method sequence (per `nv50_evo.h`, head0):
`DMA_ISO(0x874)=fb_handle; FB_OFFSET(0x860)=0; FB_SIZE(0x868)=1024|768<<16;`
`FB_STORAGE(0x86c)=pitch(2048)>>4<<8 | layout; FB_PARAMS(0x870)=R5G6B5(0xe8);`
`VP_SIZE_IN(0x8c8)=1024|768<<16; UPDATE(0x80)=0`. Reuse the existing raster,
VPLL and SOR0 (don't reprogram them yet — that's M3b).

## Open items to confirm before E3

- `G80_HEAD_STORAGE_LAYOUT` value for pitch/linear vs block-linear.
- Exact ctxdma entry format for the G86 display instance memory, and whether we
  can reuse the VBIOS-created core channel + just add our ISO ctxdma, vs. build
  a fresh channel (nouveau builds fresh; reuse is less code but less understood).
- Pixel clock method (0x804) units if we ever reprogram it (kHz).
