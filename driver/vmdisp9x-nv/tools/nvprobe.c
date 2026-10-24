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
#include <intrin.h>

#include "../vxd_nv.h"
#include "../dbgring.h"
#include "../nv50_evo.h"
#include "../nv50_gpu.h"

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

	/* "nvprobe tail <file>" : keep polling the VxD ring and append whatever
	 * is new to <file>, flushing after every write, so the VxD-side log
	 * survives a hard crash / reboot (3047). 3049: uses OP_NV_DBGLOG2, which
	 * prefixes the ring with the running byte total, so the new bytes are
	 * known exactly and an overflow between two polls is reported instead
	 * of silently dropped. Leave it running in a DOS box while starting the
	 * program under test; Ctrl+C stops it. */
	if(argc >= 3 && strcmp(argv[1], "tail") == 0)
	{
		static char cur[DBG_RING_SIZE + 16];
		DWORD prev_total = 0;
		int first = 1;
		DWORD wr;
		HANDLE f = CreateFileA(argv[2], GENERIC_WRITE, FILE_SHARE_READ, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
		if(f == INVALID_HANDLE_VALUE)
		{
			printf("cannot open %s (error %lu)\n", argv[2], GetLastError());
			CloseHandle(vxd); return 1;
		}
		SetFilePointer(f, 0, NULL, FILE_END);
		printf("tailing the VxD log to %s (Ctrl+C to stop)\n", argv[2]);
		for(;;)
		{
			DWORD total, n, delta;
			const char *ring;
			if(!DeviceIoControl(vxd, OP_NV_DBGLOG2, NULL, 0, cur, DBG_RING_SIZE + 4, &ret, NULL) || ret < 4)
			{
				printf("OP_NV_DBGLOG2 failed (error %lu) - VxD older than build 3049?\n", GetLastError());
				break;
			}
			total = *(DWORD*)cur;
			ring  = cur + 4;
			n     = ret - 4;
			if(first)
			{
				delta = n;          /* whole ring the first time */
				first = 0;
			}
			else
			{
				delta = total - prev_total;
				if(delta > n)
				{
					char msg[80];
					int l = sprintf(msg, "\r\n[nvprobe tail: %lu bytes lost]\r\n", delta - n);
					WriteFile(f, msg, l, &wr, NULL);
					delta = n;
				}
			}
			if(delta)
			{
				WriteFile(f, ring + (n - delta), delta, &wr, NULL);
				FlushFileBuffers(f);
			}
			prev_total = total;
			Sleep(20);
		}
		CloseHandle(f);
		CloseHandle(vxd);
		return 0;
	}

	/* "nvprobe mtrr" : raw variable MTRRs + MTRR.VXD result + shadow-blit
	 * statistics (M4.6). Run it twice a few seconds apart while moving windows
	 * to get cycles per blit / per pixel. */
	if(argc >= 2 && strcmp(argv[1], "mtrr") == 0)
	{
		static const char *types[] = {"UC","WC","?2","?3","WT","WP","WB","?7"};
		static const char *pat_states[] = {"not attempted","APPLIED (aperture is WC)","CPU has no PAT",
			"PTE walk failed (left uncached)","disabled by registry PAT=0","APPLIED (PAT entry 1 was already WC)"};
		DWORD o[NV_MTRR_OUT_DWORDS];
		unsigned long long px, cyc, physmask;
		DWORD n, i, physbits = 36;
		{
			int r[4];
			__cpuid(r, 0x80000000);
			if((unsigned)r[0] >= 0x80000008) { __cpuid(r, 0x80000008); physbits = r[0] & 0xFF; }
		}
		physmask = (physbits >= 64) ? ~0ULL : ((1ULL << physbits) - 1);
		memset(o, 0, sizeof(o));
		if(!DeviceIoControl(vxd, OP_NV_MTRR, NULL, 0, o, sizeof(o), &ret, NULL))
		{
			printf("OP_NV_MTRR failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("IA32_MTRRCAP     = %08lX%08lX  (vcnt=%lu, WC supported=%s)\n",
			o[1], o[0], o[0] & 0xFF, (o[0] & 0x400) ? "yes" : "no");
		printf("IA32_MTRR_DEF    = %08lX%08lX  (enable=%lu, default=%s)\n",
			o[3], o[2], (o[2] >> 11) & 1, types[o[2] & 7]);
		n = o[0] & 0xFF; if(n > 8) n = 8;
		for(i = 0; i < n; i++)
		{
			DWORD blo = o[4+i*4], bhi = o[5+i*4], mlo = o[6+i*4], mhi = o[7+i*4];
			unsigned long long mask = (((unsigned long long)mhi << 32) | mlo) & ~0xFFFULL;
			unsigned long long size = ((~mask) & physmask & ~0xFFFULL) + 1;
			if(mlo & 0x800)
				printf("  MTRR%lu: base %08lX%08lX size %llu MB type %s\n", i,
					bhi, blo & 0xFFFFF000UL, size >> 20, types[blo & 7]);
			else
				printf("  MTRR%lu: (disabled)\n", i);
		}
		printf("MTRR.VXD request : base %08lX size %lu MB -> rc %08lX (%s)\n",
			o[37], o[38] >> 20, o[36],
			o[36] == 0 ? "success" : (o[36] == 0xFFFFFFFFUL ? "not attempted" : "FAILED"));
		px  = ((unsigned long long)o[43] << 32) | o[42];
		cyc = ((unsigned long long)o[45] << 32) | o[44];
		printf("PAT              : %s; %lu pages marked, %lu bad; IA32_PAT=%08lX%08lX (entry1=%s)\n",
			o[48] < 6 ? pat_states[o[48]] : "?", o[49], o[50], o[52], o[51], types[(o[51] >> 8) & 7]);
		printf("blits: %lu (fast path %lu), pixels %llu, cycles %llu\n", o[40], o[41], px, cyc);
		if(o[40]) printf("  avg %llu cycles/blit, %llu cycles/pixel\n", cyc / o[40], px ? cyc / px : 0);
		printf("  last blit: %lu pixels, %lu cycles\n", o[46], o[47]);
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

	if(argc >= 2 && strcmp(argv[1], "submit") == 0)
	{
		/* M5.3: build a 2D fill method stream HERE in ring-3, hand it to the
		 * VxD's persistent channel, then read the result back. Proves user
		 * space can drive the GPU - the foundation the Mesa winsys needs.
		 * Two submits with different colors prove the channel persists. */
		static const unsigned long colors[2] = { 0x11223344UL, 0xaabbccddUL };
		unsigned long m[64], scratch = 0x03400000UL, scratch_vm = 0x43400000UL, in[2];
		nv_gpu_submit_t sb;
		nv_gpu_vread_t  vr;
		int pass;
		for(pass = 0; pass < 2; pass++)
		{
			unsigned long n = 0, col = colors[pass];
			m[n++] = (1UL<<18)|0x0000; m[n++] = 0xbeef502dUL;   /* SET_OBJECT 2D   */
			m[n++] = (1UL<<18)|0x0184; m[n++] = 0xbeef0003UL;   /* DMA_DST (VM)    */
			m[n++] = (1UL<<18)|0x0200; m[n++] = 0xcfUL;         /* DST_FORMAT      */
			m[n++] = (1UL<<18)|0x0204; m[n++] = 1;              /* DST_LINEAR      */
			m[n++] = (1UL<<18)|0x0214; m[n++] = 1024;           /* DST_PITCH       */
			m[n++] = (1UL<<18)|0x0218; m[n++] = 256;            /* DST_WIDTH       */
			m[n++] = (1UL<<18)|0x021c; m[n++] = 256;            /* DST_HEIGHT      */
			m[n++] = (2UL<<18)|0x0220; m[n++] = 0; m[n++] = scratch_vm; /* DST_ADDR (VM) */
			m[n++] = (1UL<<18)|0x0290; m[n++] = 0;              /* CLIP_ENABLE=0   */
			m[n++] = (1UL<<18)|0x02ac; m[n++] = 3;              /* OPERATION       */
			m[n++] = (1UL<<18)|0x0580; m[n++] = 4;              /* DRAW_SHAPE RECT */
			m[n++] = (1UL<<18)|0x0584; m[n++] = 0xcfUL;         /* DRAW_COLOR_FMT  */
			m[n++] = (1UL<<18)|0x0588; m[n++] = col;            /* DRAW_COLOR      */
			m[n++] = (4UL<<18)|0x0600; m[n++]=0; m[n++]=0; m[n++]=256; m[n++]=256; /* POINT32 */

			memset(&sb, 0, sizeof(sb));
			if(!DeviceIoControl(vxd, OP_NV_SUBMIT, m, n*4, &sb, sizeof(sb), &ret, NULL))
			{
				printf("OP_NV_SUBMIT failed (error %lu)\n", GetLastError());
				CloseHandle(vxd); return 1;
			}
			memset(&vr, 0, sizeof(vr));
			in[0] = scratch; in[1] = 1;
			DeviceIoControl(vxd, OP_NV_VRAM_READ, in, 8, &vr, sizeof(vr), &ret, NULL);
			if(pass == 0)
				printf("RING-3 submit (build %lu): ctxctl=%08lX\n", sb.build, sb.ctxctl);
			printf("submit %d: color %08lX -> IBget/put %lu/%lu %s pgi=%08lX scr=%08lX %s\n",
				pass+1, col, sb.get, sb.put, (sb.get==sb.put)?"consumed":"STALL",
				sb.pgraph_intr, vr.data[0], (vr.data[0]==col)?"OK":"MISMATCH");
			printf("   chan: cur=%08lX caches=%08lX c1GET/PUT=%08lX/%08lX dmast=%08lX\n",
				sb.c1_push1, sb.caches, sb.c1_get, sb.c1_put, sb.dma_state);
			printf("   push: DMA_PUSH=%08lX PUSH0=%08lX PULL0=%08lX\n",
				sb.dma_push, sb.push0, sb.pull0);
		}
		/* final readback of three corners from the last fill */
		{
			unsigned long off[3]; int k; unsigned long last = colors[1]; int allok = 1;
			off[0] = scratch; off[1] = scratch + 128*4 + 128*1024; off[2] = scratch + 255*4 + 255*1024;
			printf("last-fill corners: ");
			for(k = 0; k < 3; k++)
			{
				in[0] = off[k]; in[1] = 1;
				DeviceIoControl(vxd, OP_NV_VRAM_READ, in, 8, &vr, sizeof(vr), &ret, NULL);
				printf("%08lX ", vr.data[0]);
				if(vr.data[0] != last) allok = 0;
			}
			printf("\n");
			if(allok)
				printf("*** RING-3 DRIVES THE GPU - persistent channel + submit work! (M5.3) ***\n");
			else
				printf("corners mismatch - see GET/PUT and pgi above.\n");
		}
		CloseHandle(vxd);
		return 0;
	}

	if(argc >= 2 && strcmp(argv[1], "fence") == 0)
	{
		nv_gpu_2dfill_t fl;
		memset(&fl, 0, sizeof(fl));
		if(!DeviceIoControl(vxd, OP_NV_GPU_FENCE, NULL, 0, &fl, sizeof(fl), &ret, NULL))
		{
			printf("OP_NV_GPU_FENCE failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("GPU HEAP + FENCE (build %lu): stage %lu/6\n", fl.build, fl.stage);
		printf("heap-allocated fence BO @ VRAM %08lX (VM %08lX)\n", fl.heap_addr, fl.notify[2]);
		printf("DMA GET/PUT = %08lX / %08lX %s  PGRAPH_INTR=%08lX err=%lu\n",
			fl.get, fl.put, (fl.get==fl.put)?"(drained)":"(STALLED)", fl.pgraph_intr, (fl.dma_state>>29)&7);
		printf("GPU wrote fence value: got %08lX, expected %08lX (was 0)\n",
			fl.notify[0], fl.notify[1]);
		if(fl.notified)
			printf("*** FENCE WORKS - the GPU wrote the sequence to a heap BO via the VM! (M5.2b) ***\n");
		else
			printf("fence not written - see PGRAPH_INTR/err.\n");
		return 0;
	}

	if(argc >= 2 && strcmp(argv[1], "vmtest") == 0)
	{
		nv_gpu_2dfill_t fl;
		memset(&fl, 0, sizeof(fl));
		if(!DeviceIoControl(vxd, OP_NV_GPU_VMTEST, NULL, 0, &fl, sizeof(fl), &ret, NULL))
		{
			printf("OP_NV_GPU_VMTEST failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("GPU 2D fill VIA VIRTUAL MEMORY (build %lu): stage %lu/6\n", fl.build, fl.stage);
		printf("DMA GET/PUT = %08lX / %08lX %s\n", fl.get, fl.put, (fl.get==fl.put)?"(drained)":"(STALLED)");
		printf("PFIFO_INTR=%08lX PGRAPH_INTR=%08lX STATE=%08lX err=%lu(6=MEMFAULT)\n",
			fl.pfifo_intr, fl.pgraph_intr, fl.dma_state, (fl.dma_state>>29)&7);
		printf("scratch (physical) after a fill addressed by GPU VIRTUAL 0x43400000:\n");
		printf("  TL=%08lX  mid=%08lX  BR=%08lX\n", fl.px[0], fl.px[1], fl.px[2]);
		if(fl.filled)
			printf("*** GPU VIRTUAL MEMORY WORKS - the VM translated our write! (M5.2a) ***\n");
		else
			printf("not filled - the VM did not translate (see err/INTR); M5.2 needs more.\n");
		return 0;
	}

	if(argc >= 2 && strcmp(argv[1], "gpu2d") == 0)
	{
		nv_gpu_2dfill_t fl;
		memset(&fl, 0, sizeof(fl));
		if(!DeviceIoControl(vxd, OP_NV_GPU_2DFILL, NULL, 0, &fl, sizeof(fl), &ret, NULL))
		{
			printf("OP_NV_GPU_2DFILL failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("GPU 2D fill (build %lu): %s, stage %lu/6\n",
			fl.build, fl.ok ? "ran" : "ABORTED (no G86 / BAR0)", fl.stage);
		printf("scratch=%08lX  fill color=%08lX  pushbuf bytes=%lu\n",
			fl.scratch, fl.color, fl.put_bytes);
		printf("CTXCTL_CUR  = %08lX  (80003000 = our context loaded)\n", fl.ctxctl);
		printf("DMA GET/PUT = %08lX / %08lX  %s\n", fl.get, fl.put,
			(fl.get == fl.put) ? "(drained)" : "(STALLED - channel did not run)");
		printf("PFIFO_INTR  = %08lX   PGRAPH_INTR = %08lX   (0,0 = no fault)\n",
			fl.pfifo_intr, fl.pgraph_intr);
		printf("PGRAPH_STAT = %08lX   idle_loops=%lu  gr_idle_loops=%lu\n", fl.pgraph_stat, fl.idle_loops, fl.gr_idle);
		printf("CACHES=%08lX  CTX_TABLE[chid]=%08lX  CACHE1 PUSH1=%08lX GET/PUT=%08lX/%08lX STATE=%08lX\n",
			fl.caches, fl.ctxtable, fl.c1_push1, fl.c1_get, fl.c1_put, fl.c1_dmast);
		printf("pusher: STATE=%08lX err=%lu(6=MEMFAULT) PUSH=%08lX FETCH=%08lX INSTANCE=%08lX PRAMIN1700=%08lX\n",
			fl.dma_state, (fl.dma_state >> 29) & 7, fl.dma_push, fl.dma_fetch, fl.dma_inst, fl.punk1700);
		printf("VRAM read-back: ctxdma[0]=%08lX (want 0019003D)  pushbuf[0]=%08lX pushbuf[1]=%08lX\n",
			fl.ctxdma_rb, fl.push_rb0, fl.push_rb1);
		printf("scratch before fill = %08lX\n", fl.pre_px[0]);
		printf("scratch pixels after: TL=%08lX  mid=%08lX  BR=%08lX\n",
			fl.px[0], fl.px[1], fl.px[2]);
		if(fl.filled)
			printf("*** RECTANGLE FILLED BY THE GPU - 2D engine executed our commands! ***\n");
		else
			printf("not filled (pixels != color) - see GET/PUT and INTR above\n");
		return 0;
	}

	if(argc >= 2 && strcmp(argv[1], "ctxtest") == 0)
	{
		nv_gpu_ctxtest_t ct;
		memset(&ct, 0, sizeof(ct));
		if(!DeviceIoControl(vxd, OP_NV_GPU_CTXTEST, NULL, 0, &ct, sizeof(ct), &ret, NULL))
		{
			printf("OP_NV_GPU_CTXTEST failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("GPU context load test (build %lu): %s, stage %lu/5\n",
			ct.build, ct.ok ? "ran" : "ABORTED (no G86 / BAR0)", ct.stage);
		printf("ramin=%08lX  grctx=%08lX (%lu bytes, %lu values)  inst12=%05lX\n",
			ct.ramin, ct.grctx, ct.grctx_size, ct.ctxvals, ct.inst12);
		printf("CTXCTL_CUR  %08lX -> %08lX\n", ct.pre_ctxctl, ct.post_ctxctl);
		printf("CTXCTL_NEXT          -> %08lX\n", ct.post_next);
		printf("PGRAPH_INTR %08lX -> %08lX  (0 = no fault)\n", ct.pre_intr, ct.post_intr);
		printf("PGRAPH_STAT          -> %08lX  idle_loops=%lu\n", ct.post_status, ct.idle_loops);
		if(ct.loaded)
			printf("*** CONTEXT LOADED (bit31 set, instance matches) - the microcode works! ***\n");
		else
			printf("context NOT loaded (CTXCTL_CUR bit31 or instance wrong)\n");
		return 0;
	}

	if(argc >= 2 && strcmp(argv[1], "grinit") == 0)
	{
		nv_gpu_grinit_t gi;
		memset(&gi, 0, sizeof(gi));
		if(!DeviceIoControl(vxd, OP_NV_GPU_GRINIT, NULL, 0, &gi, sizeof(gi), &ret, NULL))
		{
			printf("OP_NV_GPU_GRINIT failed (error %lu)\n", GetLastError());
			CloseHandle(vxd); return 1;
		}
		CloseHandle(vxd);
		printf("GPU PGRAPH init (build %lu): %s, reached stage %lu/5\n",
			gi.build, gi.ok ? "ran" : "ABORTED (no G86 / BAR0)", gi.stage);
		printf("units now   = %08lX   (context baked for %08lX)%s\n",
			gi.units, gi.units_built,
			(gi.units != gi.units_built) ? "  <-- MISMATCH: regenerate the context!" : "");
		printf("ctxprog_len = %lu dwords   grctx_size = %lu bytes\n", gi.ctxprog_len, gi.grctx_size);
		printf("PMC_ENABLE  %08lX -> %08lX\n", gi.pre_pmc, gi.post_pmc);
		printf("CTXCTL_CUR  %08lX -> %08lX  (bit31 LOADED)\n", gi.pre_ctxctl, gi.post_ctxctl);
		printf("PGRAPH_INTR %08lX -> %08lX  (0 = no fault)\n", gi.pre_intr, gi.post_intr);
		printf("PGRAPH_STAT %08lX -> %08lX  (0 = idle)  idle_loops=%lu\n",
			gi.pre_status, gi.post_status, gi.idle_loops);
		printf("ucode read-back [0..7]: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
			gi.ucode_rb[0], gi.ucode_rb[1], gi.ucode_rb[2], gi.ucode_rb[3],
			gi.ucode_rb[4], gi.ucode_rb[5], gi.ucode_rb[6], gi.ucode_rb[7]);
		printf("(ucode_rb[0] should match the first baked ctxprog dword; INTR 0 + a sane\n");
		printf(" status mean PGRAPH accepted the init and microcode.)\n");
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
