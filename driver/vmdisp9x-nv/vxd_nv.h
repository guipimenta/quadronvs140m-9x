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
#define NV_BUILD            3051UL   /* M4.1 fix: scanout pitch rounded up to 256 bytes (NV50 linear surface rule) - 800x600 showed garbage with pitch 3200. 3039 = M4.1+M4.2: native modes 640x480/800x600/1024x768 x 8/16/32 (scaler for the lower two, 8 bpp as palette-expanded 32 bpp scanout); INF binds the root VGA devnode too. 3038 = M4.3: native memory setup - PFB VRAM size read in live context (NV_vram_check), layout validated against the EVO reservations before any native modeset, scanout window from constants+BAR1 (24 MB max, was a fixed 12 MB), all reported by nvprobe. 3037 = E4 r27: 16-bit driver linked with explicit FIXED DATA/BSS segment directives and the 2025-08 Open Watcom toolchain (the Oct-2026 wlink left DGROUP moveable -> Win98 KERNEL never called DriverInit: LoadFail 0000 on T61 and in QEMU). 3036 = E4 r26: dbg output also mirrored to I/O port 0xE9 (QEMU -debugcon) from both sides. 3035 = E4 r25: nvmini.drv also appends its dbg lines to C:\\NVMINI.LOG (KERNEL _lopen/_lwrite, capped 600 lines); VxD logs dispatch-table length, first PM16 call, unknown PM16 services, FBHDA_SETUP arrival. 3034 = E4 r24: DBGPRINT on, routed to an in-VxD ring (dbgring.h): VxD lines 'V> ', nvmini.drv lines 'D> ' via OP_NV_DBG_PUTC; read with 'nvprobe log' (OP_NV_DBGLOG). 3033 = E4 r23: Native defaults to 1 (registry Native=0 opts out); INF writes Native=1 and ships nvsetup.reg. 3032 = E4 r22: native setmode arms the blitter (vga_mode/act_mode), register_display_driver tolerates hda==NULL (Native=0), EVO takeover claims nv_owned only after a completed commit; INF adds DEFAULT\DDC=0 (skips the VDD's 173 s V86 EDID probe). 3031 = E4 r21: nvmini.drv probes VDD device ids 1..4 in VDD_REGISTER_DISPLAY_DRIVER_INFO (Win98 VDD keys EBX as a DEVICE id; id 1 = the VGA devnode, served by the global mini-VDD table; a mini-VDD loaded for a non-VGA devnode lives in that device's private table). register_display_driver now logs the id after 0x300. */

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

#endif /* __VXD_NV_H__ */
