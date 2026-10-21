# Restored, bootable stock Windows 98 registry

Inputs: `display.reg`, `enum.reg`, and the accompanying Device Manager photos.
These are the working state after SCANREG restore, not a capture of the failed
boot. No registry edits were made.

| Display entry | Enumeration instance | Driver binding |
| --- | --- | --- |
| Standard Display Adapter (VGA) | `Enum\Root\*PNP0900\0000` | `Display\0000` |
| Standard PCI Graphics Adapter (VGA) | `Enum\PCI\VEN_10DE&DEV_0429&SUBSYS_20D817AA&REV_A1\000800` | `Display\0001` |

Both class keys use `MSDISP.INF`, section `VGA`, `DevLoader=*vdd`, and
`DEFAULT\drv=vga.drv`. Both have mode `4,640,480`. Neither binds our native
driver or NVIDIA's modified driver.

The root VGA entry has `DetFunc=*:DETECTVGA` and a BootConfig containing
I/O ranges 03B0–03BB and 03C0–03DF, plus memory A0000–AFFFF and B8000–BFFFF.
The screenshots independently show that entry owning both I/O ranges.
The PCI entry's LogConfig also contains 03B0–03BB and 03C0–03DF resource
descriptors, alongside its PCI memory and other resources. LogConfig is not
proof that all those resources were successfully allocated.

Combined with the photographed PCI Code 12 (no free I/O range), the overlapping
VGA requests strongly support duplicate resource claims as the reason the PCI
entry cannot start in this working configuration. The export does not establish
the exact arbitrator decision or the cause of the six-minute delay/CONFIGMG
error after removal of the root entry. Resource assignment and subsequent
adapter initialization are distinct stages.

Neither exported display DEFAULT key contains DDC. That describes the restored
snapshot only; it does not invalidate the user's previous unsuccessful DDC=0
test, and is not a reason to repeat it.

There is also a root keyboard at `Enum\Root\*PNP0303\0000` and a BIOS keyboard at
`Enum\BIOS\*PNP0303\07`, bound to Keyboard\0000 and Keyboard\0001 respectively.
Thus duplicate enumeration is not confined to graphics. This alone does not
prove a chipset, ACPI, BIOS, or installer fault.

Keep the restored configuration as the baseline. Do not infer that deleting
the working root VGA node is a repair: the user already demonstrated that this
exposes a different startup failure. To locate that failure, the next evidence
needed is a timed logged boot from the failing/slow configuration, or a runtime
trace of CONFIGMG/VDD; this registry snapshot contains no startup timings.
