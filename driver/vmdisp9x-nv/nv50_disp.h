/*****************************************************************************
 * nv50_disp.h - NV50/G8x (Tesla) display-engine register map
 *
 * Offsets are BAR0 MMIO byte offsets, taken from the envytools register
 * database (rnndb/display/g80_pdisplay.xml, rnndb/display/nv_evo.xml) and
 * confirmed against the live dump from the T61's G86 (see docs/M3_PLAN.md).
 *
 * The G86 display engine ("PDISPLAY") is driven through EVO DMA channels, not
 * by poking these registers directly: you build a pushbuffer of method/data
 * pairs in VRAM, point a channel at it, and commit with the UPDATE method.
 * The MMIO registers below are for channel setup, clocks, and output (SOR/DAC)
 * configuration; the per-head raster/scanout state is programmed via EVO
 * methods (NV50_EVO_* below).
 *****************************************************************************/
#ifndef __NV50_DISP_H__
#define __NV50_DISP_H__

/* ------- PDISPLAY block (BAR0 + 0x610000), stride 0x20000 ---------------- */
#define NV50_PDISPLAY              0x610000UL

#define NV50_PDISP_CAPS            0x610000UL  /* [3] engine caps            */
#define NV50_PDISP_OBJECTS         0x610010UL
#define NV50_PDISP_INTR_0          0x610020UL
#define NV50_PDISP_INTR_1          0x610024UL
#define NV50_PDISP_INTR_EN_0       0x610028UL
#define NV50_PDISP_INTR_EN_1       0x61002cUL
#define NV50_PDISP_TRAPPED_ADDR    0x610080UL
#define NV50_PDISP_TRAPPED_DATA    0x610084UL
#define NV50_PDISP_CAP             0x610184UL
#define NV50_PDISP_RAM_AMOUNT      0x610384UL  /* display instance-mem size  */

/* EVO channels: array at 0x610200, stride 0x10, 5 channels.
 * ch0 = core (modeset), ch1 = base/scanout, others overlay/cursor/curs. */
#define NV50_PDISP_CHAN(i)         (0x610200UL + (i)*0x10UL)
#define NV50_PDISP_CHAN_STAT(i)    (0x610200UL + (i)*0x10UL + 0x0)
#define NV50_PDISP_CHAN_DMA_CB(i)  (0x610200UL + (i)*0x10UL + 0x4) /* pushbuf ctx */
#define NV50_PDISP_CURSOR(i)       (0x610250UL + (i)*0x10UL)
#define NV50_PDISP_CTRL_STATE(i)   (0x610300UL + (i)*0x8UL)
#define NV50_PDISP_CTRL_VAL(i)     (0x610304UL + (i)*0x8UL)

/* ------- Display clock / VPLL (PDISPLAY + 0x4000) ------------------------ */
/* One CLOCK block; the two heads' VPLLs are 0x800 apart.
 * pixclock = refclk * N / (M << P) with the stage coefficients below. */
#define NV50_PDISP_CLOCK           0x614000UL
#define NV50_VPLL_CTRL1(h)         (0x614100UL + (h)*0x800UL)
#define NV50_VPLL_S1(h)            (0x614104UL + (h)*0x800UL) /* [7:0]=N1 [23:16]=M1 */
#define NV50_VPLL_S2(h)            (0x614108UL + (h)*0x800UL) /* N2 / M2 / [30:28]=P */
#define NV50_VPLL_CTRL2(h)         (0x614200UL + (h)*0x800UL)
#define  NV50_VPLL_CTRL1_STAGE1_DISABLE  (1UL<<30)
#define  NV50_VPLL_CTRL1_STAGE2_DISABLE  (1UL<<31)

/* ------- Outputs: DAC[3] @0x61a000, SOR[2..4] @0x61c000, stride 0x800 ---- */
#define NV50_DAC(i)                (0x61a000UL + (i)*0x800UL)
#define NV50_SOR(i)                (0x61c000UL + (i)*0x800UL)
#define NV50_SOR_PWR(i)            (0x61c004UL + (i)*0x800UL) /* bit0 NORMAL_STATE PU */
/* The internal LVDS panel hangs off one SOR; the dump identifies which. */

/* ------- EVO method numbers (pushed to the core/base channel) ------------
 * These are channel method offsets, not MMIO. Used when we build the EVO
 * pushbuffer. Values from nv_evo.xml (G80 class 0x5070 core, 0x507c base). */
#define NV50_EVO_UPDATE            0x0080  /* commit pending method state     */
#define NV50_EVO_CORE_HEAD(h)      (0x0400 + (h)*0x300) /* core per-head base  */
/* Core per-head raster (RASTER stripe, relative to the head method base):
 *   +0x14 TOTAL (xy15: x=htotal, y=vtotal)
 *   +0x18 SYNC_END
 *   +0x1c BLANK_END
 *   +0x20 BLANK_START        */
#define NV50_EVO_HEAD_TOTAL        0x14
#define NV50_EVO_HEAD_SYNC_END     0x18
#define NV50_EVO_HEAD_BLANK_END    0x1c
#define NV50_EVO_HEAD_BLANK_START  0x20
/* Base channel scanout (SET_OFFSET / SET_FORMAT+PITCH). FORMAT at [15:8];
 * PITCH on G80:GT215 at bits [17:8] shr 4. */
#define NV50_EVO_BASE_OFFSET       0x0040
#define NV50_EVO_BASE_FORMAT_PITCH 0x0808

#endif /* __NV50_DISP_H__ */
