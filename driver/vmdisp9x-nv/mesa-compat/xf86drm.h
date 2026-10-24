/*****************************************************************************
 * xf86drm.h - Win9x compat shim (M5.3c)
 *
 * The nouveau gallium driver #includes <xf86drm.h> (libdrm) but, with our
 * nvshim.c replacing libdrm's nouveau.c, it actually references only
 * drmCommandWrite() - and only on the SVM (unified-memory) path, which the G86
 * does not use. Stub it so the driver compiles on Win9x; SVM init simply fails.
 * Licensed MIT.
 *****************************************************************************/
#ifndef __NVSHIM_COMPAT_XF86DRM_H__
#define __NVSHIM_COMPAT_XF86DRM_H__

#include <stddef.h>

static inline int drmCommandWrite(int fd, unsigned long request,
                                  void *data, unsigned long size)
{
	(void)fd; (void)request; (void)data; (void)size;
	return -1;   /* unsupported on Win9x (no DRM) */
}

static inline int drmCommandWriteRead(int fd, unsigned long request,
                                      void *data, unsigned long size)
{
	(void)fd; (void)request; (void)data; (void)size;
	return -1;
}

#endif /* __NVSHIM_COMPAT_XF86DRM_H__ */
