/*****************************************************************************
 * nv50_gpu.c - native G86 3D/2D engine bring-up (M5.x, hardware acceleration)
 *
 * M5.1b-step1 (this file, first flashable step): initialise PGRAPH and upload
 * the per-chip context microcode generated in M5.1a (nv50_grctx_g86.h), then
 * read back enough state to confirm the context controller accepted it. This
 * needs no GPU instance-memory VM (that comes with the channel), so it is the
 * smallest safe step that validates the context microcode on real hardware.
 *
 * Opt-in only (nvprobe grinit). Touches PGRAPH (0x400xxx) + PMC, never the EVO
 * display channel (PDISPLAY 0x610xxx), so the desktop keeps running. A PGRAPH
 * fault parks the graphics engine; recover with a cold boot. Derived from
 * nouveau v2.6.36 nv50_graph.c (see docs/HW3D_PLAN.md).
 *****************************************************************************/
#include "winhack.h"
#include "vmm.h"
#include "vxd.h"
#include "vxd_lib.h"
#include "3d_accel.h"
#include "vxd_nv.h"
#include "nv50_disp.h"
#include "nv50_evo.h"
#include "nv50_gpu.h"
#include "nv50_grctx_g86.h"
#include "nv50_grctx_g86_2636.h"
#include "code32.h"

extern nv_hwinfo_t nv_hwinfo;

/* --- registers (BAR0), from nouveau_reg.h / nv50_graph.c ------------------ */
#define PMC_ENABLE              0x00000200UL
#define  PMC_PGRAPH             (1UL << 12)
#define  PMC_PGRAPH_21          (1UL << 21)
#define PGRAPH_INTR             0x00400100UL
#define PGRAPH_INTR_138         0x00400138UL
#define PGRAPH_INTR_EN          0x0040013CUL
#define PGRAPH_DEBUG_3          0x0040008CUL
#define PGRAPH_CTXCTL_UCODE_IDX 0x00400324UL
#define PGRAPH_CTXCTL_UCODE_DAT 0x00400328UL
#define PGRAPH_CTXCTL_CUR       0x0040032CUL
#define  CTXCTL_CUR_LOADED      0x80000000UL
#define PGRAPH_CHANNEL_CTX_PTR  0x00400784UL
#define PGRAPH_STATUS           0x00400700UL
#define NV_UNITS                0x00001540UL
#define PGRAPH_CTXCTL_NEXT      0x00400330UL

/* GPU instance memory + context, physical VRAM byte offsets (NV50 instance
 * addresses are physical, like the EVO display channel - verified: nouveau
 * nouveau_object.c uses im_backing_start). Above the EVO reservations (<=32 MB). */
#define NVGPU_RAMIN   0x03000000UL   /* channel instance block (graph hdr @0x20) */
#define NVGPU_GRCTX   0x03100000UL   /* per-channel graphics context (grctx_size) */
#define NVGPU_VMPD    0x03000200UL   /* page directory @RAMIN+0x200 (only PDE 2   */
                                     /* used, inside the zeroed first 4 KiB)     */
#define NVGPU_RAMHT   0x03001000UL   /* channel RAMHT (4 KiB)  - 3061 layout     */
#define NVGPU_RAMFC   0x03002000UL   /* channel RAMFC (0x100)                    */
#define NVGPU_CACHE   0x03003000UL   /* FIFO cache (4 KiB)                       */
#define NVGPU_VRAMDMA 0x03004000UL   /* physical VRAM ctxdma (pushbuf + M5.1 DST)*/
#define NVGPU_OBJ2D   0x03004100UL   /* 2D engine object instance                */
#define NVGPU_OBJ3D   0x03004200UL   /* M5.4: NV84_3D engine object instance     */
#define NVGPU_VMDMA   0x03006000UL   /* M5.2 VM-target ctxdma (DMA_DST via VM)   */
#define NVGPU_OBJAREA 0x03008000UL   /* M5.3c: instances for Mesa-bound objects  */
#define NVGPU_OBJAREA_END 0x03010000UL /* (32 KiB, channel-relative <1MB)        */
#define NVGPU_VRAMPT  0x03010000UL   /* M5.2 VRAM page table (64 KiB)            */
#define NVGPU_PUSHBUF 0x03200000UL   /* command pushbuffer (data, physical)      */
#define NVGPU_SCRATCH 0x03400000UL   /* 2D fill destination (read back)          */
#define NVGPU_HEAP_BASE 0x03500000UL /* M5.2b VRAM heap (bump allocator) base     */
#define NVGPU_HEAP_END  0x03700000UL /* M5.2b VRAM heap end (2 MiB)               */
#define NVGPU_VM_VRAM_BASE 0x40000000UL  /* VRAM maps into the VM at +1 GiB      */
#define NVGPU_VM_SCRATCH   (NVGPU_VM_VRAM_BASE + NVGPU_SCRATCH)  /* VM addr of scratch */
/* NV50 engine/ctxdma instances are CHANNEL-RELATIVE: offset from the channel
 * RAMIN base (RAMFC 0x98 = ramin>>12), shifted. INST(x) keeps them small. */
#define NVGPU_INST(x) (((x) - NVGPU_RAMIN) >> 4)
/* gpu_2dfill_core flags */
#define NVF_VM     0x1   /* address the fill through the GPU VM (M5.2a)      */
#define NVF_FENCE  0x2   /* 2D-write fence stand-in: GPU writes a sequence (M5.2b) */
#define NVF_3D     0x4   /* M5.4: REAL NV84_3D QUERY_GET fence (the 3D engine runs) */
#define NVGPU_CHID    1UL            /* our channel id (avoid 0/127)             */
#define NV50_2D_CLASS_ 0x0000502dUL
#define NV84_3D_CLASS_ 0x00008297UL

/* PFIFO registers */
#define PFIFO_INTR_0        0x00002100UL
#define PFIFO_INTR_EN_0     0x00002140UL
#define PFIFO_CACHES        0x00002500UL
#define PFIFO_250C          0x0000250cUL
#define PFIFO_CTX_TABLE(c)  (0x00002600UL + (c)*4)
#define PFIFO_CACHE1_PUSH0  0x00003200UL
#define PFIFO_CACHE1_PUSH1  0x00003204UL
#define PFIFO_CACHE1_DMAPUSH 0x00003220UL
#define PFIFO_CACHE1_PULL0  0x00003250UL
#define PFIFO_PLAYLIST_32F4 0x000032f4UL
#define PFIFO_PLAYLIST_32EC 0x000032ecUL
#define PFIFO_USER(c)       (0x00c00000UL + (c)*0x2000UL)
#define PFIFO_USER_PUT(c)   (PFIFO_USER(c) + 0x40UL)
#define PFIFO_USER_GET(c)   (PFIFO_USER(c) + 0x44UL)
#define NVGPU_PLAYLIST 0x03310000UL  /* PFIFO playlist buffer (3092: was 0x03204000 = INSIDE the 1 MiB
                                      * pushbuffer data region, overwritten after 16 KiB of commands) */
#define NVGPU_NOTIFY   0x03320000UL  /* 3092: 4 KiB notifier block for Mesa's NOUVEAU_NOTIFIER_CLASS ctxdma
                                      * (M2MF BUFFER_NOTIFY wrote to VM address 0 before -> PROP/M2MF trap) */

/* append one (header,data...) batch to the pushbuffer; returns new dword index */
static unsigned long gpu_push(unsigned long i, unsigned long method,
                              unsigned long count, const unsigned long *data)
{
	unsigned long k;
	NV_vram_wr32(NVGPU_PUSHBUF + i*4, (count << 18) | (0 << 13) | method); i++;
	for(k = 0; k < count; k++)
	{ NV_vram_wr32(NVGPU_PUSHBUF + i*4, data[k]); i++; }
	return i;
}

/* build a class-0x3d ctxdma (6 dwords) at inst over [base,base+size) in VRAM */
/* M5.2b: a minimal VRAM heap. A bump allocator over NVGPU_HEAP_[BASE..END];
 * enough to hand the winsys buffer objects. (Free-list reuse is a later step.) */
static unsigned long nv_heap_ptr = 0;
void nv_heap_reset(void) { nv_heap_ptr = NVGPU_HEAP_BASE; }
unsigned long nv_heap_alloc(unsigned long size, unsigned long align)
{
	unsigned long a;
	if(nv_heap_ptr < NVGPU_HEAP_BASE) nv_heap_ptr = NVGPU_HEAP_BASE;
	if(align < 16) align = 16;
	a = (nv_heap_ptr + (align - 1)) & ~(align - 1);
	if(a + size > NVGPU_HEAP_END) return 0;          /* out of VRAM heap */
	nv_heap_ptr = a + size;
	return a;                                        /* physical VRAM offset */
}

static void gpu_ctxdma(unsigned long inst, unsigned long base, unsigned long size)
{
	NV_vram_wr32(inst + 0x00, 0x0019003dUL);     /* VRAM, R/W, class 0x3d */
	NV_vram_wr32(inst + 0x04, base + size - 1);
	NV_vram_wr32(inst + 0x08, base);
	NV_vram_wr32(inst + 0x0c, 0);
	NV_vram_wr32(inst + 0x10, 0);
	NV_vram_wr32(inst + 0x14, 0x00010000UL);
}

/* add a RAMHT entry: handle -> object at inst, with engine code (GR=1, SW=0). */
static void gpu_ramht(unsigned long handle, unsigned long inst, unsigned long engine)
{
	unsigned long h = handle, hash = 0, mask = (1UL << 9) - 1UL;  /* 4KiB RAMHT */
	int b;
	for(b = 32; b > 0; b -= 9) { hash ^= (h & mask); h >>= 9; }
	hash <<= 3;
	NV_vram_wr32(NVGPU_RAMHT + hash + 0, handle);
	NV_vram_wr32(NVGPU_RAMHT + hash + 4, (inst >> 4) | (engine << 20));
}


/* tiny local spin */
static unsigned long gpu_wait(unsigned long reg, unsigned long mask,
                              unsigned long want, unsigned long max)
{
	unsigned long g;
	for(g = 0; g < max; g++)
		if((NV_rd32(reg) & mask) == want)
			break;
	return g;
}

/* PGRAPH reset + register init + context-microcode upload.
 * Fills *out with before/after state so nvprobe can report it. */
