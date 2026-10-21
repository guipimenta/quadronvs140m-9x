# Win98SE boot diagnosis on the T61 - 2026-10-21 session

STATUS 2026-10-22: **E4 ACHIEVED.** With build 3037 the T61 boots to the native
G86 desktop on nvmini.drv (BOOTLOG `LoadSuccess = nvmini.drv`, nvlog shows
`NV_native_modeset 1024x768x32 OK`); the user switched to 32 bpp / native
resolution from Display Properties. Root cause of the year-long
`LoadFail 0000` is in section 8. Sections 1-7 are kept as the diagnostic
record.

## 1. How to tell which display driver is really running

DxDiag is NOT evidence. On Win9x it prints the configured names from the
Display class key (`DEFAULT\drv`, `DEFAULT\minivdd`) and SYSTEM.INI
`[boot.description]`, so it says "nvmini.drv" even on a boot where that
driver failed and Windows fell back. nvmini.vxd can genuinely be resident
while GDI runs vga.drv: the VDD loads the mini-VDD during PnP, long before
the 16-bit driver is tried.

Authoritative sources, in order:

1. `C:\WINDOWS\BOOTLOG.TXT` from a logged boot (F8 / Ctrl at startup ->
   Logged). The 16-bit sequence after `Initializing KERNEL` is decisive:

   | Pattern | Meaning |
   | --- | --- |
   | `LoadStart = nvmini.drv` ... `LoadSuccess = nvmini.drv`, no `vga.drv` | native driver is the desktop driver |
   | `LoadStart = nvmini.drv` / `LoadFail = nvmini.drv Failure code is 0000` then `LoadStart = vga.drv` | file found and loaded, DriverInit returned 0, VGA fallback |
   | `LoadFail = DISPLAY.drv Failure code is 0002` | file not found for whatever `[boot] display.drv` names (e.g. pnpdrvr.drv missing) |
   | `LoadStart = vga.drv` directly, no nvmini.drv line | SYSTEM.INI `[boot] display.drv` says vga.drv (a previous VGA fallback rewrites it) |

   The `LoadStart = DISPLAY.drv / LoadSuccess` pair that follows is the
   DISPLAY module alias, backed by whichever driver won; it says nothing.

2. Display Properties > Settings. vga.drv offers only 640x480 at 16 colours.
3. `nvprobe lastset`: non-zero `FBHDA calls` and setmode counters mean the
   16-bit driver talked to the VxD. CAVEAT: if the VxD was NOT loaded at
   boot, nvprobe loads a fresh copy when it opens `\\.\nvmini.vxd`; its
   counters are then all zero and describe nothing about the boot.
4. In the VxD phase of BOOTLOG, `Dynamic load device nvmini.vxd` between
   `Starting NVIDIA ...` and `Started NVIDIA ...` proves the mini-VDD loaded.

## 2. The "There is a problem with your display settings" dialog

Text: "The adapter type is incorrect, or the current settings do not work
with your hardware." Windows shows it when the display driver that came up
is not the one, or not in the mode, that the registry describes - this
covers both a VGA fallback and a native driver that enabled a mode different
from `Config\0001\Display\Settings`. It then opens Display Properties. It is
NOT by itself proof that nvmini.drv ran; check BOOTLOG (section 1).

## 3. Why the Settings tab cannot select 1024x768 at 16 bit

Observed: picking 1024x768 drops colours to 256; picking High Colour drops
the resolution. The tab builds its list from the class key's `MODES\<bpp>\<x>,<y>`
subkeys (our INF registers 1024x768 at 8/16/24/32), filtered by the driver's
`ValidateMode` export and the monitor limits. In native mode our
`ValidateMode` accepts ONLY 1024x768 at 16/32 (`NV_native_valid`), i.e. the
opposite of what the tab shows, so the tab is not getting a usable answer
from it. The combination offered (1024x768x8 yes, 800x600x16 yes,
1024x768x16 no) is what a ~1 MB video-memory ceiling produces. Root cause
not yet established; suspects are the stock root-VGA class entry the driver
is now bound to (`ROOT\*PNP0900\0000` -> `Class\Display\0000`) or its
monitor record. Workaround: set the mode in the registry (section 5) and
do not use the tab. Diagnostics to collect: the exact list the tab offers,
`nvprobe lastset` right after moving the slider (validmode counter), and a
regedit export of `Class\Display\0000` + `Class\Monitor`.

