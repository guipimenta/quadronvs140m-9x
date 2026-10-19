/*****************************************************************************
 * nv50_evo.c - EVO staging (E2) + the scanout flip (E3).
 *
 * E2 (NV_evo_stage): draws a test pattern and builds the head0 method stream in
 *   VRAM only, for readback verification. Does not touch the display engine.
 *
 * E3 (NV_evo_flip, r7): FULL display-engine re-initialisation, a faithful port
 *   of nouveau's nv50_display_init (Linux v3.4, MIT). Earlier revisions tried to
 *   submit methods to the VBIOS-handed-over core channel directly (DMA in r1-r4,
 *   PIO in r5-r6); the channel would not accept ANY command in that state
 *   (GET never advanced; PIO PENDING never cleared) - it needs the engine-init
 *   preamble first. So r7 runs that whole sequence: main-control-area setup, DAC
 *   DPMS, the text-mode step, cursor-channel activation, PIO_CTRL/INTR setup,
 *   then the EVO core channel DMA init, OBJECTS, and a ring that configures the
 *   notifier, pushes the head0 framebuffer methods, and commits with UPDATE +
 *   a sync we can wait on. The notifier dword going non-zero proves the channel
 *   actually processed our ring. Recover from a bad flip by a COLD boot (full
 *   power off) -> Safe Mode -> Standard VGA. Only ever reached from an IOCTL.
 *
 * Reverse-derived from nouveau (Linux v3.4, MIT): nv50_display.c, nv50_evo.c,
 * nouveau_object.c, nouveau_ramht.c, nouveau_display.c, nouveau_reg.h,
 * nv50_evo.h, nv50_crtc.c; cross-checked vs envytools rnndb and our live dumps.
 * See docs/EVO_PLAN.md.
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
	0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
};

static void stage_pattern(unsigned long fb)
{
	unsigned long y, x;
	for(y = 0; y < FB_H; y++)
	{
		unsigned long row = fb + y * FB_PITCH;
		for(x = 0; x < FB_W; x += 2)
		{
			unsigned short c0 = bar_colors[(x)   >> 7];
			unsigned short c1 = bar_colors[(x+1) >> 7];
			NV_vram_wr32(row + x * 2, ((unsigned long)c1 << 16) | c0);
		}
	}
}

static unsigned long push1(unsigned long push, unsigned long i,
                           unsigned long method, unsigned long data)
{
	NV_vram_wr32(push + i * 4, NV50_EVO_MTHD(method, 1)); i++;
	NV_vram_wr32(push + i * 4, data);                    i++;
	return i;
}

static unsigned long build_fb_stream(unsigned long push, unsigned long start,
                                     unsigned long fb)
{
	unsigned long i = start;
	i = push1(push, i, NV50_EVO_H_DMA_ISO(0), NVFB_ISO_HANDLE);
	NV_vram_wr32(push + i * 4, NV50_EVO_MTHD(NV50_EVO_H_FB_OFFSET(0), 2)); i++;
	NV_vram_wr32(push + i * 4, fb >> 8);                                   i++;
	NV_vram_wr32(push + i * 4, 0);                                         i++;
	i = push1(push, i, NV50_EVO_H_FB_SIZE(0),    NV50_EVO_WH(FB_W, FB_H));
	i = push1(push, i, NV50_EVO_H_FB_STORAGE(0), NV50_EVO_FB_CONFIG_LINEAR(FB_PITCH));
	i = push1(push, i, NV50_EVO_H_FB_PARAMS(0),  (NV50_EVO_FORMAT_R5G6B5 << 8));
	i = push1(push, i, NV50_EVO_UPDATE,          0);
	return i;
}

/* NV50 display RAMHT hash (nouveau_ramht.c, card_type >= NV_50). */
static unsigned long ramht_hash(unsigned long handle, unsigned long bits)
{
	unsigned long hash = 0;
	unsigned long mask = (1UL << bits) - 1UL;
	int i;
	for(i = 32; i > 0; i -= (int)bits)
	{
		hash ^= (handle & mask);
		handle >>= bits;
	}
	return hash << 3;
}

