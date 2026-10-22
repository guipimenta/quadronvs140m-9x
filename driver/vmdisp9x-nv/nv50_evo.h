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

/* FB_STORAGE / "FB_CONFIG" (method 0x86c). Two encodings depending on layout:
 *  - BLOCK-LINEAR: MEMORY_LAYOUT bit20 = 0, PITCH field [17:8] = (pitch_bytes>>4).
 *  - LINEAR (what we scan out): MEMORY_LAYOUT bit20 = 1 (G80_HEAD_STORAGE_LAYOUT
 *    PITCH=1), and the pitch is the RAW byte pitch in the low bits. This matches
 *    nouveau's nouveau_display.c: r_pitch = 0x00100000 | fb->pitch for an untiled
 *    surface (NV_50..NV_D0). Confirmed against rnndb nv_evo.xml + nv50_crtc.c. */
#define NV50_EVO_STORAGE_BLOCKLINEAR(bytes) ((((bytes) >> 4) & 0x3ff) << 8)
#define NV50_EVO_FB_CONFIG_LINEAR(bytes)    (0x00100000UL | ((bytes) & 0xffffUL))

/* EVO pushbuffer method header: method dword = (count<<18) | (method & 0x3ffff). */
#define NV50_EVO_MTHD(method, count)  (((count) << 18) | ((method) & 0x3ffff))

/* wh15 / xy15 packing: width/x in low 16, height/y in high 16. */
#define NV50_EVO_WH(w, h)             (((w) & 0xffff) | (((h) & 0xffff) << 16))

/* Handle for the framebuffer ctxdma. nouveau uses NvEvoVRAM_LP (0x01000003) for
 * a linear/untiled scanout surface; we register exactly that handle in our EVO
 * RAMHT (see NV_evo_flip) and reference it from the FB_DMA method. */
#define NVFB_ISO_HANDLE              0x01000003UL   /* = NvEvoVRAM_LP */

/* ====================================================================== *
 *  E3 - EVO core-channel bring-up (MMIO BAR0 registers, NOT pushbuffer)  *
 *  Offsets/flags from nouveau nv50 (Linux v3.4, MIT): nouveau_reg.h,     *
 *  nv50_evo.c, nv50_display.c, nouveau_object.c, nouveau_ramht.c.        *
 *  OBJECTS, INTR_0/1, CHAN_STAT, CHAN_DMA_CB, TRAPPED_ADDR/DATA and      *
 *  RAM_AMOUNT live in nv50_disp.h; only the channel control registers it *
 *  does not cover are defined here. CHAN_STAT is both STAT (read) and    *
 *  CTRL (write) at 0x610200+i*0x10.                                      *
 * ====================================================================== */
#define NV50_PDISP_UNK30             0x00610030UL   /* UNK30_CTRL (clock ack)  */
#define NV50_PDISP_CHAN_UNK2(i)      (0x00610208UL + (i)*0x10)

/* Engine-init preamble registers (nouveau nv50_display_init). */
#define NV50_PDISP_PIO_CTRL          0x00610300UL   /* = CTRL_STATE(0)         */
#define NV50_PDISP_CURSOR_CTRL2(i)   (0x00610270UL + (i)*0x10)
#define NV50_PDISP_CURSOR_CTRL2_ON       0x00000001UL
#define NV50_PDISP_CURSOR_CTRL2_STATUS   0x00030000UL
#define NV50_PDISP_CURSOR_CTRL2_ACTIVE   0x00010000UL
#define NV50_PDISP_DAC_DPMS_CTRL(i)  (0x0061A004UL + (i)*0x800)
#define NV50_PDISP_DAC_DPMS_PENDING      0x80000000UL
#define NV50_PDISP_DAC_CLK_CTRL1(i)  (0x0061A010UL + (i)*0x800)
#define NV50_PDISP_INTR_EN_1_CLK     0x00000070UL   /* CLK_UNK10|20|40         */
#define NV50_EVO_NOTIFY_SYNC         0xCAFE0000UL   /* NvEvoSync ctxdma handle */
/* M4.4 hardware cursor (nouveau nv50_cursor.c): head method 0x880 CURSOR_CTRL
 * (SHOW = enable | 64x64 A8R8G8B8, HIDE), 0x884 = VRAM offset >> 8, 0x89C =
 * ctxdma handle (G84+). Position goes through the head's cursor PIO channel
 * user area (channel 7+head at 0x647000): POS then a 0 to POS_CTRL. */
