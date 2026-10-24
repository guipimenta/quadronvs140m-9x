/*****************************************************************************
 * vxd_nv.c - NVIDIA G8x hardware detection (Milestone 2)
 *
 * Finds the NVIDIA display controller on the PCI bus, maps its BAR0 MMIO
 * aperture, reads PMC_BOOT_0 and decodes the chipset. This is the first code
 * in the project that touches the real G86; it does not yet drive it.
 *
 * MIT licensed (same as the vmdisp9x base). Reuses the Metalkit PCI layer
 * (vmware/pci.c, MIT) and the VMM _MapPhysToLinear service.
 *****************************************************************************/
#define NV_DETECT

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"

#include "vxd_lib.h"
#include "3d_accel.h"

#include "pci.h"      /* Metalkit PCI config access, shared with the SVGA path */
#include "vxd_nv.h"

#include "code32.h"

#include "vxd_strings.h"

/* Last probe result; also exported for the IOCTL reader in vxd_main.c. */
nv_hwinfo_t nv_hwinfo = {0};

/* Cached linear mapping of BAR0 for register peeks. */
static volatile unsigned long *nv_bar0 = 0;
static unsigned long nv_bar0_bytes = 0;

/* E4: when non-zero, NV_probe does NOT touch BAR0 MMIO (it decodes the chip
 * from the PCI device id instead). A BAR0 read of PMC_BOOT_0 at VDD DEVICEINIT
 * - which runs inside a VMM critical section - hard-freezes the machine on the
 * G86. Device_Init_proc sets this for the duration of early init and clears it
 * afterwards, so the Ring-3 IOCTL/detection path still reads real registers. */
unsigned long nv_probe_skip_mmio = 0;

/*
 * NV_probe --
 *
 *   Scan PCI for the first NVIDIA (0x10DE) display-class device, record its
 *   identity and BARs, then map BAR0 and read PMC_BOOT_0 to decode the chip.
 *
 *   Returns 1 if an NVIDIA display device was found (even if the MMIO read
 *   did not succeed), 0 otherwise.
 */
