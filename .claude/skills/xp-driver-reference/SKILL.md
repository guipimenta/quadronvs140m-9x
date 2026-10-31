---
name: xp-driver-reference
description: The working NVIDIA Windows XP driver for this exact GPU (G86 / Quadro NVS 140M) is in the repo and the T61 dual-boots XP; wine9x (WineD3D for Win9x, source in ~/projects/wine9x) is the second, source-level Direct3D-to-OpenGL reference. Consult them to understand how Direct3D (DirectDraw/D3D DDI, caps, formats, DrawPrimitives2, clipping), OpenGL and the hardware are really driven, whenever the Win98 Direct3D/OpenGL path misbehaves (vmhal9x/vmhal3d, Mesa OSMesa/nouveau) or a hardware question comes up.
---

# NVIDIA's Windows XP driver: the working reference

NVIDIA never shipped a Win9x driver for the G86, but its **XP driver supports this exact
chip** (PCI 10de:0429, Quadro NVS 140M on the ThinkPad T61). Whenever our Win98 stack
renders Direct3D or OpenGL wrongly, ask first: *what does the XP driver do for the same
request?* It answers questions the DDK docs leave open (which caps a real G86 driver
reports, which formats it exposes, how it treats pre-transformed vertices, guard band,
z-bias, flips) and hardware questions (init values, contexts, VM).

**Read [disassembly-findings.md](disassembly-findings.md) first**: everything already
disassembled from NVIDIA's XP and Win9x drivers, with exact addresses (G86 chip object and
PGRAPH context data in the XP miniport, why the Win9x stack crashes on a G86, where to start
in `nv4_disp.dll` for Direct3D). Do not redo that work; extend that file when you learn more.

## What is where

