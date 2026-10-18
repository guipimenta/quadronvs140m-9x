# Quadro NVS 140M / Windows 9x compatibility investigation

## Result

Update: the first binary reverse-engineering pass is documented in
`reverse/FINDINGS.md`. It recovered the initialization interfaces and a
22-entry chip registration routine whose predicates all reject G86 family
bits, plus an explicit G86 predicate/registration in the XP miniport.
This is a concrete static compatibility gap, not yet the exact runtime fault.

No working Windows 9x driver has been produced or hardware-tested. The supplied
packages do not establish a viable binary patch. Original packages are unchanged.
The extracted XP binaries in this directory are analysis inputs, not an installer.

## Verified local evidence

* `DISPLAY/NVLT.inf:10`: XP version 6.14.11.7874 (178.74), dated 2009-01-14.
* `DISPLAY/NVLT.inf:58`: NVS 140M uses the G86 installation section and matches
  `PCI\VEN_10DE&DEV_0429&SUBSYS_20D817AA`.
  This is a Lenovo-specific match; it does not prove the actual notebook subsystem ID.
* `modified_driver/NVAML.INF:2`: package declares version 8.2.6.9 (82.69).
* `modified_driver/NVAML.INF:349`: device 0429 is already assigned to N3GL.
* `modified_driver/NVAML.INF:1695`: string names it Quadro NVS 140M.
* `modified_driver/README.TXT:146`: explicitly describes added INF hardware
  support as theoretical. An INF entry is not evidence of hardware implementation.
* `NVCORE.VXD` and `NVMINI.VXD` are LE VxDs; `NVDISP.DRV` is an NE driver.
* Extracted XP `nv4_mini.sys` imports HAL.dll, ntoskrnl.exe and VIDEOPRT.SYS.
* Extracted XP `nv4_disp.dll` imports WIN32K.SYS graphics services.
  These are NT display-stack dependencies, not replacements for the Win9x VxDs.

## Interpretation and limits

Installer rejection and a startup crash are separate problems. The existing INF
already recognizes 0429. Setup may apply additional detection rules; its precise
failure needs installer logs. Bypassing those rules does not implement GPU support.

The crash is consistent with an unsupported GPU initialization path, but no crash
address, boot log, hardware dump or disassembly tracing is available to prove its
exact cause. Static inspection has not exhaustively established which chip paths
exist inside the 82.69 binaries.

Copying XP binaries into the Win9x package, renaming them, or changing INF device
IDs cannot satisfy their NT kernel/display service dependencies. A real port would
need G86 initialization, memory management, modesetting (including laptop panel),
command submission and rendering support connected to Win9x driver interfaces.
The XP package may help reverse engineering, but is not directly reusable as a
Win9x driver stack. That work needs sustained implementation and hardware testing.

## Information needed to pursue a real port

1. Exact notebook model, full PCI hardware ID, VRAM size and Windows release
   (95, 98 FE, 98 SE or ME), installed RAM and system patches.
2. Exact startup failure text/address, whether Safe Mode works, and BOOTLOG.TXT
   generated with boot logging. A boot log may narrow the phase, not prove cause.
3. A recoverable test installation and a way to capture debugger output on hardware.
4. A Win9x driver development toolchain and a scoped first target, ideally basic
   modesetting before attempting DirectDraw/Direct3D/OpenGL acceleration.

If the current install cannot start normally, use Safe Mode to remove the NVIDIA
adapter driver and select the standard VGA driver supplied with that Windows
version. Restore a known-good backup if that fails. Preserve diagnostic logs first.

## Reproducibility

XP files were decompressed with `7z x -y -oanalysis/xp_extracted DISPLAY/<file>`
individually for nv4_mini.sy_, nv4_disp.dl_, nvoglnt.dl_. No binaries were executed.
`manifest.json` records SHA-256 hashes and lengths. The accompanying objdump
header reports record XP imports. No installation or runtime validation was done.

## Supplied boot log

The root `BOOTLOG.TXT` was inspected after the user confirmed Windows 98 SE
on a ThinkPad T61. Line 8 explicitly says `(Safe boot)`; lines 332–333 load
`VGA.DRV` successfully. No NVCORE, NVMINI, NVDISP or NVDD32 entries occur.
Consequently this file does not capture initialization of the failing NVIDIA
driver and cannot locate its crash. It records VPD initialization failure and
font load failures, but these do not establish an NVIDIA failure.
BIGMEM also appears and reports successful initialization; its provenance and
effect on a normal boot are unknown.