/* Build a class-0x3d display ctxdma (6 dwords) at ramin+cinst over [base,base+size). */
static void build_ctxdma(unsigned long ramin, unsigned long cinst,
                         unsigned long base, unsigned long size)
{
	unsigned long o = ramin + cinst;
	NV_vram_wr32(o + 0x00, NV50_CTXDMA_FLAGS0_VRAM_RW);  /* 0x0019003d       */
	NV_vram_wr32(o + 0x04, base + size - 1);             /* limit            */
	NV_vram_wr32(o + 0x08, base);                        /* base             */
	NV_vram_wr32(o + 0x0C, 0);
	NV_vram_wr32(o + 0x10, 0);
	NV_vram_wr32(o + 0x14, NV50_CTXDMA_FLAGS5);          /* 0x00010000       */
}

/* Add a display RAMHT entry: handle -> ctxdma at ramin-relative cinst (ch0). */
static void ramht_add(unsigned long ramin, unsigned long handle, unsigned long cinst)
{
	unsigned long slot = ramht_hash(handle, NVFB_RAMHT_BITS);
	NV_vram_wr32(ramin + NVFB_RAMHT_OFFSET + slot + 0, handle);
	NV_vram_wr32(ramin + NVFB_RAMHT_OFFSET + slot + 4, cinst << 10); /* ctx, id=0 */
}

/* Spin until (rd32(reg) & mask) == want, or max iterations. Returns iterations. */
static unsigned long wait_bits(unsigned long reg, unsigned long mask,
                               unsigned long want, unsigned long max)
{
	unsigned long g;
	for(g = 0; g < max; g++)
		if((NV_rd32(reg) & mask) == want)
			break;
	return g;
}

int NV_evo_stage(nv_evo_layout_t *out)
{
	unsigned long fb     = NVFB_FB_VRAM_OFFSET;
	unsigned long push   = NVFB_PUSH_VRAM_OFFSET;
	unsigned long notify = NVFB_NOTIFY_VRAM_OFFSET;
	unsigned long i;

	memset(out, 0, sizeof(nv_evo_layout_t));
	if(!NV_vram_wr32(fb, 0))
		return 0;
	NV_vram_fill(notify, 16, 0);
	stage_pattern(fb);
	i = build_fb_stream(push, 0, fb);

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
	return 1;
}

