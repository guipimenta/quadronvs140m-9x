# First reverse-engineering pass: NVS 140M on Windows 98 SE

Follow-up: [second-pass chip-selection failure trace](CHIP_FAILURE_PATH.md)
connects the descriptors to the selector and identifies an unchecked object
call on its no-match path.

## Result

A concrete compatibility gap is now visible in the binaries. The Win98 NVCORE
registration routine at LCOD+0x134ccd registers 22 chip descriptors. Every one
of their identification predicates rejects the G86 family bits. The XP miniport
contains an explicit G86 predicate and a separate registration with associated
tables. This is stronger evidence than the INF or release dates alone.

This is a static result, not the exact fault location on the ThinkPad. We have
not executed the drivers or established the full path from CONFIGMG to the
faulting instruction. No installable patch has been produced.

## Address conventions and reproducibility

Win98 addresses below are offsets within object 1 (LCOD), unless an object is
specified. The analysis tool assigns object 1 base zero and subsequent objects
bases 0x01000000 apart. These are synthetic addresses, NOT live kernel addresses
or necessarily file offsets. XP addresses are the preferred virtual addresses
reported by objdump for that PE image, also not guaranteed runtime addresses.
The JSON exports contain page/file mappings and source SHA-256 hashes.

From the project root, run:

```
python3 tools/trace_driver.py
python3 -m unittest discover -s tools -p 'test_*.py' -v
```

Use normal Python, not `python -O`: format and fingerprint guards use assertions.
Requires Python 3 and GNU objdump; no third-party Python packages. XP extraction
from the earlier investigation must exist. Hardcoded trace offsets are guarded
by the recorded package hashes. The LE parser supports the internal 32-bit
absolute/relative fixups used by these files, including source lists and signed
page-relative offsets. It deliberately rejects unsupported relocation formats.

Decoded 1,100 fixup instances in NVMINI and 41,400 in NVCORE. Five checks pass:
original hashes, independently identified DDB pointers, a cross-object relative
call, page mappings/relocation targets, and rejection of an unsupported fixup.
These checks validate the analysis plumbing; they are not driver runtime tests.

The full `.asm` files are linear-sweep search aids and include data interpreted
as instructions. In particular, VxD `int 20h` has a four-byte service operand
that ordinary objdump incorrectly disassembles as subsequent instructions.
Do not treat those lines as real control flow. The focused excerpts used below
were inspected at known instruction boundaries and avoid that ambiguity.

## Recovered startup interfaces

| Component | Location | Evidence |
| --- | --- | --- |
| NVMINI DDB | 0x30ee0 | Name NVMINI; relocated control pointer 0x31690 |
| NVMINI control | 0x31690 | EAX 0x1b branches to ICOD+0x80 |
| NVMINI dynamic initialization | ICOD+0x80 | Calls LCOD+0x31f80 via 0x30000dd |
| NVCORE loader call | NVMINI 0x32029–0x32054 | Loads name at 0x312f0 through PNP+0x38; saves returned DDB+0x20 to global 0x30f4c |
| NVCORE DDB | 0x2df960 | Control pointer 0x2df9ff; protected-mode API pointer 0x2dfa10 |
| NVCORE control wrapper | 0x2df9ff | Calls 0x12a1e0; translates zero return into carry-clear success |
| NVCORE control dispatch | 0x12a1e0 | Two-stage table dispatch; message 0x1b selects 0x12a2a9 |
| NVCORE dynamic init branch | 0x12a2a9 | Conditional calls to 0x1343e1, 0x134441, 0x14b851; tests return values |
| NVMINI API wrapper | 0x335b4 → 0x316f4 | Builds request and calls saved pointer at 0x30f4c |
| NVCORE API wrapper | 0x2dfa10 → 0x12c5de | Passes EBX/EBP as ECX/EDX into request dispatcher |

NVMINI issues request 0x44c at 0x320f0. NVCORE dispatches 0x44c to
0x12c867, which calls 0x140eb9. That routine constructs internal objects and
calls a function at object slot +0x2dc (0x140efb). The factory code includes
assignment of 0x134ccd to that slot at 0x140d99. This connects the chip
registration routine to the initialization API, subject to confirming the
runtime object instance through a debugger.

