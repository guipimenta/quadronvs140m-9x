/**************************************************************************

Copyright (c) 2025 Jaroslav Hensl <emulator@emulace.cz>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*****************************************************************************/
#define VESA

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"

#include "wram.h"
#include "async.h"

#include "vxd_lib.h"
#include "3d_accel.h"

#include "boxvint.h" /* VGA regitry */

#include "pci.h" /* re-use PCI functions from SVGA */

#include "vesa.h"
#include "mtrr.h"

#define IO_IN8
#define IO_OUT8
#include "io32.h"

#include "vxd_terror.h"

#include "code32.h"

//#define CHOOSE_SCREEN_FREQ

#define ISA_LFB 0xE0000000UL

extern FBHDA_t *hda;
extern LONG fb_lock_cnt;
extern DWORD ThisVM;
extern ULONG hda_sem;

#ifdef NV_DETECT
static char vesa_vxd_name[] = "nvmini.vxd";
/* E4: native G86 mode-set path (nv50_evo.c). When the hardware is present and
 * the requested mode matches the panel, the VBE INT10 call is replaced by our
 * own display-engine takeover; everything else (shadow rendering, blitting,
 * cursor) stays on the shared vmdisp9x machinery. */
int NV_native_valid(unsigned long w, unsigned long h, unsigned long bpp);
int NV_native_modeset(unsigned long w, unsigned long h, unsigned long bpp);
/* mode-path instrumentation (defined in nv50_evo.c, read via OP_NV_LASTSET) */
extern unsigned long nv_dbg_valid_calls, nv_dbg_set_calls;
extern unsigned long nv_dbg_set_w, nv_dbg_set_h, nv_dbg_set_bpp;
extern unsigned long nv_dbg_branch, nv_dbg_phylin;
extern unsigned long nv_dbg_init;   /* 0=none, 1=VBE init, 2=native init */
void NV_blog(unsigned long code);   /* boot progress log (VRAM 25 MB)     */
void NV_blog_reset(void);
#include "vxd_nv.h"                 /* nv_hwinfo: BAR1 from the PCI probe */
#include "nv50_evo.h"               /* M4.3: NVFB_WINDOW_MAX and the EVO layout */
#else
static char vesa_vxd_name[] = "vesamini.vxd";
#endif

#define VESA_MODE_MAX_FREQS 16

typedef struct vesa_mode
{
	DWORD width;
	DWORD height;
	DWORD bpp;
	DWORD pitch;
	DWORD phy;
	WORD  mode_id;
	WORD  flags;
	WORD  freqs[VESA_MODE_MAX_FREQS];
} vesa_mode_t;

static vesa_mode_t *vesa_modes = NULL;
static DWORD vesa_modes_cnt = 0;
WORD vesa_version = 0;
static DWORD vesa_caps = 0;
static int act_mode = -1;
static DWORD video_bpp;
static DWORD video_pitch;
static BOOL vga_mode = TRUE;
static BOOL mttr_was_set = FALSE;
static BOOL timer_set = FALSE;

static blit_t wblit;

static void VESA_draw(blit_t *blit);

/* from gtf.c */
void gtf_calc(DWORD w, DWORD h, DWORD freq, vesa_crtc_info_t *crtc_info);

/* vxd_mouse.vxd */
BOOL mouse_get_rect(DWORD *ptr_left, DWORD *ptr_top,
	DWORD *ptr_right, DWORD *ptr_bottom);

static CRS_32 reg_state;

void vesa_bios_V86(CRS_32 *V86regs)
{
	CRS_32 *pstate = &reg_state;
	
	_asm    push      ebx
	// sizeof(CRS_32) = 108
	_asm    mov       ebx, [ThisVM]
	_asm    mov       ecx, [V86regs]
	_asm    mov       edi, [pstate]
  /* save state */
	//_asm    sub       esp, 128
	//_asm    mov       edi, esp
	VMMCall(Save_Client_State);
	/* switch to V86regs */
	_asm   mov        esi, ecx
	VMMCall(Restore_Client_State)
	/* V86 INT */
	VMMCall(Begin_Nest_V86_Exec);
	_asm    mov       eax, 10h
	VMMCall(Exec_Int);
	VMMCall(End_Nest_Exec);
	/* copy result state to V86regs */
	_asm   mov       edi, [V86regs]
	VMMCall(Save_Client_State);
	/* restore state regs */
	//_asm    mov       esi, esp
	_asm   mov       esi, [pstate]
	VMMCall(Restore_Client_State)
	_asm   pop       ebx
	//_asm    add       esp, 128
}

void vesa_bios(CRS_32 *V86regs)
{
	_asm
	{
		pushad
		mov esi, [V86regs]
		mov eax, [esi+0x1C]
		mov ebx, [esi+0x10]
		mov ecx, [esi+0x18]
		mov edx, [esi+0x14]
		mov edi, [esi+0x00]
		mov esi, [esi+0x04]
		push dword ptr 10h
	};
	VMMCall(Exec_VxD_Int);
	//VMMCall(_ExecVxDIntMustComplete);
	_asm
	{
		mov esi, [V86regs]
		mov [esi+0x1C], eax
		mov [esi+0x10], ebx
		mov [esi+0x18], ecx
		mov [esi+0x14], edx
		mov [esi+0x00], edi
		mov [esi+0x04], esi
		popad
	};
}

void load_client_state(CRS_32 *V86regs)
{
//	_asm   mov       edi, [V86regs]
//	VMMCall(Save_Client_State);
	VMCB_t *vm = (VMCB_t*)ThisVM;
	memcpy(V86regs, vm->CB_Client_Pointer, sizeof(CRS_32));
	V86regs->Client_EBP = ThisVM;
}

static DWORD vesa_buf_v86;
static void *vesa_buf;
static DWORD vesa_buf_phy;
static vesa_palette_entry_t *vesa_pal;
static int vesa_pal_bits = 6;

static BOOL vesa_valid = FALSE;

static DWORD conf_dos_window = 0;
static DWORD conf_hw_double_buf = 1; /* much better performance on real HW */
static DWORD conf_mtrr = 1;
#ifdef NV_DETECT
/* E4 kill switch: 0 = boot on the plain VBE path (safe bring-up), 1 = use the
 * native G86 modeset. Read from HKLM\Software\vmdisp9x\vesa "Native". Default 0
 * so a freshly-bound driver comes up on VBE first and cannot black-screen on
 * the native takeover before we have confirmed the driver even loads. */
static DWORD conf_native = 1;   /* 3033: native G86 modeset is the DEFAULT; registry Native=0 is the opt-out (vga.drv fallback) */
static DWORD nv_backend_native = 0;  /* 1 once NV_init_hw_native() succeeds */
/* E4 boot-hang bisection: HKLM\...\vesa "InitStop" = N makes NV_init_hw_native
 * run only stages 1..N then return FALSE. If the logged boot then reaches
 * DEVICEINITSUCCESS=VDD (black screen, not a hang), stages 1..N are innocent.
 * Stages: 1=probe, 2=BAR1 map, 3=wram, 4=FBHDA, 5=hda fields, 6=full(timer). */
static DWORD conf_init_stop = 0;
/* E4 panel diagnostic: HKLM\...\vesa "BootPattern" = 1 makes the native modeset
 * paint the 8-bar test image into the scanout buffer. With no console/serial on
 * the T61 the panel is the only output: bars => the .drv reached our setmode and
 * EVO works at boot; blinking underscore => setmode never ran (.drv load/Enable
 * failed before us). Read in nv50_evo.c NV_native_modeset. Non-static on purpose. */