int NV_probe(nv_hwinfo_t *out)
{
	PCIScanState st;
	PCIAddress target;
	PCIAddress a0;
	int have_target = 0;
	int have_display = 0;

	memset(out, 0, sizeof(nv_hwinfo_t));
	memset(&st, 0, sizeof(st));
	memset(&target, 0, sizeof(target));

	out->build = NV_BUILD;

	/* Sanity probe: the host bridge at 0:0.0. If the config mechanism reaches
	 * hardware at all, this reads as the Intel GMCH (low word 0x8086), not
	 * 0xFFFFFFFF. */
	a0.bus = 0; a0.device = 0; a0.function = 0; a0.padding = 0;
	out->host0 = PCI_ConfigRead32(&a0, 0);

	while(PCI_ScanBus(&st))
	{
		out->scan_devices++;

		if(st.vendorId == (uint16)NV_PCI_VENDOR)
		{
			/* PCI config byte 0x0B is the base class code. */
			uint8 baseclass = PCI_ConfigRead8(&st.addr, 11);

			out->nv_devices++;
			if(out->nv_devices == 1)
			{
				out->first_nv_devid = st.deviceId;
				out->first_nv_class = PCI_ConfigRead32(&st.addr, 8);
				out->first_nv_bdf   = ((unsigned long)st.addr.bus << 16)
				                    | ((unsigned long)st.addr.device << 8)
				                    |  (unsigned long)st.addr.function;
			}

			/* Prefer a display-class NVIDIA device; otherwise accept the
			 * first NVIDIA function we see (the GPU is the only 0x10DE part). */
			if(!have_display)
			{
				target        = st.addr;
				out->device_id = st.deviceId;
				have_target   = 1;
				if(baseclass == PCI_CLASS_DISPLAY)
					have_display = 1;
			}
		}
	}

	if(!have_target)
	{
		dbg_printf("NV_probe: no 0x10DE device; scanned %lu, host0=%lX\n",
			out->scan_devices, out->host0);
		return 0;
	}

	out->found     = 1;
	out->vendor_id = NV_PCI_VENDOR;
	out->pci_bus   = target.bus;
	out->pci_dev   = target.device;
	out->pci_fn    = target.function;
	out->subsys    = PCI_GetSubsystem(&target);

	/* Make sure memory space is enabled before touching MMIO. */
	PCI_SetMemEnable(&target, TRUE);

	/* BAR0 = MMIO registers. BAR1 = VRAM aperture (64-bit on NV50/G8x, so
	 * BAR2 holds its high dword; the low dword is enough to report here). */
	out->bar0_phys = PCI_GetBARAddr(&target, 0);
	out->bar0_size = PCI_GetBARSize(&target, 0);
	out->bar1_phys = PCI_GetBARAddr(&target, 1);
	out->bar1_size = PCI_GetBARSize(&target, 1);

	dbg_printf("NV_probe: %X:%X subsys=%lX at %lu:%lu.%lu\n",
		out->vendor_id, out->device_id, out->subsys,
		out->pci_bus, out->pci_dev, out->pci_fn);
	dbg_printf("NV_probe: BAR0=%lX (%lX) BAR1=%lX (%lX)\n",
		out->bar0_phys, out->bar0_size, out->bar1_phys, out->bar1_size);

	if(out->bar0_phys != 0 && out->bar0_size != 0 && !nv_probe_skip_mmio)
	{
		volatile unsigned long *regs;

		/* One page is enough to reach PMC_BOOT_0 at offset 0. */
		regs = (volatile unsigned long *)_MapPhysToLinear(out->bar0_phys, 0x1000, 0);
		if(regs != 0 && regs != (volatile unsigned long *)0xFFFFFFFFUL)
		{
			unsigned long b0 = regs[NV_MMIO_PMC_BOOT_0 / 4];

			out->boot0    = b0;
			out->family   = (b0 >> 24) & 0x1F;
			out->chipset  = (b0 >> 20) & 0xFF;
			out->revision = b0 & 0xFF;
			out->mmio_ok  = (b0 != 0 && b0 != 0xFFFFFFFFUL) ? 1 : 0;

			dbg_printf("NV_probe: BOOT_0=%lX chipset=%lX family=%lX rev=%lX\n",
				out->boot0, out->chipset, out->family, out->revision);
		}
		else
		{
			dbg_printf("NV_probe: BAR0 map failed\n");
		}
	}

	/* No usable MMIO identity (skipped at Device_Init, or the read failed):
	 * decode the chip from the PCI device id. The G86 family (GeForce 8400/8500
	 * /8600, Quadro NVS 110M/135M/140M) is device ids 0x0420-0x042F; the NVS
	 * 140M in this T61 is 0x0429. This is enough for the native backend, which
	 * only needs chipset==0x86 and bar1_phys (both known without MMIO). */
	if(!out->mmio_ok)
	{
		if((out->device_id & 0xFFF0) == 0x0420)
		{
			out->chipset = 0x86;
			out->family  = 0x05;   /* NV50 / Tesla */
			out->mmio_ok = 1;      /* identity known; real MMIO deferred */
			dbg_printf("NV_probe: devid %X -> chipset 86 (MMIO deferred)\n",
				out->device_id);
		}
	}

	return 1;
}

/*
 * NV_rd32 --
 *
 *   Read one BAR0 MMIO register (read-only), mapping BAR0 on first use and
 *   caching it. Bounds-checked against the reported BAR0 size. Returns
 *   0xDEAD00xx if the aperture is unavailable or the offset is out of range.
 */
