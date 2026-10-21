/*
 * nvprobe.c - Win9x user-space reader for the NVIDIA G86 detection (M2).
 *
 * Opens the nvmini VxD and issues OP_NV_PROBE, then prints the decoded chip
 * info. This is how we read the result on a laptop with no serial port.
 *
 * Build (Open Watcom): wcl386 -bt=nt -l=nt -fe=nvprobe.exe tools/nvprobe.c
 * Run on the T61 (after the nvmini display driver is installed and active):
 *   nvprobe
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../vxd_nv.h"
#include "../dbgring.h"
#include "../nv50_evo.h"

/* Emit the EVO flip diagnostic to a stream. Called twice: once to nvflip.txt
 * (so the result survives even if the flip black-screens the console) and once
 * to stdout (readable when the screen survives). */
static void print_flip(FILE *o, const nv_evo_flip_t *f)
{
	fprintf(o, "EVO flip PIO (vxd build %lu): ok=%lu stage=%lu\n",
		f->build, f->ok, f->stage);
	fprintf(o, "  built   : ramin=%08lX ctxdma=%08lX fb=%08lX\n",
		f->ramin, f->ctxdma, f->fb_offset);
	fprintf(o, "            ctxdma.flags0=%08lX  ramht[%08lX]=handle %08lX ctx %08lX\n",
		f->ctxdma_flags0, f->ramht_slot, f->iso_handle, f->ramht_ctx);
	fprintf(o, "            objects: pre=%08lX wrote=%08lX post=%08lX\n",
		f->pre_objects, f->objects_val, f->post_objects);
	fprintf(o, "  preamble: cursor0 %08lX->%08lX cursor1=%08lX cur_loops=%lu text_61e8=%08lX\n",
		f->pre_cursor0, f->cur0_state, f->cur1_state, f->cur_loops, f->text_61e8);
	fprintf(o, "  recover : pre=%08lX post=%08lX (stuck-state nudge, 0x02xxxx -> +0x800000)\n",
		f->fini_ctrl, f->fini_loops);
	fprintf(o, "  channel : ctrl=%08lX accept_loops=%lu\n",
		f->ctrl_after_init, f->accept_loops);
	fprintf(o, "  ring    : put=%08lX get=%08lX  notify %08lX->%08lX sync_loops=%lu\n",
		f->put_bytes, f->post_get, f->ntfy_pre, f->ntfy_post, f->sync_loops);
	fprintf(o, "  superv  : unk10 acked %lu, unk20 acked %lu, unk40 acked %lu\n",
		f->sv10, f->sv20, f->sv40);
	fprintf(o, "  trap    : addr=%08lX data=%08lX   intr0=%08lX intr1=%08lX\n",
		f->trapped_addr, f->trapped_data, f->intr0, f->intr1);
	/* The notifier going non-zero = the channel actually processed our ring.
	 * INTR_0 bit16 = FIFO_ERR_MASTER (a method the engine rejected). */
	if(f->cur0_state & 0x00020000UL)
		fprintf(o, "  => cursor channel never went ACTIVE (preamble stalled); engine not ready.\n");
	else if(f->intr0 & 0x00010000UL)
		fprintf(o, "  => FIFO ERROR: method 0x%04lX data %08lX (engine rejected it)\n",
			f->trapped_addr & 0xffcUL, f->trapped_data);
	else if(f->ntfy_post != 0)
		fprintf(o, "  => SUCCESS: channel processed the ring (notifier posted). Look at the panel!\n");
	else if(f->post_get == f->put_bytes)
		fprintf(o, "  => ring drained (get==put) but notifier silent; check scanout/notifier ctxdma.\n");
	else
		fprintf(o, "  => channel still did not drain (get=%08lX put=%08lX); engine init insufficient.\n",
			f->post_get, f->put_bytes);
}