int NV_evo_flip(nv_evo_flip_t *out)
{
	const unsigned long ch = 0;
	unsigned long ramin  = NVFB_RAMIN_VRAM_OFFSET;
	unsigned long push   = NVFB_PUSH_VRAM_OFFSET;
	unsigned long fb     = NVFB_FB_VRAM_OFFSET;
	unsigned long notify = NVFB_NOTIFY_VRAM_OFFSET;
	unsigned long i, guard, tmp, dmacb, put;

	memset(out, 0, sizeof(nv_evo_flip_t));
	out->build      = NV_BUILD;
	out->ramin      = ramin;
	out->ctxdma     = ramin + NVFB_CTXDMA_OFFSET;
	out->pushbuf    = push;
	out->fb_offset  = fb;
	out->iso_handle = NVFB_ISO_HANDLE;
	out->stage      = NV_FLIP_STAGE_ENTER;

	if((NV_rd32(0) & 0xFFFF0000UL) == 0xDEAD0000UL)
		return 0;
	if(!NV_vram_wr32(fb, 0))
		return 0;
	out->stage = NV_FLIP_STAGE_MAPPED;

	out->pre_objects = NV_rd32(NV50_PDISP_OBJECTS);
	out->pre_ctrl    = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	out->pre_cursor0 = NV_rd32(NV50_PDISP_CURSOR_CTRL2(0));

	/* 1. framebuffer test pattern. */
	stage_pattern(fb);

	/* 2. instance memory: RAMHT + fb ctxdma (NvEvoVRAM_LP, all VRAM) + notifier
	 *    ctxdma (NvEvoSync, the 4 KiB notifier buffer). */
	NV_vram_fill(ramin, NVFB_RAMIN_SIZE / 4, 0);
	NV_vram_fill(notify, 16, 0);
	build_ctxdma(ramin, NVFB_CTXDMA_OFFSET,  0,      0x08000000UL);   /* 128 MiB */
	build_ctxdma(ramin, NVFB_CTXDMA2_OFFSET, notify, 0x00001000UL);
	ramht_add(ramin, NVFB_ISO_HANDLE,     NVFB_CTXDMA_OFFSET);
	ramht_add(ramin, NV50_EVO_NOTIFY_SYNC, NVFB_CTXDMA2_OFFSET);
	out->ctxdma_flags0 = NV50_CTXDMA_FLAGS0_VRAM_RW;
	out->ramht_slot    = ramht_hash(NVFB_ISO_HANDLE, NVFB_RAMHT_BITS);
	out->ramht_ctx     = (NVFB_CTXDMA_OFFSET << 10);
	out->stage         = NV_FLIP_STAGE_OBJECTS;

	/* 3. ENGINE PREAMBLE (nouveau nv50_display_init). */
	NV_wr32(0x00610184, NV_rd32(0x00614004));
	for(i = 0; i < 2; i++)   /* "main control area" / CRTC state copy */
	{
		NV_wr32(0x00610190 + i*0x10, NV_rd32(0x00616100 + i*0x800));
		NV_wr32(0x00610194 + i*0x10, NV_rd32(0x00616104 + i*0x800));
		NV_wr32(0x00610198 + i*0x10, NV_rd32(0x00616108 + i*0x800));
		NV_wr32(0x0061019c + i*0x10, NV_rd32(0x0061610c + i*0x800));
	}
	for(i = 0; i < 3; i++)   /* DAC */
		NV_wr32(0x006101d0 + i*0x04, NV_rd32(0x0061a000 + i*0x800));
	for(i = 0; i < 2; i++)   /* SOR (nv50_sor_nr == 2 for G86) */
		NV_wr32(0x006101e0 + i*0x04, NV_rd32(0x0061c000 + i*0x800));
	for(i = 0; i < 3; i++)   /* EXT */
		NV_wr32(0x006101f0 + i*0x04, NV_rd32(0x0061e000 + i*0x800));
	for(i = 0; i < 3; i++)
	{
		NV_wr32(NV50_PDISP_DAC_DPMS_CTRL(i), 0x00550000UL | NV50_PDISP_DAC_DPMS_PENDING);
		NV_wr32(NV50_PDISP_DAC_CLK_CTRL1(i), 0x00000001UL);
	}
	if(NV_rd32(NV50_PDISP_INTR_1) & 0x100UL)   /* "text mode" step */
	{
		NV_wr32(NV50_PDISP_INTR_1, 0x100UL);
		NV_wr32(0x006194e8, NV_rd32(0x006194e8) & ~1UL);
		(void)wait_bits(0x006194e8, 2, 0, 100000UL);
	}
	out->text_61e8 = NV_rd32(0x006194e8);

	out->cur_loops = 0;        /* cursor-channel activation (both heads) */
	for(i = 0; i < 2; i++)
	{
		NV_wr32(NV50_PDISP_CURSOR_CTRL2(i), 0x2000UL);
		out->cur_loops += wait_bits(NV50_PDISP_CURSOR_CTRL2(i),
		                            NV50_PDISP_CURSOR_CTRL2_STATUS, 0, 100000UL);
		NV_wr32(NV50_PDISP_CURSOR_CTRL2(i), NV50_PDISP_CURSOR_CTRL2_ON);
		out->cur_loops += wait_bits(NV50_PDISP_CURSOR_CTRL2(i),
		                            NV50_PDISP_CURSOR_CTRL2_STATUS,
		                            NV50_PDISP_CURSOR_CTRL2_ACTIVE, 100000UL);
	}
	out->cur0_state = NV_rd32(NV50_PDISP_CURSOR_CTRL2(0));
	out->cur1_state = NV_rd32(NV50_PDISP_CURSOR_CTRL2(1));

	NV_wr32(NV50_PDISP_PIO_CTRL,  0);
	NV_wr32(NV50_PDISP_INTR_EN_0, 0);
	NV_wr32(NV50_PDISP_INTR_EN_1, NV50_PDISP_INTR_EN_1_CLK);

	/* 4. EVO core-channel DMA init (nouveau nv50_evo_channel_init).
	 * It BEGINS with two stuck-state recovery writes. After the r7 preamble the
	 * channel sits in state 0x02xxxx - exactly the state the first conditional
	 * exists for (r7 omitted these and the channel never fetched). */
	tmp = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	out->fini_ctrl = tmp;                              /* state before recovery */
	if((tmp & 0x009F0000UL) == 0x00020000UL)
		NV_wr32(NV50_PDISP_CHAN_STAT(ch), tmp | 0x00800000UL);
	tmp = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	if((tmp & 0x003F0000UL) == 0x00030000UL)
		NV_wr32(NV50_PDISP_CHAN_STAT(ch), tmp | 0x00600000UL);
	out->fini_loops = NV_rd32(NV50_PDISP_CHAN_STAT(ch)); /* state after recovery */

	dmacb = (push >> 8) | NV50_DMA_CB_LOCATION_VRAM | NV50_DMA_CB_VALID;
	NV_wr32(NV50_PDISP_CHAN_DMA_CB(ch),   dmacb);
	NV_wr32(NV50_PDISP_CHAN_UNK2(ch),     0x00010000UL);
	NV_wr32(NV50_PDISP_CHAN_HASH_TAG(ch), ch);
	tmp = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	NV_wr32(NV50_PDISP_CHAN_STAT(ch), tmp | NV50_CHAN_CTRL_DMA_ENABLED);
	NV_wr32(NV50_PDISP_USER_PUT(ch),  0);
	NV_wr32(NV50_PDISP_CHAN_STAT(ch), NV50_CHAN_CTRL_INIT);   /* 0x01000013 */
	out->accept_loops    = wait_bits(NV50_PDISP_CHAN_STAT(ch), NV50_CHAN_CTRL_BUSY, 0, 100000UL);
	out->ctrl_after_init = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	out->dmacb_val       = dmacb;
	out->stage           = NV_FLIP_STAGE_ACCEPTED;

	/* 5. point the engine at our instance memory. */
	out->objects_val = (ramin >> 8) | 9UL;
	NV_wr32(NV50_PDISP_OBJECTS, out->objects_val);

	/* 5b. identity gamma LUT in VRAM (257 entries x 8 bytes; nouveau
	 *     nv50_crtc_lut_load format: 16-bit r,g,b at 8i+0/2/4, value = i<<6). */
	for(i = 0; i <= 256; i++)
	{
		unsigned long j = (i < 256) ? i : 255;
		unsigned long v = j << 6;
		NV_vram_wr32(NVFB_LUT_VRAM_OFFSET + i*8 + 0, v | (v << 16));
		NV_vram_wr32(NVFB_LUT_VRAM_OFFSET + i*8 + 4, v);
	}

	/* 6. build the ring: the COMPLETE head0 state. r8 proved the channel
	 *    executes our ring, but UPDATE commits the WHOLE head shadow state -
	 *    fb methods alone validate as incomplete and the commit is dropped
	 *    (ring drained, notifier silent, screen unchanged). So program the
	 *    full mode (nouveau nv50_crtc_mode_set + set_base + nv50_sor_mode_set),
	 *    with the 1024x768@60 values recovered from the live dumps:
	 *    clock 65000 kHz; raster total 1344x806 (live reg 0x610AF8 confirmed
	 *    0x03260540); panel on SOR0 -> head0 (CRTC0). */
	NV_vram_fill(push, 1024, 0);
	i = NV50_EVO_PUSH_SKIPS;
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(NV50_EVO_UNK84, 2)); i++;
	NV_vram_wr32(push + i*4, 0);                     i++;   /* NOTIFY_DISABLED */
	NV_vram_wr32(push + i*4, NV50_EVO_NOTIFY_SYNC);  i++;   /* sync ctxdma     */

	/* CLOCK (0x804, 2): 0x00800000|kHz, interlace=0. */
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x0804, 2)); i++;
	NV_vram_wr32(push + i*4, 0x00800000UL | 65000UL);   i++;
	NV_vram_wr32(push + i*4, 0);                        i++;

	/* Raster (0x810, 6): border, total, sync, blanke, blanks, vblank2.
	 * VESA 1024x768@60: 1024/1048/1184/1344, 768/771/777/806 =>
	 * hsynce 135, hblanke 295, hblanks 1319; vsynce 5, vblanke 34, vblanks 802. */
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x0810, 6)); i++;
	NV_vram_wr32(push + i*4, 0x00000000UL);             i++;  /* border       */
	NV_vram_wr32(push + i*4, 0x03260540UL);             i++;  /* 806|1344     */
	NV_vram_wr32(push + i*4, 0x00050087UL);             i++;  /* 5|135        */
	NV_vram_wr32(push + i*4, 0x00220127UL);             i++;  /* 34|295       */
	NV_vram_wr32(push + i*4, 0x03220527UL);             i++;  /* 802|1319     */
	NV_vram_wr32(push + i*4, 0x00000001UL);             i++;  /* 0|1 (no ilace)*/

	i = push1(push, i, 0x082C, 0);                       /* UNK082C          */
	i = push1(push, i, 0x0900, 0x00000311UL);            /* sync chan works  */
	i = push1(push, i, 0x08C8, 0x03000400UL);            /* REAL_RES 1024x768*/
	i = push1(push, i, 0x08D4, 0);                       /* screen position  */

	/* SOR0 -> head0 (LVDS panel; mode_ctl = CRTC0). */
	i = push1(push, i, 0x0600, 0x00000001UL);

	/* CLUT: mode ON (hires), offset, ctxdma (our all-VRAM handle). */
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x0840, 2)); i++;
	NV_vram_wr32(push + i*4, 0xC0000000UL);             i++;  /* CLUT_MODE ON */
	NV_vram_wr32(push + i*4, NVFB_LUT_VRAM_OFFSET >> 8);i++;  /* CLUT_OFFSET  */
	i = push1(push, i, 0x085C, NVFB_ISO_HANDLE);              /* CLUT_DMA     */

	/* Framebuffer: ctxdma handle, then offset/size/pitch/format as one run. */
	i = push1(push, i, 0x0874, NVFB_ISO_HANDLE);              /* FB_DMA       */
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x0860, 5)); i++;
	NV_vram_wr32(push + i*4, fb >> 8);                  i++;  /* FB_OFFSET    */
	NV_vram_wr32(push + i*4, 0);                        i++;
	NV_vram_wr32(push + i*4, 0x03000400UL);             i++;  /* FB_SIZE      */
	NV_vram_wr32(push + i*4, 0x00100800UL);             i++;  /* linear|2048  */
	NV_vram_wr32(push + i*4, 0x0000E800UL);             i++;  /* R5G6B5       */
	i = push1(push, i, 0x08C0, 0);                            /* FB_POS 0,0   */

	/* Dither off; scaler inactive at native res (output size = 1024x768). */
	i = push1(push, i, 0x08A0, 0);
	i = push1(push, i, 0x08A4, 0);
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x08D8, 2)); i++;
	NV_vram_wr32(push + i*4, 0x03000400UL);             i++;  /* SCALE_RES1   */
	NV_vram_wr32(push + i*4, 0x03000400UL);             i++;  /* SCALE_RES2   */

	/* Commit with completion notify (nouveau nv50_display_sync). */
	i = push1(push, i, 0x0084, 0x80000000UL);                 /* NOTIFY en    */
	i = push1(push, i, 0x0080, 0);                            /* UPDATE       */
	i = push1(push, i, 0x0084, 0);                            /* NOTIFY dis   */
	put = i * 4;
	out->put_bytes = put;

	/* 7. clear the notifier, fire (advance PUT), wait for the notifier to post
	 *    (the engine processed our ring) or GET to catch PUT or a FIFO error. */
	NV_vram_wr32(notify, 0);
	out->ntfy_pre = NV_vram_rd32(notify);
	NV_wr32(NV50_PDISP_INTR_0, 0x00010000UL << ch);
	NV_wr32(NV50_PDISP_TRAPPED_ADDR, 0x90000000UL);
	NV_wr32(NV50_PDISP_USER_PUT(ch), put);

	/* The commit is a SUPERVISED, staged operation: the engine raises INTR_1
	 * CLK_UNK10/20/40 mid-UPDATE and waits for the driver to service and ack
	 * each stage (nouveau nv50_display_bh + unk10/20/40 handlers; c7/c8 showed
	 * our r9 commit parked at UNK10 with 0x610030=0x2A0). Our clock and SOR
	 * link are already running at exactly the committed mode, so the VBIOS
	 * script / PLL work in each stage is a no-op here and the stages reduce to
	 * their acks. Service them while waiting for the completion notifier. */
	for(guard = 0; guard < 2000000UL; guard++)
	{
		unsigned long sv = NV_rd32(NV50_PDISP_INTR_1);
		if(sv & 0x00000010UL)            /* UNK10 */
		{
			NV_wr32(0x00619494UL, NV_rd32(0x00619494UL) & ~8UL);
			NV_wr32(NV50_PDISP_INTR_1, 0x00000010UL);
			NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
			out->sv10++;
		}
		else if(sv & 0x00000020UL)       /* UNK20 (clock/encoder: unchanged) */
		{
			NV_wr32(NV50_PDISP_INTR_1, 0x00000020UL);
			NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
			out->sv20++;
		}
		else if(sv & 0x00000040UL)       /* UNK40 */
		{
			NV_wr32(NV50_PDISP_INTR_1, 0x00000040UL);
			NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
			NV_wr32(0x00619494UL, NV_rd32(0x00619494UL) | 8UL);
			out->sv40++;
		}
		if(NV_vram_rd32(notify) != 0)
			break;
		if(NV_rd32(NV50_PDISP_INTR_0) & (0x00010000UL << ch))
			break;
	}
	out->sync_loops = guard;
	out->ntfy_post  = NV_vram_rd32(notify);

	/* 8. post-flip snapshot. */
	out->post_ctrl    = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	out->post_put     = NV_rd32(NV50_PDISP_USER_PUT(ch));
	out->post_get     = NV_rd32(NV50_PDISP_USER_GET(ch));
	out->trapped_addr = NV_rd32(NV50_PDISP_TRAPPED_ADDR);
	out->trapped_data = NV_rd32(NV50_PDISP_TRAPPED_DATA);
	out->intr0        = NV_rd32(NV50_PDISP_INTR_0);
	out->intr1        = NV_rd32(NV50_PDISP_INTR_1);
	out->post_objects = NV_rd32(NV50_PDISP_OBJECTS);
	out->stage        = NV_FLIP_STAGE_DONE;
	out->ok           = 1;

	dbg_printf("NV_evo_flip(r7): ntfy=%lX get=%lX put=%lX cur0=%lX trap=%lX\n",
		out->ntfy_post, out->post_get, out->post_put, out->cur0_state,
		out->trapped_addr);
	return 1;
}