#define NV50_EVO_CURSOR_CTRL_SHOW    0x85000000UL
#define NV50_EVO_CURSOR_CTRL_HIDE    0x05000000UL
#define NV50_PDISP_CURSOR_POS_CTRL(h) (0x00647080UL + (h)*0x1000UL)
#define NV50_PDISP_CURSOR_POS(h)      (0x00647084UL + (h)*0x1000UL)
#define NV50_EVO_JUMP0               0x20000000UL   /* ring: jump to offset 0  */
#define NV50_EVO_UNK84               0x0084UL       /* notifier-ctrl method    */
#define NVFB_CTXDMA2_OFFSET          0x00001020UL   /* 2nd ctxdma (NvEvoSync)  */
#define NV50_PDISP_CHAN_HASH_TAG(i)  (0x0061020CUL + (i)*0x10)
#define NV50_PDISP_USER_PUT(i)       (0x00640000UL + (i)*0x1000)
#define NV50_PDISP_USER_GET(i)       (0x00640004UL + (i)*0x1000)

#define NV50_DMA_CB_VALID            0x00000001UL
#define NV50_DMA_CB_LOCATION_VRAM    0x00000000UL
#define NV50_CHAN_CTRL_DMA_ENABLED   0x00000010UL   /* STAT bit4 */
#define NV50_CHAN_CTRL_INIT          0x01000013UL   /* 0x01000003 | DMA_ENABLED */
#define NV50_CHAN_CTRL_BUSY          0x80000000UL

/* ctxdma object (6 dwords) built in display instance memory. class 0x3d.
 * flags0 for a linear VRAM, RW surface on G86 = 0x0019003d (= 0x3d | RW 0x80000
 * | VRAM 0x10000 | 0x100000). flags5 (dword +0x14) = 0x00010000 (chipset<0xc0). */
#define NV50_CTXDMA_FLAGS0_VRAM_RW   0x0019003DUL
#define NV50_CTXDMA_FLAGS5           0x00010000UL

/* EVO pushbuffer prefetch skips: nouveau zeroes NOUVEAU_DMA_SKIPS=128/4=32
 * dwords at the ring head before the first real method. Match it. */
#define NV50_EVO_PUSH_SKIPS          32UL

/* VRAM layout for E3 (BAR1 offsets; we assume BAR1 maps 1:1 to physical VRAM,
 * which the M2/E2 "pixels on the panel" test corroborated). All well clear of
 * the live scanout near offset 0, and inside the 128 MiB of VRAM. */
#define NVFB_RAMIN_VRAM_OFFSET       0x02000000UL  /* 32 MB: display instance mem */
#define NVFB_RAMIN_SIZE              0x00008000UL  /* 32 KiB                       */
#define NVFB_RAMHT_OFFSET            0x00000000UL  /* RAMHT at ramin+0 (4 KiB)     */
#define NVFB_RAMHT_BITS              9UL           /* 4096/8 = 512 slots           */
#define NVFB_CTXDMA_OFFSET           0x00001000UL  /* ctxdma at ramin+0x1000       */

/* ---- E2 staging (VRAM-only, no hardware activation) ---- */
/* Scratch VRAM byte offsets, well clear of the live scanout at offset 0.
 * Proven writable by the build-2005 VRAM tests. */
#define NVFB_FB_VRAM_OFFSET          0x01000000UL  /* 16 MB: our framebuffer   */
#define NVFB_PUSH_VRAM_OFFSET        0x01800000UL  /* 24 MB: EVO pushbuffer     */
#define NVFB_NOTIFY_VRAM_OFFSET      0x01810000UL  /* notifier                  */
#define NVFB_LUT_VRAM_OFFSET         0x01820000UL  /* 257x8B gamma LUT (identity)*/
#define NVFB_CURSOR_VRAM_OFFSET      0x01830000UL  /* M4.4: 64x64 A8R8G8B8 cursor (16 KiB) */

/* ---- M4.3 VRAM layout (BAR1 byte offsets) ----
 * [0, window)                  scanout framebuffer + flip buffers (the blitter's
 *                              destination; hda->vram_size)
 * [NVFB_RESERVED_BASE, _END)   our EVO structures: pushbuffer 24 MB, notifier,
 *                              LUT, VRAM boot log 25 MB, RAMIN/RAMHT/ctxdma 32 MB
 * The window may never reach NVFB_RESERVED_BASE, and the card must hold at
 * least NVFB_MIN_VRAM bytes; both are verified against the PFB size register
 * (NV_vram_check) in live VM context before the first native mode set. */
