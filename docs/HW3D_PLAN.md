# Hardware 3D on the G86 (native OpenGL/Direct3D acceleration) - plan

Goal: render OpenGL (and later D3D) on the Quadro NVS 140M's own 3D engine
instead of on the CPU (Mesa llvmpipe). This is the largest piece of the whole
project and a multi-stage hardware bring-up on the real T61, in the same style
as the EVO display bring-up (docs/EVO_PLAN.md, builds 3001-3010).

## CURRENT STATUS - RESUME HERE (as of build 3074, 2026-10-24)

The GPU bring-up and the winsys foundation are DONE and verified on the T61.
What is built, in order, each released and tested:

- M5.1  (v3061): the 2D engine (0x502d) executes commands and fills VRAM.
- M5.2a (v3064): channel VIRTUAL MEMORY - page directory @RAMIN+0x200 + a VRAM
  page table translate a GPU virtual address to physical VRAM.
- M5.2b (v3067): a VRAM HEAP allocator + a GPU FENCE (a 2D write of a sequence
  value to a heap BO via the VM; CPU polls it).
- M5.3a (v3074): a PERSISTENT CHANNEL that RING-3 submits command streams to,
  using nv50 INDIRECT-BUFFER (IB) mode. `nvprobe submit` proves user space drives
  the GPU (two fills, different colors, both land).
- M5.3b (v3075): the libdrm_nouveau SHIM - the ~30-function nouveau winsys
  contract implemented in ring-3 (driver/vmdisp9x-nv/nvshim.c + nvshim.h) over
  the VxD, so Mesa's nv50 driver can be wired to it UNCHANGED (M5.3c). VERIFIED
  on the T61: `nvprobe shim` fills a heap BO through the contract and reads it
  back (all corners = fill color).

All the mechanism facts (register values, instance layout, IB entry format, the
DMA-pusher dead end, VM/PTE/PDE formats, the grctx) are in the auto-memory file
hw3d-gpu-accel.md - READ IT before continuing.

Test commands on the T61 (opt-in, touch only PGRAPH/PFIFO):
  nvprobe gpu2d | vmtest | fence | submit | shim

### M5.3b DONE (v3075, VERIFIED on T61) - the libdrm_nouveau shim
nvshim.c implements the exact nouveau.h contract (copied verbatim from mesa-24.1.x
src/gallium/winsys/nouveau/drm/nouveau.h into nvshim.h so the struct ABI matches
and M5.3c is a mechanical include-swap). Design decisions made:
- SCOPE: fences are NOT in the contract - they live in the driver's nouveau_fence.c
  (3D QUERY method), so they are M5.4, not the shim. The shim is device/client/
  object/bo/bufctx/pushbuf only.
- BO MAP = BOUNCE BUFFER. Mesa runs in RING-3; the VxD's BAR1 linear map is a
  ring-0 system-arena address, not writable from ring-3. So nouveau_bo_map returns
  a malloc'd CPU SHADOW; pushbuf_kick flushes referenced shadows to VRAM (new ioctl
  OP_NV_VRAM_WRITE 0x5015) before submitting, and a read-map refreshes the shadow
  from VRAM (OP_NV_VRAM_READ). Submits are synchronous in the VxD, so this is
  coherent + in-order. (Later optimization: a true ring-3 BAR1 mapping to drop the
  copies - needs _PageCommitPhys into the process arena; noted, not done.)
- VRAM is a ring-3 BUMP heap over 0x04000000..0x08000000 (64 MiB), inside the
  channel's 1:1 VM window (first 128 MiB). bo->offset = phys + 1 GiB. No free yet.
- object_new only records the node; the channel/engine objects are still brought up
  by the VxD's nv_chan_init (lazily on first OP_NV_SUBMIT). Addresses are fixed, so
  pushbuf_reloc/validate are no-ops.
Build it into nvprobe: `i686-w64-mingw32-gcc -O2 -s tools/nvprobe.c nvshim.c -I. -o
tools/nvprobe.exe` (nvshim.c defines NVSHIM_STANDALONE; nvprobe.c does too before
including ../nvshim.h). `nvprobe shim` runs the whole contract end to end: a 2D fill
into a heap BO addressed by its VM offset, read back through the shim map. Expect
"*** THE libdrm_nouveau SHIM DRIVES THE GPU - M5.3b works! ***".

### M5.3c step 1 DONE (host-verified): the shim builds against Mesa's real headers
nvshim.c now compiles BOTH standalone (nvprobe, -DNVSHIM_STANDALONE) and against
Mesa's own headers (no define -> includes nouveau.h + nvif/cl0080.h). Verified on
the host with a -fsyntax-only build (exit 0, no warnings), which proves the struct
ABI matches (the container-of casts compile) and the two formerly-stubbed functions
are right: nouveau_device_info fills a real nv_device_info_v0 (platform=PCIE so
nouveau_check_for_uma()=false, chipset 0x86, family TESLA), and object_mclass needs
no change (nv50_screen picks tesla_class = NV84_3D by chipset, not via mclass).
Mesa's uapi headers drag in <drm-uapi/drm.h> -> <sys/ioccom.h> (Linux only), so a
minimal Win9x shim lives at driver/vmdisp9x-nv/mesa-compat/drm-uapi/nouveau_drm.h
(just the NOUVEAU_GETPARAM_* + SVM symbols the driver references); put mesa-compat
FIRST on the include path. Host check command:
  cd <mesa-24.1.x>; i686-w64-mingw32-gcc -fsyntax-only -Wall \
    -I <drv>/mesa-compat -I <drv> -I src/gallium/winsys/nouveau/drm \
    -I src/nouveau/drm -I src/gallium/include -I src/include -I include -I src \
    -I src/util <drv>/nvshim.c

