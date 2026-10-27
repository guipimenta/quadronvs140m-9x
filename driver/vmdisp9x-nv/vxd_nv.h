/*****************************************************************************
 * vxd_nv.h - NVIDIA G8x (NV50-family) hardware detection for the vmdisp9x fork
 *
 * Milestone 2 scope: enumerate the GPU on the PCI bus, map BAR0 (the MMIO
 * register aperture), read PMC_BOOT_0 and decode the chipset id. No modeset,
 * no framebuffer ownership yet - this only proves our code can find and talk
 * to the G86. See docs/ROADMAP.md.
 *
 * This file is part of a project to build a native Windows 9x display driver
 * for the Quadro NVS 140M (G86); it is licensed MIT like the vmdisp9x base.
 *****************************************************************************/
#ifndef __VXD_NV_H__
#define __VXD_NV_H__

/* Build stamp. Bump on every rebuild. nvprobe compares the VxD's value against
 * the one it was compiled with, so version skew (stale VxD or stale EXE) is
 * obvious. Scheme: milestone*1000 + revision. */
#define NV_BUILD            3084UL   /* 3084 = M5.4 step 4b: 3083 on the T61: screen create OK (same log as 3082) but CreateContext 126 right after - the failure is in CONTEXT creation (stw / st_api_create_context / nv50_create), which had no trace. This build adds a runtime trace hook: u_debug.c defines a NULL function pointer mesa9x_trace_hook (every target links util, so shared objects stay link-clean); the nouveau ICD sets it to nvshim_trace when nouveau is selected. Traced into C:\nvshim.log: stw_create_context_attribs (enter hdc/pfi, profile, st_api_create_context result, the no_ctx/no_st_ctx exits), stw_pixelformat_init count, st_api_create_context (pipe from screen->context_create, st), nv50_create (enter, blitctx, nouveau_context_init ret, bufctx_new x3 ret, stream_uploader, OK/FAILED), nouveau_context_init (client_new / pushbuf ret) and the shim's client_new/bufctx_new. The last line names the failing step. VxD/nvprobe byte-identical bar the stamp. 3083 = M5.4 step 4: FIRST REAL PRESENT for the nouveau screen. 3082 proved Mesa's nv50_screen_create runs on the G86 (with `set GALLIUM_DRIVER=nouveau` in the DOS box - the registry path via vmdisp9x.dll's vmhal_setup_str returns nothing, lasterr 2). wgl_present for a nouveau screen now reads the GPU colour buffer back (pipe_texture_map: Mesa's nv50 transfer de-tiles the tiled miptree with the M2MF engine into a staging BO, which the shim maps by reading VRAM) and pushes the pixels into the framebuffer via a new vramcpy_display_raw() (FBHDA fast path like the software renderer, GDI StretchDIBits fallback). Shim: nouveau_bo_map/bo_wait now kick the pending pushbuf first (libdrm does this implicitly; without it the M2MF copy was never submitted before the readback), and the bump heap rolls back when the top-most BO is freed (the per-frame staging buffer) so presents don't exhaust it. Expected on the T61: wgltest's window shows the GPU-cleared colour; log line 'wgl: present map/stride'. VxD/nvprobe byte-identical bar the stamp. 3082 = M5.4 step 3d: 3081 (fd-cache bypass) did NOT change the T61 result (CreateContext 126, still no C:\nvshim.log) - so the failure is UPSTREAM of the shim entirely. This build traces the CALLER CHAIN inside mesa3d.dll into the same C:\nvshim.log via an exported nvshim_trace(): wgl.c logs screen_create ENTER (GetLastError at entry), the GALLIUM_DRIVER name read from the registry, every driver tried and its result + GetLastError, and the nouveau_drm_screen_create enter/exit; nouveau_drm_winsys.c logs each nouveau_screen_create step (drm_new/device_new/chipset/init/context_create). One run now shows whether 'nouveau' is even selected and where 126 first appears. VxD/nvprobe byte-identical to 3081 bar the stamp. 3081 = M5.4 step 3c: ROOT CAUSE + FIX of the 3079/3080 CreateContext 126 with NO shim trace. Mesa's nouveau_drm_screen_create() first dup()s + fstat()s the DRM fd for its fd-keyed screen cache (u_pipe_screen_lookup_or_create); wgl.c passes fd 0, which for a GUI process on Win9x is invalid, so it returned NULL BEFORE nouveau_screen_create - the shim (which ignores the fd and opens the VxD itself) was never entered, hence no C:\nvshim.log. mesa9x nouveau_drm_winsys.c now calls nouveau_screen_create(fd,NULL,NULL) directly under WIN9X (4th file in nouveau-build-win9x.patch); mesa3d.dll rebuilt (import table identical to 3080's). VxD/nvprobe byte-identical to 3080 bar the stamp. Expected on the T61: C:\nvshim.log now exists and shows how far nv50_screen_create gets. 3080 = M5.4 step 3b: diagnose why Mesa's nv50_screen_create fails on hardware. 3079's wgltest nouveau run returned CreateContext err 126 (ERROR_MOD_NOT_FOUND leftover) = nouveau_drm_screen_create() returned NULL, i.e. nouveau_screen_init failed; the GALLIUM_NOUVEAU dispatch in wgl.c IS compiled in (wgl_icd.c #includes wgl.c with -DGALLIUM_NOUVEAU), so the shim WAS entered. But the `nvprobe log` came back with ZERO "S> " shim lines: the desktop's per-second HB heartbeat lines wrap the 16 KB VxD debug ring in seconds and evicted the init trace before it could be read. Fix: nvshim.c's trace now writes PRIMARILY to a ring-3 disk file C:\nvshim.log (truncated per run, flushed per line) - immune to the ring flood, survives the failure dialog; the VxD ring stays a secondary sink. Likely root cause (shared with 3079's `nvprobe 3d` regressing from cold): our minimal hwctx leaves the CROP write-fence unreliable from a pristine grctx, so screen_init's verify-fence times out. The file trace will show the exact stage (bo_new/object_new counts, last kick ok/FAIL). VxD/nvprobe binaries are byte-identical to 3079 bar the stamp. 3079 = M5.4 step 3: run the REAL Mesa nv50_screen_create on hardware. Ships the NOUVEAU mesa3d.dll (software still the default; nouveau is opt-in via GALLIUM_DRIVER=nouveau, set for icdtest.exe by nouveau.reg). The ring-3 libdrm shim (nvshim.c) now TRACES the screen-init path into the VxD debug ring via a new OP_NV_DBG_PUTS (0x5018) ioctl - lines prefixed "S> ", read with `nvprobe log` after the run (survives an app crash). Traced: drm_new/device_new/device_info/object_new(handle,oclass)/bo_new(sz,phys,vmoff)/pushbuf_new + each kick's result (count,ctxctl/put,pgraph_intr; first 64 ok + first 32 fails). Lets us see exactly how far nv50_screen_create gets and whether its hwctx+QUERY fence land through Mesa's own generated stream. Prior: 3078 = M5.4 step 2: 3D QUERY fence with hwctx prefix. Build 3077's bare QUERY TRAPped (PGRAPH_INTR 0x00200000 = PGRAPH unit TRAP) - the CROP unit the UNIT_CROP fence waits on had no valid DMA_COLOR/ZETA/RT state. 3078 runs the BO-FREE PREFIX of nv50_screen_init_hwctx before the QUERY: DMA_NOTIFY + DMA_ZETA(11) + DMA_COLOR(0)(8) all = VM ctxdma 0xbeef0003, then REG_MODE/UNK1400_LANES/WATCHDOG/ZETA_COMP/RT_COMP(8)/RT_CONTROL/CSAA/MULTISAMPLE/PRIM_RESTART/BLEND/SCREEN_Y/WINDOW_OFFSET/ZCULL - exactly nouveau's pre-fence setup (the BO-backed VP/FP/TLS/CB come later). 'nvprobe 3d' now also dumps the trap regs (0x400108 + DISPATCH 0x400808/80c/810 + VFETCH/CCACHE) so a remaining trap is diagnosable. 3077 = M5.4 step 1: REAL NV84_3D QUERY fence - OP_NV_GPU_3DTEST (0x5017) binds the NV84_3D engine object (0x8297) into the channel and submits the exact QUERY_GET write-fence that nv50_screen_create emits (addr=VM BO, seq, get=0x1000f010). Today that op TRAPs the moment a nouveau screen inits; landing it (the 3D engine writes the sequence to a VM-mapped BO) proves the 3D engine runs on real hardware and clears the last blocker before nv50_screen_create. 'nvprobe 3d' runs it (opt-in; touches PGRAPH/PFIFO only, reboot to recover a trap). gpu_2dfill_core gained NVF_3D: same proven channel/VM/grctx bring-up, 3D object + QUERY stream instead of the 2D-write fence stand-in; the 2D/VM/fence paths are byte-identical when NVF_3D is off. 3076 = M5.3c step 3: object-bind path - OP_NV_BIND (0x5016) binds an object (engine gfx 2D/M2MF/3D, or a VM ctxdma/notifier) at a CALLER-CHOSEN handle into the persistent channel's RAMHT, from a bump instance area (NVGPU_OBJAREA, 32 KiB in the channel's 1 MiB window). This is what the Mesa nouveau winsys needs: its nouveau_object_new (nvshim.c) routes the channel's vram/gart ctxdmas (0xbeef0201/0202) + engine objects (0xbeef5039/502d/5097) + notifier (0xbeef0301) here. 'nvprobe bind' binds Mesa's handles then fills via them (opt-in). Additive: the fixed nv_chan_init bindings used by gpu2d/submit are untouched. 3075 = M5.3b: the libdrm_nouveau SHIM (nvshim.c/.h) - the ~30-function nouveau winsys contract (device/client/object/bo/bufctx/pushbuf) implemented in ring-3 over the VxD, so Mesa's nv50 driver can be wired to it unchanged (M5.3c). BO map uses a bounce-buffer model (CPU shadow flushed to VRAM on kick via the new OP_NV_VRAM_WRITE block-write ioctl; refreshed from VRAM on read-map); VRAM is a ring-3 bump heap over the channel's 1:1 VM window. 'nvprobe shim' drives a 2D fill into a heap BO through the contract and reads it back - proves the ring-3 kernel contract on hardware. 3074 = M5.3a: persistent channel + ring-3 IB submit. M4.1 fix: scanout pitch rounded up to 256 bytes (NV50 linear surface rule) - 800x600 showed garbage with pitch 3200. 3039 = M4.1+M4.2: native modes 640x480/800x600/1024x768 x 8/16/32 (scaler for the lower two, 8 bpp as palette-expanded 32 bpp scanout); INF binds the root VGA devnode too. 3038 = M4.3: native memory setup - PFB VRAM size read in live context (NV_vram_check), layout validated against the EVO reservations before any native modeset, scanout window from constants+BAR1 (24 MB max, was a fixed 12 MB), all reported by nvprobe. 3037 = E4 r27: 16-bit driver linked with explicit FIXED DATA/BSS segment directives and the 2025-08 Open Watcom toolchain (the Oct-2026 wlink left DGROUP moveable -> Win98 KERNEL never called DriverInit: LoadFail 0000 on T61 and in QEMU). 3036 = E4 r26: dbg output also mirrored to I/O port 0xE9 (QEMU -debugcon) from both sides. 3035 = E4 r25: nvmini.drv also appends its dbg lines to C:\\NVMINI.LOG (KERNEL _lopen/_lwrite, capped 600 lines); VxD logs dispatch-table length, first PM16 call, unknown PM16 services, FBHDA_SETUP arrival. 3034 = E4 r24: DBGPRINT on, routed to an in-VxD ring (dbgring.h): VxD lines 'V> ', nvmini.drv lines 'D> ' via OP_NV_DBG_PUTC; read with 'nvprobe log' (OP_NV_DBGLOG). 3033 = E4 r23: Native defaults to 1 (registry Native=0 opts out); INF writes Native=1 and ships nvsetup.reg. 3032 = E4 r22: native setmode arms the blitter (vga_mode/act_mode), register_display_driver tolerates hda==NULL (Native=0), EVO takeover claims nv_owned only after a completed commit; INF adds DEFAULT\DDC=0 (skips the VDD's 173 s V86 EDID probe). 3031 = E4 r21: nvmini.drv probes VDD device ids 1..4 in VDD_REGISTER_DISPLAY_DRIVER_INFO (Win98 VDD keys EBX as a DEVICE id; id 1 = the VGA devnode, served by the global mini-VDD table; a mini-VDD loaded for a non-VGA devnode lives in that device's private table). register_display_driver now logs the id after 0x300. */