#define NVFB_RESERVED_BASE           NVFB_PUSH_VRAM_OFFSET
#define NVFB_RESERVED_END            (NVFB_RAMIN_VRAM_OFFSET + NVFB_RAMIN_SIZE)
#define NVFB_WINDOW_MAX              NVFB_RESERVED_BASE   /* 24 MB */
#define NVFB_MIN_VRAM                NVFB_RESERVED_END    /* 32 MB + 32 KiB */

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

/* ---- E3: the actual scanout flip (RISKY, opt-in, self-diagnosing) ---- *
 * Builds the display instance memory (RAMHT + one linear-VRAM ctxdma), brings
 * up EVO core channel 0 as a DMA channel, points the engine at our objects
 * (OBJECTS), pushes the head0 framebuffer methods and UPDATE. Snapshots the
 * relevant MMIO before and after so a failure is diagnosable over the IOCTL
 * path (no serial). It does NOT auto-revert the hardware; recovery from a bad
 * flip is a reboot into Safe Mode -> Standard VGA. Only ever called from an
 * explicit IOCTL, never Device_Init, so it cannot wedge a normal boot. */
typedef struct nv_evo_flip
{
	unsigned long ok;            /* reached the end of the sequence         */
	unsigned long stage;         /* NV_FLIP_STAGE_* reached (diagnosis)     */

	/* what we built / wrote */
	unsigned long ramin;         /* VRAM offset of display instance memory  */
	unsigned long ctxdma;        /* VRAM offset of the ctxdma object        */
	unsigned long ramht_slot;    /* RAMHT byte offset we wrote the entry at */
	unsigned long ramht_ctx;     /* the ctx dword we stored                 */
	unsigned long ctxdma_flags0; /* ctxdma dword[0] we wrote (sanity)       */
	unsigned long pushbuf;       /* VRAM offset of the pushbuffer           */
	unsigned long fb_offset;     /* VRAM offset of the framebuffer          */
	unsigned long iso_handle;    /* handle referenced by FB_DMA             */
	unsigned long put_bytes;     /* USER_PUT value we set (byte offset)     */
	unsigned long objects_val;   /* value written to OBJECTS                */
	unsigned long dmacb_val;     /* value written to DMA_CB                 */

	/* pre-flip snapshot */
	unsigned long pre_objects;   /* 0x610010 */
	unsigned long pre_ctrl;      /* 0x610200 (ch0 STAT)                     */
	unsigned long pre_dmacb;     /* 0x610204 */
	unsigned long pre_put;       /* 0x640000 */
	unsigned long pre_get;       /* 0x640004 */

	/* post-flip snapshot */
	unsigned long post_ctrl;     /* 0x610200 */
	unsigned long post_put;      /* 0x640000 */
	unsigned long post_get;      /* 0x640004 */
	unsigned long trapped_addr;  /* 0x610080 - nonzero => bad method/handle */
	unsigned long trapped_data;  /* 0x610084 */
	unsigned long intr0;         /* 0x610020 */
	unsigned long intr1;         /* 0x610024 */
	unsigned long post_objects;  /* 0x610010 */

	unsigned long accept_loops;  /* polls until channel accepted DMA        */
	unsigned long idle_loops;    /* polls until GET reached PUT (or timeout)*/

	/* r3 instrumentation: isolate WHY the channel does/doesn't fetch. */
	unsigned long push_hdr0;     /* VRAM readback: 1st real method header   */
	unsigned long push_hdr1;     /* VRAM readback: 2nd real method header   */
	unsigned long ctrl_after_init;/* CTRL read immediately after the 0x..13 write */
	unsigned long dmacb_rb;      /* DMA_CB read back after we wrote it       */
	unsigned long get_after;     /* GET sampled right after PUT write        */
	unsigned long get_mid;       /* GET sampled after a short spin           */
	unsigned long unk30;         /* 0x610030 (UNK30_CTRL; clock-change ack)  */
	unsigned long fini_ctrl;     /* CTRL after quiescing the VBIOS channel   */
	unsigned long fini_loops;    /* polls until active bits (0x1e0000) clear */

	/* r7: full engine re-init (nouveau nv50_display_init). */
	unsigned long pre_cursor0;   /* CURSOR_CTRL2(0) before                   */
	unsigned long cur0_state;    /* CURSOR_CTRL2(0) after ON (expect ACTIVE) */
	unsigned long cur1_state;    /* CURSOR_CTRL2(1) after ON                 */
	unsigned long cur_loops;     /* total cursor-activation polls            */
	unsigned long text_61e8;     /* 0x6194e8 after the text-mode step        */
	unsigned long ntfy_pre;      /* notifier dword before the sync FIRE      */
	unsigned long ntfy_post;     /* notifier dword after  (nonzero=sync done)*/
	unsigned long sync_loops;    /* polls waiting on the notifier            */

	/* r10: supervisor handshake stages serviced during the commit. */
	unsigned long sv10;          /* CLK_UNK10 stages acked                   */
	unsigned long sv20;          /* CLK_UNK20 stages acked                   */
	unsigned long sv40;          /* CLK_UNK40 stages acked                   */

	unsigned long build;
} nv_evo_flip_t;

