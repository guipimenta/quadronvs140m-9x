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
- **Games on the GPU (builds 3095-3124, 2026-10-28 to 2026-11-01).** OpenGL and
  Direct3D programs run on the G86 with no per-game setup: install the driver,
  play the game.
  - 3095: **Half-Life plays** on the GPU through the OpenGL ICD; 3109: clean at
    1024×768 (nouveau's VRAM row-block rule for tiled and linear buffers).
  - 3110: every OpenGL program uses the GPU by default (registry, no batch file);
    one process owns the GPU channel, a second one falls back to software.
  - 3115: GART (system-memory buffers through the GPU's page tables, nouveau's
    way): **Soldier of Fortune plays** on the GPU.
  - 3116-3117: **Direct3D on the GPU** (vmhal3d through Mesa's OSMesa); dxdiag's
    DirectX 7/8/9 tests pass. 3121: Dungeon Keeper 2 renders right.
  - 3123: OpenGL faster (Soldier of Fortune's fight demo 35.5 → 56 fps): asynchronous
    submits with fences, a GART command ring, faster locks in Mesa.
  - 3124: Soldier of Fortune's opening cutscene no longer freezes Windows (the GPU
    TLB flush done nouveau's G84 way).
- **Download:** the [Releases](https://github.com/guipimenta/quadronvs140m-9x/releases)
  page has the packaged driver (`nvmini-<build>.zip`); `INSTALL.txt` inside says
  how to install or update it.
- **Next:** OpenGL is now CPU-bound (Mesa's immediate-mode path plus the game,
  about 16 ms a frame in Soldier of Fortune's fight); Half-Life's OpenGL renderer
  is due a recheck on the latest builds.

## Layout

- `driver/vmdisp9x-nv/` — the driver (our fork of vmdisp9x). Our additions:
  `vxd_nv.{c,h}` (G86 detect + MMIO/VRAM access), `vxd_main_nv.c`, `nv50_disp.h`
  (NV50 register map), `tools/nvprobe.c` (user-space probe/peek tool), plus the
  `nvmini` makefile target and small `#ifdef NV_DETECT` hooks.
- `driver/vmdisp9x-nv/nv50_gpu.c`, `nvshim.c` — the GPU side: PFIFO channel, GPU
  virtual memory and GART in the VxD; `nvshim.c` replaces libdrm_nouveau inside
  `mesa3d.dll` (buffer objects, pushbuffers, fences) on top of the VxD's ioctls.
- `driver/mesa9x-bin/` — the patches and recipe (`PROVENANCE.txt`) that build
  `mesa3d.dll` from [Mesa9x](https://github.com/JHRobotics/mesa9x) with nouveau's
  nv50 driver. `driver/vmhal9x-bin/` — the same for the DirectDraw/Direct3D HAL
  ([vmhal9x](https://github.com/JHRobotics/vmhal9x)).
- `dist/driver/` — the current built driver + INF + test tools + diagnostic
  batches, ready to copy to the T61 (`tools/make_dist.sh` fills it).
- `tools/` — Python reverse-engineering helpers (LE/VxD parser, trace scripts,
  `nvkick_decode.py` command-stream validator), `qemu98/` (headless Win98 VM
  harness), `remote/` (drives the T61 over the LAN: files, runs, input,
  screenshots, debugger, reboots).
- `analysis/` — our RE write-ups (the raw proprietary binaries are gitignored).
- `docs/` — roadmap, the M3 plan and `HW3D_PLAN.md` (GPU 3D, build by build).

## Build

Open Watcom 2.0 (Linux host). From `driver/vmdisp9x-nv/`:

```
export WATCOM=/path/to/watcom
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
export EDPATH="$WATCOM/eddat"
export INCLUDE="$WATCOM/h/win:$WATCOM/h"
gcc fixlink/fixlink.c -o ./fixlink.bin      # one-time
wmake nvmini.vxd nvmini.drv
i686-w64-mingw32-gcc -O2 -s -DNVSHIM_STANDALONE tools/nvprobe.c nvshim.c -I. -o tools/nvprobe.exe
```

`mesa3d.dll` (OpenGL ICD and Direct3D rasterizer) is cross-built with MinGW from
Mesa9x plus `driver/mesa9x-bin/*.patch`; the full recipe is in
`driver/mesa9x-bin/PROVENANCE.txt`. `tools/make_dist.sh` then repopulates
`dist/driver/`.

## Legal

This repository contains only original code, the MIT-licensed vmdisp9x base, and
our own reverse-engineering notes. It does **not** contain NVIDIA's proprietary
driver binaries or disassembly of them; those are gitignored. The work is for
interoperability/retro use on hardware the vendor no longer supports.