DWORD nv_boot_pattern = 0;
#endif

#define MODE_OFFSET 1024
#define CRTC_OFFSET 2048
#define PAL_OFFSET  3072 // pal size = 4*256

#ifdef CHOOSE_SCREEN_FREQ
static DWORD clock_to_test[] = {120, 110, 100, 85, 75, 70, 60, 50, 30, 25, 0};
#endif

#define V86_SEG(_lin) ((_lin) >> 4)
#define V86_OFF(_lin) ((_lin) & 0xF)

#define LIN_FROM_V86(_flatptr) ((((_flatptr) >> 12) & 0xFFFF0UL) + ((_flatptr) & 0xFFFFUL))

#define VESA_SUCC(_r) (((_r).Client_EAX & 0xFFFF)==0x004F)

static void offset_calc(DWORD offset, DWORD *ox, DWORD *oy)
{
	DWORD ps = ((video_bpp+7)/8);
	*oy = offset/video_pitch;
	*ox = (offset % video_pitch)/ps;
}

static void alloc_modes_info(DWORD cnt)
{
	DWORD vesa_modes_pages = ((sizeof(vesa_mode_t) * cnt) + P_SIZE - 1) / P_SIZE;
	vesa_modes = (vesa_mode_t*)_PageAllocate(vesa_modes_pages, PG_SYS, 0, 0x0, PAGE_ALLOC_MIN, PAGE_ALLOC_MAX, NULL, PAGEZEROINIT);
	dbg_printf("vesa_modes = %lX\n", vesa_modes);
	vesa_modes_cnt = 0;
}

static DWORD VESA_pitch(DWORD width, DWORD bpp)
{
	DWORD bp = (bpp + 7) / 8;
	return (bp * width + (FBHDA_ROW_ALIGN-1)) & (~((DWORD)FBHDA_ROW_ALIGN-1));
}

#define DSWAP(_a, _i, _j) {WORD tmp = _a[_i]; _a[_i] = _a[_j]; _a[_j] = tmp;}

#ifdef CHOOSE_SCREEN_FREQ
/* begin mode list with 60 Hz, 50 Hz and the higher freqs */
static void mode_sort_freqs(int m)
{
	int k = 0;
	int f60 = -1;
	int f50 = -1;

	while(vesa_modes[m].freqs[k] != 0)
	{
		if(vesa_modes[m].freqs[k] == 50)	f50 = k;
		if(vesa_modes[m].freqs[k] == 60)	f60 = k;
		k++;
	}
	
	k = 0;
	if(f60 >= 0)
	{
		DSWAP(vesa_modes[m].freqs, k, f60);
		k++;
	}
	
	if(f50 >= 0)
	{
		DSWAP(vesa_modes[m].freqs, k, f50);
		k++;
	}
}
#endif

DWORD vram_phy = 0;

extern const char reg_path[];
static const char VESA_conf_path[] = "Software\\vmdisp9x\\vesa";

void VESA_load_vbios_pm();

#ifdef NV_DETECT
/*
 * NV_init_hw_native --
 *
 *   Backend init with NO VBIOS/VBE dependence: on this hardware (G86) the
 *   VBE 2.0 interaction fails, which made VESA_init_hw return FALSE and the
 *   display driver fall back to vga.drv forever. Everything VBE provided is
 *   available elsewhere: VRAM physical base/size come from the PCI BARs (M2
 *   probe), scanout comes from the native mode-set (E3). The shared shadow
 *   (wram) + blitter machinery is initialised exactly as the VBE path does.
 */
static BOOL NV_init_hw_native(DWORD conf_vram_limit, DWORD wram_size)
{
	void *vram_lin;
	DWORD vram_size;

	/* VESA_init_hw runs during Device_Init, BEFORE NV_probe populates
	 * nv_hwinfo in the later Device_Init stage - so probe here on demand,
	 * otherwise native init always fell through to the (hanging) VBE path. */
	if(!nv_hwinfo.found)
		NV_probe(&nv_hwinfo);

	if(!nv_hwinfo.found || !nv_hwinfo.mmio_ok || nv_hwinfo.chipset != 0x86)
		return FALSE;
	if(nv_hwinfo.bar1_phys == 0)
		return FALSE;

	NV_blog_reset();     /* BAR1 reachable now (probe done) -> start the log */
	NV_blog(0x100);
	/* stage 1 done = NV_probe survived at Device_Init */
	if(conf_init_stop == 1) return FALSE;

	/* M4.3: the scanout window = visible fb + flip buffers. It is bounded by
	 * the registry VRAMLimit (MB), by the PCI BAR1 aperture, and by the start
	 * of our EVO reservation (NVFB_WINDOW_MAX, 24 MB). No MMIO is allowed in
	 * this context, so the real VRAM size is checked later by NV_vram_check. */
	vram_size = conf_vram_limit * 1024 * 1024;
	if(vram_size > NVFB_WINDOW_MAX)
		vram_size = NVFB_WINDOW_MAX;
	if(nv_hwinfo.bar1_size != 0 && vram_size > nv_hwinfo.bar1_size)
		vram_size = nv_hwinfo.bar1_size;
	nv_fb_window = vram_size;

	vram_lin = (void*)_MapPhysToLinear(nv_hwinfo.bar1_phys, vram_size, 0);
	if(vram_lin == NULL || vram_lin == (void*)0xFFFFFFFFUL)
		return FALSE;
	NV_blog(0x103);
	if(conf_init_stop == 2) return FALSE;   /* stage 2 = BAR1 12MB map */

	if(!wram_init(wram_size*1024*1024))
	{
		dbg_printf("native init: wram_init(%lu MB) failed\n", wram_size);
		return FALSE;
	}
	NV_blog(0x104);
	if(conf_init_stop == 3) return FALSE;   /* stage 3 = wram_init(64MB) */
	if(!FBHDA_init_hw())
	{
		dbg_printf("native init: FBHDA_init_hw failed\n");
		return FALSE;
	}
	NV_blog(0x105);
	if(conf_init_stop == 4) return FALSE;   /* stage 4 = FBHDA_init_hw */

	memcpy(hda->vxdname, vesa_vxd_name, sizeof(vesa_vxd_name));
	hda->vram_size      = vram_size;
	hda->vram_size_bar  = vram_size;
	hda->vram_phylin    = vram_lin;
	hda->vram_pm32      = (BYTE*)wram + wram->regs.s.fbmin;
	hda->vram_size_virt = wram->regs.s.fbmax - wram->regs.s.fbmin;
	hda->flags |= FB_SUPPORT_FLIPING;

	vesa_version   = 0;      /* no VBE anywhere in this path  */
	vesa_caps      = 0;
	vesa_modes_cnt = 0;      /* mode list = the native modes  */
	vram_phy       = nv_hwinfo.bar1_phys;
	vesa_valid     = TRUE;
	nv_dbg_init    = 2;
	nv_backend_native = 1;   /* -> mode path is native-only, never VBE/V86 */

	NV_blog(0x106);
	if(conf_init_stop == 5) return FALSE;   /* stage 5 = everything but the blit timer */
	wblit.num_changes = 0;
	/* E4 FIX3: do NOT start the async blit timer here. Set_Global_Time_Out()
	 * inside the Device_Init critical section hard-freezes the G86 - this is the
	 * SOLE remaining boot hang (InitStop=5, which skips exactly this call, boots
	 * to a live black screen; InitStop unset hangs). Leave timer_set=FALSE so
	 * FBHDA_access_end()/FBHDA_swap() blit the shadow synchronously on each GDI
	 * access - the desktop still renders, just without a periodic timer. The
	 * timer is started lazily at the first native setmode (live VM context,
	 * outside the critical section); see VESA_setmode_phy. */
	timer_set = FALSE;
	NV_blog(0x107);

	dbg_printf("NV_init_hw_native OK: vram_phy=%lX lin=%lX size=%lu\n",
		vram_phy, (DWORD)vram_lin, vram_size);
	return TRUE;
}

