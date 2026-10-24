/*****************************************************************************
 * util/os_mman.h - Win9x compat shim (M5.3c)
 *
 * mesa's own util/os_mman.h #errors "Unsupported OS" on non-POSIX. The nouveau
 * gallium driver references only os_mmap/os_munmap, and only to CPU-map buffer
 * objects - which on Win9x our nvshim.c handles itself (bounce buffers), not
 * through mmap. So these are stubs: os_mmap always fails, os_munmap is a no-op.
 * This file shadows mesa's copy (keep mesa-compat first on the include path).
 * Licensed MIT.
 *****************************************************************************/
#ifndef _OS_MMAN_H_          /* match mesa's guard so its header stays out */
#define _OS_MMAN_H_

#include <stddef.h>

#ifndef PROT_READ
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#endif
#ifndef MAP_FAILED
#define MAP_FAILED  ((void *)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline void *os_mmap(void *addr, size_t length, int prot, int flags,
                            int fd, long long offset)
{
	(void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
	return MAP_FAILED;   /* Win9x: BO mapping is done by nvshim, not mmap */
}

static inline int os_munmap(void *addr, size_t length)
{
	(void)addr; (void)length;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _OS_MMAN_H_ */
