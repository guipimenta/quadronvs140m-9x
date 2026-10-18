/*****************************************************************************
 * nv50_evo.c - EVO staging (M3 E2). VRAM-only; does not touch the display.
 *
 * Draws a test pattern into a 1024x768x16bpp framebuffer in VRAM and builds the
 * head0 "framebuffer takeover" EVO method stream in a pushbuffer in VRAM, so we
 * can verify the encoding by reading it back. Nothing here points the display
 * engine at this memory - that is E3.
 *****************************************************************************/
#define NV_DETECT

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"
#include "vxd_lib.h"
#include "3d_accel.h"

#include "vxd_nv.h"
#include "nv50_disp.h"
#include "nv50_evo.h"

#include "code32.h"

#define FB_W     1024UL
#define FB_H     768UL
#define FB_BPP   16UL
#define FB_PITCH (FB_W * (FB_BPP / 8))   /* 2048 bytes */

/* 8 vertical color bars in RGB565, left to right. */
static const unsigned short bar_colors[8] = {
	0xFFFF, /* white  */
	0xFFE0, /* yellow */
	0x07FF, /* cyan   */
	0x07E0, /* green  */
	0xF81F, /* magenta*/
	0xF800, /* red    */
	0x001F, /* blue   */
	0x0000  /* black  */
};

/* Draw the test pattern into the framebuffer region (two 16bpp pixels per
 * dword). Pure VRAM writes. */
static void stage_pattern(unsigned long fb)
{
	unsigned long y, x;
	for(y = 0; y < FB_H; y++)
	{
		unsigned long row = fb + y * FB_PITCH;
		for(x = 0; x < FB_W; x += 2)
		{
			unsigned short c0 = bar_colors[(x)   >> 7]; /* /128 -> 8 bars */
			unsigned short c1 = bar_colors[(x+1) >> 7];
			NV_vram_wr32(row + x * 2, ((unsigned long)c1 << 16) | c0);
		}
	}
}

/* Append a one-dword method + data to the pushbuffer; returns new dword index. */
static unsigned long push1(unsigned long push, unsigned long i,
                           unsigned long method, unsigned long data)
{
	NV_vram_wr32(push + i * 4, NV50_EVO_MTHD(method, 1));
	i++;
	NV_vram_wr32(push + i * 4, data);
	i++;
	return i;
}

int NV_evo_stage(nv_evo_layout_t *out)
{
	unsigned long fb     = NVFB_FB_VRAM_OFFSET;
	unsigned long push   = NVFB_PUSH_VRAM_OFFSET;
	unsigned long notify = NVFB_NOTIFY_VRAM_OFFSET;
	unsigned long i = 0;

	memset(out, 0, sizeof(nv_evo_layout_t));

	/* Make sure the framebuffer region is writable (maps BAR1). */
	if(!NV_vram_wr32(fb, 0))
		return 0;

	/* Clear the notifier and draw the test pattern. */
	NV_vram_fill(notify, 16, 0);
	stage_pattern(fb);

	/* Build the head0 framebuffer-takeover method stream (reuse live raster,
	 * clock and SOR0). The ISO handle references a ctxdma created in E3. */
	i = push1(push, i, NV50_EVO_H_DMA_ISO(0),    NVFB_ISO_HANDLE);
	i = push1(push, i, NV50_EVO_H_FB_OFFSET(0),  0);
	i = push1(push, i, NV50_EVO_H_FB_SIZE(0),    NV50_EVO_WH(FB_W, FB_H));
	i = push1(push, i, NV50_EVO_H_FB_STORAGE(0), NV50_EVO_STORAGE_PITCH(FB_PITCH));
	i = push1(push, i, NV50_EVO_H_FB_PARAMS(0),  (NV50_EVO_FORMAT_R5G6B5 << 8));
	i = push1(push, i, NV50_EVO_H_VP_POINT_IN(0),0);
	i = push1(push, i, NV50_EVO_H_VP_SIZE_IN(0), NV50_EVO_WH(FB_W, FB_H));
	i = push1(push, i, NV50_EVO_UPDATE,          0);

	out->fb_offset    = fb;
	out->fb_width     = FB_W;
	out->fb_height    = FB_H;
	out->fb_bpp       = FB_BPP;
	out->fb_pitch     = FB_PITCH;
	out->push_offset  = push;
	out->push_dwords  = i;
	out->notify_offset= notify;
	out->iso_handle   = NVFB_ISO_HANDLE;
	out->first_method = NV_vram_rd32(push);
	out->ok           = 1;

	dbg_printf("NV_evo_stage: fb=%lX push=%lX dwords=%lu\n", fb, push, i);
	return 1;
}
