/*****************************************************************************
 * nvshim.c - libdrm_nouveau contract over the nvmini VxD (M5.3b)
 *
 * Implements the ~30 functions in nvshim.h / mesa's nouveau.h on top of the
 * VxD's DeviceIoControl surface, so Mesa's nv50 gallium winsys can drive the
 * G86 with no DRM kernel. Replaces Linux libdrm_nouveau's nouveau.c.
 *
 * Model (bring-up, correctness over speed):
 *  - VRAM is managed by a ring-3 BUMP allocator over a fixed heap window
 *    (NVSHIM_HEAP_BASE..END) that the persistent channel's 1:1 VM map covers.
 *    A buffer object's GPU address is its physical offset + 1 GiB.
 *  - nouveau_bo_map returns a CPU SHADOW (malloc); the real VRAM lives behind
 *    BAR1 which is ring-0 only, so we bounce: flush referenced shadows into
 *    VRAM (OP_NV_VRAM_WRITE) just before a submit, and refresh a shadow from
 *    VRAM (OP_NV_VRAM_READ) when it is mapped for reading. Submits are
 *    synchronous in the VxD, so this gives coherent, in-order semantics.
 *  - A pushbuf is a ring-3 dword buffer; kick hands [buf..cur) to OP_NV_SUBMIT
 *    (nv50 IB mode in the VxD). Addresses are fixed (VM), so relocs are no-ops.
 *
 * Private state uses the container-of pattern (the public struct is the first
 * member) because Mesa owns pushbuf->user_priv. See docs/HW3D_PLAN.md (M5.3).
 * Licensed MIT.
 *****************************************************************************/
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifdef NVSHIM_STANDALONE
#include "nvshim.h"
#include "vxd_nv.h"      /* OP_NV_SUBMIT / OP_NV_VRAM_READ / OP_NV_VRAM_WRITE */
#include "nv50_gpu.h"    /* nv_gpu_submit_t / nv_gpu_vread_t result structs    */
#define NVSHIM_HWINFO_BUILD 22   /* nv_hwinfo_t.build as a dword index (3099)        */
#define NVSHIM_BUILD        NV_BUILD
#else
#include "nouveau.h"       /* Mesa's own copy - ABI identical to nvshim.h */
#include "nvif/cl0080.h"   /* struct nv_device_info_v0 (device_info fields) */
#include "nvshim_ioctl.h"  /* self-contained VxD opcodes + result structs   */
#endif

/* ---- VRAM layout (must match nv50_gpu.c's persistent channel) --------- *
 * The channel reserves 0x03000000..0x03700000 for its instance structures,
 * pushbuffer, IB ring and scratch. BOs live above that, below 128 MiB so the
 * channel's 1:1 VM page table (first 2048 x 64 KiB pages) can translate them. */
#define NVSHIM_HEAP_BASE  0x04000000UL   /* 64 MiB */
#define NVSHIM_HEAP_END   0x08000000UL   /* 128 MiB (end of the VM 1:1 window) */
#ifndef NVSHIM_VM_BASE
#define NVSHIM_VM_BASE    0x40000000UL   /* BO GPU addr = physical offset + 1 GiB */
#endif
#define NVSHIM_G86_UNITS  0xF3010001UL   /* 3094: PMC 0x1540 on the T61's G86 (1 TP x 2 MPs); the fake GPU's answer */
#define NVSHIM_G86_DEVID  0x0429UL       /* 10DE:0429 Quadro NVS 140M */
/* 3096: NV50 PDISPLAY raster state for head 0 (the LVDS panel; nouveau nv50_disp
 * scanoutpos): current raster line, and the mode's vblank end/start + total lines
 * (EVO shadow regs, vertical in bits 16-31) */
#define NV50_PDISPLAY_VLINE0    0x616340UL
#define NV50_PDISPLAY_VBLANKE0  0x610aecUL
#define NV50_PDISPLAY_VBLANKS0  0x610af4UL
#define NV50_PDISPLAY_VTOTAL0   0x610afcUL
/* 3095: a second heap region below the GPU channel's structures. VRAM is 128 MiB
 * and the channel VM maps all of it 1:1; the display driver's window ends at
 * 24 MiB, the EVO reservation (pushbuffer, notifier, LUT, cursor, display
 * instance block) at 32 MiB + 32 KiB (nv50_evo.h NVFB_RESERVED_END), and the
 * GPU channel (nv50_gpu.c NVGPU_RAMIN) starts at 48 MiB. The 15.9 MiB between
 * were unused; Half-Life's first map filled 61 of the 64 MiB above. */
#define NVSHIM_HEAP_LO     0x02010000UL  /* 32 MiB + 64 KiB (page-aligned, above NVFB_RESERVED_END) */
#define NVSHIM_HEAP_LO_END 0x03000000UL  /* 48 MiB = NVGPU_RAMIN */

/* ========================= VxD transport ============================== */
static HANDLE g_vxd = INVALID_HANDLE_VALUE;
static int    g_vxd_owned = 0;   /* did we open it (vs. a handle bound in)? */
static int  shim_fake(void);
static BOOL shim_ioctl(DWORD op, void *in, DWORD insz, void *out, DWORD outsz, DWORD *ret);

void nvshim_bind(void *vxd_handle)
{
	g_vxd = (HANDLE)vxd_handle;
	g_vxd_owned = 0;
}

static int nvshim_open(void)
{
	if(g_vxd != INVALID_HANDLE_VALUE)
		return 1;
	if(shim_fake())
	{
		g_vxd = (HANDLE)(uintptr_t)0x4641;   /* sentinel: the fake backend answers */
		g_vxd_owned = 0;
		return 1;
	}
	g_vxd = CreateFileA("\\\\.\\nvmini.vxd", 0, 0, 0, OPEN_EXISTING,
	                    FILE_ATTRIBUTE_NORMAL, 0);
	if(g_vxd == INVALID_HANDLE_VALUE)
		return 0;
	g_vxd_owned = 1;
	return 1;
}

/* ---- bring-up trace (M5.4): trace the Mesa nv50_screen_create path so a
 * failure on the T61 is diagnosable. Standalone nvprobe has printf and does not
 * need this.
 *
 * PRIMARY sink is a disk file, C:\nvshim.log: the shim runs in ring 3 (the GL
 * app's process), so it can write straight to disk. That is immune to the VxD
 * debug ring's heartbeat flood (the desktop's per-second HB lines wrap a 16 KB
 * ring in seconds and evict the init trace before `nvprobe log` can read it).
 * The file is truncated at first use and flushed per line, so the full trace
 * survives even if screen_create faults. The VxD ring is kept as a secondary
 * sink so the shim lines still interleave with the VxD's own V>/D> events. */