/* E4: run the EXACT backend init the boot display driver runs, on demand from
 * nvprobe (OP_NV_INITSIM). Logs the 0x100..0x107 markers; if the boot hang is
 * in the backend (12MB map / 64MB wram / FBHDA / blit timer) it reproduces it
 * here recoverably. Returns what VESA_init_hw's native branch would return. */
BOOL NV_initsim(void)
{
	return NV_init_hw_native(32, 64);
}

/* defined later in this file */
BOOL VESA_setmode_phy(DWORD w, DWORD h, DWORD bpp, DWORD rr_min, DWORD rr_max);

/* E4: run the EXACT display-driver setmode hook on demand (OP_NV_SETMODESIM):
 * the 0x210..0x212 path, the post-modeset wblit/wram reconfiguration, and the
 * full 1024x768x16 blit it kicks off - the one boot piece the sims miss. Must
 * be called AFTER NV_initsim (needs hda/wram/wblit set up). */
BOOL NV_setmodesim(void)
{
	return VESA_setmode_phy(1024, 768, 16, 0, 0);
}
#endif /* NV_DETECT */

BOOL VESA_init_hw()
{
	DWORD flat;
	DWORD conf_vram_limit = 32;
	DWORD wram_size = 64;

	dbg_printf("VESA init begin...\n");

	RegReadConf(HKEY_LOCAL_MACHINE, reg_path,       "WRAMSize",         &wram_size);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "VRAMLimit",        &conf_vram_limit);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "MTRR",             &conf_mtrr);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "DosWindowSetMode", &conf_dos_window);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "HWDoubleBuffer",   &conf_hw_double_buf);
#ifdef NV_DETECT
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "Native",           &conf_native);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "InitStop",         &conf_init_stop);
	RegReadConf(HKEY_LOCAL_MACHINE, VESA_conf_path, "BootPattern",      &nv_boot_pattern);
	dbg_printf("NV native modeset = %lu, InitStop = %lu, BootPattern = %lu\n",
		conf_native, conf_init_stop, nv_boot_pattern);
#endif

	if(wram_size < WRAM_MIN_MB)
	{
		wram_size = WRAM_MIN_MB;
	}

#ifdef NV_DETECT
	/* Native-ONLY on the G86: the VBE 2.0 / V86 INT10h interaction DEADLOCKS on
	 * this hardware (vesa_bios_V86 below never returns), so it is not a safe
	 * fallback here - it only hangs the boot. Drive the backend from the PCI
	 * probe instead of the video BIOS; when native is off or fails, fail CLEANLY
	 * to Windows' own vga.drv rather than touching V86 at all.
	 *
	 * Consequence: Native=0 is a reliable non-hang boot (desktop on vga.drv)
	 * that STILL loads this VxD, so the boot log can be read back with nvprobe
	 * after a Native=1 boot has hung. */
	if(conf_native)
	{
		return NV_init_hw_native(conf_vram_limit, wram_size);
	}
	dbg_printf("Native=0: VBE path skipped (deadlocks on G86), falling to vga.drv\n");
	return FALSE;