unsigned long NV_rd32(unsigned long offset)
{
	if(nv_bar0 == 0)
	{
		if(!nv_hwinfo.found)
		{
			if(!NV_probe(&nv_hwinfo))
				return 0xDEAD0000UL;
		}
		if(nv_hwinfo.bar0_phys == 0 || nv_hwinfo.bar0_size == 0)
			return 0xDEAD0002UL;

		nv_bar0 = (volatile unsigned long *)
			_MapPhysToLinear(nv_hwinfo.bar0_phys, nv_hwinfo.bar0_size, 0);
		if(nv_bar0 == 0 || nv_bar0 == (volatile unsigned long *)0xFFFFFFFFUL)
		{
			nv_bar0 = 0;
			return 0xDEAD0003UL;
		}
		nv_bar0_bytes = nv_hwinfo.bar0_size;
	}

	offset &= ~3UL;
	if(offset + 4 > nv_bar0_bytes)
		return 0xDEAD0001UL;

	return nv_bar0[offset >> 2];
}

/*
 * NV_wr32 --
 *
 *   Write one BAR0 MMIO register. Ensures BAR0 is mapped (reuses NV_rd32's
 *   mapping path by poking offset 0), then bounds-checks and writes. Only the
 *   E3 EVO bring-up uses this; everything earlier is read-only.
 */
int NV_wr32(unsigned long offset, unsigned long value)
{
	if(nv_bar0 == 0)
		(void)NV_rd32(0);          /* side effect: maps and caches BAR0 */
	if(nv_bar0 == 0)
		return 0;

	offset &= ~3UL;
	if(offset + 4 > nv_bar0_bytes)
		return 0;

	nv_bar0[offset >> 2] = value;
	return 1;
}

/* Cached linear mapping of BAR1 (VRAM aperture) for framebuffer access. */
static volatile unsigned long *nv_bar1 = 0;
static unsigned long nv_bar1_bytes = 0;

static int NV_map_bar1(void)
{
	if(nv_bar1 != 0)
		return 1;
	if(!nv_hwinfo.found)
	{
		if(!NV_probe(&nv_hwinfo))
			return 0;
	}
	if(nv_hwinfo.bar1_phys == 0 || nv_hwinfo.bar1_size == 0)
		return 0;

	nv_bar1 = (volatile unsigned long *)
		_MapPhysToLinear(nv_hwinfo.bar1_phys, nv_hwinfo.bar1_size, 0);
	if(nv_bar1 == 0 || nv_bar1 == (volatile unsigned long *)0xFFFFFFFFUL)
	{
		nv_bar1 = 0;
		return 0;
	}
	nv_bar1_bytes = nv_hwinfo.bar1_size;
	return 1;
}

/* Read one VRAM dword through the BAR1 aperture. */
unsigned long NV_vram_rd32(unsigned long offset)
{
	if(!NV_map_bar1())
		return 0xDEAD1000UL;
	offset &= ~3UL;
	if(offset + 4 > nv_bar1_bytes)
		return 0xDEAD1001UL;
	return nv_bar1[offset >> 2];
}

/* Fill `count` dwords at VRAM byte `offset` with `value`. Memory-only; cannot
 * affect the display raster. Returns the number of dwords actually written. */
unsigned long NV_vram_fill(unsigned long offset, unsigned long count, unsigned long value)
{
	unsigned long i;
	if(!NV_map_bar1())
		return 0;
	offset &= ~3UL;
	if(offset >= nv_bar1_bytes)
		return 0;
	if(offset + count * 4 > nv_bar1_bytes)
		count = (nv_bar1_bytes - offset) >> 2;
	for(i = 0; i < count; i++)
		nv_bar1[(offset >> 2) + i] = value;
	return count;
}

/* Write one dword to VRAM through the BAR1 aperture. Bounds-checked. */
int NV_vram_wr32(unsigned long offset, unsigned long value)
{
	if(!NV_map_bar1())
		return 0;
	offset &= ~3UL;
	if(offset + 4 > nv_bar1_bytes)
		return 0;
	nv_bar1[offset >> 2] = value;
	return 1;
}