int NV_gpu_grinit(nv_gpu_grinit_t *out)
{
	unsigned long units, i, pmc;

	memset(out, 0, sizeof(*out));
	out->build = NV_BUILD;

	if(!nv_hwinfo.found || !nv_hwinfo.mmio_ok)
		return 0;
	if((NV_rd32(0) & 0xFFFF0000UL) == 0xDEAD0000UL)   /* BAR0 sanity */
		return 0;

	out->units        = NV_rd32(NV_UNITS);
	out->units_built  = NV50_GRCTX_UNITS;
	out->ctxprog_len  = NV50_CTXPROG_LEN;
	out->grctx_size   = NV50_GRCTX_SIZE;
	out->pre_pmc      = NV_rd32(PMC_ENABLE);
	out->pre_ctxctl   = NV_rd32(PGRAPH_CTXCTL_CUR);
	out->pre_intr     = NV_rd32(PGRAPH_INTR);
	out->pre_status   = NV_rd32(PGRAPH_STATUS);

	/* 1. reset PGRAPH via PMC (nv50_graph_init_reset) */
	pmc = NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, pmc & ~(PMC_PGRAPH | PMC_PGRAPH_21));
	(void)NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, NV_rd32(PMC_ENABLE) | (PMC_PGRAPH | PMC_PGRAPH_21));
	(void)NV_rd32(PMC_ENABLE);
	out->stage = 1;

	/* 2. init_regs__nv (per-unit writes gated by the units mask; G86 chipset<0xa0) */
	units = out->units;
	NV_wr32(0x400804, 0xc0000000UL);
	NV_wr32(0x406800, 0xc0000000UL);
	NV_wr32(0x400c04, 0xc0000000UL);
	NV_wr32(0x401800, 0xc0000000UL);
	NV_wr32(0x405018, 0xc0000000UL);
	NV_wr32(0x402000, 0xc0000000UL);
	for(i = 0; i < 16; i++)
	{
		if(units & (1UL << i))
		{
			NV_wr32(0x408900 + (i << 12), 0xc0000000UL);
			NV_wr32(0x408e08 + (i << 12), 0xc0000000UL);
			NV_wr32(0x408314 + (i << 12), 0xc0000000UL);
		}
	}
	NV_wr32(0x400108, 0xffffffffUL);
	NV_wr32(0x400824, 0x00004000UL);
	NV_wr32(0x400500, 0x00010001UL);
	out->stage = 2;

	/* 3. init_regs */
	NV_wr32(PGRAPH_DEBUG_3, (1UL << 2));   /* HW_CONTEXT_SWITCH_ENABLED */
	NV_wr32(0x402ca8, 0x800UL);
	/* 4. init_intr */
	NV_wr32(PGRAPH_INTR,     0xffffffffUL);
	NV_wr32(PGRAPH_INTR_138, 0xffffffffUL);
	NV_wr32(PGRAPH_INTR_EN,  0xffffffffUL);
	out->stage = 3;

	/* 5. upload the context microcode (nv50_graph_init_ctxctl) */
	NV_wr32(PGRAPH_CTXCTL_UCODE_IDX, 0);
	for(i = 0; i < NV50_CTXPROG_LEN; i++)
		NV_wr32(PGRAPH_CTXCTL_UCODE_DAT, nv50_ctxprog[i]);
	NV_wr32(0x400320, 4);
	NV_wr32(PGRAPH_CTXCTL_CUR, 0);
	NV_wr32(PGRAPH_CHANNEL_CTX_PTR, 0);
	out->stage = 4;

	/* 6. read back the uploaded microcode to confirm the upload path */
	NV_wr32(PGRAPH_CTXCTL_UCODE_IDX, 0);
	for(i = 0; i < 8 && i < NV50_CTXPROG_LEN; i++)
		out->ucode_rb[i] = NV_rd32(PGRAPH_CTXCTL_UCODE_DAT);

	/* 7. final state */
	out->idle_loops = gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 200000UL);
	out->post_pmc     = NV_rd32(PMC_ENABLE);
	out->post_ctxctl  = NV_rd32(PGRAPH_CTXCTL_CUR);
	out->post_intr    = NV_rd32(PGRAPH_INTR);
	out->post_status  = NV_rd32(PGRAPH_STATUS);
	out->stage = 5;
	out->ok = 1;
	dbg_printf("NV_gpu_grinit: units=%lX ucode[0]=%lX rb[0]=%lX ctxctl %lX->%lX intr %lX->%lX status %lX->%lX\n",
		out->units, nv50_ctxprog[0], out->ucode_rb[0], out->pre_ctxctl, out->post_ctxctl,
		out->pre_intr, out->post_intr, out->pre_status, out->post_status);
	return 1;
}

/* 3101: which PGRAPH context generation to load, HKLM\Software\vmdisp9x\vesa\GrCtx,
 * read at every PGRAPH init = every channel creation, so a GL program started after
 * editing the registry gets the new one without a reboot:
 *   0 = nouveau v2.6.36 generation (builds 3091-3099, the proven one) - DEFAULT
 *   1 = the same layout/ctxprog with current nouveau's three differing G86 values
 *   2 = current nouveau generation + the zcull/ctxctl zeroing (build 3100) */
static unsigned long g_grctx_variant = 0;
static const char GRCTX_conf_path[] = "Software\\vmdisp9x\\vesa";

static void grctx_read_variant(void)
{
	DWORD v = 0;
	g_grctx_variant = (RegReadConf(HKEY_LOCAL_MACHINE, (const char*)GRCTX_conf_path, "GrCtx", &v) && v <= 2) ? v : 0;
	dbg_printf("grctx variant %lu (%s)\n", g_grctx_variant,
		g_grctx_variant == 0 ? "nouveau 2.6.36 = 3099" :
		g_grctx_variant == 1 ? "2.6.36 layout + current nouveau values" : "current nouveau + zcull init = 3100");
}

/* zero a channel's grctx and write the selected generation's initial values */
static void grctx_fill(unsigned long grctx)
{
	unsigned long i;
	NV_vram_fill(grctx, NV50_GRCTX_DWORDS, 0);
	if(g_grctx_variant == 2)
	{
		for(i = 0; i < NV50_CTXVALS_CNT; i++)
			NV_vram_wr32(grctx + nv50_ctxvals[i][0] * 4, nv50_ctxvals[i][1]);
	}
	else
	{
		for(i = 0; i < NV50_CTXVALS_2636_CNT; i++)
			NV_vram_wr32(grctx + nv50_ctxvals_2636[i][0] * 4, nv50_ctxvals_2636[i][1]);
		if(g_grctx_variant == 1)
			for(i = 0; i < NV50_CTXVALS_2636_FIX_CNT; i++)
				NV_vram_wr32(grctx + nv50_ctxvals_2636_fix[i][0] * 4, nv50_ctxvals_2636_fix[i][1]);
	}
}

/* Shared: PGRAPH reset + register init + context-microcode upload.
 * Returns 1 on success. Leaves PGRAPH powered and idle. */
static int gpu_pgraph_init(void)
{
	unsigned long units, i, pmc;
	if(!nv_hwinfo.found || !nv_hwinfo.mmio_ok)
		return 0;
	grctx_read_variant();
	if((NV_rd32(0) & 0xFFFF0000UL) == 0xDEAD0000UL)
		return 0;
	pmc = NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, pmc & ~(PMC_PGRAPH | PMC_PGRAPH_21));
	(void)NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, NV_rd32(PMC_ENABLE) | (PMC_PGRAPH | PMC_PGRAPH_21));
	(void)NV_rd32(PMC_ENABLE);
	units = NV_rd32(NV_UNITS);
	NV_wr32(0x400804, 0xc0000000UL); NV_wr32(0x406800, 0xc0000000UL);
	NV_wr32(0x400c04, 0xc0000000UL); NV_wr32(0x401800, 0xc0000000UL);
	NV_wr32(0x405018, 0xc0000000UL); NV_wr32(0x402000, 0xc0000000UL);
	for(i = 0; i < 16; i++)
		if(units & (1UL << i))
		{
			NV_wr32(0x408900 + (i << 12), 0xc0000000UL);
			NV_wr32(0x408e08 + (i << 12), 0xc0000000UL);
			NV_wr32(0x408314 + (i << 12), 0xc0000000UL);
		}
	NV_wr32(0x400108, 0xffffffffUL);
	NV_wr32(0x400824, 0x00004000UL);
	NV_wr32(0x400500, 0x00010001UL);
	NV_wr32(PGRAPH_DEBUG_3, (1UL << 2));
	NV_wr32(0x402ca8, 0x800UL);
	NV_wr32(PGRAPH_INTR,     0xffffffffUL);
	NV_wr32(PGRAPH_INTR_138, 0xffffffffUL);
	NV_wr32(PGRAPH_INTR_EN,  0xffffffffUL);
	NV_wr32(PGRAPH_CTXCTL_UCODE_IDX, 0);
	if(g_grctx_variant == 2)
	{
		for(i = 0; i < NV50_CTXPROG_LEN; i++)
			NV_wr32(PGRAPH_CTXCTL_UCODE_DAT, nv50_ctxprog[i]);
		/* 3100: what current nouveau's nv50_gr_init does after the microcode upload
		 * and v2.6.36 did not: clear the ctxctl scratch registers (0x400824 keeps the
		 * 0x4000 our manual context load was proven with) and zero the eight zcull
		 * region descriptors. 3101: variant 2 only. */
		NV_wr32(0x400828, 0); NV_wr32(0x40082c, 0); NV_wr32(0x400830, 0);
		NV_wr32(0x40032c, 0); NV_wr32(0x400330, 0);
		for(i = 0; i < 8; i++)
		{
			NV_wr32(0x402c20 + i * 0x10, 0); NV_wr32(0x402c24 + i * 0x10, 0);
			NV_wr32(0x402c28 + i * 0x10, 0); NV_wr32(0x402c2c + i * 0x10, 0);
		}
	}
	else
	{
		for(i = 0; i < NV50_CTXPROG_2636_LEN; i++)
			NV_wr32(PGRAPH_CTXCTL_UCODE_DAT, nv50_ctxprog_2636[i]);
	}
	NV_wr32(0x400320, 4);
	NV_wr32(PGRAPH_CTXCTL_CUR, 0);
	NV_wr32(PGRAPH_CHANNEL_CTX_PTR, 0);
	return 1;
}

