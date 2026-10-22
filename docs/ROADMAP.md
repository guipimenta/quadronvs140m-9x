# Native G86 Windows 9x display driver — roadmap

Target hardware: NVIDIA Quadro NVS 140M (G86, `PCI\VEN_10DE&DEV_0429`),
in a ThinkPad T61, running Windows 98 SE. No emulator can stand in for a G86
(86Box/PCem stop around the Riva/GeForce-256 era), so all GPU bring-up is
tested on the real T61.

## Why this is a new driver, not a patch

The Win9x NVIDIA stack (`NVCORE.VXD`) registers 22 chip descriptors covering
families `0x01`–`0x04` (NV1x…G7x). G86's BOOT0 family is `0x08`; no descriptor
matches, and the no-match path makes an unchecked indirect call through a null
object (`NVCORE` LCOD `0x18bd4b`) — the CONFIGMG protection-error hang.
See `analysis/reverse/CHIP_FAILURE_PATH.md`.

NVIDIA never wrote G86 code for the Win9x VxD architecture (the last official
9x driver, 81.98, topped out at GeForce 7). The XP driver *does* have G86
(`nv4_mini.sys` predicate `0xe0230`, family `0x08` + impl `0x600000`), but it is
NT kernel-mode (imports ntoskrnl/HAL/videoprt/win32k) and cannot be transplanted.
So G86 support must be *re-implemented* for the Win9x model, using the XP binary
and the open nouveau/envytools NV50-family documentation as hardware references
(G80/G84/G86 share the NV50 display engine).

## Architecture: fork vmdisp9x, add an NVIDIA backend

Base: JHRobotics/vmdisp9x (MIT), a DIBENG-based Win9x display driver with a
clean backend split — a shared 16-bit display driver (`.drv`) + 32-bit VxD
framework, behind swappable backends (VMware, VESA, QEMU). Our G86 backend
slots in where the **VESA** backend sits, providing the same framebuffer
abstraction (`vxd_fbhda.c`) via native MMIO instead of VBE BIOS calls.

Our fork lives in `driver/vmdisp9x-nv/` (de-nested from the MIT clone; upstream
commit in `driver/PROVENANCE.txt`). Our additions are `vxd_nv.{c,h}`,
`vxd_main_nv.c`, `tools/nvprobe.c`, the `nvmini` makefile target, and small
`#ifdef NV_DETECT` hooks in `vxd_main.c` / `vxd.h` / `vxd_vesa.c`.

## Milestones

- **M0 — toolchain (DONE).** Open Watcom 2.0 (`/home/guilherme/watcom`) builds
  the vmdisp9x VESA pair from source on this Linux host:
  `vesamini.vxd` (LE) + `vesamini.drv` (NE). Self-verified with
  `tools/le_vxd.py` (DDB `VESAVXD`, 484 fixups). Build→inspect loop closed here;
  only loading/running needs the T61.
- **M1 — prove the display stack on the T61.** Install the stock vmdisp9x VESA
  driver on the T61 via its INF; confirm a working 9x desktop over the G86 VBIOS
  linear framebuffer. De-risks the whole DIBENG/VxD path independent of NVIDIA
  specifics. (Needs T61.)
- **M2 — touch the hardware from our code (CODE DONE, awaiting T61 test).**
  `vxd_nv.c` enumerates the NVIDIA display device on PCI, maps BAR0 (MMIO),
  reads `PMC_BOOT_0` and decodes chipset/family/rev. Built into the `nvmini`
  pair (VESA scanout + probe); results read on the laptop with `nvprobe.exe`
  via `OP_NV_PROBE` (0x5000), so no serial port is needed. Package:
  `dist/driver/`. Success = `nvprobe` prints `chipset 0x86, mmio_ok 1`.
  Still scanning out via VESA. (Needs T61 to run.)
- **M3 — native modeset (the hard one).** Program the NV50 display engine
  (PLLs, CRTC, LVDS/panel) to light the T61 panel at native resolution with a
  linear framebuffer, per nouveau's documented sequence; read panel EDID over
  DDC or hardcode T61 timings. This is the first genuinely native driver.
- **M3 result (2026-10-19, build 3010): DONE.** EVO core-channel takeover, 8 colour
  bars on the panel. Recipe in nv50_evo.c / docs/EVO_PLAN.md.
