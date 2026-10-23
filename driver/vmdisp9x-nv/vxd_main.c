/*****************************************************************************

Copyright (c) 2024 Jaroslav Hensl <emulator@emulace.cz>

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

/* 
 * Code for QEMU is inspired by boxvmini.asm by Philip Kelley
 */

#ifndef VXD32
#error VXD32 not defined!
#endif

#define VXD_MAIN

#ifndef SVGA
# ifndef VESA
#  define VBE
# endif
#endif

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"
#include "vxd_lib.h"
#include "dbgring.h"

#include "3d_accel.h"
#include "vxd_vdd.h"

#ifdef SVGA
# include "svga_all.h"
# include "vxd_halloc.h"
# include "vxd_svga.h"
#endif

#ifdef VESA
BOOL VESA_check_int10h(DWORD vm, PCRS_32 regs);
#endif

#include "version.h"

#ifdef NV_DETECT
#include "vxd_nv.h"
#include "nv50_evo.h"
#include "nv50_gpu.h"
#include "wram.h"
extern DWORD nv_mtrr_rc, nv_mtrr_base, nv_mtrr_size;   /* vxd_vesa.c */
extern DWORD nv_pat_state, nv_pat_pages, nv_pat_bad;    /* vxd_vesa.c */
static DWORD nv_cpuid1_edx(void);
#pragma aux nv_cpuid1_edx = "mov eax, 1" "cpuid" "mov eax, edx" value [eax] modify exact [eax ebx ecx edx];
static void nv_rdmsr(DWORD msr, DWORD *lo, DWORD *hi);
#pragma aux nv_rdmsr = "rdmsr" "mov [esi], eax" "mov [edi], edx" \
	parm [ecx] [esi] [edi] modify exact [eax edx];
extern unsigned long nv_dbg_fbhda;  /* OP_FBHDA_SETUP count = nvmini.drv loaded */
#endif

#include "code32.h"

void VXD_control();
void VXD_API_entry();
void __stdcall Device_Init_proc(DWORD VM);
void __stdcall Device_Exit_proc(DWORD VM);
void __stdcall Device_Init_Complete(DWORD VM);
void __stdcall win32_destroy_process_proc(DWORD pid);

#define SERVICE_TABLE_CNT (FBHDA__OVERLAY_UNLOCK+1)
extern DWORD service_table[SERVICE_TABLE_CNT];

/*
  VXD structure
  this variable must be in first address in code segment.
  In other cases VXD isn't loadable (WLINK bug?)
*/
DDB VXD_DDB = {
	NULL,                       // must be NULL
	DDK_VERSION,                // DDK_Version
	VXD_DEVICE_ID,              // Device ID
	VXD_MAJOR_VER,              // Major Version
	VXD_MINOR_VER,              // Minor Version
	NULL,
	VXD_DEVICE_NAME,
	VDD_Init_Order,
	(DWORD)VXD_control,
	(DWORD)VXD_API_entry,
	(DWORD)VXD_API_entry,
	NULL,// CS:IP of API entry point
	NULL,// CS:IP of API entry point
	NULL,// Reference data from real mode
	(DWORD)&service_table,// Pointer to service table
	SERVICE_TABLE_CNT,// Number of services
	NULL, // DDB_Win32_Service_Table
	'Prev', //NULL, // prev
	sizeof(DDB),
	'Rsv1',
	'Rsv2',
	'Rsv3',
};

#include "vxd_strings.h"

DWORD *DispatchTable = 0;
DWORD DispatchTableLength = 0;
DWORD ThisVM = 0;
void *DeviceCTX = NULL;
DWORD is_qemu = FALSE;

void __stdcall port_info(DWORD p_eax, DWORD p_ecx, DWORD p_edx)
{
	dbg_printf("virtual_0x1ce eax=%lX ecx=%lX edx=%lX\n", p_eax, p_ecx, p_edx);
}

DWORD __stdcall virtual_int_proc(DWORD int_num, DWORD vm, PCRS_32 regs)
{
	if(int_num == 0x10)
	{
#ifdef VESA
		if(!VESA_check_int10h(vm, regs))
		{
			dbg_printf("INT 10h ignored for EAX=%lX\n", regs->Client_EAX);
			return 1;
		}
#endif
	}
	
	return 0;
}

#if defined(QEMU) || defined(SVGA) || defined(VESA)
/**
 * This is fix of broken screen when open DOS window
 *
 **/
void __declspec(naked) virtual_0x1ce()
{
/*
 * AX/AL contains the value to be read or written on the port.
 * EBX contains the handle of the VM accessing the port.
 * ECX contains the direction (in/out) and size (byte/word) of the operation.
 * EDX contains the port number, which for us will either be 1CEh or 1CFh.
 */
/*
	_asm{
		pushad
		push edx
		push ecx
		push eax
		call port_info
		popad
	}*/
	VxDCall(VDD, Get_VM_Info); // esi = result
	_asm{
		cmp edi, ThisVM             ; Is the CRTC controlled by Windows?
    jne _Virtual1CEPhysical     ; If not, we should allow the I/O
    cmp ebx, edi                ; Is the calling VM Windows?
    je _Virtual1CEPhysical      ; If not, we should eat the I/O
    	ret
		_Virtual1CEPhysical:
	}
	VxDJmp(VDD, Do_Physical_IO);
}

static DWORD int_10h_next_hook = 0;
void __declspec(naked) virtual_int_10h()
{
	_asm
	{
		; header (8 bytes)
		jmp short virtual_int_10h_entry
		jmp [int_10h_next_hook]
		virtual_int_10h_entry:

		pushad
		push ebp
		push ebx
		push eax
		call virtual_int_proc
		sub eax,1
		popad
		retn

		jmp [int_10h_next_hook]
	}
}

#endif /* QEMU/SVGA/VESA */

/**
 * VDD calls wrapers
 **/
void VDD_Get_Mini_Dispatch_Table()
{
	VxDCall(VDD, Get_Mini_Dispatch_Table);
	_asm mov [DispatchTable],edi
	_asm mov [DispatchTableLength],ecx
}

/**
 * Control Handles
 **/
void Sys_Critical_Init_proc()
{
	// nop
}