Next capture: choose Logged from F8 for a normal boot attempt with the NVIDIA
driver installed. After failure, boot to Command prompt only and copy
`C:\BOOTLOG.TXT` to `C:\NVFAIL.TXT` before entering Safe Mode, which may replace
the boot log. If present, preserve `C:\BOOTLOG.PRV` separately as well; it may
contain an earlier attempt, whose contents must be inspected to establish that.
Supply NVFAIL.TXT and the exact visible failure symptoms or message.

## Follow-up logs: NVFAIL.TXT and NVFAIL.PRV

`NVFAIL.TXT` contains only seven lines of early real-mode driver loading; it
does not show the NVIDIA stack. `NVFAIL.PRV` contains the useful normal-boot
trace (740 lines). It identifies the actual adapter as
`PCI\VEN_10DE&DEV_0429&SUBSYS_20D817AA&REV_A1` (line 728), matching the supplied XP INF.

Lines 729–740 report successful dynamic loading and initialization of NVMINI
and NVCORE. The log ends immediately after the second NVCORE load-success
entry, without a matching `Started NVIDIA Quadro NVS 140M` entry. No NVDISP
or NVDD32 entry is recorded. This narrows the last recorded activity to the
adapter-start phase after VxD loading, but does not identify the faulting
instruction or establish that full GPU initialization succeeded. The final
flushed line need not be the operation that caused the hang.

Earlier NDIS2SUP, PnP BIOS and VPD failures are followed by further startup
activity; the trace does not establish them as the cause. BIGMEM and other
system modifications are present, so the installation is not a demonstrated
unmodified baseline.

Next evidence required is the visible failure behavior and any exception
module/address. Preserve these logs; another boot-log capture is not needed
just to repeat the same trace. For installed-driver verification, copies of
the installed NVMINI.VXD, NVCORE.VXD, NVDISP.DRV, SYSTEM.INI and CONFIG.SYS
would allow comparison with the supplied package and boot configuration.
The user subsequently confirmed that the installed driver binaries are the
supplied versions; a second copy is not required for the current investigation.

## Configuration inspection and reported error

The user reports a CONFIGMG Windows protection error (initially transcribed
CONFIGMHG), followed on another boot by Code 11 on the adapter. Caps Lock still
responds when the display is black. Code 11 indicates a recorded failed startup
and skipping of the device; it is not evidence of hardware damage or successful
driver operation. A retry after clearing the skip is a new initialization attempt,
not a fix. Caps Lock alone does not establish that the desktop is running.

The supplied `CONFIG.SYS` is zero bytes. `SYSTEM.INI` contains:

* `[boot] display.drv=pnpdrvr.drv`, consistent with PnP display selection.
* `[boot.description] display.drv=NVIDIA Quadro NVS 140M`, a description rather
  than an executable driver path.
* `[386Enh] MaxPhysPage=20000`, a nominal 512 MiB physical-memory ceiling
  (hexadecimal 0x20000 pages of 4096 bytes).
* `[vcache] MaxFileCache=65536`, a 64 MiB cache ceiling in KiB.
* `[386Enh] display=*vdd,*vflatd` and `EMMExclude=C000-CFFF`.

There is no explicit NVIDIA VxD loading line in SYSTEM.INI; the INF registers
NVDISP.DRV and NVMINI.VXD under the device's DEFAULT settings, consistent with
the observed PnP loading. No obvious misconfiguration in these two files
provides an evidence-based fix. The existing RAM/cache limits already address
the usual excessive-memory/cache troubleshooting avenue, though they do not
rule out all memory/resource issues or interactions with installed patches.
The earlier trace contains BIGMEM and other extensions not explained by this
empty CONFIG.SYS; these files are not a complete record of the installed system.

No configuration changes or installable patches were made. The next development
step would be tracing the NVCORE/NVMINI device-start and chip-selection paths
and validating any proposed change on hardware. INF matching and boot logs alone
cannot establish such a patch. A compatible accelerated driver remains unbuilt.

## Primary references

* Microsoft memory/cache guidance:
  https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/253/912.HTM
* Microsoft Device Manager error codes (archived):
  https://mskb.pkisolutions.com/kb/125174

* NVIDIA device-ID table: https://download.nvidia.com/solaris/337.12/README/supportedchips.html
* Dell identifies the NVS 140M driver family as G86:
  https://www.dell.com/support/home/en-us/drivers/driversdetails?driverid=c01pm
* Microsoft display driver architecture overview:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/