### M5.3c step 2a DONE (host-verified): the WHOLE nv50 stack compiles under mingw
*** The active mesa tree is mesa-25.1.x (config.mk MESA_VER), NOT 24.1.x. *** Its
nouveau.h contract differs from 24.1 only by a `struct nv_device_info info` field in
nouveau_device + a nouveau_device_set_classes_for_debug() hook - both handled in
nvshim.c (the latter under #ifndef NVSHIM_STANDALONE). Confirmed with -fsyntax-only
against mesa-25.1.x (i686-w64-mingw32-gcc-posix / g++-posix, -std=gnu99 / gnu++17):
- 31/31 CORE driver C files compile (drivers/nouveau/*.c + nv50/*.c MINUS the 4
  video-decode files nouveau_vp3_video.c / nv84_video.c / nv98_video.c /
  nv98_video_vp.c - GL does not need VP3/VP2; EXCLUDE them from the lib).
- 26/26 codegen C++ files (src/nouveau/codegen/*.cpp) compile.
- nouveau_drm_winsys.c + nvshim.c compile.
So step 2 is a Makefile-integration + link task, NOT a source port. Requirements found:
- Compat shims in driver/vmdisp9x-nv/mesa-compat/ (put FIRST on -I): drm-uapi/
  nouveau_drm.h (GETPARAM + FIFO_ENGINE_* + SVM), xf86drm.h (drmCommandWrite stub),
  sys/ioccom.h (_IO* macros), util/os_mman.h (os_mmap/os_munmap stubs + MAP_*/PROT_*).
- Include path MUST include src/gallium/winsys/nouveau/drm, src/nouveau/headers (for
  nv_device_info.h), src/nouveau/codegen, src/nouveau/drm (for nvif/*), and
  src/gallium/drivers/nouveau; and MUST NOT include src/nouveau/winsys (its
  nouveau_context.h/etc shadow the classic gallium headers). The real build gets
  git_sha1.h from mesa9x/include and -DMESA_GIT_SHA1 from DEFS.
- nvshim.c compiles in TWO modes: -DNVSHIM_STANDALONE (nvprobe) or, without it,
  against Mesa's own nouveau.h; for the mesa build it needs -I<drv> and -I<drv>/
  mesa-compat added (for vxd_nv.h/nv50_gpu.h), but driver files must NOT get -I<drv>
  (our io.h collides).

### M5.3c step 2b-i DONE (host-verified): MesaNouveauLib.a builds
The nouveau driver lib is wired into the mesa9x build and archives cleanly:
- mesa-25.1.x.mk: MesaNouveauLib_SRC (NVDRV var + wildcard of drivers/nouveau/*.c +
  nv50/*.c with the 4 bad video files filtered out, + codegen/*.cpp + winsys
  nouveau_drm_winsys.c + $(NVDRV)/nvshim.c). Makefile: NV_INCLUDE/NV_CFLAGS/
  NV_CXXFLAGS (mesa-compat FIRST, + NV_DEFS=-DMESA_GIT_SHA1=\"\" since nouveau_screen.c
  lacks svga's fallback), a dedicated `_nv` object suffix + rules, and the
  MesaNouveauLib lib rule. `make libMesaNouveauLib.a` -> 2 MB archive, 58 objects,
  exit 0. The mesa9x-tree diff is saved in driver/mesa9x-bin/nouveau-build-win9x.patch
  (that tree is outside this repo).

### M5.3c step 2b-ii DONE (host-verified): the nouveau lib LINKS cleanly
Trial-linked a stub that calls nouveau_drm_screen_create against libMesaNouveauLib.a +
the mesa simd libs (--start-group): resolves to just 3 undefs - crt_sse2_is_safe
(extra/crtex.c), _aligned_malloc9x/_aligned_free9x - all already in mesa3d.dll's own
link set. So linking MesaNouveauLib into mesa3d.dll will resolve. Needed fixes found:
- Exclude ALL video files (%_video.c %_video_bsp.c %_video_vp.c %_video_ppp.c), not
  just the 4 that fail to compile - the others pull the vl_* video layer.
- Their entry points are still REFERENCED by core screen/context init, so stub them
  in driver/vmdisp9x-nv/nvshim_stubs.c (also in MesaNouveauLib_SRC): nv30/nvc0_screen_create
  (return NULL - only nv50 built; they return struct nouveau_screen *, NOT pipe_screen),
  nouveau_screen_init_vdec/_context_init_vdec, nv84/nv98/vp3 video query+codec hooks.
- Link with g++ (libstdc++ for the C++ codegen) and --start-group (circular lib deps).

### M5.3c step 2b-iii DONE (host-verified): the nouveau mesa3d.dll BUILDS + loads
`make mesa3d.w98me.dll` -> 80 MB, exit 0, no undefined refs, fixlink checksum applied,
and impcheck_win98.py reports "missing on Win98: none" for every imported DLL. The
first hardware-nouveau OpenGL ICD for Win9x exists and is Win98-loadable. How:
- Makefile: MesaNouveauLib added to the mesa3d.w98me.dll prereqs + link line, wrapped
  -Wl,--start-group ... $(opengl_simd_LIBS) -Wl,--end-group (circular lib deps). A
  TARGET-SPECIFIC `wgl_icd.c_simd.o: SIMD_CFLAGS += -DGALLIUM_NOUVEAU` sets the define
  on ONLY the ICD's wgl object, so opengl32/SVGA (which share wgl.c) stay software and
  need not link nouveau.
- wgl.c: added a `nouveau` case to wgl_screen_create_by_name (GALLIUM_DRIVER=nouveau ->
  nouveau_drm_screen_create(0); our nouveau_drm_new opens \\.\nvmini.vxd), a use_nouveau
  flag, and a wgl_present guard that early-returns for a nouveau screen (real present is
  M5.4; a hw screen has no sw displaytarget to gdi_sw_display). nouveau is OPT-IN - the
  default DLL stays llvmpipe/software, so it is safe to ship before hw init works.
All mesa-tree edits are in driver/mesa9x-bin/nouveau-build-win9x.patch (3 files).

### M5.4 DONE (build 3078, released v3078, T61-VERIFIED) - the NV84_3D engine runs
`nvprobe 3d`: the G86's own NV84_3D (0x8297) engine executes the exact QUERY_GET
write-fence nv50_screen_create emits and writes the sequence (3D000001) to a
VM-mapped BO with PGRAPH_INTR=0 (no trap). First hardware execution of the 3D
engine under this driver; the screen-init fence blocker is cleared.
- 3077 (step 1): bare QUERY after SET_OBJECT(3D)+DMA_NOTIFY TRAPped
  (PGRAPH_INTR=0x00200000, a PGRAPH unit trap) - the UNIT_CROP fence waits on the
  CROP/ROP unit, which had no valid DMA/RT state.
- 3078 (step 2): run the BO-FREE PREFIX of nv50_screen_init_hwctx before the QUERY
  (DMA_NOTIFY + DMA_ZETA(11) + DMA_COLOR(0)(8) all = VM ctxdma 0xbeef0003, then
  REG_MODE/UNK1400_LANES/WATCHDOG/ZETA_COMP/RT_COMP(8)/RT_CONTROL/CSAA/MULTISAMPLE/
  PRIM_RESTART/BLEND/SCREEN_Y/WINDOW_OFFSET/ZCULL). Now the CROP unit is valid and
  the fence lands. Implemented as NVF_3D in gpu_2dfill_core; `nvprobe 3d` dumps the
  trap regs (0x400108 + DISPATCH/VFETCH/CCACHE) if a future trap occurs.

### M5.4 step 3 (builds 3079-3081): running Mesa's nv50_screen_create on the T61
- 3079 shipped the NOUVEAU mesa3d.dll (opt-in: nouveau.reg sets GALLIUM_DRIVER=nouveau
  for wgltest.exe/icdtest.exe via HKLM\Software\vmdisp9x\apps\<exe>\mesa, the same
  per-exe registry env mechanism as MESA_EXTENSION_MAX_YEAR in d3dsetup.reg). On the
  T61: wgltest -> "FAILURE: CreateContext: 126" (= nouveau_drm_screen_create returned
  NULL; 126 is a stale GetLastError, not the cause).
- 3080 moved the shim trace to a ring-3 file C:\nvshim.log (the VxD ring's per-second
  heartbeat evicted it). T61: still 126 AND "File not found" - the shim's first trace
  point (nouveau_drm_new) was never reached.
- 3081 ROOT CAUSE: Mesa's nouveau_drm_screen_create() = u_pipe_screen_lookup_or_create(
  os_dupfd_cloexec(fd), ...): it dup()s + fstat()s the DRM fd for an fd-keyed screen
  cache. wgl.c passes fd 0; for a GUI process on Win9x that is invalid, so it returned
  NULL before nouveau_screen_create (which calls the shim) ever ran. FIX: under WIN9X
  nouveau_drm_winsys.c calls nouveau_screen_create(fd, NULL, NULL) directly (the shim
  ignores the fd and opens the VxD itself). 4th file in nouveau-build-win9x.patch.
  mesa3d.dll rebuilt; its import table is IDENTICAL to 3080's (266 entries, the six
  Win98 DLLs) - verified by diffing import sets with impcheck's own parser, since the
  QEMU Win98 image (source of the export set) is gone. Awaiting the T61 run: expect
  C:\nvshim.log to exist and show how far nv50_screen_create gets.

- 3081 on the T61: NO change (126, no log) -> the failure is upstream of the shim.
- 3082 traces the CALLER CHAIN into C:\nvshim.log via an exported nvshim_trace():
  wgl.c logs screen_create ENTER (+GetLastError), the GALLIUM_DRIVER name read, each
  driver tried + result + GetLastError, nouveau_drm_screen_create enter/exit; the
  winsys logs each nouveau_screen_create step. BUILD TRAP FOUND: wgl_icd.c #includes
  wgl.c but make did not know, so wgl_icd.c_simd.o had been STALE since 10-24 23:29 -
  every wgl.c edit after that (the nouveau case, the present guard, these traces) was
  NOT in the shipped DLLs until the object was deleted and rebuilt. The Makefile now
  declares wgl.c a prerequisite of that object (in nouveau-build-win9x.patch). After a
  Mesa rebuild ALWAYS check: strings mesa3d.w98me.dll | grep 'wgl: try driver' (>0).

### M5.4 step 3 DONE (build 3082, T61-VERIFIED 2026-10-27): nv50_screen_create RUNS
With `set GALLIUM_DRIVER=nouveau` in the DOS box, wgltest's C:\nvshim.log shows the
whole path succeed on hardware: try driver 'nouveau' -> drm_new/device_new ok, chipset
0x86 -> Mesa binds notifier/M2MF/2D/NV84_3D(0x8297)/compute via the shim -> 6 BOs
allocated -> ONE kick of 0x25c dwords (Mesa's full init_hwctx incl. the BO-backed
VP/FP/TLS/CB tail) with PGRAPH_INTR=0 -> init(dev) screen=00d62010, context_create
valid, wgl result screen ok. Mesa's unmodified nv50 driver is running the G86.
WHY it never worked before: GALLIUM_DRIVER read EMPTY from the registry path -
os_get_option (WIN9X) = getenv() then vmhal_setup_str("mesa",name) which lives in
vmdisp9x.dll (win9x/vmsetup.h: GetModuleHandle/LoadLibrary "vmdisp9x.dll" +
GetProcAddress); that returned nothing (lasterr 2). The env var bypasses it.
(Earlier 126 errors came from the STALE wgl object, not the hardware.)

### M5.4 step 4 (build 3083): first real present for the nouveau screen - AWAITING T61
wgl_present(nouveau) = pipe_texture_map(ctx,res,READ) -> Mesa's nv50 transfer de-tiles
the tiled window miptree with M2MF into a staging BO -> shim maps it (VRAM readback)
-> vramcpy_display_raw() (win9x/vramcpy.c, new: FBHDA fast path / StretchDIBits
fallback) pushes the pixels into the framebuffer. Shim changes needed for this:
nouveau_bo_map/bo_wait now kick the pending pushbuf first (libdrm's implicit kick;
without it the M2MF copy is never submitted before the readback) and the bump heap
rolls back when the top-most BO is freed (the per-present staging buffer), so
presenting every frame does not exhaust it. Patch is now 6 files (+ vramcpy.c/.h).
Expect: wgltest's window shows the GPU-cleared gray; log 'wgl: present map/stride'
with a non-zero map. If map=0: transfer_map failed (bo_new/M2MF) - check the kick
lines for pgi != 0 (an M2MF trap would be the first use of that engine).

### M5.4 step 4b (build 3084): context-creation trace - AWAITING T61
3083 on the T61: screen create OK (log identical to 3082) then CreateContext 126 - the
failure is in CONTEXT creation, which had no trace. 3084 adds a runtime hook:
u_debug.c defines `void (*mesa9x_trace_hook)(label,n,vals)` = NULL (every target links
util, so the shared stw/st objects stay link-clean for opengl32/svga); wgl.c sets it to
nvshim_trace when nouveau is selected. Traced into C:\nvshim.log: stw_create_context_
attribs (enter hdc/pfi, profile, st_api_create_context result, no_ctx/no_st_ctx exits),
stw_pixelformat_init count, st_api_create_context (pipe, st), nv50_create (enter,
blitctx, nouveau_context_init ret, bufctx_new x3, stream_uploader, OK/FAILED),
nouveau_context_init (client_new/pushbuf ret), shim client_new/bufctx_new. The LAST
line names the failing step. (Build gotcha: this Mesa's stw_device has no
pixelformat_extended_count.) Patch is now 12 files.

### M5.4 step 4c (build 3085): ICD-surface trace - AWAITING T61
3084 on the T61: the log ends at `stw: pixelformat_init count 0x1e0` (480 displayable
formats) and NOTHING from stw_create_context_attribs - so wglCreateContext never reaches
context creation; the failure is earlier. In `wgltest driver mesa3d.dll` mode the only
non-ICD step is ChoosePixelFormat: wgltest's DrvGetProcAddress("wglChoosePixelFormat")
misses (stw only exports the ARB variant), so it falls back to GDI's ChoosePixelFormat ->
opengl32 -> the registered ICD (nvmini.drv reports "SOFTWARE" -> OpenGLdrivers\SOFTWARE
= mesa3d.dll; pftest proved that route in 3051 with 576 llvmpipe formats). Then
DrvSetPixelFormat(idx) must create the framebuffer and DrvCreateLayerContext must find
it; by the code both succeed for any idx in 1..480. Candidates: ChoosePixelFormat
returned 0 (index>=count exit), a framebuffer already existed, stw_dev went NULL
(stw_cleanup from a DETACH), or opengl32 loaded a SECOND mesa3d.dll instance through
its registry path. 3085 traces the whole ICD surface: DescribePixelFormat (count
queries, first/every-100th describe, bad-size/no-pfi exits), SetPixelFormat (every
exit, framebuffer_create result, the GDI GetPixelFormat/SetPixelFormat calls + lasterr),
CreateLayerContext (entry hdc/lasterr, stw_dev NULL exit, pfi, ctx/handle), SetContext/
ReleaseContext/DeleteContext, SwapBuffers/PresentBuffers, ValidateVersion/
SetCallbackProcs (opengl32's handshake), GetProcAddress misses by name, stw_cleanup,
choose/guess/get_info_from_hdc, DllMain ATTACH per instance + DETACH. The shim log is
now shared APPEND mode with a `==== shim log open, instance` header, so a second
instance can neither truncate nor be locked out (delete C:\nvshim.log between runs).
Patch is now 15 files (+ stw_framebuffer.c, stw_getprocaddress.c, stw_device.c);
import table +SetFilePointer only.

### M5.4 step 4d (build 3086): ROOT CAUSE of "CreateContext 126" - it was never the GPU
3085 on the T61 (nouveau AND `nvgl sw`): the trace ends `DescribePF count-query` ->
`SetPF enter ipf 0` -> `SetPF FAIL index>=count` -> `CreateLayerContext lasterr 0x7e`,
i.e. GDI's ChoosePixelFormat returned 0 with error 126 before any ICD call, for every
renderer. Reproduced on this host in a QEMU Win98 SE VM (tools/qemu98/, raw copy of the
VirtualBox disk, headless, QMP-driven, ~3 min/boot) on plain vga.drv with softpipe:
identical log. pfprobe.exe (driver/vmdisp9x-nv/tools) then repeated GDI
ChoosePixelFormat at each step: fresh process -> 0/126; after LoadLibrary("opengl32.dll")
-> format 3; after FreeLibrary -> 0/126 again; mesa3d.dll load / screen create change
nothing. **Windows 9x GDI32 forwards ChoosePixelFormat/SetPixelFormat/
DescribePixelFormat to OPENGL32.DLL only if it is already loaded in the process; it
does not load it.** Apps that import opengl32 (pftest, Half-Life) never see this;
wgltest's `driver` mode loads only mesa3d.dll + gdi32, so every driver-mode run since
3079 died there. Fix: wgltest.c driver mode LoadLibrary()s opengl32.dll (16th file in
nouveau-build-win9x.patch). mesa3d.dll: NVSHIM_TRACE=1 routes the stw/st traces for any
driver (nvgl.bat sets it) - no other change. Also learned: nvmini.vxd with Native=0
deliberately skips the VBE path ("deadlocks on G86"), so nvmini cannot drive the VM's
desktop; for an ICD-route test in the VM build the vesamini pair and bind that instead.
Windows 98's opengl32 ICD loader (disassembled: OPENGL32.DLL 78ab021c) requires the
OPENGL_GETINFO version field == 2, LoadLibraryA()s the registry value as-is, and needs
DrvValidateVersion + 7 Drv entry points; 126 is what its LoadLibrary failure returns.
Expected on the T61 with 3086: `nvgl` log continues into stw_create_context_attribs ->
st_api_create_context -> nv50_create (the 3084 traces), then the first present.

### M5.4 step 5 (build 3086 on the T61): FIRST GPU FRAMES - then a freeze
With wgltest fixed, `nvgl` on the T61 ran the whole path (log kept in
analysis/t61-logs/3086-nvshim-first-gpu-frames.log): opengl32's ICD handshake
(ValidateVersion/SetCallbackProcs, ~1000 DescribePF calls), format 51 chosen, framebuffer
created, GDI SetPixelFormat accepted, `nv50: create OK`, `st_create_context`,
`CreateLayerContext dhglrc 2`, MakeCurrent, then TWO FRAMES through Mesa's nv50 driver:
colour buffers (0x140000 each) + a 0x200000 depth buffer allocated, 11 kicks all pgi=0,
SwapBuffers -> M2MF de-tile into a 0x1165a0 staging BO -> `wgl: present map/stride`
twice -> the window showed BANDED NOISE (stale VRAM, not the clear colour). The log ends
in the third frame after a `kick ok` with NO failure line: the next kick/readback never
returned = a wedged GPU + the VxD's bounded-but-long ring-0 polls (2M+4M register reads
per kick) repeated for every kick of the render loop -> the machine froze. Also seen: a
0x140000 BO per frame (stw re-creates a texture each frame; the bump heap never reuses it
-> ~45 frames until the 64 MB heap is gone - needs a real allocator soon).
3087: VxD latches `g_gpu_wedged` on PGRAPH trap or unconsumed IB (ok=0, wedged/idle_loops
fields) and refuses later kicks at once; `S> submit begin/end/WEDGED/REFUSED` in the ring,
captured by `nvprobe tail C:\nvtail.txt` which nvgl.bat now starts first. Shim logs every
kick before/after (first 400), stops after the first failed kick (GPU DEAD), logs big
readbacks and heap exhaustion. Present logs 4 sample pixels (expected: the clear colour
in the corners, triangle colour mid-frame). Next log answers: which kick wedges (its
first methods), and whether the colour buffer ever received the clear.

### M5.4 step 5b (build 3087 on the T61 -> 3088): the 3D engine hangs SILENTLY in the frame kick
3087 did not freeze. Logs (analysis/t61-logs/3087-*): no trap anywhere. Kick sequence of the
first frame: #1 hwctx init 604 dw (ok), #2 2D 34 dw + #3 2D 126 dw (DST_FORMAT 0xf3 on subch 4
= nv50's 2D clears/blits, ok), #4 the frame 668 dw (3D subch 3, starts COLOR_MASK_COMMON
0x0f90) "consumed", #5 the 4-dw QUERY fence (3D 0x1b00) NEVER consumed: IB get 4 / put 5,
pfi=pgi=0 after 2M polls. So the 3D engine stopped inside #4 without a trap and the next 3D
method stalled the pusher. Present samples random -> the clear never landed. The VxD passed
#4 as ok because it never checked that PGRAPH went idle. Side facts: the T61 desktop is
640x480x16 (scaled); Mesa maps the new 2 MB depth BO right after creating it (the shim seeds
the shadow with a 2 MB VRAM readback, 32768 ioctls); the bump heap grows 1.3 MB per frame.
3088: VxD latches wedged when PGRAPH is still busy after the idle wait (gridle + PGRAPH_STATUS
logged); shim dumps the first 16 kicks' full method streams to C:\nvkick.txt. NEXT: decode
kick #4 against nv50_3d.xml.h (which state/draw it carries, which BO addresses it
references) and compare with a reference nouveau trace of the same frame; suspects: the
hwctx tail Mesa expects (VP/FP code BO, TLS, stack, uniforms) vs what our channel provides,
and vertex-fetch/shader addresses in the VM window.

### M5.4 step 5c (build 3088 on the T61 -> 3089): it is the PFIFO PULLER that is stuck, and the
### init kick binds a COMPUTE object the shim never created
3088 reproduced 3087 exactly (analysis/t61-logs/3088-*) with one new fact: at the wedge
PGRAPH_STATUS reads 0 (idle) and gridle=0 after the frame kick - the 3D engine is NOT busy,
so the pusher/puller is what stopped, not the engine. The kick dump decoded cleanly
(tools/nvkick_decode.py, output in 3088-nvkick-decoded.txt): kick 0 = nv50_screen_create's
init (SET_OBJECT M2MF/2D/3D, DMA handles, the whole hwctx tail), kicks 1/2 = a 32-byte TSC
upload through 2D SIFC (R8 linear, DST 0x44250000) + TSC_FLUSH, kick 2 also = the glClear
(RT 0x44274000 A8R8G8B8 632x451 tiled, ZETA 0x443b4000 Z24S8, CLEAR_BUFFERS 0x3c), kick 3 =
the frame (state, VP/FP code uploads via SIFC into the code BO at +0x7ff40/+0x7ffc0,
VERTEX_ARRAY 0x444f400c stride 24, VERTEX_BEGIN_GL POLYGON, 3 vertices), kick 4 = the QUERY
fence. Every method is a plain nv50 frame - EXCEPT dword 427 of kick 0: SET_OBJECT
0xbeef50c0 on subchannel 6 (COMPUTE) followed by ~100 compute methods. nvshim.c's
nouveau_object_new only RECORDS class 0x50c0 (never bound it; nv_class_is_gfx in the VxD
did not list it either), so the puller met a handle absent from RAMHT. The arithmetic fits:
everything after dword ~410 of the init stream (1002 dwords) fits the pusher's cache, which
is why IB get reached 4 (fetched) and stopped before 5. The clear never ran because the
puller never got past kick 0 -> random present samples. No CACHE_ERROR interrupt was seen
(pfi=0), which the 3089 stall dump will explain (PULL0 / the cache entry at CACHE1 GET).
3089: shim + VxD bind 0x50c0/0x85c0 as PGRAPH objects; NV_gpu_submit dumps the PFIFO
pusher/puller + PGRAPH state on a wedge (S> pfifo / S> c1 / S> cache[..] <- GET / S> gr
lines in nvtail); 'submit end' carries dmaget/c1put/c1get/pull0; the shim logs the captured
pusher state on a failed kick. Expect: no wedge, the clear colour (ff000000) in the present
samples, the triangle in the window. If it still wedges: the cache[..] <- GET line names
the stuck method.

### TESTING PRINCIPLE (2026-10-27, user directive): simulate first, T61 last
The T61 round trip is the scarce resource. Everything that does not need the silicon is
tested in a simulated environment BEFORE a build is released for the T61:
- Win9x loader / GDI / ICD-route behaviour: the QEMU Win98 SE harness (tools/qemu98).
- Mesa's nv50 command streams: the shim's FAKE-GPU mode. `set NVSHIM_FAKE=1` (plus
  `GALLIUM_DRIVER=nouveau`) and mesa3d.dll never opens the VxD: VRAM is a RAM heap,
  binds are recorded, and every kick is walked the way the PFIFO puller would react to
  it (unbound SET_OBJECT handle, method on a subchannel with no object, DMA_* naming an
  unbound handle, bad header, truncated packet, QUERY address outside the VM window ->
  the kick wedges exactly like the VxD reports a real stall: `kick FAIL`, `GPU DEAD`,
  plus a `FAKE WEDGE <reason> dword/..` line naming the dword). The 3D QUERY fence is
  emulated so Mesa's fences signal. Nothing is rendered. `NVSHIM_KICKDUMP=N` dumps N
  kicks to C:\nvkick.txt (default 16). Runs in the Win98 VM or under Wine on the host
  (`cd dist/driver; GALLIUM_DRIVER=nouveau NVSHIM_FAKE=1 wine wgltest.exe driver
  mesa3d.dll window`; logs land in ~/.wine/drive_c/).
- Kick dumps from ANY source (T61 or fake): `python3 -I tools/nvkick_decode.py
  nvkick.txt --check --log nvshim.log [--tail nvtail.txt] [--quiet]` decodes the streams
  against the nv50 class headers and runs the same puller checks against the bound
  handle set (exit 2 on a fault). On the 3088 T61 dump it reports
  "kick 0 dword 427: SET_OBJECT subch 6 handle beef50c0 is NOT bound" - the 3087/3088
  stall, found offline.
- Import tables: `tools/impcheck_win98.py` after every mesa3d.dll rebuild.
What only the T61 can answer: PFIFO/PGRAPH behaviour on real streams, modesetting, timing.
Each T61 run must therefore carry maximal instrumentation (kick dump, stall dump, tail).

### M5.4 step 5d (build 3090): simulate-first tooling, and the first bug it caught
Built under the user's directive (2026-10-27): fake-GPU shim mode (NVSHIM_FAKE=1), the
kick validator (tools/nvkick_decode.py --check), a Wine loop on the host
(tools/qemu98/wine_fake_run.sh run from dist/driver: ~30 s per iteration, logs in
~/.wine/drive_c/). Results of the first runs:
- The validator on the 3088 T61 dump reports the compute-handle fault at kick 0 dword
  427 - the 3087/3088 stall, found with no hardware.
- Mesa's nouveau path under Wine with the fake GPU: screen, two contexts, frames; every
  kick clean (reference trace in analysis/fake-runs/3090-wine-fake-nvkick*.txt - compare
  future T61 dumps against it). Then the bump heap ran out after ~53 frames
  (fence BO + present staging + an occasional non-top-most 1.25 MB surface per frame)
  and Mesa crashed on the failed bo_new - exactly what the T61 would have done a minute
  into any run. Replaced by a first-fit free-list allocator (coalescing frees, tiled BOs
  64 KiB-aligned like nouveau): 4987 frames in 45 s, heap flat at 8.5 MB used / 55 MB
  largest free block.
- Fences: Mesa's fence.update reads the screen fence BO's map directly, never through
  bo_map, so the shim's shadow never saw the GPU's sequence write and every
  nouveau_fence_wait quietly returned false (on the T61 too). The shim now refreshes the
  3D QUERY target dwords into the owning BO's shadow after each synchronous kick.
- Every bound handle is logged ('bind handle/class/engine'), NVSHIM_KICKDUMP=N.
VxD/DRV byte-identical to 3089 bar the stamp. The T61 run of 3090 answers what only the
silicon can: does the puller get through the init kick now that the compute object is
bound, and does the clear land (present samples ff000000, triangle in the window).

### M5.4 step 6 (build 3090 on the T61 -> 3091): the puller runs; the first tiled RT trips a PROP trap
3090 on the T61 (analysis/t61-logs/3090-*): the init kick (604 dw) and the TSC upload (34 dw)
complete (c1put == c1get, no interrupt) - 3089's compute bind was the puller stall. Kick 2
(TSC upload + glClear) raises PGRAPH_INTR 0x00200000 (TRAP) with 0x400108 = 0x100 = PROP
(nouveau nv50_gr_prop_trap: 2D-destination / render-target / CUDA-memory faults) while the
dispatcher sits at RT_ARRAY_MODE (0x400704 = 0x80031224), i.e. right after RT_ADDRESS
0x44270000 + RT_TILE_MODE 0x40 bound the first TILED render target. The stream itself is
byte-identical to the Wine reference bar addresses (validator clean). Cause: nouveau writes a
surface's storage KIND (memtype & 0x7f) into PTE bits 40-46 (vmmnv50.c: map->type |= kind
<< 40; kernel: kind = (tile_flags & 0x7f00) >> 8); our 1:1 VM page table has 0 there
(linear), and the ROP path refuses a tiled target on linear-kind pages.
3091: OP_NV_VM_KIND (0x5019): the shim tags every tiled BO (tile_mode/memtype set; now
64 KiB-aligned and 64 KiB-sized) with Mesa's kind (0x70 colour, 0x28 Z24S8, ...; no
compression bits - nouveau drops comp when it has no tags) and the VxD writes the PTE high
dwords + invalidates PGRAPH's TLB (0x100c80 = (0 << 16) | 1, wait bit 0); freed BOs go back to
kind 0. NV_gpu_submit recovers PGRAPH traps like nouveau's nv50_gr_intr (per-unit dump:
DISPATCH/M2MF/VFETCH/STRMOUT/CCACHE and per-TP TEXTURE/MP/PROP with PROP's fault address
e10/e14 and the 2D(0x10)/RT(0x40)/CUDA(0x80) bits; clear unit status 0xc0000000; ack
0x400108 + 0x400100; 0x400500 = 0x10001) and continues the kick (16 max), reporting
traps/trap_units to the shim ('kick TRAP recovered'). So one T61 run now reports every
fault with its address instead of stopping at the first. Fake-GPU/Wine run of 3091: clean.
Expect on the T61: no trap, present samples ff000000, the triangle. If PROP still traps:
the 'S> tp-prop' line gives the faulting address and which of 2D/RT/CUDA.

### MILESTONE 2026-10-28 (build 3091 on the T61): THE TRIANGLE. Mesa's nv50 driver renders on the G86 under Windows 98
wgltest's triangle drawn by the GPU through Mesa's unmodified nv50 Gallium driver, the Win9x
libdrm shim and the nvmini VxD (analysis/t61-logs/3091-nvshim-FIRST-TRIANGLE.log: no traps
in the draw kicks, present samples ff000000 / ff3f407f). The PTE storage kind was the last
blocker. What the run also showed: ~1 frame/s, and every frame's M2MF readback copy trapped
on its notifier write (M2MF ustatus 0x80000001 = NOTIFY: our notifier ctxdma covered the VM
window, so BUFFER_NOTIFY wrote to VM address 0; recovered each time).

### M5.5 step 1 (build 3092): speed - GPU present, dirty-diff flushes, 1024-dword reads
Per-frame cost in 3091: read the 1.1 MB colour buffer back through the uncached BAR in
64-dword ioctls (~4500 ioctls), write it into the framebuffer, re-write EVERY mapped BO's
whole shadow into VRAM before EVERY kick (the 2 MB vertex buffer, the code BO...), and on a
BO's first map read it all back (32768 ioctls for the vertex buffer). 3092:
- GPU PRESENT: wgl.c imports the FBHDA scanout surface (surface offset, pitch, bpp) as a
  pipe_resource via resource_from_handle (shim: nouveau_bo_name_ref name 1 = the front
  buffer, registered with nvshim_set_front) and blits the window buffer into the window's
  client rect with pipe->blit + flush. Under the fake GPU Mesa chose the 2D engine: a 37-dword
  kick (DST R5G6B5 linear pitch 1280 @ VM 0x40100000, SRC the tiled A8R8G8B8 window
  buffer, BLIT_DST_X/Y = the window position) - reference in analysis/fake-runs/3092-*.
  NVSHIM_CPU_PRESENT=1 restores the readback path. Clipping against other windows is not
  done (vramcpy_top_window gate only).
- Shim: DIRTY-DIFF flush (1 KiB chunks against a clean copy, only CPU-changed runs written;
  GPU-written data in a mapped BO is no longer clobbered), no seed readback for write-only
  maps, OP_NV_VRAM_READ2 (1024 dwords/ioctl), [ms] timestamps on every log line,
  'wgl: present frames/ms' + kick/flush/read counters every 2 s.
- VxD: the notifier class gets a real 4 KiB block (NVGPU_NOTIFY 0x03320000) -> no more
  per-frame M2MF trap; NVGPU_PLAYLIST moved to 0x03310000 - it was at 0x03204000, INSIDE
  the 1 MiB pushbuffer data region, overwritten after 16 KiB of commands (latent hang if
  PFIFO ever re-reads the playlist).
Fake-GPU/Wine: ~370 fps, flush ~1.4 KB/frame, validator clean. Expect on the T61: the
triangle moving smoothly; the log's frames/ms line gives the real frame rate.
NEXT (M5.5): per-frame readback of nothing; then Half-Life (fullscreen present = the same
blit; textures through M2MF now that the notifier is real); window clipping/overlap.

### 3092 on the T61 (2026-10-28): ~60 fps, smooth, no traps
analysis/t61-logs/3092-*: 'wgl: present frames/ms' = 113/2012, 126/2002, 115/2005 ms (wgltest's
own pacing), 4 kicks per frame (frame 126 dw, 92 dw, the 37-dw 2D present blit, the 5-dw fence),
flushes ~1 KB/frame, reads ~2 dwords/frame, zero traps, zero failed kicks. The desktop is
1024x768x32 (FBHDA surface 0, pitch 4096, bpp 32) and the 2D engine blits the A8R8G8B8 window
buffer into it directly. From ~1 fps (3091) to ~60 fps with the CPU out of the frame loop.
NEXT: Half-Life. Needs: (1) the front resource must follow display-mode changes (HL goes
fullscreen 640x480/800x600 x16: FBHDA surface/pitch/bpp/size change -> re-import), (2) a
textured scene in wgltest to validate texture upload (M2MF linear->tiled, TIC/TSC) under the
fake GPU before the T61, (3) launch recipe: DOS box with set GALLIUM_DRIVER=nouveau (+
MESA_EXTENSION_MAX_YEAR=2000 for hl.exe) and the registry ICD route (OpenGLdrivers\SOFTWARE).

### M5.5 step 2 (build 3093): towards Half-Life
- wgl.c: the front resource is dropped and re-imported when FBHDA surface/pitch/bpp/width/
  height change (a game going fullscreen 640x480x16 gets a B5G6R5 front; the 2D blit
  converts from the window buffer's format).
- wgltest 'texture' scene (wgltest.c + wgltest_api.h, 17th file of the Mesa patch): two
  textures (256^2 RGBA with 9 mip levels via glTexImage2D per level, 64^2 RGBA alpha sprite),
  a glTexSubImage2D 32x32 patch every 16 frames (lightmaps), a textured cube with depth test
  and linear fog, a blended + alpha-tested sprite, perspective projection. Under the fake GPU
  (analysis/fake-runs/3093-wine-fake-texture-*): Mesa uploads textures with M2MF linear ->
  tiled copies from a staging BO (TILING_MODE_OUT 0x40, 1024 B x 256 lines), tiled texture
  BOs get kind 0x70 (0x40000 + 0x60000 for the mip chain), TIC/TSC uploads via 2D SIFC, 96
  kicks clean, 567 frames / 2 s; the software renderer runs the same scene.
- nvhl.bat + INSTALL.txt: copy nvhl.bat + mesa3d.dll next to hl.exe, set NVDRV, run nvhl
  [-width 640 -height 480]; it sets GALLIUM_DRIVER=nouveau + MESA_EXTENSION_MAX_YEAR=2000,
  tails the VxD log, runs hl.exe, leaves the three log files.
T61 order: (1) `wgltest driver mesa3d.dll window texture` with GALLIUM_DRIVER=nouveau (nvgl's
DOS box) - the textured cube must render; (2) Half-Life via nvhl.bat. Known gaps for games:
no clipping of the present blit against overlapping windows (fullscreen is fine), 64 MB VRAM
heap with no eviction (a bo_new failure crashes Mesa - watch 'heap full' in the log), no GART.

### 3093 on the T61 (2026-10-28): untextured cube, Half-Life "OpenGL mode not supported"
analysis/t61-logs/3093-*. Two shim bugs, neither visible to the fake GPU as it was:
- wgltest 'texture': ~59 fps, 0 traps, every kick ok, the M2MF uploads exactly as in the fake
  reference - but the cube had no texture. The source of every upload is a staging BO
  (nouveau_bo_new GART|MAP, mapped write-only, filled by the CPU) that Mesa refs in bufctx
  bin 0, PUSH_VALs, then resets bin 0 and lets go of (nouveau_fence_work unref) BEFORE the
  kick. libdrm's pushbuf_validate krefs the bufctx's BOs into the pushbuf and holds them
  until the submit; our validate was a no-op and the kick flushed only the bufctx current at
  kick time, so the staging shadow never reached VRAM and the engine copied untouched VRAM
  into the texture. Vertex data worked only because the 2 MB uploader BO stays in bufctx_3d.
- Half-Life: the shim log ends after 'bo_new 00181000' (code) + 'bo_new 00000100' = a 0-byte
  stack BO. nouveau_getparam returned ticks*1e6 for EVERY parameter, so GRAPH_UNITS was
  uptime-derived (TPs/MPsInTP = popcounts of it: wgltest's process got stack 0x30000 + tls
  0xc0000, HL's got MPsInTP = 0) and nv50_screen_create then divides vram_size by
  size_of_one_temp = 0 -> exception inside opengl32's first ICD call -> GL init fails ->
  GoldSrc reports "mode not supported". One process start in 16 hits it.

### M5.5 step 3 (build 3094): krefs, real getparam, a data-flow self-check
- nvshim.c nouveau_getparam: per-parameter answers; GRAPH_UNITS = PMC 0x1540 read through
  OP_NV_RD32 (0xF3010001 on the T61 = 1 TP x 2 MPs; the fake GPU answers the same, so
  simulated and real runs size stack/TLS alike), PCI 10DE:0429, FB_SIZE, CHIPSET_ID,
  VM_VRAM_BASE, HAS_BO_USAGE; unknown params return -1 and log.
- nvshim.c pushbuf: libdrm's kref semantics - validate/refn record the current bufctx's BOs
  in the pushbuf (dedup) and hold a reference until the kick; the kick flushes them (plus the
  bufctx current at kick time) and drops them after the synchronous submit; pushbuf_space
  re-krefs after a forced flush; a full list (512) kicks early. NVSHIM_NOKREF=1 reproduces
  3093.
- shim_kick_check on every submit, real and fake: M2MF linear sources (OFFSET_IN, the staged
  rectangle) and 3D VERTEX_ARRAY_START_LOW BOs must be live and carry no unflushed CPU
  writes -> 'KICK WARN ...' (first 32 logged), stats[5] in 'wgl: stats traps/warns'.
  tools/nvkick_decode.py --check --log now fails on KICK WARN / FAKE WEDGE / kick FAIL /
  GPU DEAD / bo_new FAIL / kick TRAP lines. pushbuf_data submits are tagged ' data' in
  C:\nvkick.txt and skipped by the decoder (index payloads are not methods).
- Proof under Wine (analysis/fake-runs/3094-*): NOKREF=1 -> 32 logged / 309 counted 'KICK
  WARN M2MF src has unflushed CPU writes' (the 256^2 level 0 at 444f0000 len 0x40000, every
  mip level, every 16th-frame TexSubImage patch), validator exit 2 = the 3093 T61 failure
  reproduced offline; default -> 0 warns, ~290 fps, heap flat at 7.3 MB, 64 kicks clean.
  tools/qemu98/wine_fake_run.sh gained SCENE= and NOKREF=.
- VxD/DRV byte-identical to 3093 bar the stamp.
Lessons for the simulate-first rule: (1) the fake GPU validates the STREAM; a data-flow bug
(what the engine reads) needs the BO-registry check, now built into every submit; (2) any
input the shim fabricates (getparam) must be fixed and identical in fake and real mode - an
uptime-dependent value let the fake runs pass by luck and the T61 fail by luck.
T61 order: (1) `wgltest driver mesa3d.dll window texture` in the nvgl box - the textured cube;
the log must show 'graph_units reg/used f3010001 f3010001' and 'stats traps/warns 0 0';
(2) Half-Life via nvhl.bat.

### 3094 on the T61 (2026-10-28): MILESTONE - Half-Life renders on the GPU; ~1 frame per 13 s
Both tests passed: the textured cube, and Half-Life (640x480x16 fullscreen, front = B5G6R5
surface 0 pitch 1280) rendering through Mesa's nv50 driver on the G86 under Windows 98 - the
goal of M5. But in-game it was a slide show: 'wgl: present frames/ms' = 1 frame per 13-17 s,
with ~1,163 kicks per frame (stats deltas), ~60k method dwords and ~1.2 MB of flushes per
frame, zero traps, zero warnings. analysis/t61-logs/3094-*. Level load allocated ~1,900
tiled textures (GoldSrc uploads each mip level with glTexImage2D, so Mesa reallocates the
miptree per level), 61 MB of the 64 MB heap.
Cause: upstream Mesa's nv50_draw_vbo ends EVERY draw with PUSH_KICK (added with the screen
state lock for multi-context use). With libdrm a kick is an asynchronous queue operation;
here every kick is a synchronous submit through the VxD that waits for the GPU to drain
(~1-10 ms each: ioctl, method writes through BAR1, IB wait, PGRAPH idle poll, two ring
lines, the shim's whole-BO dirty-diff of the 2 MB uploader). GoldSrc draws every world
surface with its own glBegin/glEnd, so ~1,160 draws = ~1,160 drains = 13 s.

### M5.5 step 4 (build 3095): game speed
- Mesa patch, nv50_vbo.c (18th file): no PUSH_KICK at the end of nv50_draw_vbo under
  WIN9X - draws accumulate until a flush/swap, a fence wait, a map of a pending BO or a
  full pushbuffer (the pre-lock behaviour; single-threaded, the lock is uncontended).
  Stress scene under the fake GPU: ~650 kicks/frame -> ~3.
- Shim: libdrm map/wait semantics - nouveau_bo_map/bo_wait submit only when THAT BO is
  kref'd by a pending pushbuf and access != 0 (3083's "libdrm kicks implicitly on map"
  was wrong: it kicks only the pushbuf holding the BO); nouveau_pushbuf_data copies the
  VB_ELEMENT index payload inline after its header (was two submits per indexed draw;
  header re-emitted after a forced flush when the data does not fit); RANGE-DIRECTED
  FLUSH - shim_kick_scan walks the stream before the flush and bounds what the engines
  read per BO (VERTEX_ARRAY_FETCH enable|stride + START_LOW, VERTEX_BUFFER_FIRST/COUNT per
  draw, LIMIT clip; CB_DEF_ADDRESS/SET; M2MF linear sources; indexed draws and 2D sources =
  whole BO; an unseen reference = whole BO) and shim_bo_flush diffs only that window; the
  3094 self-check is the verify pass of the same walk; second heap region [32 MiB+64 KiB,
  48 MiB) between the EVO reservation and the GPU channel (88 MiB in all); kref list 2048
  with O(1) dedup (push, gen stamp); BO registry sorted by phys + bisection (shim_bo_at was
  a linear scan over ~2,000 BOs per vertex array per draw); pushbuf cap 0x3C000 dwords
  (one submit must fit the VxD's 1 MiB region - Mesa's 512 KiB was read as dwords); kick
  dump capped at 8 MB; log policy: FlushFileBuffers for the first 512 lines, then 1 in 64
  and always for WARN/FAIL/DEAD/TRAP/WEDGE/kick/DETACH/stats lines (a per-line fsync cost
  ~1 ms = 34 ms/frame in the stress scene; FAT write-through on the T61 likewise), bo_new/
  bo_kind lines for the first 512 then 1 in 256.
- Stats: 'wgl: stats kicks mapwait/internal' (kicks the shim forced), 'submit ms' (time
  inside OP_NV_SUBMIT), 'kcyc validate/kick' and 'kcyc flush/bo' (shim CPU time, TSC
  kcycles), one 'kick #/ms/cnt' sample per 256 kicks.
- VxD: 'S> submit begin/end' only for the first 64 kicks and 1 in 256 (wedge/trap/fail
  always); a submit larger than the 1 MiB region is refused ('too large') instead of
  overrunning the IB ring. DRV unchanged bar the stamp.
- wgltest 'stress' scene (GoldSrc-shaped: 600 glBegin/glEnd quads over 72 textures, 32
  lightmap patches, 50 client-array glDrawElements per frame; wgltest_api.h gains
  glVertexPointer/glTexCoordPointer/glEnable/DisableClientState/glDrawElements).
- Fake GPU (analysis/fake-runs/3095-*): stress ~3 kicks/frame, ~70 ms/frame under Wine of
  which the shim is ~4 ms (kick 3.5, validate 0.1, flush 0.2; llvmpipe takes 110 ms for the
  same scene - the rest is Mesa's own per-draw cost), validator clean, 0 warnings; texture
  and triangle scenes clean.
T61: Half-Life via nvhl.bat (copy the new mesa3d.dll next to hl.exe again) - read 'present
frames/ms' vs 'submit ms' and the kcyc lines to see what remains: GPU/VxD wait, shim CPU or
Mesa CPU. Also `wgltest driver mesa3d.dll window stress` for a comparable number.
Known: Mesa's per-draw CPU cost (~100 us per textured glBegin/glEnd under Wine) is now the
likely ceiling; options later: the VxD writing methods with rep movsd / WC, ring-3 direct
BAR1 access for fences and flushes (Win9x shares the system arena), fewer state atoms.

### 3095 on the T61 (2026-10-28): Half-Life PLAYABLE - 30-68 fps at native resolution
Logs in analysis/t61-logs/3095-*: 4 minutes of play, 0 traps, 0 KICK WARN, validator clean,
~4 kicks/frame, GPU wait ('submit ms') 5-12 ms/frame, shim CPU 2-6 ms/frame, the rest Mesa +
game. Flushed 30-190 KB/frame. Heap: 0x1300 BOs, 64 MB used, largest free 13 MB. Released as
v3095 (policy: T61-confirmed). Remaining: occasional on-screen artifacts during play.

### M5.5 step 5 (build 3096): the artifacts - an oracle fix and two A/B switches
- Suspect 1, read from the walk: shim_kick_scan kept the M2MF state (PITCH_IN etc.) in locals
  reset per kick while the engine's persists; nv50's BEGIN_NV04 kicks between ANY two packets
  when the pushbuffer fills, so a transfer cut between PITCH_IN and LINE_COUNT/NOTIFY was
  bounded to one row by the next kick's walk. Simulated under Wine with NVSHIM_PUSHCAP=0x800
  (capped + jittered pushbuffer: ~16 cuts/frame; the jitter is needed because a fixed-length
  test frame cuts at the same packets forever) and NVSHIM_SCANRESET=1 (the 3095 walk in the
  mark pass only): the cut copies happen ('wgl: stats m2mf splits') but the verify pass finds
  NOTHING unflushed - the staging BO is kref'd (PUSH_VAL) before its packets, so the kick at
  the cut flushed it whole (no range marked = whole-BO diff) and the one-row range later was
  moot. So: not the cause. The walk state (vertex arrays, constbuf, M2MF, VERTEX_BUFFER_FIRST)
  now persists across kicks and the verify pass replays each kick from the state the mark pass
  started in (3095's verify pass started from the END state - a weaker oracle than it looked).
- Suspect 2, which no validator can see: TEARING. The present blit has no vertical-blank sync
  at all; at 30-68 fps on the 60 Hz panel every fast turn tears. NVSHIM_VSYNC=1 ('nvhl vsync'):
  wgl.c drains the draws (synchronous submit), nvshim_vsync_wait polls head 0's raster line
  (0x616340) through OP_NV_RD32 until it is in the blank window read from the EVO shadow regs
  (0x610aec vblanke / 0x610af4 vblanks / 0x610afc vtotal, nouveau nv50_disp scanoutpos; fallback
  vline >= active lines when they look implausible), then kicks the blit alone: a 1440x900x32
  copy (~1 ms) stays ahead of the beam (16 ms). 40 ms timeout, 'vsync wait timed out' log line,
  'wgl: stats vsync waits/ms'. The fake GPU answers the raster regs (a raster that advances 7
  lines per poll - Wine's tick counter is too coarse to drive it).
- Suspect 3, exonerate or convict the flush ranges on hardware: NVSHIM_FULLFLUSH=1 ('nvhl
  fullflush') = whole-BO dirty-diff at every kick (slower, 3094-style).
- wgltest stress: lightmap patches 64x16 (were 16x16, which fit one flush chunk).
- Fake GPU (analysis/fake-runs/3096-*): texture + stress + fullflush + vsync + the PUSHCAP/
  SCANRESET pair all clean; stress still ~3 kicks/frame.
T61: `nvhl` (artifacts as before?), `nvhl vsync` (gone = tearing), `nvhl fullflush` (still there
= not the flush ranges). Please describe/photograph them: horizontal seam in fast turns vs a
patch of wrong texture/lighting vs stray triangles. Logs as usual.

### 3096 on the T61 (2026-10-28): the artifacts are wrong texture patches, only at 1024x768
Neither switch changed anything (and 'nvhl vsync' did not cap the frame rate, so its wait
probably timed out - the 3097 log will show the raster samples). At 640x480 (scaled by the
display engine to the 1024x768 panel) the artifacts are gone. Wrong texture patches that
depend on the mode fit a corruption confined to the big mip levels: at 640x480 nearby walls
are sampled from level 1+, at 1024x768 from level 0. Audited and cleared: the VxD's VRAM map
vs both heap regions (no overlap), the submit path (reads BAR1 back before the put write),
the VRAM block-write chunking.

### M5.5 step 6 (build 3097): decide the texture corruption on hardware
- wgltest 'texcheck': 160 mipmapped textures (3/4 at 256^2, 1/4 at 128^2; odd ones GL_RGB =
  Mesa's 2D-engine conversion upload, even GL_RGBA = the M2MF upload; ~50 MB like a level)
  with a per-texture/level pattern, every level read back through glGetTexImage and compared
  byte by byte, then 13 of them drawn 1:1 (ortho, nearest) and the framebuffer compared (the
  sampler's view, alpha ignored: the window has none). A draw per texture like a loading
  screen (see below). llvmpipe: 1400 levels + 13 draws pass. Results: wgltest_texcheck.txt.
  Also at the game's mode: wgltest driver mesa3d.dll fullscreen width 1024 height 768 texcheck
- NVSHIM_VERIFYVRAM=1 ('nvhl verify'): after every synchronous submit the shim reads the
  ranges the walk marked back from VRAM (1 MiB/BO bound) and compares with the CPU shadow:
  'VRAM WARN bo/off/vram/shadow' + 'wgl: stats vram checks/mismatches'. Fake stress: 4030
  checks, 0 mismatches.
- NVSHIM_HEAPHI=1 ('nvhl heaphi'): the 3094 single-region heap, an A/B for the low region.
- nvhl.bat writes the clock PLL registers (0x4000-0x403c, 0xc040 MAST, 0x22000 crystal) to
  C:\nvclk.txt - to be decoded with nouveau's nv50 clk code; the GPU runs at whatever the
  VBIOS POSTed (mobile G86s boot at the lowest perf level), which would explain the 5-12 ms
  GPU time per frame too.
- Found by the fake texcheck (no draws between 1280 uploads): the 88 MiB heap ran dry.
  Two causes. (1) Mesa's state tracker allocates only a level-0 resource when level 0 is
  uploaded under the default GL_NEAREST_MIPMAP_LINEAR filter (allocate_full_mipmap calls
  it rare), allocates the chain at level 1 and keeps the level-0 copy until the texture's
  first use - 64 KiB-rounded tiled BOs, one per texture; a game's loading screen draws in
  between, so it never shows there. (2) nouveau_fence_work holds no reference on the fence,
  so _nouveau_fence_next never emitted a fence that only carried staging-buffer releases;
  they waited for the 64-work kick threshold = 27 MB of staging per fence at 256^2. Fixes:
  Mesa patch nouveau_fence.c (19th file) emits a fence with pending work at every kick
  under WIN9X; nouveau_bo_new submits pending work (twice - the QUERY lands in the next
  pushbuffer) and retries before failing; a failed allocation dumps who holds the heap
  (live BOs by size/kind, kref'd, mapped - how (1) was found). Fake texcheck now: heap
  steady at 57 MB, 0 warnings.
- Vsync wait: also accepts the raster counter wrapping (a new frame) and logs the first
  waits' polls/min/max line samples - the 3096 run's 'no difference' needs those.
T61: (1) wgltest driver mesa3d.dll window texcheck and the same with fullscreen width 1024
height 768; send wgltest_texcheck.txt. (2) nvhl verify (play until artifacts show, quit;
send the logs + C:\nvclk.txt). (3) nvhl heaphi. (4) nvhl vsync once more for the samples.

### 3097 on the T61 (2026-10-28): the launcher was broken, the "good" run was software
nvhl.bat reported an invalid command (it had since 3096: the Python edits wrote LF line
endings, COMMAND.COM needs CRLF), so the user started hl.exe directly - without
GALLIUM_DRIVER=nouveau that is the software renderer: textures correct, frame rate low even
at 640x480. So: the 3096 switches never ran, and the 3097 result says nothing about the GPU.

### M5.5 step 7 (build 3098): CRLF, and a complete submit drain
- nvhl.bat and INSTALL.txt rewritten with CRLF (make_dist does not touch them; the masters are
  edited on Linux - keep them CRLF).
- VxD NV_gpu_submit: after IBGET==put it now also waits for the pusher's data fetch
  (CACHE1 DMA_GET 0x3244 == DMA_PUT 0x3240) and the method cache (CACHE1 GET 0x3270 == PUT
  0x3210) before the PGRAPH idle wait. IBGET==put only means the IB entries were taken;
  PGRAPH_STATUS can read idle between two methods while the puller still holds this kick's
  methods, so the "synchronous" submit could return early and ring 3 then overwrote reused
  staging VRAM (the next lightmap/texture patch) under the engines: timing-bound and worse
  with bigger frames, which fits the 1024x768-only wrong texture patches. Bounded (400k
  polls), latched off after one timeout, 'S> drain loops' for the first 16 kicks in nvtail.
T61 order: (1) plain `nvhl` from a DOS box - are the patches gone? (2) `wgltest driver
mesa3d.dll window texcheck` with GALLIUM_DRIVER=nouveau set in that DOS box (nvgl.bat sets
it), and the 1024x768 fullscreen variant; send wgltest_texcheck.txt. (3) if patches remain:
`nvhl verify`, `nvhl heaphi`. Always: C:\nvshim.log, C:\nvtail.txt (the drain lines),
C:\nvkick.txt, C:\nvclk.txt.

### 3097 package on the T61 with `nvhl heaphi` (2026-10-28; analysis/t61-logs/3097-heaphi-*)
- The 3098 VxD was NOT installed: nvtail says "nvmini.drv build 3096", has no 'S> drain' lines,
  nvclk.txt says nvprobe 3097. The run = 3097 shim + Mesa on the 3096 VxD; the drain fix is
  still untested. (Lesson: logs must name the VxD build - done in 3099.)
- Textures fine at 1024x768 for 3 minutes; HUD (health, flashlight) wrong. Cause: the 64 MiB
  heaphi heap is exhausted 3.3 s in (620 bo_new FAILs of 0x10000/0x40000, largest free
  0x30000); Half-Life holds ~65 MiB of RGBA8 textures (3095 run: 65 MiB used of 88), so the
  sprites loaded last get no VRAM. heaphi is a diagnostic only. Its clean-texture result is
  an A/B hint that the low region [0x02010000,0x03000000) matters, not a conclusion: the VxD
  touches nothing there (VRAM rd/wr go through the 256 MiB BAR1 map, the VxD heap is at
  0x03500000, the 1:1 VM covers all 128 MiB), and one 3-minute run of an occasional artifact
  proves little.
- 800x600 "does not work": nvtail shows the mode set OK (shadow pitch 1600, scanout pitch 1792,
  scaled to the panel) and a GL session with 'front surface/pitch 0 0x640': wgl.c imports the
  scanout with hda->pitch = the GDI shadow pitch, but the scanout is 256-byte aligned
  (NV_hw_pitch). 640x480 (1280) and 1024x768 (2048) are multiples of 256, 800x600 is not, so
  GL presented a sheared picture. The VxD's HB blit counter stays flat during every GL
  session (GL writes the scanout directly), so that was not it.
- Clock registers (nvclk.txt): 0x4008/0x400c 80016400/00001c05, 0x4020/0x4024
  80010000/00000b01, 0x4028/0x402c a0020000/00001602, 0x4030/0x4034 80010000/00001202,
  0xc040 2ee01b33. Decode pending (nouveau nvkm/subdev/clk/nv50.c); the small N/M values look
  like VBIOS boot clocks, which would explain a lot of the frame-rate ceiling.

### M5.5 step 8 (build 3099): 800x600 pitch, self-identifying logs
- vxd_vesa.c native mode set: at 16/32 bpp hda->pitch = NV_hw_pitch(w, bpp), the scanout
  pitch. GDI's shadow (wram, 64 MiB) takes the same pitch, the blitter copies row by row,
  DirectDraw and the GL front import read hda->pitch - one pitch everywhere. 8 bpp keeps the
  packed shadow (the blitter expands it to 32 bpp).
- nvshim.c: 'vxd build / shim build' logged at device_new (OP_NV_PROBE .build); nvhl.bat
  appends the full nvprobe output (VxD build) to C:\nvclk.txt.
- T61 order: copy nvmini.vxd to C:\WINDOWS\SYSTEM + reboot, check 'vxd build 3099' in
  nvshim.log; plain `nvhl` at 1024x768 (patches gone? 'S> drain loops' in nvtail);
  `nvhl -width 800 -height 600`; texcheck with GALLIUM_DRIVER=nouveau set; `nvhl heaphi` is
  not useful any more.

### 3099 on the T61 (2026-10-28 night; analysis/t61-logs/3099-*): the artifacts are CORNER HOLES
- The 3099 VxD ran ('vxd build / shim build 00000c1b 00000c1b'); the 3098 submit drain ran
  ('S> drain loops 16' once, then 0) and changed nothing. 800x600 fixed. texcheck PASSED on the
  hardware (160 textures, 1400 levels read back, 13 draws, 0 bad) - texture data is fine.
  One Half-Life process: 1024x768 for 80 s (artifacts), then the in-game switch to 800x600
  (clean; textures re-uploaded by vid_restart). Heap never full, VxD blitter idle in-game,
  validator clean on the 64 dumped kicks, no traps.
- What the artifacts ARE (user's crops): small rectangular blocks of garbage at the screen
  corners (bottom-left/upper-right, also upper-left/bottom-right), on the world, not the HUD.
  Reading: Half-Life never clears the colour buffer (gl_clear 0), so a block = pixels that
  no triangle covered this frame, showing stale VRAM. Opposite corners = the tips of the two
  triangles of a screen-filling quad (sky faces, near walls, floors - vertices far off screen
  or behind the eye), and only at the largest viewport = the rasterizer's coordinate range,
  i.e. clipping/guard band, not textures. Mesa builds nv50 with NV50_SCISSORS_CLIPPING
  (VIEW_VOLUME_CLIP_CTRL=0: x/y clipping left to the hardware guard band).
- Where we differ from Linux there: the PGRAPH context the VxD loads (nv50_grctx_g86.h) came
  from nouveau v2.6.36 (2010). Ran that generator and the CURRENT ctxnv50.c on the host with
  a register-mapping hook (analysis/grctx_host): same 502 MMIO registers and 94528-dword
  layout, but for the G86: 0x40821c/0x40829c = 0x018c0000 now (was 0x008c0000, chip-specific),
  the clipping strand's FP_CTRL_UNK196C/UNK1968 defaults (the 3 sat one slot late: Mesa
  rewrites 0x196c, never 0x1968), the 2D IFC_CLIP_W/H defaults, and a reordered 114-dword
  ctxprog. Current nv50_gr_init also zeroes the 8 zcull region descriptors and the ctxctl
  scratch after the microcode upload; the VxD did neither.
- Checks that would decide it without a build: in Half-Life 'gl_clear 1' (blocks become the
  clear colour if they are uncovered pixels); wgltest 'sky' (below) fullscreen 1024x768x16
  vs 800x600x16.

### M5.5 step 9 (build 3100): current-nouveau PGRAPH context, zcull init, wgltest 'sky'
- nv50_grctx_g86.h regenerated from current nouveau (analysis/grctx_host/fetch_build_run.sh;
  the harness reproduces the old header from the old source bit for bit, so the diff above
  is the whole difference). gpu_pgraph_init: after the microcode upload clear 0x400828/
  0x40082c/0x400830/0x40032c/0x400330 and zero 0x402c20..0x402c9c (zcull regions); 0x400824
  keeps 0x4000 (our manual context load was proven with it).
- wgltest 'sky' scene (wgltest.c, 'bpp <n>' option for fullscreen): GoldSrc-style skybox (6
  huge quads around the eye, depth off, UL-BR split), floor, near wall, yaw + pitch, cleared
  to MAGENTA every frame: magenta blocks = uncovered pixels, non-checker = wrong sampling.
  llvmpipe under Wine renders it clean (scratchpad screenshots); fake-GPU validator clean.
  dist/driver/nvsky.bat: `nvsky [w h [bpp]]` = fullscreen 1024x768x16 by default.
- T61 order: copy nvmini.vxd to C:\WINDOWS\SYSTEM + reboot (the VxD build shows in
  nvshim.log as 00000c1c); `nvsky` (corner blocks? magenta?), `nvsky 800 600`; `nvhl` at
  1024x768 (holes gone?); if not, in the HL console `gl_clear 1`. Logs as usual + photos.
- If 3100 does not fix it: the next candidates are the PGRAPH init registers nouveau writes
  that we never touch (compare nv50_gr_init / nv50_gr_init_tpc once more), then an A/B of
  VIEW_VOLUME_CLIP_CTRL=0x1080 (geometric x/y clipping, Mesa's non-scissor mode) through a
  shim-side method override.

### 3100 on the T61 (2026-10-28 late): MUCH WORSE - garbage screen; 3101 = three contexts, registry-selected
- User: after the reboot `nvsky` (1024x768x16) was clean, `nvsky 800 600` garbage, Half-Life
  garbage. Logs (nvshim.log = the HL session): VxD 3100 confirmed (shim still 3099 = old DLL
  next to hl.exe, harmless); no traps, no failed kicks; session 2 (nvsky 800x600) ran at the
  same 57 fps as the clean session 1, so its garbage is not a slow/wedged GPU; Half-Life ran
  at 4-6 fps at 1024x768 and ~38 fps at 640x480 (3099: 55 fps), submit 3.3 ms/kick (was 1.5).
  No photo yet. Not decidable from the logs whether the current ctxprog/flow (our 2.6.36-style
  manual USER_LOAD against the current ctxprog) or one of the three changed values is at fault.
- User's direction: stop hand-rolling - make the GPU bring-up work "the same way" as the
  reference (nouveau), using everything available, instead of reinventing it.
- 3101: all three generations in one VxD (nv50_grctx_g86.h = current, nv50_grctx_g86_2636.h =
  2.6.36 + the 6-slot fix list), chosen by HKLM\Software\vmdisp9x\vesa\GrCtx read in
  gpu_pgraph_init (= every channel creation, no reboot): 0 = 2.6.36 = 3099 (DEFAULT), 1 =
  2.6.36 layout/ctxprog + current nouveau's three G86 values, 2 = current + zcull init = 3100.
  dist/driver/grctx0/1/2.reg set it; the VxD log says 'grctx variant N'. One reboot to
  install, then A/B without reboots: grctx1 nvsky / nvsky 800 600 / nvhl, then grctx2.
  Outcome table to fill: variant 1 clean + holes gone => the values were the fix; variant 1
  garbage => one of the three values is wrong for this board (split them next); variant 1
  clean but holes remain => the context is not the hole cause.
- The faithful route after that (the user's point): port current nvkm nv50 gr/fifo channel
  bring-up as a unit - nv50_gr_init register sequence, ctxprog + ctxvals, PFIFO-driven
  context switch (engine-context slot at inst+0x20, no manual USER_LOAD poke), nv50_fifo
  RAMFC/playlist - rather than the 2.6.36 manual load with a current ctxprog.

### M5.5 step 11 (build 3102): the corner holes checked against NVIDIA's XP driver (nv4_mini.sys)
- User's ask 2026-10-29: 640x480/800x600 clean, 1024x768 shows small blocks in the screen
  corners - look at what the XP driver does differently and fix the 98 one. The XP miniport
  (analysis/xp_extracted/nv4_mini.sys, objdump in analysis/reverse/XP_nv4_mini.asm, VA =
  file offset + 0x10000) is the authoritative G86 reference: it ran this exact chip.
- How the XP G86 PGRAPH data was found: chip objects are registered by constructors that
  store a predicate and a method table ({engine<<24|id, fn} pairs); the G86 one is 0xe0280
  (predicate 0xe0230 = BOOT0 0x086xxxxx minus rev A1, table 0x44ba18 with 0x50c pairs, class
  list 0x449178 naming 8297/5097/5039/502d/50c0). Engine 6 = PGRAPH, 61 methods, three of
  them G86-specific: 0x2f = 0x2506c0 (unit init), 0x30 = 0x250ae0 (installs the context
  data after a 0x400800 signature check: 0x5049e8ac or 0x7ee17cc6), 0x40 = 0x250d10.
  0x250ae0 points at: ctxprog = 224 dwords at 0x47a2b8; context size 0x5bca0; 723 {byte
  offset, value} golden context pairs at 0x3aeaf0 (written by context create, 0x2a1780,
  into a zeroed buffer); 512 {register, grctx offset, init value} triples at 0x3b0188 (the
  MMIO list; engine init 0x2a8b40 = method 0x1f writes every register, TPC ones gated by
  the TPC mask); 230 {method>>2, value} pairs at 0x47a638 that the same engine init feeds
  to PGRAPH through ICMD (0x400200/0x400204) = the 3D class defaults. Context switches:
  first bind 0x2a8760 (0x400824 |= 0x800, CTXCTL_CUR written directly), later ones
  0x2a38c0 (0x400824 |= 0x20 save / 0x40 load, then 0x400304 |= 1) - the same flag bits
  nouveau 2.6.33 used when it loaded these blob ctxprogs as firmware, i.e. our manual load.
  analysis/xp_grctx/extract_xp_g86.py regenerates nv50_grctx_g86_xp.h and the listing
  analysis/xp_grctx/g86_xp_context.txt (ctxprog disassembly, MMIO list, golden values,
  ICMD methods decoded with Mesa's nv50_3d.xml.h).
- What differs from nouveau (ctxprog disassembled with the CP_* opcode table of ctxnv50.c):
  same 502+10 MMIO registers in the same order (the blob also lists the second ROP
  partition's 0x4071xx, which this 1-ROP G86 does not have; XP writes them anyway), MMIO
  part at grctx byte 0x100 with 0x400828 at 0x11c (nouveau: dword 1), xfer areas 0x100
  bytes later than nouveau's (0xa00 / 0x52d00, same strand size 0x2916 and 0x442), the TPC
  registers context-switched only when 0x400830 bit 0 is set, and the manual-load path
  with CMD 0x11 / NEWCTXDMA that our load triggers. The golden xfer values are nouveau's
  (current and 2.6.36) plus the second ROP partition's strand-3 values and nothing else;
  the MMIO init values differ in exactly three registers: 0x401c00 TRAST (XP 0x042500df,
  nouveau 0x044d00df - ctxnv50.c groups the G86 with G84/G94, the blob gives it the
  G92/G96/G98 value), 0x405000 CCACHE (0x000e0080 vs 0x00000080 - again the G84 value) and
  0x400844 (0 vs 2). The per-TP 0x40821c/0x40829c = 0x018c0000 of current nouveau is what
  XP writes too (variant 1 was right about that). envytools names the 0xf0c242 family XP
  also writes at init as the units' clock-gating registers; TPC 0x408000 = 0x30 is unnamed.
- 3102: GrCtx = 3 (the XP way: XP ctxprog, XP golden context, the 512 MMIO registers
  written at init, XP unit init, 0x400828 |= 1; 0x400824 keeps 0x4000 for our manual load)
  and GrCtx = 4 (variant 0 = 3099 + the three XP values at their 2.6.36-layout slots: dwords
  171/189/75). grctx3.reg/grctx4.reg; the ICMD methods are not replayed (the golden
  context carries that state). Validation here: VxD builds, LE header parses, both
  tables in the binary; the fake GPU cannot judge rasterizer config, so the T61 decides.
- T61 order: install (reboot once), grctx4 nvsky / nvsky 800 600 / nvhl (holes gone =>
  the TRAST/CCACHE values are the fix, then make 4 the default), then grctx3 the same way
  (garbage or hang possible: first run of the blob microcode under our manual load; note
  the 'grctx xp: 0x400800=' line). If neither helps: an A/B of VIEW_VOLUME_CLIP_CTRL
  (Mesa's NV50_SCISSORS_CLIPPING = 0 vs 0x1080, a shim-side method override), and the
  XP ICMD defaults for methods Mesa never writes (list in g86_xp_context.txt).

### 3102 on the T61 -> M5.5 step 12 (build 3103): one channel per GL process
- User: grctx3 and grctx4 "none worked" - nvsky 1024x768 fine, nvsky 800 600 garbage,
  Half-Life garbage. Logs (nvtail.txt 643 KB, nvshim.log = the HL session, VxD 3102):
  no 'grctx variant' line at all, object instances at 0x0300A300 and 0x0300AF00 (the
  OBJAREA bump had already served 5 processes), Half-Life at 9 fps, no traps. The channel
  is created ONCE per boot (g_chan_ready) and every later GL process reuses it, so (1) the
  GrCtx value was read only at that first creation - no variant of 3101/3102 was ever
  switched without a reboot, contrary to the 3101 notes - and (2) every log we have fits
  "only the first GL process after a boot renders correctly": 3099's Half-Life was the
  first process (texcheck separate), 3100's "first nvsky clean, then garbage" and today's
  runs are the same pattern. 3100 was probably never "much worse" either; the resolution
  was a red herring (800x600 in-game in 3099's single process was clean).
- 3103: NV_gpu_bind sees a handle already in the RAMHT (Mesa binds 0xbeef0201 first in
  every process) -> waits for PGRAPH idle, PFIFO caches off, rebuilds the channel from cold
  (nv_chan_init: PGRAPH reset + ctxprog + grctx with the variant re-read, RAMIN/RAMHT/
  RAMFC, PFIFO, playlist), resets the pushbuffer/IB indices, object area, wedge/drain/log
  latches. Log line 'bound again -> new GL process, rebuilding the channel'.
- T61 order: several programs per boot - grctx0: nvsky, nvsky 800 600, nvhl (all clean
  except the holes = the per-process channel is the fix for the "garbage" runs); then
  grctx4 (holes?); then grctx3 (XP microcode; 'grctx xp: 0x400800=' line).

### 3103 on the T61 -> build 3104: the "bound again" test must not read the RAMHT
- User: "much worse": nvsky = gray window, Half-Life = invalid page fault in MESA3D.DLL
  (cmp byte [ebx+0x6ec] with ebx = 0). Logs (analysis/t61-logs/3103-*): the FIRST GL
  process of the boot binds 0201, 0202, 0301, 5039 and then, at its own first 0xbeef502d,
  the VxD prints 'handle BEEF502D bound again -> new GL process, rebuilding the channel':
  nv_chan_init pre-seeds the RAMHT with the VxD's own 0xbeef502d (the blitter's 2D object
  for nvprobe gpu2d / the DRV), so the RAMHT lookup matched a handle no GL process had
  bound. The rebuild restarted the object area: 502D landed at 0x03008000 (over the vram
  ctxdma 0201), 5097 over the gart ctxdma 0202, 50C0 over the notifier 0301. Kick 0 (M2MF
  init) still passed, kick 1 (the first NV84_3D stream) hung: 'kick FAIL pfi/pgi/wedged
  0 0 1', 400001 drain loops. Everything after follows from the wedge: nothing drawn (gray
  window), no fence ever retired, 'bo_new FAIL heap full' x30 in Half-Life, then Mesa
  dereferenced the NULL bo.
- 3104: g_gl_handles[32] records the handles bound through NV_gpu_bind since the channel
  was (re)built; a handle in that table = new process. The RAMHT is no longer consulted.
  Expected log: 'bound again' only right after a program's first bind (BEEF0201), never
  mid-process. Same T61 order as 3103.

### 3104 on the T61 + independent review -> build 3105: write-only maps assumed zeroed VRAM
- User: with 3104 and grctx0, nvsky 800x600 is FIXED (the per-process channel works), but
  Half-Life still shows the corner squares. So: 3103's finding (channel reuse) explained the
  "garbage" runs, NOT the 1024x768 squares; the 3099 in-process observation (squares at
  1024x768, clean after switching to 800x600 in the same process) stands, and "800x600 is a
  red herring" in the step-12 notes was too strong.
- Independent review (analysis/OPENGL_ARTIFACT_REVIEW_2026-10-29.md, reproducer
  tools/repro_nvshim_zero_upload.py, which compiles the tree's own nouveau_bo_map +
  shim_bo_flush against a byte-array VRAM): a write-only nouveau_bo_map callocs both the
  shadow and `clean`, i.e. it tells the dirty-diff flush that VRAM already holds zeros - but
  heap_free never clears VRAM, so a fresh BO inherits the previous BO's bytes, and a 1 KiB
  chunk the CPU fills entirely with zeros compares equal to `clean` and is never uploaded.
  FULLFLUSH widens the window but keeps the comparison, so it never caught this either.
  Repro before the fix: 2048/3072 stale bytes; after: 0.
- 3105 (shim only; nvmini.vxd = 3104 code restamped): `clean` of a write-only first map is
  a sentinel pattern (0xA5C3F00F ^ golden-ratio*i) = "VRAM unknown", so the first flush of
  each chunk in a kick's window is unconditional and later flushes diff as before. Read
  maps and refreshes still seed `clean` from VRAM. Cost: nothing for staging uploads (the
  CPU writes all of them anyway); untouched chunks inside a first window get zeros once.
  Chosen over "zero VRAM at first map" (would double the traffic of every lightmap staging
  upload) and over per-chunk write tracking (more state). mesa3d.dll relinked
  (impcheck: missing on Win98 none), nvprobe rebuilt.
- Relevance to the squares: UNPROVEN. Candidates where a fully-zero 1 KiB chunk occurs in
  HL: black texture rows / lightmap patches (staging BOs are fresh per transfer = exactly
  the reuse pattern), unused constant-buffer slots. That would give garbage blocks on
  textured surfaces, not obviously screen corners.
- T61 next, grctx0, one Half-Life run at 1024x768: (1) 3105 still shows the squares?
  (2) `gl_clear 1`: squares turn into the clear colour (uncovered pixels -> clipping /
  guard band / zcull) or stay garbage (texture or present path); (3) `set
  NVSHIM_CPU_PRESENT=1` + nvhl: squares gone -> the 2D present blit; still there ->
  rendering. Photos. Only then touch VIEW_VOLUME_CLIP_CTRL / zcull / context values.

### 3105 on the T61 -> THE BLOCKS ARE THE GPU PRESENT BLIT -> build 3106: present A/B switches
- User: `gl_clear 1` changed nothing; `set NVSHIM_CPU_PRESENT=1` + nvhl = slow but the
  corner blocks are GONE. So the rendered frame (tiled nv50 window buffer) is correct -
  the M2MF de-tile readback + CPU copy shows it clean - and the blocks come from the GPU
  present: nv50_blit -> nv50_blit_eng2d, the 2D engine (0x502d BLIT, point sample,
  ORIGIN_CENTER, 1:1) copying the tiled B8G8R8A8 window buffer into the linear scanout
  surface (16 bpp -> R5G6B5 conversion on the way; 3099 log: front 1024x768x16 pitch 2048).
  Clipping / guard band / zcull / the PGRAPH context are OFF the table for this one; so is
  the 3105 upload fix (not even loaded: the 3105 nvshim.log still says shim build 0c1b =
  the 3099 mesa3d.dll next to hl.exe; the CPU-present result stands regardless).
- The 3099 note "on the world, not the HUD" fits a STALE READ in the 2D path: previous-
  frame data is invisible where the picture does not change (HUD) and garbage where it
  does (world). Candidates: (a) the 3D engine's render-target writes (ROP) not visible to
  the 2D engine's reads within the same channel without a GRAPH SERIALIZE / cache flush
  (Mesa's eng2d path emits none; under X the DDX blit lives in another channel, which
  forces a full PGRAPH context switch); (b) a 2D-side read cache/TLB holding the previous
  frame's first/last tiles (corners = extreme addresses of the 3 MiB tiled surface, 768 =
  exactly 12 tile rows). Resolution dependence unexplained either way.
- 3106 (mesa3d.dll only, VxD = 3104 code restamped): wgl.c present gets
  NVSHIM_PRESENT_BARRIER=1 (ctx->texture_barrier = SUBC_3D GRAPH_SERIALIZE + TEX_CACHE_CTL
  0x20 right before the blit) and NVSHIM_PRESENT_3D=1 (blit.mask = RGB -> nv50_blit takes
  nv50_blit_3d: the frame is drawn into the scanout surface as a textured quad, a
  different read path: TEX units instead of the 2D engine). nvhl.bat flags `barrier`,
  `present3d`, `cpupresent` (+ the existing `vsync`). Log line 'wgl: present barrier/3d'.
- T61 next (grctx0, 1024x768, same spot): nvhl barrier / nvhl present3d / nvhl vsync ->
  blocks yes/no + fps each. barrier fixes it -> add the barrier to the present for good
  (nouveau does the equivalent through the channel switch). present3d fixes it but barrier
  does not -> the 2D engine's read path (tiled source) is the problem: compare the 2D
  SRC setup (BLOCK_DIMENSIONS/tile mode, width/height) and the PTE kind of the window
  buffer against what the 3D engine was given. Neither -> the scanout side (front BO
  'external', pitch, the VxD's own blitter/cursor) and the vsync timing.

### Build 3107: nvhl.bat reviewed (user request)
- nvprobe tail never exited: every `nvhl` run started one with `start /min` and the next
  run deleted/re-created nvtail.txt under it, so two tails appended to the same file.
  `nvprobe tail <file> <runfile>` now exits once the run file is gone (checked every
  0.5 s); nvhl.bat creates C:\nvtail.run before the tail and deletes it after hl.exe.
- The 3105 INSTALL text said `set NVSHIM_CPU_PRESENT=1` then `nvhl`, but the 3106 batch
  clears every NVSHIM_* at the top (so a flag from one run never leaks into the next, since
  command.com keeps a batch's SETs): the instruction became `nvhl cpupresent`.
- `heaphi` removed (3099: starves the HUD sprites, proves nothing); `grctx N` added
  (regedit /s %NVDRV%\grctxN.reg before the launch - valid since 3104 reads the value per
  GL start); `kickdump N`; hl.exe gets %1..%9 instead of %1..%6; a warning when
  GALLIUM_DRIVER does not expand (DOS box out of environment space = the switches
  silently did not apply); a warning when nvprobe.exe is missing at NVDRV.

### 3105 revisited by the user -> build 3108: the holes depend on WHERE the buffers sit in VRAM
- User corrections (still 3105): `gl_clear 1` DOES paint the blocks red (GoldSrc's clear
  colour) - so they ARE pixels no fragment reached (a rendering hole; the "present blit"
  reading of the CPU-present run was wrong - that run most likely just changed the heap
  layout). The blocks are also on the console/main menu. Two blocks, one in the upper
  half and one in the lower half, mutually exclusive, around the screen's half height.
  And: `nvhl heaphi` (every BO in the 64 MiB+ region) REMOVES them. (The user mentions
  green-highlighted photos; none received yet.)
- Default layout: the window buffer (3 MiB, 0x02260000) and its depth buffer (3 MiB,
  0x02560000) are the first allocations and land in the LOW heap region 32-48 MiB
  (NVSHIM_HEAP_LO 0x02010000 .. 0x03000000); textures spill to 64 MiB+. heaphi moves
  the two buffers to 64 MiB+ -> clean. So: an address-dependent fault in the 32-48 MiB
  range for RT/depth traffic (textures are fine there: texcheck).
- Found in the VxD: the channel's PDE 2 was written as VRAMPT | 0x61 in both init paths.
  nouveau nv50_vm_map_pgt: bits 5-6 = page-table size, 0x60 = "up to 32 MiB of 64 KiB
  pages", 0x40 = 64 MiB, 0x20 = 128 MiB, 0 = the full 512 MiB table. Our table is the
  full 64 KiB (8192 entries; the 1:1 window fills 2048 = 128 MiB), so the flag must be 0.
  0x60 dates from M5.2 (4 scratch pages). With it the MMU was told the table ends at
  32 MiB - exactly where the low heap region starts. Why most traffic still worked and
  only RT/depth in 32-48 MiB broke is not known (TLB/prefetch behaviour of the size
  field); the fix is nouveau's value either way.
- 3108: PDE 2 = VRAMPT | 0x01 (both paths). Shim: NVSHIM_HIKIND=depth|color|tiled
  (nvhl hidepth / hicolor / hitiled) prefers the 64 MiB+ region for that storage class
  only (kinds < 0x70 = depth/stencil, >= 0x70 = colour), falling back to any region -
  the probe that names the buffer if the PDE fix alone is not it. heaphi is back as a
  documented diagnostic switch.
- T61: nvmini.vxd + reboot; plain `nvhl` at 1024x768 (holes gone?); if not, `nvhl hidepth`,
  `nvhl hicolor`. Photos of the blocks (before/after gl_clear 1) still wanted.
- Photos received (analysis/t61-logs/3105-photos-*): with gl_clear 1, two RED blocks at the
  top-left corner, each ~16 px wide x ~64 px tall = ONE nv50 memory tile of a 32-bpp
  surface (64 B x 64 rows); with gl_clear 0 the same places show stale content and a block
  at the bottom-right corner; the two corners alternate. On the main menu a thin
  rainbow-striped band across the 'Console' line. One-tile blocks at the far corners,
  alternating with the view = the far tips of the two triangles of a screen-filling quad,
  per-tile coverage dropped by the rasterizer/clipper - the 3099 reading, which the
  invalid 3102/3103 runs had buried.
- Mesa sets ZCULL_REGION = 0x3f (zcull unused) -> zcull is NOT it. Mesa builds nv50 with
  NV50_SCISSORS_CLIPPING: VIEW_VOLUME_CLIP_CTRL = 0 at init and from the rasterizer state
  (no UNK7/UNK12 bits) - x/y clipping is left to the hardware guard band.
- The one clipping-related divergence from nouveau we KNOW of: the 2.6.36 context (variant
  0, the default since 3099) has the clipping strand's defaults shifted - current ctxnv50.c
  emits FP_CTRL_UNK196C = 3 then UNK1968 = 0; the old generator put the 3 one slot later,
  so our default context runs with tesla UNK1968 = 3, and Mesa never writes 0x1968 (it does
  rewrite 0x196c). Variant 1 (2.6.36 layout + the three current values) and variant 2
  (current nouveau) carry the fix, but both were only ever run under the channel-reuse bug
  (3101/3102), i.e. never validly. On 3104+ `nvhl grctx 1` / `nvhl grctx 2` are one-command
  tests. If variant 1 cures it: make the corrected clipping defaults the default.
- T61 order on 3108 (vxd + reboot), 1024x768, same spot: `nvhl` (PDE fix alone) -> `nvhl
  grctx 1` -> `nvhl grctx 2` -> `nvhl hidepth` / `nvhl hicolor`. Blocks yes/no each.

### 3108 on the T61 -> ROOT CAUSE -> build 3109: nouveau's row-block rule for storage kinds
- User: plain 3108 = blocks; grctx 1 = blocks; grctx 2 = blocks; `nvhl hidepth` = CLEAN.
  So neither the PDE flag nor the context: the DEPTH BUFFER'S ADDRESS decides.
- nouveau (nvkm/subdev/mmu/nv50.c, nv50_mmu_kind): every storage kind is classed 0x01 "no
  bank swizzle" or 0x02 "bank swizzled" (0x7f invalid) - "values understood by the VRAM
  allocator, and required to avoid mixing the two types within a certain range". All
  depth/stencil kinds are 0x02 (Z24S8 0x28, X8Z24 0x18, Z16 0x6c), linear 0x00 and the
  render-target/texture colour kind 0x70 are 0x01 (the scanout kind 0x7a is 0x02).
  nvkm_mm_head (core/mm.c) rounds an allocation's start UP and its end DOWN to
  mm->block_size whenever the neighbouring node has another type; for VRAM that block is
  nv50_fb_vram_rblock (fb/ramnv50.c): rowsize = partitions(0x1540 bits 16-23, G86: 1) x
  banks (0x100204 bits 24-25 -> 8) x columns (bits 12-15 -> 512) x 8 = 32 KiB, x3 when
  0x100250 bit 0 is set (96 KiB). The memory controller bank-swizzles the 0x02 kinds, so
  within one row block a swizzled surface and an unswizzled one occupy the same DRAM
  cells: the shim's allocator (64 KiB-aligned tiled BOs back to back, linear BOs at 256 B)
  put the 0x28 depth buffer (0x02560000) right after the 0x70 colour buffer and right
  before a linear BO -> its first and last tiles were overwritten -> depth test fails in
  those tiles -> one untouched tile top-left + one bottom-right. Matches every fact: the
  gl_clear red (the colour clear wrote them, the geometry failed the depth test), the
  alternation (which neighbour wrote last), the resolution (800x600 buffers land on other
  boundaries), heaphi/hidepth (an isolated depth buffer), texcheck fine (no depth), nvsky
  1024x768 clean. 3108's PDE flag fix stays (it is nouveau's value).
- 3109 (shim): nv50_kind_class[] = nouveau's table; heap_init reads 0x100200/0x100204/
  0x100250/0x1540 through OP_NV_RD32 and computes the row block like nouveau (fake GPU or
  read failure: 96 KiB); heap_alloc_in applies nvkm_mm_head's rounding against the classes
  of the live BOs ending at / starting at the free block's edges (no BO = class 1, so the
  EVO area and the VxD's channel structures count as linear). NVSHIM_RBLOCK=<bytes>
  overrides, =0 disables (`nvhl norblock` for the A/B). Log: 'heap: memcfg ...' and
  'heap: parts(1540)/rblock'. Lesson: the user directive was right - nouveau's allocator
  carries hardware rules that are invisible until a buffer lands on the wrong boundary.
- T61: mesa3d.dll (next to hl.exe) + nvhl.bat, no reboot: `nvhl` at 1024x768 -> holes
  gone? `nvhl norblock` -> holes back? Send nvshim.log (the rblock line).
- **3109 CONFIRMED on the T61 (2026-10-29 night): plain nvhl clean at 1024x768, `nvhl
  norblock` brings the holes back. Released as v3109.**

### Build 3110: no batch file - GPU OpenGL by registry default, one owner per channel
- User: "a version that I don't need to execute games using .bat files, that loads mesa3d
  with opengl support when it's needed". What nvhl.bat actually supplied: GALLIUM_DRIVER=
  nouveau (mesa3d.dll's os_get_option on Win9x = getenv, then vmhal_setup_str("mesa", name)
  = HKLM\Software\vmdisp9x\apps\<exe>\mesa, then apps\global\mesa), MESA_EXTENSION_MAX_YEAR
  (already in the registry for hl.exe since 3051), the mesa3d.dll copy next to hl.exe (only
  because an old copy there wins over C:\WINDOWS\SYSTEM), and diagnostics.
- 3110: (1) nvgl-on.reg / INF: apps\global\mesa GALLIUM_DRIVER=nouveau -> every OpenGL
  program gets the hardware screen; per-exe opt-out GALLIUM_DRIVER=llvmpipe; nvgl-off.reg.
  (2) wgl.c: when the named driver "nouveau" fails to create a screen, continue to llvmpipe
  (upstream breaks out of the loop when GALLIUM_DRIVER is set -> no OpenGL at all).
  (3) VxD OP_NV_GPU_ACQUIRE 0x501B: NV_gpu_acquire(tagProcess) - one owner at a time,
  taken at the shim's device_new, a different live owner = busy -> device_new fails ->
  software for that process; released by NV_gpu_release at DIOC_CLOSEHANDLE (CloseHandle,
  process exit, crash). A new owner gets a cold channel (gpu_chan_restart, shared with the
  3104 bound-again path, which stays as the safety net for old shims). Old VxD + new shim:
  the ioctl fails -> treated as ok (old behaviour). (4) nvhl.bat stays for diagnostics.
- Test: import nvgl-on.reg, delete the mesa3d.dll next to hl.exe, reboot (VxD changed),
  start hl.exe directly; nvshim.log must show 'GPU channel acquired ok/owner'. Then start a
  second GL program while HL runs (wgltest window) - it must come up in software ('GPU
  channel BUSY' in its log) and HL must survive.
- **3110 CONFIRMED on the T61 (2026-10-30): Half-Life started from its own folder, no
  batch file, renders on the GPU. Released as v3110.** Milestone: a plain install (vxd +
  drv + mesa3d.dll in SYSTEM + nvgl-on.reg) runs OpenGL games on the hardware.

### Build 3111: PGRAPH context default = variant 2 (current nouveau)
- With 3110 confirmed the only known divergence from nouveau was the default context
  generation: variant 0 (2.6.36) leaves tesla UNK1968 = 3 where nouveau writes 0. 3100's
  garbage was the shared channel (3104), and on 3108 grctx 2 rendered like grctx 0 (holes
  from the row-block bug). 3111: g_grctx_variant defaults to 2 when the GrCtx key is
  absent; grctx0.reg selects the old one without a reboot. VxD changed (reboot).
- Untested on the T61. Test: Half-Life as usual ('grctx variant 2' in nvtail.txt); A/B
  against grctx0.reg if anything regressed.

### NEXT: (a) real present for the nouveau screen (wgl_present is a stub guard, so a
drawn frame never reaches the display) - read the GPU color buffer back via the shim
and blit it to the visible framebuffer (first version), later a 2D-engine blit into
the scanout surface; then a clear + triangle visible in the wgltest window.
(b) make the registry path work (debug vmhal_setup_str / vmdisp9x.dll load in a
GL-only process) so Half-Life, launched without a DOS box, picks nouveau.

### NEXT (M5.4 continued): run nv50_screen_create on the T61, then draw
Wire GALLIUM_DRIVER=nouveau and run nv50_screen_create on the T61 - its fence now
lands. Then the BO-backed hwctx tail (VP/FP/GP code BO, TLS, stack, uniforms/CB with
real nv_heap allocations), a clear+triangle, and the real present path (wgl_present
is a stub guard today) -> Half-Life on the GPU.

### REFERENCE: making the nouveau screen INITIALISE on the T61
Set GALLIUM_DRIVER=nouveau (HKLM\Software\vmdisp9x\apps\<exe>\mesa, like
MESA_EXTENSION_MAX_YEAR) so the ICD picks nouveau. Then nouveau_drm_screen_create ->
nouveau_screen_create -> nv50_screen_create must run against the VxD. For that:
- The VxD channel must bind MESA's RAMHT handle set (nv04_fifo.vram=0xbeef0201 /
  .gart=0xbeef0202 from nouveau_screen.c, plus engine objs 0xbeef0301 notifier /
  5039 M2MF / 502d 2D / 5097 3D) instead of the current fixed 0xbeef502d/0002/0003.
  Add an object-bind path (extend nv_chan_init or a new OP_NV_BIND ioctl driven from
  nouveau_object_new). Get nouveau_screen_init + nv50_screen_create to run.
- NOTE: screen init emits a fence (3D QUERY) which TRAPs until M5.4 - expect M5.4
  (the 3D class) to follow immediately.
Then M5.4: full NV84_3D (0x8297) bring-up (the 3D QUERY fence TRAPs today, needs the
~15 DMA-object + register setup nv50_screen does) and wire Mesa as the GL ICD ->
Half-Life on the GPU.

Build/test loop: edit under driver/vmdisp9x-nv; build VxD with /home/guilherme/watcom,
DRV with /home/guilherme/watcom-2025-08, nvprobe with `i686-w64-mingw32-gcc -O2 -s
-DNVSHIM_STANDALONE tools/nvprobe.c nvshim.c -I. -o tools/nvprobe.exe` (the -D is
REQUIRED since M5.3c - nvshim.c defaults to Mesa-header mode without it);
run tools/nerelocs.py on the DRV; sh tools/make_dist.sh; bump NV_BUILD in
vxd_nv.h + init.c; `cd dist && zip -qr ../nvmini-NNNN.zip driver`; commit + push.
User flashes dist/driver (or the local zip) to the T61, runs the test, confirms.
RELEASE POLICY (user, 2026-10-28): `gh release create vNNNN <zip> --title ...
--notes-file ... --target main` ONLY for builds confirmed working on the T61 or
for milestones - never for untested/diagnostic builds. Commit attribution: Co-Authored-By: Claude Opus 4.8.

## Why it is big, and why it is feasible

The G86 is an NV50-family (Tesla) GPU. Its 3D engine is class 0x8297
(G84_3D / "NV84_3D"), fed - like the 2D (0x502d) and M2MF copy (0x5039)
engines - through a PFIFO DMA pushbuffer channel (PGRAPH), which is a
different channel type from the EVO *display* channel we already drive.

The whole **userspace** stack already exists and we already compile it:
Mesa's nouveau nv50 Gallium driver (`src/gallium/drivers/nouveau/nv50`, ~20k
lines) turns OpenGL state into nv50 hardware methods, and Mesa's nv50 codegen
compiles GLSL to the Tesla shader ISA. We do not write any of that.

What does NOT exist for Windows 9x is the **kernel half**. On Linux, nouveau's
Mesa winsys calls libdrm_nouveau, which calls the kernel nouveau DRM for GPU
channels, memory, command submission and fences. On the T61 there is no such
kernel, so our VxD must provide those services, and a small Win9x winsys must
map libdrm_nouveau onto them. That winsys contract is ~25 functions:

    device/client:  nouveau_drm_new, nouveau_device_new, nouveau_client_new (+del)
    objects:        nouveau_object_new/del      (channel + engine classes via RAMHT)
    GPU memory:     nouveau_bo_new/ref/map/wait/wrap   (VRAM heap + GART)
    bufctx:         nouveau_bufctx_new/del/refn        (per-draw BO reference bins)
    submission:     nouveau_pushbuf_new/space/data/reloc/refn/kick/validate
    fences:         nouveau_fence_new/ref/wait/kick     (semaphore write + poll)

We already know a lot of the hardware from EVO: RAMHT hashing, ctxdma objects,
instance memory (RAMIN) layout, channel bring-up and notifier-polling for
sync (we have no serviceable display IRQ, so 3D sync is by polling too).
The genuinely new VxD pieces are (a) a GPU memory manager (VRAM heap + a GART
page table so the GPU can read system-memory buffers) and (b) a PFIFO DMA
channel with pushbuffer submission + relocation patching.

## Milestones

- **M5.1 - PFIFO channel + the 2D engine (first PGRAPH command).** Bring up a
  nv50 PFIFO DMA pushbuffer channel, bind the 2D class (0x502d) through RAMHT,
  and submit a solid-fill / copy of a rectangle in VRAM. Verify by reading the
  region back with `nvprobe`. Proves we can drive PGRAPH at all; reuses the EVO
  RAMHT/ctxdma/instance-memory/channel-init machinery. Opt-in IOCTL, no effect
  on the desktop. **This is the first testable step.**
- **M5.2 - GPU memory manager + fences.** A VRAM heap allocator and a GART
  (system pages mapped into the GPU address space) in the VxD, plus
  semaphore-based fence writes polled from the CPU. Enough to back
  nouveau_bo_* and nouveau_fence_*.
- **M5.3 - Win9x nouveau winsys.** Implement the ~25-function contract against
  the VxD services; compile Mesa's nv50 Gallium driver into mesa3d.dll
  (selected instead of llvmpipe when the G86 is present).
- **M5.4 - 3D class bring-up + wire as the ICD.** NV84_3D (0x8297): render
  target, clear, one shaded triangle through the real pipeline; then hand the
  hardware ICD to opengl32 and run Half-Life on the GPU.

Each milestone is cold-boot tested on the T61 and can black-screen the 3D
engine (never the display channel, which stays independent), so every step is
opt-in and reversible. Hardware references: Mesa's nv50 *.xml.h method headers
(in the Mesa tree we build), envytools NV50 docs, nouveau, and the XP
nv4_mini.sys disassembly.

## Honest expectation

This is weeks of staged, iterative bring-up on real hardware, not a single
change. The payoff is real GPU rendering. The fallback (software llvmpipe)
stays the default until M5.4 proves out, so nothing that works today regresses.

## M5.1 recipe (derived from nouveau v2.6.36 + envytools + reused EVO machinery)

References pulled 2026-10-23 via `gh api` (nouveau Linux v2.6.36: nv50_fifo.c,
nv50_instmem.c, nv50_graph.c, nouveau_channel.c, nouveau_object.c,
nouveau_reg.h) and the envytools method headers now in analysis/nv50_ref/
(nv_object.xml.h, nv_m2mf.xml.h, nv50_2d.xml.h, nv50_3d.xml.h). Kept in the
scratchpad, NOT committed (GPL kernel source); only our derived recipe and the
MIT envytools headers live in the tree.

Goal of M5.1: bring up one PFIFO DMA channel, bind the 2D engine (0x502d),
solid-fill a rectangle in VRAM, read it back with nvprobe. Opt-in IOCTL
(OP_NV_GPU_2DFILL), never touches the display channel. Reuses our existing
RAMHT (ramht_add/ramht_hash), ctxdma (build_ctxdma), instance memory and
NV_vram_* helpers from nv50_evo.c.

### Registers (BAR0), from nouveau_reg.h
- PFIFO channel USER control:  NV50_USER(c)      = 0x00c00000 + c*0x2000
    DMA_PUT = 0x00c00040 + c*0x2000,  DMA_GET = 0x00c00044 + c*0x2000
- Context table (per channel):  NV50_PFIFO_CTX_TABLE(c) = 0x2600 + c*4
    bit31 CHANNEL_ENABLED; value = ramfc_instance >> 8 (G84/G86) | enabled
- PFIFO caches/init: 0x2500 (CACHES: 0 off, 0x101 on), 0x250c=0x6f3cfc34,
    0x3250/0x3220/0x3204/0x3210/0x3270 = 0, INTR_0 0x2100 = 0xFFFFFFFF,
    INTR_EN_0 0x2140 = 0xFFFFFFFF.
- PMC enable 0x200: ensure PFIFO (bit8) + PGRAPH (bit12) on (already on after
    the EVO path runs, but assert it).

### Instance memory layout (extend our RAMIN region in VRAM)
We already own RAMIN at NVFB_RAMIN_VRAM_OFFSET with RAMHT + two ctxdmas for
EVO. Add, in free RAMIN space, for the GPU channel (all offsets are byte
offsets into the channel's RAMIN block; nv50 chan RAMIN is 0x1000-aligned):
- channel RAMFC at +0x0000 (0x100 bytes), RAMFC fields (dword offsets):
    0x48 = pushbuf_ctxdma_instance >> 4      (pushbuffer ctxdma, class 0x3d)
    0x80 = (0<<27)|(4<<24)|(ramht_instance >> 4)   /* 4 KiB, SEARCH_FULL */
    0x44 = 0x2101ffff
    0x60 = 0x7fffffff
    0x40 = 0
    0x7c = 0x30000001
    0x78 = 0
    0x3c = 0x403f6078
    0x50 = pushbuf_base (byte offset of the first method in the pushbuf DMA)
    0x54 = order(ib_max+1)<<16   (0 if not using an indirect buffer)
  G84/G86 ALSO:  ramin[0]=chan_id, ramin[1]=ramfc_instance>>8,
    ramfc 0x88 = cache_instance>>10, ramfc 0x98 = ramin_instance>>12
- FIFO cache at +0x0400 (4 KiB)
- RAMHT: reuse ours (methods are bound by handle through it, like EVO).
- pushbuffer: a buffer in VRAM; a ctxdma (class 0x3d) spanning it (or all
    VRAM, like NvEvoVRAM_LP) is referenced by RAMFC 0x48. We use DIRECT DMA
    (no IB): write methods into the pushbuffer, set DMA_PUT = byte length,
    hardware runs GET->PUT, poll GET==PUT. Same model as our EVO ring.

### Channel bring-up (direct-DMA)
1. Assert PMC 0x200 bits for PFIFO+PGRAPH. nv50_graph must be initialised
   (nv50_graph.c nv50_graph_init: PGRAPH reset via 0x200, ctxprog upload) -
   for M5.1 the 2D engine needs PGRAPH context; confirm from nv50_graph.c how
   much init the 2D object needs (may need the ctxctl microcode load; if so,
   M5.1 grows - verify before coding).
2. Build RAMFC (above) + pushbuffer ctxdma over VRAM.
3. Bind engine objects through RAMHT (ramht_add), like EVO:
     handle 0xbeef502d -> ctx for 2D class 0x502d
     (later 0xbeef5039 M2MF 0x5039, 0xbeef8297 3D)
   nv50 object ctx value in RAMHT entry for an engine object encodes the
   oclass + engine; derive the exact ctx dword from nouveau_object.c
   (nv50_gpuobj_class / the RAMHT entry format) - TODO confirm.
4. CTX_TABLE(c) = (ramfc_instance>>8) | CHANNEL_ENABLED; playlist update
   (nv50_fifo_playlist_update writes the list of active chids to a VRAM
   buffer and pokes 0x32f4/0x32ec - get exact regs from nv50_fifo.c).
5. CACHES on: 0x2500 = 0x101.

### 2D solid fill method stream (nv50_2d.xml.h), written to the pushbuffer
Header per batch = (count<<18)|(subchannel<<13)|method, subchannel 0 bound to
the 2D object via a BIND (0x0000 = SET_OBJECT, data = handle 0xbeef502d).
  0x0000 SET_OBJECT        = 0xbeef502d
  0x0184 DMA_DST           = dst ctxdma handle (VRAM)
  0x0200 DST_FORMAT        = e.g. 0x000000e5 (A8R8G8B8) or a 16bpp fmt
  0x0204 DST_LINEAR        = 1
  0x0214 DST_PITCH         = pitch bytes
  0x0218 DST_WIDTH         = w
  0x021c DST_HEIGHT        = h
  0x0220 DST_ADDRESS_HIGH  = (dst>>32)
  0x0224 DST_ADDRESS_LOW   = dst (byte offset in the VRAM ctxdma)
  0x02ac OPERATION         = SRCCOPY (0x3) for an opaque fill
  0x0580 DRAW_SHAPE        = 4 (RECTANGLES)
  0x0584 DRAW_COLOR_FORMAT = same family as DST_FORMAT
  0x0588 DRAW_COLOR        = fill colour
  0x0600 DRAW_POINT32_X(0) = x0 ; 0x0604 DRAW_POINT32_Y(0) = y0
  0x0608 DRAW_POINT32_X(1) = x1 ; 0x060c DRAW_POINT32_Y(1) = y1
Then DMA_PUT = stream_bytes; poll DMA_GET==DMA_PUT; read back VRAM.

### RESOLVED: PGRAPH needs the chip-specific context microcode (the big one)

On NV50 every data-moving/rendering engine - 2D (0x502d), M2MF (0x5039) and
3D (0x8297) - is a PGRAPH object, and PGRAPH executes NOTHING until:
  (a) nv50_graph_init_ctxctl uploads a "ctxprog" context-switch microcode to
      PGRAPH_CTXCTL (NV40_PGRAPH_CTXCTL_UCODE_INDEX/_DATA), and
  (b) each channel has a per-channel graphics context (ramin_grctx) seeded with
      chip-specific "ctxvals", referenced from the channel RAMIN header.
Both come from nouveau's nv50_grctx.c (nv50_grctx_init) - 2383 lines,
table-driven, and it explicitly handles chipset 0x86 (`case 0x84: case 0x86:`).
No shortcut: even a 2D fill or an M2MF copy needs it, because the engine's
thousands of state registers are set up by loading the initial context. So:

  **M5.1a - port nv50_grctx.c for the G86 (chipset 0x86).** Reimplement the
  ctxprog + ctxvals generator in the VxD (or precompute both on the host from
  the ported generator and ship them as blobs). ~2.4k lines of portable C with
  little kernel dependency; cross-check the generated G86 context against what
  the XP nv4_mini.sys loads. This is the crux of nv50 bring-up.
  **M5.1b** - then the PFIFO channel + 2D fill recipe above.

### Other open items (smaller)
- Exact RAMHT ctx-dword format for an nv50 ENGINE object (nouveau_object.c).
- nv50_fifo_playlist_update exact registers (0x32f4 / 0x32ec area).
- DST_FORMAT/DRAW_COLOR_FORMAT enum values (g80_defs.xml.h).

The single biggest unknown is now de-risked: the context generator exists as
portable C in nouveau and covers the G86, so M5.1a is a porting job, not a
research dead-end.

## M5.1a DONE + full M5.1b recipe (2026-10-23)

M5.1a (context microcode) is DONE. nouveau v2.6.36 nv50_grctx.c was ported to
run on the host unmodified (scratchpad tools/grctx_host: drmP.h/nouveau_drv.h
stubs + main.c; `gcc main.c nv50_grctx.c`). The T61's G86 reports
0x1540 = 0xF3010001 (read with `nvprobe rd 1540`). Generated for
chipset 0x86 + that units value: ctxprog = 110 dwords, grctx_size = 378112
bytes (94528 dwords, 660 non-zero). Baked into driver/vmdisp9x-nv/
nv50_grctx_g86.h (derived data, like the EVO streams). Regenerate for another
board by reading its 0x1540 and re-running the host tool.

### M5.1b - PGRAPH init + channel + 2D fill (all registers derived)

PGRAPH init (BAR0), from nv50_graph.c, do once:
- reset: PMC_ENABLE(0x200) &= ~((1<<12)|(1<<21)); then |= it (toggle PGRAPH).
- init_regs__nv: 0x400804=0xc0000000, 0x406800=0xc0000000, 0x400c04=.., 0x401800,
  0x405018, 0x402000 all 0xc0000000; per-unit (chipset<0xa0, i in enabled set)
  0x408900+(i<<12),0x408e08+(i<<12),0x408314+(i<<12)=0xc0000000 and
  0x408600+(i<<11),0x408708+(i<<11),0x40831c+(i<<11)=0xc0000000; then
  0x400108=0xffffffff, 0x400824=0x00004000, 0x400500=0x00010001.
- init_regs: 0x400be0(NV04_PGRAPH_DEBUG_3)=(1<<2) HW_CONTEXT_SWITCH_ENABLED;
  0x402ca8=0x800.
- init_ctxctl: NV40_PGRAPH_CTXCTL_UCODE_INDEX=0; loop NV40_PGRAPH_CTXCTL_UCODE_DATA
  = nv50_ctxprog[i] (110 of them); 0x400320=4; CTXCTL_CUR=0;
  CHANNEL_CTX_POINTER=0.  (CTXCTL_UCODE_INDEX/_DATA, CTXCTL_CUR regs from
  nouveau_reg.h - grab exact addrs.)

Channel (reuse EVO ramht_hash/build_ctxdma/NV_vram_* ; own RAMIN block):
- pushbuffer ctxdma (class 0x3d) over VRAM (like NvEvoVRAM_LP).
- RAMFC (dword offsets): 0x48=pushbuf_ctxdma_inst>>4; 0x80=(0<<27)|(4<<24)|
  (ramht_inst>>4); 0x44=0x2101ffff; 0x60=0x7fffffff; 0x40=0; 0x7c=0x30000001;
  0x78=0; 0x3c=0x403f6078; 0x50=pushbuf_base(byte off of 1st method); 0x54=0
  (no IB). G86 also: ramin[0]=chid(0), ramin[1]=ramfc_inst>>8, ramfc 0x88=
  cache_inst>>10, ramfc 0x98=ramin_inst>>12.
- grctx: 378112-byte VRAM block, 0x1000-aligned, ZEROED, then apply the 660
  nv50_ctxvals pairs; grctx[0]=ramin_inst>>12. Channel RAMIN header at byte
  0x20: +0x00=0x00190002; +0x04=grctx_inst+grctx_size-1; +0x08=grctx_inst;
  +0x0c=0; +0x10=0; +0x14=0x00010000.
- 2D object instance (class 0x502d): small block (class_instmem_size; use 6
  dwords), obj[0]=0x502d, obj[5]=0x00010000, engine=GR(1). RAMHT entry:
  handle=0xbeef502d, ctx=(obj_inst>>4)|(1<<20)  [NV40_RAMHT_CONTEXT_ENGINE_SHIFT
  =20, graphics engine=1]. hash = fold(handle, ramht_bits)<<3 (NO channel xor on
  nv50) - our ramht_hash already does this.
- enable: CTX_TABLE(0)=(ramfc_inst>>8)|(1<<31); playlist update (write active
  chids to a VRAM list, poke the playlist regs - get exact from nv50_fifo.c
  nv50_fifo_playlist_update); CACHES 0x2500=0x101.

2D solid fill into the pushbuffer (header=(count<<18)|(subc<<13)|method,
subc 0): 0x0000 SET_OBJECT=0xbeef502d; 0x0184 DMA_DST=pushbuf/VRAM ctxdma
handle; 0x0200 DST_FORMAT; 0x0204 DST_LINEAR=1; 0x0214 PITCH; 0x0218 W; 0x021c
H; 0x0220/0x0224 DST_ADDR hi/lo; 0x02ac OPERATION=SRCCOPY; 0x0580 DRAW_SHAPE=4;
0x0584 DRAW_COLOR_FORMAT; 0x0588 DRAW_COLOR; 0x0600/0x0604 POINT32(0)=x0,y0;
0x0608/0x060c POINT32(1)=x1,y1. Then USER_DMA_PUT(0)(0xc00040)=bytes; poll
USER_DMA_GET(0)(0xc00044)==PUT; read back the VRAM rect with nvprobe.

Opt-in IOCTL OP_NV_GPU_2DFILL; nvprobe `gpu2d`. Never touches the EVO display
channel. A fault parks PGRAPH, recover by cold boot (like EVO bring-up).

## M5.2 recipe - the NV50 GPU virtual memory (derived 2026-10-24)

M5.1 proved the 2D engine with a PHYSICAL ctxdma (no VM). Mesa's nv50 3D driver
addresses all buffers by GPU VIRTUAL address, so M5.2 brings up the channel VM.
Derived from nouveau v2.6.36 (nv50_instmem.c, nouveau_object.c
nouveau_gpuobj_channel_init, nouveau_mem.c nv50_mem_vm_bind_linear,
nouveau_drv.h). Constants: NV50_VM_BLOCK = 512 MiB (coverage per PDE),
NV50_VM_MAX_VRAM = 2 GiB, 64 KiB pages, 8-byte PTEs.

VM address layout (one PDE = 512 MiB):
  PDE 0  (VM 0)        = PRAMIN/instance   (nouveau "cheat": instance at VM 0)
  PDE 2  (VM 512 MiB)  = GART   (vm_gart_base)      [skip for M5.2a]
  PDE 4  (VM 1 GiB)    = VRAM PT[0] (vm_vram_base); PDE 6/8/10 = PT[1..3] for 2 GiB
  So a VRAM BO at physical P maps 1:1 to VM address (1 GiB + P).

Page directory (per channel): 0x4000 bytes (2048 PDEs x 8). Lives INSIDE the
channel RAMIN at offset 0x200 (G84/G86; 0x1400 on G80). Init every PDE to
{0, 0xdeadcafe}. Then:
  PDE dword (vm_vram_base/512MiB)*2 = 4  ->  nv_wo32(pd, 4, vram_pt_inst | 0x61)
                                             nv_wo32(pd, 5, 0)
  (GART PDE dword 2 = gart_pt | 0x03, when GART is added.)

VRAM page table: covers 512 MiB => 512MiB/64KiB = 8192 PTEs x 8 = 64 KiB.
Map VM page -> physical VRAM page P (nv50_mem_vm_bind_linear, VRAM case):
  pte_index (dword) = ((VM_addr - vm_vram_base) >> 16) << 1
  pte_lo = (P & 0xffffffff) | 1          (bit0 = present; flags 0 for linear)
  pte_hi = upper_32_bits(P)              (0 for our <4 GiB VRAM)
  (GART/system pages instead OR in 0x30 and add vram_sys_base.)

ctxdma that targets the VM (for engine DMA_DST/SRC etc. to use VM addresses),
vs the physical VIDMEM ctxdma M5.1 used:
  VM/AGP target:  dword0 = 0x7fc00000 | class(0x3d),  dword5 = 0x00080000
  VIDMEM target:  dword0 = 0x00190000 | 0x3d,         dword5 = 0x00010000  (M5.1)
The engine translates a VM ctxdma's offset through the channel's PD at RAMIN+0x200.

VM config / BAR regs (nouveau_reg.h): NV50_PUNK_BAR0_PRAMIN 0x1700 (CPU PRAMIN
window, =pramin_phys>>16), NV50_PUNK_BAR_CFG_BASE 0x1704 (=ramin>>12 |VALID<<30),
NV50_PUNK_BAR1_CTXDMA 0x1708, NV50_PUNK_UNK1710 0x1710. These set up BAR1/PRAMIN
instance access for the dummy channel; a graphics channel's own VM is found via
its RAMIN PD at +0x200.

### M5.2a - first test: a VM-addressed 2D fill
Reuse the M5.1 channel. Add: a VRAM page table; the channel PD at RAMIN+0x200
with PDE 4 -> the PT; map the scratch region into the VM at (1 GiB + scratch);
a VM-target ctxdma; redo the 2D fill with DST_ADDR = the VM address and
DMA_DST = the VM ctxdma. Read back the scratch PHYSICALLY: if it holds the fill
colour, the VM translated our virtual write correctly. OPEN: whether PDE->PT and
the PD itself are referenced physically or PRAMIN-relative (M5.1 showed PGRAPH
ctx = physical, PFIFO objects = channel-relative; the hardware test decides).
Then M5.2b: a VRAM heap allocator + semaphore fences; M5.3: the Mesa winsys.
