/*****************************************************************************
 * sys/ioccom.h - Win9x compat shim (M5.3c)
 *
 * mesa's drm-uapi/drm.h (pulled in by nouveau_winsys.h) takes its "one of the
 * BSDs" branch on non-Linux and needs <sys/ioccom.h> for the _IO/_IOR/_IOW/
 * _IOWR ioctl-encoding macros. mingw has no such header. These are the classic
 * BSD definitions; the values are irrelevant on Win9x (we issue no DRM ioctls),
 * they only need to exist so drm.h's DRM_IO* macros expand. Licensed MIT.
 *****************************************************************************/
#ifndef __NVSHIM_COMPAT_SYS_IOCCOM_H__
#define __NVSHIM_COMPAT_SYS_IOCCOM_H__

#define IOCPARM_MASK  0x1fff
#define IOC_VOID      0x20000000U
#define IOC_OUT       0x40000000U
#define IOC_IN        0x80000000U
#define IOC_INOUT     (IOC_IN | IOC_OUT)

#define _IOC(inout,group,num,len) \
	((int)((inout) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num)))
#define _IO(g,n)      _IOC(IOC_VOID,  (g), (n), 0)
#define _IOR(g,n,t)   _IOC(IOC_OUT,   (g), (n), sizeof(t))
#define _IOW(g,n,t)   _IOC(IOC_IN,    (g), (n), sizeof(t))
#define _IOWR(g,n,t)  _IOC(IOC_INOUT, (g), (n), sizeof(t))

#endif /* __NVSHIM_COMPAT_SYS_IOCCOM_H__ */
