# What we already disassembled from NVIDIA's drivers

Everything below was recovered by static disassembly in earlier sessions (2026-10-18 to
2026-10-29), unless marked otherwise. Addresses are exact; the conventions matter, so read
"Inputs and address conventions" first. Function names ("constructor", "predicate",
"selector") are our labels: the binaries have no symbols. Re-derive before relying on a value
for new code, and never copy NVIDIA code: we use it to understand, then implement our way
(nouveau's way for hardware programming).

## Inputs and address conventions

| Binary | Package | SHA-256 (analysis/manifest.json) |
| --- | --- | --- |
| `analysis/xp_extracted/nv4_mini.sys` (6,620,064 B) | XP 6.14.11.7874 (178.74, Lenovo, 2009-01-14), `DISPLAY/nv4_mini.sy_` | `2251ed03...9dc32afc` |
| `analysis/xp_extracted/nv4_disp.dll` (6,284,160 B) | same, `DISPLAY/nv4_disp.dl_` | `1e84bf38...d85becd4` |
| `analysis/xp_extracted/nvoglnt.dll` (8,777,728 B) | same, `DISPLAY/nvoglnt.dl_` | `2e3908c4...dfaadcde` |
| `modified_driver/NVCORE.VXD`, `NVMINI.VXD`, `NVDISP.DRV`, `NVDD32.DLL`, `NVOPENGL.DLL` | the Win9x 82.69 package with a community INF (`NVAML.INF`) that lists device 0429 | see manifest |
| `nvidia_old_drivers/nvidia_9x_81.98` | last official NVIDIA Win9x driver (tops out at GeForce 7) | - |

- **XP PE images:** addresses are objdump VAs. For `nv4_mini.sys` the image base is
  0x10000 and **VA = file offset + 0x10000** (what `extract_xp_g86.py` relies on).
  `nv4_disp.dll`: ImageBase 0x10000 too, built 2009-01-15.
- **Win9x LE VxDs:** addresses are offsets inside object 1 (LCOD); `tools/trace_driver.py`
  gives object 1 base 0 and later objects bases 0x01000000 apart. These are synthetic,
  not runtime, addresses. Reproduce with `python3 tools/trace_driver.py` and
  `python3 -m unittest discover -s tools -p 'test_*.py' -v` (plain python, not -O). The LE
  parser (`tools/le_vxd.py`) decodes 1,100 fixups in NVMINI and 41,400 in NVCORE.
- **Linear-sweep caveat:** the big `.asm` dumps (`analysis/reverse/NVCORE.LCOD.asm`,
  `NVMINI.LCOD.asm`, `XP_nv4_mini.asm` 110 MB) disassemble data as code. A VxD `int 20h` is
  followed by a 4-byte service operand that objdump shows as bogus instructions. Use the
  focused excerpts (`analysis/reverse/*.asm`, small files) or start at known boundaries.
- **Unpack:** `7z x -y -oanalysis/xp_extracted DISPLAY/<file>` (what was used) or
  `msexpand`. Both folders are git-ignored.

## XP miniport `nv4_mini.sys` (resource manager): chip objects and PGRAPH

**Chip registration model.** Chip objects are registered by constructors that store a
predicate (identifies the chip from BOOT0) and an engine-method table of
`{engine<<24 | method id, function}` pairs. On XP the predicate sits at descriptor +0x24
(Win9x NVCORE: +0x3c, a different layout).

**The G86 object** (`analysis/reverse/xp_g86_predicate_and_registration.asm`):
- Constructor **0xe0280** installs predicate **0xe0230** and method table **0x44ba18**
  (0x50c pairs); class list **0x449178** names classes **8297** (G84+ 3D), **5097** (G80
  3D), **5039** (M2MF), **502d** (2D), **50c0** (compute).
- Predicate 0xe0230, exactly:
  ```c
  if ((boot0 & 0x1f000000) != 0x08000000) return 0;   /* NV50 family */
  if ((boot0 & 0x00f00000) != 0x00600000) return 0;   /* G86 */
  if ((boot0 & 0xf0) == 0xa0 && (boot0 & 0x0f) == 1) return 0;  /* not BOOT0 stepping A1 */
  return 1;
  ```
  The PCI revision (A1 in the T61's boot log) is a different field from BOOT0's low byte.
  Other tables tied to it include 0x449058.

**PGRAPH = engine 6**, 61 methods; three are G86-specific:
- **0x2f = 0x2506c0, unit init:** 0x40008c = 4; ctxctl flags 0x400824/0x40082c/0x400830 = 0;
  0x402ca8 = 0x800; zcull regions zeroed; TPC 0x408000 = 0x30; the units' clock-gating
  registers (the 0xf0c242 family, envytools names).
- **0x30 = 0x250ae0, installs the context data**, after a PGRAPH signature check
  (0x400800 == 0x5049e8ac or 0x7ee17cc6):
  - ctxprog = **224 dwords at 0x47a2b8** (starts 0x0070008e, ends 0x0060000c);
  - context size **0x5bca0** bytes;
  - golden context = **723 {byte offset, value} pairs at 0x3aeaf0** (written by context
    create **0x2a1780** into a zeroed buffer);
  - MMIO list = **512 {register, grctx offset, init value} triples at 0x3b0188** (first
    register 0x400808);
  - **230 {method>>2, value} pairs at 0x47a638** = the 3D class defaults, fed to PGRAPH
    through ICMD (0x400200 / 0x400204).
- **0x40 = 0x250d10** (G86-specific, not analysed further).
- **0x1f = 0x2a8b40, engine init:** writes every MMIO-list register (TPC ones gated by
  the TPC mask), then the ICMD methods.
- **Context switching:** first bind **0x2a8760** sets 0x400824 |= 0x800 and writes CTXCTL_CUR
  directly; later switches **0x2a38c0** use 0x400824 |= 0x20 (save) / 0x40 (load) then
  0x400304 |= 1. These are the flag bits nouveau 2.6.33 used when it loaded these blob
  ctxprogs as firmware (= our manual load).

`analysis/xp_grctx/extract_xp_g86.py` pulls all of it (with sanity asserts) into
`driver/vmdisp9x-nv/nv50_grctx_g86_xp.h` and the readable listing
`analysis/xp_grctx/g86_xp_context.txt` (ctxprog disassembled with the CP_* opcodes of
nouveau's ctxnv50.c, MMIO list, golden values, ICMD methods named from Mesa's
`nv50_3d.xml.h`).

**XP vs nouveau (ctxnv50.c) for the G86:**
- Same 502+10 MMIO registers in the same order. The blob also lists the second ROP
  partition's 0x4071xx, which this 1-ROP G86 lacks; XP writes them anyway.
- MMIO part at grctx byte 0x100, with 0x400828 at 0x11c (nouveau: dword 1). The xfer areas
  sit 0x100 bytes later (0xa00 / 0x52d00), with the same strand sizes 0x2916 and 0x442.
- TPC registers are context-switched only when 0x400830 bit 0 is set. The manual-load path
  uses CMD 0x11 / NEWCTXDMA.
- Golden xfer values = nouveau's (current and 2.6.36) plus the second ROP partition's
  strand-3 values, nothing else.
- MMIO init values differ in exactly **three registers**:

| Register | XP | nouveau |
| --- | --- | --- |
| 0x401c00 TRAST | 0x042500df (G92/G96/G98 value) | 0x044d00df |
| 0x405000 CCACHE | 0x000e0080 | 0x00000080 |
| 0x400844 | 0 | 2 |

- Per-TP 0x40821c / 0x40829c = 0x018c0000, the same as current nouveau.

**Outcome on the T61:** build 3102 offered GrCtx 3 (XP ctxprog + golden context + XP init) and
GrCtx 4 (nouveau + the three XP values). Neither removed the 1024x768 corner holes.
The real cause was storage-kind row-block packing in our VRAM heap (fixed in 3109). The
default since 3111 is variant 2, current nouveau. Lesson: the XP context data matches
nouveau's, so a hardware context discrepancy is unlikely to be the cause of a rendering bug.

## Win9x NVIDIA stack (82.69 modded package): why it cannot run a G86

From `analysis/reverse/FINDINGS.md` and `CHIP_FAILURE_PATH.md`.

**Startup interfaces** (LCOD offsets):

| Item | Location |
| --- | --- |
| NVMINI DDB | 0x30ee0; control 0x31690 (EAX 0x1b -> ICOD+0x80) |
| Dynamic init | calls LCOD 0x31f80 |
| NVCORE loading | NVMINI loads `NVCORE.VXD` through VXDLDR (service 0x00270001, PNP+0x38) at 0x32029-0x32054; keeps DDB+0x20 at 0x30f4c |
| NVCORE DDB | 0x2df960; control 0x2df9ff -> dispatcher 0x12a1e0 (message 0x1b -> 0x12a2a9) |
| NVCORE PM API | 0x2dfa10 -> request dispatcher 0x12c5de |
| Request 0x44c | NVMINI sends it at 0x320f0; NVCORE 0x12c867 -> 0x140eb9 builds objects and calls slot +0x2dc, which the factory sets to **0x134ccd** at 0x140d99 |

**Chip registration 0x134ccd:** 22 constructors (`analysis/reverse/chip_registration.json`).
Every predicate masks with 0x1f000000 and accepts only families **0x01-0x04** (NV1x...G7x).
G86's family 0x08 matches none. Example: constructor 0x2854d9 installs predicate
0x285573 = `(id & 0x1f000000) == 0x04000000 && (id & 0x00f00000) == 0x00600000` (NV46).

**The crash path** (CONFIGMG protection error, then Code 11):
- Method installs: 0x18ef48 installs chip-object methods (+0x80 = 0x18efa7 returns
  object+0xa4, so the descriptor's +0x3c predicate lives at chip object +0xe0).
- Selector: 0x140e49 installs selector 0x1413bc at root +0x84. It scans 31 slots and calls
  each +0xe0 predicate. The initial ID is 0x1f; it returns 0 on a match, 0xffffffff
  otherwise.
- Select-and-bind: 0x18bce1 gets the BOOT0 read through device method +0x40c and saves it
  at device +0x150. It selects into device+0x84, keeps the status in EDI at 0x18bd1b, and
  calls binder 0x18bd29 **without checking it**.
- The fault: the binder looks up type 6, ID 31 through root lookup 0x141148 -> 0x14126b
  (slots hold IDs 0..30), gets 0, and executes **`call [esi+0x98]` at 0x18bd4b** through a
  null object. The status check at 0x18bbed comes too late.

**Conclusion:** G86 support had to be re-implemented, not patched. A guard would only turn the
crash into a refusal. Accepting the G86 in an old predicate would bind NV4x method tables. The
XP code is NT kernel-mode (imports ntoskrnl, HAL, VIDEOPRT, WIN32K), so it cannot be transplanted.

## XP display driver `nv4_disp.dll` (DirectDraw / Direct3D DDI): NOT yet disassembled

Known so far:
- XPDM kernel-mode display driver; imports only WIN32K.SYS.
- One export: DrvEnableDriver at RVA 0x2eaf60.
- Strings show shared D3D/OpenGL registry settings (`D3DOGL_*`, e.g.
  `D3DOGL_EnableConsumerStereoSupport`) and `ZDirection`.

How to get into the Direct3D code when needed:
1. DrvEnableDriver fills a DRVENABLEDATA table of {INDEX_Drv*, function} pairs. The
   DirectDraw entries are INDEX_DrvGetDirectDrawInfo (59), INDEX_DrvEnableDirectDraw (60)
   and INDEX_DrvDisableDirectDraw (61).
2. DrvGetDirectDrawInfo / DrvEnableDirectDraw return the DD callbacks; the
   `GetDriverInfo` callback compares the request GUID against the DDI GUIDs. Search the image
   for the GUID bytes (values in mingw's `ddrawint.h` / `d3dhal.h`, or vmhal9x's `d3d.c`).
3. The GUID branches give the D3D callbacks (context create/destroy, DrawPrimitives2) and
   the caps blocks (D3DCAPS8/9 via GETD3DCAPS8/9, the format list via GETFORMAT, Z formats).
   The DrawPrimitives2 parser is a large switch over D3DDP2OP_* opcodes.
4. To see what it programs into the hardware for a D3D state, follow writes of nv50 3D class
   (0x8297) methods; Mesa's `nv50_3d.xml.h` names them.

## XP OpenGL ICD `nvoglnt.dll`: NOT yet analysed

User-mode GL ICD (PE, Windows 4.00 subsystem). It is the reference for how NVIDIA maps GL
state to the 3D class when a Mesa/nouveau choice is in doubt.

## Related: Microsoft's Windows 98 VDD (not NVIDIA)

`analysis/win98_vdd/README.md`: the DDK debug `VDD.VXD` with symbols, disassembled during
E4 (display-driver API keyed on device id; the mini-VDD of a non-VGA devnode is reachable
only through its id; `IgnoreRegistry` and DDC behaviours).