/* M5.1b-step2a: build a channel instance + per-channel grctx in VRAM, then ask
 * PGRAPH to load that context (nv50_graph_do_load_context). CTXCTL_CUR going to
 * (ramin>>12)|LOADED proves the grctx layout + physical instance addressing +
 * the context microcode all work - the pivotal GPU validation before 2D. */
int NV_gpu_ctxtest(nv_gpu_ctxtest_t *out)
{
	unsigned long ramin = NVGPU_RAMIN;
	unsigned long grctx = NVGPU_GRCTX;
	unsigned long inst12, i, fifo;

	memset(out, 0, sizeof(*out));
	out->build      = NV_BUILD;
	out->ramin      = ramin;
	out->grctx      = grctx;
	out->grctx_size = NV50_GRCTX_SIZE;
	out->ctxvals    = NV50_CTXVALS_CNT;

	if(!gpu_pgraph_init())
		return 0;
	out->stage = 1;

	/* 1. channel RAMIN: zero, then the graph context header at byte 0x20 */
	NV_vram_fill(ramin, 0x1000 / 4, 0);
	NV_vram_wr32(ramin + 0x20 + 0x00, 0x00190002UL);
	NV_vram_wr32(ramin + 0x20 + 0x04, grctx + NV50_GRCTX_SIZE - 1);
	NV_vram_wr32(ramin + 0x20 + 0x08, grctx);
	NV_vram_wr32(ramin + 0x20 + 0x0c, 0);
	NV_vram_wr32(ramin + 0x20 + 0x10, 0);
	NV_vram_wr32(ramin + 0x20 + 0x14, 0x00010000UL);
	out->stage = 2;

	/* 2. per-channel grctx: zero the whole thing, apply the baked values,
	 *    then the channel back-pointer at grctx[0]. */
	grctx_fill(grctx);                               /* 3101: the selected generation */
	inst12 = ramin >> 12;
	NV_vram_wr32(grctx + 0, inst12);
	out->inst12 = inst12;
	out->stage = 3;

	out->pre_ctxctl = NV_rd32(PGRAPH_CTXCTL_CUR);
	out->pre_intr   = NV_rd32(PGRAPH_INTR);

	/* 3. load the context (nv50_graph_do_load_context) */
	fifo = NV_rd32(0x400500);
	NV_wr32(0x400500, fifo & ~1UL);
	NV_wr32(0x400784, inst12);
	NV_wr32(0x400824, NV_rd32(0x400824) | 0x40UL);
	NV_wr32(0x400320, NV_rd32(0x400320) | 0x11UL);
	NV_wr32(0x400040, 0xffffffffUL);
	(void)NV_rd32(0x400040);
	NV_wr32(0x400040, 0);
	NV_wr32(0x400304, NV_rd32(0x400304) | 1UL);
	out->idle_loops = gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 2000000UL);
	NV_wr32(PGRAPH_CTXCTL_CUR, inst12 | (1UL << 31));
	NV_wr32(0x400500, fifo);
	out->stage = 4;

	out->post_ctxctl = NV_rd32(PGRAPH_CTXCTL_CUR);
	out->post_next   = NV_rd32(PGRAPH_CTXCTL_NEXT);
	out->post_intr   = NV_rd32(PGRAPH_INTR);
	out->post_status = NV_rd32(PGRAPH_STATUS);
	out->loaded      = ((out->post_ctxctl & 0x80000000UL) &&
	                    ((out->post_ctxctl & 0x00ffffffUL) == inst12)) ? 1 : 0;
	out->stage = 5;
	out->ok    = 1;
	dbg_printf("NV_gpu_ctxtest: inst12=%lX ctxctl %lX->%lX intr %lX->%lX idle=%lu loaded=%lu\n",
		inst12, out->pre_ctxctl, out->post_ctxctl, out->pre_intr, out->post_intr,
		out->idle_loops, out->loaded);
	return 1;
}

/* M5.1b-step2b: full path - PGRAPH + a PFIFO channel + the 2D engine fill a
 * rectangle in VRAM scratch, read back. The first GPU-executed drawing. */