/* How far NV_evo_flip got, for post-mortem when ok==0. */
#define NV_FLIP_STAGE_ENTER     0
#define NV_FLIP_STAGE_MAPPED    1   /* BAR0+BAR1 mapped                      */
#define NV_FLIP_STAGE_OBJECTS   2   /* RAMHT + ctxdma built in VRAM          */
#define NV_FLIP_STAGE_PUSH      3   /* pushbuffer method stream built        */
#define NV_FLIP_STAGE_ACCEPTED  4   /* channel accepted DMA (CTRL bit31 clr) */
#define NV_FLIP_STAGE_SUBMITTED 5   /* USER_PUT advanced                     */
#define NV_FLIP_STAGE_DONE      6   /* polled to idle / timeout, snapshot    */

int NV_evo_flip(nv_evo_flip_t *out);

/* ---- E4: native backend for the display driver ---- *
 * NV_native_valid: can the native path drive w x h x bpp (G86 + the fixed
 * 1024x768 LVDS raster, 16/32 bpp)? NV_native_modeset: perform the takeover
 * scanning out VRAM offset 0 (the shadow blitter's destination); returns 1
 * only when the engine confirmed the commit. Both are callable from the
 * VESA backend's mode-set path (vxd_vesa.c, NV_DETECT builds). */
int NV_native_valid(unsigned long w, unsigned long h, unsigned long bpp);
/* M4.1: mode table (640x480/800x600/1024x768 x 8/16/32) and the scanout depth
 * used for a desktop depth (8 bpp desktops scan out as 32 bpp). */
int NV_mode_ok(unsigned long w, unsigned long h, unsigned long bpp);
unsigned long NV_hw_bpp(unsigned long bpp);
unsigned long NV_hw_pitch(unsigned long w, unsigned long bpp);   /* 256-byte aligned scanout pitch */
int NV_native_modeset(unsigned long w, unsigned long h, unsigned long bpp);
/* Copy the last native mode-set diagnostic + counters (valid/set/ok hits and
 * the last requested w/h/bpp; cnts must hold 6 dwords). For OP_NV_LASTSET. */
void NV_native_lastdiag(nv_evo_flip_t *out, unsigned long *cnts);
/* ---- M4.4: hardware cursor on head0 ----
 * Only active once the native path owns the engine (NV_cursor_hw). The image
 * is built from the already-expanded 32-bit AND/XOR masks of vmdisp9x's
 * software cursor (wram->cursor): AND=0 -> opaque XOR colour, AND=1/XOR=0 ->
 * transparent, AND=1/XOR!=0 (screen invert) -> opaque black. Positions are
 * the image's top-left corner (hotspot already subtracted). Registry (HKLM\
 * Software\vmdisp9x\vesa): HwCursor=0 falls back to the software cursor;
 * HwCursorScale=1 scales positions to the panel in the scaled (800x600 /
 * 640x480) modes, for the case where the hardware composites the cursor in
 * output space. */
extern unsigned long nv_hw_cursor;        /* default 1 */
extern unsigned long nv_hw_cursor_scale;  /* default 0 */
int  NV_cursor_hw(void);
int  NV_cursor_load(const unsigned long *andmask, const unsigned long *xormask,
                    unsigned long w, unsigned long h, unsigned long stride,
                    long x, long y);
void NV_cursor_move(long x, long y);
int  NV_cursor_show(void);
void NV_cursor_hide(int invalidate);
/* Boot-progress log in VRAM (0x1900000): reset clears it + writes the magic. */
void NV_blog_reset(void);
/* Append one progress code to the VRAM boot log (no-op while skip_mmio). */
void NV_blog(unsigned long code);

#endif /* __NV50_EVO_H__ */
