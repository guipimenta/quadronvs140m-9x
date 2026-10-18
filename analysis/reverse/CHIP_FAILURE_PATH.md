# Second pass: chip-selection failure reaches an unchecked object call

## Finding and limits

The recovered Win98 initialization path contains a concrete error-handling
problem: a failed chip selection is followed by chip-specific binding before
the caller checks the failure. Under the recovered object layout, that binding
looks up the out-of-range sentinel chip ID and attempts an indirect call through
a null object pointer.

This is a static, conditional failure path in the supplied binary, not a crash
dump or an observed execution on the ThinkPad. It is a plausible explanation
for the protection error. The boot log cannot establish the faulting instruction,
and there may also be earlier failures in resource mapping or register access.
No compatible driver or installable patch is produced by this analysis.

## Trace

All addresses here are NVCORE LCOD offsets, not runtime addresses.

1. `0x18ef48` installs chip-object methods. The `+0x80` method at
   `0x18efa7` returns `object + 0xa4`. Therefore the predicate written at
   descriptor `+0x3c` by each registration constructor lives at chip-object
   `+0xe0`. This resolves the indirect-call linkage missing from the first pass.
2. `0x140e49` installs selector `0x1413bc` at root-object `+0x84`.
   The selector scans 31 slots, skipping empty entries. It retrieves each
   populated type-6 chip object and calls its `+0xe0` predicate with the ID.
3. The selector starts with selected ID `0x1f`. On a match it replaces that
   value with the slot's identifier. It writes the selected ID through the
   output pointer, returning zero for a match and `0xffffffff` for no match.
   The first pass established that all 22 registered predicates reject G86
   family bits. Unregistered slots are skipped, not treated as fallbacks.
4. `0x18bce1` gets the root object and calls device method `+0x40c` with offset
   zero. It saves the returned value at device `+0x150` and passes it to the
   selector with output pointer `device + 0x84`. The method assignment at
   `0x18b8ff` points to `0x18ce26`; that wrapper ultimately calls another
   object's `+0x148` method. This pass does not yet prove the final MMIO mapping.
5. At `0x18bd1b`, selection status is saved in EDI. **Without testing it**, the
   function calls `0x18bd29`, which binds chip-specific methods.
6. The binder asks the root for type 6, ID `device->selected_id`. Root lookup
   dispatcher `0x141148` routes type 6 to `0x14126b`.
   Initialization at `0x140ddc` gives its 31 slots IDs `0..30`; the miss value
   31 is outside that set. Lookup returns EAX=0 if the ID is absent.
7. The binder copies EAX to ESI and immediately executes
   `call dword ptr [esi+0x98]` at **`0x18bd4b`**, with no null check.
   For the missing ID, the call reads through address `0x98` instead of a
   valid object's method. Whether this faults at the operand read or transfers
   to an invalid target depends on the live low-memory mapping and contents.
8. The outer caller does test the selection status at `0x18bbed`, but only
   after the select-and-bind function returns. The unchecked call precedes it.

Conceptual pseudocode, reconstructed from instructions (not original source):

```c
int select_and_bind(Device *d) {
    Root *root = get_object(global_root, 0, 0);
    uint32_t id = d->read_wrapper(d, 0);
    d->id = id;
    int status = root->select_chip(root, id, &d->selected_id);
    bind_chip_methods(d);       // unconditional, including status == -1
    return status;
}

void bind_chip_methods(Device *d) {
    Chip *chip = get_object(global_root, 6, d->selected_id);
    d->method_3c4 = chip->lookup_method(chip, 0xf0000001);
    // Other method lookups follow. No check for chip == NULL.
}
```

## What a fix would and would not achieve

A guard that returns the selection error before binding could remove this
specific failure path. It would still refuse to initialize G86: that is a
diagnostic/error-handling repair, not GPU support. It must also preserve the
original successful-call behavior and relocation rules.

Changing an old chip predicate to accept G86 would bypass the rejection while
selecting an older chip's method tables. The binary evidence does not justify
that substitution. A compatibility port still requires G86-specific methods
and their dependencies from the XP implementation, adapted to the Win98 object
and OS interfaces.

The next useful runtime observation is the selector's returned status and ID,
followed by the type-6 lookup result. `chip_failure_path.json` contains offsets
for these observation points and their original file offsets. A debugger must
add NVCORE's **live LCOD base**; do not use file offsets as breakpoint addresses.
If selection is not reached, tracing must move earlier into the register-read
wrapper or device resource setup. If selection succeeds, this conditional
no-match path is not the explanation for that particular boot.

## Reproduce

```
python3 tools/trace_driver.py
python3 tools/trace_chip_failure.py
python3 -m unittest discover -s tools -p 'test_*.py' -v
```

The second script verifies original SHA-256 and exact instruction anchors for
the descriptor layout, selector, lookup, and unchecked call. It exports six
focused disassembly files and the JSON observation points. These are static
verification checks, not runtime tests or hardware emulation.

Additional disassembly caveat: parts of this driver contain `eb 01 0f`
sequences: a short jump skips a byte that can misalign a linear sweep. Follow
the jump target before interpreting subsequent instructions. The focused
selection/binding excerpts above do not depend on interpreting those bytes.
