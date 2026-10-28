#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint8_t u8; typedef uint64_t u64; typedef int32_t s32;
#define BUG_ON(x) do { if (x) { fprintf(stderr, "BUG_ON\n"); exit(2); } } while (0)
#define GFP_KERNEL 0
#define ENOMEM 12
#define kmalloc(sz, f) malloc(sz)
#define kfree(p) free(p)
struct nvkm_ram { int type; }; struct nvkm_fb { struct nvkm_ram *ram; };
#define NVKM_RAM_TYPE_GDDR5 1
struct nvkm_device { u8 chipset; u32 units; struct nvkm_fb *fb; u32 *ctxprog; u32 ctxprog_len; };
struct nvkm_gpuobj { u32 *data; u32 size; };
static inline void nvkm_wo32(struct nvkm_gpuobj *o, u32 off, u32 val) { if (off / 4 >= o->size) { fprintf(stderr, "wo32 out of range %u\n", off); exit(3); } o->data[off / 4] = val; }
static inline u32 nvkm_rd32(struct nvkm_device *d, u32 addr) { if (addr == 0x1540) return d->units; fprintf(stderr, "rd32 %08x\n", addr); return 0; }
static inline void nvkm_wr32(struct nvkm_device *d, u32 addr, u32 val) { if (addr == 0x400324) d->ctxprog_len = 0; else if (addr == 0x400328) d->ctxprog[d->ctxprog_len++] = val; }
