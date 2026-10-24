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
#define NVGPU_VMDMA   0x03006000UL   /* M5.2 VM-target ctxdma (DMA_DST via VM)   */
#define NVGPU_VRAMPT  0x03010000UL   /* M5.2 VRAM page table (64 KiB)            */
#define NVGPU_PUSHBUF 0x03200000UL   /* command pushbuffer (data, physical)      */
#define NVGPU_SCRATCH 0x03400000UL   /* 2D fill destination (read back)          */
#define NVGPU_VM_VRAM_BASE 0x40000000UL  /* VRAM maps into the VM at +1 GiB      */
#define NVGPU_VM_SCRATCH   (NVGPU_VM_VRAM_BASE + NVGPU_SCRATCH)  /* VM addr of scratch */
/* NV50 engine/ctxdma instances are CHANNEL-RELATIVE: offset from the channel
 * RAMIN base (RAMFC 0x98 = ramin>>12), shifted. INST(x) keeps them small. */
#define NVGPU_INST(x) (((x) - NVGPU_RAMIN) >> 4)
#define NVGPU_CHID    1UL            /* our channel id (avoid 0/127)             */
#define NV50_2D_CLASS_ 0x0000502dUL

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
#define NVGPU_PLAYLIST 0x03204000UL  /* PFIFO playlist buffer */

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

/* Shared: PGRAPH reset + register init + context-microcode upload.
 * Returns 1 on success. Leaves PGRAPH powered and idle. */
static int gpu_pgraph_init(void)
{
	unsigned long units, i, pmc;
	if(!nv_hwinfo.found || !nv_hwinfo.mmio_ok)
		return 0;
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
	for(i = 0; i < NV50_CTXPROG_LEN; i++)
		NV_wr32(PGRAPH_CTXCTL_UCODE_DAT, nv50_ctxprog[i]);
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
	NV_vram_fill(grctx, NV50_GRCTX_DWORDS, 0);
	for(i = 0; i < NV50_CTXVALS_CNT; i++)
		NV_vram_wr32(grctx + nv50_ctxvals[i][0] * 4, nv50_ctxvals[i][1]);
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
static int gpu_2dfill_core(nv_gpu_2dfill_t *out, int use_vm)
{
	const unsigned long chid   = NVGPU_CHID;
	const unsigned long ramin  = NVGPU_RAMIN;
	const unsigned long grctx  = NVGPU_GRCTX;
	const unsigned long color  = 0x11223344UL;
	const unsigned long pitch  = 1024UL;              /* bytes (256 px * 4)    */
	const unsigned long w = 256UL, h = 256UL;
	unsigned long inst12, i, pushbytes, d[4], pmc, fifo;

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
	NV_vram_fill(grctx, NV50_GRCTX_DWORDS, 0);
	for(i = 0; i < NV50_CTXVALS_CNT; i++)
		NV_vram_wr32(grctx + nv50_ctxvals[i][0] * 4, nv50_ctxvals[i][1]);
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
	NV_vram_fill(NVGPU_CACHE, 0x1000 / 4, 0);
	NV_vram_wr32(NVGPU_OBJ2D + 0,  NV50_2D_CLASS_);   /* 0x502d                */
	NV_vram_wr32(NVGPU_OBJ2D + 20, 0x00010000UL);     /* dword 5               */

	/* RAMHT: 2D object (graphics engine 1), VRAM ctxdma (SW engine 0) */
	NV_vram_fill(NVGPU_RAMHT, 0x1000 / 4, 0);
	gpu_ramht(0xbeef502dUL, NVGPU_OBJ2D   - NVGPU_RAMIN, 1);
	gpu_ramht(0xbeef0002UL, NVGPU_VRAMDMA - NVGPU_RAMIN, 0);
	if(use_vm)
		gpu_ramht(0xbeef0003UL, NVGPU_VMDMA - NVGPU_RAMIN, 0);

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

	/* method stream: bind 2D, set destination surface, fill the rectangle */
	i = 0;
	d[0] = 0xbeef502dUL;             i = gpu_push(i, 0x0000, 1, d);  /* SET_OBJECT */
	d[0] = use_vm ? 0xbeef0003UL : 0xbeef0002UL; i = gpu_push(i, 0x0184, 1, d); /* DMA_DST */
	d[0] = 0xcfUL;                   i = gpu_push(i, 0x0200, 1, d);  /* DST_FORMAT */
	d[0] = 1;                        i = gpu_push(i, 0x0204, 1, d);  /* DST_LINEAR */
	d[0] = pitch;                    i = gpu_push(i, 0x0214, 1, d);  /* DST_PITCH  */
	d[0] = w;                        i = gpu_push(i, 0x0218, 1, d);  /* DST_WIDTH  */
	d[0] = h;                        i = gpu_push(i, 0x021c, 1, d);  /* DST_HEIGHT */
	d[0] = 0; d[1] = use_vm ? NVGPU_VM_SCRATCH : NVGPU_SCRATCH; i = gpu_push(i, 0x0220, 2, d); /* DST_ADDR hi,lo */
	d[0] = 0;                        i = gpu_push(i, 0x0290, 1, d);  /* CLIP_ENABLE=0 */
	d[0] = 3;                        i = gpu_push(i, 0x02ac, 1, d);  /* OPERATION SRCCOPY */
	d[0] = 4;                        i = gpu_push(i, 0x0580, 1, d);  /* DRAW_SHAPE RECT */
	d[0] = 0xcfUL;                   i = gpu_push(i, 0x0584, 1, d);  /* DRAW_COLOR_FORMAT */
	d[0] = color;                    i = gpu_push(i, 0x0588, 1, d);  /* DRAW_COLOR */
	d[0] = 0; d[1] = 0; d[2] = w; d[3] = h;
	                                 i = gpu_push(i, 0x0600, 4, d);  /* POINT32 (0,0)-(w,h) */
	pushbytes = i * 4;
	out->put_bytes = pushbytes;

	/* clear scratch to a different value so a successful fill is visible */
	NV_vram_fill(NVGPU_SCRATCH, (w * h), 0x99999999UL);
	out->pre_px[0] = NV_vram_rd32(NVGPU_SCRATCH + 0);

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
	/* read back the fill */
	out->px[0] = NV_vram_rd32(NVGPU_SCRATCH + 0);
	out->px[1] = NV_vram_rd32(NVGPU_SCRATCH + (w/2)*4 + (h/2)*pitch);
	out->px[2] = NV_vram_rd32(NVGPU_SCRATCH + (w-1)*4 + (h-1)*pitch);
	out->filled = (out->px[0] == color && out->px[1] == color && out->px[2] == color) ? 1 : 0;
	out->ok    = 1;
	dbg_printf("NV_gpu_2dfill: ctxctl=%lX get=%lX put=%lX pfi=%lX pgi=%lX px=%lX,%lX,%lX filled=%lu\n",
		out->ctxctl, out->get, out->put, out->pfifo_intr, out->pgraph_intr,
		out->px[0], out->px[1], out->px[2], out->filled);
	return 1;
}

int NV_gpu_2dfill(nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, 0); }
int NV_gpu_vmtest(nv_gpu_2dfill_t *out) { return gpu_2dfill_core(out, 1); }
