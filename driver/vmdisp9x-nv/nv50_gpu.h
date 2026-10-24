/* nv50_gpu.h - native G86 3D/2D engine bring-up (M5.x). See nv50_gpu.c. */
#ifndef NV50_GPU_H
#define NV50_GPU_H

/* OP_NV_GPU_GRINIT (0x500E) result: PGRAPH init + context-microcode upload. */
typedef struct nv_gpu_grinit
{
	unsigned long build;         /* NV_BUILD                                 */
	unsigned long ok;            /* 1 = ran to the end                       */
	unsigned long stage;         /* last stage reached (0..5)                */
	unsigned long units;         /* 0x1540 read now                          */
	unsigned long units_built;   /* 0x1540 the baked context was built for   */
	unsigned long ctxprog_len;   /* microcode length (dwords)                */
	unsigned long grctx_size;    /* per-channel context size (bytes)         */
	unsigned long pre_pmc,    post_pmc;
	unsigned long pre_ctxctl, post_ctxctl;
	unsigned long pre_intr,   post_intr;
	unsigned long pre_status, post_status;
	unsigned long ucode_rb[8];   /* first 8 microcode dwords read back       */
	unsigned long idle_loops;    /* spins waiting PGRAPH status -> 0          */
} nv_gpu_grinit_t;

int NV_gpu_grinit(nv_gpu_grinit_t *out);

/* OP_NV_GPU_CTXTEST (0x500F): build a channel + per-channel grctx and load it. */
typedef struct nv_gpu_ctxtest
{
	unsigned long build, ok, stage, loaded;
	unsigned long ramin, grctx, grctx_size, ctxvals, inst12;
	unsigned long pre_ctxctl, post_ctxctl, post_next;
	unsigned long pre_intr,   post_intr,   post_status;
	unsigned long idle_loops;
} nv_gpu_ctxtest_t;

int NV_gpu_ctxtest(nv_gpu_ctxtest_t *out);

/* OP_NV_GPU_2DFILL (0x5010): PFIFO channel + 2D engine fill a VRAM rectangle. */
typedef struct nv_gpu_2dfill
{
	unsigned long build, ok, stage, filled;
	unsigned long scratch, color, ctxctl, put_bytes;
	unsigned long get, put, pfifo_intr, pgraph_intr, pgraph_stat, idle_loops;
	unsigned long c1_get, c1_put, c1_push1, c1_dmast, caches, ctxtable;
	unsigned long dma_state, dma_push, dma_fetch, dma_inst, punk1700, ctxdma_rb, push_rb0, push_rb1;
	unsigned long gr_idle;
	unsigned long pre_px[1], px[3];
	unsigned long heap_addr, notified, notify[4];  /* M5.2b fence */
} nv_gpu_2dfill_t;

int NV_gpu_2dfill(nv_gpu_2dfill_t *out);
int NV_gpu_vmtest(nv_gpu_2dfill_t *out);  /* M5.2a: fill via a GPU VIRTUAL address */
int NV_gpu_fence (nv_gpu_2dfill_t *out);  /* M5.2b: VRAM heap + notifier-based fence */

/* M5.3: persistent channel + ring-3 command submission */
typedef struct nv_gpu_submit
{
	unsigned long build, ok, ctxctl, put_off, gr_idle;
	unsigned long get, put, pfifo_intr, pgraph_intr, dma_state;
	unsigned long c1_push1, caches, c1_get, c1_put, ctxtable;
	unsigned long dma_push, push0, pull0, dma_get_rb;
} nv_gpu_submit_t;

typedef struct nv_gpu_vread
{
	unsigned long build, ok, offset, count;
	unsigned long data[64];
} nv_gpu_vread_t;

int NV_gpu_submit(unsigned long *methods, unsigned long count, nv_gpu_submit_t *out);
int NV_gpu_vram_read(unsigned long offset, unsigned long count, nv_gpu_vread_t *out);

#endif /* NV50_GPU_H */