#endif

	flat = _PageAllocate(1, PG_SYS, 0, 0x0, PAGE_ALLOC_MIN, PAGE_ALLOC_MAX, &vesa_buf_phy, PAGEUSEALIGN | PAGECONTIG | PAGEFIXED);
	vesa_buf = (void*)flat;

	if(vesa_buf)
	{
		DWORD modes_count = 0;
		DWORD v86_page = 0xB0; /* Is this OK? Any collisions? */

		if(_PhysIntoV86(vesa_buf_phy >> 12, ThisVM, v86_page, 1, 0))
		{
			vesa_buf_v86 = v86_page*4096;
		}
		dbg_printf("VESA V86 addr=0x%lX, phy=0x%lX\n", vesa_buf_v86, vesa_buf_phy);

		if(vesa_buf_v86)
		{
			DWORD i;
			DWORD fb_phy = 0xFFFFFFFFF;
			CRS_32 regs;
			vesa_info_block_t *info = vesa_buf;
			vesa_mode_info_t *modeinfo = (vesa_mode_info_t*)((char*)vesa_buf + MODE_OFFSET);

			memset(info,     0, sizeof(vesa_info_block_t));
			memset(modeinfo, 0, sizeof(vesa_mode_info_t));
			memcpy(&info->VESASignature[0], "VBE2", 4);

			load_client_state(&regs);
			regs.Client_EAX = VESA_CMD_ADAPTER_INFO;
			regs.Client_ES  = V86_SEG(vesa_buf_v86);
			regs.Client_EDI = V86_OFF(vesa_buf_v86);

			vesa_bios_V86(&regs);

			if((regs.Client_EAX & 0xFFFF) == 0x004F &&
				memcmp(info->VESASignature, "VESA", 4) == 0)
			{
				WORD *modes = NULL;
				DWORD vram_size = 0;
				void  *vram_lin = NULL;

				dbg_printf("VESA bios result=%X, vesa_version=%X, modes_ptr=%lX vesa_caps=%lX\n",
					regs.Client_EAX & 0xFFFF, info->VESAVersion, info->VideoModePtr, info->Capabilities);

				if(info->VESAVersion < VESA_VBE_2_0)
				{
					terror("We need VBE 2.0 as minimum, abort\n");
					tpause();
					return FALSE;
				}

				modes = (WORD*)LIN_FROM_V86(info->VideoModePtr);
				if(modes != NULL)
				{
					while(*modes != 0xFFFF)
					{
						//dbg_printf("modes: 0x%04X\n", *modes);
						modes++;
						modes_count++;
					}
				}

				alloc_modes_info(modes_count);
				modes = (WORD*)LIN_FROM_V86(info->VideoModePtr);

				for(i = 0; i < modes_count; i++)
				{
					regs.Client_EAX = VESA_CMD_MODE_INFO;
					regs.Client_ECX = modes[i];
					regs.Client_ES  = V86_SEG(vesa_buf_v86+MODE_OFFSET);
					regs.Client_EDI = V86_OFF(vesa_buf_v86+MODE_OFFSET);
					vesa_bios_V86(&regs);

					//dbg_printf("mode=%X atrs=0x%lX eax=0x%lX\n", modes[i], modeinfo->ModeAttributes, regs.Client_EAX);
					if(VESA_SUCC(regs))
					{
						if((modeinfo->ModeAttributes &
							(VESA_MODE_HW_SUPPORTED | VESA_MODE_COLOR | VESA_MODE_GRAPHICS | VESA_MODE_LFB)) ==
							(VESA_MODE_HW_SUPPORTED | VESA_MODE_COLOR | VESA_MODE_GRAPHICS | VESA_MODE_LFB))
						{
							vesa_mode_t *m = &vesa_modes[vesa_modes_cnt];
							m->width   = modeinfo->XResolution;
							m->height  = modeinfo->YResolution;
							m->bpp     = modeinfo->BitsPerPixel;
							m->pitch   = modeinfo->BytesPerScanLine;
							m->phy     = modeinfo->PhysBasePtr;
							m->mode_id = modes[i];
							m->flags   = modeinfo->ModeAttributes;
							if(m->bpp == 16)
							{
								if(modeinfo->GreenMaskSize == 5)
								{
									m->bpp = 15;
								}
							}

							vesa_modes_cnt++;

							if(m->phy)
							{
								if(m->phy < fb_phy)
								{
									fb_phy = m->phy;
								}
							}

							dbg_printf("Mode 0x%X = (%ld x %ld x %ld) = phy:%lX\n",
								m->mode_id, m->width, m->height, m->bpp, m->phy);

#ifdef CHOOSE_SCREEN_FREQ
							if(info->VESAVersion >= VESA_VBE_3_0)
							{
								vesa_crtc_info_t crtc_test;
								int k = 0;
								DWORD *pfreq = clock_to_test;
								while(*pfreq != 0)
								{
									DWORD last_freq = 0;
									gtf_calc(m->width, m->height, *pfreq, &crtc_test);
									if(crtc_test.PixelClock <= modeinfo->MaxPixelClock)
									{
										regs.Client_EAX = VESA_CMD_PIXEL_CLOCK;
										regs.Client_EBX = 0;
										regs.Client_EDX = m->mode_id;
										regs.Client_ECX = crtc_test.PixelClock;
										vesa_bios(&regs);
										if(VESA_SUCC(regs))
										{
											if(k > 0)
											{
												if(last_freq > regs.Client_ECX)
												{
													m->freqs[k] = *pfreq;
													k++;
												}
											}
											else
											{
												m->freqs[k] = *pfreq;
												k++;
											}
											last_freq = regs.Client_ECX;
											dbg_printf("  freq=%d pixclk=%ld\n", *pfreq, last_freq);
										}
									}
								}
								m->freqs[k] = 0;
								mode_sort_freqs(i);
							}
							else
#endif
							{
								m->freqs[0] = 0;
							}
						}						
					}
				} // for

				if(vesa_modes_cnt == 0)
				{
					terrorf("No VESA modes found, bad VBIOS?\n");
					tpause();
				}

				vesa_version = info->VESAVersion;
				vesa_caps = info->Capabilities;
				vram_size = info->TotalMemory * 0x10000UL;

				if(vram_size > (conf_vram_limit*1024*1024))
				{
					vram_size = conf_vram_limit*1024*1024;
				}
				
				if(fb_phy == 0xFFFFFFFF)
				{
					fb_phy = ISA_LFB;
				}
				
				vram_phy = fb_phy;
				
				/* 9x have trobles with alloc large continuous memory block */
				vram_lin = (void*)_MapPhysToLinear(vram_phy, vram_size, 0);

				if(!wram_init(wram_size*1024*1024))
				{
					terrorf("Cannot alocate %d MB RAM!\n", wram_size);
					terror("Please adjust WRAMSize registry key or increase physical RAM\n");
					tpause();
					return FALSE;
				}

			 	if(!FBHDA_init_hw())
			 	{
			 		dbg_printf("cannot init FBHDA\n");
			 		return FALSE;
			 	}

				memcpy(hda->vxdname, vesa_vxd_name, sizeof(vesa_vxd_name));

				hda->vram_size = vram_size;
				hda->vram_size_bar = conf_vram_limit*1024*1024;

				hda->vram_phylin = vram_lin;
				hda->vram_pm32   = (BYTE*)wram + wram->regs.s.fbmin;
				hda->vram_size_virt = wram->regs.s.fbmax - wram->regs.s.fbmin;
				dbg_printf("vram_phy=%lX vram_phylin=%lX\n", vram_phy, hda->vram_phylin);

				hda->flags |= FB_SUPPORT_FLIPING | FB_VESA_MODES;
				if(vesa_version >= VESA_VBE_3_0)
				{
					//hda->flags |= FB_SUPPORT_CLOCK;
				}

				vesa_pal = (vesa_palette_entry_t*)(((BYTE*)vesa_buf)+PAL_OFFSET);

				vesa_valid  = TRUE;
#ifdef NV_DETECT
				nv_dbg_init = 1;
#endif

				VESA_load_vbios_pm();

				wblit.num_changes = 0;
				timer_set = async_blit_init(&wblit, VESA_draw);
				dbg_printf("timer = %d\n", timer_set);

				dbg_printf("VESA_init_hw(vram_size=%ld, wram_size=%ld, real_memory=%ld) = TRUE\n",
					hda->vram_size, hda->vram_size_virt, hda->vram_size_bar);

				return TRUE;
			}
			else
			{
				terror("Can't interact with VESA BIOS, this driver can't work\n");
				tpause();
			}
		}
	}
	return FALSE;
}

static WORD *vbios32 = NULL;
static WORD vbios32_sel = 0;
static DWORD vbios32_mem = 0;

BOOL VESA_display_start_pm(BOOL vtrace, DWORD start_addr)
{
	BYTE cmd = vtrace ? 0x80 : 0;
	/* DOC: DX:CX is the 32 bit offset in display memory, aligned to a plane boundar,
	... but in 8+ bits per pixel modes this is the offset from the start of memory divided by 4 */
	WORD h1 = (start_addr >> 2) & 0xFFFF;
	WORD h2 = start_addr >> 18;

	if(vbios32 != NULL && vbios32[VESA_PMTABLE_OFF_DISPLAY_START])
	{
		//dbg_printf("VESA_display_start_pm ...");
		_asm
		{
			pushad
			push es
			mov eax, [vbios32]
			xor ebx, ebx
			mov bx,  [eax+2] /* VESA_PMTABLE_OFF_DISPLAY_START*2 */
			add eax, ebx
			mov bx, [vbios32_sel]
			or bx, bx
			jz skip_load_es
				mov es, bx
			skip_load_es:
			xor ebx, ebx
			xor ecx, ecx
			xor edx, edx
			mov bl,[cmd]
			mov cx,[h1]
			mov dx,[h2]
			call eax
			pop es
			popad
		};
		//dbg_printf("OK\n");
		
		return TRUE;
	}
	
	return FALSE;
}

