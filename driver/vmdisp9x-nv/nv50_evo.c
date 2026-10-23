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

/* ---- boot progress log (VRAM 25 MB) ------------------------------------- *
 * Each step writes a code to a fresh slot; VRAM survives a warm reset, so after
 * a boot hang we reset -> Safe Mode -> nvprobe bootlog reads how far we got.
 * The last nonzero code is the last step reached before the hang. */
#define NVFB_BOOTLOG_OFFSET  0x01900000UL
#define NVFB_BOOTLOG_MAGIC   0xB0070100UL
static unsigned long nv_blog_idx = 0;

void NV_blog(unsigned long code)
{
	/* The boot log lives in BAR1 VRAM; touching BAR1 during Device_Init (inside
	 * the VMM critical section, GPU not ready) hard-freezes the G86. Skip the
	 * write in the same early-init window where NV_probe skips BAR0 MMIO. */
	if(nv_probe_skip_mmio)
	{
		nv_blog_idx++;
		return;
	}
	if(nv_blog_idx < 62)
		NV_vram_wr32(NVFB_BOOTLOG_OFFSET + nv_blog_idx * 4, code);
	nv_blog_idx++;
}

void NV_blog_reset(void)
{
	nv_blog_idx = 0;
	if(nv_probe_skip_mmio)
		return;
	NV_vram_fill(NVFB_BOOTLOG_OFFSET, 64, 0);
	NV_blog(NVFB_BOOTLOG_MAGIC);
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

/* Once the engine has been taken over, the channel stays alive: later mode sets
 * append to the ring instead of re-running the init (nouveau's operating model;
 * re-running the full init on our own live channel is unproven). nv_ring_pos is
 * the dword index just past the last drained submission. */
static unsigned long nv_owned    = 0;
static unsigned long nv_ring_pos = 0;
static unsigned long nv_objects_val = 0;   /* PDISP_OBJECTS we programmed */
static long          nv_resume_ticks = 0;  /* M4.5: countdown to the post-resume check */

/* M4.5: did somebody else (VBIOS POST after a resume, INT 10h after a
 * full-screen DOS box) reprogram the display engine behind our back? Then our
 * core channel, RAMHT and ring are gone and the next modeset must run the full
 * init again instead of appending to a ring nobody executes. */
int NV_hw_lost(void)
{
	unsigned long obj, stat;
	if(!nv_owned)
		return 0;
	obj  = NV_rd32(NV50_PDISP_OBJECTS);
	stat = NV_rd32(NV50_PDISP_CHAN_STAT(0));
	dbg_printf("NV_hw_lost? objects %lX (ours %lX) ch0 stat %lX\n", obj, nv_objects_val, stat);
	if(obj == nv_objects_val && (stat & NV50_CHAN_CTRL_DMA_ENABLED))
		return 0;
	dbg_printf("NV: display engine state LOST (objects %lX, ours %lX, ch0 stat %lX) - full re-init on the next modeset\n",
		obj, nv_objects_val, stat);
	nv_owned    = 0;
	nv_ring_pos = 0;
	return 1;
}

void NV_power_notify(unsigned long state)
{
	dbg_printf("NV_power_notify: adapter power state %lX (1=D0 2=D1 4=D2 8=D3)\n", state);
	if(state == 1)
		nv_resume_ticks = 128;   /* ~2 s of blit-timer ticks, then NV_display_resume_check */
}

int NV_resume_tick(void)
{
	if(nv_resume_ticks > 0 && --nv_resume_ticks == 0)
		return 1;
	return 0;
}

/* Service the supervisor handshake stages the engine raises mid-UPDATE
 * (nouveau nv50_display_bh + unk10/20/40 handlers). Our clock and SOR link
 * never change, so each stage reduces to its ack. `out` (may be NULL) counts
 * the stages seen. */
static void evo_service_sv(nv_evo_flip_t *out)
{
	unsigned long sv = NV_rd32(NV50_PDISP_INTR_1);
	if(sv & 0x00000010UL)            /* UNK10 */
	{
		NV_wr32(0x00619494UL, NV_rd32(0x00619494UL) & ~8UL);
		NV_wr32(NV50_PDISP_INTR_1, 0x00000010UL);
		NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
		if(out) out->sv10++;
	}
	else if(sv & 0x00000020UL)       /* UNK20 (clock/encoder: unchanged) */
	{
		NV_wr32(NV50_PDISP_INTR_1, 0x00000020UL);
		NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
		if(out) out->sv20++;
	}
	else if(sv & 0x00000040UL)       /* UNK40 */
	{
		NV_wr32(NV50_PDISP_INTR_1, 0x00000040UL);
		NV_wr32(NV50_PDISP_UNK30, 0x80000000UL);
		NV_wr32(0x00619494UL, NV_rd32(0x00619494UL) | 8UL);
		if(out) out->sv40++;
	}
}

/* Reserve `nd` dwords in the live core ring (nouveau evo_wait): when the 4 KiB
 * ring would overflow, write a JUMP-to-0 at the current position, reset PUT to
 * 0 and wait for GET to follow. Returns the dword index to write at, or -1. */
static long evo_ring_begin(unsigned long nd)
{
	unsigned long g;
	if(!nv_owned)
		return -1;
	if(nv_ring_pos + nd + 1 > 1000)
	{
		NV_vram_wr32(NVFB_PUSH_VRAM_OFFSET + nv_ring_pos * 4, NV50_EVO_JUMP0);
		NV_wr32(NV50_PDISP_USER_PUT(0), 0);
		for(g = 0; g < 200000UL; g++)
		{
			evo_service_sv(NULL);
			if(NV_rd32(NV50_PDISP_USER_GET(0)) == 0)
				break;
		}
		if(g == 200000UL)
		{
			dbg_printf("evo_ring_begin: wrap timeout, GET=%lX\n", NV_rd32(NV50_PDISP_USER_GET(0)));
			return -1;
		}
		nv_ring_pos = 0;
	}
	return (long)nv_ring_pos;
}

/* Submit everything up to dword index `i` and wait for the engine to fetch it
 * (GET == PUT). Returns 1 on success, 0 on timeout (ring position is kept
 * either way, PUT was advanced). */
static int evo_ring_fire(unsigned long i)
{
	unsigned long put = i * 4, g;
	NV_wr32(NV50_PDISP_USER_PUT(0), put);
	nv_ring_pos = i;
	for(g = 0; g < 200000UL; g++)
	{
		evo_service_sv(NULL);
		if(NV_rd32(NV50_PDISP_USER_GET(0)) == put)
			return 1;
	}
	dbg_printf("evo_ring_fire: timeout, PUT=%lX GET=%lX\n", put, NV_rd32(NV50_PDISP_USER_GET(0)));
	return 0;
}

/*
 * NV_evo_takeover --
 *
 *   The complete native display takeover proven by E3 r10, parameterised:
 *   scan out a linear w*h surface (16 or 32 bpp, pitch = w*bpp/8) at VRAM byte
 *   offset `fb`. The panel raster stays the fixed 1024x768@60 LVDS mode, so
 *   only w=1024 h=768 is accepted for now. When `pattern` is nonzero the 8-bar
 *   test image is drawn into the surface first (the nvprobe test path); the
 *   display-driver path leaves the surface contents alone.
 */
/* M4.1: the desktop modes the native path offers. The panel raster is always
 * the fixed 1024x768@60 LVDS timing; smaller framebuffers are stretched to it
 * by the head's scaler (nouveau nv50_crtc_set_scale: SCALE_CTRL=9, SCALE_RES =
 * output size, 0x8C8 = input size). 8 bpp is a 32 bpp scanout surface fed by
 * the shadow blitter's palette expansion (readpx8), so the hardware never sees
 * an indexed surface and no CLUT programming is needed. */
int NV_mode_ok(unsigned long w, unsigned long h, unsigned long bpp)
{
	if(bpp != 8 && bpp != 16 && bpp != 32)
		return 0;
	if((w == 1024 && h == 768) || (w == 800 && h == 600) || (w == 640 && h == 480))
		return 1;
	return 0;
}

/* Hardware depth used for a desktop depth: 8 bpp desktops scan out as 32 bpp. */
unsigned long NV_hw_bpp(unsigned long bpp)
{
	return (bpp == 8) ? 32UL : bpp;
}

/* Scanout pitch for w pixels at the hardware depth. NV50 linear surfaces need
 * a 256-byte multiple (nouveau_fbcon: roundup(pitch, 256)); 800x600 at 16/32
 * bpp (1600/3200 bytes) is not, and showed as a sheared, garbage screen. */
unsigned long NV_hw_pitch(unsigned long w, unsigned long bpp)
{
	unsigned long pitch = w * (NV_hw_bpp(bpp) / 8);
	return (pitch + 255UL) & ~255UL;
}

static unsigned long nv_cur_mode_w = 1024, nv_cur_mode_h = 768; /* live mode (cursor scaling) */

static int NV_evo_takeover(nv_evo_flip_t *out, unsigned long w, unsigned long h,
                           unsigned long bpp, unsigned long fb, int pattern)
{
	const unsigned long ch = 0;
	unsigned long ramin  = NVFB_RAMIN_VRAM_OFFSET;
	unsigned long push   = NVFB_PUSH_VRAM_OFFSET;
	unsigned long notify = NVFB_NOTIFY_VRAM_OFFSET;
	unsigned long hwbpp  = NV_hw_bpp(bpp);
	unsigned long pitch  = NV_hw_pitch(w, bpp);               /* 256-byte aligned     */
	unsigned long wh     = NV50_EVO_WH(w, h);                 /* framebuffer / input  */
	unsigned long owh    = NV50_EVO_WH(1024UL, 768UL);        /* panel output         */
	unsigned long scale  = (w == 1024 && h == 768) ? 0UL : 9UL; /* SCALE_CTRL        */
	unsigned long depth  = (hwbpp == 32) ? 0x0000CF00UL : 0x0000E800UL;
	unsigned long i, guard, tmp, dmacb, put;

	memset(out, 0, sizeof(nv_evo_flip_t));
	out->build      = NV_BUILD;
	out->ramin      = ramin;
	out->ctxdma     = ramin + NVFB_CTXDMA_OFFSET;
	out->pushbuf    = push;
	out->fb_offset  = fb;
	out->iso_handle = NVFB_ISO_HANDLE;
	out->stage      = NV_FLIP_STAGE_ENTER;

	if(!NV_mode_ok(w, h, bpp))
		return 0;
	if((NV_rd32(0) & 0xFFFF0000UL) == 0xDEAD0000UL)
		return 0;
	if(!NV_vram_wr32(fb + pitch * h - 4, NV_vram_rd32(fb + pitch * h - 4)))
		return 0;
	out->stage = NV_FLIP_STAGE_MAPPED;
	NV_blog(0x200);

	out->pre_objects = NV_rd32(NV50_PDISP_OBJECTS);
	out->pre_ctrl    = NV_rd32(NV50_PDISP_CHAN_STAT(ch));
	out->pre_cursor0 = NV_rd32(NV50_PDISP_CURSOR_CTRL2(0));

	/* 1. optional test pattern (never for the display-driver path). */
	if(pattern)
		stage_pattern(fb);
	NV_blog(0x201);

	NV_hw_lost();   /* M4.5: drop a stale claim before deciding append vs. full init */
	if(nv_owned)
	{
		/* Engine already ours: just append the new head state to the live
		 * ring (wrapping it with a jump when the 4 KiB are used up). */
		long ri;
		dbg_printf("NV_evo_takeover: append path, ring_pos=%lu ch0 stat=%lX get=%lX put=%lX\n",
			nv_ring_pos, NV_rd32(NV50_PDISP_CHAN_STAT(ch)),
			NV_rd32(NV50_PDISP_USER_GET(ch)), NV_rd32(NV50_PDISP_USER_PUT(ch)));
		ri = evo_ring_begin(96);
		if(ri < 0)
			return 0;
		i = (unsigned long)ri;
		goto build_ring;
	}

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
	NV_blog(0x202);

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
	NV_blog(0x203);

	/* 5. point the engine at our instance memory. */
	out->objects_val = (ramin >> 8) | 9UL;
	NV_wr32(NV50_PDISP_OBJECTS, out->objects_val);
	NV_blog(0x204);

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

build_ring:
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
	i = push1(push, i, 0x08C8, wh);                      /* REAL_RES         */
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
	NV_vram_wr32(push + i*4, wh);                       i++;  /* FB_SIZE      */
	NV_vram_wr32(push + i*4, 0x00100000UL | pitch);     i++;  /* linear|pitch */
	NV_vram_wr32(push + i*4, depth);                    i++;  /* pixel format */
	i = push1(push, i, 0x08C0, 0);                            /* FB_POS 0,0   */

	/* Dither off. Scaler: inactive at the panel's native 1024x768, otherwise
	 * ACTIVE (9) stretching the w x h framebuffer to the 1024x768 output
	 * (nouveau nv50_crtc_set_scale; 0x8C8 above carries the input size). */
	i = push1(push, i, 0x08A0, 0);
	i = push1(push, i, 0x08A4, scale);
	NV_vram_wr32(push + i*4, NV50_EVO_MTHD(0x08D8, 2)); i++;
	NV_vram_wr32(push + i*4, owh);                      i++;  /* SCALE_RES1   */
	NV_vram_wr32(push + i*4, owh);                      i++;  /* SCALE_RES2   */

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
	NV_blog(0x205);

	/* The commit is a SUPERVISED, staged operation: the engine raises INTR_1
	 * CLK_UNK10/20/40 mid-UPDATE and waits for the driver to service and ack
	 * each stage (nouveau nv50_display_bh + unk10/20/40 handlers; c7/c8 showed
	 * our r9 commit parked at UNK10 with 0x610030=0x2A0). Our clock and SOR
	 * link are already running at exactly the committed mode, so the VBIOS
	 * script / PLL work in each stage is a no-op here and the stages reduce to
	 * their acks. Service them while waiting for the completion notifier. */
	/* M4.5: a normal append drains in a few iterations; cap it so a stalled
	 * append fails fast and the caller can re-init. The full first takeover
	 * needs the long count for the real supervisor handshake. */
	{
	unsigned long guard_max = nv_owned ? 300000UL : 2000000UL;
	for(guard = 0; guard < guard_max; guard++)
	{
		evo_service_sv(out);
		if(NV_vram_rd32(notify) != 0)
			break;
		if(NV_rd32(NV50_PDISP_INTR_0) & (0x00010000UL << ch))
			break;
	}
	}
	out->sync_loops = guard;
	out->ntfy_post  = NV_vram_rd32(notify);
	NV_blog(0x206);

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

	/* 3032: PUT was advanced either way, so a ring we already own must keep
	 * tracking it. But only CLAIM the engine when this commit completed
	 * (notifier posted, no channel error): a timed-out/trapped first takeover
	 * leaves nv_owned=0 so the retry re-runs the full init (channel reset)
	 * instead of appending to a stalled channel. */
	if(nv_owned)
	{
		if(out->ntfy_post != 0)
		{
			nv_ring_pos = i;
		}
		else
		{
			/* M4.5: the appended UPDATE never drained -> the engine was reset
			 * behind us (screen switch / DPMS / resume) into a state that reads
			 * DMA_ENABLED but will not fetch. Drop the claim so the caller
			 * re-runs the full init (which re-does the stuck-state recovery). */
			dbg_printf("NV_evo_takeover: append stalled (get=%lX put=%lX), dropping engine claim\n",
				out->post_get, out->post_put);
			nv_owned    = 0;
			nv_ring_pos = 0;
		}
	}
	else if(out->ntfy_post != 0 && (out->intr0 & (0x00010000UL << ch)) == 0)
	{
		nv_owned       = 1;
		nv_ring_pos    = i;
		nv_objects_val = out->objects_val;
	}
	if(nv_owned && out->ntfy_post != 0)
	{
		nv_cur_mode_w = w;
		nv_cur_mode_h = h;
	}

	dbg_printf("NV_evo_takeover: %lux%lux%lu fb=%lX ntfy=%lX get=%lX put=%lX\n",
		w, h, bpp, fb, out->ntfy_post, out->post_get, out->post_put);
	return 1;
}

/* nvprobe test command: takeover + 8-bar pattern at the scratch fb (16 MB). */
int NV_evo_flip(nv_evo_flip_t *out)
{
	return NV_evo_takeover(out, 1024UL, 768UL, 16UL, NVFB_FB_VRAM_OFFSET, 1);
}

/* Can the native path drive w x h x bpp? (G86 present + the panel's fixed
 * 1024x768 raster; 16 or 32 bpp linear.) */
/* Boot/runtime diagnostics for the display-driver path: the last native
 * mode-set's full diagnostic block plus call counters, readable later via
 * OP_NV_LASTSET even after a failed boot-time attempt (Windows falls back to
 * VGA but the VxD stays loaded). */
static nv_evo_flip_t nv_last_diag = {0};
static unsigned long nv_cnt_valid = 0;   /* NV_native_valid() hits          */
static unsigned long nv_cnt_set   = 0;   /* NV_native_modeset() attempts    */
static unsigned long nv_cnt_ok    = 0;   /* ...of which succeeded           */
static unsigned long nv_last_w = 0, nv_last_h = 0, nv_last_bpp = 0;

int NV_native_valid(unsigned long w, unsigned long h, unsigned long bpp)
{
	if(!nv_hwinfo.found || !nv_hwinfo.mmio_ok || nv_hwinfo.chipset != 0x86)
		return 0;
	if(NV_mode_ok(w, h, bpp))
	{
		nv_cnt_valid++;
		return 1;
	}
	return 0;
}

/* Written by the vxd_vesa.c hooks: how far the display driver's mode path got.
 * nv_dbg_branch: 0=hook never evaluated, 1=NV_native_valid said no,
 * 2=vram_phylin was NULL, 3=native modeset attempted. */
unsigned long nv_dbg_valid_calls = 0;  /* VESA_validmode() total calls       */
unsigned long nv_dbg_set_calls   = 0;  /* VESA_setmode_phy() total calls     */
unsigned long nv_dbg_set_w = 0, nv_dbg_set_h = 0, nv_dbg_set_bpp = 0;
unsigned long nv_dbg_branch      = 0;
unsigned long nv_dbg_phylin      = 0;  /* hda->vram_phylin at the hook       */
unsigned long nv_dbg_init        = 0;  /* backend init: 1=VBE, 2=native      */
unsigned long nv_dbg_fbhda       = 0;  /* OP_FBHDA_SETUP calls (=nvmini.drv loaded) */

void NV_native_lastdiag(nv_evo_flip_t *out, unsigned long *cnts)
{
	memcpy(out, &nv_last_diag, sizeof(nv_evo_flip_t));
	cnts[0]  = nv_cnt_valid;
	cnts[1]  = nv_cnt_set;
	cnts[2]  = nv_cnt_ok;
	cnts[3]  = nv_last_w;
	cnts[4]  = nv_last_h;
	cnts[5]  = nv_last_bpp;
	cnts[6]  = nv_dbg_valid_calls;
	cnts[7]  = nv_dbg_set_calls;
	cnts[8]  = nv_dbg_set_w;
	cnts[9]  = nv_dbg_set_h;
	cnts[10] = nv_dbg_set_bpp;
	cnts[11] = nv_dbg_branch;
	cnts[12] = nv_dbg_phylin;
	cnts[13] = nv_dbg_init;
	cnts[14] = nv_dbg_fbhda;
	cnts[15] = 0;
}

/* Display-driver mode set: native takeover scanning out VRAM offset 0, where
 * the vmdisp9x shadow blitter writes the desktop. No test pattern. Succeeds
 * only when the engine confirmed the commit (completion notifier posted). */
/* E4 panel diagnostic flag (vxd_vesa.c): 1 => draw the 8-bar test pattern into
 * the scanout buffer during the boot modeset, so the panel itself reports
 * whether the .drv reached our setmode. 0 => normal (leave the surface to the
 * shadow blitter). Set via HKLM\...\vesa "BootPattern". */
extern unsigned long nv_boot_pattern;

int NV_native_modeset(unsigned long w, unsigned long h, unsigned long bpp)
{
	int ok;
	nv_cnt_set++;
	nv_last_w = w; nv_last_h = h; nv_last_bpp = bpp;
	/* M4.3: never program EVO structures into VRAM we have not confirmed exists
	 * (live VM context here, so the PFB read is safe). */
	if(!NV_vram_check())
	{
		dbg_printf("NV_native_modeset: refused, VRAM layout check failed\n");
		return 0;
	}
	{
		unsigned long was_owned;
		NV_hw_lost();
		was_owned = nv_owned;
		ok = NV_evo_takeover(&nv_last_diag, w, h, bpp, 0, nv_boot_pattern ? 1 : 0);
		if(ok && nv_last_diag.ntfy_post != 0)
		{
			nv_cnt_ok++;
			dbg_printf("NV_native_modeset %lux%lux%lu OK%s\n", w, h, bpp,
				was_owned ? "" : " (full engine init)");
			if(!was_owned)
				NV_cursor_restore();   /* image + visibility were lost with the engine */
			return 1;
		}
		/* M4.5: an append that stalled just dropped nv_owned - retry once from
		 * scratch (full RAMHT/channel/preamble/head rebuild). */
		if(was_owned && !nv_owned)
		{
			dbg_printf("NV_native_modeset: append stalled, retrying with full engine init\n");
			ok = NV_evo_takeover(&nv_last_diag, w, h, bpp, 0, nv_boot_pattern ? 1 : 0);
			if(ok && nv_last_diag.ntfy_post != 0)
			{
				nv_cnt_ok++;
				dbg_printf("NV_native_modeset %lux%lux%lu OK (full init after stall)\n", w, h, bpp);
				NV_cursor_restore();
				return 1;
			}
		}
	}
	dbg_printf("NV_native_modeset %lux%lux%lu FAILED stage=%lu ntfy=%lX\n",
		w, h, bpp, nv_last_diag.stage, nv_last_diag.ntfy_post);
	return 0;
}

/* ====================================================================== *
 *  M4.4 - hardware cursor (head0), nouveau nv50_cursor.c / nv50_crtc.c   *
 * ====================================================================== */
unsigned long nv_hw_cursor       = 1;
unsigned long nv_hw_cursor_scale = 0;
static unsigned long nv_cur_valid   = 0;   /* image in VRAM                 */
static unsigned long nv_cur_visible = 0;   /* CURSOR_CTRL = SHOW committed  */
static unsigned long nv_cur_want    = 0;   /* M4.5: GDI wants it shown      */
static unsigned long nv_cur_img[64*64];    /* M4.5: ARGB copy for re-upload */
static long          nv_cur_x = 0, nv_cur_y = 0;
static unsigned long nv_cur_dbg = 0;       /* log the first few operations  */

int NV_cursor_hw(void)
{
	return (nv_hw_cursor && nv_owned && nv_hwinfo.found && nv_hwinfo.mmio_ok) ? 1 : 0;
}

/* SHOW/HIDE through the core channel + UPDATE (nouveau nv50_cursor_show/hide,
 * chipset != 0x50 flavour: CURSOR_DMA handle before SHOW, none after HIDE). */
static int cursor_ctrl(int show)
{
	unsigned long push = NVFB_PUSH_VRAM_OFFSET;
	unsigned long i;
	long ri = evo_ring_begin(12);
	if(ri < 0)
		return 0;
	i = (unsigned long)ri;
	if(show)
	{
		i = push1(push, i, NV50_EVO_H_DMA_CURSOR(0), NVFB_ISO_HANDLE);
		NV_vram_wr32(push + i*4, NV50_EVO_MTHD(NV50_EVO_H_CTRL_CURSOR(0), 2)); i++;
		NV_vram_wr32(push + i*4, NV50_EVO_CURSOR_CTRL_SHOW);                  i++;
		NV_vram_wr32(push + i*4, NVFB_CURSOR_VRAM_OFFSET >> 8);               i++;
	}
	else
	{
		NV_vram_wr32(push + i*4, NV50_EVO_MTHD(NV50_EVO_H_CTRL_CURSOR(0), 2)); i++;
		NV_vram_wr32(push + i*4, NV50_EVO_CURSOR_CTRL_HIDE);                  i++;
		NV_vram_wr32(push + i*4, 0);                                          i++;
		i = push1(push, i, NV50_EVO_H_DMA_CURSOR(0), 0);
	}
	i = push1(push, i, NV50_EVO_UPDATE, 0);
	return evo_ring_fire(i);
}

void NV_cursor_move(long x, long y)
{
	long px = x, py = y;
	nv_cur_x = x;
	nv_cur_y = y;
	if(!NV_cursor_hw())
		return;
	if(nv_hw_cursor_scale && (nv_cur_mode_w != 1024 || nv_cur_mode_h != 768))
	{
		px = (x * 1024L) / (long)nv_cur_mode_w;
		py = (y * 768L)  / (long)nv_cur_mode_h;
	}
	NV_wr32(NV50_PDISP_CURSOR_POS(0), ((unsigned long)(py & 0xFFFF) << 16) | (unsigned long)(px & 0xFFFF));
	NV_wr32(NV50_PDISP_CURSOR_POS_CTRL(0), 0);   /* "needed to make the cursor move" */
}

int NV_cursor_load(const unsigned long *andmask, const unsigned long *xormask,
                   unsigned long w, unsigned long h, unsigned long stride,
                   long x, long y)
{
	unsigned long cx, cy, opaque = 0;
	int r;
	if(!NV_cursor_hw())
		return 0;
	if(w > 64) w = 64;
	if(h > 64) h = 64;
	for(cy = 0; cy < 64; cy++)
	{
		for(cx = 0; cx < 64; cx++)
		{
			unsigned long px = 0;                      /* transparent */
			if(cx < w && cy < h)
			{
				unsigned long a = andmask[cy * stride + cx];
				unsigned long o = xormask[cy * stride + cx];
				if(a == 0)
				{
					px = 0xFF000000UL | (o & 0x00FFFFFFUL);   /* opaque colour */
					opaque++;
				}
				else if(o != 0)
				{
					px = 0xFF000000UL;                        /* invert -> black */
					opaque++;
				}
			}
			NV_vram_wr32(NVFB_CURSOR_VRAM_OFFSET + (cy * 64 + cx) * 4, px);
			nv_cur_img[cy * 64 + cx] = px;
		}
	}
	nv_cur_valid = (opaque != 0) ? 1 : 0;
	NV_cursor_move(x, y);
	if(nv_cur_valid)
	{
		r = cursor_ctrl(1);
		nv_cur_visible = r ? 1 : 0;
		nv_cur_want    = 1;
	}
	else
	{
		r = nv_cur_visible ? cursor_ctrl(0) : 1;
		nv_cur_visible = 0;
		nv_cur_want    = 0;
	}
	if(nv_cur_dbg < 8)
	{
		nv_cur_dbg++;
		dbg_printf("NV_cursor_load: %lux%lu opaque=%lu at %ld,%ld -> %s\n",
			w, h, opaque, x, y, r ? "OK" : "FAILED");
	}
	return r;
}

/* Returns 1 when a hardware cursor is (now) shown, 0 when the caller should
 * fall back to the software cursor. */
/* M4.5: after a full engine re-init the cursor surface in VRAM and the head's
 * cursor state are gone; put both back from our copies. */
void NV_cursor_restore(void)
{
	unsigned long i;
	if(!NV_cursor_hw() || !nv_cur_valid)
		return;
	for(i = 0; i < 64*64; i++)
		NV_vram_wr32(NVFB_CURSOR_VRAM_OFFSET + i * 4, nv_cur_img[i]);
	nv_cur_visible = 0;
	NV_cursor_move(nv_cur_x, nv_cur_y);
	if(nv_cur_want)
		nv_cur_visible = cursor_ctrl(1) ? 1 : 0;
	dbg_printf("NV_cursor_restore: image re-uploaded, %s\n", nv_cur_visible ? "shown" : "hidden");
}

int NV_cursor_show(void)
{
	if(!NV_cursor_hw() || !nv_cur_valid)
		return 0;
	nv_cur_want = 1;
	if(nv_cur_visible)
		return 1;
	NV_cursor_move(nv_cur_x, nv_cur_y);
	if(cursor_ctrl(1))
	{
		nv_cur_visible = 1;
		return 1;
	}
	return 0;
}

void NV_cursor_hide(int invalidate)
{
	nv_cur_want = 0;
	if(NV_cursor_hw() && nv_cur_visible)
	{
		cursor_ctrl(0);
		nv_cur_visible = 0;
	}
	if(invalidate)
	{
		nv_cur_valid   = 0;
		nv_cur_visible = 0;
	}
}