int main(int argc, char **argv)
{
	nv_hwinfo_t info;
	DWORD ret = 0;
	BOOL ok;
	HANDLE vxd;

	vxd = CreateFileA("\\\\.\\nvmini.vxd", 0, 0, 0,
		CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, 0);
	if(vxd == INVALID_HANDLE_VALUE)
	{
		printf("Cannot open nvmini.vxd.\n");
		printf("Is the NVIDIA (nvmini) display driver installed and running?\n");
		return 1;
	}

	printf("nvprobe build (exe): %lu\n", (unsigned long)NV_BUILD);

	/* "nvprobe rd <hexoffset> [count]" : dump BAR0 MMIO registers (read-only). */
	if(argc >= 3 && strcmp(argv[1], "rd") == 0)
	{
		DWORD in[2];
		DWORD out[NV_RD32_MAX];
		DWORD start = (DWORD)strtoul(argv[2], NULL, 16);
		DWORD count = (argc >= 4) ? (DWORD)strtoul(argv[3], NULL, 0) : 1;
		DWORD i;

		if(count == 0) count = 1;
		if(count > NV_RD32_MAX) count = NV_RD32_MAX;
		in[0] = start;
		in[1] = count;

		if(!DeviceIoControl(vxd, OP_NV_RD32, in, sizeof(in),
			out, count * sizeof(DWORD), &ret, NULL))
		{
			printf("OP_NV_RD32 failed (error %lu)\n", GetLastError());
			CloseHandle(vxd);
			return 1;
		}
		for(i = 0; i < count; i++)
			printf("  %08lX: %08lX\n", start + i * 4, out[i]);
		CloseHandle(vxd);
		return 0;
	}

	/* "nvprobe vramrd <hexoffset> [count]" : dump VRAM (BAR1) dwords. */
	if(argc >= 3 && strcmp(argv[1], "vramrd") == 0)
	{
		DWORD in[2];
		DWORD out[NV_RD32_MAX];
		DWORD start = (DWORD)strtoul(argv[2], NULL, 16);
		DWORD count = (argc >= 4) ? (DWORD)strtoul(argv[3], NULL, 0) : 1;
		DWORD i;

		if(count == 0) count = 1;
		if(count > NV_RD32_MAX) count = NV_RD32_MAX;
		in[0] = start; in[1] = count;
		if(!DeviceIoControl(vxd, OP_NV_VRAM_RD, in, sizeof(in),
			out, count * sizeof(DWORD), &ret, NULL))
		{
			printf("OP_NV_VRAM_RD failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		for(i = 0; i < count; i++)
			printf("  VRAM+%08lX: %08lX\n", start + i * 4, out[i]);
		CloseHandle(vxd);
		return 0;
	}

	/* "nvprobe vramfill <hexoffset> <dwordcount> <hexvalue>" : fill VRAM.
	 * Memory-only test; cannot affect the raster. Reversible by reboot. */
	if(argc >= 5 && strcmp(argv[1], "vramfill") == 0)
	{
		DWORD in[3];
		DWORD out[1];
		in[0] = (DWORD)strtoul(argv[2], NULL, 16);
		in[1] = (DWORD)strtoul(argv[3], NULL, 0);
		in[2] = (DWORD)strtoul(argv[4], NULL, 16);
		out[0] = 0;
		if(!DeviceIoControl(vxd, OP_NV_VRAM_FILL, in, sizeof(in),
			out, sizeof(out), &ret, NULL))
		{
			printf("OP_NV_VRAM_FILL failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		printf("filled %lu dwords at VRAM+%08lX with %08lX\n",
			out[0], in[0], in[2]);
		CloseHandle(vxd);
		return 0;
	}

	/* "nvprobe evostage" : build the fb + pushbuffer in VRAM (safe, E2). */
	if(argc >= 2 && strcmp(argv[1], "evostage") == 0)
	{
		nv_evo_layout_t lay;
		DWORD i;
		memset(&lay, 0, sizeof(lay));
		if(!DeviceIoControl(vxd, OP_NV_EVO_STAGE, NULL, 0,
			&lay, sizeof(lay), &ret, NULL))
		{
			printf("OP_NV_EVO_STAGE failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		printf("EVO staged (ok=%lu)\n", lay.ok);
		printf("  framebuffer : VRAM+%08lX  %lux%lu %lubpp pitch %lu\n",
			lay.fb_offset, lay.fb_width, lay.fb_height, lay.fb_bpp, lay.fb_pitch);
		printf("  pushbuffer  : VRAM+%08lX  %lu dwords, first method %08lX\n",
			lay.push_offset, lay.push_dwords, lay.first_method);
		printf("  notifier    : VRAM+%08lX   iso handle %08lX\n",
			lay.notify_offset, lay.iso_handle);
		printf("  (read it back with: nvprobe vramrd %lX %lu)\n",
			lay.push_offset, lay.push_dwords);
		CloseHandle(vxd);
		return 0;
	}

	/* "nvprobe evoflip go" : E3 - bring up EVO and flip scanout to our own
	 * framebuffer. RISKY: may black-screen; recover by rebooting into Safe Mode
	 * (F8) -> Standard VGA. Requires the literal word "go" so it is never run by
	 * accident. Prints the full before/after diagnostic snapshot either way. */
	if(argc >= 2 && strcmp(argv[1], "evoflip") == 0)
	{
		nv_evo_flip_t f;
		if(argc < 3 || strcmp(argv[2], "go") != 0)
		{
			printf("evoflip performs the RISKY scanout flip (E3).\n");
			printf("It may black-screen the panel; recovery is reboot -> Safe Mode\n");
			printf("(hold F8 at boot) -> Standard VGA. To proceed, run:\n");
			printf("    nvprobe evoflip go\n");
			CloseHandle(vxd);
			return 0;
		}
		memset(&f, 0, sizeof(f));
		if(!DeviceIoControl(vxd, OP_NV_EVO_FLIP, NULL, 0,
			&f, sizeof(f), &ret, NULL))
		{
			printf("OP_NV_EVO_FLIP failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);

		/* Save to a file FIRST - the flip may have blanked/garbaged the
		 * console, in which case the stdout copy below is unreadable, but the
		 * file survives the reboot. */
		{
			FILE *fp = fopen("nvflip.txt", "w");
			if(fp)
			{
				print_flip(fp, &f);
				fflush(fp);
				fclose(fp);
			}
		}
		print_flip(stdout, &f);
		printf("(diagnostic also written to nvflip.txt - bring that back)\n");
		return 0;
	}

	/* "nvprobe bootsim [go]" : run the EXACT takeover the boot display driver
	 * runs (NV_native_modeset, 1024x768x16, fb offset 0, no pattern) on demand.
	 * Reproduces the boot hang from a safe desktop; if it hangs, warm-reset and
	 * read the 0x1900000 log. Needs "go" to actually fire (it IS the risky op). */
	if(argc >= 2 && strcmp(argv[1], "bootsim") == 0)
	{
		struct { nv_evo_flip_t f; DWORD cnts[16]; } d;
		if(argc < 3 || strcmp(argv[2], "go") != 0)
		{
			printf("bootsim runs the boot-path native mode set (RISKY: may hang).\n");
			printf("Recovery: warm-reset, then 'nvprobe vramrd 0x1900000 48'.\n");
			printf("Re-run as: nvprobe bootsim go\n");
			CloseHandle(vxd);
			return 0;
		}
		memset(&d, 0, sizeof(d));
		if(!DeviceIoControl(vxd, OP_NV_BOOTSIM, NULL, 0,
			&d, sizeof(d), &ret, NULL))
		{
			printf("OP_NV_BOOTSIM failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("bootsim returned (did NOT hang).\n");
		printf("native-path counters: valid=%lu set=%lu ok=%lu  last req %lux%lux%lu\n",
			d.cnts[0], d.cnts[1], d.cnts[2], d.cnts[3], d.cnts[4], d.cnts[5]);
		print_flip(stdout, &d.f);
		/* The screen is now garbage (scanout moved to VRAM 0), so also write
		 * the result to disk where it can be read after a warm-reset + VGA. */
		{
			FILE *fp = fopen("nvboot.txt", "w");
			if(fp)
			{
				fprintf(fp, "bootsim returned (did NOT hang). build=%lu\n", (unsigned long)NV_BUILD);
				fprintf(fp, "counters: valid=%lu set=%lu ok=%lu last %lux%lux%lu\n",
					d.cnts[0], d.cnts[1], d.cnts[2], d.cnts[3], d.cnts[4], d.cnts[5]);
				print_flip(fp, &d.f);
				fclose(fp);
			}
		}
		return 0;
	}

	/* "nvprobe initsim [go]" : run the EXACT backend init the boot display
	 * driver runs (12MB BAR1 map, 64MB wram, FBHDA, blit timer). Reproduces a
	 * backend boot hang from a safe desktop. The VGA desktop stays readable
	 * (the blit timer writes to VRAM, not VGA memory), so output is visible. */
	if(argc >= 2 && strcmp(argv[1], "initsim") == 0)
	{
		DWORD out = 0;
		if(argc < 3 || strcmp(argv[2], "go") != 0)
		{
			printf("initsim runs the boot-path backend init (may hang).\n");
			printf("Recovery: warm-reset, then 'nvprobe vramrd 0x1900000 48'.\n");
			printf("Re-run as: nvprobe initsim go\n");
			CloseHandle(vxd);
			return 0;
		}
		if(!DeviceIoControl(vxd, OP_NV_INITSIM, NULL, 0,
			&out, sizeof(out), &ret, NULL))
		{
			printf("OP_NV_INITSIM failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("initsim returned (did NOT hang): NV_init_hw_native() = %lu\n", out);
		printf("markers reached: run 'nvprobe vramrd 0x1900000 48' (expect up to 00000107).\n");
		return 0;
	}

	/* "nvprobe setmodesim [go]" : run the EXACT display-driver setmode hook
	 * (VESA_setmode_phy 1024x768x16) on demand. MUST be preceded by a
	 * 'nvprobe initsim go' in the same (resident) VxD session. Reproduces the
	 * hard boot hang recoverably if it is in the setmode-hook context. */
	if(argc >= 2 && strcmp(argv[1], "setmodesim") == 0)
	{
		DWORD out = 0;
		if(argc < 3 || strcmp(argv[2], "go") != 0)
		{
			printf("setmodesim runs the DRV setmode hook (may hard-hang).\n");
			printf("Run 'nvprobe initsim go' FIRST, then 'nvprobe setmodesim go'.\n");
			printf("Recovery: power-cycle -> Safe Mode -> Standard VGA.\n");
			CloseHandle(vxd);
			return 0;
		}
		if(!DeviceIoControl(vxd, OP_NV_SETMODESIM, NULL, 0,
			&out, sizeof(out), &ret, NULL))
		{
			printf("OP_NV_SETMODESIM failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("setmodesim returned (did NOT hang): VESA_setmode_phy() = %lu\n", out);
		return 0;
	}

	/* "nvprobe log" : dump the VxD's in-memory dbg_printf ring (dbgring.h).
	 * Holds both the VxD's lines ("V> ") and nvmini.drv's ("D> "), in order,
	 * since the VxD was loaded. Read-only. */
	if(argc >= 2 && strcmp(argv[1], "log") == 0)
	{
		static char buf[DBG_RING_SIZE + 16];
		memset(buf, 0, sizeof(buf));
		if(!DeviceIoControl(vxd, OP_NV_DBGLOG, NULL, 0,
			buf, DBG_RING_SIZE, &ret, NULL))
		{
			printf("OP_NV_DBGLOG failed (error %lu) - VxD built without DBGRING?\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		buf[ret] = 0;
		fwrite(buf, 1, ret, stdout);
		printf("\n[nvprobe log: %lu bytes]\n", ret);
		return 0;
	}

	/* "nvprobe lastset" : diagnostics of the last native mode set performed by
	 * the display driver (e.g. a failed boot-time attempt). Read-only. */
	if(argc >= 2 && strcmp(argv[1], "lastset") == 0)
	{
		struct { nv_evo_flip_t f; DWORD cnts[16]; } d;
		memset(&d, 0, sizeof(d));
		if(!DeviceIoControl(vxd, OP_NV_LASTSET, NULL, 0,
			&d, sizeof(d), &ret, NULL))
		{
			printf("OP_NV_LASTSET failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("native-path counters: valid=%lu set=%lu ok=%lu  last req %lux%lux%lu\n",
			d.cnts[0], d.cnts[1], d.cnts[2], d.cnts[3], d.cnts[4], d.cnts[5]);
		printf("mode-path calls    : validmode=%lu setmode=%lu last setmode %lux%lux%lu\n",
			d.cnts[6], d.cnts[7], d.cnts[8], d.cnts[9], d.cnts[10]);
		printf("backend init       : %lu (0=none 1=VBE 2=native)  nvmini.drv FBHDA calls=%lu\n", d.cnts[13], d.cnts[14]);
		if(d.cnts[14] == 0)
			printf("=> nvmini.drv did NOT load this boot - Windows is on vga.drv (binding/primary-adapter).\n");
		else
			printf("=> nvmini.drv DID load (FBHDA calls>0) but aborted before setmode.\n");
		printf("hook branch        : %lu (0=never ran, 1=mode not native, 2=no LFB map, 3=native tried)"
			"  phylin=%08lX\n", d.cnts[11], d.cnts[12]);
		if(d.cnts[7] == 0)
			printf("=> the display driver NEVER called the VxD mode-set this boot (binding/drv issue).\n");
		if(d.cnts[1] == 0)
		{
			printf("no native mode set was attempted since boot.\n");
			return 0;
		}
		print_flip(stdout, &d.f);
		{
			FILE *fp = fopen("nvlast.txt", "w");
			if(fp)
			{
				fprintf(fp, "counters: valid=%lu set=%lu ok=%lu last %lux%lux%lu\n",
					d.cnts[0], d.cnts[1], d.cnts[2], d.cnts[3], d.cnts[4], d.cnts[5]);
				print_flip(fp, &d.f);
				fclose(fp);
				printf("(also written to nvlast.txt)\n");
			}
		}
		return 0;
	}

	memset(&info, 0, sizeof(info));
	ok = DeviceIoControl(vxd, OP_NV_PROBE, NULL, 0,
		&info, sizeof(info), &ret, NULL);
	CloseHandle(vxd);

	if(!ok)
	{
		printf("OP_NV_PROBE failed (error %lu)\n", GetLastError());
		return 1;
	}

	printf("NVIDIA G86 probe result\n");
	printf("  build     : vxd=%lu  exe=%lu%s\n",
		info.build, (unsigned long)NV_BUILD,
		(info.build == NV_BUILD) ? "" : "   *** VERSION SKEW ***");
	printf("  scan      : %lu PCI functions seen, host0(0:0.0)=%08lX\n",
		info.scan_devices, info.host0);
	printf("  NVIDIA    : %lu function(s); first devid=%04lX class/rev=%08lX bdf=%06lX\n",
		info.nv_devices, info.first_nv_devid, info.first_nv_class, info.first_nv_bdf);
	printf("  found     : %lu\n", info.found);
	if(!info.found)
	{
		if(info.scan_devices == 0 || info.host0 == 0xFFFFFFFFUL)
			printf("  => PCI config scan did NOT reach hardware (ports 0xCF8/0xCFC).\n");
		else
			printf("  => PCI scan works (%lu devices) but no 0x10DE device was visible.\n",
				info.scan_devices);
		return 0;
	}

	printf("  PCI       : %02lu:%02lu.%lu  %04lX:%04lX  subsys %08lX\n",
		info.pci_bus, info.pci_dev, info.pci_fn,
		info.vendor_id, info.device_id, info.subsys);
	printf("  BAR0 MMIO : phys %08lX  size %08lX\n", info.bar0_phys, info.bar0_size);
	printf("  BAR1 VRAM : phys %08lX  size %08lX\n", info.bar1_phys, info.bar1_size);
	printf("  BOOT0     : %08lX\n", info.boot0);
	printf("  chipset   : 0x%02lX   family 0x%02lX   rev 0x%02lX   mmio_ok %lu\n",
		info.chipset, info.family, info.revision, info.mmio_ok);

	{
		static const char *chk[] = { "not run yet (needs one live-context entry: boot driver or a native mode set)",
			"OK", "PFB size read failed", "VRAM too small for the EVO reservation", "window overlaps the EVO reservation" };
		printf("  VRAM      : ");
		if(info.vram_size || info.vram_hi)
			printf("%lu MB (PFB 0x10020C cfg0 %08lX cfg1 %08lX)\n", (info.vram_size >> 20) + (info.vram_hi << 12), info.vram_cfg0, info.vram_cfg1);
		else
			printf("unknown\n");
		printf("  layout    : window 0-%lu MB (blitter/flip), EVO reserved 24-32 MB+32K, check: %s\n",
			info.fb_window >> 20, info.vram_check < 5 ? chk[info.vram_check] : "?");
	}
	if(info.chipset == 0x86)
		printf("  => G86 confirmed (Quadro NVS 140M family). We can talk to the chip.\n");
	else if(info.mmio_ok)
		printf("  => chipset 0x%02lX read, but expected 0x86 for this card.\n", info.chipset);
	else
		printf("  => MMIO did not return a sane BOOT0 (BAR0 map or access failed).\n");

	return 0;
}