static int gpu_2dfill_core(nv_gpu_2dfill_t *out, int flags)
{
	const int do_fence = (flags & NVF_FENCE)  ? 1 : 0;
	const int do_3d    = (flags & NVF_3D)     ? 1 : 0;
	/* both fence paths write through the channel VM, so a fence implies use_vm */
	const int use_vm   = (flags & (NVF_VM | NVF_FENCE | NVF_3D)) ? 1 : 0;
	/* both the 2D-write stand-in and the real 3D QUERY need a VM-mapped fence BO */
	const int want_fence_bo = do_fence || do_3d;
	unsigned long fence_addr = 0, fence_vm = 0;
	const unsigned long fence_seq = do_3d ? 0x3d000001UL : 0xcafe0001UL;
	const unsigned long chid   = NVGPU_CHID;
	const unsigned long ramin  = NVGPU_RAMIN;
	const unsigned long grctx  = NVGPU_GRCTX;
	const unsigned long color  = 0x11223344UL;
	const unsigned long pitch  = 1024UL;              /* bytes (256 px * 4)    */
	const unsigned long w = 256UL, h = 256UL;
	unsigned long inst12, i, pushbytes, d[4], pmc, fifo;
	unsigned long dv[16];   /* M5.4: multi-dword method runs (DMA_ZETA/COLOR/RT) */
	unsigned long ecolor, epitch, ew, eh, edst;

	memset(out, 0, sizeof(*out));
	out->build   = NV_BUILD;
	out->scratch = NVGPU_SCRATCH;
	out->color   = color;

	if(!gpu_pgraph_init())
		return 0;
	out->stage = 1;

	/* channel RAMIN: chid, ramfc pointer, graph context header @0x20 */
	NV_vram_fill(ramin, 0x1000 / 4, 0);
	NV_vram_wr32(ramin + 0x00, chid);
	NV_vram_wr32(ramin + 0x04, NVGPU_RAMFC >> 8);
	NV_vram_wr32(ramin + 0x20 + 0x00, 0x00190002UL);
	NV_vram_wr32(ramin + 0x20 + 0x04, grctx + NV50_GRCTX_SIZE - 1);
	NV_vram_wr32(ramin + 0x20 + 0x08, grctx);
	NV_vram_wr32(ramin + 0x20 + 0x14, 0x00010000UL);

	/* per-channel grctx */
	grctx_fill(grctx);                               /* 3101: the selected generation */
	inst12 = ramin >> 12;
	NV_vram_wr32(grctx + 0, inst12);
	out->stage = 2;

	/* instance objects: a ctxdma over all VRAM (pushbuf fetch + DMA_DST),
	 * the FIFO cache, and the 2D engine object. */
	gpu_ctxdma(NVGPU_VRAMDMA, 0, 0x08000000UL);       /* 128 MiB VRAM (phys)   */
	if(use_vm)
	{
		unsigned long pg;
		/* VM-target ctxdma spanning the whole VM address space */
		NV_vram_wr32(NVGPU_VMDMA + 0x00, 0x7fc00000UL | 0x3dUL);
		NV_vram_wr32(NVGPU_VMDMA + 0x04, 0xffffffffUL);   /* limit low  */
		NV_vram_wr32(NVGPU_VMDMA + 0x08, 0);              /* base  low  */
		NV_vram_wr32(NVGPU_VMDMA + 0x0c, 0);
		NV_vram_wr32(NVGPU_VMDMA + 0x10, 0);
		NV_vram_wr32(NVGPU_VMDMA + 0x14, 0x00080000UL);   /* flags5 (VM) */
		/* VRAM page table: zero it, then map the 4 scratch pages 1:1 into
		 * the VM at (1 GiB + physical). PTE_lo = phys|present, PTE_hi = 0. */
		NV_vram_fill(NVGPU_VRAMPT, 0x10000 / 4, 0);
		for(pg = 0; pg < 4; pg++)
		{
			unsigned long phys = NVGPU_SCRATCH + pg * 0x10000UL;
			unsigned long idx  = ((NVGPU_VM_SCRATCH + pg * 0x10000UL - NVGPU_VM_VRAM_BASE) >> 16) << 1;
			NV_vram_wr32(NVGPU_VRAMPT + idx * 4 + 0, phys | 1UL);
			NV_vram_wr32(NVGPU_VRAMPT + idx * 4 + 4, 0);
		}
		/* channel page directory at RAMIN+0x200. Only PDE 2 (byte 0x10,
		 * VM 1 GiB) is needed; it maps to our VRAM page table. The rest of
		 * the PD lives in the already-zeroed first 4 KiB (invalid PDEs). */
		NV_vram_wr32(NVGPU_VMPD + 0x10, NVGPU_VRAMPT | 0x61UL);
		NV_vram_wr32(NVGPU_VMPD + 0x14, 0);
	}
	if(want_fence_bo)
	{
		/* allocate a fence BO from the VRAM heap, map its page into the VM
		 * (so the 3D engine can reach it), and pre-fill it with 0. The 3D
		 * QUERY_GET will write fence_seq there through the channel VM. */
		unsigned long fidx;
		nv_heap_reset();
		fence_addr = nv_heap_alloc(0x1000, 0x1000);       /* physical VRAM     */
		fence_vm   = NVGPU_VM_VRAM_BASE + fence_addr;      /* its VM address    */
		out->heap_addr = fence_addr;
		fidx = ((fence_vm - NVGPU_VM_VRAM_BASE) >> 16) << 1;
		NV_vram_wr32(NVGPU_VRAMPT + fidx * 4 + 0, fence_addr | 1UL);
		NV_vram_wr32(NVGPU_VRAMPT + fidx * 4 + 4, 0);
		NV_vram_wr32(fence_addr, 0);
	}
	NV_vram_fill(NVGPU_CACHE, 0x1000 / 4, 0);
	NV_vram_wr32(NVGPU_OBJ2D + 0,  NV50_2D_CLASS_);   /* 0x502d                */
	NV_vram_wr32(NVGPU_OBJ2D + 20, 0x00010000UL);     /* dword 5               */
	if(do_3d)
	{
		/* M5.4: the NV84_3D engine object. Same instance layout as 2D:
		 * dword0 = class, dword5 = 0x00010000 (engine = PGRAPH). */
		NV_vram_wr32(NVGPU_OBJ3D + 0,  NV84_3D_CLASS_);  /* 0x8297             */
		NV_vram_wr32(NVGPU_OBJ3D + 20, 0x00010000UL);
	}

	/* RAMHT: 2D object (graphics engine 1), VRAM ctxdma (SW engine 0) */
	NV_vram_fill(NVGPU_RAMHT, 0x1000 / 4, 0);
	gpu_ramht(0xbeef502dUL, NVGPU_OBJ2D   - NVGPU_RAMIN, 1);
	gpu_ramht(0xbeef0002UL, NVGPU_VRAMDMA - NVGPU_RAMIN, 0);
	if(use_vm)
		gpu_ramht(0xbeef0003UL, NVGPU_VMDMA - NVGPU_RAMIN, 0);
	if(do_3d)
		gpu_ramht(0xbeef8297UL, NVGPU_OBJ3D - NVGPU_RAMIN, 1);  /* 3D engine */

	/* RAMFC (G84/G86 layout) */
	NV_vram_fill(NVGPU_RAMFC, 0x100 / 4, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x48, (NVGPU_VRAMDMA - NVGPU_RAMIN) >> 4);
	NV_vram_wr32(NVGPU_RAMFC + 0x80, (0UL << 27) | (4UL << 24) | ((NVGPU_RAMHT - NVGPU_RAMIN) >> 4));
	NV_vram_wr32(NVGPU_RAMFC + 0x44, 0x2101ffffUL);
	NV_vram_wr32(NVGPU_RAMFC + 0x60, 0x7fffffffUL);
	NV_vram_wr32(NVGPU_RAMFC + 0x40, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x08, NVGPU_PUSHBUF);   /* DMA_PUT init (->0x3240) */
	NV_vram_wr32(NVGPU_RAMFC + 0x10, NVGPU_PUSHBUF);   /* DMA_GET init (->0x3244) */
	NV_vram_wr32(NVGPU_RAMFC + 0x7c, 0x30000001UL);
	NV_vram_wr32(NVGPU_RAMFC + 0x78, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x3c, 0x403f6078UL);
	NV_vram_wr32(NVGPU_RAMFC + 0x50, NVGPU_PUSHBUF);  /* pushbuf_base          */
	NV_vram_wr32(NVGPU_RAMFC + 0x54, 0);              /* no indirect buffer    */
	NV_vram_wr32(NVGPU_RAMFC + 0x88, NVGPU_CACHE >> 10);
	NV_vram_wr32(NVGPU_RAMFC + 0x98, ramin >> 12);
	out->stage = 3;

	/* PFIFO init: reset (PMC bit8), interrupts, clear the context table */
	pmc = NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, pmc & ~(1UL << 8));
	(void)NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, NV_rd32(PMC_ENABLE) | (1UL << 8));
	NV_wr32(PFIFO_INTR_0,    0xffffffffUL);
	NV_wr32(PFIFO_INTR_EN_0, 0xffffffffUL);
	for(i = 0; i < 128; i++)
		NV_wr32(PFIFO_CTX_TABLE(i), 0);
	NV_wr32(PFIFO_250C, 0x6f3cfc34UL);
	NV_wr32(PFIFO_CACHES, 0);
	NV_wr32(0x3250, 0); NV_wr32(0x3220, 0); NV_wr32(0x3204, 0);
	NV_wr32(0x3210, 0); NV_wr32(0x3270, 0);
	out->stage = 4;

	/* load our graph context so PGRAPH holds it */
	fifo = NV_rd32(0x400500);
	NV_wr32(0x400500, fifo & ~1UL);
	NV_wr32(0x400784, inst12);
	NV_wr32(0x400824, NV_rd32(0x400824) | 0x40UL);
	NV_wr32(0x400320, NV_rd32(0x400320) | 0x11UL);
	NV_wr32(0x400040, 0xffffffffUL); (void)NV_rd32(0x400040); NV_wr32(0x400040, 0);
	NV_wr32(0x400304, NV_rd32(0x400304) | 1UL);
	(void)gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 2000000UL);
	NV_wr32(PGRAPH_CTXCTL_CUR, inst12 | (1UL << 31));
	NV_wr32(0x400500, fifo);
	out->ctxctl = NV_rd32(PGRAPH_CTXCTL_CUR);

	/* enable the FIFO puller (nv04_fifo_enable), then our channel, then the
	 * runlist - which ends with CACHES=0x101 (nv50_fifo_playlist_update). The
	 * nv50 scheduler loads the channel; no manual CACHE1 poking. */
	NV_wr32(PFIFO_CACHE1_PUSH0, 1);
	NV_wr32(PFIFO_CACHE1_PULL0, 1);
	NV_wr32(PFIFO_CTX_TABLE(chid), (NVGPU_RAMFC >> 8) | (1UL << 31));
	NV_vram_wr32(NVGPU_PLAYLIST + 0, chid);
	NV_wr32(PFIFO_PLAYLIST_32F4, NVGPU_PLAYLIST >> 12);
	NV_wr32(PFIFO_PLAYLIST_32EC, 1);
	NV_wr32(PFIFO_CACHES, 0x101);
	out->stage = 5;

	/* The "fence" is a 2D write of a sequence value to the heap BO through the
	 * VM. 2D and 3D share the unified PGRAPH and the FIFO is in-order, so a 2D
	 * write ordered after a batch signals that batch's completion - a valid
	 * fence. (nouveau's native 3D QUERY fence needs full 3D-engine setup, which
	 * TRAPs until M5.4; this 2D fence works on the path we have proven.) */
	ecolor = do_fence ? fence_seq : color;
	epitch = do_fence ? 64UL      : pitch;        /* fits the 64 KiB fence page */
	ew     = do_fence ? 4UL       : w;
	eh     = do_fence ? 1UL       : h;
	edst   = do_fence ? fence_vm  : (use_vm ? NVGPU_VM_SCRATCH : NVGPU_SCRATCH);

	i = 0;
	if(do_3d)
	{
		/* M5.4: bind the REAL NV84_3D engine, do the BO-FREE PREFIX of
		 * nv50_screen_init_hwctx (DMA_NOTIFY/ZETA/COLOR + the ROP/raster enables,
		 * all pointing DMA at the VM ctxdma 0xbeef0003), THEN emit the QUERY_GET
		 * write-fence. A bare QUERY (build 3077) TRAPped (PGRAPH_INTR 0x00200000)
		 * because the CROP unit the fence waits on had no valid DMA_COLOR/ZETA or
		 * RT state. This prefix is exactly what nouveau runs before its first
		 * fence; everything here uses only the VRAM ctxdma + constants (no BOs -
		 * the BO-backed VP/FP/TLS/CB setup comes later with real allocations).
		 *   QUERY_GET value = WRITE_UNK0 | UNK4(0x10) | UNIT_CROP(0xf000) |
		 *                     TYPE_QUERY(0) | QUERY_SELECT_ZERO(0) | SHORT(0x10000000)
		 *                   = 0x1000f010  (the nouveau "short" write-fence). */
		for(i = 0; i < 16; i++) dv[i] = 0xbeef0003UL;   /* VM ctxdma handle run  */
		i = 0;
		d[0] = 0xbeef8297UL;             i = gpu_push(i, 0x0000, 1, d);  /* SET_OBJECT 3D      */
		d[0] = 1;                        i = gpu_push(i, 0x1558, 1, d);  /* COND_MODE ALWAYS   */
		d[0] = 0xbeef0003UL;             i = gpu_push(i, 0x0180, 1, d);  /* DMA_NOTIFY         */
		                                 i = gpu_push(i, 0x0184, 11, dv);/* DMA_ZETA (11)      */
		                                 i = gpu_push(i, 0x01c0, 8,  dv);/* DMA_COLOR(0) (8)   */
		d[0] = 2;                        i = gpu_push(i, 0x19a0, 1, d);  /* REG_MODE STRIPED   */
		d[0] = 0xf;                      i = gpu_push(i, 0x1400, 1, d);  /* UNK1400_LANES      */
		d[0] = 0x18;                     i = gpu_push(i, 0x0de4, 1, d);  /* WATCHDOG_TIMER     */
		d[0] = 0;                        i = gpu_push(i, 0x19cc, 1, d);  /* ZETA_COMP_ENABLE   */
		dv[0]=dv[1]=dv[2]=dv[3]=dv[4]=dv[5]=dv[6]=dv[7]=0;
		                                 i = gpu_push(i, 0x19e0, 8,  dv);/* RT_COMP_ENABLE (8) */
		d[0] = 1;                        i = gpu_push(i, 0x121c, 1, d);  /* RT_CONTROL = 1     */
		d[0] = 0;                        i = gpu_push(i, 0x15b4, 1, d);  /* CSAA_ENABLE        */
		d[0] = 0;                        i = gpu_push(i, 0x1534, 1, d);  /* MULTISAMPLE_ENABLE */
		d[0] = 0;                        i = gpu_push(i, 0x15d0, 1, d);  /* MULTISAMPLE_MODE MS1 */
		d[0] = 0;                        i = gpu_push(i, 0x153c, 1, d);  /* MULTISAMPLE_CTRL   */
		d[0] = 1;                        i = gpu_push(i, 0x0de8, 1, d);  /* PRIM_RESTART_W_DA  */
		d[0] = 1;                        i = gpu_push(i, 0x133c, 1, d);  /* BLEND_SEPARATE_ALPHA */
		d[0] = 0;                        i = gpu_push(i, 0x13ac, 1, d);  /* SCREEN_Y_CONTROL   */
		d[0] = 0; d[1] = 0;              i = gpu_push(i, 0x0df8, 2, d);  /* WINDOW_OFFSET_X/Y  */
		d[0] = 0x3f;                     i = gpu_push(i, 0x1590, 1, d);  /* ZCULL_REGION       */
		d[0] = 0;                                 /* QUERY_ADDRESS_HIGH (<4GiB)   */
		d[1] = (unsigned long)fence_vm;           /* QUERY_ADDRESS_LOW            */
		d[2] = fence_seq;                         /* QUERY_SEQUENCE               */
		d[3] = 0x1000f010UL;                      /* QUERY_GET (short write-fence)*/
		                                 i = gpu_push(i, 0x1b00, 4, d);  /* QUERY_* (4 dwords) */
	}
	else
	{
	/* method stream: bind 2D, set destination surface, fill the rectangle */
	d[0] = 0xbeef502dUL;             i = gpu_push(i, 0x0000, 1, d);  /* SET_OBJECT */
	d[0] = use_vm ? 0xbeef0003UL : 0xbeef0002UL; i = gpu_push(i, 0x0184, 1, d); /* DMA_DST */
	d[0] = 0xcfUL;                   i = gpu_push(i, 0x0200, 1, d);  /* DST_FORMAT */
	d[0] = 1;                        i = gpu_push(i, 0x0204, 1, d);  /* DST_LINEAR */
	d[0] = epitch;                   i = gpu_push(i, 0x0214, 1, d);  /* DST_PITCH  */
	d[0] = ew;                       i = gpu_push(i, 0x0218, 1, d);  /* DST_WIDTH  */
	d[0] = eh;                       i = gpu_push(i, 0x021c, 1, d);  /* DST_HEIGHT */
	d[0] = 0; d[1] = edst;           i = gpu_push(i, 0x0220, 2, d);  /* DST_ADDR hi,lo */
	d[0] = 0;                        i = gpu_push(i, 0x0290, 1, d);  /* CLIP_ENABLE=0 */
	d[0] = 3;                        i = gpu_push(i, 0x02ac, 1, d);  /* OPERATION SRCCOPY */
	d[0] = 4;                        i = gpu_push(i, 0x0580, 1, d);  /* DRAW_SHAPE RECT */
	d[0] = 0xcfUL;                   i = gpu_push(i, 0x0584, 1, d);  /* DRAW_COLOR_FORMAT */
	d[0] = ecolor;                   i = gpu_push(i, 0x0588, 1, d);  /* DRAW_COLOR */
	d[0] = 0; d[1] = 0; d[2] = ew; d[3] = eh;
	                                 i = gpu_push(i, 0x0600, 4, d);  /* POINT32 (0,0)-(w,h) */
	}
	pushbytes = i * 4;
	out->put_bytes = pushbytes;

	/* clear scratch to a different value so a successful fill is visible */
	if(!want_fence_bo)
	{
		NV_vram_fill(NVGPU_SCRATCH, (w * h), 0x99999999UL);
		out->pre_px[0] = NV_vram_rd32(NVGPU_SCRATCH + 0);
	}

	/* kick: GET starts at pushbuf_base, advance PUT past the stream */
	NV_wr32(PFIFO_INTR_0, 0xffffffffUL);
	NV_wr32(PFIFO_USER_PUT(chid), NVGPU_PUSHBUF + pushbytes);
	out->idle_loops = 0;
	for(i = 0; i < 2000000UL; i++)
	{
		out->idle_loops++;
		if(NV_rd32(PFIFO_USER_GET(chid)) == (NVGPU_PUSHBUF + pushbytes))
			break;
		if(NV_rd32(PFIFO_INTR_0) || NV_rd32(PGRAPH_INTR))
			break;
	}
	/* the pusher drained the commands; now wait for the 2D engine itself to
	 * finish rendering before reading the result (it is asynchronous). */
	out->gr_idle = gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 4000000UL);
	out->stage = 6;

	out->get        = NV_rd32(PFIFO_USER_GET(chid));
	out->put        = NV_rd32(PFIFO_USER_PUT(chid));
	out->c1_get     = NV_rd32(0x3244);      /* live CACHE1 DMA GET  */
	out->c1_put     = NV_rd32(0x3240);      /* live CACHE1 DMA PUT  */
	out->c1_push1   = NV_rd32(0x3204);      /* current channel id   */
	out->c1_dmast   = NV_rd32(0x3228);      /* CACHE1 DMA STATE     */
	out->caches     = NV_rd32(PFIFO_CACHES);
	out->ctxtable   = NV_rd32(PFIFO_CTX_TABLE(chid));
	out->dma_state  = NV_rd32(0x3228);   /* pusher state; (>>29)&7 = err: 6=MEM_FAULT */
	out->dma_push   = NV_rd32(0x3220);
	out->dma_fetch  = NV_rd32(0x3224);
	out->dma_inst   = NV_rd32(0x322c);
	out->punk1700   = NV_rd32(0x1700);   /* PRAMIN window */
	out->ctxdma_rb  = NV_vram_rd32(NVGPU_VRAMDMA + 0);   /* ctxdma dword0 as written */
	out->push_rb0   = NV_vram_rd32(NVGPU_PUSHBUF + 0);   /* 1st method header       */
	out->push_rb1   = NV_vram_rd32(NVGPU_PUSHBUF + 4);   /* 1st data (handle)       */
	out->pfifo_intr = NV_rd32(PFIFO_INTR_0);
	out->pgraph_intr= NV_rd32(PGRAPH_INTR);
	out->pgraph_stat= NV_rd32(PGRAPH_STATUS);
	/* M5.4: on a PGRAPH TRAP (PGRAPH_INTR 0x00200000) the per-unit trap status
	 * is at 0x400108 and the DISPATCH unit latches the faulting method/subc/data
	 * at 0x400808/0x40080c/0x400810 - capture them so a trap is diagnosable. */
	out->trap_status  = NV_rd32(0x400108);
	out->trap_regs[0] = NV_rd32(0x400808);  /* DISPATCH: bit31 valid, mthd/subc  */
	out->trap_regs[1] = NV_rd32(0x40080c);  /* DISPATCH: data                    */
	out->trap_regs[2] = NV_rd32(0x400810);  /* DISPATCH: more                    */
	out->trap_regs[3] = NV_rd32(0x400c04);  /* VFETCH unit trap                  */
	out->trap_regs[4] = NV_rd32(0x405018);  /* CCACHE unit trap                  */
	/* read back the fill */
	out->px[0] = NV_vram_rd32(NVGPU_SCRATCH + 0);
	out->px[1] = NV_vram_rd32(NVGPU_SCRATCH + (w/2)*4 + (h/2)*pitch);
	out->px[2] = NV_vram_rd32(NVGPU_SCRATCH + (w-1)*4 + (h-1)*pitch);
	out->filled = (out->px[0] == color && out->px[1] == color && out->px[2] == color) ? 1 : 0;
	if(want_fence_bo)
	{
		out->notify[0] = NV_vram_rd32(fence_addr + 0);  /* the value the GPU wrote */
		out->notify[1] = fence_seq;                     /* the value we asked for  */
		out->notify[2] = (unsigned long)fence_vm;       /* VM address used         */
		out->notify[3] = 0;
		out->notified  = (out->notify[0] == fence_seq) ? 1 : 0;
	}
	out->ok    = 1;
	dbg_printf("NV_gpu_2dfill: ctxctl=%lX get=%lX put=%lX pfi=%lX pgi=%lX px=%lX,%lX,%lX filled=%lu\n",
		out->ctxctl, out->get, out->put, out->pfifo_intr, out->pgraph_intr,
		out->px[0], out->px[1], out->px[2], out->filled);
	return 1;
}