#define NV_PCI_VENDOR       0x10DEUL  /* NVIDIA */
#define NV_DEV_NVS140M      0x0429UL  /* Quadro NVS 140M (G86) */
#define PCI_CLASS_DISPLAY   0x03      /* base class: display controller */

/* PMC_BOOT_0 lives at MMIO offset 0. For NV10 and later the chipset id is in
 * bits 20..27 (G86 => 0x86); the architecture/family nibble is bits 24..27
 * (G8x "Tesla" => 0x08, which is exactly what the XP miniport's G86 predicate
 * at nv4_mini.sys:0xe0230 tested). Revision is the low byte.
 */
#define NV_MMIO_PMC_BOOT_0  0x000000UL

/* Our op-code namespace is 0x5000 (SVGA=0x2000, VBE=0x3000, VESA=0x4000). */
#define OP_NV_PROBE         0x5000
#define OP_NV_RD32          0x5001  /* read a range of BAR0 MMIO dwords (RO) */
#define OP_NV_VRAM_RD       0x5002  /* read a range of BAR1 VRAM dwords       */
#define OP_NV_VRAM_FILL     0x5003  /* fill BAR1 VRAM dwords with a value     */
#define OP_NV_EVO_STAGE     0x5004  /* E2: stage fb+pushbuffer in VRAM (safe) */
#define OP_NV_EVO_FLIP      0x5005  /* E3: bring up EVO + flip scanout (RISKY) */
#define OP_NV_LASTSET       0x5006  /* E4: last native mode-set diag + counters */
#define OP_NV_BOOTSIM       0x5007  /* E4: run the EXACT boot takeover on demand */
#define OP_NV_INITSIM       0x5008  /* E4: run the EXACT backend init on demand   */
#define OP_NV_SETMODESIM    0x5009  /* E4: run the EXACT DRV setmode hook on demand*/
/* 0x500A/0x500B: debug ring (dbgring.h) */
#define OP_NV_MTRR          0x500C  /* M4.6: raw MTRR MSRs + MTRR.VXD result + blit stats */
#define OP_NV_GPU_GRINIT    0x500E  /* M5.1b: PGRAPH init + ctxprog upload (opt-in) */
#define OP_NV_GPU_CTXTEST   0x500F  /* M5.1b: build a channel + load its grctx (opt-in) */
#define OP_NV_GPU_2DFILL    0x5010  /* M5.1b: PFIFO channel + 2D fill a VRAM rect (opt-in) */
#define OP_NV_GPU_VMTEST    0x5011  /* M5.2a: 2D fill via a GPU virtual address (opt-in) */
#define OP_NV_GPU_FENCE     0x5012  /* M5.2b: VRAM heap + notifier fence test (opt-in) */
#define OP_NV_SUBMIT        0x5013  /* M5.3: submit a ring-3 method stream to the channel */
#define OP_NV_VRAM_READ     0x5014  /* M5.3: read back VRAM dwords (result verification)  */
#define OP_NV_VRAM_WRITE    0x5015  /* M5.3b: write a block of dwords into VRAM (BO flush) */
#define OP_NV_BIND          0x5016  /* M5.3c: bind an object at a caller handle (Mesa RAMHT) */
#define OP_NV_GPU_3DTEST    0x5017  /* M5.4: REAL NV84_3D QUERY_GET fence (the 3D engine runs) */
#define NV_MTRR_OUT_DWORDS  64      /* [0..1] MTRRcap [2..3] DEF_TYPE [4..35] 8x(base lo,hi,mask lo,hi)
                                       [36] MTRR.VXD rc [37] base [38] size [40..47] wram_stats
                                       [48] PAT state [49] PAT pages [50] PAT bad [51..52] IA32_PAT */