- **E4 — make nvmini.drv the boot display driver: DONE (2026-10-22, build 3037).**
  Blocked for days by a toolchain-induced NE relocation to a non-existent
  segment (see analysis/BOOT_DIAGNOSIS_2026-10-21.md §8). The T61 now boots to
  the native 1024x768x32 desktop.
- **M4 — polish, in this order.**
  - M4.1 mode list / Settings tab **(build 3040, DONE 2026-10-22):** 640x480,
    800x600, 1024x768 at 8/16/32 bpp; lower modes via the EVO scaler, 8 bpp as
    palette-expanded 32 bpp scanout. Scanout pitch is 256-byte aligned
    (3040 fix for 800x600 True Color). All nine modes verified on the T61.
  - M4.2 install from the INF alone **(build 3039, code done; fresh-install test on the T61 pending):** INF binds
    the root VGA devnode too; nvsetup.reg demoted to a repair tool.
  - M4.3 native memory setup **(build 3038, DONE 2026-10-22):** real VRAM size
    from PFB (128 MB on the T61), layout validated against the EVO
    reservations, 24 MB window.
  - M4.4 hardware cursor **(build 3041, DONE 2026-10-22):** 64x64 ARGB image
    in VRAM, head0 CURSOR_CTRL/OFFSET/DMA methods + UPDATE on the core ring,
    position via the cursor PIO channel; HwCursor=0 restores the software
    cursor.
  - M4.5 power / screen switching (VGA<->hires, standby, lid).
  - M4.6 shadow-blit performance **(build 3043, DONE 2026-10-22, 110 -> 6 cycles/pixel):** row-wise
    rep movsd / tight conversion loops instead of per-pixel indirect calls
    (3042: no gain, the bus was the limit at ~110 cycles/pixel); MTRR.VXD
    cannot write-combine the aperture on the T61 (BIOS UC MTRR over the PCI
    hole), so 3043 does it through the PAT (entry 1 = WC, PWT on the
    aperture PTEs). `nvprobe mtrr` shows MTRRs, PAT state and blit cycle
    counters. Dirty rects were already in place.
- **DD — DirectDraw (build 3044, DONE 2026-10-22).** The HAL is JHRobotics'
  ring-3 vmhal9x (MIT), loaded by DDRAW through the 16-bit driver's DCICOMMAND
  escape and driving surfaces through the VxD's FBHDA API; nvmini already had
  all the plumbing, only the DLLs were missing. Shipped vmhal9x.dll /
  vmdisp9x.dll / vmhal3d.dll built from the vmhal9x commit matching our FBHDA
  API (driver/vmhal9x-bin/PROVENANCE.txt). DXDIAG DirectDraw tests pass on
  the T61. Direct3D (DDI 8 via vmhal3d + Mesa9x software rasterizer) is the
  natural next step; it needs mesa3d.dll (~44 MB), not shipped yet.
  Hardware 2D/3D acceleration on the G86 itself remains out of scope.

## Build (verified, this host)

```
export WATCOM=/home/guilherme/watcom
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
export INCLUDE="$WATCOM/h/win:$WATCOM/h"
export EDPATH="$WATCOM/eddat"
cd driver/vmdisp9x-nv
gcc fixlink/fixlink.c -o ./fixlink.bin   # one-time, Linux host fixup tool
wmake vesamini.vxd vesamini.drv          # M1: stock VESA pair
wmake nvmini.vxd nvmini.drv              # M2: NVIDIA detection pair
INCLUDE="$WATCOM/h/nt:$WATCOM/h" wcl386 -bt=nt -l=nt -q -fe=nvprobe.exe tools/nvprobe.c
```

Inspect any built VxD:
`python3 -c "import sys;sys.path.insert(0,'tools');from le_vxd import LE;print(LE('<file>').ddb)"`

## References

- vmdisp9x: https://github.com/JHRobotics/vmdisp9x (MIT; our base)
- envytools NV50 display: https://envytools.readthedocs.io/
- nouveau (NV50 modeset reference implementation)
- XP G86 predicate/tables: `analysis/reverse/xp_g86_predicate_and_registration.asm`
