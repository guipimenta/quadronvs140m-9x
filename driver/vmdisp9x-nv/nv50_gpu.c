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
