# nvs140m-9x — a native Windows 9x display driver for the NVIDIA Quadro NVS 140M (G86)

An effort to give a ThinkPad T61 (Quadro NVS 140M, a G86 / GeForce-8-generation
GPU) a working **Windows 98 SE** display driver. NVIDIA never supported the G8x
family on Windows 9x — the last official 9x driver (81.98) stops at GeForce 7 —
so this is a from-scratch reimplementation of G86 display support against the
Win9x driver model, using the XP driver and the open nouveau / envytools NV50
documentation as hardware references.

## Status

- **M0 — toolchain:** Open Watcom builds a loadable VxD + 16-bit display driver. ✓
- **M1 — display stack:** forked [vmdisp9x](https://github.com/JHRobotics/vmdisp9x)
  (MIT) as the base (DIBENG display driver + VxD framework). ✓
- **M2 — hardware bring-up (on real T61):** our code enumerates the G86, maps
  BAR0 MMIO, reads `PMC_BOOT_0` (chipset 0x86 confirmed), reads/writes VRAM, and
  **puts pixels on the panel** from our own code. ✓
- **M3 — native modeset: ACHIEVED (build 3010).** Our driver re-initialises the
  G86 display engine, brings up the EVO core channel, programs the complete
  1024×768@60 head state through its method ring, services the supervised
  commit handshake, and scans out our own linear framebuffer — verified by
  8 color bars on the T61 panel. See `docs/EVO_PLAN.md` for the full recipe.
- **M4/E4 — native desktop: ACHIEVED (build 3037, 2026-10-22).** The native
  framebuffer is wired into DIBENG (FBHDA); Windows 98 runs its desktop on our
  modeset (640×480 / 800×600 / 1024×768 at 8/16/32 bpp).
- **M5 — hardware-accelerated 3D: MILESTONE (builds 3091/3092, 2026-10-28).**
  Mesa's unmodified **nv50 Gallium driver** renders on the G86 under Windows 98
  through a ring-3 libdrm shim and the VxD (persistent PFIFO channel, IB submit,
  GPU virtual memory with per-surface storage kinds, PGRAPH context, trap
  recovery). Build 3091 drew the first GPU triangle; build 3092 presents by
  2D-engine blit into the scanout surface and runs wgltest at **~60 fps** with
  zero traps. Everything that does not need the silicon is tested first in a
  simulated environment: a QEMU Win98 harness, a fake-GPU mode of the shim
  (runs Mesa's whole nv50 path under Wine or in the VM and validates every
  command stream), and a kick-stream validator. See `docs/HW3D_PLAN.md`.
- **Next:** Half-Life through the OpenGL ICD route (fullscreen mode changes,
  textures), then Direct3D via Mesa9x's wrapper.

## Layout

- `driver/vmdisp9x-nv/` — the driver (our fork of vmdisp9x). Our additions:
  `vxd_nv.{c,h}` (G86 detect + MMIO/VRAM access), `vxd_main_nv.c`, `nv50_disp.h`
  (NV50 register map), `tools/nvprobe.c` (user-space probe/peek tool), plus the
  `nvmini` makefile target and small `#ifdef NV_DETECT` hooks.
- `dist/driver/` — the current built driver + INF + `nvprobe.exe` + dump
  batches, ready to copy to the T61.
- `tools/` — Python reverse-engineering helpers (LE/VxD parser, trace scripts).
- `analysis/` — our RE write-ups (the raw proprietary binaries are gitignored).
- `docs/` — roadmap and the M3 plan.

## Build

Open Watcom 2.0 (Linux host). From `driver/vmdisp9x-nv/`:

```
export WATCOM=/path/to/watcom
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
export EDPATH="$WATCOM/eddat"
export INCLUDE="$WATCOM/h/win:$WATCOM/h"
gcc fixlink/fixlink.c -o ./fixlink.bin      # one-time
wmake nvmini.vxd nvmini.drv
INCLUDE="$WATCOM/h/nt:$WATCOM/h" wcl386 -bt=nt -l=nt -q -fe=nvprobe.exe tools/nvprobe.c
```

## Legal

This repository contains only original code, the MIT-licensed vmdisp9x base, and
our own reverse-engineering notes. It does **not** contain NVIDIA's proprietary
driver binaries or disassembly of them; those are gitignored. The work is for
interoperability/retro use on hardware the vendor no longer supports.
