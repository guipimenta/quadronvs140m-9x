/* dbgring.h - in-memory debug log shared by nvmini.vxd and nvmini.drv.
 *
 * With DBGRING defined, dbg_printf() no longer writes to a COM port (the T61
 * has none). The 32-bit VxD keeps a 16 KB ring buffer; its own dbg_printf
 * lines go there directly (prefixed "V> "), and the 16-bit display driver
 * pushes its dbg_printf characters into the same ring through the VxD's
 * PM16 API (one OP_NV_DBG_PUTC call per character, lines prefixed "D> ").
 * The ring survives a vga.drv fallback because the VxD stays loaded; read it
 * with "nvprobe log" (OP_NV_DBGLOG). Nothing touches the disk at boot.
 */
#ifndef __DBGRING_H__INCLUDED__
#define __DBGRING_H__INCLUDED__

#define DBG_RING_SIZE      16384

#define OP_NV_DBGLOG       0x500A  /* W32 DeviceIoControl: copy ring (oldest first) to outBuf */
#define OP_NV_DBG_PUTC     0x500B  /* PM16 API: ECX = one character from the 16-bit driver     */
#define OP_NV_DBGLOG2      0x500D  /* 3049: outBuf = DWORD total_bytes_ever_written, then the ring (oldest first) */

#endif