## 4. IgnoreRegistry=1 in SYSTEM.INI disables the mini-VDD load (ROOT CAUSE, FIXED)

vmdisp9x's init.c reads `[display] IgnoreRegistry` to prefer the SYSTEM.INI
mode over the registry mode. The Windows 98 VDD reads THE SAME KEY:
`VDD_PNP_Load_MiniVDD` (VDD.VXD, _RARE 0x10eb) begins with
`Get_Profile_Boolean("Display","IgnoreRegistry")` and, if true, returns
before `VDD_PNP_Get_Display_Info` and before the `_CONFIGMG_Load_DLVxDs`
call that loads `DEFAULT\minivdd`. Effect on the T61 (boot log tick
00153D97): no `Dynamic load device nvmini.vxd`, nvmini.drv `LoadFail 0000`,
vga.drv desktop + the dialog. Removing the key restored the VxD load
(ticks 001579A2 and later). Disassembly notes: `analysis/win98_vdd/README.md`.
Rule: never set IgnoreRegistry; put the mode in the registry instead.

## 5. Reference configuration (what dist/driver now ships)

SYSTEM.INI (`dist/driver/SYSTEM.INI`, CRLF; copy over C:\WINDOWS\SYSTEM.INI):

```
[boot]
display.drv=nvmini.drv
[386Enh]
display=*vdd,*vflatd
[Display]
x_resolution=1024      ; fallback only - the registry mode wins
y_resolution=768
bpp=16
                       ; NO IgnoreRegistry line (section 4)
```

Registry (`dist/driver/nvsetup.reg`, merge with `regedit /s nvsetup.reg`):

```
[HKLM\Software\vmdisp9x\vesa]                      Native=1 InitStop=0 BootPattern=0
[HKLM\System\CurrentControlSet\Services\Class\Display\0000\DEFAULT]
                                                   DDC="0"  Mode="16,1024,768"
[HKLM\Config\0001\Display\Settings]                Resolution="1024,768" BitsPerPixel="16"
```

`Class\Display\0000` is the key that currently holds `minivdd=nvmini.vxd`
on the T61 (verified by the user); the adapter is bound to the root VGA
devnode `ROOT\*PNP0900\0000`. `Config\0001` is the default hardware profile.
`DDC="0"` keeps the VDD's V86 EDID probe off (1 tick instead of ~3 min).

## 6. Timeline of the boots examined

| BOOTLOG first tick | SYSTEM.INI | VxD loaded at boot | 16-bit result |
| --- | --- | --- | --- |
| 00029AAB (Oct 20 02:41) | display.drv=nvmini.drv, IgnoreRegistry=1 | yes (old PCI devnode, slow DDC probe) | LoadFail nvmini.drv 0000 -> vga.drv |
| 00147806 (Oct 21 20:28) | display.drv=vga.drv (rewritten by fallback) | yes, root devnode, 1-tick enum | vga.drv loaded directly |
| 00153D97 (Oct 21 21:14) | nvmini.drv + [Display] keys incl. IgnoreRegistry=1 | NO (section 4) | LoadFail 0000 -> vga.drv |
| 001579A2 .. 00158A0B (Oct 21 21:32, 3 boots) | same minus IgnoreRegistry | yes | LoadFail 0000 -> vga.drv |

## 7. Open problem and pending diagnostics

DriverInit (init.c) returns 0 at exactly two places: `VXD_VM_connect()`
failed (no device id 1..4 returned an FBHDA alias from
`VDD_REGISTER_DISPLAY_DRIVER_INFO`), or the alias came back NULL
(`register_display_driver` saw `hda == NULL`, i.e. native init failed
before `FBHDA_init_hw`, VRAM log slot 3 = 0x302). To tell them apart,
collect on the T61 (the VxD must have loaded at boot - check BOOTLOG for
`Dynamic load device nvmini.vxd` - otherwise nvprobe loads a fresh copy and
the data describes that copy, not the boot):

