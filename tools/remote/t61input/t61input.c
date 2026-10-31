/*
 * t61input.vxd - mouse and keyboard input for the remote helper (t61util.exe)
 * that games reading DirectInput can see.
 *
 * user32's mouse_event/keybd_event only reach USER's queue; on Windows 9x
 * DirectInput takes its data from VMOUSE and VKD below that. This dynamic VxD
 * posts input where the real hardware's drivers do:
 *   mouse: VMD_Post_Pointer_Message (what a mouse mini driver calls on an IRQ)
 *   keys:  VKD_Force_Keys (scan codes "as if typed on the physical keyboard")
 *
 * t61util opens it with CreateFile("\\.\C:\REMOTE\T61INPUT.VXD") - loaded on
 * open, unloaded when the last handle closes; nothing to install.
 *
 * DeviceIoControl codes:
 *   T61IN_MOUSE  in: struct t61in_mouse  {long dx, dy; DWORD buttons}
 *                buttons: 1 = left, 2 = right, 4 = middle (held while posted)
 *   T61IN_KEYS   in: scan code bytes (0x80 set = key up, 0xE0 prefix = extended)
 *
 * Calling conventions from the Win95 DDK (inc32/vmd.inc, vkd.inc) and a working
 * Win9x HID mouse VxD (squadtak/w95hid mouhid.asm):
 *   VMD_Post_Pointer_Message (000C:0003): ESI = dx, EDI = dy (mickeys),
 *     AL = buttons (bit 5 = 1/left, bit 4 = 2/right, bit 3 = 3/middle),
 *     EBX = a VM handle.
 *   VKD_Force_Keys (000D:0007): ESI -> scan codes, ECX = count; CF = overflow.
 *
 * Build: make -C tools/remote/t61input (Open Watcom + fixlink from the driver tree).
 */
#include "winhack.h"
#include "vmm.h"

#define VMD__Post_Pointer_Message 3
#define VKD__Force_Keys           7

#define T61IN_MOUSE 0x7101
#define T61IN_KEYS  0x7102

struct t61in_mouse { long dx, dy; DWORD buttons; };

void T61IN_control();

/* must be the first thing in the code segment (see vxd_main.c) */
DDB T61IN_DDB = {
	NULL,
	DDK_VERSION,
	UNDEFINED_DEVICE_ID,
	1, 0,
	NULL,
	{'T','6','1','I','N','P','U','T'},
	Undefined_Init_Order,
	(DWORD)T61IN_control,
	0, 0, 0, 0, 0, 0, 0, 0,
	'Prev',
	sizeof(DDB),
	'Rsv1', 'Rsv2', 'Rsv3',
};

static BYTE keybuf[64];

/* register names (dx, al) cannot be C names inside Watcom's _asm */
static void post_pointer(long mx, long my, DWORD btn)
{
	_asm pushad
	VMMCall(Get_Cur_VM_Handle);
	_asm mov esi, [mx]
	_asm mov edi, [my]
	_asm mov eax, [btn]
	_asm cld
	_asm pushfd                /* as from a mouse IRQ: interrupts off */
	_asm cli
	VxDCall(VMD, Post_Pointer_Message);
	_asm popfd
	_asm popad
}

static DWORD force_keys(BYTE *buf, DWORD cnt)
{
	DWORD left = 0;
	_asm pushad
	_asm mov esi, [buf]
	_asm mov ecx, [cnt]
	VxDCall(VKD, Force_Keys);
	_asm sbb eax, eax          /* CF (overflow) -> -1 */
	_asm and ecx, eax
	_asm mov [left], ecx
	_asm popad
	return left;
}

/* returns 0 = done (DeviceIoControl TRUE), else an error code */
DWORD __stdcall T61IN_ioctl(DWORD vm, struct DIOCParams *p)
{
	(void)vm;
	switch (p->dwIoControlCode) {
	case DIOC_OPEN:
	case DIOC_CLOSEHANDLE:
		return 0;
	case T61IN_MOUSE: {
		struct t61in_mouse *m = (struct t61in_mouse *)p->lpInBuffer;
		DWORD b = 0;
		if (p->cbInBuffer < sizeof(*m) || !m) return 87;
		if (m->buttons & 1) b |= 0x20;
		if (m->buttons & 2) b |= 0x10;
		if (m->buttons & 4) b |= 0x08;
		post_pointer(m->dx, m->dy, b);
		return 0;
	}
	case T61IN_KEYS: {
		DWORD n = p->cbInBuffer, i;
		if (!n || n > sizeof(keybuf) || !p->lpInBuffer) return 87;
		for (i = 0; i < n; i++) keybuf[i] = ((BYTE *)p->lpInBuffer)[i];
		return force_keys(keybuf, n) ? 111 : 0;
	}
	}
	return 50;
}

void __declspec(naked) T61IN_control()
{
	_asm {
		cmp eax, Sys_Dynamic_Device_Init
		jz ok
		cmp eax, Sys_Dynamic_Device_Exit
		jz ok
		cmp eax, W32_DEVICEIOCONTROL
		jnz ok
		push esi
		push ebx
		call T61IN_ioctl
		clc
		ret
	ok:
		xor eax, eax
		clc
		ret
	}
}