/* ===================== M5.3: persistent channel + ring-3 submit ===============
 * The winsys needs ONE long-lived channel that ring-3 (Mesa) submits command
 * streams to, repeatedly. nv_chan_init() brings the channel up once (VM enabled,
 * a 1:1 map of the first 128 MiB of VRAM into the VM at +1 GiB, both ctxdmas and
 * the 2D object bound). NV_gpu_submit() appends a ring-3-built method stream to
 * the pushbuffer and kicks the scheduler. This is the same proven bring-up as
 * gpu_2dfill_core, kept separate so the verified gpu2d/vmtest/fence tests are
 * untouched. */
static int           g_chan_ready = 0;
static unsigned long g_chan_put   = 0;    /* next free byte offset in the command region */
static unsigned long g_ib_put     = 0;    /* next free IB ring entry index */
static unsigned long g_drain_off    = 0;  /* 3098: pusher/cache drain latched off after a timeout */
static unsigned long g_drain_logged = 0;
static unsigned long g_nsub       = 0;    /* 3095: submits so far (log gating) */
static int           nsub_verbose = 0;
#define NVGPU_PUSHBUF_SIZE 0x00100000UL   /* 1 MiB command data region            */
#define NVGPU_IB          0x03300000UL    /* IB ring (indirect-buffer entries)     */
#define NVGPU_IB_ENTRIES  256UL           /* must be a power of two                 */
#define NVGPU_IB_ORDER    8UL             /* log2(NVGPU_IB_ENTRIES)                 */
#define PFIFO_USER_IBPUT(c) (PFIFO_USER(c) + 0x8cUL)  /* nv50 IB put index         */
#define PFIFO_USER_IBGET(c) (PFIFO_USER(c) + 0x88UL)  /* nv50 IB get index         */

