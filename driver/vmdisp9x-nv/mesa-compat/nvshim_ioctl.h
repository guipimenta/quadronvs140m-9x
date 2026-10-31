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
#define OP_NV_PROBE         0x5000  /* 3099: nv_hwinfo_t (29 dwords, vxd_nv.h); [22] = the VxD's NV_BUILD */
#define NVSHIM_HWINFO_BUILD 22      /* dword index of nv_hwinfo_t.build                                   */
#define NVSHIM_BUILD        3120UL  /* this shim's own build (bump with every nvshim.c change; the        */
                                    /* standalone nvprobe build uses NV_BUILD from vxd_nv.h instead)      */
#define OP_NV_RD32          0x5001  /* 3094: read BAR0 MMIO dwords: in[0]=byte offset, in[1]=count -> out dwords (GRAPH_UNITS 0x1540) */
#define OP_NV_SUBMIT      0x5013  /* submit a ring-3 method stream to the channel */
#define OP_NV_VRAM_READ   0x5014  /* read back VRAM dwords                        */
#define OP_NV_VRAM_WRITE  0x5015  /* write a block of dwords into VRAM (BO flush) */
#define OP_NV_BIND        0x5016  /* bind an object at a caller handle (RAMHT)   */
#define OP_NV_DBG_PUTS    0x5018  /* append a trace string to the VxD debug ring */
#define OP_NV_VM_KIND     0x5019  /* 3091: set the PTE storage kind of a BO's pages  */
#define OP_NV_VRAM_READ2  0x501A  /* 3092: read up to 1024 VRAM dwords into the caller's buffer */
#define OP_NV_GPU_ACQUIRE   0x501B  /* 3110: this process takes the GPU channel (out[0] = 1 ok / 0 busy, out[1] = owner tag) */
#define OP_NV_GART_ALLOC    0x501C  /* 3115: in[0] = 4 KiB pages -> nv_gart_alloc_t (ring-3 linear address + VM offset) */
#define OP_NV_GART_FREE     0x501D  /* 3115: in[0] = the linear address OP_NV_GART_ALLOC returned */
#define OP_NV_GART_KIND     0x501E  /* 3115: in[0] = GART VM address, [1] = size, [2] = storage kind of those 4 KiB PTEs */

/* OP_NV_SUBMIT result (nv50_gpu.h: nv_gpu_submit_t). nvshim.c reads .ok only;
 * the rest keeps the layout identical to the VxD's so the ioctl memcpy fits. */
typedef struct nv_gpu_submit
{
	unsigned long build, ok, ctxctl, put_off, gr_idle;
	unsigned long get, put, pfifo_intr, pgraph_intr, dma_state;
	unsigned long c1_push1, caches, c1_get, c1_put, ctxtable;
	unsigned long dma_push, push0, pull0, dma_get_rb;
	unsigned long wedged, idle_loops;   /* 3087: mirrors nv50_gpu.h (keep both in sync) */
	unsigned long traps, trap_units;   /* 3091: PGRAPH traps recovered during this kick, OR of 0x400108 bits */
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

/* OP_NV_GART_ALLOC result (nv50_gpu.h: nv_gart_alloc_t). 3115. */
typedef struct nv_gart_alloc
{
	unsigned long build, ok, lin, vmoff, npages, total_pages, used_pages;
} nv_gart_alloc_t;

#endif /* NVSHIM_IOCTL_H */