DWORD __stdcall Device_IO_Control_proc(DWORD vmhandle, struct DIOCParams *params);

/*
 * 32-bit DeviceIoControl ends here
 *
 */
void __declspec(naked) Device_IO_Control_entry()
{
	_asm {
		push esi /* struct DIOCParams */
		push ebx /* VMHandle */
		call Device_IO_Control_proc
		retn
	}
}

void VESA_check_state();

/* M4.5: VPOWERD/APM system control message. EBX = PWR_* event
 * (1 SUSPENDREQUEST, 2 SUSPENDRESUME, 3 CRITICALRESUME; Win98 adds standby
 * variants) - logged, and a resume arms the post-resume engine check. */
void __stdcall Power_Event_proc(DWORD event, DWORD flags)
{
	dbg_printf("Power_Event: %lX flags=%lX\n", event, flags);
#ifdef NV_DETECT
	if(event == 2 || event == 3 || event == 5 || event == 6)
		NV_power_notify(1);
#endif
}

/* M4.5: mini-VDD SET_ADAPTER_POWER_STATE (index 52), called by the VDD's
 * _CallPowerStateFunc as a C function: (DEVNODE, DWORD state), state is the
 * CM power state (1 D0, 2 D1, 4 D2, 8 D3). Return 1 so the VDD continues with
 * its default handling (it re-POSTs the VGA ROM on D3 -> D0, after which the
 * display driver restores the desktop through our modeset path). */
DWORD __cdecl set_adapter_power_state_proc(DWORD devnode, DWORD state)
{
	dbg_printf("SET_ADAPTER_POWER_STATE: devnode=%lX state=%lX\n", devnode, state);
#ifdef NV_DETECT
	NV_power_notify(state);
#endif
	return 1;
}

void __stdcall Set_Device_Focus_proc(DWORD VMHandle, DWORD VID, DWORD Flags, DWORD AssocVMHandle)
{
#ifdef VESA
//	dbg_printf("Set_Device_Focus\n");
#endif
}

#ifdef DBGPRINT
void __stdcall print_ctrl(DWORD type)
{
	VMCB_t *cur = Get_Cur_VM_Handle();
	dbg_printf("VXD_control: type=%lX curVM=%lX thisVM=%lX\n", type, cur, ThisVM);
}
#endif

/*
 * service module calls (init, exit, deviceIoControl, ...)
 * clear carry if succes (this is always success)
 */
void __declspec(naked) VXD_control()
{
	// eax = 0x00 - Sys Critical Init
	// eax = 0x01 - sys dynamic init
	// eax = 0x1B - dynamic init
	// eax = 0x1C - dynamic exit
	// eax = 0x23 - device IO control

#if 0
	_asm
	{
		pushad
		push eax
		call print_ctrl
		popad
	}
#endif
	
	_asm {
		cmp eax,Sys_Critical_Init
		jnz control_1
			pushad
			call Sys_Critical_Init_proc
			popad
			clc
			ret
		control_1: 
		cmp eax,Device_Init
		jnz control_2
			push ebx ; VM handle
		  call Device_Init_proc
			popad
			clc
			ret
		control_2:
		cmp eax,Init_Complete
		jnz control_3
			pushad
			push ebx ; VM handle
		  call Device_Init_Complete
			popad
			clc
			ret
		control_3:
		cmp eax,Sys_Dynamic_Device_Init
		jnz control_4
			pushad
			push ebx ; VM handle
		  call Device_Init_proc
		  popad
			clc
			ret
		control_4:
		cmp eax,W32_DEVICEIOCONTROL
		jnz control_5
			jmp Device_IO_Control_entry
		control_5:
		cmp eax,System_Exit
		jnz control_6
			pushad
			push ebx ; VM handle
		  call Device_Exit_proc
			popad
			clc
			ret
/*
	https://www.codeproject.com/Articles/3874/The-Ultimate-Process-Thread-spy-for-Windows-9x
	
	"You will find the definition of CREATE_PROCESS, but wont find how to get the
	PID of a newly created process. For thread (CREATE_THREAD), the thread ID is
	passed in EDI register, but for CREATE_PROCESS it's not there. At last, by
	trial and error I found where it is, the PID value, it is in EDX reg."
*/
		control_6:
		cmp eax,DESTROY_PROCESS
		jnz control_7
			pushad
			push edx ; PID
			call win32_destroy_process_proc
			popad
			clc
			ret
		control_7:
		cmp eax, Set_Device_Focus
		jnz control_8
			pushad
			push edi ; AssocVMHandle
			push esi ; Flags
			push edx ; VID
			push ebx ; VMHandle
			call Set_Device_Focus_proc
			popad
			clc
			ret
		control_8:
		cmp eax, Power_Event
		jnz control_9
			pushad
			push esi ; flags
			push ebx ; event
			call Power_Event_proc
			popad
			clc
			ret
		control_9:
;			pushad
;			push eax
;			call print_ctrl
;			popad
		clc
	  ret
	};
}

#ifdef NV_DETECT
static void nv_pm16_trace(DWORD service, PCRS_32 state);
extern FBHDA_t *hda;
#endif