static unsigned long nv_chan_init(void)
{
	const unsigned long chid = NVGPU_CHID, ramin = NVGPU_RAMIN, grctx = NVGPU_GRCTX;
	unsigned long inst12, i, pmc, fifo, ctxctl;

	if(!gpu_pgraph_init())
		return 0;

	/* channel RAMIN: chid, ramfc pointer, graph context header @0x20 */
	NV_vram_fill(ramin, 0x1000 / 4, 0);
	NV_vram_wr32(ramin + 0x00, chid);
	NV_vram_wr32(ramin + 0x04, NVGPU_RAMFC >> 8);
	NV_vram_wr32(ramin + 0x20 + 0x00, 0x00190002UL);
	NV_vram_wr32(ramin + 0x20 + 0x04, grctx + NV50_GRCTX_SIZE - 1);
	NV_vram_wr32(ramin + 0x20 + 0x08, grctx);
	NV_vram_wr32(ramin + 0x20 + 0x14, 0x00010000UL);

	/* per-channel grctx */
	grctx_fill(grctx);                               /* 3101: the selected generation */
	inst12 = ramin >> 12;
	NV_vram_wr32(grctx + 0, inst12);

	/* ctxdmas: physical VRAM (pushbuf fetch + physical DST) and VM-target */
	gpu_ctxdma(NVGPU_VRAMDMA, 0, 0x08000000UL);
	NV_vram_wr32(NVGPU_VMDMA + 0x00, 0x7fc00000UL | 0x3dUL);
	NV_vram_wr32(NVGPU_VMDMA + 0x04, 0xffffffffUL);
	NV_vram_wr32(NVGPU_VMDMA + 0x08, 0);
	NV_vram_wr32(NVGPU_VMDMA + 0x0c, 0);
	NV_vram_wr32(NVGPU_VMDMA + 0x10, 0);
	NV_vram_wr32(NVGPU_VMDMA + 0x14, 0x00080000UL);

	/* VRAM page table: 1:1 map the first 128 MiB of VRAM into the VM at +1 GiB
	 * (2048 x 64 KiB pages), so any physical offset P has VM address 1GiB+P. */
	NV_vram_fill(NVGPU_VRAMPT, 0x10000 / 4, 0);
	for(i = 0; i < 2048; i++)
	{
		NV_vram_wr32(NVGPU_VRAMPT + i * 8 + 0, (i * 0x10000UL) | 1UL);
		NV_vram_wr32(NVGPU_VRAMPT + i * 8 + 4, 0);
	}
	NV_vram_wr32(NVGPU_VMPD + 0x10, NVGPU_VRAMPT | 0x61UL);   /* PDE 2 -> the PT */
	NV_vram_wr32(NVGPU_VMPD + 0x14, 0);

	NV_vram_fill(NVGPU_CACHE, 0x1000 / 4, 0);
	NV_vram_wr32(NVGPU_OBJ2D + 0,  NV50_2D_CLASS_);
	NV_vram_wr32(NVGPU_OBJ2D + 20, 0x00010000UL);

	NV_vram_fill(NVGPU_RAMHT, 0x1000 / 4, 0);
	gpu_ramht(0xbeef502dUL, NVGPU_OBJ2D   - NVGPU_RAMIN, 1);
	gpu_ramht(0xbeef0002UL, NVGPU_VRAMDMA - NVGPU_RAMIN, 0);
	gpu_ramht(0xbeef0003UL, NVGPU_VMDMA   - NVGPU_RAMIN, 0);

	NV_vram_fill(NVGPU_RAMFC, 0x100 / 4, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x48, (NVGPU_VRAMDMA - NVGPU_RAMIN) >> 4);
	NV_vram_wr32(NVGPU_RAMFC + 0x80, (0UL << 27) | (4UL << 24) | ((NVGPU_RAMHT - NVGPU_RAMIN) >> 4));
	NV_vram_wr32(NVGPU_RAMFC + 0x44, 0x2101ffffUL);
	NV_vram_wr32(NVGPU_RAMFC + 0x60, 0x7fffffffUL);
	NV_vram_wr32(NVGPU_RAMFC + 0x40, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x08, NVGPU_PUSHBUF);
	NV_vram_wr32(NVGPU_RAMFC + 0x10, NVGPU_PUSHBUF);
	NV_vram_wr32(NVGPU_RAMFC + 0x7c, 0x30000001UL);
	NV_vram_wr32(NVGPU_RAMFC + 0x78, 0);
	NV_vram_wr32(NVGPU_RAMFC + 0x3c, 0x403f6078UL);
	/* IB (indirect buffer) mode: 0x50 = IB ring base, 0x54 = log2(entries)<<16. */
	NV_vram_wr32(NVGPU_RAMFC + 0x50, NVGPU_IB);
	NV_vram_wr32(NVGPU_RAMFC + 0x54, NVGPU_IB_ORDER << 16);
	NV_vram_wr32(NVGPU_RAMFC + 0x88, NVGPU_CACHE >> 10);
	NV_vram_wr32(NVGPU_RAMFC + 0x98, ramin >> 12);

	pmc = NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, pmc & ~(1UL << 8));
	(void)NV_rd32(PMC_ENABLE);
	NV_wr32(PMC_ENABLE, NV_rd32(PMC_ENABLE) | (1UL << 8));
	NV_wr32(PFIFO_INTR_0,    0xffffffffUL);
	NV_wr32(PFIFO_INTR_EN_0, 0xffffffffUL);
	for(i = 0; i < 128; i++)
		NV_wr32(PFIFO_CTX_TABLE(i), 0);
	NV_wr32(PFIFO_250C, 0x6f3cfc34UL);
	NV_wr32(PFIFO_CACHES, 0);
	NV_wr32(0x3250, 0); NV_wr32(0x3220, 0); NV_wr32(0x3204, 0);
	NV_wr32(0x3210, 0); NV_wr32(0x3270, 0);

	fifo = NV_rd32(0x400500);
	NV_wr32(0x400500, fifo & ~1UL);
	NV_wr32(0x400784, inst12);
	NV_wr32(0x400824, NV_rd32(0x400824) | 0x40UL);
	NV_wr32(0x400320, NV_rd32(0x400320) | 0x11UL);
	NV_wr32(0x400040, 0xffffffffUL); (void)NV_rd32(0x400040); NV_wr32(0x400040, 0);
	NV_wr32(0x400304, NV_rd32(0x400304) | 1UL);
	(void)gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 2000000UL);
	NV_wr32(PGRAPH_CTXCTL_CUR, inst12 | (1UL << 31));
	NV_wr32(0x400500, fifo);
	ctxctl = NV_rd32(PGRAPH_CTXCTL_CUR);

	NV_wr32(PFIFO_CACHE1_PUSH0, 1);
	NV_wr32(PFIFO_CACHE1_PULL0, 1);
	NV_wr32(PFIFO_CTX_TABLE(chid), (NVGPU_RAMFC >> 8) | (1UL << 31));
	NV_vram_wr32(NVGPU_PLAYLIST + 0, chid);
	NV_wr32(PFIFO_PLAYLIST_32F4, NVGPU_PLAYLIST >> 12);
	NV_wr32(PFIFO_PLAYLIST_32EC, 1);
	NV_wr32(PFIFO_CACHES, 0x101);
	return ctxctl;
}

/* Submit a ring-3-built method stream (count dwords). On the first call the
 * channel is brought up. Appends to the pushbuf, advances PUT, waits for the
 * engine to drain and go idle, and fills diagnostics. */
static unsigned long g_gpu_wedged = 0;   /* 3087: latched by a trap or an unconsumed kick */

/* 3091: PGRAPH trap handling, after nouveau's nv50_gr_trap_handler. Dumps the
 * faulting unit's registers (DISPATCH 0x400804.., M2MF 0x406800.., VFETCH
 * 0x400c00.., STRMOUT 0x401800.., CCACHE 0x405000.., and per TP (0x1540 bits
 * 16+): TEXTURE 0x408900, MP 0x408314, PROP 0x408e08 - PROP's e10/e14 are the
 * fault address, ustatus 0x10 = 2D dst, 0x40 = render target, 0x80 = CUDA
 * memory), clears each unit's status (0xc0000000), acks 0x400108/0x400100
 * and restarts PGRAPH (0x400500 = 0x10001). The kick then continues, so one
 * T61 run reports EVERY fault instead of stopping at the first. */