The loader shim at NVMINI PNP+0x38 encodes VXDLDR service 0x00270001.
This must be read as a VxD service call plus operand, not ordinary instructions
following the interrupt. The name at NVMINI LCOD+0x312f0 is NVCORE.VXD.

## Chip identification comparison

`chip_registration.json` follows all 22 direct constructor calls in
NVCORE's 0x134ccd routine, extracts each installed predicate from its
`[eax+0x3c]` assignment, and verifies its first masked-family comparison.
The only family values in this recovered registration list are:

```
0x01000000  0x02000000  0x03000000  0x04000000
```

Each compares the input against mask `0x1f000000`. G86's BOOT0 family portion
is `0x08000000`, so it fails that first condition in all 22 predicates,
independent of stepping. This result is limited to this registration list;
it is not an exhaustive proof that no other dormant path exists anywhere.

Example: Win98 constructor 0x2854d9 installs predicate 0x285573 and several
table addresses. Its predicate is equivalent to:

```
return (id & 0x1f000000) == 0x04000000
    && (id & 0x00f00000) == 0x00600000;
```

That identifies NV46, not G86. It is accompanied by tables at 0x57110,
0x57158, 0x571a0, 0x571e8, 0x57230 and 0x57340, among others.

XP predicate 0xe0230 checks G86's family 0x08000000 and implementation
0x00600000, then excludes a specific low-byte stepping combination (0xa1).
Its constructor at 0xe0280 installs it in slot +0x24 and associates tables
including 0x449058 and 0x44ba18. The PCI revision A1 in the user's boot log
must NOT be substituted for the low byte of the GPU BOOT0 register: they are
different fields. Other revision-specific registrations may exist.

The differing descriptor layouts (Win98 predicate at +0x3c versus XP +0x24)
and associated implementation tables demonstrate why copying the small XP
predicate or changing the Win98 family comparison would be insufficient.
An altered predicate would select older chip-specific implementations; it
would not port G86 memory, display, interrupt or rendering behavior.

## Interpretation of the observed failure

The log's successful dynamic initialization is consistent with reaching the
NVCORE control branch and returning successfully before later API work. The
log does not record each internal API request or chip predicate. CONFIGMG's
protection error and subsequent Code 11 locate the broader device-start failure,
but do not prove which specific call faults. Caps Lock responsiveness does not
establish a working graphics stack or a simple LCD routing issue.

## Next concrete reverse-engineering work

1. Trace the descriptor-selection loop and its no-match result back into the
   adapter-start request, identifying the error/null-pointer propagation path.
   The recovered 22 predicates give a specific no-match hypothesis to test.
2. Follow the factory assignment at 0x140d99 through the slot +0x2dc call, and
   map subsequent NVMINI requests after 0x44c to the NVCORE dispatcher.
3. Inventory the XP G86 descriptor tables and their reachable functions; compare
   the required object layouts and OS services before attempting any transplant.
4. For a hardware trace, obtain actual loaded object bases with a compatible
   kernel debugger and break on the recovered entry points. Do not use synthetic
   addresses directly. Capture the failing instruction, register state, stack,
   and relevant GPU identification registers. Boot logs cannot supply these.

A diagnostic patch might eventually expose or gracefully report the no-match
path, but no such patch is justified yet. A diagnostic success would also not
mean accelerated graphics support. The current evidence favors a substantial
G86 implementation port over a small compatibility patch.

## Reference material

* LE/VxD loading and fixup research:
  https://www.geoffchappell.com/notes/windows/archive/vxdldr.htm
* GPU identification register research:
  https://envytools.readthedocs.io/en/latest/hw/bus/pmc.html
* VxD control constants in a current Win9x driver project:
  https://github.com/JHRobotics/vmdisp9x/blob/main/vmm.h

All driver-specific addresses and comparisons above come from the supplied
binaries. Function names such as "constructor" and "predicate" are descriptive
reverse-engineering labels, not recovered NVIDIA symbols.