/* Write `count` dwords from `src` into VRAM at byte `offset`. Memory-only
 * (BAR1 aperture), bounds-checked. Returns the number of dwords written.
 * M5.3b: the ring-3 libdrm_nouveau shim uses this to flush a buffer object's
 * CPU shadow into VRAM before a submit (the bounce-buffer map model). */
unsigned long NV_vram_write(unsigned long offset, unsigned long count, const unsigned long *src)
{
	unsigned long i;
	if(!NV_map_bar1() || src == 0)
		return 0;
	offset &= ~3UL;
	if(offset >= nv_bar1_bytes)
		return 0;
	if(offset + count * 4 > nv_bar1_bytes)
		count = (nv_bar1_bytes - offset) >> 2;
	for(i = 0; i < count; i++)
		nv_bar1[(offset >> 2) + i] = src[i];
	return count;
}

/*
 * M4.3 native memory setup
 *
 * The window handed to the shadow blitter is mapped at Device_Init, where we
 * must not touch BAR0. So the size of VRAM is read later, in live VM context,
 * and the fixed layout from nv50_evo.h is checked against it there. Anything
 * that fails leaves nv_vram_ok == 0 and NV_native_modeset refuses to run.
 */
#include "nv50_evo.h"

unsigned long nv_fb_window = 0;
static unsigned long nv_vram_size = 0, nv_vram_hi = 0, nv_vram_cfg0 = 0, nv_vram_cfg1 = 0;
static unsigned long nv_vram_check_result = 0;   /* see nv_hwinfo_t.vram_check */

int NV_vram_check(void)
{
	unsigned long v;

	if(nv_vram_check_result != 0)
		return nv_vram_check_result == 1;

	v = NV_rd32(NV_PFB_VRAM_SIZE);
	if(v == 0 || v == 0xFFFFFFFFUL || (v & 0xFFFF0000UL) == 0xDEAD0000UL)
	{
		dbg_printf("NV_vram_check: PFB size read failed (%lX)\n", v);
		nv_vram_check_result = 2;
		return 0;
	}
	nv_vram_size = v & 0xFFFFFF00UL;
	nv_vram_hi   = v & 0xFFUL;
	nv_vram_cfg0 = NV_rd32(NV_PFB_CFG0);
	nv_vram_cfg1 = NV_rd32(NV_PFB_CFG1);

	dbg_printf("NV_vram_check: VRAM %lu MB (PFB %lX, cfg0 %lX cfg1 %lX), BAR1 %lu MB, window %lu MB\n",
		nv_vram_size >> 20, v, nv_vram_cfg0, nv_vram_cfg1,
		nv_hwinfo.bar1_size >> 20, nv_fb_window >> 20);

	if(nv_vram_hi == 0 && nv_vram_size < NVFB_MIN_VRAM)
	{
		dbg_printf("NV_vram_check: too small - EVO structures need %lu MB\n", NVFB_MIN_VRAM >> 20);
		nv_vram_check_result = 3;
		return 0;
	}
	if(nv_fb_window > NVFB_WINDOW_MAX)
	{
		dbg_printf("NV_vram_check: window %lX overlaps the EVO reservation at %lX\n",
			nv_fb_window, NVFB_RESERVED_BASE);
		nv_vram_check_result = 4;
		return 0;
	}
	dbg_printf("NV_vram_check: layout OK (window 0-%lX, EVO %lX-%lX)\n",
		nv_fb_window, NVFB_RESERVED_BASE, NVFB_RESERVED_END);
	nv_vram_check_result = 1;
	return 1;
}

void NV_vram_export(nv_hwinfo_t *out)
{
	out->vram_size  = nv_vram_size;
	out->vram_hi    = nv_vram_hi;
	out->vram_cfg0  = nv_vram_cfg0;
	out->vram_cfg1  = nv_vram_cfg1;
	out->fb_window  = nv_fb_window;
	out->vram_check = nv_vram_check_result;
}
