/*****************************************************************************
 * nvshim_ioctl.h - the nvmini VxD ioctl surface nvshim.c needs (M5.3c)
 *
 * When nvshim.c is compiled into mesa3d.dll it must NOT pull in the VxD source
 * tree (vxd_nv.h / nv50_gpu.h live beside io.h, which collides with mesa files
 * that include <io.h>). This self-contained header carries just the three
 * opcodes and two result structs nvshim.c uses, copied from vxd_nv.h /
 * nv50_gpu.h - keep them in sync. The standalone nvprobe build instead includes
 * the originals directly (NVSHIM_STANDALONE). Licensed MIT.
 *****************************************************************************/
#ifndef NVSHIM_IOCTL_H
#define NVSHIM_IOCTL_H

/* VxD DeviceIoControl opcodes (vxd_nv.h). */
#define OP_NV_SUBMIT      0x5013  /* submit a ring-3 method stream to the channel */
#define OP_NV_VRAM_READ   0x5014  /* read back VRAM dwords                        */
#define OP_NV_VRAM_WRITE  0x5015  /* write a block of dwords into VRAM (BO flush) */
#define OP_NV_BIND        0x5016  /* bind an object at a caller handle (RAMHT)   */

/* OP_NV_SUBMIT result (nv50_gpu.h: nv_gpu_submit_t). nvshim.c reads .ok only;
 * the rest keeps the layout identical to the VxD's so the ioctl memcpy fits. */
typedef struct nv_gpu_submit
{
	unsigned long build, ok, ctxctl, put_off, gr_idle;
	unsigned long get, put, pfifo_intr, pgraph_intr, dma_state;
	unsigned long c1_push1, caches, c1_get, c1_put, ctxtable;
	unsigned long dma_push, push0, pull0, dma_get_rb;
} nv_gpu_submit_t;

/* OP_NV_VRAM_READ result (nv50_gpu.h: nv_gpu_vread_t). */
typedef struct nv_gpu_vread
{
	unsigned long build, ok, offset, count;
	unsigned long data[64];
} nv_gpu_vread_t;

/* OP_NV_BIND result (nv50_gpu.h: nv_gpu_bind_t). */
typedef struct nv_gpu_bind
{
	unsigned long build, ok, handle, oclass, inst, engine, ctxctl;
} nv_gpu_bind_t;

#endif /* NVSHIM_IOCTL_H */
