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
		FlushFileBuffers(g_logf);
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

#define FAKE_PAGE_SHIFT 20                         /* 1 MiB pages, allocated on first touch */
#define FAKE_PAGES ((NVSHIM_HEAP_END - NVSHIM_HEAP_BASE) >> FAKE_PAGE_SHIFT)
static uint8_t *g_fake_page[FAKE_PAGES];
static uint32_t g_fake_handles[64];
static int      g_fake_nhandles = 0;
static uint32_t g_fake_sub[8];                    /* handle bound per subchannel (channel state) */
static uint32_t g_fake_put = 0;                   /* running command-region offset (put_off) */
static uint32_t g_fake_ib  = 0;                   /* kicks accepted (IB get/put index) */

static uint8_t *fake_ptr(uint32_t off, int alloc)
{
	uint32_t pg;
	if(off < NVSHIM_HEAP_BASE || off >= NVSHIM_HEAP_END) return NULL;
	pg = (off - NVSHIM_HEAP_BASE) >> FAKE_PAGE_SHIFT;
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
					if(qaddr < NVSHIM_VM_BASE + NVSHIM_HEAP_BASE || qaddr >= NVSHIM_VM_BASE + NVSHIM_HEAP_END)
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
		for(i = 0; i < n; i++) o[i] = (w[0] + i * 4 == 0x1540UL) ? NVSHIM_G86_UNITS : 0;
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
void nvshim_stats(uint32_t *v)
{
	v[0] = g_st_kicks; v[1] = g_st_flush_dw; v[2] = g_st_read_dw; v[3] = g_st_traps; v[4] = g_st_kick_dw;
	v[5] = g_st_warn;
}
static void shim_refresh_queries(const uint32_t *m, uint32_t count);
static void shim_kick_check(const uint32_t *m, uint32_t count);
static int vxd_submit(const uint32_t *methods, uint32_t count)
{
	nv_gpu_submit_t sb;
	DWORD ret = 0;
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
	/* 3094: every DMA source this stream names must be a live, flushed BO */
	if(!g_submit_data)
		shim_kick_check(methods, count);
	memset(&sb, 0, sizeof(sb));
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
#ifndef NVSHIM_STANDALONE
	if(nkick++ < 400) NVLOG3("kick ok cnt/put/pgi", count, sb.put_off, sb.pgraph_intr);
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
	NVLOG3("bo_kind phys/size/kind", phys, size, kind | (r ? 0 : 0x80000000UL));
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

static void heap_init(void)
{
	g_free[0].start = NVSHIM_HEAP_BASE;
	g_free[0].size  = NVSHIM_HEAP_END - NVSHIM_HEAP_BASE;
	g_nfree = 1;
}
static void heap_stats(uint32_t *free_total, uint32_t *largest)
{
	int i; uint32_t t = 0, l = 0;
	if(g_nfree < 0) heap_init();
	for(i = 0; i < g_nfree; i++) { t += g_free[i].size; if(g_free[i].size > l) l = g_free[i].size; }
	*free_total = t; *largest = l;
}
static uint32_t heap_alloc(uint32_t size, uint32_t align)
{
	int i;
	if(g_nfree < 0) heap_init();
	if(align < 0x100) align = 0x100;
	size = (size + 0xff) & ~0xffUL;
	for(i = 0; i < g_nfree; i++)
	{
		uint32_t bs = g_free[i].start, be = g_free[i].start + g_free[i].size;
		uint32_t a = (bs + (align - 1)) & ~(align - 1);
		if(a < bs || a + size > be || a + size < a)
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
static void heap_free(uint32_t start, uint32_t size)
{
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
#define SHIM_MAXREF 512

struct shim_bo {
	struct nouveau_bo base;
	int      refcnt;
	uint32_t phys;        /* VRAM byte offset */
	uint32_t shadow_sz;   /* bytes backing base.map, 0 until first map */
	uint32_t asize;       /* bytes actually allocated (tiled: rounded to 64 KiB) */
	uint32_t kind;        /* 3091: PTE storage kind set for this BO (0 = linear) */
	uint8_t *clean;       /* 3092: copy of the shadow as last flushed/refreshed (dirty diffing) */
	int      external;    /* 3092: wraps memory that is not ours (the scanout front buffer) */
};
#define SHIM_MAXBO 4096
static struct shim_bo *g_bos[SHIM_MAXBO];   /* live BOs, for address -> BO lookups */
static int             g_nbos = 0;
static struct shim_bo *shim_bo_at(uint32_t phys)
{
	int i;
	for(i = 0; i < g_nbos; i++)
		if(phys >= g_bos[i]->phys && phys < g_bos[i]->phys + (uint32_t)g_bos[i]->base.size)
			return g_bos[i];
	return NULL;
}
/* 3094: self-check of a kick against the BO registry before it is submitted
 * (real GPU and fake GPU alike). Every DMA address the stream names must sit
 * in a live BO whose CPU shadow has no unflushed writes, or the engine reads
 * stale VRAM - which is what 3093's black textures were. Checked: M2MF linear
 * sources (OFFSET_IN, the staged rectangle) and 3D vertex-array starts (the
 * whole BO). Logged as 'KICK WARN' (first 32), counted in stats[5];
 * tools/nvkick_decode.py --check fails a run whose log carries one. */
static void shim_check_range(int what, uint32_t vm, uint32_t len)
{
	struct shim_bo *b; uint32_t phys, off;
	static int nwarn = 0;
	if(vm < NVSHIM_VM_BASE + NVSHIM_HEAP_BASE || vm >= NVSHIM_VM_BASE + NVSHIM_HEAP_END)
		return;                              /* scanout surface / instance area: not ours to check */
	phys = vm - NVSHIM_VM_BASE;
	b = shim_bo_at(phys);
	if(!b)
	{
		g_st_warn++;
		if(nwarn++ < 32)
		{
			if(what) NVLOG2("KICK WARN vertex array not in a live BO vm/len", vm, len);
			else     NVLOG2("KICK WARN M2MF src not in a live BO vm/len", vm, len);
		}
		return;
	}
	if(!b->base.map || !b->clean || b->external) return;
	off = phys - b->phys;
	if(off >= b->shadow_sz) return;
	if(len == 0 || off + len > b->shadow_sz) len = b->shadow_sz - off;
	if(memcmp((char*)b->base.map + off, b->clean + off, len) != 0)
	{
		g_st_warn++;
		if(nwarn++ < 32)
		{
			if(what) NVLOG3("KICK WARN vertex array has unflushed CPU writes vm/len/bo", vm, len, b->phys);
			else     NVLOG3("KICK WARN M2MF src has unflushed CPU writes vm/len/bo", vm, len, b->phys);
		}
	}
}
static void shim_kick_check(const uint32_t *m, uint32_t count)
{
	uint32_t i = 0, in_linear = 1, in_off = 0, in_pitch = 0, in_len = 0, in_lines = 0;
	while(i < count)
	{
		uint32_t h = m[i], typ = h >> 29, subc = (h >> 13) & 7, mthd = h & 0x1ffc, n = (h >> 18) & 0x7ff, k;
		if((typ != 0 && typ != 2) || i + 1 + n > count) return;   /* malformed: the puller walk reports it */
		for(k = 0; k < n; k++)
		{
			uint32_t mm = typ == 0 ? mthd + 4 * k : mthd, v = m[i + 1 + k];
			if(subc == 5)                                   /* M2MF (nv50_winsys.h SUBC_M2MF) */
			{
				if(mm == 0x200)      in_linear = v;         /* LINEAR_IN */
				else if(mm == 0x30c) in_off = v;            /* OFFSET_IN */
				else if(mm == 0x314) in_pitch = v;          /* PITCH_IN */
				else if(mm == 0x31c) in_len = v;            /* LINE_LENGTH_IN */
				else if(mm == 0x320) in_lines = v;          /* LINE_COUNT */
				else if(mm == 0x328 && in_linear && in_lines && in_len)   /* BUFFER_NOTIFY = go */
					shim_check_range(0, in_off, (in_lines - 1) * in_pitch + in_len);
			}
			else if(subc == 3 && mm >= 0x908 && mm < 0x908 + 16 * 0x10 && ((mm - 0x908) & 0xf) == 0)
				shim_check_range(1, v, 0);                  /* 3D VERTEX_ARRAY_START_LOW(i): whole BO */
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
						vxd_vram_read(phys, (uint32_t*)((char*)b->base.map + (phys - b->phys)), 4);
						if(b->clean) memcpy(b->clean + (phys - b->phys), (char*)b->base.map + (phys - b->phys), 16);
					}
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
};
static void push_unref_all(struct shim_push *p);

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
	if(size == 0) size = 0x100;
	/* 3091: a tiled surface (tile_mode / memtype set) gets whole 64 KiB pages
	 * of its own, because its pages carry a storage kind in the PTEs. */
	tiled = (config && (config->nv50.tile_mode || config->nv50.memtype)) ? 1 : 0;
	if(tiled)
	{
		if(align < 0x10000) align = 0x10000;
		size = ((uint32_t)size + 0xffffUL) & ~0xffffUL;
	}
	phys = heap_alloc((uint32_t)size, align);
	if(!phys)
	{
#ifndef NVSHIM_STANDALONE
		uint32_t ft, lg; heap_stats(&ft, &lg);
		NVLOG3("bo_new FAIL heap full sz/free/largest", (uint32_t)size, ft, lg);
#endif
		return -1;
	}
#ifndef NVSHIM_STANDALONE
	if((g_heap_allocs & 255) == 0)
	{
		uint32_t ft, lg; heap_stats(&ft, &lg);
		NVLOG3("heap allocs/used/largest", g_heap_allocs, g_heap_used, lg);
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
	NVLOG3("bo_new sz/phys/vmoff", (uint32_t)size, phys, (uint32_t)b->base.offset);
	if(b->kind)
		vxd_vmkind(phys, b->asize, b->kind);
	if(g_nbos < SHIM_MAXBO) g_bos[g_nbos++] = b;
	*pbo = &b->base;
	return 0;
}

int nouveau_bo_map(struct nouveau_bo *bo, uint32_t access, struct nouveau_client *client)
{
	struct shim_bo *b = (struct shim_bo *)bo;
	(void)client;
	/* Submit whatever Mesa has queued (e.g. the M2MF copy that fills a staging
	 * BO) before reading VRAM back - libdrm does this implicitly. */
	shim_flush_pending();
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
	}
	else if(access & NOUVEAU_BO_RD)
	{
		/* Refresh with whatever the GPU has written since the last map. */
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		memcpy(b->clean, b->base.map, b->shadow_sz);
	}
	return 0;
}

void nouveau_bo_ref(struct nouveau_bo *bo, struct nouveau_bo **pref)
{
	struct shim_bo *old = (struct shim_bo *)(pref ? *pref : NULL);
	if(bo)
		((struct shim_bo *)bo)->refcnt++;
	if(old && --old->refcnt == 0)
	{
		int i;
		for(i = 0; i < g_nbos; i++)
			if(g_bos[i] == old) { g_bos[i] = g_bos[--g_nbos]; break; }
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
	/* Submit anything still queued first (libdrm kicks implicitly here), then
	 * - submits being synchronous in the VxD - the GPU is idle. For a read
	 * wait, pull the latest VRAM contents into the shadow. */
	shim_flush_pending();
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
	if(bc->nref >= SHIM_MAXREF)
		return NULL;
	r = &bc->refs[bc->nref];
	memset(r, 0, sizeof(*r));
	r->bo    = bo;
	r->flags = flags;
	bc->bin[bc->nref] = (uint8_t)bin;
	bc->nref++;
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
	uint32_t off, run_start = 0, run_len = 0;
	if(!b->base.map || !b->shadow_sz || b->external) return;
	if(!b->clean)
	{
		vxd_vram_write(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		g_st_flush_dw += (b->shadow_sz + 3) / 4;
		return;
	}
	for(off = 0; off < b->shadow_sz; off += SHIM_FLUSH_CHUNK)
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
	if(shim_nokref()) { p->nrefn = 0; return; }      /* 3093 mode: recorded without a reference */
	for(i = 0; i < p->nrefn; i++)
	{
		struct nouveau_bo *bo = p->refn[i];
		p->refn[i] = NULL;
		nouveau_bo_ref(NULL, &bo);                    /* frees it here if Mesa already let go */
	}
	p->nrefn = 0;
}

int nouveau_pushbuf_kick(struct nouveau_pushbuf *push)
{
	struct shim_push *p = (struct shim_push *)push;
	uint32_t count = (uint32_t)(push->cur - p->buf);
	int ret = 0;

	push_flush_refs(p);
	if(count)
		ret = vxd_submit(p->buf, count);

	/* reset the command buffer for the next batch */
	push->cur = p->buf;
	push->end = p->buf + p->cap;
	push_unref_all(p);                               /* submits are synchronous: the GPU is done with them */

	if(push->kick_notify)
		push->kick_notify(push);
	return ret;
}

/* Record bo in the pushbuf's kref list (once) and hold a reference on it. A
 * full list submits what is queued first, as libdrm does when its buffer
 * list overflows (PUSH_VAL sits at a packet boundary, so that is safe). */
static void push_kref(struct shim_push *p, struct nouveau_bo *bo)
{
	int i;
	if(!bo) return;
	for(i = 0; i < p->nrefn; i++)
		if(p->refn[i] == bo) return;
	if(p->nrefn >= SHIM_MAXREF)
	{
		static int nfull = 0;
		if(nfull++ < 8) NVLOG1("kref list full - kicking early, cap", (uint32_t)SHIM_MAXREF);
		nouveau_pushbuf_kick(&p->base);
	}
	if(!shim_nokref())
		((struct shim_bo *)bo)->refcnt++;
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
		int ret = nouveau_pushbuf_kick(push);  /* flush what we have, start fresh */
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
	/* A secondary command buffer living in a BO: in IB mode each is its own
	 * submit. Flush the current ring-3 batch first to keep ordering, then push
	 * the BO's bytes straight from its shadow (they already hold the methods). */
	if(push->cur != p->buf)
		nouveau_pushbuf_kick(push);
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
	push_kref_bufctx((struct shim_push *)push);
	return 0;
}
