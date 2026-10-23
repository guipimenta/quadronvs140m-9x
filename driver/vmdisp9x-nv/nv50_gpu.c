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