void VESA_load_vbios_pm()
{
	CRS_32 regs;

	load_client_state(&regs);
	regs.Client_EAX = VESA_CMD_PM_ENTRY;
	regs.Client_EBX = 0;

	vesa_bios_V86(&regs);

	if(VESA_SUCC(regs))
	{
		WORD *bios_pm;
		WORD *ports;
		DWORD bios_pm_flat = regs.Client_ES * 0x10 + (regs.Client_EDI & 0xFFFF);
		dbg_printf("table_flat es=%lX, edi=%lX, code_size=%ld\n",
			regs.Client_ES, regs.Client_EDI, regs.Client_ECX & 0xFFFF);

		if(bios_pm_flat != 0)
		{
			DWORD code_size = regs.Client_ECX & 0xFFFF;
			DWORD code_pages = RoundToPages(code_size);

			bios_pm = (WORD*)bios_pm_flat;
			
			dbg_printf("set display off func: %X\n", bios_pm[VESA_PMTABLE_OFF_DISPLAY_START]);

			vbios32 = (WORD*)_PageAllocate(code_pages, PG_SYS, 0, 0x0, PAGE_ALLOC_MIN, PAGE_ALLOC_MAX, NULL, PAGEZEROINIT);
			if(vbios32 != NULL)
			{
				memset(vbios32, 0xFF, code_pages*P_SIZE);
				memcpy(vbios32, bios_pm, code_size);
				_PageModifyPermissions(((DWORD)vbios32) / P_SIZE, code_pages, 0, PC_USER | PC_WRITEABLE);
				dbg_printf("copy to vbios32 success\n");
			}
/*
			terror("Here is dump of pm32 table:\n");
			terrorf("vbios32[0] = %X\n", vbios32[0]);
			terrorf("vbios32[1] = %X\n", vbios32[1]);
			terrorf("vbios32[2] = %X\n", vbios32[2]);
			terrorf("vbios32[3] = %X\n", vbios32[3]);
			terrorf("code size = %d\n", code_size);
*/
			if(vbios32[VESA_PMTABLE_OFF_PORTS])
			{
				ports = vbios32 + vbios32[VESA_PMTABLE_OFF_PORTS];
				// ^ some BIOSes has this table after the code to copy
				
				while(*ports != 0xFFFF)
				{
					dbg_printf("port: %04X\n", *ports);
					//terrorf("port: = %X\n", *ports);
					ports++;
				}
				ports++;
				if(*ports != 0xFFFF)
				{
					DWORD mem_adr = *((DWORD*)ports);
					DWORD mem_size = ports[2];
					
					//terrorf("mem: = %X, size = %d\n", mem_adr, mem_size);
					
					vbios32_mem = _MapPhysToLinear(mem_adr, mem_size, 0);
					if(vbios32_mem != 0xFFFFFFFF)
					{
						DWORD hi  = 0;
						DWORD low = 0;
						DWORD selector;
						_BuildDescriptorDWORDs(mem_adr, RoundToPages(mem_adr), 0x92, 0x80, 0, &hi, &low);
	
						_Allocate_GDT_Selector(hi, low, 0, &selector, NULL);
						vbios32_sel = selector << 16;
					}
					
					dbg_printf("VESA PM need memory addr=0x%8X (size=%d)\n", mem_adr, mem_size);
				}
			}

#if 0
			/* print part of PM code (until first RET) */
			{
				BYTE *code = ((BYTE*)vbios32) + vbios32[VESA_PMTABLE_OFF_DISPLAY_START];
				dbg_printf("display start code: ");
				while(*code != 0xC3)
				{
					dbg_printf("0x%02X ", *code);
					code++;
				}
				dbg_printf("\n");
			}
#endif
		}
		else
		{
			dbg_printf("VESA_CMD_ADAPTER_INFO success but no table\n");
		}
	}
	else
	{
		dbg_printf("VESA_CMD_ADAPTER_INFO failed\n");
	}
}

BOOL VESA_valid()
{
	return vesa_valid;
}

void VESA_clear()
{
	memset((BYTE*)hda->vram_pm32+hda->system_surface, 0, hda->stride);
	dbg_printf("Clear success\n");
}

static int VESA_modechoice(DWORD w, DWORD h, DWORD bpp)
{
	DWORD i = 0;
	
	// try #1: look for exact resolution and 32bpp
	for(i = 0; i < vesa_modes_cnt; i++)
	{
		if(vesa_modes[i].width == w && vesa_modes[i].height == h && vesa_modes[i].bpp == 32)
		{
			return i;
		}
	}
	
	// try #2: look for any larger resolution in 32 bpp
	for(i = 0; i < vesa_modes_cnt; i++)
	{
		if(vesa_modes[i].width >= w && vesa_modes[i].height >= h && vesa_modes[i].bpp == 32)
		{
			return i;
		}
	}
	
	// try #3: look for exact resolution in best bpp
	for(i = 0; i < vesa_modes_cnt; i++)
	{
		if(vesa_modes[i].width == w && vesa_modes[i].height == h && vesa_modes[i].bpp >= bpp)
		{
			return i;
		}
	}
	
	// try #4: lok for any larger resolution in best bpp
	for(i = 0; i < vesa_modes_cnt; i++)
	{
		if(vesa_modes[i].width >= w && vesa_modes[i].height >= h && vesa_modes[i].bpp >= bpp)
		{
			return i;
		}
	}
	
	return vesa_modes_cnt;
}

static DWORD VESA_freqchoice(int mode, DWORD rr_min, DWORD rr_max)
{
	int k;
	for(k = 0; k < VESA_MODE_MAX_FREQS; k++)
	{
		DWORD f = vesa_modes[mode].freqs[k];
		if(f == 0)
		{
			return 0; /* default */
		}
		
		if(f >= rr_min && f <= rr_max)
		{
			return f;
		}
	}
	return 0;
}

BOOL VESA_validmode(DWORD w, DWORD h, DWORD bpp)
{
	DWORD mode;

#ifdef NV_DETECT
	nv_dbg_valid_calls++;
	if(nv_backend_native)
	{
		/* Native-only backend: the valid mode list IS the native list; there is
		 * no VBE mode table to consult. */
		return NV_native_valid(w, h, bpp) ? TRUE : FALSE;
	}
	if(conf_native && NV_native_valid(w, h, bpp))
	{
		return TRUE;
	}
#endif

	mode = VESA_modechoice(w, h, bpp);
	if(mode < vesa_modes_cnt)
	{
		return TRUE;
	}

	return FALSE;
}


