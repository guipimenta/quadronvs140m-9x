# M3 — native G86 modeset / framebuffer

Target recovered from the live M2 dump: the T61 panel is **1024×768 @ 60 Hz**
(htotal 1344, vtotal 806), driven on **head0**; head1 is disabled. VRAM aperture
BAR1 = 256 MB @ 0xE0000000, MMIO BAR0 = 16 MB @ 0xDE000000. GPU = G86 rev A2.

## The key architectural fact

NV50/G8x does **not** set modes by poking CRTC registers. It uses **EVO**: a set
of DMA channels (`PDISPLAY` `CHANNEL[0..4]` at MMIO 0x610200) that consume a
pushbuffer of method/data pairs from VRAM and commit them with the `UPDATE`
(0x80) method. Channel 0 is the *core* channel (modeset: head raster, output
routing, VPLL); channel 1 is the *base* channel (scanout address/format/pitch).

So native display work means:
1. Allocate a small pushbuffer in VRAM (BAR1 window) + a DMA object/ctxdma.
2. Bind it to an EVO channel via `PDISPLAY_CHAN_DMA_CB` and start the channel.
3. Push methods, then `UPDATE`, and wait for the channel to go idle.

This is the same mechanism nouveau's `nv50_disp` uses.

## Staged approach (lowest risk first)

Because the panel is *already lit* at 1024×768 by the VBIOS, we don't have to
compute a mode blind and risk a black screen. Stages:

- **M3a — framebuffer takeover (reuse the live raster).** Keep the VBIOS head
  raster, VPLL and SOR/LVDS exactly as they are. Only bring up the base/core EVO
  channel enough to repoint scanout at our own linear framebuffer in VRAM and set
  the pixel format (16bpp/32bpp) and pitch. Result: a proper-colour 1024×768
  desktop, wired to the existing DIBENG display driver. No PLL/SOR reprogramming.
  Safe: if it fails, the VBIOS mode is untouched and VGA fallback still works.
- **M3b — full native modeset.** Program the head raster (EVO RASTER methods),
  VPLL (coefficients from nvdump2), and SOR/LVDS link ourselves, so we can set
  the mode from cold and support mode changes. Needs the nvdump2 clock/SOR data.

## Register map

See `driver/vmdisp9x-nv/nv50_disp.h` (offsets from envytools g80_pdisplay.xml /
nv_evo.xml, confirmed against the T61 dump). VPLL pixel clock decodes as
`refclk * N / (M << P)` from `VPLL_S1`/`VPLL_S2`; nvdump2 captures the live
coefficients so M3b can reproduce 65 MHz exactly.

## Live values recovered (nvdump2, 2026-10-18)

- **Pixel clock = 64.8 MHz** (≈65, the 1024×768@60 clock). VPLL head0:
  `VPLL_S1 0x614104 = 0x00020010` → N1=16 M1=2; `VPLL_S2 0x614108 = 0x3005000C`
  → N2=12 M2=5 P=3. Two-stage: 27MHz × 16/2 × 12/(5<<3) = 64.8 MHz (refclk 27).
- **Panel output = SOR0.** `SOR0 PWR 0x61c004 = 1` (powered up); `SOR1 PWR = 0`
  (down); DAC0 idle. The internal LVDS panel is on SOR0. M3b drives SOR0.
- Head0 active @ 1024×768, raster 1344×806 (from M2 dump). Head1 disabled.

## Open questions / risks

- VRAM allocation on Win9x: we map BAR1 and carve a framebuffer; must avoid the
  region the VBIOS scanout currently uses until we take over.
- EVO channel error handling: `TRAPPED_ADDR/DATA` (0x610080/84) report bad
  methods — wire these into dbg output during bring-up.
- Our only test path is the real T61; keep each step reversible to VGA.