#ifndef NVSHIM_STANDALONE
static HANDLE g_logf = INVALID_HANDLE_VALUE;
static DWORD  g_log_t0 = 0;
static void nvshim_logn(const char *label, unsigned n, const uint32_t *v)
{
	char buf[176];
	int p = 0, i; unsigned k;
	DWORD ret, ms;
	/* 3092: every line carries milliseconds since the first line (profiling) */
	if(g_log_t0 == 0) g_log_t0 = GetTickCount() | 1;
	ms = GetTickCount() - g_log_t0;
	{ char d[12]; int nd = 0; do { d[nd++] = (char)('0' + ms % 10); ms /= 10; } while(ms && nd < 11);
	  buf[p++] = '['; while(nd) buf[p++] = d[--nd]; buf[p++] = ']'; buf[p++] = ' '; }
	for(i = 0; label[i] && p < 130; i++) buf[p++] = label[i];
	for(k = 0; k < n; k++)
	{
		buf[p++] = ' ';
		for(i = 28; i >= 0; i -= 4)
		{ int d = (int)((v[k] >> i) & 0xf); buf[p++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
	}
	buf[p++] = '\n';
	/* primary: C:\nvshim.log. APPEND mode since 3085 (shared read+write, seek to
	 * the end before every line): a second instance of this DLL in the same
	 * process (opengl32 loading the ICD by its own path) can then neither
	 * truncate this instance's lines nor be locked out, and each instance
	 * announces itself once with a per-instance address. Delete the file between
	 * runs (`del C:\nvshim.log`). */
	if(g_logf == INVALID_HANDLE_VALUE)
	{
		g_logf = CreateFileA("C:\\nvshim.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                     0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
		if(g_logf != INVALID_HANDLE_VALUE)
		{
			uint32_t hv[1]; hv[0] = (uint32_t)(uintptr_t)&g_logf;
			nvshim_logn("==== shim log open, instance", 1, hv);
		}
	}
	if(g_logf != INVALID_HANDLE_VALUE)
	{
		SetFilePointer(g_logf, 0, NULL, FILE_END);
		WriteFile(g_logf, buf, (DWORD)p, &ret, NULL);
		/* 3095: a FlushFileBuffers per line (write-through FAT on the T61,
		 * fsync under Wine) cost ~1 ms a line - 34 ms a frame in the stress
		 * scene. Flush the first 512 lines, one line in 64 after that, and
		 * always the lines that matter after a crash or a hang. */
		{
			static uint32_t nl = 0;
			nl++;
			if(nl <= 512 || (nl & 63) == 0 || strstr(label, "WARN") || strstr(label, "FAIL") ||
			   strstr(label, "DEAD") || strstr(label, "TRAP") || strstr(label, "WEDGE") ||
			   strstr(label, "kick") || strstr(label, "DETACH") || strstr(label, "stats"))
				FlushFileBuffers(g_logf);
		}
	}
	/* secondary: the VxD debug ring, to interleave with V>/D> events */
	if(nvshim_open())
		shim_ioctl(OP_NV_DBG_PUTS, buf, (DWORD)p, NULL, 0, &ret);
}
/* Exported for the caller chain (wgl.c, nouveau_drm_winsys.c): trace into the
 * same C:\nvshim.log so one file shows the whole path into screen creation. */
void nvshim_trace(const char *label, unsigned n, const uint32_t *v) { nvshim_logn(label, n, v); }
#define NVLOG0(l)         nvshim_logn((l), 0, NULL)
#define NVLOG1(l,a)       do{ uint32_t _v[1]; _v[0]=(uint32_t)(a); nvshim_logn((l),1,_v);}while(0)
#define NVLOG2(l,a,b)     do{ uint32_t _v[2]; _v[0]=(uint32_t)(a); _v[1]=(uint32_t)(b); nvshim_logn((l),2,_v);}while(0)
#define NVLOG3(l,a,b,c)   do{ uint32_t _v[3]; _v[0]=(uint32_t)(a); _v[1]=(uint32_t)(b); _v[2]=(uint32_t)(c); nvshim_logn((l),3,_v);}while(0)
#else
#define NVLOG0(l)
#define NVLOG1(l,a)
#define NVLOG2(l,a,b)
#define NVLOG3(l,a,b,c)
#endif

/* Submit a method-dword stream to the persistent channel (one kick). */
/* ========================= NVSHIM_FAKE: in-process GPU stand-in ========
 * Set NVSHIM_FAKE=1 in the environment and the shim never opens the VxD: VRAM
 * is a RAM heap, OP_NV_BIND records handles, OP_NV_SUBMIT VALIDATES the method
 * stream the way the PFIFO puller would react to it (unknown object handle,
 * method on a subchannel without an object, bad header, unbound DMA handle,
 * query address outside the VM window -> the kick "wedges", ok=0 + wedged=1,
 * exactly what the VxD reports when the real puller stalls) and emulates the
 * 3D QUERY fence write. Nothing is rendered (the window shows the heap's
 * zeros). Purpose: run Mesa's whole nv50 path - screen, context, every frame
 * of an application - in the Win98 VM or under Wine and catch stream-level
 * faults before a T61 round trip (the 3087/3088 stall was one: an unbound
 * COMPUTE handle at dword 427 of the init kick). NVSHIM_KICKDUMP=N sets how
 * many kicks go to C:\nvkick.txt (default 16; the host decoder reads it). */
static int g_fake = -1;
static int shim_fake(void)
{
	if(g_fake < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_FAKE", b, sizeof(b));
		g_fake = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return g_fake;
}
static int shim_kickdump_limit(void)
{
	static int lim = -1;
	if(lim < 0)
	{
		char b[16]; DWORD n = GetEnvironmentVariableA("NVSHIM_KICKDUMP", b, sizeof(b));
		lim = (n > 0 && n < sizeof(b)) ? atoi(b) : 16;
		if(lim < 0) lim = 0;
	}
	return lim;
}
/* 3096: NVSHIM_PUSHCAP=<dwords> caps the pushbuffer (simulation knob: forces
 * frequent pushbuf-full kicks so packet sequences get cut across kicks the way
 * a long game session cuts them now and then; floor 0x800) */
static uint32_t shim_pushcap(void)
{
	static int32_t cap = -1;
	if(cap < 0)
	{
		char b[16]; DWORD n = GetEnvironmentVariableA("NVSHIM_PUSHCAP", b, sizeof(b));
		cap = (n > 0 && n < sizeof(b)) ? (int32_t)strtoul(b, NULL, 0) : 0;
		if(cap && cap < 0x800) cap = 0x800;
	}
	return (uint32_t)cap;
}
/* 3096: NVSHIM_FULLFLUSH=1 ('nvhl fullflush') - whole-BO dirty-diff at every
 * kick instead of the range-directed one, an A/B switch for the T61: artifacts
 * that stay with it are not the flush ranges' doing */
static int shim_fullflush(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_FULLFLUSH", b, sizeof(b));
		v = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return v;
}
/* 3097: NVSHIM_VERIFYVRAM=1 ('nvhl verify') - after every kick, read the ranges
 * the walk marked (what the engines just read) back from VRAM and compare them
 * with the CPU shadow: anything that overwrites or fails to receive the flushed
 * data shows up as 'VRAM WARN' with the address */
static int shim_verifyvram(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_VERIFYVRAM", b, sizeof(b));
		v = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return v;
}
/* 3097: NVSHIM_HEAPHI=1 ('nvhl heaphi') - only the 64 MiB region above the GPU
 * channel (the 3094 layout), an A/B for the low region added in 3095 */
static int shim_heaphi(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_HEAPHI", b, sizeof(b));
		v = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return v;
}
/* 3096: NVSHIM_SCANRESET=1 restores the 3095 kick walk (M2MF state forgotten at
 * every kick) in the MARK pass only, so the verify pass shows what it missed */
static int shim_scanreset(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_SCANRESET", b, sizeof(b));
		v = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return v;
}

#define FAKE_PAGE_SHIFT 20                         /* 1 MiB pages, allocated on first touch */
#define FAKE_BASE  0x02000000UL                    /* 3095: covers both heap regions */
#define FAKE_PAGES ((NVSHIM_HEAP_END - FAKE_BASE) >> FAKE_PAGE_SHIFT)
static uint8_t *g_fake_page[FAKE_PAGES];
static uint32_t g_fake_handles[64];
static int      g_fake_nhandles = 0;
static uint32_t g_fake_sub[8];                    /* handle bound per subchannel (channel state) */
static uint32_t g_fake_put = 0;                   /* running command-region offset (put_off) */
static uint32_t g_fake_ib  = 0;                   /* kicks accepted (IB get/put index) */

static uint8_t *fake_ptr(uint32_t off, int alloc)
{
	uint32_t pg;
	if(off < FAKE_BASE || off >= NVSHIM_HEAP_END) return NULL;
	pg = (off - FAKE_BASE) >> FAKE_PAGE_SHIFT;
	if(!g_fake_page[pg])
	{
		if(!alloc) return NULL;
		g_fake_page[pg] = (uint8_t*)calloc(1, 1UL << FAKE_PAGE_SHIFT);
		if(!g_fake_page[pg]) return NULL;
	}
	return g_fake_page[pg] + (off & ((1UL << FAKE_PAGE_SHIFT) - 1));
}
static uint32_t fake_rd32(uint32_t off) { uint8_t *p = fake_ptr(off, 0); uint32_t v = 0; if(p) memcpy(&v, p, 4); return v; }
static void     fake_wr32(uint32_t off, uint32_t v) { uint8_t *p = fake_ptr(off, 1); if(p) memcpy(p, &v, 4); }

static int fake_handle_bound(uint32_t h)
{
	int i;
	if(h == 0xbeef502dUL || h == 0xbeef0002UL || h == 0xbeef0003UL) return 1;   /* nv_chan_init's fixed set */
	for(i = 0; i < g_fake_nhandles; i++) if(g_fake_handles[i] == h) return 1;
	return 0;
}
static int fake_class_is_gfx(uint32_t c)
{
	switch(c) { case 0x502d: case 0x5039: case 0x5097: case 0x8297: case 0x8597: case 0x8697: case 0x50c0: case 0x85c0: return 1; }
	return 0;
}

/* Walk one kick like the puller would. Returns 0 when it would stall (and
 * logs why: which dword, which value), 1 when it runs through. */
static int fake_run(const uint32_t *m, uint32_t count)
{
	uint32_t i = 0, qaddr = 0, qseq = 0;
	while(i < count)
	{
		uint32_t h = m[i], typ = h >> 29, subc = (h >> 13) & 7, mthd = h & 0x1ffc, n = (h >> 18) & 0x7ff, k;
		int inc;
		if(typ == 0) inc = 1; else if(typ == 2) inc = 0;
		else { NVLOG3("FAKE WEDGE bad header dword/value/type", i, h, typ); return 0; }
		if(i + 1 + n > count) { NVLOG3("FAKE WEDGE truncated header dword/value/count", i, h, n); return 0; }
		if(mthd == 0 && n == 1)
		{
			if(!fake_handle_bound(m[i + 1])) { NVLOG3("FAKE WEDGE SET_OBJECT unbound handle dword/subch/handle", i, subc, m[i + 1]); return 0; }
			g_fake_sub[subc] = m[i + 1];
			i += 2;
			continue;
		}
		if(g_fake_sub[subc] == 0) { NVLOG3("FAKE WEDGE method on unbound subchannel dword/subch/method", i, subc, mthd); return 0; }
		for(k = 0; k < n; k++)
		{
			uint32_t mm = inc ? mthd + 4 * k : mthd, v = m[i + 1 + k];
			if(mm >= 0x180 && mm < 0x200 && !fake_handle_bound(v))
			{ NVLOG3("FAKE WEDGE DMA method unbound handle dword/method/handle", i + 1 + k, mm, v); return 0; }
			if(subc == 3)   /* 3D: QUERY_ADDRESS_HIGH/LOW/SEQUENCE/GET */
			{
				if(mm == 0x1b00) qaddr = 0;             /* high dword: the VM window is below 4 GiB */
				else if(mm == 0x1b04) qaddr = v;
				else if(mm == 0x1b08) qseq = v;
				else if(mm == 0x1b0c)
				{
					if(qaddr < NVSHIM_VM_BASE + NVSHIM_HEAP_LO || qaddr >= NVSHIM_VM_BASE + NVSHIM_HEAP_END)
					{ NVLOG3("FAKE WEDGE QUERY address outside VM window dword/addr/get", i + 1 + k, qaddr, v); return 0; }
					fake_wr32(qaddr - NVSHIM_VM_BASE, qseq);
				}
			}
		}
		i += 1 + n;
	}
	return 1;
}

static BOOL fake_ioctl(DWORD op, void *in, DWORD insz, void *out, DWORD outsz, DWORD *ret)
{
	const uint32_t *w = (const uint32_t *)in;
	if(ret) *ret = 0;
	switch(op)
	{
	case OP_NV_BIND:
	{
		nv_gpu_bind_t *bd = (nv_gpu_bind_t *)out;
		if(insz < 8 || outsz < sizeof(*bd)) return FALSE;
		memset(bd, 0, sizeof(*bd));
		bd->handle = w[0]; bd->oclass = w[1];
		bd->inst   = 0x03008000UL + (uint32_t)g_fake_nhandles * 0x100UL;
		bd->engine = fake_class_is_gfx(w[1]) ? 1 : 0;
		bd->ok     = 1;
		if(g_fake_nhandles < 64 && !fake_handle_bound(w[0])) g_fake_handles[g_fake_nhandles++] = w[0];
		if(ret) *ret = sizeof(*bd);
		return TRUE;
	}
	case OP_NV_VRAM_READ:
	{
		nv_gpu_vread_t *vr = (nv_gpu_vread_t *)out; uint32_t i;
		if(insz < 8 || outsz < sizeof(*vr)) return FALSE;
		memset(vr, 0, sizeof(*vr));
		vr->offset = w[0]; vr->count = w[1] > 64 ? 64 : w[1];
		for(i = 0; i < vr->count; i++) vr->data[i] = fake_rd32(w[0] + i * 4);
		vr->ok = 1;
		if(ret) *ret = sizeof(*vr);
		return TRUE;
	}
	case OP_NV_VRAM_READ2:
	{
		uint32_t *o = (uint32_t *)out, i, n;
		if(insz < 8 || !o) return FALSE;
		n = w[1] > 1024 ? 1024 : w[1];
		if(outsz < n * 4) n = outsz / 4;
		for(i = 0; i < n; i++) o[i] = fake_rd32(w[0] + i * 4);
		if(ret) *ret = n * 4;
		return TRUE;
	}
	case OP_NV_VRAM_WRITE:
	{
		uint32_t i;
		if(insz < 8 || insz < (2 + w[1]) * 4) return FALSE;
		for(i = 0; i < w[1]; i++) fake_wr32(w[0] + i * 4, w[2 + i]);
		return TRUE;
	}
	case OP_NV_SUBMIT:
	{
		nv_gpu_submit_t *sb = (nv_gpu_submit_t *)out; uint32_t count = insz / 4;
		if(outsz < sizeof(*sb)) return FALSE;
		memset(sb, 0, sizeof(*sb));
		sb->put_off = 0x03200000UL + g_fake_put;
		sb->ctxctl  = 0x80003000UL;
		if(!fake_run(w, count))
		{
			sb->ok = 0; sb->wedged = 1;
			sb->get = g_fake_ib; sb->put = g_fake_ib + 1; sb->idle_loops = 2000000UL;
		}
		else
		{
			g_fake_put = (g_fake_put + count * 4) & 0x000fffffUL;
			g_fake_ib  = (g_fake_ib + 1) & 255;
			sb->ok = 1; sb->get = sb->put = g_fake_ib; sb->idle_loops = 1;
		}
		if(ret) *ret = sizeof(*sb);
		return TRUE;
	}
	case OP_NV_VM_KIND:
		if(out && outsz >= 4) *(uint32_t *)out = 1;   /* accepted (no page tables to tag) */
		return TRUE;
	case OP_NV_RD32:
	{
		/* 3094: the one MMIO read on Mesa's path - PMC 0x1540 (GRAPH_UNITS) */
		uint32_t *o = (uint32_t *)out, i, n;
		if(insz < 8 || !o) return FALSE;
		n = w[1] ? w[1] : 1; if(outsz < n * 4) n = outsz / 4;
		for(i = 0; i < n; i++)
		{
			uint32_t a = w[0] + i * 4;
			if(a == 0x1540UL)                    o[i] = NVSHIM_G86_UNITS;
			else if(a == NV50_PDISPLAY_VLINE0)   { static uint32_t raster = 0; o[i] = (raster += 7) % 525; }   /* 3096: a raster that advances 7 lines per poll (640x480 timing; the tick counter is too coarse under Wine) */
			else if(a == NV50_PDISPLAY_VBLANKE0) o[i] = 3 << 16;
			else if(a == NV50_PDISPLAY_VBLANKS0) o[i] = 484 << 16;
			else if(a == NV50_PDISPLAY_VTOTAL0)  o[i] = (525 << 16) | 800;
			else                                 o[i] = 0;
		}
		if(ret) *ret = n * 4;
		return TRUE;
	}
	default:
		return TRUE;   /* OP_NV_DBG_PUTS and anything else: accepted, no effect */
	}
}

static BOOL shim_ioctl(DWORD op, void *in, DWORD insz, void *out, DWORD outsz, DWORD *ret)
{
	if(shim_fake())
		return fake_ioctl(op, in, insz, out, outsz, ret);
	return DeviceIoControl(g_vxd, op, in, insz, out, outsz, ret, NULL);
}

static int g_gpu_dead = 0;   /* 3087: set after a wedged/failed kick; no more submits */
static int g_submit_data = 0; /* 3094: the submit in flight is a pushbuf_data payload (index data), not methods */
/* 3095: why a kick happened - 0 Mesa's own PUSH_KICK (flush/swap/fence wait),
 * 1 pushbuf full, 2 kref list full, 3 bo_map, 4 bo_wait, 5 pushbuf_data
 * fallback. Half-Life on 3094 made ~1,160 kicks per frame, all of them from
 * the shim kicking on every bo_map; the stats line now says which. */
static int      g_kick_src = 0;
static uint32_t g_st_kick_src[8];
static uint32_t g_st_submit_ms = 0;   /* 3095: ms spent inside OP_NV_SUBMIT (GetTickCount) */
#ifndef NVSHIM_STANDALONE
/* 3088: dump the full method stream of the first kicks to C:\nvkick.txt so a
 * silent engine hang can be decoded offline against the class headers. */
static void nvshim_dump_kick(int idx, const uint32_t *m, uint32_t count)
{
	static HANDLE f = INVALID_HANDLE_VALUE;
	char line[16 + 8 * 9 + 2]; int p; uint32_t i, k; DWORD ret;
	if(f == INVALID_HANDLE_VALUE)
		f = CreateFileA("C:\\nvkick.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if(f == INVALID_HANDLE_VALUE) return;
	{	/* 3095: whole-frame kicks are large; cap the dump at 8 MB */
		static DWORD total = 0;
		total += 24 + count * 9;
		if(total > (8u << 20)) return;
	}
	SetFilePointer(f, 0, NULL, FILE_END);
	p = 0; { const char *h = "---- kick "; while(*h) line[p++] = *h++; }
	{ char d[12]; int n = 0, v = idx; do { d[n++] = (char)('0' + v % 10); v /= 10; } while(v); while(n) line[p++] = d[--n]; }
	{ const char *h = " cnt="; while(*h) line[p++] = *h++; }
	{ char d[12]; int n = 0; uint32_t v = count; do { d[n++] = (char)('0' + v % 10); v /= 10; } while(v); while(n) line[p++] = d[--n]; }
	if(g_submit_data) { const char *h = " data"; while(*h) line[p++] = *h++; }   /* 3094: not methods */
	line[p++] = '\n'; WriteFile(f, line, (DWORD)p, &ret, NULL);
	for(i = 0; i < count; i += 8)
	{
		p = 0;
		for(k = i; k < i + 8 && k < count; k++)
		{
			int s; uint32_t v = m[k];
			for(s = 28; s >= 0; s -= 4) { int d = (int)((v >> s) & 0xf); line[p++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
			line[p++] = ' ';
		}
		line[p++] = '\n'; WriteFile(f, line, (DWORD)p, &ret, NULL);
	}
	FlushFileBuffers(f);
}
#endif
/* 3092: per-run counters, read by wgl.c's present stats line (nvshim_stats) */
static uint32_t g_st_kicks = 0, g_st_flush_dw = 0, g_st_read_dw = 0, g_st_traps = 0, g_st_kick_dw = 0;
static uint32_t g_st_warn = 0;   /* 3094: KICK WARN count (dead or unflushed DMA sources) */
static uint32_t g_st_m2mf_split = 0;                    /* 3096: M2MF copies whose PITCH_IN came from an earlier kick */
static uint32_t g_st_vsync_n = 0, g_st_vsync_ms = 0;    /* 3096: NVSHIM_VSYNC waits before the present blit, total ms */
static uint32_t g_st_vchk = 0, g_st_vbad = 0;            /* 3097: NVSHIM_VERIFYVRAM readbacks, mismatching BOs */
/* 3095: where the shim's own CPU time goes, in TSC cycles (reported /1000):
 * 0 validate (kref), 1 bufctx add/reset, 2 kick (whole), 3 of which flush,
 * 4 bo_new + bo_map. rdtsc: ~20 cycles, unlike Win9x's port-I/O QPC. */
static uint64_t g_st_kc[5];
static __inline uint64_t shim_tsc(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}
void nvshim_stats(uint32_t *v)
{
	v[0] = g_st_kicks; v[1] = g_st_flush_dw; v[2] = g_st_read_dw; v[3] = g_st_traps; v[4] = g_st_kick_dw;
	v[5] = g_st_warn;
	v[6] = g_st_kick_src[3] + g_st_kick_src[4];                       /* 3095: kicks forced by bo_map/bo_wait */
	v[7] = g_st_kick_src[1] + g_st_kick_src[2] + g_st_kick_src[5] + g_st_kick_src[6];    /* pushbuf full / kref list full / data fallback / heap full (3097) */
	v[8] = g_st_submit_ms;
	v[9]  = (uint32_t)((g_st_kc[0] + g_st_kc[1]) / 1000);   /* validate + bufctx kcycles */
	v[10] = (uint32_t)(g_st_kc[2] / 1000);                  /* kick kcycles (incl. flush + submit) */
	v[11] = (uint32_t)(g_st_kc[3] / 1000);                  /* flush kcycles */
	v[12] = (uint32_t)(g_st_kc[4] / 1000);                  /* bo_new + bo_map kcycles */
	v[13] = g_st_vsync_n; v[14] = g_st_vsync_ms;            /* 3096 */
	v[15] = g_st_m2mf_split;
	v[16] = g_st_vchk; v[17] = g_st_vbad;                   /* 3097 */
}
static void shim_refresh_queries(const uint32_t *m, uint32_t count);
static void shim_kick_scan(const uint32_t *m, uint32_t count, int verify);
static int vxd_submit(const uint32_t *methods, uint32_t count)
{
	nv_gpu_submit_t sb;
	DWORD ret = 0, t0, t1;
#ifndef NVSHIM_STANDALONE
	static int nkick = 0;
#endif
	if(count == 0)
		return 0;
	if(!nvshim_open())
		return -1;
#ifndef NVSHIM_STANDALONE
	if(g_gpu_dead)
	{
		static int nrefused = 0;
		if(nrefused++ < 4) NVLOG1("kick refused: GPU dead, cnt", count);
		return -1;
	}
	/* 3087: log BEFORE the kick too (first 400), so a kick that never returns
	 * from the VxD is identified by its count and first two methods. */
	if(nkick < 400) NVLOG3("kick > cnt/m0/m1", count, methods[0], count > 1 ? methods[1] : 0);
	if(nkick < shim_kickdump_limit()) nvshim_dump_kick(nkick, methods, count);
#endif
	/* 3094: every DMA source this stream names must be a live, flushed BO
	 * (verify pass of the same walk that directed the flush) */
	if(!g_submit_data)
		shim_kick_scan(methods, count, 1);
	memset(&sb, 0, sizeof(sb));
	t0 = GetTickCount();
	if(!shim_ioctl(OP_NV_SUBMIT, (void*)methods, count * 4,
	               &sb, sizeof(sb), &ret) || !sb.ok)
	{
#ifndef NVSHIM_STANDALONE
		/* Log the failure with the GPU state so a TRAP during screen_create/draw
		 * is visible, then stop submitting: the app keeps running on the CPU side
		 * (garbage window, complete log) instead of freezing the machine in the
		 * VxD's per-kick polls. */
		static int nfail = 0;
		if(nfail++ < 32) NVLOG3("kick FAIL cnt/get/put", count, sb.get, sb.put);
		if(nfail <= 32) NVLOG3("kick FAIL pfi/pgi/wedged", sb.pfifo_intr, sb.pgraph_intr, sb.wedged);
		/* 3089: the pusher/puller state the VxD already captured (where the
		 * pusher's DMA GET sits, the cache fill, the header being parsed). */
		if(nfail <= 32) NVLOG3("kick FAIL dmaget/dmaput/state", sb.c1_get, sb.c1_put, sb.dma_state);
		if(nfail <= 32) NVLOG3("kick FAIL pull0/dmapush/push0", sb.pull0, sb.dma_push, sb.push0);
		if(nfail <= 32) NVLOG3("kick FAIL caches/ctxtab/putoff", sb.caches, sb.ctxtable, sb.put_off);
		if(!g_gpu_dead) { g_gpu_dead = 1; NVLOG1("GPU DEAD - submissions stopped, loops", sb.idle_loops); }
#endif
		return -1;
	}
	t1 = GetTickCount(); g_st_submit_ms += t1 - t0;
#ifndef NVSHIM_STANDALONE
	if(nkick < 400) NVLOG3("kick ok cnt/put/pgi", count, sb.put_off, sb.pgraph_intr);
	else if((nkick & 255) == 0) NVLOG3("kick #/ms/cnt", nkick, t1 - t0, count);   /* 3095: one sample per 256 kicks */
	nkick++;
	/* 3091: the VxD recovered PGRAPH trap(s) inside this kick and carried on;
	 * the unit bits + the per-unit dump are in nvtail (S> TRAP ...). */
	if(sb.traps) NVLOG3("kick TRAP recovered cnt/traps/units", count, sb.traps, sb.trap_units);
#endif
	g_st_kicks++; g_st_kick_dw += count; g_st_traps += sb.traps;
	/* 3089b: kicks are synchronous, so every 3D QUERY (fence) write in this
	 * stream has landed in VRAM; pull those dwords into the owning BO's shadow
	 * so Mesa's fence.update (which reads the screen fence BO's map directly,
	 * never through bo_map) sees the sequence. Before this, no fence ever
	 * signalled on the T61 and every nouveau_fence_wait quietly failed. */
	shim_refresh_queries(methods, count);
	return 0;
}

/* Copy `dwords` dwords from VRAM byte `off` into `dst` (shadow refresh). */
static void vxd_vram_read(uint32_t off, uint32_t *dst, uint32_t dwords)
{
	DWORD ret;
	uint32_t in[2];
	nv_gpu_vread_t vr;
	uint32_t done = 0;
	if(!nvshim_open())
		return;
#ifndef NVSHIM_STANDALONE
	if(dwords > 4096) NVLOG2("vram_read > off/dwords", off, dwords);
#endif
	(void)vr;
	g_st_read_dw += dwords;
	while(done < dwords)
	{
		/* 3092: 1024 dwords per ioctl straight into the caller's buffer */
		uint32_t chunk = dwords - done;
		if(chunk > 1024) chunk = 1024;
		in[0] = off + done * 4;
		in[1] = chunk;
		if(!shim_ioctl(OP_NV_VRAM_READ2, in, sizeof(in),
		               dst + done, chunk * 4, &ret))
			return;
		done += chunk;
	}
#ifndef NVSHIM_STANDALONE
	if(dwords > 4096) NVLOG1("vram_read < dwords", dwords);
#endif
}

#ifndef NVSHIM_STANDALONE
/* Bind an object at `handle`/`oclass` into the persistent channel's RAMHT so a
 * method stream can reference it by handle (M5.3c). Used for Mesa's engine and
 * ctxdma objects via nouveau_object_new. */
static void vxd_bind(uint32_t handle, uint32_t oclass)
{
	nv_gpu_bind_t bd;
	DWORD ret = 0;
	uint32_t in[2];
	if(!nvshim_open())
		return;
	in[0] = handle; in[1] = oclass;
	memset(&bd, 0, sizeof(bd));
	shim_ioctl(OP_NV_BIND, in, sizeof(in), &bd, sizeof(bd), &ret);
	/* 3089b: name every bound handle in the log so the host validator
	 * (tools/nvkick_decode.py --check) knows the channel's object set. */
	NVLOG3("bind handle/class/engine", handle, oclass, bd.ok ? bd.engine : 0xffffffffUL);
}
#endif

/* Copy `dwords` dwords from `src` into VRAM byte `off` (shadow flush). */
static void vxd_vram_write(uint32_t off, const uint32_t *src, uint32_t dwords)
{
	DWORD ret;
	uint32_t done = 0;
	/* inBuf = [off, count, data...]; keep each ioctl well under a page. */
	static uint32_t buf[2 + 1024];
	if(!nvshim_open())
		return;
	while(done < dwords)
	{
		uint32_t chunk = dwords - done;
		if(chunk > 1024) chunk = 1024;
		buf[0] = off + done * 4;
		buf[1] = chunk;
		memcpy(&buf[2], src + done, chunk * 4);
		if(!shim_ioctl(OP_NV_VRAM_WRITE, buf, (2 + chunk) * 4,
		               &ret, sizeof(ret), &ret))
			return;
		done += chunk;
	}
}

/* 3091: tag the 64 KiB pages of [phys, phys+size) with a storage kind (Mesa's
 * nv50 memtype & 0x7f, nouveau's PTE bits 40-46) so tiled surfaces render;
 * kind 0 restores linear. The VxD flushes PGRAPH's TLB. */
static void vxd_vmkind(uint32_t phys, uint32_t size, uint32_t kind)
{
	uint32_t in[3]; DWORD r = 0, ret = 0;
	if(!nvshim_open())
		return;
	in[0] = phys; in[1] = size; in[2] = kind;
	shim_ioctl(OP_NV_VM_KIND, in, sizeof(in), &r, sizeof(r), &ret);
	{	/* 3095: first 512 then one in 256, like bo_new; a failure (bit 31) always */
		static uint32_t nk = 0;
		if(nk < 512 || (nk & 255) == 0 || !r) NVLOG3("bo_kind phys/size/kind", phys, size, kind | (r ? 0 : 0x80000000UL));
		nk++;
	}
}

/* ========================= VRAM heap ================================== */
/* M5.4 present: the most recently created pushbuf. libdrm's bo_map/bo_wait
 * implicitly kick the client's pending pushbuf before waiting on a BO; Mesa's
 * nv50 transfer_map relies on that (it queues an M2MF de-tile copy, then maps
 * the staging BO). Mirror it here so a read-map sees the GPU's result. */
struct shim_push_hdr { struct nouveau_pushbuf base; uint32_t *buf; };
static struct nouveau_pushbuf *g_cur_push = NULL;
static void shim_flush_pending(void)
{
	if(g_cur_push && g_cur_push->cur != (uint32_t*)((struct shim_push_hdr*)g_cur_push)->buf)
		nouveau_pushbuf_kick(g_cur_push);
}

/* 3090: a real allocator. The bump heap (with its top-most-only rollback) ran
 * out after ~53 frames in the Wine fake-GPU run (each frame allocates a fence
 * BO, a present staging BO and, now and then, a 1.25 MB surface that is not
 * the top-most block when freed) and Mesa crashed on the failed allocation.
 * First-fit over an address-sorted free list; frees coalesce with neighbours.
 * Granularity 256 bytes; tiled BOs (config.nv50.tile_mode) are aligned to the
 * 64 KiB VM page like nouveau does, everything else to the caller's align. */
#define HEAP_MAXFREE 1024
static struct { uint32_t start, size; } g_free[HEAP_MAXFREE];
static int g_nfree = -1;                      /* -1 = not initialised */
static uint32_t g_heap_used = 0, g_heap_allocs = 0;

/* 3109: nouveau's nv50 storage-kind classes (nvkm/subdev/mmu/nv50.c
 * nv50_mmu_kind): 0x01 = no bank swizzle, 0x02 = bank swizzled, 0x7f =
 * invalid. The Tesla memory controller swizzles the DRAM banks of 0x02 kinds
 * (every depth/stencil format - Z24S8 is 0x28 -, Z16 0x6c, the scanout colour
 * kind 0x7a, ...) and not those of 0x01 kinds (linear 0x00, the 0x70 colour
 * kind of render targets and textures). Two surfaces of different classes
 * must not share one memory ROW BLOCK, or their bytes land on the same DRAM
 * cells: nouveau's VRAM allocator (nvkm_mm_head) rounds an allocation away
 * from a differently-classed neighbour to the row block, which it sizes from
 * the memory controller (nv50_fb_vram_rblock). The shim packed every BO back
 * to back, so the depth buffer of Half-Life's 1024x768 window (0x28, between
 * the 0x70 colour buffer and a linear BO) had its first and its last tile
 * corrupted by its neighbours: the depth test failed there and the screen
 * showed one untouched tile at the top-left and one at the bottom-right
 * (T61 photos, build 3105; 'nvhl hidepth' - the depth buffer alone in the
 * empty 64 MiB+ region - was clean). */
static const uint8_t nv50_kind_class[128] = {
	0x01, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, /* 0x00 */
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x01, 0x01, 0x01, 0x01, 0x7f, 0x7f, 0x7f, 0x7f, /* 0x10 */
	0x02, 0x02, 0x02, 0x02, 0x7f, 0x7f, 0x7f, 0x7f,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x7f, /* 0x20 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, /* 0x30 */
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, /* 0x40 */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x01, 0x01, 0x7f, /* 0x50 */
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x7f, /* 0x60 */
	0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
	0x01, 0x7f, 0x02, 0x7f, 0x01, 0x7f, 0x02, 0x7f, /* 0x70 */
	0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0x7f, 0x7f
};
static int shim_kind_class(uint32_t kind)
{
	uint8_t c = (kind < 128) ? nv50_kind_class[kind] : 0x7f;
	return (c == 0x02) ? 2 : 1;          /* unknown kinds count as unswizzled */
}
static uint32_t shim_rd32(uint32_t off);
static uint32_t g_rblock = 0;            /* row block in bytes, 0 = separation off */
static void heap_rblock_init(void)
{
	char b[16]; DWORD n = GetEnvironmentVariableA("NVSHIM_RBLOCK", b, sizeof(b));
	uint32_t r0 = 0, r4 = 0, rt = 0, r1540 = 0, parts = 0, banks, colbits, rowsize;
	if(n > 0 && n < sizeof(b))             /* NVSHIM_RBLOCK=0 off, =<bytes> override */
	{
		g_rblock = (uint32_t)strtoul(b, NULL, 0);
		NVLOG1("heap: rblock from NVSHIM_RBLOCK", g_rblock);
		return;
	}
	if(!shim_fake())
	{
		r0 = shim_rd32(0x100200); r4 = shim_rd32(0x100204);
		rt = shim_rd32(0x100250); r1540 = shim_rd32(0x001540);
	}
	if(r4 == 0xffffffffUL || r4 == 0 || r1540 == 0xffffffffUL)
	{
		g_rblock = 0x18000;                  /* no registers (fake GPU): the T61's worst case */
		NVLOG1("heap: rblock default (no PFB regs)", g_rblock);
		return;
	}
	/* nouveau nv50_fb_vram_rblock + nv50_ram_ctor (parts = partitions in 0x1540) */
	{ uint32_t m = (r1540 >> 16) & 0xff; while(m) { parts += m & 1; m >>= 1; } }
	if(!parts) parts = 1;
	colbits = (r4 & 0x0000f000) >> 12;
	banks   = 1u << (((r4 & 0x03000000) >> 24) + 2);
	rowsize = parts * banks * (1u << colbits) * 8;
	g_rblock = (rt & 1) ? rowsize * 3 : rowsize;
	if(g_rblock < 0x1000 || g_rblock > 0x100000) g_rblock = 0x18000;
	NVLOG3("heap: memcfg 100200/100204/100250", r0, r4, rt);
	NVLOG2("heap: parts(1540)/rblock", r1540, g_rblock);
}
static void heap_init(void)
{
	/* 3095: lowest first, so the first-fit allocator fills the 15.9 MiB gap
	 * below the GPU channel before the 64 MiB above it (88 MiB in all) */
	heap_rblock_init();
	g_free[0].start = NVSHIM_HEAP_LO;
	g_free[0].size  = NVSHIM_HEAP_LO_END - NVSHIM_HEAP_LO;
	g_free[1].start = NVSHIM_HEAP_BASE;
	g_free[1].size  = NVSHIM_HEAP_END - NVSHIM_HEAP_BASE;
	g_nfree = 2;
	if(shim_heaphi()) { g_free[0] = g_free[1]; g_nfree = 1; NVLOG0("heap: high region only (NVSHIM_HEAPHI)"); }
}
static void heap_stats(uint32_t *free_total, uint32_t *largest)
{
	int i; uint32_t t = 0, l = 0;
	if(g_nfree < 0) heap_init();
	for(i = 0; i < g_nfree; i++) { t += g_free[i].size; if(g_free[i].size > l) l = g_free[i].size; }
	*free_total = t; *largest = l;
}
/* 3108: NVSHIM_HIKIND=depth|color|tiled ('nvhl hidepth' / 'hicolor' / 'hitiled'):
 * BOs of that storage class are placed in the 64 MiB+ heap region first (the
 * low 32-48 MiB region only when the high one is full). The T61 showed the
 * 1024x768 holes only with the default layout, where the window buffer and
 * its depth buffer are the first allocations and land in the low region, and
 * not with NVSHIM_HEAPHI (everything high): this switch says WHICH of the two
 * buffers it is without starving the textures the way heaphi does. */
static int shim_hikind(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[16]; DWORD n = GetEnvironmentVariableA("NVSHIM_HIKIND", b, sizeof(b));
		v = 0;
		if(n > 0 && n < sizeof(b))
		{
			if(!strcmp(b, "depth")) v = 1;
			else if(!strcmp(b, "color")) v = 2;
			else if(!strcmp(b, "tiled")) v = 3;
		}
	}
	return v;
}
static int shim_kind_wants_hi(uint32_t kind)
{
	int hk = shim_hikind();
	if(!kind || !hk) return 0;
	/* nv50 storage kinds: 0x18-0x2f and 0x40-0x6f depth/stencil (Z24S8 0x28,
	 * X8Z24 0x18, Z32F 0x40, Z16 0x6c), 0x70+ colour */
	return hk == 3 || (hk == 1 && kind < 0x70) || (hk == 2 && kind >= 0x70);
}
/* 3109: the class of the live BO that ends exactly at / starts exactly at an
 * address; a boundary with no BO (region edges: the EVO area below the low
 * region, the VxD's channel structures above it) counts as class 1 (linear). */
static int shim_class_ending_at(uint32_t addr);
static int shim_class_starting_at(uint32_t addr);
static uint32_t heap_alloc_in(uint32_t size, uint32_t align, uint32_t lowest, int cls)
{
	int i;
	for(i = 0; i < g_nfree; i++)
	{
		uint32_t bs = g_free[i].start, be = g_free[i].start + g_free[i].size;
		uint32_t s = bs, e = be, a;
		if(g_rblock)
		{	/* nvkm_mm_head: keep away from differently-classed neighbours */
			if(shim_class_ending_at(bs) != cls)   s = (bs + g_rblock - 1) / g_rblock * g_rblock;
			if(shim_class_starting_at(be) != cls) e = be / g_rblock * g_rblock;
			if(s < bs || e > be || s >= e) continue;
		}
		a = (s + (align - 1)) & ~(align - 1);
		if(a < lowest) a = (lowest + (align - 1)) & ~(align - 1);
		if(a < s || a + size > e || a + size < a)
			continue;
		/* carve [a, a+size) out of block i: head [bs,a) stays as block i (or
		 * is removed), tail [a+size, be) becomes a new block after it */
		if(a + size < be)
		{
			if(g_nfree >= HEAP_MAXFREE) continue;          /* no room to split */
			memmove(&g_free[i + 1], &g_free[i], (size_t)(g_nfree - i) * sizeof(g_free[0]));
			g_nfree++;
			g_free[i + 1].start = a + size;
			g_free[i + 1].size  = be - (a + size);
		}
		if(a > bs)
			g_free[i].size = a - bs;
		else
		{
			memmove(&g_free[i], &g_free[i + 1], (size_t)(g_nfree - i - 1) * sizeof(g_free[0]));
			g_nfree--;
		}
		g_heap_used += size;
		g_heap_allocs++;
		return a;
	}
	return 0;                                   /* out of VRAM */
}
static uint32_t heap_alloc(uint32_t size, uint32_t align, int hi, int cls)
{
	uint32_t a;
	if(g_nfree < 0) heap_init();
	if(align < 0x100) align = 0x100;
	size = (size + 0xff) & ~0xffUL;
	if(hi && (a = heap_alloc_in(size, align, NVSHIM_HEAP_BASE, cls)) != 0) return a;
	return heap_alloc_in(size, align, 0, cls);
}
static uint32_t g_heap_frees = 0;
static void heap_free(uint32_t start, uint32_t size)
{
	g_heap_frees++;
	int i;
	size = (size + 0xff) & ~0xffUL;
	if(g_nfree < 0 || size == 0) return;
	for(i = 0; i < g_nfree && g_free[i].start < start; i++) ;
	/* coalesce with the previous block */
	if(i > 0 && g_free[i - 1].start + g_free[i - 1].size == start)
	{
		g_free[i - 1].size += size;
		if(i < g_nfree && g_free[i - 1].start + g_free[i - 1].size == g_free[i].start)
		{
			g_free[i - 1].size += g_free[i].size;
			memmove(&g_free[i], &g_free[i + 1], (size_t)(g_nfree - i - 1) * sizeof(g_free[0]));
			g_nfree--;
		}
	}
	else if(i < g_nfree && start + size == g_free[i].start)
	{
		g_free[i].start = start;
		g_free[i].size += size;
	}
	else
	{
		if(g_nfree >= HEAP_MAXFREE) return;             /* leak rather than corrupt */
		memmove(&g_free[i + 1], &g_free[i], (size_t)(g_nfree - i) * sizeof(g_free[0]));
		g_free[i].start = start;
		g_free[i].size  = size;
		g_nfree++;
	}
	g_heap_used -= size;
}

/* ========================= private structs ============================ */
#define SHIM_MAXREF 2048   /* 3095: was 512 - a GoldSrc frame references several hundred texture BOs */

struct shim_push;
struct shim_bo {
	struct nouveau_bo base;
	int      refcnt;
	uint32_t phys;        /* VRAM byte offset */
	uint32_t shadow_sz;   /* bytes backing base.map, 0 until first map */
	uint32_t asize;       /* bytes actually allocated (tiled: rounded to 64 KiB) */
	uint32_t kind;        /* 3091: PTE storage kind set for this BO (0 = linear) */
	uint8_t *clean;       /* 3092: copy of the shadow as last flushed/refreshed (dirty diffing) */
	int      external;    /* 3092: wraps memory that is not ours (the scanout front buffer) */
	int      nkref;       /* 3095: pushbuf kref lists holding this BO = pending work references it */
	struct shim_push *kref_push;   /* 3095: the pushbuf that kref'd it last */
	uint32_t kref_gen;    /* 3095: that pushbuf's list generation (dedup stamp) */
	uint32_t kgen;        /* 3095: kick generation the range below belongs to */
	uint32_t klo, khi;    /* 3095: union of the byte ranges this kick's stream reads (BO offsets) */
	int      kwhole;      /* 3095: referenced in a way the walk cannot bound: flush it whole */
};
#define SHIM_MAXBO 4096
static struct shim_bo *g_bos[SHIM_MAXBO];   /* live BOs, for address -> BO lookups */
static int             g_nbos = 0;
/* 3095: the registry is kept sorted by phys and searched by bisection - the
 * kick walk looks up a BO per vertex array per draw (thousands a frame in a
 * game) and Half-Life keeps ~2,000 BOs alive. */
static int shim_bo_index(uint32_t phys)      /* index of the last BO with ->phys <= phys, or -1 */
{
	int lo = 0, hi = g_nbos - 1, r = -1;
	while(lo <= hi)
	{
		int mid = (lo + hi) >> 1;
		if(g_bos[mid]->phys <= phys) { r = mid; lo = mid + 1; } else hi = mid - 1;
	}
	return r;
}
static struct shim_bo *shim_bo_at(uint32_t phys)
{
	int i = shim_bo_index(phys);
	if(i >= 0 && phys < g_bos[i]->phys + (uint32_t)g_bos[i]->base.size)
		return g_bos[i];
	return NULL;
}
static int shim_class_ending_at(uint32_t addr)
{
	int i = addr ? shim_bo_index(addr - 1) : -1;
	if(i >= 0 && g_bos[i]->phys + g_bos[i]->asize == addr && !g_bos[i]->external)
		return shim_kind_class(g_bos[i]->kind);
	return 1;
}
static int shim_class_starting_at(uint32_t addr)
{
	int i = shim_bo_index(addr);
	if(i >= 0 && g_bos[i]->phys == addr && !g_bos[i]->external)
		return shim_kind_class(g_bos[i]->kind);
	return 1;
}
static void shim_bo_register(struct shim_bo *b)
{
	int i;
	if(g_nbos >= SHIM_MAXBO) return;
	i = shim_bo_index(b->phys) + 1;          /* insert after the last BO at or below */
	memmove(&g_bos[i + 1], &g_bos[i], (size_t)(g_nbos - i) * sizeof(g_bos[0]));
	g_bos[i] = b;
	g_nbos++;
}
static void shim_bo_unregister(struct shim_bo *b)
{
	int i = shim_bo_index(b->phys);
	while(i >= 0 && g_bos[i]->phys == b->phys && g_bos[i] != b) i--;   /* external twins share a phys */
	if(i < 0 || g_bos[i] != b) return;
	memmove(&g_bos[i], &g_bos[i + 1], (size_t)(g_nbos - i - 1) * sizeof(g_bos[0]));
	g_nbos--;
}
/* 3094/3095: one walk over a kick's method stream, for two purposes.
 * (1) RANGE-DIRECTED FLUSH (3095, verify=0, before the flush): note which
 * bytes of which BOs the engines will READ - 3D vertex arrays
 * (VERTEX_ARRAY_FETCH enable|stride + START, VERTEX_BUFFER_FIRST/COUNT per
 * draw), constant buffers (CB_DEF_ADDRESS/SET), M2MF linear sources - so
 * push_flush_refs dirty-diffs only those ranges instead of every mapped BO
 * in full (the 2 MB vertex uploader was compared whole on every kick, ~1 ms
 * each, 1,160 times a frame in Half-Life). A reference the walk cannot bound
 * (indexed draws, 2D sources) marks the BO 'whole'; a referenced BO the walk
 * never sees is still flushed whole - an unknown pattern costs time, never
 * correctness. (2) SELF-CHECK (3094, verify=1, after the flush): every such
 * source must be a live BO with no unflushed CPU writes in the noted range
 * -> 'KICK WARN' (first 32), stats[5]; the validator fails on it. */
static uint32_t g_kgen = 0;
/* 3096: everything the walk tracks is ENGINE STATE that persists across kicks
 * (the hardware's does - a kick boundary is only where the shim cut the
 * stream), and the verify pass replays a kick from the state the mark pass
 * started in. 3095 kept the M2MF part in locals reset per kick: when the
 * pushbuffer filled between a transfer's PITCH_IN and its LINE_COUNT/NOTIFY
 * (nv50's BEGIN_NV04 may kick between any two packets), the next kick's walk
 * saw pitch 0 and bounded the source to ONE ROW; the rest of the lightmap /
 * texture patch stayed in the shadow and the engine copied stale VRAM - the
 * occasional artifacts of 3095's Half-Life run on the T61. */
static struct shim_scan {
	struct { uint32_t fetch, start, limit; } va[16];   /* 3D vertex arrays */
	int      va_indexed;                               /* VB_ELEMENT seen since the last VERTEX_END_GL */
	uint32_t cb_lo;                                    /* CB_DEF_ADDRESS_LOW */
	uint32_t first;                                    /* VERTEX_BUFFER_FIRST */
	uint32_t m_linear, m_off, m_pitch, m_len, m_lines; /* M2MF LINEAR_IN, OFFSET_IN, PITCH_IN, LINE_LENGTH_IN, LINE_COUNT */
	int      m_pitch_set;                              /* PITCH_IN seen in the kick being walked */
} g_scan = { {{0, 0, 0}}, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, g_scan0;

static void range_mark(struct shim_bo *b, uint32_t off, uint32_t len, int whole)
{
	if(b->kgen != g_kgen) { b->kgen = g_kgen; b->klo = 0xffffffffUL; b->khi = 0; b->kwhole = 0; }
	if(whole) { b->kwhole = 1; return; }
	if(off < b->klo) b->klo = off;
	if(off + len > b->khi) b->khi = off + len;
}
/* what: 0 M2MF source, 1 vertex array, 2 constant buffer, 3 2D source */
static void range_add(uint32_t vm, uint32_t len, int whole, int what, int verify)
{
	struct shim_bo *b; uint32_t phys, off;
	static int nwarn = 0;
	if(vm < NVSHIM_VM_BASE + NVSHIM_HEAP_LO || vm >= NVSHIM_VM_BASE + NVSHIM_HEAP_END)
		return;                              /* scanout surface / instance area: not ours */
	phys = vm - NVSHIM_VM_BASE;
	b = shim_bo_at(phys);
	if(!b)
	{
		if(!verify) return;
		g_st_warn++;
		if(nwarn++ < 32)
		{
			if(what == 1)      NVLOG2("KICK WARN vertex array not in a live BO vm/len", vm, len);
			else if(what == 2) NVLOG2("KICK WARN constbuf not in a live BO vm/len", vm, len);
			else if(what == 3) NVLOG2("KICK WARN 2D src not in a live BO vm/len", vm, len);
			else               NVLOG2("KICK WARN M2MF src not in a live BO vm/len", vm, len);
		}
		return;
	}
	off = phys - b->phys;
	if(!verify) { range_mark(b, off, len, whole); return; }
	if(!b->base.map || !b->clean || b->external) return;
	if(off >= b->shadow_sz) return;
	if(whole) { off = 0; len = b->shadow_sz; }
	else if(len == 0 || off + len > b->shadow_sz) len = b->shadow_sz - off;
	if(memcmp((char*)b->base.map + off, b->clean + off, len) != 0)
	{
		g_st_warn++;
		if(nwarn++ < 32)
		{
			if(what == 1)      NVLOG3("KICK WARN vertex array has unflushed CPU writes vm/len/bo", vm, len, b->phys);
			else if(what == 2) NVLOG3("KICK WARN constbuf has unflushed CPU writes vm/len/bo", vm, len, b->phys);
			else if(what == 3) NVLOG3("KICK WARN 2D src has unflushed CPU writes vm/len/bo", vm, len, b->phys);
			else               NVLOG3("KICK WARN M2MF src has unflushed CPU writes vm/len/bo", vm, len, b->phys);
		}
	}
}
/* a draw: the bytes each enabled vertex array contributes (whole BO when the
 * draw is indexed - the index payload decides what is read) */
static void shim_va_draw(uint32_t first, uint32_t count, int whole, int verify)
{
	int i;
	for(i = 0; i < 16; i++)
	{
		uint32_t stride = g_scan.va[i].fetch & 0xfff, lo, len;
		if(!(g_scan.va[i].fetch & 0x20000000UL) || !g_scan.va[i].start) continue;
		if(whole || stride == 0) { range_add(g_scan.va[i].start, 64, whole, 1, verify); continue; }
		lo = g_scan.va[i].start + first * stride; len = count * stride;
		if(g_scan.va[i].limit >= lo && lo + len > g_scan.va[i].limit + 1) len = g_scan.va[i].limit + 1 - lo;
		range_add(lo, len, 0, 1, verify);
	}
}
static void shim_kick_scan(const uint32_t *m, uint32_t count, int verify)
{
	struct shim_scan *s = &g_scan;
	uint32_t i = 0;
	if(!verify)
	{
		g_kgen++;
		g_scan0 = g_scan;                              /* the verify pass replays from here */
		if(shim_scanreset()) { s->m_linear = 1; s->m_off = s->m_pitch = s->m_len = s->m_lines = 0; }   /* the 3095 walk */
	}
	else g_scan = g_scan0;
	s->m_pitch_set = 0;
	while(i < count)
	{
		uint32_t h = m[i], typ = h >> 29, subc = (h >> 13) & 7, mthd = h & 0x1ffc, n = (h >> 18) & 0x7ff, k;
		if((typ != 0 && typ != 2) || i + 1 + n > count) return;   /* malformed: the puller walk reports it */
		for(k = 0; k < n; k++)
		{
			uint32_t mm = typ == 0 ? mthd + 4 * k : mthd, v = m[i + 1 + k];
			if(subc == 5)                                   /* M2MF (nv50_winsys.h SUBC_M2MF) */
			{
				if(mm == 0x200)      s->m_linear = v;        /* LINEAR_IN */
				else if(mm == 0x30c) s->m_off = v;           /* OFFSET_IN */
				else if(mm == 0x314) { s->m_pitch = v; s->m_pitch_set = 1; }   /* PITCH_IN */
				else if(mm == 0x31c) s->m_len = v;           /* LINE_LENGTH_IN */
				else if(mm == 0x320) s->m_lines = v;         /* LINE_COUNT */
				else if(mm == 0x328 && s->m_linear && s->m_lines && s->m_len)   /* BUFFER_NOTIFY = go */
				{
					int whole = (s->m_lines > 1 && s->m_pitch == 0);     /* pitch unknown: cannot bound it */
					if(!verify && s->m_lines > 1 && !s->m_pitch_set) g_st_m2mf_split++;
					if(!verify && shim_scanreset()) whole = 0;           /* 3095: one row */
					range_add(s->m_off, (s->m_lines - 1) * s->m_pitch + s->m_len, whole, 0, verify);
				}
			}
			else if(subc == 3)                              /* 3D */
			{
				if(mm >= 0x900 && mm < 0xa00)
				{
					uint32_t a = (mm - 0x900) >> 4, r = (mm - 0x900) & 0xf;
					if(r == 0)      s->va[a].fetch = v;       /* VERTEX_ARRAY_FETCH(i): ENABLE | stride */
					else if(r == 8) s->va[a].start = v;       /* VERTEX_ARRAY_START_LOW(i) */
				}
				else if(mm >= 0x1084 && mm < 0x1104 && ((mm - 0x1084) & 7) == 0)
					s->va[(mm - 0x1084) >> 3].limit = v;      /* VERTEX_ARRAY_LIMIT_LOW(i) */
				else if(mm == 0xd74) s->first = v;            /* VERTEX_BUFFER_FIRST */
				else if(mm == 0xd78) shim_va_draw(s->first, v, 0, verify);   /* VERTEX_BUFFER_COUNT: a non-indexed draw */
				else if(mm == 0x15e8 || mm == 0x15f0 || mm == 0x1304) s->va_indexed = 1;   /* VB_ELEMENT_U32/U16/U8 */
				else if(mm == 0x15e0) { if(s->va_indexed) shim_va_draw(0, 0, 1, verify); s->va_indexed = 0; }   /* VERTEX_END_GL */
				else if(mm == 0x1284) s->cb_lo = v;           /* CB_DEF_ADDRESS_LOW */
				else if(mm == 0x1288 && s->cb_lo) range_add(s->cb_lo, (v & 0xffff) ? (v & 0xffff) : 0x10000, 0, 2, verify);   /* CB_DEF_SET: size */
			}
			else if(subc == 4 && mm == 0x254)              /* 2D SRC_ADDRESS_LOW: extent unknown */
				range_add(v, 0, 1, 3, verify);
		}
		i += 1 + n;
	}
}
static void shim_refresh_queries(const uint32_t *m, uint32_t count)
{
	uint32_t i = 0, qaddr = 0;
	while(i < count)
	{
		uint32_t h = m[i], typ = h >> 29, subc = (h >> 13) & 7, mthd = h & 0x1ffc, n = (h >> 18) & 0x7ff, k;
		if(typ != 0 && typ != 2) return;
		if(i + 1 + n > count) return;
		if(subc == 3 && !(mthd == 0 && n == 1))
			for(k = 0; k < n; k++)
			{
				uint32_t mm = typ == 0 ? mthd + 4 * k : mthd, v = m[i + 1 + k];
				if(mm == 0x1b04) qaddr = v;
				else if(mm == 0x1b0c && qaddr >= NVSHIM_VM_BASE)
				{
					uint32_t phys = qaddr - NVSHIM_VM_BASE;
					struct shim_bo *b = shim_bo_at(phys);
					if(b && b->base.map && (phys - b->phys) + 16 <= b->shadow_sz)
					{
						static int nq = 0;
						vxd_vram_read(phys, (uint32_t*)((char*)b->base.map + (phys - b->phys)), 4);
						if(b->clean) memcpy(b->clean + (phys - b->phys), (char*)b->base.map + (phys - b->phys), 16);
						if(nq++ < 24) NVLOG2("fence refresh phys/seq", phys, *(uint32_t*)((char*)b->base.map + (phys - b->phys)));
					}
					else { static int nb = 0; if(nb++ < 8) NVLOG2("fence refresh: QUERY target not a mapped BO phys/bo", phys, b ? b->phys : 0); }
				}
			}
		i += 1 + n;
	}
}

struct shim_bufctx {
	struct nouveau_bufctx base;
	struct nouveau_bufref refs[SHIM_MAXREF];
	uint8_t               bin[SHIM_MAXREF];
	int                   nref;
};

struct shim_push {
	struct nouveau_pushbuf base;
	uint32_t *buf;
	uint32_t  cap;        /* dwords */
	struct nouveau_bo *refn[SHIM_MAXREF];   /* 3094: BOs kref'd for the next kick (a reference each) */
	int       nrefn;
	uint32_t  gen;        /* 3095: bumped when the list is dropped; BOs carry (push, gen) to dedup in O(1) */
};
static void push_unref_all(struct shim_push *p);
static void push_kref_bufctx(struct shim_push *p);

/* ========================= drm / device / client ====================== */
int nouveau_drm_new(int fd, struct nouveau_drm **pdrm)
{
	struct nouveau_drm *drm = calloc(1, sizeof(*drm));
	NVLOG1("drm_new fd", (uint32_t)fd);
	if(!drm) return -1;
	drm->fd = fd;
	drm->version = 0x01000000;   /* pretend DRM 1.0.0 */
	drm->client.oclass = 0;
	*pdrm = drm;
	return 0;
}

void nouveau_drm_del(struct nouveau_drm **pdrm)
{
	if(pdrm && *pdrm) { free(*pdrm); *pdrm = NULL; }
}

int nouveau_device_new(struct nouveau_object *parent, struct nouveau_device **pdev)
{
	struct nouveau_device *dev = calloc(1, sizeof(*dev));
	NVLOG0("device_new");
	if(!dev) return -1;
	{
		/* 3099: name the VxD this log came from. The 3097 T61 round was read as a
		 * 3098 run until nvtail/nvclk showed the 3096 VxD and nvprobe 3097. */
		DWORD hw[64]; DWORD ret = 0;             /* nv_hwinfo_t is 29 dwords; room to grow */
		memset(hw, 0, sizeof(hw));
		if(!shim_fake() && shim_ioctl(OP_NV_PROBE, NULL, 0, hw, sizeof(hw), &ret))
			NVLOG2("vxd build / shim build", hw[NVSHIM_HWINFO_BUILD], (uint32_t)NVSHIM_BUILD);
		else
			NVLOG2("vxd build UNKNOWN (fake backend or probe failed; ret) / shim build", ret, (uint32_t)NVSHIM_BUILD);
	}
	dev->object.parent = parent;
	dev->object.oclass = 0x80000000;   /* NV_DEVICE */
	dev->chipset  = 0x86;              /* G86 (Quadro NVS 140M) */
	/* VRAM the shim manages. The card has more; the VM 1:1 window caps us. */
	dev->vram_size = NVSHIM_HEAP_END;
	dev->gart_size = 0;
	dev->vram_limit = (dev->vram_size * 3) / 4;
	*pdev = dev;
	return 0;
}

void nouveau_device_del(struct nouveau_device **pdev)
{
	if(pdev && *pdev) { free(*pdev); *pdev = NULL; }
}

#ifndef NVSHIM_STANDALONE
/* 25.1.x-only: a debug hook that records the engine class ids on the device.
 * Purely informational; our channel binds classes itself, so this is a no-op
 * beyond noting them (the fields live in dev->info in Mesa's struct). */
void nouveau_device_set_classes_for_debug(struct nouveau_device *dev,
                                          uint32_t cls_eng3d, uint32_t cls_compute,
                                          uint32_t cls_m2mf,  uint32_t cls_copy)
{
	(void)dev; (void)cls_eng3d; (void)cls_compute; (void)cls_m2mf; (void)cls_copy;
}
#endif

int nouveau_device_info(struct nouveau_device *dev, struct nv_device_info_v0 *info)
{
#ifndef NVSHIM_STANDALONE
	/* The G86 is a discrete PCIe Tesla part. nouveau_check_for_uma() only reads
	 * .platform (IGP/SOC => unified memory); PCIE keeps us on the discrete path. */
	memset(info, 0, sizeof(*info));
	info->version  = 0;
	info->platform = NV_DEVICE_INFO_V0_PCIE;
	info->chipset  = (uint16_t)dev->chipset;   /* 0x86 */
	info->revision = 0;
	info->family   = NV_DEVICE_INFO_V0_TESLA;
	info->ram_size = dev->vram_size;
	info->ram_user = dev->vram_limit;
	NVLOG2("device_info chipset/family", info->chipset, info->family);
#else
	(void)dev; (void)info;
#endif
	return 0;
}

/* 3094: real answers per parameter. Up to 3093 EVERY param got the tick-based
 * PTIMER stand-in, so nv50_screen_create read GRAPH_UNITS as a number that
 * depends on the uptime: TPs = popcount(ticks & 0xffff), MPsInTP =
 * popcount(ticks & 0x0f000000) - a different stack/TLS size in every process,
 * and 1 start in 16 (bits 24-27 clear) got MPsInTP = 0: the stack BO is sized
 * 0 and the next statement DIVIDES BY ZERO (vram_size / size_of_one_temp)
 * inside opengl32's first ICD call. That was Half-Life's "OpenGL mode not
 * supported" on 3093: its shim log ends right after a 0x100-byte stack BO.
 * GRAPH_UNITS is PMC 0x1540 (0xF3010001 on the T61's G86: TP mask bit 0, MP
 * mask bits 24-25 = 1 TP x 2 MPs), read through OP_NV_RD32; the fake GPU
 * answers the same value, so simulated and real runs size the same. */
#define NVSHIM_GETPARAM_PCI_VENDOR    3
#define NVSHIM_GETPARAM_PCI_DEVICE    4
#define NVSHIM_GETPARAM_BUS_TYPE      5
#define NVSHIM_GETPARAM_FB_SIZE       8
#define NVSHIM_GETPARAM_AGP_SIZE      9
#define NVSHIM_GETPARAM_CHIPSET_ID    11
#define NVSHIM_GETPARAM_VM_VRAM_BASE  12
#define NVSHIM_GETPARAM_GRAPH_UNITS   13
#define NVSHIM_GETPARAM_PTIMER_TIME   14
#define NVSHIM_GETPARAM_HAS_BO_USAGE  15
#define NVSHIM_GETPARAM_HAS_PAGEFLIP  16
static uint32_t shim_graph_units(void)
{
	static uint32_t units = 0;
	if(!units)
	{
		uint32_t in[2], out[4]; DWORD ret = 0;
		in[0] = 0x1540; in[1] = 1; out[0] = 0;
		if(nvshim_open())
			shim_ioctl(OP_NV_RD32, in, sizeof(in), out, sizeof(out), &ret);
		/* plausibility: at least one TP and one MP, else the known G86 value */
		units = ((out[0] & 0xffffUL) && (out[0] & 0x0f000000UL)) ? out[0] : NVSHIM_G86_UNITS;
		NVLOG2("graph_units reg/used", out[0], units);
	}
	return units;
}

/* 3096: NVSHIM_VSYNC=1 - tear-free present. wgl.c drains the frame's draws,
 * calls this, then kicks the blit into the scanout surface: we wait until head
 * 0's raster is in vertical blank (nouveau nv50_disp scanoutpos: blank when
 * vline >= vblanks or vline <= vblanke), so the 2D engine's top-down copy
 * (~1 ms for 1440x900x32) stays ahead of the beam (16 ms). Polls through
 * OP_NV_RD32; gives up after 40 ms (a wrong register must not freeze every
 * frame) and uses vline >= active lines when the raster regs look implausible. */
static uint32_t shim_rd32(uint32_t off)
{
	uint32_t in[2], out[1]; DWORD ret = 0;
	in[0] = off; in[1] = 1; out[0] = 0xffffffffUL;
	if(!shim_ioctl(OP_NV_RD32, in, sizeof(in), out, sizeof(out), &ret)) return 0xffffffffUL;
	return out[0];
}
void nvshim_vsync_wait(uint32_t active_lines)
{
	static int on = -1;
	static uint32_t blanks = 0, blanke = 0, nlate = 0;
	uint32_t t0, t1, vline, polls = 0;
	if(on < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_VSYNC", b, sizeof(b));
		on = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
		if(on && nvshim_open())
		{
			uint32_t e = shim_rd32(NV50_PDISPLAY_VBLANKE0), st = shim_rd32(NV50_PDISPLAY_VBLANKS0), t = shim_rd32(NV50_PDISPLAY_VTOTAL0);
			blanke = e >> 16; blanks = st >> 16;
			NVLOG3("vsync head0 vblanke/vblanks/vtotal", e, st, t);
			if(blanks == 0 || blanks > 4096 || blanke >= blanks || (t >> 16) < blanks) { blanks = 0; blanke = 0; }
			NVLOG2("vsync on - blank from line (0 = use active lines) / active", blanks, active_lines);
		}
	}
	if(!on) return;
	t0 = GetTickCount();
	{
		/* 3097: also accept the raster counter WRAPPING (a new frame began) - works
		 * whatever the blank-window regs say - and log the first waits' samples */
		static uint32_t nsample = 0;
		uint32_t last = 0xffffffffUL, vmin = 0xffff, vmax = 0;
		for(;;)
		{
			vline = shim_rd32(NV50_PDISPLAY_VLINE0);
			if(vline == 0xffffffffUL) { on = 0; NVLOG0("vsync OFF - raster read failed"); return; }
			vline &= 0xffff;
			if(vline < vmin) vmin = vline;
			if(vline > vmax) vmax = vline;
			if(blanks ? (vline >= blanks || vline <= blanke) : (vline >= active_lines)) break;
			if(last != 0xffffffffUL && vline + 64 < last) break;     /* wrapped: top of a new frame */
			last = vline;
			polls++;
			t1 = GetTickCount();
			if(t1 - t0 > 40) { if(nlate++ < 8) NVLOG2("vsync wait timed out vline/polls", vline, polls); break; }
		}
		if(nsample++ < 6) NVLOG3("vsync wait polls/vline min/max", polls, vmin, vmax);
	}
	t1 = GetTickCount();
	g_st_vsync_n++; g_st_vsync_ms += t1 - t0;
}

int nouveau_getparam(struct nouveau_device *dev, uint64_t param, uint64_t *value)
{
	switch((uint32_t)param)
	{
	case NVSHIM_GETPARAM_PCI_VENDOR:   *value = 0x10deUL;           return 0;
	case NVSHIM_GETPARAM_PCI_DEVICE:   *value = NVSHIM_G86_DEVID;   return 0;
	case NVSHIM_GETPARAM_BUS_TYPE:     *value = 2;                  return 0;   /* NV_PCIE */
	case NVSHIM_GETPARAM_FB_SIZE:      *value = dev->vram_size;     return 0;
	case NVSHIM_GETPARAM_AGP_SIZE:     *value = 0;                  return 0;
	case NVSHIM_GETPARAM_CHIPSET_ID:   *value = dev->chipset;       return 0;
	case NVSHIM_GETPARAM_VM_VRAM_BASE: *value = NVSHIM_VM_BASE;     return 0;
	case NVSHIM_GETPARAM_GRAPH_UNITS:  *value = shim_graph_units(); return 0;
	case NVSHIM_GETPARAM_HAS_BO_USAGE: *value = 1;                  return 0;
	case NVSHIM_GETPARAM_HAS_PAGEFLIP: *value = 0;                  return 0;
	case NVSHIM_GETPARAM_PTIMER_TIME:
		/* a GPU timestamp used for fence staleness: a CPU clock in ns will do */
		*value = (uint64_t)GetTickCount() * 1000000ULL;
		return 0;
	default:
		NVLOG1("getparam unknown param", (uint32_t)param);
		*value = 0;
		return -1;
	}
}

int nouveau_client_new(struct nouveau_device *dev, struct nouveau_client **pclient)
{
	struct nouveau_client *c = calloc(1, sizeof(*c));
	if(!c) return -1;
	c->device = dev;
	*pclient = c;
	NVLOG0("client_new ok");
	return 0;
}

void nouveau_client_del(struct nouveau_client **pclient)
{
	if(pclient && *pclient) { free(*pclient); *pclient = NULL; }
}

/* ========================= objects (channel/engines) ================== */
int nouveau_object_new(struct nouveau_object *parent, uint64_t handle, uint32_t oclass,
                       void *data, uint32_t length, struct nouveau_object **pobj)
{
	struct nouveau_object *obj = calloc(1, sizeof(*obj));
	NVLOG2("object_new handle/oclass", (uint32_t)handle, oclass);
	if(!obj) return -1;
	obj->parent = parent;
	obj->handle = handle;
	obj->oclass = oclass;
	if(length && data)
	{
		obj->data = malloc(length);
		if(obj->data) memcpy(obj->data, data, length);
	}
	/* Bind the object into the persistent channel's RAMHT at Mesa's handle so
	 * method streams resolve it (M5.3c). The channel itself contributes its VRAM
	 * and GART ctxdmas (from nv04_fifo); engine (M2MF/2D/3D) and notifier objects
	 * are bound by class. Unknown/software-only classes are just recorded. */
#ifndef NVSHIM_STANDALONE
	if(oclass == NOUVEAU_FIFO_CHANNEL_CLASS)
	{
		if(data && length >= sizeof(struct nv04_fifo))
		{
			struct nv04_fifo *f = (struct nv04_fifo *)data;
			vxd_bind(f->vram, 0x3d);   /* VRAM ctxdma (VM window) */
			vxd_bind(f->gart, 0x3d);   /* GART ctxdma (no GART -> VM window) */
		}
	}
	else switch(oclass)
	{
	case 0x502d: case 0x5039: case 0x5097:              /* 2D / M2MF / NV50_3D */
	case 0x8297: case 0x8597: case 0x8697:              /* NV84/94/96 3D       */
	case 0x50c0: case 0x85c0:                           /* NV50/NVA3 COMPUTE   */
	/* 3089: the compute class was NOT bound before. nv50_screen_create still
	 * emits SET_OBJECT 0xbeef50c0 on subchannel 6 plus ~100 compute methods
	 * (dword 427 of the 604-dword init kick), so the puller met a handle that
	 * is not in RAMHT - the prime suspect for the silent stall of 3087/3088
	 * (every kick after dword ~410 of the init stream fits in the pusher's
	 * cache, which is exactly why IB get "advanced" to 4 and not 5). */
	case NOUVEAU_NOTIFIER_CLASS:
		vxd_bind((uint32_t)handle, oclass);
		break;
	default:
		break;
	}
#endif
	*pobj = obj;
	return 0;
}

void nouveau_object_del(struct nouveau_object **pobj)
{
	if(pobj && *pobj)
	{
		if((*pobj)->data) free((*pobj)->data);
		free(*pobj);
		*pobj = NULL;
	}
}

int nouveau_object_mclass(struct nouveau_object *obj, const struct nouveau_mclass *mclass)
{
	int i;
	(void)obj;
	/* Return the index of the first class we support. The G86 3D engine is
	 * NV84_3D (0x8297); we also accept the generic NV50 classes. */
	for(i = 0; mclass[i].oclass; i++)
	{
		switch(mclass[i].oclass)
		{
		case 0x8297: case 0x5097: case 0x502d: case 0x5039:
		case 0x8597: case 0x8697:
			return i;
		}
	}
	return -1;
}

/* ========================= buffer objects ============================= */
int nouveau_bo_new(struct nouveau_device *dev, uint32_t flags, uint32_t align,
                   uint64_t size, union nouveau_bo_config *config, struct nouveau_bo **pbo)
{
	struct shim_bo *b;
	uint32_t phys;
	int tiled;
	uint64_t t = shim_tsc();
	if(size == 0) size = 0x100;
	/* 3091: a tiled surface (tile_mode / memtype set) gets whole 64 KiB pages
	 * of its own, because its pages carry a storage kind in the PTEs. */
	tiled = (config && (config->nv50.tile_mode || config->nv50.memtype)) ? 1 : 0;
	if(tiled)
	{
		if(align < 0x10000) align = 0x10000;
		size = ((uint32_t)size + 0xffffUL) & ~0xffffUL;
	}
	phys = heap_alloc((uint32_t)size, align, tiled && shim_kind_wants_hi(config->nv50.memtype & 0x7f),
	                  shim_kind_class(tiled ? (config->nv50.memtype & 0x7f) : 0));
	if(!phys)
	{
		/* 3097: nothing is evicted here, but pending work holds the staging BOs
		 * that Mesa frees once its fences signal: submit it, re-validate the
		 * bufctx the way a pushbuf-full kick does, and try again. Twice, because
		 * Mesa's kick_notify emits the pending fence's QUERY into the NEXT
		 * pushbuffer (fence_next after fence_update), so the work attached to it
		 * runs after the kick after this one. A level load uploads thousands of
		 * textures between two draws; wgltest's texcheck ran the heap dry. */
		static int n = 0;
		int tries;
		uint32_t ft0, lg0; heap_stats(&ft0, &lg0);
		for(tries = 0; !phys && tries < 2; tries++)
		{
			if(!g_cur_push || g_cur_push->cur == (uint32_t*)((struct shim_push_hdr*)g_cur_push)->buf) break;
			g_kick_src = 6; nouveau_pushbuf_kick(g_cur_push); g_kick_src = 0;
			push_kref_bufctx((struct shim_push *)g_cur_push);
			phys = heap_alloc((uint32_t)size, align, tiled && shim_kind_wants_hi(config->nv50.memtype & 0x7f),
			                  shim_kind_class(tiled ? (config->nv50.memtype & 0x7f) : 0));
		}
		if(n++ < 16) NVLOG3("bo_new: heap full - kicked pending work; size/free before/phys after", (uint32_t)size, ft0, phys);
	}
	if(!phys)
	{
#ifndef NVSHIM_STANDALONE
		static int ndump = 0;
		uint32_t ft, lg; heap_stats(&ft, &lg);
		NVLOG3("bo_new FAIL heap full sz/free/largest", (uint32_t)size, ft, lg);
		if(ndump++ < 2)
		{
			/* 3097: who holds the heap - live BOs by class */
			int i; uint32_t n = 0, nb = 0, nk = 0, kb = 0, nm = 0, mb = 0, nr = 0, rb = 0, nbig = 0, bigb = 0;
			for(i = 0; i < g_nbos; i++)
			{
				struct shim_bo *x = g_bos[i];
				n++; nb += x->asize;
				if(x->nkref) { nk++; kb += x->asize; }
				if(x->base.map) { nm++; mb += x->asize; }
				if(x->refcnt > 1) { nr++; rb += x->asize; }
				if(x->asize >= 0x40000) { nbig++; bigb += x->asize; }
			}
			NVLOG2("heap full: live BOs n/bytes", n, nb);
			NVLOG2("heap full: kref'd (pending kick) n/bytes", nk, kb);
			NVLOG2("heap full: CPU-mapped n/bytes", nm, mb);
			NVLOG2("heap full: refcnt>1 n/bytes", nr, rb);
			NVLOG2("heap full: >=256 KiB n/bytes", nbig, bigb);
			{	/* every distinct (size, kind): count and bytes, 24 slots */
				uint32_t sz[24], cnt[24], kd[24]; int j, ns = 0;
				for(i = 0; i < g_nbos; i++)
				{
					struct shim_bo *x = g_bos[i];
					for(j = 0; j < ns; j++) if(sz[j] == x->asize && kd[j] == x->kind) break;
					if(j == ns && ns < 24) { sz[ns] = x->asize; kd[ns] = x->kind; cnt[ns] = 0; ns++; }
					if(j < ns) cnt[j]++;
				}
				for(j = 0; j < ns; j++) NVLOG3("heap full: size/count/kind", sz[j], cnt[j], kd[j]);
			}
		}
#endif
		return -1;
	}
#ifndef NVSHIM_STANDALONE
	if((g_heap_allocs & 255) == 0)
	{
		uint32_t ft, lg; heap_stats(&ft, &lg);
		NVLOG3("heap allocs/used/largest", g_heap_allocs, g_heap_used, lg);
		NVLOG2("heap frees/kicks", g_heap_frees, g_st_kicks);
	}
#endif
	b = calloc(1, sizeof(*b));
	if(!b) return -1;
	b->refcnt       = 1;
	b->phys         = phys;
	b->base.device  = dev;
	b->base.handle  = phys;                           /* opaque; we key on phys */
	b->base.size    = size;
	b->base.flags   = flags;
	b->base.offset  = (uint64_t)phys + NVSHIM_VM_BASE; /* GPU (VM) address */
	b->base.map     = NULL;
	if(config) b->base.config = *config;
	b->asize = ((uint32_t)size + 0xffUL) & ~0xffUL;
	b->kind  = tiled ? (config->nv50.memtype & 0x7f) : 0;
	{	/* 3095: the first 512 allocations, then one in 256 (a game allocates a
		 * staging BO per texture update: ~34 a frame) */
		static uint32_t nb = 0;
		if(nb < 512 || (nb & 255) == 0) NVLOG3("bo_new sz/phys/vmoff", (uint32_t)size, phys, (uint32_t)b->base.offset);
		nb++;
	}
	if(b->kind)
		vxd_vmkind(phys, b->asize, b->kind);
	shim_bo_register(b);
	*pbo = &b->base;
	g_st_kc[4] += shim_tsc() - t;
	return 0;
}

/* 3095: kick the pushbuf that holds a kref on this BO (libdrm: cli_push_get) */
static void shim_kick_bo(struct shim_bo *b, int src)
{
	struct shim_push *p = b->kref_push;
	if(!p) return;
	g_kick_src = src;
	nouveau_pushbuf_kick(&p->base);
	g_kick_src = 0;
}

int nouveau_bo_map(struct nouveau_bo *bo, uint32_t access, struct nouveau_client *client)
{
	struct shim_bo *b = (struct shim_bo *)bo;
	uint64_t t = shim_tsc();
	(void)client;
	/* 3095: libdrm semantics (nouveau_bo_map -> nouveau_bo_wait): submit only
	 * when THIS BO is referenced by a pending pushbuf and the CPU wants it
	 * (access != 0) - e.g. a readback staging BO whose M2MF copy is queued.
	 * 3083-3094 kicked on EVERY map; in Half-Life that was ~1,160 synchronous
	 * kicks per frame (a lightmap update or draw buffer map each) = 13 s/frame. */
	if((access & (NOUVEAU_BO_RD | NOUVEAU_BO_WR)) && b->nkref)
		shim_kick_bo(b, 3);
	if(!b->base.map)
	{
		b->shadow_sz = (uint32_t)b->base.size;
		b->base.map  = calloc(1, b->shadow_sz + 4);
		b->clean     = calloc(1, b->shadow_sz + 4);
		if(!b->base.map || !b->clean) { free(b->base.map); free(b->clean); b->base.map = NULL; b->clean = NULL; b->shadow_sz = 0; return -1; }
		/* 3092: seed the shadow from VRAM only when the CPU will READ it; a
		 * write-only map (uploads: vertex/index/constant data, the 2 MB vertex
		 * buffer that cost 32768 ioctls per run) starts from zeros, and the
		 * dirty-diff flush writes back only what the CPU touched. */
		if(access & NOUVEAU_BO_RD)
		{
			vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
			memcpy(b->clean, b->base.map, b->shadow_sz);
		}
		else
		{
			/* 3105: a write-only map starts from a shadow of zeros, but VRAM is
			 * NOT zero - heap_free never clears, so a new BO inherits the bytes
			 * of whatever lived in that range before. 3092-3104 also set `clean`
			 * to zeros, which told the dirty-diff flush "VRAM already holds
			 * zeros": a 1 KiB chunk the CPU filled entirely with zeros (black
			 * texture rows, lightmap patches, unused constant slots) compared
			 * equal and was never uploaded, and the engines read the previous
			 * BO's data there (tools/repro_nvshim_zero_upload.py; FULLFLUSH did
			 * not catch it either, same comparison). `clean` now starts as a
			 * sentinel pattern no CPU write produces, i.e. "VRAM contents
			 * unknown": the first flush of every chunk in a kick's window is
			 * unconditional (zeros included), later flushes diff as before. A
			 * read map or a refresh still seeds `clean` from real VRAM. */
			uint32_t *c = (uint32_t*)b->clean, n = (b->shadow_sz + 3) / 4, i;
			for(i = 0; i < n; i++) c[i] = 0xA5C3F00Fu ^ (0x9E3779B9u * (i + 1));
		}
	}
	else if(access & NOUVEAU_BO_RD)
	{
		/* Refresh with whatever the GPU has written since the last map. */
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		memcpy(b->clean, b->base.map, b->shadow_sz);
	}
	g_st_kc[4] += shim_tsc() - t;
	return 0;
}

void nouveau_bo_ref(struct nouveau_bo *bo, struct nouveau_bo **pref)
{
	struct shim_bo *old = (struct shim_bo *)(pref ? *pref : NULL);
	if(bo)
		((struct shim_bo *)bo)->refcnt++;
	if(old && --old->refcnt == 0)
	{
		shim_bo_unregister(old);
		if(old->base.map) free(old->base.map);
		if(old->clean) free(old->clean);
		if(!old->external)
		{
			if(old->kind) vxd_vmkind(old->phys, old->asize, 0);   /* back to linear */
			heap_free(old->phys, old->asize);
		}
		free(old);
	}
	if(pref)
		*pref = bo;
}

int nouveau_bo_wait(struct nouveau_bo *bo, uint32_t access, struct nouveau_client *client)
{
	struct shim_bo *b = (struct shim_bo *)bo;
	(void)client;
	/* 3095: like libdrm, submit only if a pending pushbuf references this BO;
	 * submits being synchronous, the GPU is then done with it. For a read
	 * wait, pull the latest VRAM contents into the shadow. */
	if((access & (NOUVEAU_BO_RD | NOUVEAU_BO_WR)) && b->nkref)
		shim_kick_bo(b, 4);
	if((access & NOUVEAU_BO_RD) && b->base.map)
	{
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		if(b->clean) memcpy(b->clean, b->base.map, b->shadow_sz);
	}
	return 0;
}

/* 3092: the scanout front buffer as an importable BO. wgl.c registers its
 * physical VRAM range (FBHDA surface offset, pitch * height) and then imports
 * it through resource_from_handle with name NVSHIM_FRONT_NAME; present is a
 * GPU blit into it instead of a CPU readback + copy (1.1 MB through the
 * uncached BAR per frame). The VM maps VRAM 1:1, so its address is known. */
#define NVSHIM_FRONT_NAME 1
static uint32_t g_front_phys = 0, g_front_size = 0;
void nvshim_set_front(uint32_t phys, uint32_t size)
{
	g_front_phys = phys; g_front_size = size;
	NVLOG2("front phys/size", phys, size);
}
int nouveau_bo_name_get(struct nouveau_bo *bo, uint32_t *name) { (void)bo; (void)name; return -1; }
int nouveau_bo_name_ref(struct nouveau_device *dev, uint32_t name, struct nouveau_bo **pbo)
{
	struct shim_bo *b;
	if(name != NVSHIM_FRONT_NAME || !g_front_size)
		return -1;
	b = calloc(1, sizeof(*b));
	if(!b) return -1;
	b->refcnt      = 1;
	b->phys        = g_front_phys;
	b->asize       = g_front_size;
	b->external    = 1;
	b->base.device = dev;
	b->base.handle = g_front_phys;
	b->base.size   = g_front_size;
	b->base.flags  = NOUVEAU_BO_VRAM;
	b->base.offset = (uint64_t)g_front_phys + NVSHIM_VM_BASE;
	NVLOG2("bo_front phys/vmoff", b->phys, (uint32_t)b->base.offset);
	*pbo = &b->base;
	return 0;
}
int nouveau_bo_prime_handle_ref(struct nouveau_device *dev, int fd, struct nouveau_bo **pbo) { (void)dev; (void)fd; (void)pbo; return -1; }
int nouveau_bo_set_prime(struct nouveau_bo *bo, int *fd) { (void)bo; (void)fd; return -1; }
int nouveau_bo_wrap(struct nouveau_device *dev, uint32_t handle, struct nouveau_bo **pbo) { (void)dev; (void)handle; (void)pbo; return -1; }

/* ========================= bufctx ===================================== */
int nouveau_bufctx_new(struct nouveau_client *client, int bins, struct nouveau_bufctx **pbc)
{
	struct shim_bufctx *bc = calloc(1, sizeof(*bc));
	(void)bins;
	if(!bc) return -1;
	bc->base.client = client;
	bc->base.head.prev = bc->base.head.next = &bc->base.head;
	bc->base.pending.prev = bc->base.pending.next = &bc->base.pending;
	bc->base.current.prev = bc->base.current.next = &bc->base.current;
	bc->nref = 0;
	*pbc = &bc->base;
	NVLOG1("bufctx_new ok bins", bins);
	return 0;
}

void nouveau_bufctx_del(struct nouveau_bufctx **pbc)
{
	if(pbc && *pbc) { free((struct shim_bufctx *)*pbc); *pbc = NULL; }
}

static struct nouveau_bufref *bufctx_add(struct shim_bufctx *bc, int bin, struct nouveau_bo *bo,
                                         uint32_t flags)
{
	struct nouveau_bufref *r;
	uint64_t t = shim_tsc();
	if(bc->nref >= SHIM_MAXREF)
		return NULL;
	r = &bc->refs[bc->nref];
	memset(r, 0, sizeof(*r));
	r->bo    = bo;
	r->flags = flags;
	bc->bin[bc->nref] = (uint8_t)bin;
	bc->nref++;
	g_st_kc[1] += shim_tsc() - t;
	return r;
}

struct nouveau_bufref *
nouveau_bufctx_refn(struct nouveau_bufctx *bc, int bin, struct nouveau_bo *bo, uint32_t flags)
{
	return bufctx_add((struct shim_bufctx *)bc, bin, bo, flags);
}

struct nouveau_bufref *
nouveau_bufctx_mthd(struct nouveau_bufctx *bc, int bin, uint32_t packet, struct nouveau_bo *bo,
                    uint64_t data, uint32_t flags, uint32_t vor, uint32_t tor)
{
	struct nouveau_bufref *r = bufctx_add((struct shim_bufctx *)bc, bin, bo, flags);
	if(r)
	{
		r->packet = packet;
		r->data   = (uint32_t)data;   /* fixed VM address: no relocation needed */
		r->vor    = vor;
		r->tor    = tor;
	}
	return r;
}

void nouveau_bufctx_reset(struct nouveau_bufctx *bc, int bin)
{
	struct shim_bufctx *b = (struct shim_bufctx *)bc;
	int i, j = 0;
	uint64_t t = shim_tsc();
	for(i = 0; i < b->nref; i++)
	{
		if(b->bin[i] == (uint8_t)bin)
			continue;                 /* drop this bin's refs */
		if(j != i)
		{
			b->refs[j] = b->refs[i];
			b->bin[j]  = b->bin[i];
		}
		j++;
	}
	b->nref = j;
	g_st_kc[1] += shim_tsc() - t;
}

/* ========================= pushbuf ==================================== */
int nouveau_pushbuf_new(struct nouveau_client *client, struct nouveau_object *chan, int nr,
                        uint32_t size, struct nouveau_pushbuf **ppush)
{
	struct shim_push *p = calloc(1, sizeof(*p));
	(void)nr;
	if(!p) return -1;
	/* `size` is the per-push dword budget; give ourselves generous headroom
	 * so PUSH_SPACE rarely forces an intermediate kick. */
	p->cap = size ? size : 0x4000;
	if(p->cap < 0x4000) p->cap = 0x4000;
	/* 3095: one submit must fit the VxD's 1 MiB command region (NVGPU_PUSHBUF_SIZE);
	 * Mesa asks for 512 KiB which the shim had read as dwords (2 MiB) */
	if(p->cap > 0x3C000) p->cap = 0x3C000;
	if(shim_pushcap() && p->cap > shim_pushcap()) p->cap = shim_pushcap();   /* 3096: simulation knob */
	p->buf = malloc(p->cap * 4);
	if(!p->buf) { free(p); return -1; }
	p->base.client  = client;
	p->base.channel = chan;
	p->base.cur     = p->buf;
	p->base.end     = p->buf + p->cap;
	g_cur_push      = &p->base;
	p->nrefn = 0;
	NVLOG1("pushbuf_new cap", p->cap);
	*ppush = &p->base;
	return 0;
}

void nouveau_pushbuf_del(struct nouveau_pushbuf **ppush)
{
	if(ppush && *ppush)
	{
		struct shim_push *p = (struct shim_push *)*ppush;
		push_unref_all(p);
		if(p->buf) free(p->buf);
		free(p);
		*ppush = NULL;
	}
}

struct nouveau_bufctx *
nouveau_pushbuf_bufctx(struct nouveau_pushbuf *push, struct nouveau_bufctx *bc)
{
	struct nouveau_bufctx *prev = push->bufctx;
	push->bufctx = bc;
	return prev;
}

/* Flush every referenced BO that has a CPU shadow into VRAM (bounce), so the
 * GPU sees the latest CPU writes before it runs. */
/* 3092: flush only what the CPU changed since the last flush: compare the
 * shadow with its clean copy in 1 KiB chunks and write the changed runs.
 * Before this every referenced mapped BO was written whole before EVERY kick
 * (the 2 MB vertex buffer, ...), through the uncached BAR, several times per
 * frame. Chunks the CPU never wrote are never flushed, so GPU-written data in
 * a mapped BO is no longer clobbered by a stale shadow either. */
#define SHIM_FLUSH_CHUNK 1024
static void shim_bo_flush(struct shim_bo *b)
{
	uint32_t off, run_start = 0, run_len = 0, lo, hi;
	if(!b->base.map || !b->shadow_sz || b->external) return;
	if(!b->clean)
	{
		vxd_vram_write(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		g_st_flush_dw += (b->shadow_sz + 3) / 4;
		return;
	}
	/* 3095: when this kick's stream walk bounded what the engines read from
	 * this BO, diff only that (chunk-aligned) window; otherwise the whole BO */
	lo = 0; hi = b->shadow_sz;
	if(!shim_fullflush() && b->kgen == g_kgen && !b->kwhole && b->klo < b->khi && b->klo < b->shadow_sz)
	{
		lo = b->klo & ~(uint32_t)(SHIM_FLUSH_CHUNK - 1);
		hi = (b->khi + SHIM_FLUSH_CHUNK - 1) & ~(uint32_t)(SHIM_FLUSH_CHUNK - 1);
		if(hi > b->shadow_sz) hi = b->shadow_sz;
	}
	for(off = lo; off < hi; off += SHIM_FLUSH_CHUNK)
	{
		uint32_t len = b->shadow_sz - off; if(len > SHIM_FLUSH_CHUNK) len = SHIM_FLUSH_CHUNK;
		if(memcmp((char*)b->base.map + off, b->clean + off, len) != 0)
		{
			memcpy(b->clean + off, (char*)b->base.map + off, len);
			if(run_len && run_start + run_len == off) run_len += len;
			else
			{
				if(run_len) { vxd_vram_write(b->phys + run_start, (uint32_t*)((char*)b->base.map + run_start), (run_len + 3) / 4); g_st_flush_dw += (run_len + 3) / 4; }
				run_start = off; run_len = len;
			}
		}
	}
	if(run_len) { vxd_vram_write(b->phys + run_start, (uint32_t*)((char*)b->base.map + run_start), (run_len + 3) / 4); g_st_flush_dw += (run_len + 3) / 4; }
}
/* 3097: NVSHIM_VERIFYVRAM - after a synchronous kick, read every range the walk
 * marked back from VRAM and compare with the CPU shadow (the flush wrote it there
 * just before the kick). A mismatch means the flush did not land or something
 * else wrote that VRAM in between: 'VRAM WARN' with BO/offset/values. Bounded
 * to 1 MiB per BO; GPU-written mapped BOs are not marked by the walk. */
static void shim_verify_bo(struct shim_bo *b)
{
	static uint32_t *tmp = NULL;
	static uint32_t nwarn = 0;
	uint32_t lo, hi, off, n;
	if(!b->base.map || !b->shadow_sz || b->external || b->kgen != g_kgen) return;
	lo = 0; hi = b->shadow_sz;
	if(!b->kwhole && b->klo < b->khi && b->klo < b->shadow_sz)
	{
		lo = b->klo & ~3u; hi = (b->khi + 3) & ~3u;
		if(hi > b->shadow_sz) hi = b->shadow_sz;
	}
	if(hi - lo > 0x100000) hi = lo + 0x100000;
	if(!tmp) tmp = (uint32_t *)malloc(0x100000);
	if(!tmp || hi <= lo + 3) return;
	n = (hi - lo) / 4;
	vxd_vram_read(b->phys + lo, tmp, n);
	g_st_vchk++;
	for(off = 0; off < n; off++)
	{
		uint32_t want = ((const uint32_t *)((const char *)b->base.map + lo))[off];
		if(tmp[off] != want)
		{
			uint32_t last = off;
			while(last + 1 < n && tmp[last + 1] != ((const uint32_t *)((const char *)b->base.map + lo))[last + 1]) last++;
			g_st_vbad++;
			if(nwarn++ < 32)
			{
				NVLOG3("VRAM WARN bo phys/size/kind-whole", b->phys, b->shadow_sz, (uint32_t)b->kwhole);
				NVLOG3("VRAM WARN first bad off/run bytes/checked", lo + off * 4, (last - off + 1) * 4, hi - lo);
				NVLOG2("VRAM WARN vram/shadow", tmp[off], want);
			}
			break;
		}
	}
}
static void push_verify_refs(struct shim_push *p)
{
	int i;
	if(p->base.bufctx)
	{
		struct shim_bufctx *bc = (struct shim_bufctx *)p->base.bufctx;
		for(i = 0; i < bc->nref; i++)
			if(bc->refs[i].bo) shim_verify_bo((struct shim_bo *)bc->refs[i].bo);
	}
	for(i = 0; i < p->nrefn; i++)
		if(p->refn[i]) shim_verify_bo((struct shim_bo *)p->refn[i]);
}
static void push_flush_refs(struct shim_push *p)
{
	int i;
	if(p->base.bufctx)
	{
		struct shim_bufctx *bc = (struct shim_bufctx *)p->base.bufctx;
		for(i = 0; i < bc->nref; i++)
			if(bc->refs[i].bo) shim_bo_flush((struct shim_bo *)bc->refs[i].bo);
	}
	for(i = 0; i < p->nrefn; i++)
		if(p->refn[i]) shim_bo_flush((struct shim_bo *)p->refn[i]);
}

/* 3094: libdrm's kref semantics. nouveau_pushbuf_validate (Mesa's PUSH_VAL)
 * copies every BO of the pushbuf's CURRENT bufctx into the pushbuf's own list
 * and HOLDS a reference until the kick; the kick flushes that list (plus the
 * bufctx current at kick time) and drops the references after the submit. So
 * a BO Mesa lets go of before the kick - nouveau_bufctx_reset() of a
 * temporary bin, nouveau_bo_ref(NULL) - stays alive and gets its CPU writes
 * flushed. 3093 on the T61 drew the 'texture' cube BLACK because of exactly
 * that: nv50_m2mf_transfer_rect refs the texture staging BO in bufctx bin 0,
 * PUSH_VALs, emits the M2MF copy and resets bin 0 before the kick, so the
 * kick saw neither the ref nor the staging data (never flushed) and the
 * engine copied untouched VRAM into every texture. The fake GPU could not
 * see it (nothing renders); shim_kick_check now does. NVSHIM_NOKREF=1
 * restores the 3093 behaviour, to demonstrate that detector. */
static int shim_nokref(void)
{
	static int v = -1;
	if(v < 0)
	{
		char b[8]; DWORD n = GetEnvironmentVariableA("NVSHIM_NOKREF", b, sizeof(b));
		v = (n > 0 && n < sizeof(b) && b[0] != '0') ? 1 : 0;
	}
	return v;
}
static void push_unref_all(struct shim_push *p)
{
	int i;
	if(shim_nokref()) { p->nrefn = 0; p->gen++; return; }      /* 3093 mode: recorded without a reference */
	for(i = 0; i < p->nrefn; i++)
	{
		struct nouveau_bo *bo = p->refn[i];
		struct shim_bo *b = (struct shim_bo *)bo;
		p->refn[i] = NULL;
		if(b->nkref > 0 && --b->nkref == 0) b->kref_push = NULL;
		nouveau_bo_ref(NULL, &bo);                    /* frees it here if Mesa already let go */
	}
	p->nrefn = 0;
	p->gen++;
}

int nouveau_pushbuf_kick(struct nouveau_pushbuf *push)
{
	struct shim_push *p = (struct shim_push *)push;
	uint32_t count = (uint32_t)(push->cur - p->buf);
	int ret = 0;
	uint64_t t = shim_tsc(), tf;

	g_st_kick_src[g_kick_src & 7]++;
	if(count)
		shim_kick_scan(p->buf, count, 0);     /* 3095: bound what the engines read from each BO */
	tf = shim_tsc();
	push_flush_refs(p);
	g_st_kc[3] += shim_tsc() - tf;
	if(count)
		ret = vxd_submit(p->buf, count);
	if(count && ret == 0 && shim_verifyvram())
		push_verify_refs(p);                         /* 3097: what the engines just read, back from VRAM */

	/* reset the command buffer for the next batch */
	push->cur = p->buf;
	if(shim_pushcap())
	{
		/* 3096: jitter the capped size per kick so the cut points drift through
		 * the stream (a fixed-length test frame cuts at the same packets forever) */
		static uint32_t x = 12345;
		x = x * 1103515245u + 12345u;
		p->cap = shim_pushcap() - ((x >> 16) & 0xff);
	}
	push->end = p->buf + p->cap;
	push_unref_all(p);                               /* submits are synchronous: the GPU is done with them */

	if(push->kick_notify)
		push->kick_notify(push);
	g_st_kc[2] += shim_tsc() - t;
	return ret;
}

/* Record bo in the pushbuf's kref list (once) and hold a reference on it. A
 * full list submits what is queued first, as libdrm does when its buffer
 * list overflows (PUSH_VAL sits at a packet boundary, so that is safe). */
static void push_kref(struct shim_push *p, struct nouveau_bo *bo)
{
	struct shim_bo *b = (struct shim_bo *)bo;
	if(!bo) return;
	if(b->kref_push == p && b->kref_gen == p->gen) return;   /* 3095: already in this list (O(1), was a scan) */
	if(p->nrefn >= SHIM_MAXREF)
	{
		static int nfull = 0;
		if(nfull++ < 8) NVLOG1("kref list full - kicking early, cap", (uint32_t)SHIM_MAXREF);
		g_kick_src = 2; nouveau_pushbuf_kick(&p->base); g_kick_src = 0;
	}
	if(!shim_nokref())
	{
		b->refcnt++;
		b->nkref++;
		b->kref_push = p;
		b->kref_gen  = p->gen;
	}
	p->refn[p->nrefn++] = bo;
}
static void push_kref_bufctx(struct shim_push *p)
{
	struct shim_bufctx *bc = (struct shim_bufctx *)p->base.bufctx;
	int i;
	if(!bc || shim_nokref()) return;
	for(i = 0; i < bc->nref; i++)
		push_kref(p, bc->refs[i].bo);
}

int nouveau_pushbuf_space(struct nouveau_pushbuf *push, uint32_t dwords, uint32_t relocs,
                          uint32_t pushes)
{
	struct shim_push *p = (struct shim_push *)push;
	uint32_t used = (uint32_t)(push->cur - p->buf);
	(void)relocs; (void)pushes;
	if(used + dwords > p->cap)
	{
		int ret;
		g_kick_src = 1; ret = nouveau_pushbuf_kick(push); g_kick_src = 0;   /* flush what we have, start fresh */
		if(ret) return ret;
		push_kref_bufctx(p);                   /* 3094: like libdrm, re-validate the bufctx after a forced flush */
		used = 0;
	}
	if(dwords > p->cap)
		return -1;                             /* single batch too large */
	return 0;
}

void nouveau_pushbuf_data(struct nouveau_pushbuf *push, struct nouveau_bo *bo, uint64_t offset,
                          uint64_t length)
{
	struct shim_push *p = (struct shim_push *)push;
	struct shim_bo *b = (struct shim_bo *)bo;
	uint32_t dwords = (uint32_t)(length / 4);
	if(!bo || dwords == 0)
		return;
	/* The payload of a VB_ELEMENT_* header Mesa just wrote (nv50_vbo.c:
	 * BEGIN_NL50 + nouveau_pushbuf_data = index data read from a BO). libdrm
	 * hands the BO to the kernel as its own IB entry; 3095 COPIES the bytes
	 * inline right after the header - the puller sees the same stream - where
	 * 3083-3094 made two submits per indexed draw (the methods so far, then the
	 * data), each a synchronous GPU drain. */
	if(b->base.map && push->cur > p->buf && dwords + 1 < p->cap)
	{
		if((uint32_t)(push->cur - p->buf) + dwords > p->cap)
		{
			/* no room: the header must stay adjacent to its data - take it off,
			 * submit everything before it, re-emit it in the fresh buffer */
			uint32_t hdr = push->cur[-1];
			push->cur--;
			g_kick_src = 1; nouveau_pushbuf_kick(push); g_kick_src = 0;
			push_kref_bufctx(p);
			*push->cur++ = hdr;
		}
		memcpy(push->cur, (char*)b->base.map + offset, (size_t)dwords * 4);
		push->cur += dwords;
		return;
	}
	/* fallback (BO without a CPU shadow): the two-submit path */
	if(push->cur != p->buf) { g_kick_src = 5; nouveau_pushbuf_kick(push); g_kick_src = 0; }
	if(b->base.map)
	{
		g_submit_data = 1;                     /* 3094: index payload, not methods (dump tag, no kick check) */
		(void)vxd_submit((const uint32_t*)((char*)b->base.map + offset), dwords);
		g_submit_data = 0;
	}
}

int nouveau_pushbuf_refn(struct nouveau_pushbuf *push, struct nouveau_pushbuf_refn *refs, int nr)
{
	struct shim_push *p = (struct shim_push *)push;
	int i;
	for(i = 0; i < nr; i++)
		push_kref(p, refs[i].bo);              /* 3094: held until the kick, like libdrm */
	return 0;
}

void nouveau_pushbuf_reloc(struct nouveau_pushbuf *push, struct nouveau_bo *bo, uint32_t data,
                           uint32_t flags, uint32_t vor, uint32_t tor)
{
	/* Addresses are fixed (the BO's VM offset), so nothing to patch. */
	(void)push; (void)bo; (void)data; (void)flags; (void)vor; (void)tor;
}

int nouveau_pushbuf_validate(struct nouveau_pushbuf *push)
{
	/* Every BO is permanently resident in the VM window, so residency never
	 * fails; what validate does here (3094) is libdrm's other job: kref the
	 * current bufctx's BOs into the pushbuf until the kick (see push_kref). */
	{
		uint64_t t = shim_tsc();
		push_kref_bufctx((struct shim_push *)push);
		g_st_kc[0] += shim_tsc() - t;
	}
	return 0;
}