BOOL VESA_setmode_phy(DWORD w, DWORD h, DWORD bpp, DWORD rr_min, DWORD rr_max)
{
	DWORD mode = 0;
	DWORD num_buffers = 0;
	DWORD video_stride;
	DWORD freq;
	CRS_32 regs;
	vesa_crtc_info_t *crtc_info = NULL;
	DWORD refresh = ASYNC_DEFAULT;

#ifdef NV_DETECT
	/* E4: native G86 mode set. The engine scans out VRAM offset 0 - exactly
	 * where the shadow blitter already writes (hda->vram_phylin = the BAR1
	 * LFB mapping) - so on success we only have to fill the same soft state
	 * the VBE path fills, and the shared machinery does the rest. */
	nv_dbg_set_calls++;
	nv_dbg_set_w = w; nv_dbg_set_h = h; nv_dbg_set_bpp = bpp;
	nv_dbg_phylin = (DWORD)hda->vram_phylin;
	nv_dbg_branch = !NV_native_valid(w, h, bpp) ? 1 :
	                (hda->vram_phylin == NULL ? 2 : 3);
	NV_blog(0x210);
	if((nv_backend_native || conf_native) && NV_native_valid(w, h, bpp) && hda->vram_phylin != NULL)
	{
		NV_blog(0x211);
		if(NV_native_modeset(w, h, bpp))
		{
			DWORD pitch = w * ((bpp + 7) / 8);
			NV_blog(0x212);

			hda->width  = w;
			hda->height = h;
			hda->bpp    = bpp;
			hda->pitch  = pitch;
			hda->stride = h * pitch;
			hda->surface        = 0;
			hda->system_surface = 0;
			hda->flags &= ~FB_SUPPORT_VSYNC;

			video_bpp   = bpp;
			video_pitch = pitch;

			wblit.base.ptr        = hda->vram_phylin;
			wblit.dst[0].flat.ptr = hda->vram_phylin;
			wblit.dst[1].flat.ptr = NULL;
			wblit.dst[2].flat.ptr = NULL;
			wblit.dst_w     = w;
			wblit.dst_h     = h;
			wblit.dst_pitch = pitch;
			wblit.dst_mode  = (bpp == 32) ? MODE_32 : MODE_16;
			wblit.dst_scans = 1;
			wblit.dst_padx  = 0;
			wblit.dst_pady  = 0;

			wram->regs.s.width  = w;
			wram->regs.s.height = h;
			wram->regs.s.mode   = (bpp == 32) ? MODE_32 : MODE_16;
			wram->regs.s.pitch  = pitch;

			async_blit_settime(ASYNC_DEFAULT);

			/* E4 FIX3 (cont.): start the async blit timer HERE, at the first
			 * native setmode, not at Device_Init. This runs in live GUI context
			 * (DISPLAY.drv load, long after DEVICEINITSUCCESS), where
			 * Set_Global_Time_Out is safe - unlike the Device_Init critical
			 * section, where it hard-freezes the G86. Until the timer is up the
			 * shadow blits synchronously on each GDI access (correct but slow);
			 * once it is, blits batch dirty rects on the timer (fast). */
			if(!timer_set)
			{
				timer_set = async_blit_init(&wblit, VESA_draw);
				dbg_printf("native: async blit timer started = %d\n", timer_set);
			}

			/* 3032: arm the blitter. VESA_draw() returns at once while vga_mode is
			 * set or act_mode < 0; the VBE path clears both further down
			 * (VESA_HIRES_enable() and "act_mode = mode"), which this early
			 * return used to skip - GDI painted the shadow, nothing reached VRAM. */
			vga_mode = FALSE;
			act_mode = 0;

			dbg_printf("native mode set: %lux%lux%lu pitch=%lu\n", w, h, bpp, pitch);
			return TRUE;
		}
		dbg_printf("native mode set FAILED\n");
	}
	/* On the native-only backend there is NO VBE to fall back to (no mode table,
	 * and the V86 BIOS call is exactly what hangs on this HW). Fail cleanly so
	 * Windows falls back to its own vga.drv rather than hanging in our V86 path. */
	if(nv_backend_native)
	{
		dbg_printf("native backend: mode %lux%lux%lu not available (no VBE fallback)\n",
			w, h, bpp);
		return FALSE;
	}
#endif

	VESA_HIRES_enable();

	mode = VESA_modechoice(w, h, bpp);
	if(mode >= vesa_modes_cnt) return FALSE;
		
	freq = VESA_freqchoice(mode, rr_min, rr_max);
	
	if(freq != 0)
	{
		crtc_info = (vesa_crtc_info_t*)((char*)vesa_buf + CRTC_OFFSET);
		gtf_calc(vesa_modes[mode].width, vesa_modes[mode].height, freq, crtc_info);
	}

	load_client_state(&regs);

	// set mode
	regs.Client_EAX = VESA_CMD_MODE_SET;
	regs.Client_EBX = vesa_modes[mode].mode_id | VESA_SETMODE_LFB | VESA_SETMODE_NOCLEAR;
	if(crtc_info)
	{
		regs.Client_ES  = V86_SEG(vesa_buf_v86+CRTC_OFFSET);
		regs.Client_EDI = V86_OFF(vesa_buf_v86+CRTC_OFFSET);
		
		vesa_bios(&regs);
		if(!VESA_SUCC(regs))
		{
			/* fail to set mode, try with default frequency */
			regs.Client_ES = 0;
			regs.Client_EDI = 0;
			dbg_printf("freq=%d failed, try default\n", freq);
			vesa_bios(&regs);
		}
		else
		{
			refresh = 1000 / freq;
		}
	}
	else
	{
		regs.Client_ES = 0;
		regs.Client_EDI = 0;
		vesa_bios(&regs);
	}
	
	if(VESA_SUCC(regs))
	{
		DWORD test_x;
		DWORD test_y;

		dbg_printf("SET success: %d %d %d\n", w, h, bpp);
		hda->width  = w;
		hda->height = h;
		hda->bpp    = bpp;
		hda->pitch  = VESA_pitch(w, bpp); //vesa_modes[mode].pitch;
		hda->stride = h * hda->pitch;
		hda->surface = 0;
		
		video_bpp    = vesa_modes[mode].bpp;
		video_pitch  = vesa_modes[mode].pitch;
		video_stride = h * video_pitch;

		/* set DAC to 8BIT if supported */
		if(bpp <= 8)
		{
			if((vesa_caps & VESA_CAP_DAC8BIT) != 0)
			{
				/* set DAC to 8 bpp */
				regs.Client_EAX = VESA_CMD_PALETTE_FORMAT;
				regs.Client_EBX = VESA_DAC_SETFORMAT | VESA_DAC_SET_8BIT;
				vesa_bios(&regs);
				if(VESA_SUCC(regs))
				{
					vesa_pal_bits = (regs.Client_EBX >> 8) & 0xFF;
				}
				else
				{
					vesa_pal_bits = 6;
				}
			}
			else
			{
				vesa_pal_bits = 6;
			}
		}
		num_buffers = 1;

		/* I found there is some deadlock hazard on V86 or PM16 ints,
		 * and when is called on very frame there is big chance on it.
		 * This is why HW double/tripple buffering is available only
		 * when there is PM32 procedure in VBIOS.
		 */
		if(vbios32 && vbios32[VESA_PMTABLE_OFF_DISPLAY_START])
		{
			if(conf_hw_double_buf > 0)
			{
				if(conf_hw_double_buf >= 2)
				{
					/* test if HW flip and vtrace supported */
					offset_calc(video_stride, &test_x, &test_y);
					regs.Client_EAX = VESA_CMD_DISPLAY_START;
					regs.Client_EBX = VESA_DISPLAYSTART_VTRACE;
					regs.Client_ECX = test_x;
					regs.Client_EDX = test_y;
					vesa_bios(&regs);
					if(VESA_SUCC(regs))
					{
						num_buffers = 3;
	
						regs.Client_EAX = VESA_CMD_DISPLAY_START;
						regs.Client_EBX = VESA_DISPLAYSTART_SET;
						regs.Client_ECX = 0;
						regs.Client_EDX = 0;
						vesa_bios(&regs);
					}
				}
	
				if(num_buffers <= 1)
				{
					/* test if HW flip supported */
					offset_calc(video_stride, &test_x, &test_y);
					regs.Client_EAX = VESA_CMD_DISPLAY_START;
					regs.Client_EBX = VESA_DISPLAYSTART_SET;
					regs.Client_ECX = test_x;
					regs.Client_EDX = test_y;
					vesa_bios(&regs);
	
					if(VESA_SUCC(regs))
					{
						num_buffers = 2;
	
						regs.Client_EAX = VESA_CMD_DISPLAY_START;
						regs.Client_EBX = VESA_DISPLAYSTART_SET;
						regs.Client_ECX = 0;
						regs.Client_EDX = 0;
						vesa_bios(&regs);
					}
				}
			}
		}

		hda->system_surface = 0;
		hda->surface        = 0;
		if(num_buffers >= 3)
		{
			hda->flags |= FB_SUPPORT_VSYNC;
		}
		else
		{
			hda->flags &= ~FB_SUPPORT_VSYNC;
		}
		
		/* set VRAM destination */
		wblit.base.ptr = hda->vram_phylin;
		wblit.dst[0].flat.ptr = hda->vram_phylin;
		wblit.dst[1].flat.ptr = NULL;
		wblit.dst[2].flat.ptr = NULL;
		wblit.dst_w     = vesa_modes[mode].width;
		wblit.dst_h     = vesa_modes[mode].height;
		wblit.dst_pitch = vesa_modes[mode].pitch;

		switch(vesa_modes[mode].bpp)
		{
			case 8:  wblit.dst_mode = MODE_8;  break;
			case 15: wblit.dst_mode = MODE_15; break;
			case 16: wblit.dst_mode = MODE_16; break;
			case 24: wblit.dst_mode = MODE_24; break;
			case 32: wblit.dst_mode = MODE_32; break;
		}
	
		if(wblit.dst_w > w || wblit.dst_h > h)
		{
			DWORD scans = wblit.dst_w/w;
			if((scans*h) > wblit.dst_h)
			{
				scans = wblit.dst_w/h;
			}
			
			wblit.dst_scans = scans;
			wblit.dst_padx  = (wblit.dst_w - scans*w) / 2;
			wblit.dst_pady  = (wblit.dst_h - scans*h) / 2;	
		}
		else
		{
			wblit.dst_scans = 1;
			wblit.dst_padx  = 0;
			wblit.dst_pady  = 0;
		}
		
		/* set WRAM source */
		wram->regs.s.width  = w;
		wram->regs.s.height = h;
		switch(bpp)
		{
			case 8:  wram->regs.s.mode = MODE_8;  break;
			case 15: wram->regs.s.mode = MODE_15; break;
			case 16: wram->regs.s.mode = MODE_16; break;
			case 24: wram->regs.s.mode = MODE_24; break;
			case 32: wram->regs.s.mode = MODE_32; break;
		}
		wram->regs.s.pitch = hda->pitch;

		act_mode = mode;

		/* check if we have enought memory for all buffers */
		if(num_buffers >= 2)
		{
			DWORD pages = (video_stride + P_SIZE - 1) / P_SIZE;
			DWORD lines = ((pages * P_SIZE) + video_pitch - 1) / video_pitch;
			DWORD page_stride = lines * video_pitch;

			if(hda->vram_size >= (page_stride + video_stride))
			{
				wblit.dst[1].flat.dw = wblit.dst[0].flat.dw + page_stride;
			}
			else
			{
				num_buffers = 1;
			}
			
			if(num_buffers >= 3)
			{
				if(hda->vram_size >= ((page_stride*2) + video_stride))
				{
					wblit.dst[2].flat.dw = wblit.dst[1].flat.dw + page_stride;
				}
				else
				{
					num_buffers = 2;
				}
			}
		}
		
		async_blit_settime(refresh);

		dbg_printf("mode set, mode_id=%d, i=%d, pitch=%d, buffers=%d\n", vesa_modes[mode].mode_id, mode, wram->regs.s.pitch, num_buffers);
		return TRUE;
	}
	else
	{
		dbg_printf("vbios fail: eax=0x%lX\n", regs.Client_EAX);
	}

	dbg_printf("fail to set %ld %ld %ld\n", w, h, bpp);
	return FALSE;
}