static void gpu_dump8(const char *tag, unsigned long base, unsigned long n)
{
	unsigned long v[8], k;
	for(k = 0; k < 8; k++) v[k] = (k < n) ? NV_rd32(base + k * 4) : 0;
	dbg_printf("S> %s @%06lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n", tag, base,
	           v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}
static int gpu_trap_recover(unsigned long stat, unsigned long *units_acc)
{
	unsigned long trap = NV_rd32(0x400108UL), units = NV_rd32(0x001540UL), i;
	dbg_printf("S> TRAP intr=%08lX trap=%08lX taddr=%08lX tdata=%08lX class=%08lX cur=%08lX units=%08lX\n",
	           stat, trap, NV_rd32(0x400704UL), NV_rd32(0x400708UL), NV_rd32(0x400814UL),
	           NV_rd32(PGRAPH_CTXCTL_CUR), units);
	*units_acc |= trap;
	if(!(stat & 0x00200000UL))
	{
		NV_wr32(PGRAPH_INTR, stat);          /* not a trap: just acknowledge */
		return 1;
	}
	if(trap & 0x001UL)
	{
		gpu_dump8("dispatch", 0x400804UL, 5); gpu_dump8("dispatch2", 0x400848UL, 6);
		NV_wr32(0x400500UL, 0); NV_wr32(0x400808UL, 0); NV_wr32(0x40084cUL, 0);
		NV_wr32(0x400804UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x001UL);
	}
	if(trap & 0x002UL)
	{
		gpu_dump8("m2mf", 0x406800UL, 5);
		NV_wr32(0x400040UL, 2); NV_wr32(0x400040UL, 0);
		NV_wr32(0x406800UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x002UL);
	}
	if(trap & 0x004UL)
	{
		gpu_dump8("vfetch", 0x400c00UL, 5);
		NV_wr32(0x400c04UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x004UL);
	}
	if(trap & 0x008UL)
	{
		gpu_dump8("strmout", 0x401800UL, 5);
		NV_wr32(0x400040UL, 0x80); NV_wr32(0x400040UL, 0);
		NV_wr32(0x401800UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x008UL);
	}
	if(trap & 0x010UL)
	{
		gpu_dump8("ccache", 0x405000UL, 8);
		NV_wr32(0x405018UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x010UL);
	}
	if(trap & 0x020UL)
	{
		gpu_dump8("unk402000", 0x402000UL, 1);
		NV_wr32(0x402000UL, 0xc0000000UL); NV_wr32(0x400108UL, 0x020UL);
	}
	for(i = 0; i < 8; i++)
	{
		if(!(units & (1UL << (16 + i)))) continue;
		if(trap & 0x040UL) { gpu_dump8("tp-texture", 0x408900UL + (i << 12), 5); NV_wr32(0x408900UL + (i << 12), 0xc0000000UL); }
		if(trap & 0x080UL) { gpu_dump8("tp-mp",      0x408314UL + (i << 12), 6); NV_wr32(0x408314UL + (i << 12), 0xc0000000UL); }
		if(trap & 0x100UL) { gpu_dump8("tp-prop",    0x408e08UL + (i << 12), 8); NV_wr32(0x408e08UL + (i << 12), 0xc0000000UL); }
	}
	if(trap & 0x040UL) NV_wr32(0x400108UL, 0x040UL);
	if(trap & 0x080UL) NV_wr32(0x400108UL, 0x080UL);
	if(trap & 0x100UL) NV_wr32(0x400108UL, 0x100UL);
	if(trap & ~0x1ffUL) NV_wr32(0x400108UL, trap & ~0x1ffUL);
	NV_wr32(PGRAPH_INTR, stat);
	NV_wr32(0x400500UL, 0x00010001UL);
	if(NV_rd32(0x400824UL) & 0x80000000UL)
		NV_wr32(0x400824UL, NV_rd32(0x400824UL) & 0x7fffffffUL);
	dbg_printf("S> TRAP acked; status=%08lX intr=%08lX trap=%08lX\n", NV_rd32(PGRAPH_STATUS), NV_rd32(PGRAPH_INTR), NV_rd32(0x400108UL));
	return 1;
}

/* 3091: write the storage kind (nouveau's PTE bits 40-46 = high dword bits
 * 8-14) into the 64 KiB PTEs covering [phys, phys+size), then invalidate
 * PGRAPH's TLB (0x100c80, engine id 0, after nv50_vmm_flush). Tiled surfaces
 * (RT_TILE_MODE/ZETA_TILE_MODE != 0) need the PTE kind that matches their
 * format or the ROP path faults (3090 on the T61: PROP trap at the first RT). */
int NV_gpu_vmkind(unsigned long phys, unsigned long size, unsigned long kind)
{
	unsigned long first, last, i, t;
	if(!g_chan_ready)
	{
		if(!nv_chan_init()) return 0;
		g_chan_ready = 1; g_chan_put = 0; g_ib_put = 0;
	}
	if(size == 0) return 0;
	first = phys >> 16;
	last  = (phys + size - 1) >> 16;
	if(last >= 2048UL || first > last) return 0;
	for(i = first; i <= last; i++)
		NV_vram_wr32(NVGPU_VRAMPT + i * 8 + 4, (kind & 0x7fUL) << 8);
	(void)NV_vram_rd32(NVGPU_VRAMPT + last * 8 + 4);
	NV_wr32(0x100c80UL, (0UL << 16) | 1UL);
	for(t = 0; t < 1000000UL; t++)
		if(!(NV_rd32(0x100c80UL) & 1UL)) break;
	dbg_printf("S> vmkind phys=%08lX size=%08lX kind=%02lX pages=%lu flushloops=%lu\n",
	           phys, size, kind, last - first + 1, t);
	return 1;
}

/* 3089: when a kick wedges, dump the PFIFO pusher/puller and PGRAPH state so the
 * stall point is visible: DMA GET/PUT (how far the pusher fetched), CACHE1
 * GET/PUT (cache fill; PUT-GET == cache size means the puller is stuck), PULL0
 * (bit 4 = puller hung on a method it cannot deliver), the cache entries around
 * CACHE1 GET (the method the puller is stuck on; NV40+ cache RAM at 0x90000,
 * 512 entries of method/data), and the PGRAPH context/trap registers. */
#define PFIFO_CACHE1_MTHD(i) (0x00090000UL + (i) * 8UL)
#define PFIFO_CACHE1_DATA(i) (0x00090004UL + (i) * 8UL)
static void gpu_dump_stall(unsigned long chid)
{
	unsigned long c1get, c1put, k, idx;

	dbg_printf("S> pfifo intr=%08lX en=%08lX pmc=%08lX caches=%08lX ctxtab=%08lX ibput=%lu ibget=%lu 3330=%08lX 3334=%08lX\n",
	           NV_rd32(PFIFO_INTR_0), NV_rd32(PFIFO_INTR_EN_0), NV_rd32(0x000100UL), NV_rd32(PFIFO_CACHES),
	           NV_rd32(PFIFO_CTX_TABLE(chid)), NV_rd32(PFIFO_USER_IBPUT(chid)), NV_rd32(PFIFO_USER_IBGET(chid)),
	           NV_rd32(0x3330UL), NV_rd32(0x3334UL));
	dbg_printf("S> c1 push0=%08lX push1=%08lX dmapush=%08lX fetch=%08lX state=%08lX inst=%08lX ctl=%08lX\n",
	           NV_rd32(0x3200UL), NV_rd32(0x3204UL), NV_rd32(0x3220UL), NV_rd32(0x3224UL),
	           NV_rd32(0x3228UL), NV_rd32(0x322cUL), NV_rd32(0x3230UL));
	c1put = NV_rd32(0x3210UL);
	c1get = NV_rd32(0x3270UL);
	dbg_printf("S> c1 dmaput=%08lX dmaget=%08lX ref=%08lX sub=%08lX c1put=%08lX c1get=%08lX pull0=%08lX pull1=%08lX eng=%08lX\n",
	           NV_rd32(0x3240UL), NV_rd32(0x3244UL), NV_rd32(0x3248UL), NV_rd32(0x324cUL),
	           c1put, c1get, NV_rd32(0x3250UL), NV_rd32(0x3254UL), NV_rd32(0x3280UL));
	for(k = 0; k < 8; k++)
	{
		idx = ((c1get >> 2) + k + 510UL) & 511UL;   /* two before GET, then six from GET */
		dbg_printf("S> cache[%03lX] mthd=%08lX data=%08lX%s\n", idx,
		           NV_rd32(PFIFO_CACHE1_MTHD(idx)), NV_rd32(PFIFO_CACHE1_DATA(idx)), k == 2 ? "  <- GET" : "");
	}
	dbg_printf("S> gr intr=%08lX trap=%08lX status=%08lX taddr=%08lX tdata=%08lX cur=%08lX ctxptr=%08lX class=%08lX\n",
	           NV_rd32(PGRAPH_INTR), NV_rd32(0x400108UL), NV_rd32(PGRAPH_STATUS), NV_rd32(0x400704UL),
	           NV_rd32(0x400708UL), NV_rd32(PGRAPH_CTXCTL_CUR), NV_rd32(PGRAPH_CHANNEL_CTX_PTR), NV_rd32(0x400814UL));
	dbg_printf("S> gr 320=%08lX 500=%08lX 804=%08lX 808=%08lX 80c=%08lX 810=%08lX 824=%08lX 13c=%08lX\n",
	           NV_rd32(0x400320UL), NV_rd32(0x400500UL), NV_rd32(0x400804UL), NV_rd32(0x400808UL),
	           NV_rd32(0x40080cUL), NV_rd32(0x400810UL), NV_rd32(0x400824UL), NV_rd32(PGRAPH_INTR_EN));
}

int NV_gpu_submit(unsigned long *methods, unsigned long count, nv_gpu_submit_t *out)
{
	const unsigned long chid = NVGPU_CHID;
	unsigned long i, bytes = count * 4;

	memset(out, 0, sizeof(*out));
	out->build = NV_BUILD;
	if(count == 0 || bytes > NVGPU_PUSHBUF_SIZE)
		return 0;

	/* 3087: once the GPU trapped or stopped consuming, refuse further kicks at
	 * once instead of re-spinning the multi-second polls in ring 0 for every
	 * frame of a render loop (that is what froze the T61 after 3086's first
	 * frames). A reboot clears it (the channel is rebuilt from cold). */
	if(g_gpu_wedged)
	{
		out->wedged      = 1;
		out->ok          = 0;
		out->pfifo_intr  = NV_rd32(PFIFO_INTR_0);
		out->pgraph_intr = NV_rd32(PGRAPH_INTR);
		out->get         = NV_rd32(PFIFO_USER_IBGET(chid));
		out->put         = g_ib_put;
		dbg_printf("S> submit REFUSED (wedged) cnt=%lu pgi=%08lX\n", count, out->pgraph_intr);
		return 0;
	}
	/* 3095: the per-kick begin/end lines are kept for the first 64 kicks and
	 * then one in 256 - Half-Life makes hundreds of kicks a frame and the
	 * ring + the nvprobe tail writer were a per-kick cost; wedges, traps and
	 * failures are always logged. */
	nsub_verbose = (g_nsub < 64UL || (g_nsub & 255UL) == 0) ? 1 : 0;
	g_nsub++;
	if(nsub_verbose)
		dbg_printf("S> submit begin cnt=%lu m0=%08lX m1=%08lX ibput=%lu\n", count, methods[0],
		           count > 1 ? methods[1] : 0, g_ib_put);

	if(!g_chan_ready)
	{
		out->ctxctl = nv_chan_init();
		if(!out->ctxctl) return 0;
		g_chan_ready = 1;
		g_chan_put   = 0;
		g_ib_put     = 0;
	}
	else
		out->ctxctl = NV_rd32(PGRAPH_CTXCTL_CUR);

	/* IB mode: place the command stream in the data region (wrap if needed),
	 * then post an indirect-buffer entry pointing at it and advance IB_PUT. */
	if(bytes > NVGPU_PUSHBUF_SIZE)
	{
		/* 3095: a whole-frame kick larger than the region would run into the
		 * IB ring behind it; refuse it instead (the shim caps its batches) */
		out->ok = 0;
		dbg_printf("S> submit REFUSED (too large) bytes=%lu\n", bytes);
		return 0;
	}
	if(g_chan_put + bytes > NVGPU_PUSHBUF_SIZE)
		g_chan_put = 0;
	for(i = 0; i < count; i++)
		NV_vram_wr32(NVGPU_PUSHBUF + g_chan_put + i * 4, methods[i]);
	out->put_off = NVGPU_PUSHBUF + g_chan_put;

	/* IB entry (2 dwords): dw0 = command address (low), dw1 = (addr_high & 0xff)
	 * | (byte-length << 8) - which equals dwords<<10, the hw size field. */
	NV_vram_wr32(NVGPU_IB + g_ib_put * 8 + 0, NVGPU_PUSHBUF + g_chan_put);
	NV_vram_wr32(NVGPU_IB + g_ib_put * 8 + 4, (bytes << 8));
	g_chan_put += bytes;
	g_ib_put = (g_ib_put + 1) & (NVGPU_IB_ENTRIES - 1);

	NV_wr32(PFIFO_INTR_0, 0xffffffffUL);
	(void)NV_vram_rd32(NVGPU_IB);              /* flush the IB write to VRAM */
	NV_wr32(PFIFO_USER_IBPUT(chid), g_ib_put); /* kick: advance the IB put index */
	for(i = 0; i < 2000000UL; i++)
	{
		unsigned long pgi;
		if(NV_rd32(PFIFO_USER_IBGET(chid)) == g_ib_put)
			break;
		if(NV_rd32(PFIFO_INTR_0))
			break;
		pgi = NV_rd32(PGRAPH_INTR);
		if(pgi)
		{
			/* 3091: recover the trap and keep going (bounded) */
			if(++out->traps > 16UL || !gpu_trap_recover(pgi, &out->trap_units))
				break;
		}
	}
	out->idle_loops = i;
	/* 3098: IBGET==put only says the pusher TOOK this kick's IB entries; it may
	 * still be fetching their pushbuffer data (CACHE1 DMA_GET 0x3244 behind
	 * DMA_PUT 0x3240) and the puller may still hold methods in the cache (CACHE1
	 * GET 0x3270 behind PUT 0x3210) while PGRAPH reads idle between two methods,
	 * so a "synchronous" submit could return before the engines had read all of
	 * it - and ring 3 then overwrites staging VRAM (reused for the next texture
	 * patch) under them. Drain the pusher and the method cache before the PGRAPH
	 * idle wait. Latched off after one timeout, so a misread register costs one
	 * slow kick, not every kick; the first kicks report their drain loops. */
	if(!g_drain_off)
	{
		unsigned long j;
		for(j = 0; j < 400000UL; j++)
		{
			if(NV_rd32(0x3244) == NV_rd32(0x3240) && NV_rd32(0x3270) == NV_rd32(0x3210))
				break;
			if(NV_rd32(PFIFO_INTR_0) || NV_rd32(PGRAPH_INTR))
				break;
		}
		if(j >= 400000UL)
		{
			g_drain_off = 1;
			dbg_printf("S> drain TIMEOUT - disabled: dmaget/put %08lx/%08lx c1get/put %08lx/%08lx\n",
			           NV_rd32(0x3244), NV_rd32(0x3240), NV_rd32(0x3270), NV_rd32(0x3210));
		}
		else if(g_drain_logged < 16UL)
		{
			g_drain_logged++;
			dbg_printf("S> drain loops %lu (ib loops %lu)\n", j, i);
		}
		out->idle_loops += j;
	}
	out->gr_idle = gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 4000000UL);
	for(i = 0; i < 4; i++)
	{
		unsigned long pgi = NV_rd32(PGRAPH_INTR);   /* traps raised while draining */
		if(!pgi || out->traps >= 16UL) break;
		out->traps++;
		gpu_trap_recover(pgi, &out->trap_units);
		out->gr_idle = gpu_wait(PGRAPH_STATUS, 0xffffffffUL, 0, 4000000UL);
	}

	out->get         = NV_rd32(PFIFO_USER_IBGET(chid));  /* IB get index */
	out->put         = g_ib_put;                          /* IB put index */
	out->pfifo_intr  = NV_rd32(PFIFO_INTR_0);
	out->pgraph_intr = NV_rd32(PGRAPH_INTR);
	out->dma_state   = NV_rd32(0x3228);
	out->c1_push1    = NV_rd32(0x3204);   /* current channel id (loaded chan) */
	out->caches      = NV_rd32(PFIFO_CACHES);
	out->c1_get      = NV_rd32(0x3244);   /* live CACHE1 DMA GET */
	out->c1_put      = NV_rd32(0x3240);   /* live CACHE1 DMA PUT */
	out->ctxtable    = NV_rd32(PFIFO_CTX_TABLE(chid));
	out->dma_push    = NV_rd32(0x3220);   /* CACHE1_DMA_PUSH (bit0 ACCESS, bit4 STATE) */
	out->push0       = NV_rd32(0x3200);   /* CACHE1_PUSH0 */
	out->pull0       = NV_rd32(0x3250);   /* CACHE1_PULL0 */
	out->dma_get_rb  = NV_rd32(0x3244);
	/* 3087: a PGRAPH trap or an IB entry the pusher never consumed latches the
	 * GPU as wedged; the caller sees ok=0 (PFIFO_INTR alone is only reported). */
	/* 3088: a graphics engine still busy after the idle wait (gr_idle hit its
	 * bound) is a silent hang - 3087's T61 run showed the frame kick "consumed"
	 * while the engine never finished, so the NEXT 3D method stalled the pusher. */
	if(out->pgraph_intr || out->get != out->put || out->gr_idle >= 4000000UL || out->traps >= 16UL)
	{
		g_gpu_wedged = 1;
		out->wedged  = 1;
		out->ok      = 0;
		dbg_printf("S> submit WEDGED get=%lu put=%lu pfi=%08lX pgi=%08lX loops=%lu gridle=%lu status=%08lX\n",
		           out->get, out->put, out->pfifo_intr, out->pgraph_intr, out->idle_loops,
		           out->gr_idle, NV_rd32(PGRAPH_STATUS));
		gpu_dump_stall(chid);
		return 1;
	}
	out->ok          = 1;
	if(out->traps)
		dbg_printf("S> submit end with %lu TRAP(s) recovered, units=%08lX\n", out->traps, out->trap_units);
	if(nsub_verbose)
		dbg_printf("S> submit end get=%lu pfi=%08lX loops=%lu gridle=%lu dmaget=%08lX c1put=%08lX c1get=%08lX pull0=%08lX\n",
		           out->get, out->pfifo_intr, out->idle_loops, out->gr_idle, out->c1_get,
		           NV_rd32(0x3210UL), NV_rd32(0x3270UL), out->pull0);
	return 1;
}