| Path | Contents |
| --- | --- |
| `DISPLAY/` | The original Lenovo package, **NVIDIA 6.14.11.7874 (2009-01-14)**, files SZDD-packed (`*.dl_`, `*.sy_`). `NVLT.inf` = install INF. Git-ignored. |
| `analysis/xp_extracted/nv4_disp.dll` | XPDM **display driver**: GDI + **DirectDraw + Direct3D DDI** (kernel mode, imports only WIN32K.SYS, one export = DrvEnableDriver). |
| `analysis/xp_extracted/nvoglnt.dll` | **OpenGL ICD** (user mode). |
| `analysis/xp_extracted/nv4_mini.sys` | **Miniport + resource manager**: chip init, PFIFO/PGRAPH, contexts, VM. |
| `analysis/reverse/XP_nv4_mini.asm` | Full objdump of the miniport (110 MB; grep, don't read). |
| `analysis/reverse/xp_g86_predicate_and_registration.asm`, `FINDINGS.md` | G86 detection and chip registration in the miniport. |
| `analysis/xp_grctx/` | `extract_xp_g86.py` + `g86_xp_context.txt`: the G86 PGRAPH context program, golden context and MMIO init the XP miniport loads (used in builds 3101-3102). |
| `analysis/nv4_disp.dll.headers.txt`, `nv4_mini.sys.headers.txt` | PE headers. |

`xp_extracted/` is git-ignored: if missing, unpack again (any other file the same way):

```bash
msexpand < DISPLAY/nv4_disp.dl_ > analysis/xp_extracted/nv4_disp.dll
```

(`7z x` also reads SZDD.)

## Why it maps 1:1 onto our Direct3D path

On XP, Direct3D drivers are kernel-mode parts of the display driver: DirectDraw asks
`nv4_disp.dll` for `DrvGetDirectDrawInfo`, then `DdGetDriverInfo` with the same GUIDs our
HAL answers (`GUID_D3DCallbacks3`, `GUID_D3DExtendedCaps`, `GUID_ZPixelFormats`,
`GUID_GetDriverInfo2` with `D3DGDI2_TYPE_GETD3DCAPS8/9` and `GETFORMAT`...). Rendering arrives
as `D3dDrawPrimitives2` command buffers (DP2 tokens), exactly what
`vmhal3d.dll` (JHRobotics/vmhal9x, source `~/projects/audio_reverse/vmhal9x-src`, `d3d.c`,
`d3dhal.c`, `mesa3d_draw*.c`) receives on Win98. So for any DDI-level behaviour, the XP
driver is a working implementation of the same contract for the same GPU.

## How to consult it

1. **Live, on the real hardware (best for behaviour and caps).** The T61 dual-boots:
   `C:\BOOT.INI` lists `partition(2)\WINDOWS="Microsoft Windows XP Professional"` (Win98 is
   the default, 30 s menu). Under XP the same test programs run on NVIDIA's own driver:
   dxdiag (Save All Information = the real driver's caps/formats), `d3d8test.exe`
   (`driver/vmdisp9x-nv/tools/d3d8test.c`), games, wgltest. Screenshots there are the
   **reference picture** for a Win98 rendering bug. Booting XP needs the user at the boot
   menu (or a boot.ini default change, which is a system change: ask first), and the
   win98-remote agent is only set up on Win98 (tools/remote/README.md), so plan an XP
   session with the user. Verified 2026-10-31: XP's partition is NTFS and Win98 on the T61
   reads it as **D:** (`t61.py dir D:\WINDOWS`); XP's installed `nv4_disp.dll`,
   `nvoglnt.dll` and `drivers\nv4_mini.sys` are byte-identical to `analysis/xp_extracted/`,
   and XP has DirectX 9 (d3d8.dll/d3d9.dll). So results an XP session saves in XP's
   `C:\xpref` can be read back from Win98 at `D:\xpref`.
2. **Static, from the binaries.** Symbols are stripped; find code through:
   - strings and constants: DDI GUID bytes, `D3DCAPS8/9` field values, FourCC/`D3DFMT_*`
     codes, registry value names (`strings -n 6 analysis/xp_extracted/nv4_disp.dll`);
   - the object classes and methods it writes (nv50 3D class 0x8297, 2D 0x502d; method
     offsets from Mesa's `nv50_3d.xml.h`) to locate how it programs the hardware for a
     given D3D state;
   - `i686-w64-mingw32-objdump -d analysis/xp_extracted/nv4_disp.dll` (warnings about
     NOT_PAGED sections are harmless).
3. **Compare, don't copy.** Use it to understand semantics and correct values; implement in
   our code (nouveau's way for hardware programming, per the standing user directive), and
   never paste NVIDIA's code. Raw XP command streams are not portable to Mesa's.

## Known uses so far

- 3101-3102: XP G86 PGRAPH context vs nouveau's: only three MMIO init values differ; neither
  XP variant fixed the corner holes (their cause was our VRAM heap, 3109). Details and
  addresses in disassembly-findings.md.
- Chip detection: the XP miniport has the G86 predicate (0xe0230) that the Win9x NVCORE lacks;
  the Win9x crash is an unchecked null call at NVCORE 0x18bd4b.
- Direct3D (open, 2026-10-31): Half-Life 1.0's Direct3D renderer draws wrong through
  vmhal3d on Win98 on BOTH nouveau and llvmpipe (`analysis/t61-remote/hl-d3d/`); the XP
  driver (live under XP, or `nv4_disp.dll`) is the reference for what the DDI should do with
  Half-Life's draws. `nv4_disp.dll` has not been disassembled yet.

When you learn something new from these binaries, add it to disassembly-findings.md (exact
addresses, the conventions used) so it is never re-derived.

## Second reference: wine9x (WineD3D on Win9x, source)

`~/projects/wine9x` (https://github.com/JHRobotics/wine9x, cloned 2026-10-31 at 5fa464d) is
Wine 1.7.55's WineD3D ported to Windows 9x by the vmhal9x author: `winedd.dll` (DirectDraw /
Direct3D 6-7), `wined8.dll`, `wined9.dll` over `wined3d.dll`, on any OpenGL 2.1 ICD (ours:
mesa3d.dll), with per-application switchers (`DDSwitcher`/`D8Switcher`/`D9Switcher`). Where
the XP driver shows *what* the right result is, wine9x shows *how to get it through OpenGL*
in readable source - the same job vmhal3d does, done independently. Consult it for
Direct3D-to-GL translation questions (state mapping, formats, flips, texture management).
Known points:

- Pre-transformed (XYZRHW) vertices: `wined3d/directx.c:5899` `position_float4` sends
  `glVertex4f(x/rhw, y/rhw, z/rhw, 1/rhw)` with an ortho projection (`state.c`, `last_was_rhw`)
  - mathematically what vmhal3d's `SV_UNPROJECT` does; its fast path converts them into a
  real vertex buffer first (`wined3d/buffer.c:543`) instead of immediate mode.
- Its README names Half-Life as an application that breaks under Wine (switch it to the
  system DLL), so it is a reference, not a drop-in replacement for vmhal3d.