static void mtrr_setup()
{
	if(conf_mtrr && !mttr_was_set)
	{
		if(vram_phy > 1*1024*1024)
		{
			if(MTRR_GetVersion())
			{
				dbg_printf("MTRR supported\n");
				dbg_printf("MTRR_SetPhysicalCacheTypeRange(%lX, 0, %ld, %ld)\n", vram_phy, hda->vram_size, MTRR_FRAMEBUFFERCACHED);
				MTRR_SetPhysicalCacheTypeRange(vram_phy, 0, hda->vram_size, MTRR_FRAMEBUFFERCACHED);
				mttr_was_set = TRUE;
			}
			else
			{
				dbg_printf("MTRR unsupported\n");
			}
		}
	}
}

static void VESA_setptr(DWORD offset, BOOL triplebuf)
{
	if(!VESA_display_start_pm(triplebuf, offset))
	{
		CRS_32 regs;
		DWORD off_x, off_y;
	
		offset_calc(offset, &off_x, &off_y);
	
		load_client_state(&regs);
	
		regs.Client_EAX = VESA_CMD_DISPLAY_START;
		regs.Client_EBX = triplebuf ? VESA_DISPLAYSTART_VTRACE : VESA_DISPLAYSTART_SET;
		regs.Client_ECX = off_x;
		regs.Client_EDX = off_y;
	
		vesa_bios(&regs);
	}
}

BOOL VESA_setmode(DWORD w, DWORD h, DWORD bpp, DWORD rr_min, DWORD rr_max)
{
	BOOL valid;
	
	FBHDA_lock();
	valid = VESA_setmode_phy(w, h, bpp, rr_min, rr_max);
	FBHDA_unlock();
	
	if(valid)
	{
		mtrr_setup();
		VESA_clear();
		mouse_invalidate();
		return TRUE;
	}
	return FALSE;
}

void FBHDA_palette_set(unsigned char index, DWORD rgb)
{
	if(wblit.dst_mode != MODE_8)
	{
		wram->palette[index].dw = rgb;
		return;
	}
	
	if((vesa_caps & VESA_CAP_NONVGA) == 0)
	{
		/* if mode is VGA compatible, we can use VGA regitry */
		outp(VGA_DAC_W_INDEX, index);    /* Set starting index. */
		if(vesa_pal_bits == 8)
		{
			outp(VGA_DAC_DATA,   (rgb >> 16) & 0xFF);
			outp(VGA_DAC_DATA,   (rgb >>  8) & 0xFF);
			outp(VGA_DAC_DATA,    rgb        & 0xFF);
		}
		else
		{
			outp(VGA_DAC_DATA,   (rgb >> 18) & 0x3F);
			outp(VGA_DAC_DATA,   (rgb >> 10) & 0x3F);
			outp(VGA_DAC_DATA,   (rgb >>  2) & 0x3F);
		}
	}
	else
	{
		CRS_32 regs;
		DWORD v86_ptr = vesa_buf_v86+PAL_OFFSET+4*index;

		if(vesa_pal_bits == 8)
		{
			vesa_pal[index].Red     = (rgb >> 16) & 0xFF;
			vesa_pal[index].Green   = (rgb >>  8) & 0xFF;
			vesa_pal[index].Blue    =  rgb        & 0xFF;
		}
		else
		{
			vesa_pal[index].Red     = (rgb >> 18) & 0x3F;
			vesa_pal[index].Green   = (rgb >> 10) & 0x3F;
			vesa_pal[index].Blue    = (rgb >>  2) & 0x3F;
		}

		load_client_state(&regs);
		regs.Client_EAX = VESA_CMD_PALETTE_DATA;
		regs.Client_EBX = VESA_RAMDAC_DATA_SET;
		regs.Client_ECX = 1;
		regs.Client_EDX = index;
		regs.Client_ES  = V86_SEG(v86_ptr);
		regs.Client_EDI = V86_OFF(v86_ptr);
		//regs.Client_EDI = ((DWORD)vesa_buf)+PAL_OFFSET+4*index;
		
		vesa_bios_V86(&regs);
	}
}

