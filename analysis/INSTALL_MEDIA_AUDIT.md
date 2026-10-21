# Installation-media audit

Inspected `win98se/README.TXT`, the loose BAT/INF/CONFIG.SYS files, and
AUTOEXEC.BAT / CONFIG.SYS read from `_BOOT_/BOOT_1_4.IMG` using `7z e -so`.
No installation scripts or bundled executables were executed.

## Menu and actual setup commands

The boot image selects PREPC1..PREPC10.BAT. Each preparation script formats C:,
copies the installation files to C:\WIN98, and installs the corresponding
AUTO1_N.BAT as C:\AUTOEXEC.BAT. These are installation scripts, not repair tools.

| Menu choices | Batch INF | Post-install action |
| --- | --- | --- |
| 1 / 2 | MSBATCH1.INF | LocalLoadHigh=0, MaxPhysPage=20000, MaxFileCache=65536 |
| 3 / 4 | MSBATCH3.INF | Same settings, then PATCHMEM.EXE |
| 5 / 6 | MSBATCH5.INF | Same settings, then install HIMEMX.EXE and UMBPCI.SYS and replace CONFIG.SYS |
| 7 / 8 | MSBATCH7.INF | Same initial settings, then PATCHMEM.EXE and replace MaxPhysPage=20000 with 80000 |
| 9 / 10 | MSBATCH.INF | No memory INI edits or PATCHMEM invocation in this path |

Odd choices run `setup.exe C:\WIN98\<batch.inf>`.
Even choices run `setup.exe /p i C:\WIN98\<batch.inf>`.
The ISO labels the even choices “No PNP”; the observed fact is that they add
the `/p i` detection switch. That label alone should not be interpreted as
disabling every form of PCI/Plug-and-Play enumeration.

Every batch INF adds C:\WIN98 to OtherDevicePath and schedules its MEMUPD script
through RunOnce. The loose custom INFs do not import Enum device instances,
create the duplicate VGA/keyboard entries, or install a custom display driver.
DESKTOP.BAT copies optional NUSB/sound installers to the desktop; it does not
execute those installers. This audit does not certify that the CAB contents
or bundled executables match pristine Microsoft/vendor distributions.

## Recover which path actually ran

MEMUPD1/3/5/7/9.BAT all execute:

```
move C:\AUTOEXEC.BAT C:\AUTOEXEC.OLD
copy C:\WIN98\AUTO2.BAT C:\AUTOEXEC.BAT
```

Therefore the installed machine's C:\AUTOEXEC.OLD may still contain the exact
Setup invocation, including `/p i` and the selected batch INF. This is a much
better discriminator than guessing the selected menu option from the current
SYSTEM.INI, whose settings overlap across several choices.

FIXSND.BAT also writes AUTOEXEC.OLD, so a later sound fix or other installer may
have overwritten that evidence. If the file is absent or unrelated, inspect
the installed Windows SETUPLOG.TXT instead. Files in the ISO show available
paths, not which one was actually used.

## Conclusion

There is a concrete hardware-detection choice separate from the memory patch
choice. The inspected scripts do not prove it caused the duplicate devices,
the six-minute delay, or CONFIGMG error. Preserve the restored working system,
retrieve AUTOEXEC.OLD (and the current CONFIG.SYS / SYSTEM.INI if needed), and
establish the actual setup path before proposing a reinstall or detection change.
