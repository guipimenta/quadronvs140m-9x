# OpenGL artifact review — 2026-10-29

Scope: the resolution-dependent artifacts in Half-Life, especially 1024×768,
reviewed at commit `50cce8e` (build 3104). This is a review and a reproducible
defect report, not a claim that the T61 artifacts have been fixed. Production
driver code and packaged binaries were left unchanged.

## Findings

### 1. Confirmed: zero uploads can retain bytes from a previous BO

In `driver/vmdisp9x-nv/nvshim.c`, `nouveau_bo_map()` allocates both `base.map`
and `clean` with `calloc`. It seeds them from real VRAM only for a read map.
A new write-only map therefore claims that zero is the clean GPU baseline,
even though `heap_free()` does not clear the freed physical allocation.

`shim_bo_flush()` compares these buffers in 1024-byte chunks. A chunk that
the caller explicitly writes entirely with zeros compares equal and is never
uploaded. Reused VRAM still contains the previous BO's data. Partial zero
regions inside an otherwise changed chunk do get uploaded; the failure needs
an entire comparison chunk to remain equal to the invented baseline.

The defect reproduces using the actual map and flush functions, with only
the VRAM transport replaced by a byte array:

```
python3 tools/repro_nvshim_zero_upload.py

WR, zero chunks: stale bytes=2048/3072; shadow equals clean=1
WR, zero chunks, FULLFLUSH: stale bytes=2048/3072; shadow equals clean=1
RDWR baseline control: stale bytes=0/3072; shadow equals clean=1
WR, nonzero control: stale bytes=0/3072; shadow equals clean=1
```

Expected exit status with the reviewed source: 1. The fixture compiles with
`gcc -std=c99 -Wall -Wextra -Werror` and reads the implementation from the
working tree rather than maintaining a second implementation of it.

This also explains a diagnostic blind spot: the ordinary command-stream
self-check compares the CPU shadow with `clean`, so it accepts this failure.
`NVSHIM_FULLFLUSH` expands the comparison to the whole BO; it does not disable
dirty comparison. Its failure to cure an artifact cannot rule out this bug.
`NVSHIM_VERIFYVRAM` can detect an affected upload if the walk marks the
corresponding range and it falls within the readback limit.

**Relevance:** a proven upload-coherence defect, potentially affecting texture,
vertex, and constant data. It is not yet proven to cause the observed corner
blocks or their resolution dependence. The patterned texcheck result does not
exercise every possible upload content/reuse sequence.

**Fix requirement:** establish an accurate initial GPU baseline before relying
on dirty comparison. Reading VRAM into both buffers at the first map is a
straightforward correctness reference, with a potential staging-upload cost.
An optimized solution must distinguish initialized CPU upload ranges from
GPU-owned contents. Blindly forcing every mapped BO to upload on every kick
can overwrite GPU results with stale CPU shadows.

### 2. The resolution evidence remains valid; the channel bug is separate

The 3099 notes and capture document one Half-Life process rendering with
artifacts at 1024×768 and cleanly after an in-game switch to 800×600. That
comparison survives the later discovery that subsequent GL processes reused
a stale channel. The 3103 notes' statement that resolution was a “red herring”
is too strong: process order confounded the later nvsky comparisons, not this
earlier within-process observation.

Build 3103 introduced a separate mid-initialization reset bug; 3104 changes
the duplicate-handle detector to address it. The root-level captures available
at review time still identify VxD 3103 / shim 3099. There is no reviewed T61
capture proving that 3104 restores clean sequential launches, or that the
original 1024×768 problem disappears with it.

Use build 3104's default context (`GrCtx=0`) as the candidate baseline, and
verify first and second launches before interpreting a context-variant A/B.
The detector still infers a new process from repeated object handles; it is
not an actual process/channel ownership mechanism. Simultaneous clients or
object recreation require more work and are outside this artifact review.

### 3. “Corner holes caused by clipping” is a hypothesis, not a result

The 3099 notes infer missing triangle coverage from descriptions/crops of
corner blocks. No reviewed result records those blocks turning into the clear
color with `gl_clear 1`. The later sky tests were also confounded by process
order, and a clean sky test alone does not reproduce Half-Life's full state.

The checked-in Mesa patch and active Mesa source do use scissor-based clipping:
`VIEW_VOLUME_CLIP_CTRL=0` is intentional upstream behavior, not evidence by
itself of incorrect state. Upstream also has a geometric-clipping path using
`0x1080`, making that a useful controlled diagnostic, not an established fix.

Do not replace more context blobs or global PGRAPH initialization values until
the failure is localized. Their broad effects make comparisons harder, and
the earlier context variants were not all tested under equivalent conditions.

## The next hardware comparisons

Keep the same driver, context variant, game scene/viewpoint and pixel depth.
Save logs per run, including the VxD/shim build line. First verify sequential
launches at one resolution, then compare 800×600 → 1024×768 → 800×600 within
one Half-Life process. Mode switches can reallocate/re-upload resources, so
this does not hold allocation history constant, but it removes process order.

| Comparison | What it establishes |
| --- | --- |
| `gl_clear 0` versus `gl_clear 1` at the failing viewpoint | Blocks becoming the clear color supports uncovered pixels. Unchanged textured garbage leaves other paths open. |
| Default present versus `NVSHIM_CPU_PRESENT=1` | Existing CPU path detiles/readbacks and displays the rendered buffer. Clean CPU output implicates the GPU present path or its interaction with rendering state; corruption in both points earlier. Timing changes mean this is evidence, not absolute proof. |
| First-map VRAM-baseline reference fix, default clipping | Tests the confirmed zero-upload defect independently of PGRAPH context changes. Measure upload cost as well as image correctness. |
| Geometric clipping diagnostic, after the above | Tests guard-band/clipping only if evidence still points at missing coverage. Preserve depth-clamp bits and account for state emitted by draws and blits. |

The existing CPU-present switch is in the Mesa WGL path but is not exposed as
an `nvhl` argument. Set it before launching from a DOS box, then clear it after:

```
set NVSHIM_CPU_PRESENT=1
nvhl
set NVSHIM_CPU_PRESENT=
```

Expect a substantial performance drop. A screenshot/readback of an affected
frame would be more decisive than FPS or a packet-validator pass.

## Checks performed

- Read `README.md`, the artifact and context history in `docs/HW3D_PLAN.md`,
  the native GPU submit/bind/VM-kind paths, shim allocation/map/flush logic,
  the Mesa WGL present patch and active nv50 framebuffer/scissor/viewport code.
- Revalidated the 3099 capture with
  `python3 tools/nvkick_decode.py analysis/t61-logs/3099-nvkick.txt --check --log analysis/t61-logs/3099-nvshim.log --quiet`:
  64 captured kicks, nine resolved handles, no reported packet/log errors.
  This validates the captured stream's structure, not raster output or the
  remainder of the game session.
- Reproduced the stale-zero upload defect with the native fixture above,
  including the FULLFLUSH failure and two passing controls.
- Cross-checked the scissor/geometric distinction against upstream Mesa source:
  [nv50_screen.c](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/a9e75e34661414afe02ab9d1050560c19d3a21c3/src/gallium/drivers/nouveau/nv50/nv50_screen.c)
  and [nv50_state.c](https://chromium.googlesource.com/chromiumos/third_party/mesa/+/a11d1bd247df18057ecb9945e01bf39ff85d8d9d/src/gallium/drivers/nouveau/nv50/nv50_state.c).

No real G86 rendering test was possible on this host. A narrow final fix may
be small, but a working desktop and basic 3D path do not establish correctness
of buffer coherence, clipping, or presentation at every resolution.