0. **Build 3034 (dist/driver) adds `nvprobe log > nvlog.txt`**: the full
   dbg_printf output of both the VxD (`V> `) and nvmini.drv (`D> `), kept in
   a 16 KB ring inside the VxD (see `driver/vmdisp9x-nv/dbgring.h`). It
   names the DriverInit exit directly ("VXD connect failure!" vs
   "failed to get FBHDA!") and shows each device id tried. Run it first.
1. `nvprobe lastset > lastset.txt`
2. `nvprobe vramrd 0x1900000 16 > vramrd.txt`
   slot0 B0070100 = register handler reached; slot1 = device id; slot2 = hda
   pointer; slot3 0x302 = hda NULL. No magic = handler never reached; the
   log then holds the native-init stage markers 0x100..0x107.
3. `dir C:\WINDOWS\SYSTEM\nvmini.*` - check the sizes against dist/driver (3035),
   nvmini.vxd 49241 bytes (3033 was 16274 / 40001).
4. regedit export of `HKLM\Software\vmdisp9x` and
   `HKLM\System\CurrentControlSet\Services\Class\Display\0000`.

## 8. ROOT CAUSE FOUND (2026-10-22, build 3037): a bad NE relocation, not the driver

Every `LoadFail = nvmini.drv Failure code is 0000` since M1 was the Win98 KERNEL
refusing the NE image before DriverInit ran. Proof chain, all reproduced in a
QEMU Win98SE harness built from the user's VirtualBox disk (scratch copy):

1. Ring-3 `out` to port E9h reaches QEMU's `-debugcon` (a Win32 probe in the
   StartUp folder printed). A driver whose NE entry did nothing but `out` and
   `retf` with AX=1 still gave LoadFail 0000 and printed nothing, so KERNEL never
   called the entry point.
2. `nerelocs.py` (scratch tool) showed one `internal(seg3)` relocation in every
   one of our builds, including the committed M3 binary, and none in the official
   vmdisp9x release. The NE has only two segments. The site is `push seg pText`
   in DriverInit: init.c declared `extern char __based(__segname("_TEXT")) *pText`
   and called `GlobalSmartPageLock((__segment)pText)`. Current Open Watcom treats
   `__segname("_TEXT")` as a user data segment, emits an empty FAR_DATA segment
   `_TEXT`, wlink drops it, the relocation points at segment 3, and the loader
   fails the fixup with error 0 (LME_MEM).
3. Fix: read CS (`#pragma aux get_cs = "mov ax,cs"`). With it, vesamini.drv built
   from the same objects logs `D> DriverInit: ENTER`, registers with the VxD and
   enables 1024x768x16 under QEMU through the standard pnpdrvr/registry path.

Side findings worth keeping:
- `pnpdrvr.drv` is not a file. KERNEL resolves it through the VDD to the bound
  devnode's `DEFAULT\drv` during the boot-driver phase and logs
  `LoadStart = <real>.drv`; with no bound devnode it tries the literal file and
  logs 0002. Direct `display.drv=nvmini.drv` takes the same loader path.
- The Oct-2026 Open Watcom wlink snapshot ignores `segment type data preload
  fixed` (DGROUP comes out MOVEABLE, far data references become entry-table
  thunks into the data segment). The 2025-08 snapshot honours it. The makefile
  now also names class DATA/BSS explicitly; build the 16-bit driver with
  /home/guilherme/watcom-2025-08.
- Patching bytes at a function entry to probe a Win16 binary is invalid when the
  bytes include a relocation chain pointer: the loader then fails. That is why
  the "official binary with stub" test was a false negative.
- Earlier NE differences (flags 8301 vs 8201, moveable DGROUP, align, heap) were
  each tested and are not disqualifiers on their own; Microsoft's own boot
  drivers vary in all of them.