#define NV_RD32_MAX         256     /* max dwords per read request            */

/* Fixed-layout result, shared between the VxD and any user-space reader.
 * Only unsigned 32-bit fields so the layout is identical on both sides.
 */
typedef struct nv_hwinfo
{
	unsigned long found;       /* 1 if an NVIDIA display device was found     */
	unsigned long vendor_id;   /* expect 0x10DE                               */
	unsigned long device_id;   /* expect 0x0429 for the NVS 140M              */
	unsigned long subsys;      /* PCI config 0x2C (subsys vendor|device<<16)  */
	unsigned long pci_bus;
	unsigned long pci_dev;
	unsigned long pci_fn;
	unsigned long bar0_phys;   /* MMIO register aperture base                 */
	unsigned long bar0_size;
	unsigned long bar1_phys;   /* VRAM / framebuffer aperture base (low 32b)  */
	unsigned long bar1_size;
	unsigned long boot0;       /* raw PMC_BOOT_0                              */
	unsigned long family;      /* (boot0 >> 24) & 0x1F, expect 0x08 for G8x   */
	unsigned long chipset;     /* (boot0 >> 20) & 0xFF, expect 0x86 for G86   */
	unsigned long revision;    /* boot0 & 0xFF                                */
	unsigned long mmio_ok;     /* 1 if BAR0 mapped and boot0 read back sane   */

	/* Diagnostics, always filled - tell us whether the PCI scan reached HW. */
	unsigned long scan_devices;  /* total PCI functions with a valid vendor  */
	unsigned long nv_devices;    /* how many 0x10DE functions were seen      */
	unsigned long first_nv_devid;/* device id of the first 0x10DE function   */
	unsigned long first_nv_class;/* its class/rev dword (config offset 0x08) */
	unsigned long first_nv_bdf;  /* (bus<<16)|(dev<<8)|fn of that function    */
	unsigned long host0;         /* config dword at 0:0.0 (sanity: Intel GMCH)*/
	unsigned long build;         /* NV_BUILD of the VxD that filled this      */

	/* M4.3 native memory setup (filled by NV_vram_check / NV_vram_export) */
	unsigned long vram_size;     /* bytes of VRAM per PFB 0x10020C (0 = not read yet)   */
	unsigned long vram_hi;       /* bits 32+ of that size (G86: 0)                      */
	unsigned long vram_cfg0;     /* raw PFB 0x100200 (partition config, info only)      */
	unsigned long vram_cfg1;     /* raw PFB 0x100204                                     */
	unsigned long fb_window;     /* bytes of BAR1 mapped as the scanout window           */
	unsigned long vram_check;    /* 0 not run, 1 OK, 2 PFB read failed, 3 VRAM too small,
	                                4 window overlaps the EVO reservation               */
} nv_hwinfo_t;

