# Windows 98 SE VDD.VXD analysis (E4 r21, 2026-10-20)

Source: Microsoft Windows 98 DDK (archive.org item `DDK-9x-ME`, `98DDK.RAR`):
`DEBUG_WINDOWS98SE/DEBUG/VDD.VXD` + `.SYM` (full symbols) and the real
`minivdd.h` / `minivdd.inc` from `INCS_DDK.CAB`. Analysis copies only.

## Why it mattered
nvmini.drv loaded at boot but `DriverInit` returned 0 (`LoadFail = nvmini.drv
0000`). Disassembly of `VDD_PM_API` (segment _DOSVM) shows the display-driver
API dispatcher keys on **Client_EBX as a display device id (1..N)**:

- `cmp word [ebp+0x10],1` (Client_EBX): id 1 -> jump table A
  (`VDDRegisterDisplayDriverInfo` -> global `MiniVDDDispatchTable[0]`),
  any other id -> table B (`VDDRegisterDisplayDriverInfo2` -> `_CallMiniVDD2`
  -> that device's private table at `device+0x2b5`).
- `_LoadMiniVDD` (segment _PNP): for a devnode flagged as the VGA
  (`_IsThisDevNodeTheVGA` = its config owns I/O 3B0-3BB) the mini-VDD from
  `DEFAULT\minivdd` is loaded and the global table is also copied to the
  device table. For a NON-VGA devnode it reads `DEFAULT\MiniVDD2`, loads it,
  copies the global table into the device's private table, then RESTORES the
  global table from the first (VGA) device. So the mini-VDD of a non-VGA
  devnode is only reachable through its device id.
- `_OpenDevice` (VDD_OPEN 0x8D): "DISPLAY" is rejected; names are
  `\\.\DisplayN` (`_GetDeviceName`: `%s\UnitN` for multi-unit). Handle for
  the first device unit 0 is 1, otherwise `device_base_id + unit`.
  `VDD_OPEN_ENUM` (EDX=2) with EBX=id just validates the id.

vmdisp9x's 16-bit side passed EBX = VM id (1) everywhere, which only works
while our adapter is the VGA devnode. With the T61's ghost "Standard PCI
Graphics Adapter (VGA)" occupying id 1, our mini-VDD sat in id 2's table.
Fix (build 3031): `pm16_calls.c VXD_VM_connect` probes ids 1..4.

NVIDIA's own 9x driver (nvdisp.drv 81.98, `Enable` at seg1:0x5cc) does the
equivalent: `VDD_OPEN(lpDeviceType)` then `VDD_REGISTER_DISPLAY_DRIVER_INFO`
with the returned handle, and never closes handle 1.

## Tools (repo `tools/`)
- `ne_header.py <file.drv>` / `ne_relocs.py <file.drv> <seg>` - NE parsing.
- `mapsym.py <file.SYM>` - MAPSYM reader.
- `vdd_disasm.py <workdir> <maxlines> <symbol|GROUP:off:len>...` - symbol
  annotated ndisasm over LE object images produced by `tools/le_vxd.py`
  (expects `vddimg_<n>_<name>.bin` + `vdd_se_debug.syms` in workdir).

## IgnoreRegistry kills the mini-VDD load (2026-10-21)
`VDD_PNP_Load_MiniVDD` (_RARE 0x10eb) starts with
`Get_Profile_Boolean("Display", "IgnoreRegistry", default 0)` and stores the
result in `IgnoreRegistryFlag` (_RARE 0x104d). If it is non-zero the routine
takes `VPLM_Good_Exit` BEFORE `VDD_PNP_Get_Display_Info` and before the
`_CONFIGMG_Load_DLVxDs` call that loads `DEFAULT\minivdd`. So a SYSTEM.INI
`[display] IgnoreRegistry=1` (which vmdisp9x's init.c reads for its own mode
override) makes Win98 boot WITHOUT the mini-VDD: BOOTLOG shows no
"Dynamic load device nvmini.vxd", nvmini.drv then fails DriverInit
(`LoadFail = nvmini.drv 0000`) and Windows falls back to vga.drv.
Observed on the T61 on 2026-10-21 (boot log tick 00153D97). Fix: never set
IgnoreRegistry; put the desktop mode in the registry instead (nvsetup.reg:
`Config\0001\Display\Settings` Resolution/BitsPerPixel and `DEFAULT\Mode`).
