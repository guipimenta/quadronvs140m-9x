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
#define NV_BUILD            3010UL   /* M3 E3 r10: r9 + supervisor (unk10/20/40) handshake */

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
} nv_hwinfo_t;

/* Probe the PCI bus, fill *out. Returns non-zero if an NVIDIA display
 * controller was found (independent of whether MMIO read succeeded).
 */
int NV_probe(nv_hwinfo_t *out);

/* Last probe result, filled during Device_Init_proc when NV_DETECT is built. */
extern nv_hwinfo_t nv_hwinfo;

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
/* Fill `count` dwords at VRAM byte `offset` with `value`. Returns dwords written. */
unsigned long NV_vram_fill(unsigned long offset, unsigned long count, unsigned long value);
/* Write one dword to VRAM byte `offset`. Returns 1 on success, 0 if out of range. */
int NV_vram_wr32(unsigned long offset, unsigned long value);

#endif /* __VXD_NV_H__ */
