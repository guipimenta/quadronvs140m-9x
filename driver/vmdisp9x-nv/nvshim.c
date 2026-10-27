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

/* ========================= VxD transport ============================== */
static HANDLE g_vxd = INVALID_HANDLE_VALUE;
static int    g_vxd_owned = 0;   /* did we open it (vs. a handle bound in)? */

void nvshim_bind(void *vxd_handle)
{
	g_vxd = (HANDLE)vxd_handle;
	g_vxd_owned = 0;
}

static int nvshim_open(void)
{
	if(g_vxd != INVALID_HANDLE_VALUE)
		return 1;
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
static void nvshim_logn(const char *label, unsigned n, const uint32_t *v)
{
	char buf[160];
	int p = 0, i; unsigned k;
	DWORD ret;
	for(i = 0; label[i] && p < 120; i++) buf[p++] = label[i];
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
		DeviceIoControl(g_vxd, OP_NV_DBG_PUTS, buf, (DWORD)p, NULL, 0, &ret, NULL);
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
static int g_gpu_dead = 0;   /* 3087: set after a wedged/failed kick; no more submits */
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
	if(nkick < 16) nvshim_dump_kick(nkick, methods, count);
#endif
	memset(&sb, 0, sizeof(sb));
	if(!DeviceIoControl(g_vxd, OP_NV_SUBMIT, (void*)methods, count * 4,
	                    &sb, sizeof(sb), &ret, NULL) || !sb.ok)
	{
#ifndef NVSHIM_STANDALONE
		/* Log the failure with the GPU state so a TRAP during screen_create/draw
		 * is visible, then stop submitting: the app keeps running on the CPU side
		 * (garbage window, complete log) instead of freezing the machine in the
		 * VxD's per-kick polls. */
		static int nfail = 0;
		if(nfail++ < 32) NVLOG3("kick FAIL cnt/get/put", count, sb.get, sb.put);
		if(nfail <= 32) NVLOG3("kick FAIL pfi/pgi/wedged", sb.pfifo_intr, sb.pgraph_intr, sb.wedged);
		if(!g_gpu_dead) { g_gpu_dead = 1; NVLOG1("GPU DEAD - submissions stopped, loops", sb.idle_loops); }
#endif
		return -1;
	}
#ifndef NVSHIM_STANDALONE
	if(nkick++ < 400) NVLOG3("kick ok cnt/put/pgi", count, sb.put_off, sb.pgraph_intr);
#endif
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
	while(done < dwords)
	{
		uint32_t chunk = dwords - done;
		if(chunk > 64) chunk = 64;
		in[0] = off + done * 4;
		in[1] = chunk;
		memset(&vr, 0, sizeof(vr));
		if(!DeviceIoControl(g_vxd, OP_NV_VRAM_READ, in, sizeof(in),
		                    &vr, sizeof(vr), &ret, NULL))
			return;
		memcpy(dst + done, vr.data, chunk * 4);
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
	DeviceIoControl(g_vxd, OP_NV_BIND, in, sizeof(in), &bd, sizeof(bd), &ret, NULL);
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
		if(!DeviceIoControl(g_vxd, OP_NV_VRAM_WRITE, buf, (2 + chunk) * 4,
		                    &ret, sizeof(ret), &ret, NULL))
			return;
		done += chunk;
	}
}

/* ========================= VRAM heap ================================== */
static uint32_t g_heap = NVSHIM_HEAP_BASE;
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

static uint32_t heap_alloc(uint32_t size, uint32_t align)
{
	uint32_t a;
	if(align < 0x100) align = 0x100;        /* nv50 wants generous alignment */
	size = (size + 0xff) & ~0xffUL;
	a = (g_heap + (align - 1)) & ~(align - 1);
	if(a + size > NVSHIM_HEAP_END)
		return 0;                            /* out of VRAM (bring-up limit) */
	g_heap = a + size;
	return a;
}

/* ========================= private structs ============================ */
#define SHIM_MAXREF 512

struct shim_bo {
	struct nouveau_bo base;
	int      refcnt;
	uint32_t phys;        /* VRAM byte offset */
	uint32_t shadow_sz;   /* bytes backing base.map, 0 until first map */
};

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
	struct nouveau_bo *refn[SHIM_MAXREF];
	int       nrefn;
};

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

int nouveau_getparam(struct nouveau_device *dev, uint64_t param, uint64_t *value)
{
	(void)dev; (void)param;
	/* The only param on the hot path is PTIMER_TIME (a GPU timestamp used for
	 * fence staleness). A CPU monotonic clock in ns is a safe stand-in. */
	*value = (uint64_t)GetTickCount() * 1000000ULL;
	return 0;
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
	if(size == 0) size = 0x100;
	phys = heap_alloc((uint32_t)size, align);
	if(!phys)
	{
#ifndef NVSHIM_STANDALONE
		NVLOG2("bo_new FAIL heap full sz/top", (uint32_t)size, g_heap);
#endif
		return -1;
	}
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
	NVLOG3("bo_new sz/phys/vmoff", (uint32_t)size, phys, (uint32_t)b->base.offset);
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
		b->base.map  = malloc(b->shadow_sz);
		if(!b->base.map) { b->shadow_sz = 0; return -1; }
		/* Seed the shadow from VRAM so read-modify-write is correct. */
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
	}
	else if(access & NOUVEAU_BO_RD)
	{
		/* Refresh with whatever the GPU has written since the last map. */
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
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
		if(old->base.map) free(old->base.map);
		/* Bump heap: reclaim when this BO is the top-most allocation (the
		 * per-present staging buffer always is), else the space is leaked. */
		if(old->phys + (uint32_t)old->base.size == g_heap)
			g_heap = old->phys;
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
		vxd_vram_read(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
	return 0;
}

/* Sharing / import-export is unsupported on Win9x. */
int nouveau_bo_name_get(struct nouveau_bo *bo, uint32_t *name) { (void)bo; (void)name; return -1; }
int nouveau_bo_name_ref(struct nouveau_device *dev, uint32_t name, struct nouveau_bo **pbo) { (void)dev; (void)name; (void)pbo; return -1; }
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
static void push_flush_refs(struct shim_push *p)
{
	int i;
	if(p->base.bufctx)
	{
		struct shim_bufctx *bc = (struct shim_bufctx *)p->base.bufctx;
		for(i = 0; i < bc->nref; i++)
		{
			struct shim_bo *b = (struct shim_bo *)bc->refs[i].bo;
			if(b && b->base.map)
				vxd_vram_write(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
		}
	}
	for(i = 0; i < p->nrefn; i++)
	{
		struct shim_bo *b = (struct shim_bo *)p->refn[i];
		if(b && b->base.map)
			vxd_vram_write(b->phys, (uint32_t*)b->base.map, (b->shadow_sz + 3) / 4);
	}
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
	p->nrefn = 0;

	if(push->kick_notify)
		push->kick_notify(push);
	return ret;
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
		(void)vxd_submit((const uint32_t*)((char*)b->base.map + offset), dwords);
}

int nouveau_pushbuf_refn(struct nouveau_pushbuf *push, struct nouveau_pushbuf_refn *refs, int nr)
{
	struct shim_push *p = (struct shim_push *)push;
	int i;
	for(i = 0; i < nr; i++)
	{
		if(p->nrefn >= SHIM_MAXREF)
			break;
		p->refn[p->nrefn++] = refs[i].bo;
	}
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
	/* Every BO is permanently resident in the VM window, so validation always
	 * succeeds. (A real heap with eviction would re-check residency here.) */
	(void)push;
	return 0;
}