/* Read up to 64 dwords of VRAM back to ring-3 (result readback for tests). */
int NV_gpu_vram_read(unsigned long offset, unsigned long count, nv_gpu_vread_t *out)
{
	unsigned long i;
	memset(out, 0, sizeof(*out));
	out->build = NV_BUILD;
	if(count > 64) count = 64;
	out->offset = offset;
	out->count  = count;
	for(i = 0; i < count; i++)
		out->data[i] = NV_vram_rd32(offset + i * 4);
	out->ok = 1;
	return 1;
}

int NV_gpu_2dfill(nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, 0); }
int NV_gpu_vmtest(nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, NVF_VM); }
int NV_gpu_fence (nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, NVF_FENCE); }
int NV_gpu_3dtest(nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, NVF_3D); }

/* ===================== M5.3c: bind an object at a caller's handle ============
 * The Mesa nouveau winsys creates channel objects with ITS OWN handles (vram
 * 0xbeef0201, gart 0xbeef0202, notifier 0xbeef0301, M2MF 0xbeef5039, 2D
 * 0xbeef502d, 3D 0xbeef5097). The shim's nouveau_object_new routes each here so
 * the persistent channel's RAMHT gains an entry for that handle. Graphics engine
 * classes (2D/M2MF/3D) get a small gfx-object instance (engine GR=1); everything
 * else (DMA/notifier) gets a VM-target ctxdma (engine 0). Instances come from a
 * bump area inside the channel's 1 MiB window. Additive - the fixed nv_chan_init
 * bindings (used by nvprobe gpu2d/submit) are untouched. */
static unsigned long g_obj_next = 0;

static int nv_class_is_gfx(unsigned long oclass)
{
	switch(oclass)
	{
	case 0x502dUL: case 0x5039UL: case 0x5097UL:
	case 0x8297UL: case 0x8597UL: case 0x8697UL:
	case 0x50c0UL: case 0x85c0UL:            /* 3089: COMPUTE runs on PGRAPH too */
		return 1;
	}
	return 0;
}

int NV_gpu_bind(unsigned long handle, unsigned long oclass, nv_gpu_bind_t *out)
{
	unsigned long inst;

	memset(out, 0, sizeof(*out));
	out->build  = NV_BUILD;
	out->handle = handle;
	out->oclass = oclass;

	if(!g_chan_ready)
	{
		out->ctxctl = nv_chan_init();
		if(!out->ctxctl) return 0;
		g_chan_ready = 1;
		g_chan_put   = 0;
		g_ib_put     = 0;
	}
	if(g_obj_next < NVGPU_OBJAREA) g_obj_next = NVGPU_OBJAREA;

	inst = (g_obj_next + 0xffUL) & ~0xffUL;        /* 256-byte aligned instance */
	if(inst + 0x100UL > NVGPU_OBJAREA_END) return 0;   /* object area full */
	g_obj_next = inst + 0x100UL;
	out->inst  = inst;

	NV_vram_fill(inst, 0x100 / 4, 0);
	if(oclass == 0x80000002UL)
	{
		/* 3092: notifier object: a real linear ctxdma over a zeroed 4 KiB block
		 * in physical VRAM (nouveau's notifier). The VM-window ctxdma used
		 * before made M2MF's notify write land at VM address 0 (unmapped). */
		NV_vram_fill(NVGPU_NOTIFY, 0x1000 / 4, 0);
		gpu_ctxdma(inst, NVGPU_NOTIFY, 0x1000UL);
		gpu_ramht(handle, inst - NVGPU_RAMIN, 0);
		out->engine = 0;
	}
	else if(nv_class_is_gfx(oclass))
	{
		/* nv50 graphics object: obj[0]=class, obj[5]=0x00010000; engine GR=1 */
		NV_vram_wr32(inst + 0,  oclass);
		NV_vram_wr32(inst + 20, 0x00010000UL);
		gpu_ramht(handle, inst - NVGPU_RAMIN, 1);
		out->engine = 1;
	}
	else
	{
		/* VM-target ctxdma (covers the whole 1:1 VM window), engine 0 */
		NV_vram_wr32(inst + 0x00, 0x7fc00000UL | 0x3dUL);
		NV_vram_wr32(inst + 0x04, 0xffffffffUL);
		NV_vram_wr32(inst + 0x08, 0);
		NV_vram_wr32(inst + 0x0c, 0);
		NV_vram_wr32(inst + 0x10, 0);
		NV_vram_wr32(inst + 0x14, 0x00080000UL);
		gpu_ramht(handle, inst - NVGPU_RAMIN, 0);
		out->engine = 0;
	}
	out->ok = 1;
	dbg_printf("NV_gpu_bind: handle=%lX class=%lX -> inst=%lX engine=%lu\n",
		handle, oclass, inst, out->engine);
	return 1;
}