/* Probe the PCI bus, fill *out. Returns non-zero if an NVIDIA display
 * controller was found (independent of whether MMIO read succeeded).
 */
int NV_probe(nv_hwinfo_t *out);

/* Last probe result, filled during Device_Init_proc when NV_DETECT is built. */
extern nv_hwinfo_t nv_hwinfo;

/* Non-zero => NV_probe decodes the chip from the PCI device id and does NOT
 * touch BAR0 MMIO (which hard-freezes at VDD DEVICEINIT). Set during early
 * Device_Init only; the Ring-3 path leaves it 0 and reads real registers. */
extern unsigned long nv_probe_skip_mmio;

/* E4: run the display driver's native backend init on demand (OP_NV_INITSIM).
 * Defined in vxd_vesa.c (NV_DETECT builds only). */
int NV_initsim(void);
/* E4: run the display driver's setmode hook on demand (OP_NV_SETMODESIM).
 * Call after NV_initsim. Defined in vxd_vesa.c (NV_DETECT builds only). */
int NV_setmodesim(void);

/* Read one BAR0 MMIO register (dword). Read-only; bounds-checked. Returns
 * 0xDEAD00xx sentinels if BAR0 could not be mapped or the offset is out of
 * range. Maps BAR0 once and caches it. */
unsigned long NV_rd32(unsigned long offset);