DWORD FBHDA_palette_get(unsigned char index)
{
	if(wblit.dst_mode != MODE_8)
	{
		return wram->palette[index].dw;
	}

	if((vesa_caps & VESA_CAP_NONVGA) == 0)
	{
		DWORD r, g, b;
		outp(VGA_DAC_W_INDEX, index);
		r = inp(VGA_DAC_DATA);
		g = inp(VGA_DAC_DATA);
		b = inp(VGA_DAC_DATA);

		if(vesa_pal_bits == 8)
		{
			return (r << 16) | (g << 8) | b;
		}
		else
		{
			return (r << 18) | (g << 10) | (b << 2);
		}
	}
	else
	{
		if(vesa_pal_bits == 8)
		{
			return
				((DWORD)(vesa_pal[index].Red)   << 16) |
				((DWORD)(vesa_pal[index].Green) <<  8) |
				 (DWORD)(vesa_pal[index].Blue);
		}
		else
		{
			return
				((DWORD)(vesa_pal[index].Red)   << 18) |
				((DWORD)(vesa_pal[index].Green) << 10) |
				((DWORD)(vesa_pal[index].Blue)  << 2);
		}
	}

	return 0;
}

static void VESA_draw(blit_t *blit)
{
	if(vga_mode) return;
	if(act_mode < 0) return;

	if(blit->num_changes)
	{
		if(blit->dst[1].flat.ptr == NULL && blit->dst[2].flat.ptr == NULL)
		{
			/* single buffer, write only changes */
			wram_blit(&wblit);
		}
		else if(blit->dst[2].flat.ptr == NULL)
		{
			/* double buffer = write full frame
			
				dst[0] <- visible
				dst[1] <- CPU write
			*/
			DWORD offset;
			{
				void *new_ptr = blit->dst[1].flat.ptr;
				blit->dst[1].flat.ptr = blit->dst[0].flat.ptr;
				blit->dst[0].flat.ptr = new_ptr;
			}
			offset = blit->dst[0].flat.dw - blit->base.dw;
			blit->src_sx = 0;
			blit->src_sy = 0;
			blit->src_ex = hda->width;
			blit->src_ey = hda->height;
			wram_blit(&wblit);
			
			VESA_setptr(offset, FALSE);
		}
		else
		{
			/* triple buffer, write full frame
				dst[0] <- unvisible -> visible (swap on VSYNC)
				dst[1] <- CPU write
				dst[2] <- visible -> unvisible ( will be on next VSYNC)
			*/
			DWORD offset;
			{
				void *old_ptr = blit->dst[0].flat.ptr;
				blit->dst[0].flat.ptr = blit->dst[1].flat.ptr;
				blit->dst[1].flat.ptr = blit->dst[2].flat.ptr;
				blit->dst[2].flat.ptr = old_ptr;
			}
			offset = blit->dst[0].flat.dw - blit->base.dw;
			blit->src_sx = 0;
			blit->src_sy = 0;
			blit->src_ex = hda->width;
			blit->src_ey = hda->height;
			wram_blit(&wblit);
			
			VESA_setptr(offset, TRUE);
		}
		blit->num_changes = 0;
	}
}

BOOL FBHDA_swap(DWORD offset, DWORD flags)
{
	void *new_fb;
	if((flags & FBHDA_SWAP_QUERY) != 0)
	{
		return TRUE;
	}

	new_fb = ((BYTE*)hda->vram_pm32) + offset;
	if(wram_swap(new_fb, &wblit))
	{
		if(!timer_set)
		{
			VESA_draw(&wblit);
		}
		else
		{
			hda->onflip = 1;
		}
		hda->surface = offset;
		return TRUE;
	}
	return TRUE;
}

void FBHDA_access_begin(DWORD flags)
{
	FBHDA_lock();
	
	if(flags & FBHDA_ACCESS_MOUSE_MOVE)
	{
		DWORD left, top, right, bottom;
		if(mouse_get_rect(&left, &top, &right, &bottom))
		{
			wram_changes(&wblit, left, top, right, bottom);
		}
	}
	else
	{
		wram_changes(&wblit, 0, 0, hda->width, hda->height);
	}
	
	FBHDA_unlock();
}

void FBHDA_access_rect(DWORD left, DWORD top, DWORD right, DWORD bottom)
{
	FBHDA_lock();
	wram_changes(&wblit, left, top, right, bottom);
	FBHDA_unlock();
}

void FBHDA_access_end(DWORD flags)
{
	FBHDA_lock();

	if(flags & FBHDA_ACCESS_MOUSE_MOVE)
	{
		DWORD left, top, right, bottom;
		if(mouse_get_rect(&left, &top, &right, &bottom))
		{
			wram_changes(&wblit, left, top, right, bottom);
		}
	}

	fb_lock_cnt--;
	if(fb_lock_cnt < 0) fb_lock_cnt = 0;

	if(!timer_set)
	{
		if(fb_lock_cnt == 0)
		{
			VESA_draw(&wblit);
		}
	}

	FBHDA_unlock();
}

DWORD FBHDA_overlay_setup(DWORD overlay, DWORD width, DWORD height, DWORD bpp)
{
	return 0;
}

void FBHDA_overlay_lock(DWORD left, DWORD top, DWORD right, DWORD bottom)
{

}

void  FBHDA_overlay_unlock(DWORD flags)
{

}

void VESA_HIRES_enable()
{
	dbg_printf("VESA_HIRES_enable\n");
	vga_mode = FALSE;
}

void VESA_HIRES_disable()
{
	dbg_printf("VESA_HIRES_disable\n");
	vga_mode = TRUE;
}

extern BOOL mode_changing;

BOOL FBHDA_mode_query(DWORD index, FBHDA_mode_t *mode)
{
	if(index >= vesa_modes_cnt)
	{
		return FALSE;
	}

	if(mode)
	{
		int i;
		
		mode->cb = sizeof(FBHDA_mode_t);
		mode->width = vesa_modes[index].width;
		mode->height = vesa_modes[index].height;
		mode->bpp = vesa_modes[index].bpp;
		for(i = 0; i < FBHDA_MODE_MAX_REFRESH; i++)
		{
			mode->refresh[i] = vesa_modes[index].freqs[i];
			if(mode->refresh[i] == 0)
			{
				break;
			}
		}
		for(; i < FBHDA_MODE_MAX_REFRESH; i++)
		{
			mode->refresh[i] = 0;
		}
	}

	return TRUE;
}

static BYTE saved_vga_mode = 0xFF;

BOOL VESA_check_int10h(DWORD vm, PCRS_32 regs)
{
	if(vm != ThisVM)
	{
		BYTE ah = (regs->Client_EAX >> 8) & 0xFF;
		BYTE al =  regs->Client_EAX & 0xFF;
		if(ah == 0)
		{
			if(!vga_mode && conf_dos_window == 0)
			{
				saved_vga_mode = al & 0x7F;
				return FALSE;
			}
		}

		if(ah == 0x4F)
		{
			if(al == 0x02) /* mode set */
			{
				if(!vga_mode && conf_dos_window == 0)
				{
					regs->Client_EAX &= 0xFFFF;
					regs->Client_EAX |= 0x034F; /* Function call invalid in current video mode */
					return FALSE;
				}
			}
		}
	}
	return TRUE;
}

#if 0
/* NOTE: idea was forbind switch between window and full screen DOS mode,
         but this currently useless because windows only checking if can out
         but not if can in 
 */
static BOOL is_text_mode(DWORD mode)
{
	switch(mode)
	{
		case 00:
		case 01:
		case 03:
		case 07:
			return TRUE;
	}
	return FALSE;
}

BOOL VESA_check_switch(DWORD mode)
{
	if(vga_mode)
	{
		if(!is_text_mode(mode))
		{
			return FALSE;
		}
	}
	else
	{
		if(is_text_mode(mode))
		{
			return FALSE;
		}
	}
	return TRUE;
}

#else

BOOL VESA_check_switch(DWORD mode)
{
	return TRUE;
}

#endif
