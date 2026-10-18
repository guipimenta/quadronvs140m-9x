/*****************************************************************************
 * nv50_evo.h - NV50/G8x EVO core-channel method map (class 0x5070 / 827d)
 *
 * Method numbers for the G86 display "core" (master) channel, extracted from
 * envytools rnndb/display/nv_evo.xml (variants G80:GF119 / G84:GF119, which
 * cover G86). These are CHANNEL METHOD offsets written into the EVO pushbuffer
 * as (method|count<<18) headers followed by data dwords - NOT MMIO offsets.
 *
 * Head array base = 0x800, stride 0x400 (so head1 = 0xC00). The T61 panel is
 * on head0. See docs/EVO_PLAN.md.
 *****************************************************************************/
#ifndef __NV50_EVO_H__
#define __NV50_EVO_H__

/* ---- core-channel global methods ---- */
#define NV50_EVO_UPDATE              0x0080  /* write 0 -> execute pending FIFO */
#define NV50_EVO_NOTIFIER_CTRL       0x0084
#define NV50_EVO_DMA_NOTIFIER        0x0088  /* notifier ctxdma handle          */
#define NV50_EVO_GET_CAPABILITIES    0x008c
#define NV50_EVO_SOR(i)              (0x0600 + (i)*0x40) /* output routing (G80:G94) */

/* ---- per-head methods (head0 base 0x800) ---- */
#define NV50_EVO_HEAD(h)             (0x0800 + (h)*0x400)

#define NV50_EVO_H_PRESENT_CTRL(h)   (NV50_EVO_HEAD(h) + 0x00)
#define NV50_EVO_H_PIXEL_CLOCK(h)    (NV50_EVO_HEAD(h) + 0x04) /* [21:0]=kHz, [23:22]=MODE */
#define NV50_EVO_H_CTRL(h)           (NV50_EVO_HEAD(h) + 0x08)
#define NV50_EVO_H_OVERSCAN(h)       (NV50_EVO_HEAD(h) + 0x10)
/* raster timing (xy15: x in [14:0], y in [30:16]; reference = start of sync) */
#define NV50_EVO_H_RASTER_SIZE(h)    (NV50_EVO_HEAD(h) + 0x14) /* total  (wh15) */
#define NV50_EVO_H_RASTER_SYNC(h)    (NV50_EVO_HEAD(h) + 0x18) /* sync end      */
#define NV50_EVO_H_RASTER_BLANK_E(h) (NV50_EVO_HEAD(h) + 0x1c) /* blank end     */
#define NV50_EVO_H_RASTER_BLANK_S(h) (NV50_EVO_HEAD(h) + 0x20) /* blank start   */
#define NV50_EVO_H_RASTER_VBLANK2(h) (NV50_EVO_HEAD(h) + 0x24)
#define NV50_EVO_H_DEFAULT_COLOR(h)  (NV50_EVO_HEAD(h) + 0x2c)
#define NV50_EVO_H_DMA_LUT(h)        (NV50_EVO_HEAD(h) + 0x5c)
/* framebuffer (G80_EVO_FB_SETTINGS at head+0x60) */
#define NV50_EVO_H_FB_OFFSET(h)      (NV50_EVO_HEAD(h) + 0x60) /* +0x64 = 2nd (field) */
#define NV50_EVO_H_FB_SIZE(h)        (NV50_EVO_HEAD(h) + 0x68) /* wh15 w|h<<16   */
#define NV50_EVO_H_FB_STORAGE(h)     (NV50_EVO_HEAD(h) + 0x6c) /* pitch/layout   */
#define NV50_EVO_H_FB_PARAMS(h)      (NV50_EVO_HEAD(h) + 0x70) /* format         */
#define NV50_EVO_H_DMA_ISO(h)        (NV50_EVO_HEAD(h) + 0x74) /* fb ctxdma handle (G84: +0x78 2nd) */
#define NV50_EVO_H_CTRL_CURSOR(h)    (NV50_EVO_HEAD(h) + 0x80)
#define NV50_EVO_H_OFFSET_CURSOR(h)  (NV50_EVO_HEAD(h) + 0x84)
#define NV50_EVO_H_DMA_CURSOR(h)     (NV50_EVO_HEAD(h) + 0x9c) /* G84:GF119      */
#define NV50_EVO_H_DITHER(h)         (NV50_EVO_HEAD(h) + 0xa0)
#define NV50_EVO_H_SCALER(h)         (NV50_EVO_HEAD(h) + 0xa4)
/* viewport (G80:GF119 at head+0xc0) */
#define NV50_EVO_H_VP_POINT_IN(h)    (NV50_EVO_HEAD(h) + 0xc0) /* xy15           */
#define NV50_EVO_H_VP_SIZE_IN(h)     (NV50_EVO_HEAD(h) + 0xc8) /* wh15           */
#define NV50_EVO_H_VP_POINT_OUT(h)   (NV50_EVO_HEAD(h) + 0xd4)
#define NV50_EVO_H_VP_SIZE_OUT(h)    (NV50_EVO_HEAD(h) + 0xd8)