/* process V86/PM16 calls */
WORD __stdcall VXD_API_Proc(PCRS_32 state)
{
	WORD rc = 0xFFFF;
	WORD service = state->Client_EDX & 0xFFFF;
#ifdef DBGRING
	static int first_call = 1;
	if(first_call && service != OP_NV_DBG_PUTC)
	{
		first_call = 0;
		dbg_printf("PM16 API: first call from 16-bit side, service %X\n", service);
	}
#endif
	
	//dbg_printf("VXD_API_Proc, service: %X\n", service);
	//Begin_Critical_Section(0);
#ifdef NV_DETECT
	nv_pm16_trace(service, state);
#endif
	
	switch(service)
	{
		case VXD_PM16_VERSION:
			rc = (VXD_MAJOR_VER << 8) | VXD_MINOR_VER;
			break;
		case VXD_PM16_VMM_VERSION:
			rc = Get_VMM_Version();
			break;
#ifdef DBGRING
		case OP_NV_DBG_PUTC:
			/* one dbg_printf character from the 16-bit driver (dbgring.h) */
			dbg_ring_putc((char)state->Client_ECX);
			rc = 1;
			break;
#endif
		case OP_FBHDA_SETUP:
			{
				DWORD dptr;
				dbg_printf("OP_FBHDA_SETUP from the 16-bit driver\n");
#ifdef NV_DETECT
				/* E4 breadcrumb: the 16-bit nvmini.drv calls FBHDA_SETUP as it
				 * loads. Logging 0x108 here (post-Device_Init, so NV_blog writes)
				 * proves the .drv reached us - distinguishes "driver never loaded"
				 * from "loaded but Enable/setmode failed" in the warm-reboot VRAM
				 * marker trail. */
				NV_blog(0x108);
#endif
				dptr = (DWORD)FBHDA_setup();
				if(dptr)
				{
					state->Client_ECX = dptr;
					rc = 1;
				}
				break;
			}
		case OP_FBHDA_ACCESS_BEGIN:
			FBHDA_access_begin(state->Client_ECX);
			rc = 1;
			break;
		case OP_FBHDA_ACCESS_END:
			FBHDA_access_end(state->Client_ECX);
			rc = 1;
			break;
		case OP_FBHDA_ACCESS_RECT:
			FBHDA_access_rect(state->Client_EBX, state->Client_ECX, state->Client_ESI, state->Client_EDI);
			rc = 1;
			break;
		case OP_FBHDA_SWAP:
			{
				BOOL rs;
				rs = FBHDA_swap(state->Client_ESI, state->Client_ECX);
				state->Client_ECX = (DWORD)rs;
				rc = 1;
				break;
			}
		case OP_FBHDA_CLEAN:
			FBHDA_clean();
			rc = 1;
			break;
		case OP_FBHDA_PALETTE_SET:
			FBHDA_palette_set(state->Client_ECX & 0xFF, state->Client_EDI);
			rc = 1;
			break;
		case OP_FBHDA_PALETTE_GET:
		{
			DWORD color;
			color = FBHDA_palette_get(state->Client_ECX & 0xFF);
			state->Client_EDI = color;
			rc = 1;
			break;
		}
		case OP_FBHDA_GAMMA_GET:
			state->Client_ECX = FBHDA_gamma_get((void *)state->Client_EDI, state->Client_ECX);
			rc = 1;
			break;
		case OP_FBHDA_GAMMA_SET:
			state->Client_ECX = FBHDA_gamma_set((void *)state->Client_ESI, state->Client_ECX);
			rc = 1;
			break;
		case OP_FBHDA_REFRESH:
			FBHDA_refresh(state->Client_ECX);
			break;
		/* mouse */
		case OP_MOUSE_LOAD:
			critical_section_enter();
			state->Client_ECX = mouse_load();
			critical_section_leave();
			rc = 1;
			break;
		case OP_MOUSE_BUFFER:
			critical_section_enter();
			state->Client_ECX = (DWORD)mouse_buffer();
			critical_section_leave();
			rc = 1;
			break;
		case OP_MOUSE_MOVE:
			mouse_move(state->Client_ESI, state->Client_EDI);
			rc = 1;
			break;
		case OP_MOUSE_SHOW:
			mouse_show();
			rc = 1;
			break;
		case OP_MOUSE_HIDE:
			mouse_hide();
			rc = 1;
			break;
#ifdef SVGA
		case OP_SVGA_VALID:
		{
			BOOL rs;
			rs = SVGA_valid();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_SVGA_SETMODE:
		{
			BOOL rs;
			critical_section_enter();
			rs = SVGA_setmode(state->Client_ESI, state->Client_EDI, state->Client_ECX);
			critical_section_leave();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_SVGA_VALIDMODE:
		{
			BOOL rs;
			rs = SVGA_validmode(state->Client_ESI, state->Client_EDI, state->Client_ECX);
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_SVGA_HW_ENABLE:
			SVGA_HW_enable();
			rc = 1;
			break;
		case OP_SVGA_HW_DISABLE:
			SVGA_HW_disable();
			rc = 1;
			break;
#endif

#ifdef VBE
		case OP_VBE_VALID:
		{
			BOOL rs;
			rs = VBE_valid();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_VBE_SETMODE:
		{
			BOOL rs;
			critical_section_enter();
			rs = VBE_setmode(state->Client_ESI, state->Client_EDI, state->Client_ECX);
			critical_section_leave();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_VBE_VALIDMODE:
		{
			BOOL rs;
			rs = VBE_validmode(state->Client_ESI, state->Client_EDI, state->Client_ECX);
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
#endif

#ifdef VESA
		case OP_VESA_VALID:
		{
			BOOL rs;
			rs = VESA_valid();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_VESA_SETMODE:
		{
			BOOL rs;
			DWORD w, h, rr_min, rr_max;
			w = state->Client_ESI >> 16;
			h = state->Client_ESI & 0xFFFF;
			rr_min = state->Client_EDI >> 16;
			rr_max = state->Client_EDI & 0xFFFF;

			critical_section_enter();
			rs = VESA_setmode(w, h, state->Client_ECX, rr_min, rr_max);
			critical_section_leave();
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_VESA_VALIDMODE:
		{
			BOOL rs;
			rs = VESA_validmode(state->Client_ESI, state->Client_EDI, state->Client_ECX);
			state->Client_ECX = (DWORD)rs;
			rc = 1;
			break;
		}
		case OP_VESA_HIRES:
		{
			DWORD enable = state->Client_ECX;
			if(enable)
			{
				VESA_HIRES_enable();
			}
			else
			{
				VESA_HIRES_disable();
			}
			rc = 1;
			break;
		}
#endif

	}
	
	if(rc == 0xFFFF)
	{
		state->Client_EFlags |= 0x1; // set carry
#ifdef DBGRING
		if(service != OP_NV_DBG_PUTC)
			dbg_printf("PM16 API: unknown service %X\n", service);
#endif
	}
	else
	{
		state->Client_EFlags &= 0xFFFFFFFEUL; // clear carry
	}
	
	//dbg_printf("VXD_API_Proc end\n");
	
	//End_Critical_Section();
	
	return rc;
}

/*
 * Service calls from protected mode (usually 16 bit) and virtual 86 mode
 * CPU state before call is already on stack (EBP points it).
 * Converts the call to __stdcall and call 'VXD_API_Proc', result of this
 * function is placed to saved AX register.
 *
 */
void __declspec(naked) VXD_API_entry()
{
	_asm {
		push ebp
		mov eax,cr3 ; TLB bug here? Looks like...
		mov cr3,eax
		call VXD_API_Proc
		mov [ebp+1Ch], ax
		retn
	}
}

const char reg_path[] = "Software\\vmdisp9x\\driver";
extern DWORD gamma_quirk;

static void configure_FBHDA()
{
	FBHDA_t *fbhda;
	DWORD force_sw = 0;
	DWORD force_qemu3dfx = 0;
	int i;
	char buf[32];
	
	fbhda = FBHDA_setup();
	
	if(fbhda)
	{
		DWORD size_mb;
		
		RegReadConf(HKEY_LOCAL_MACHINE, reg_path, "FORCE_SOFTWARE", &force_sw);
		RegReadConf(HKEY_LOCAL_MACHINE, reg_path, "FORCE_QEMU3DFX", &force_qemu3dfx);
		RegReadConf(HKEY_LOCAL_MACHINE, reg_path, "GAMMA_QUIRK",    &gamma_quirk);

		for(i = 1; i < FBHDA_OVERLAYS_MAX; i++)
		{
			char *ptr;
			strcpy(buf, "OVERLAY_");
			
			ptr = buf + strlen(buf);
			if(i >= 10)
			{
				*ptr = '0' + (i/10);
				ptr++;
			}
			*ptr = '0' + (i % 10);
			ptr++;
			*ptr = '\0';
			strcat(buf, "_SIZE");
			
			size_mb = 0;
			RegReadConf(HKEY_LOCAL_MACHINE, reg_path, buf, &size_mb);
			fbhda->overlays[i].size = size_mb*(1024*1024);

			if(fbhda->overlays[i].size > 0)
			{
				fbhda->overlays_size += fbhda->overlays[i].size;
				fbhda->overlays[i].ptr = ((BYTE*)fbhda->vram_pm32) + fbhda->vram_size_virt - fbhda->overlays_size;
			}
			else
			{
				fbhda->overlays[i].ptr = 0;
			}
			
			dbg_printf("Overlay #%ld: %ld\n", i, fbhda->overlays[i].size);			
		}
		
		if(force_sw)
		{
			fbhda->flags |= FB_FORCE_SOFTWARE;
		}
		
		if(force_qemu3dfx)
		{
			fbhda->flags |= FB_ACCEL_QEMU3DFX;
		}
	}
}

/* generate all entry pro VDD function */
#define VDDFUNC(_fnname, _procname) void __declspec(naked) _procname ## _entry() { \
	_asm { push edi }; \
	_asm { push esi }; \
	_asm { push edx }; \
	_asm { push ecx }; /* extra_ecx */ \
	_asm { push eax }; /* extra_eax */ \
	_asm { push ebp }; /* state */ \
	_asm { push ebx }; /* vm */ \
	_asm { call _procname ## _proc }; \
	_asm { pop ebx }; \
	_asm { pop ebp }; \
	_asm { pop eax }; \
	_asm { pop ecx }; \
	_asm { pop edx }; \
	_asm { pop esi }; \
	_asm { pop edi }; \
	_asm { retn }; \
	}

#define VDDFUNCRET(_fnname, _procname) void __declspec(naked) _procname ## _entry() { \
	_asm { push edi }; \
	_asm { push esi }; \
	_asm { push edx }; \
	_asm { push ecx }; /* extra_ecx */ \
	_asm { push eax }; /* extra_eax */ \
	_asm { push ebp }; /* state */ \
	_asm { push ebx }; /* vm */ \
	_asm { call _procname ## _proc }; \
	_asm { sub eax, 1 }; /* set carry on zero */ \
	_asm { pop ebx }; \
	_asm { pop ebp }; \
	_asm { pop eax }; \
	_asm { pop ecx }; \
	_asm { pop edx }; \
	_asm { pop esi }; \
	_asm { pop edi }; \
	_asm { retn }; \
	}

#include "vxd_vdd_list.h"
#undef VDDFUNC
#undef VDDFUNCRET

#define VDDFUNC(id, procname) DispatchTable[id] = (DWORD)(procname ## _entry);
#define VDDFUNCRET VDDFUNC

/* init device and fill dispatch table */
void Device_Init_proc(DWORD VM)
{
	critical_section_enter();
	dbg_printf(dbg_Device_Init_proc);

	VMMCall(_Allocate_Device_CB_Area);
 	ThisVM = VM;
 	DeviceCTX = _GetCurrentContext();

#ifdef NV_DETECT
	/* Early init runs inside a VMM critical section; a BAR0 MMIO read here
	 * hard-freezes the G86. Make every NV_probe below decode the chip from the
	 * PCI device id instead, and defer real MMIO to the Ring-3 path. */
	nv_probe_skip_mmio = 1;
#endif

#ifdef SVGA
	gpu_allocated = vxd_hinit();
	SVGA_init_hw();
#endif

#ifdef VBE
	VBE_init_hw();
#endif

#ifdef VESA
	VESA_init_hw();
#endif

#ifdef NV_DETECT
	NV_probe(&nv_hwinfo);
	nv_probe_skip_mmio = 0;   /* back to real MMIO for the Ring-3 path */
	/* NOTE: do NOT write the VRAM boot log here - we are still inside the
	 * Device_Init critical section, where a BAR1 write hard-freezes the G86
	 * (3025). The boot-handshake trace is emitted from register_display_driver,
	 * which runs later in live VM context. */
#endif

#if defined(VESA)
	Hook_V86_Int_Chain(0x10, ((DWORD)virtual_int_10h)+8);
#elif defined(QEMU)
	Install_IO_Handler(0x1ce, (DWORD)virtual_0x1ce);
	Disable_Global_Trapping(0x1ce);
	Install_IO_Handler(0x1cf, (DWORD)virtual_0x1ce);
	Disable_Global_Trapping(0x1cf);
	
	is_qemu = TRUE;
#elif defined(SVGA)
	if(!SVGA_HasFIFOCap(SVGA_FIFO_CAP_FENCE)) /* no SVGA_FIFO_CAP_FENCE => QEMU */
	{
		Install_IO_Handler(0x1ce, (DWORD)virtual_0x1ce);
		Disable_Global_Trapping(0x1ce);
		Install_IO_Handler(0x1cf, (DWORD)virtual_0x1ce);
		Disable_Global_Trapping(0x1cf);
		
		is_qemu = TRUE;
	}
#endif

	/* register miniVDD functions */
	VDD_Get_Mini_Dispatch_Table();
	dbg_printf("VDD mini dispatch table: ptr=%lX len=%lX (%s)\n", (DWORD)DispatchTable,
		(DWORD)DispatchTableLength, DispatchTableLength >= 0x31 ? "handlers installed" : "TOO SHORT - handlers NOT installed");
	if(DispatchTableLength >= 0x31)
	{
		#include "vxd_vdd_list.h"
	}
	if(DispatchTableLength > SET_ADAPTER_POWER_STATE)
	{
		/* M4.5: C-callable entry, no register stub (see set_adapter_power_state_proc) */
		DispatchTable[SET_ADAPTER_POWER_STATE] = (DWORD)set_adapter_power_state_proc;
		dbg_printf("VDD: SET_ADAPTER_POWER_STATE hook installed\n");
	}

	configure_FBHDA();
#ifdef SVGA
	vxd_hstats_update();
#endif

	critical_section_leave();
	dbg_printf("Device_Init_proc DONE\n");
	dbg_printf("Address=0x%lX\n", port_info);
}
#undef VDDFUNC
#undef VDDNAKED

static DWORD io_open_cnt = 0;

BOOL FBHDA_page_modify(DWORD flat_address, DWORD size, const BYTE *new_data)
{
	DWORD page = _PAGE(flat_address);
	DWORD pages = 1;
	if(_PAGE(flat_address+size) != page)
	{
		pages++;
	}
	
	_PageModifyPermissions(page, pages, PC_USER | PC_WRITEABLE, PC_WRITEABLE);
	
	memcpy((void*)flat_address, new_data, size);
	
	return TRUE;
}

#ifdef NV_DETECT
/* 3047 diagnostics: log the first NV_IOCTL_TRACE_N calls of every distinct
 * ring-3 ioctl op (DirectDraw HAL / Mesa / nvprobe) with their first input
 * dwords, so that "nvprobe tail" shows which HAL request preceded a crash.
 * Housekeeping ops (open/close, the log reader itself) are skipped. */
#define NV_IOCTL_TRACE_SLOTS 48
#define NV_IOCTL_TRACE_N     6
static DWORD nv_iot_op[NV_IOCTL_TRACE_SLOTS];
static DWORD nv_iot_cnt[NV_IOCTL_TRACE_SLOTS];
static DWORD nv_iot_used = 0;
DWORD nv_ioctl_total = 0;
/* 3048: the same for the 16-bit driver's PM16 services */
static DWORD nv_pmt_op[NV_IOCTL_TRACE_SLOTS];
static DWORD nv_pmt_cnt[NV_IOCTL_TRACE_SLOTS];
static DWORD nv_pmt_used = 0;
static DWORD nv_trace_w = 0, nv_trace_h = 0, nv_trace_bpp = 0;

/* 3048: both traces start over whenever the mode changes, so the calls that
 * follow a game's mode switch are visible even for ops already seen. */
static void nv_trace_check_mode(void)
{
	if(hda == NULL) return;
	if(hda->width != nv_trace_w || hda->height != nv_trace_h || hda->bpp != nv_trace_bpp)
	{
		nv_trace_w = hda->width; nv_trace_h = hda->height; nv_trace_bpp = hda->bpp;
		nv_iot_used = 0;
		nv_pmt_used = 0;
		dbg_printf("trace reset: mode %lux%lux%lu\n", nv_trace_w, nv_trace_h, nv_trace_bpp);
	}
}

static void nv_pm16_trace(DWORD service, PCRS_32 state)
{
	DWORD slot;
	if(service == OP_NV_DBG_PUTC) return;
	nv_trace_check_mode();
	for(slot = 0; slot < nv_pmt_used; slot++)
	{
		if(nv_pmt_op[slot] == service) break;
	}
	if(slot == nv_pmt_used)
	{
		if(nv_pmt_used >= NV_IOCTL_TRACE_SLOTS) return;
		nv_pmt_op[slot]  = service;
		nv_pmt_cnt[slot] = 0;
		nv_pmt_used++;
	}
	nv_pmt_cnt[slot]++;
	if(nv_pmt_cnt[slot] > NV_IOCTL_TRACE_N) return;
	dbg_printf("PM16 %lX #%lu ecx=%lX ebx=%lX esi=%lX edi=%lX\n", service, nv_pmt_cnt[slot],
		state->Client_ECX, state->Client_EBX, state->Client_ESI, state->Client_EDI);
}

static void nv_ioctl_trace(struct DIOCParams *params)
{
	DWORD op = params->dwIoControlCode;
	DWORD slot;
	DWORD i;
	DWORD n;
	DWORD a[4];
	const DWORD *in = (const DWORD*)params->lpInBuffer;

	nv_ioctl_total++;
	if(op == DIOC_OPEN || op == DIOC_CLOSEHANDLE || op == OP_NV_DBGLOG || op == OP_NV_DBGLOG2) return;
	nv_trace_check_mode();

	for(slot = 0; slot < nv_iot_used; slot++)
	{
		if(nv_iot_op[slot] == op) break;
	}
	if(slot == nv_iot_used)
	{
		if(nv_iot_used >= NV_IOCTL_TRACE_SLOTS) return;
		nv_iot_op[slot]  = op;
		nv_iot_cnt[slot] = 0;
		nv_iot_used++;
	}
	nv_iot_cnt[slot]++;
	if(nv_iot_cnt[slot] > NV_IOCTL_TRACE_N) return;

	a[0] = a[1] = a[2] = a[3] = 0;
	n = params->cbInBuffer / 4;
	if(n > 4) n = 4;
	if(in == NULL) n = 0;
	for(i = 0; i < n; i++) a[i] = in[i];
	dbg_printf("IOCTL %lX #%lu in=%lu [%lX %lX %lX %lX] out=%lu\n",
		op, nv_iot_cnt[slot], params->cbInBuffer,
		a[0], a[1], a[2], a[3], params->cbOutBuffer);
}
#endif

/* process user space (PM32, RING-3) call */
DWORD __stdcall Device_IO_Control_proc(DWORD vmhandle, struct DIOCParams *params)
{
	DWORD *inBuf  = (DWORD*)params->lpInBuffer;
	DWORD *outBuf = (DWORD*)params->lpOutBuffer;
	DWORD rc = 1;
	
	//Begin_Critical_Section(0);
	
	//dbg_printf("I%x\n", params->dwIoControlCode);
#ifdef NV_DETECT
	nv_ioctl_trace(params);
#endif
	
	switch(params->dwIoControlCode)
	{
		/* DX */
		case DIOC_OPEN:
			dbg_printf("DIOC_OPEN\n");
			io_open_cnt++;
			rc = 0;
			break;
		case DIOC_CLOSEHANDLE:
			io_open_cnt--;
#ifdef SVGA
			if(io_open_cnt == 0)
			{
				// JH: too aggressive - cause problems
				//dbg_printf("Zero reference, do ALL cleanup!\n");
				//SVGA_AllProcessCleanup();
			}
#endif
			rc = 0;
			break;
		case OP_FBHDA_SETUP:
#ifdef NV_DETECT
			nv_dbg_fbhda++;   /* only nvmini.drv issues this; vga.drv never does */
#endif
			outBuf[0] = (DWORD)FBHDA_setup();
			rc = 0;
			break;
		case OP_FBHDA_ACCESS_BEGIN:
			FBHDA_access_begin(inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_ACCESS_END:
			FBHDA_access_end(inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_ACCESS_RECT:
			FBHDA_access_rect(inBuf[0], inBuf[1], inBuf[2], inBuf[3]);
			rc = 0;
			break;
		case OP_FBHDA_SWAP:
			outBuf[0] = FBHDA_swap(inBuf[0], inBuf[1]);
			rc = 0;
			break;
		case OP_FBHDA_CLEAN:
			FBHDA_clean();
			rc = 0;
			break;
		case OP_FBHDA_PALETTE_SET:
			FBHDA_palette_set(inBuf[0], inBuf[1]);
			rc = 0;
			break;
		case OP_FBHDA_PALETTE_GET:
			outBuf[0] = FBHDA_palette_get(inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_OVERLAY_SETUP:
			outBuf[0] = FBHDA_overlay_setup(inBuf[0], inBuf[1], inBuf[2], inBuf[3]);
			rc = 0;
			break;
		case OP_FBHDA_OVERLAY_LOCK:
			FBHDA_overlay_lock(inBuf[0], inBuf[1], inBuf[2], inBuf[3]);
			rc = 0;
			break;
		case OP_FBHDA_OVERLAY_UNLOCK:
			FBHDA_overlay_unlock(inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_MODE_QUERY:
#ifdef VESA
			/* currently only for VESA */
			outBuf[0] = FBHDA_mode_query(inBuf[0], (FBHDA_mode_t*)(&outBuf[1]));
#else
			outBuf[0] = FALSE;
#endif
			rc = 0;
			break;
		case OP_FBHDA_REFRESH:
			FBHDA_refresh(inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_GET:
			outBuf[0] = FBHDA_DD_surface_get((void*)inBuf[0], (FBHDA_DD_surface_t*)inBuf[1]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_SET:
			outBuf[0] = FBHDA_DD_surface_set((void*)inBuf[0], (FBHDA_DD_surface_t*)inBuf[1]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_DELETE:
			FBHDA_DD_surface_delete((void*)inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_MODIFY:
			outBuf[0] = FBHDA_DD_surface_modify((void*)inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_NOTIFY:
			FBHDA_DD_surface_notify((void*)inBuf[0]);
			rc = 0;
			break;
		case OP_FBHDA_SURFACE_WATCH:
			outBuf[0] = FBHDA_DD_surface_watch((void*)inBuf[0], (FBHDA_DD_watch_callback_t)inBuf[1]);
			rc = 0;
			break;
#ifdef SVGA
		case OP_SVGA_VALID:
			outBuf[0] = SVGA_valid();
			rc = 0;
			break;
		case OP_SVGA_CMB_ALLOC:
			outBuf[0] = (DWORD)SVGA_CMB_alloc();
			rc = 0;
			break;
		case OP_SVGA_CMB_FREE:
			SVGA_CMB_free((DWORD*)inBuf[0]);
			rc = 0;
			break;
		case OP_SVGA_CMB_SUBMIT:
		{
				SVGA_CMB_submit_io_t *inio  = (SVGA_CMB_submit_io_t*)inBuf;
				SVGA_CMB_status_t *status = (SVGA_CMB_status_t*)outBuf;
				
				SVGA_CMB_submit(inio->cmb, inio->cmb_size, status, inio->flags, inio->DXCtxId);
				rc = 0;
				break;
		}
		case OP_SVGA_FENCE_GET:
			outBuf[0] = SVGA_fence_get();
			rc = 0;
			break;
		case OP_SVGA_FENCE_QUERY:
			SVGA_fence_query(&outBuf[0], &outBuf[1]);
			rc = 0;
			break;
		case OP_SVGA_FENCE_WAIT:
			SVGA_fence_wait(inBuf[0]);
			rc = 0;
			break;
		case OP_SVGA_REGION_CREATE:
		{
			SVGA_region_info_t *inio  = (SVGA_region_info_t*)inBuf;
			SVGA_region_info_t *outio = (SVGA_region_info_t*)outBuf;
			if(outio != inio)
			{
				memcpy(outio, inio, sizeof(SVGA_region_info_t));
			}
			//outio->address = NULL;
			if(!SVGA_region_create(outio))
			{
				outio->address = NULL;
				//SVGA_flushcache();
				//SVGA_region_create(outio);
				// FIXME: return 1?
			}
			rc = 0;
			break;
		}
		case OP_SVGA_REGION_FREE:
			{
				SVGA_region_info_t *inio  = (SVGA_region_info_t*)inBuf;
				SVGA_region_free(inio);
				rc = 0;
				break;
			}
		case OP_SVGA_QUERY:
			outBuf[0] = SVGA_query(inBuf[0], inBuf[1]);
			rc = 0;
			break;
		case OP_SVGA_QUERY_VECTOR:
			SVGA_query_vector(inBuf[0], inBuf[1], inBuf[2], outBuf);
			rc = 0;
			break;
		case OP_SVGA_DB_SETUP:
			outBuf[0] = (DWORD)SVGA_DB_setup();
			rc = 0;
			break;
		case OP_SVGA_OT_SETUP:
			outBuf[0] = (DWORD)SVGA_OT_setup();
			rc = 0;
			break;
		case OP_SVGA_FLUSHCACHE:
			//SVGA_flushcache();
			rc = 0;
			break;
		case OP_SVGA_VXDCMD:
			outBuf[0] = (DWORD)SVGA_vxdcmd(inBuf[0], inBuf[1]);
			rc = 0;
			break;
#ifdef DBGPRINT
		/* export some mouse function for debuging */
		case OP_MOUSE_MOVE:
			mouse_move(inBuf[0], inBuf[1]);
			rc = 0;
			break;
		case OP_MOUSE_SHOW:
			mouse_show();
			rc = 0;
			break;
		case OP_MOUSE_HIDE:
			mouse_hide();
			rc = 0;
			break;
#endif /*DBGPRINT */
#endif /* SVGA */
		case OP_FBHDA_PAGE_MOD:
			FBHDA_page_modify(inBuf[0], inBuf[1], (BYTE *)&inBuf[2]);
			rc = 0;
			break;
#ifdef DBGRING
		case OP_NV_DBGLOG:
		{
			/* "nvprobe log": hand the ring to user space, oldest byte first */
			DWORD n = dbg_ring_read((char*)outBuf, params->cbOutBuffer);
			if(params->lpcbBytesReturned)
				*(DWORD*)params->lpcbBytesReturned = n;
			rc = 0;
			break;
		}
		case OP_NV_DBGLOG2:
		{
			/* "nvprobe tail": total written + ring, see dbg_ring_read2 */
			DWORD n = dbg_ring_read2((char*)outBuf, params->cbOutBuffer);
			if(params->lpcbBytesReturned)
				*(DWORD*)params->lpcbBytesReturned = n;
			rc = 0;
			break;
		}
#endif
#ifdef NV_DETECT
		case OP_NV_MTRR:
		{
			/* M4.6: dump the variable-range MTRRs (rdmsr, ring 0) so the user can
			 * verify the aperture really is write-combined, plus blit counters. */
			DWORD *o = (DWORD*)outBuf;
			DWORD n, i;
			if(params->cbOutBuffer < NV_MTRR_OUT_DWORDS * 4)
			{
				rc = 1;
				break;
			}
			memset(o, 0, NV_MTRR_OUT_DWORDS * 4);
			nv_rdmsr(0xFEUL,  &o[0], &o[1]);     /* IA32_MTRRCAP      */
			nv_rdmsr(0x2FFUL, &o[2], &o[3]);     /* IA32_MTRR_DEF_TYPE */
			n = o[0] & 0xFFUL;
			if(n > 8) n = 8;
			for(i = 0; i < n; i++)
			{
				nv_rdmsr(0x200UL + i*2,     &o[4 + i*4 + 0], &o[4 + i*4 + 1]);
				nv_rdmsr(0x200UL + i*2 + 1, &o[4 + i*4 + 2], &o[4 + i*4 + 3]);
			}
			o[36] = nv_mtrr_rc;
			o[37] = nv_mtrr_base;
			o[38] = nv_mtrr_size;
			wram_stats(&o[40]);
			o[48] = nv_pat_state;
			o[49] = nv_pat_pages;
			o[50] = nv_pat_bad;
			if(nv_cpuid1_edx() & 0x10000UL)
				nv_rdmsr(0x277UL, &o[51], &o[52]);   /* IA32_PAT */
			if(params->lpcbBytesReturned)
				*(DWORD*)params->lpcbBytesReturned = NV_MTRR_OUT_DWORDS * 4;
			rc = 0;
			break;
		}
		case OP_NV_PROBE:
			/* refresh the probe and hand the struct back to user space */
			NV_probe(&nv_hwinfo);
			NV_vram_export(&nv_hwinfo);   /* NV_probe zeroes the M4.3 fields */
			memcpy(outBuf, &nv_hwinfo, sizeof(nv_hwinfo_t));
			rc = 0;
			break;
		case OP_NV_RD32:
		{
			/* inBuf[0] = byte offset into BAR0, inBuf[1] = dword count.
			 * Returns the dwords in outBuf. Read-only, bounds-checked. */
			DWORD start = inBuf[0];
			DWORD count = inBuf[1];
			DWORD i;
			if(count == 0) count = 1;
			if(count > NV_RD32_MAX) count = NV_RD32_MAX;
			for(i = 0; i < count; i++)
				outBuf[i] = NV_rd32(start + i * 4);
			rc = 0;
			break;
		}
		case OP_NV_VRAM_RD:
		{
			/* inBuf[0] = byte offset into BAR1 VRAM, inBuf[1] = dword count. */
			DWORD start = inBuf[0];
			DWORD count = inBuf[1];
			DWORD i;
			if(count == 0) count = 1;
			if(count > NV_RD32_MAX) count = NV_RD32_MAX;
			for(i = 0; i < count; i++)
				outBuf[i] = NV_vram_rd32(start + i * 4);
			rc = 0;
			break;
		}
		case OP_NV_VRAM_FILL:
			/* inBuf[0]=VRAM byte offset, inBuf[1]=dword count, inBuf[2]=value */
			outBuf[0] = NV_vram_fill(inBuf[0], inBuf[1], inBuf[2]);
			rc = 0;
			break;
		case OP_NV_EVO_STAGE:
		{
			/* E2: build fb + pushbuffer + test pattern in VRAM (safe). */
			nv_evo_layout_t lay;
			NV_evo_stage(&lay);
			memcpy(outBuf, &lay, sizeof(lay));
			rc = 0;
			break;
		}
		case OP_NV_GPU_CTXTEST:
		{
			nv_gpu_ctxtest_t ct;
			NV_gpu_ctxtest(&ct);
			memcpy(outBuf, &ct, sizeof(ct));
			rc = 0;
			break;
		}
		case OP_NV_GPU_GRINIT:
		{
			/* M5.1b: PGRAPH init + context-microcode upload (opt-in, GPU accel
			 * bring-up). Touches PGRAPH only; recover a fault with a cold boot. */
			nv_gpu_grinit_t gi;
			NV_gpu_grinit(&gi);
			memcpy(outBuf, &gi, sizeof(gi));
			rc = 0;
			break;
		}
		case OP_NV_EVO_FLIP:
		{
			/* E3: bring up EVO + flip scanout to our framebuffer (RISKY).
			 * Opt-in only (never Device_Init); self-diagnosing; recover from
			 * a bad flip by rebooting into Safe Mode -> Standard VGA. */
			nv_evo_flip_t flip;
			NV_evo_flip(&flip);
			memcpy(outBuf, &flip, sizeof(flip));
			rc = 0;
			break;
		}
		case OP_NV_LASTSET:
		{
			/* E4: diagnostics of the last display-driver native mode set
			 * (survives the attempt; readable from the VGA fallback). */
			nv_evo_flip_t diag;
			DWORD cnts[16];
			NV_native_lastdiag(&diag, cnts);
			memcpy(outBuf, &diag, sizeof(diag));
			memcpy((BYTE*)outBuf + sizeof(diag), cnts, sizeof(cnts));
			rc = 0;
			break;
		}
		case OP_NV_BOOTSIM:
		{
			/* E4: run the EXACT takeover the boot display driver runs
			 * (NV_native_modeset: fb offset 0, no pattern), but on demand
			 * from a safe Standard-VGA desktop. The VxD is dynamically
			 * loaded by nvprobe, so this needs neither nvmini bound nor a
			 * reboot - it reproduces the boot hang recoverably and logs the
			 * same 0x2xx markers. Returns the same diag blob as LASTSET. */
			nv_evo_flip_t diag;
			DWORD cnts[16];
			NV_blog_reset();      /* clean log for this on-demand run */
			NV_native_modeset(1024UL, 768UL, 16UL);
			NV_native_lastdiag(&diag, cnts);
			memcpy(outBuf, &diag, sizeof(diag));
			memcpy((BYTE*)outBuf + sizeof(diag), cnts, sizeof(cnts));
			rc = 0;
			break;
		}
		case OP_NV_INITSIM:
		{
			/* E4: run the EXACT backend init the boot display driver runs
			 * (NV_init_hw_native: 12MB BAR1 map, 64MB wram, FBHDA, blit
			 * timer), on demand from a safe desktop. Logs 0x100..0x107;
			 * outBuf[0] = the BOOL it returned (reached the end). If the
			 * boot hang is in the backend it reproduces it recoverably. */
			outBuf[0] = (DWORD)NV_initsim();
			rc = 0;
			break;
		}
		case OP_NV_SETMODESIM:
		{
			/* E4: run the EXACT display-driver setmode hook (VESA_setmode_phy
			 * 1024x768x16) on demand - the 0x210..0x212 path, post-modeset
			 * wblit/wram reconfig and full-frame blit. Call after initsim.
			 * outBuf[0] = the BOOL it returned. */
			outBuf[0] = (DWORD)NV_setmodesim();
			rc = 0;
			break;
		}
#endif
		default:
			dbg_printf("DeviceIOControl: Unknown: %d\n", params->dwIoControlCode);
			break;
	}
	
	//dbg_printf("IL\n");
	//End_Critical_Section();

	return rc;
}

void Device_Init_Complete(DWORD VM)
{
#if defined(QEMU) || defined(SVGA)
/*
 * At Windows shutdown time, the display driver calls VDD_DRIVER_UNREGISTER,
 * which calls our DisplayDriverDisabling callback, and soon thereafter
 * does an INT 10h (AH=0) to mode 3 (and then 13) in V86 mode. However, the
 * memory ranges for display memory are not always mapped!
 *
 * If the VGA ROM BIOS, during execution of such a set video mode call, tries to
 * clear the screen, and the appropriate memory range isn't mapped, then we end
 * up in a page fault handler, which I guess maps the memory and then resumes
 * execution in V86 mode. But strangely, in QEMU, this fault & resume mechanism
 * does not work with KVM or WHPX assist, at least on Intel. The machine hangs
 * instead (I have not root caused why).
 *
 * We can dodge this problem by setting up real mappings up front.
 *
 * At entry, EBX contains a Windows VM handle.
 *
 */
	if(is_qemu)
	{
		_PhysIntoV86(0xA0, VM, 0xA0, 16, 0);
		_PhysIntoV86(0xB8, VM, 0xB8, 8, 0);
	}
#endif
}

/* shutdown procedure */
void Device_Exit_proc(DWORD VM)
{
#ifdef SVGA
	SVGA_HW_disable();
#endif

	dbg_printf(dbg_destroy);
	
	// TODO: free memory
}

static DWORD __stdcall service_get_ver()
{
	return API_3DACCEL_VER;
}

static FBHDA_t *__stdcall service_setup()
{
	return FBHDA_setup();
}

static DWORD __stdcall service_overlay_setup(DWORD overlay, DWORD width, DWORD height, DWORD bpp)
{
	return FBHDA_overlay_setup(overlay, width, height, bpp);
}

static void __stdcall service_overlay_lock(DWORD left, DWORD top, DWORD right, DWORD bottom)
{
	FBHDA_overlay_lock(left, top, right, bottom);
}

static void __stdcall service_overlay_unlock(DWORD flags)
{
	FBHDA_overlay_unlock(flags);
}

DWORD service_table[SERVICE_TABLE_CNT] = {
	(DWORD)&service_get_ver, // 0
	(DWORD)&service_setup, // 1
	0, // 2
	0, // 3
	0, // 4
	0, // 5
	0, // 6
	0, // 7
	0, // 8
	(DWORD)&service_overlay_setup, // 9
	(DWORD)&service_overlay_lock, // 10
	(DWORD)&service_overlay_unlock, // 11
};

void __stdcall win32_destroy_process_proc(DWORD pid)
{
	dbg_printf("DESTROY_PROCESS: %lX\n", pid);
	
#ifdef SVGA
	SVGA_ProcessCleanup(pid);
#endif
}