/* Write one BAR0 MMIO register (dword). Maps BAR0 on first use (shared with
 * NV_rd32), bounds-checked. Returns 1 on success, 0 if BAR0 is unavailable or
 * the offset is out of range. Used only by the E3 EVO bring-up. */
int NV_wr32(unsigned long offset, unsigned long value);

/* BAR1 VRAM access (maps BAR1 once, cached). Bounds-checked. Touches video
 * memory only, never the display raster/PLL, so it is safe for the panel. */
unsigned long NV_vram_rd32(unsigned long offset);

/* M4.3: PFB registers (NV50 family). 0x10020C holds the VRAM size: bits 31:8 =
 * size in bytes (low part), bits 7:0 = bits 39:32 (nouveau nv50_vram.c). */
#define NV_PFB_CFG0            0x100200UL
#define NV_PFB_CFG1            0x100204UL
#define NV_PFB_VRAM_SIZE       0x10020CUL

/* Read the VRAM size from PFB and validate the layout in nv50_evo.h against it
 * (window must stay below the EVO reservation, card must hold NVFB_MIN_VRAM).
 * MUST run in live VM context (BAR0 access inside Device_Init freezes the G86);
 * idempotent, result cached in nv_hwinfo.vram_check. Returns 1 when OK. */
int NV_vram_check(void);
/* Copy the cached VRAM/layout fields into an nv_hwinfo_t (NV_probe zeroes them). */
void NV_vram_export(nv_hwinfo_t *out);
/* Window size chosen at Device_Init (bytes), recorded for the report. */
extern unsigned long nv_fb_window;
/* Fill `count` dwords at VRAM byte `offset` with `value`. Returns dwords written. */
unsigned long NV_vram_fill(unsigned long offset, unsigned long count, unsigned long value);
/* Write one dword to VRAM byte `offset`. Returns 1 on success, 0 if out of range. */
int NV_vram_wr32(unsigned long offset, unsigned long value);
unsigned long NV_vram_write(unsigned long offset, unsigned long count, const unsigned long *src);

#endif /* __VXD_NV_H__ */
