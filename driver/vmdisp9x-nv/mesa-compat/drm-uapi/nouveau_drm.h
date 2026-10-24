/*****************************************************************************
 * drm-uapi/nouveau_drm.h - Win9x compat shim (M5.3c)
 *
 * Mesa's nouveau winsys/driver include <drm-uapi/nouveau_drm.h>, whose real
 * copy pulls in <drm-uapi/drm.h> -> <sys/ioccom.h> and the whole Linux DRM
 * ioctl machinery. None of that exists (or means anything) on Win9x: our GPU
 * kernel is the nvmini VxD, reached through nvshim.c, not /dev/dri. This header
 * stands in for the real one when building mesa3d.dll for Win9x, defining ONLY
 * the handful of symbols the nv50 gallium driver actually references:
 *   - NOUVEAU_GETPARAM_* (values mirror the real uapi)
 *   - the SVM init/bind structs + ioctl ids (nouveau_screen.c SVM path)
 * Put this directory first on the include path so it shadows mesa's include/.
 * Values kept in sync with mesa-24.1.x include/drm-uapi/nouveau_drm.h.
 * Licensed MIT.
 *****************************************************************************/
#ifndef __NVSHIM_COMPAT_NOUVEAU_DRM_H__
#define __NVSHIM_COMPAT_NOUVEAU_DRM_H__

#include <stdint.h>

#ifndef __u8
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define NOUVEAU_GETPARAM_PCI_VENDOR       3
#define NOUVEAU_GETPARAM_PCI_DEVICE       4
#define NOUVEAU_GETPARAM_BUS_TYPE         5
#define NOUVEAU_GETPARAM_FB_SIZE          8
#define NOUVEAU_GETPARAM_AGP_SIZE         9
#define NOUVEAU_GETPARAM_CHIPSET_ID       11
#define NOUVEAU_GETPARAM_VM_VRAM_BASE     12
#define NOUVEAU_GETPARAM_GRAPH_UNITS      13
#define NOUVEAU_GETPARAM_PTIMER_TIME      14
#define NOUVEAU_GETPARAM_HAS_BO_USAGE     15
#define NOUVEAU_GETPARAM_HAS_PAGEFLIP     16
#define NOUVEAU_GETPARAM_EXEC_PUSH_MAX    17
#define NOUVEAU_GETPARAM_VRAM_BAR_SIZE    18
#define NOUVEAU_GETPARAM_VRAM_USED        19
#define NOUVEAU_GETPARAM_HAS_VMA_TILEMODE 20

#define DRM_NOUVEAU_SVM_INIT              0x08
#define DRM_NOUVEAU_SVM_BIND              0x09

struct drm_nouveau_svm_init {
	__u64 unmanaged_addr;
	__u64 unmanaged_size;
};

struct drm_nouveau_svm_bind {
	__u64 header;
	__u64 va_start;
	__u64 va_end;
	__u64 npages;
	__u64 stride;
	__u64 result;
	__u64 reserved0;
	__u64 reserved1;
};

#endif /* __NVSHIM_COMPAT_NOUVEAU_DRM_H__ */