/* FB_PARAMS pixel formats ([15:8]) */
#define NV50_EVO_FORMAT_I8           0x1e
#define NV50_EVO_FORMAT_R5G6B5       0xe8   /* 16bpp */
#define NV50_EVO_FORMAT_A1R5G5B5     0xe9
#define NV50_EVO_FORMAT_A8R8G8B8     0xcf   /* 32bpp */

/* FB_STORAGE: pitch is bits[17:8] of (pitch_bytes >> 4) on G80:GT215, i.e.
 * ((pitch_bytes >> 4) << 8). MEMORY_LAYOUT is bit20 (G80:GF119); confirm which
 * value means pitch/linear vs block-linear before use (TODO: G80_HEAD_STORAGE_LAYOUT). */
#define NV50_EVO_STORAGE_PITCH(bytes) ((((bytes) >> 4) & 0x3ff) << 8)

/* EVO pushbuffer method header: method dword = (count<<18) | (method & 0x3ffff). */
#define NV50_EVO_MTHD(method, count)  (((count) << 18) | ((method) & 0x3ffff))

/* wh15 / xy15 packing: width/x in low 16, height/y in high 16. */
#define NV50_EVO_WH(w, h)             (((w) & 0xffff) | (((h) & 0xffff) << 16))

/* Handle we will give the framebuffer ctxdma (created in E3). */
#define NVFB_ISO_HANDLE              0xcafe0001UL

/* ---- E2 staging (VRAM-only, no hardware activation) ---- */
/* Scratch VRAM byte offsets, well clear of the live scanout at offset 0.
 * Proven writable by the build-2005 VRAM tests. */
#define NVFB_FB_VRAM_OFFSET          0x01000000UL  /* 16 MB: our framebuffer   */
#define NVFB_PUSH_VRAM_OFFSET        0x01800000UL  /* 24 MB: EVO pushbuffer     */
#define NVFB_NOTIFY_VRAM_OFFSET      0x01810000UL  /* notifier                  */

typedef struct nv_evo_layout
{
	unsigned long fb_offset;      /* VRAM byte offset of the framebuffer      */
	unsigned long fb_width;
	unsigned long fb_height;
	unsigned long fb_bpp;
	unsigned long fb_pitch;       /* bytes per row                            */
	unsigned long push_offset;    /* VRAM byte offset of the pushbuffer       */
	unsigned long push_dwords;    /* method-stream length in dwords           */
	unsigned long notify_offset;  /* VRAM byte offset of the notifier         */
	unsigned long iso_handle;     /* fb ctxdma handle the stream references   */
	unsigned long first_method;   /* pushbuffer[0], for readback verification */
	unsigned long ok;             /* 1 if staged (VRAM mapped & written)      */
} nv_evo_layout_t;

/* Build head0 framebuffer + pushbuffer + test pattern in VRAM. VRAM writes
 * only; does NOT touch the display engine. Returns 1 on success. */
int NV_evo_stage(nv_evo_layout_t *out);

#endif /* __NV50_EVO_H__ */
