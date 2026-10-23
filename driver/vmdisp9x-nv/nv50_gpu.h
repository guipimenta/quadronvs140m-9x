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

#endif /* NV50_GPU_H */
