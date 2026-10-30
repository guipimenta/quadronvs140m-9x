/* t61util.exe - helper the win98-remote agent uploads and runs on the T61.
 *
 * The agent (projects/win98-remote, protocol 1) can only PUT/GET flat file
 * names in its own folder, at most 64 MiB, and RUN an .exe there. This
 * helper fills the gaps for driver work; tools/remote/t61.py drives it.
 *
 *   info                        Windows version, folders, memory, disk, time
 *   dir <path|pattern>          list files (name, size, date)
 *   type <path>                 print a text file
 *   crc <path>                  "<size> <crc32>" (same CRC32 as zlib.crc32)
 *   copy <src> <dst>            copy a file (overwrites dst)
 *   part <src> <off> <len> <dst> copy a byte range (files over 64 MiB)
 *   join <dst> <part>...        concatenate parts into dst
 *   del <name>                  delete a file in the agent folder only
 *   mkdir <path>                create a folder
 *   exec <secs> <exe> [args]    run a program in its own folder, wait, kill on timeout
 *   sh <secs> <line>...         run DOS lines (one per argument) as a batch file
 *   start <exe> [args]          start a program in its own folder, do not wait
 *   ps                          list processes
 *   kill <exe name>             terminate every process with that file name
 *   shot <file.bmp>             capture the screen to a 24-bit BMP
 *   reboot                      restart Windows (the agent does not come back by itself)
 *
 * Output goes to stdout, which the agent stores in lastrun.log. Exit code 0 =
 * done, 1 = failed, 2 = usage, 124 = the exec/sh child timed out.
 * Build: i686-w64-mingw32-gcc -O2 -s -Wall tools/remote/t61util.c -o tools/remote/t61util.exe -lgdi32
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>  /* sprintf/_vsnprintf only: output goes through out() */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Our output handle. The agent hands us an INHERITABLE handle to its
 * lastrun.log; a child that inherited it (a game we start, a grandchild a
 * timeout could not kill) would keep the file open and the agent would refuse
 * every later request. So main() swaps it for a private duplicate and closes
 * the original before anything is spawned, and no CRT stdio is used. */
static HANDLE hout;
static char obuf[2048];
static void out(const char *fmt, ...)
{
	va_list ap; DWORD w; int n;
	va_start(ap, fmt);
	n = _vsnprintf(obuf, sizeof(obuf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) n = sizeof(obuf) - 1;
	WriteFile(hout, obuf, (DWORD)n, &w, NULL);
}

static unsigned long crctab[256];
static void crc_init(void)
{
	unsigned long c; int i, k;
	for (i = 0; i < 256; i++) {
		c = (unsigned long)i;
		for (k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xedb88320UL : 0);
		crctab[i] = c;
	}
}
static unsigned long crc_add(unsigned long c, const unsigned char *p, DWORD n)
{
	while (n--) c = crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c;
}

static char buf[65536];

static int fail(const char *what, const char *arg)
{
	out("t61util: %s %s failed (error %lu)\n", what, arg ? arg : "", (unsigned long)GetLastError());
	return 1;
}

/* The raw rest of the command line after `skip` tokens (quote aware), so
 * arguments reach the child exactly as the controller quoted them. */
static const char *rest_of_line(int skip)
{
	const char *p = GetCommandLineA();
	int i, q;
	for (i = 0; i < skip; i++) {
		while (*p == ' ' || *p == '\t') p++;
		q = 0;
		while (*p && (q || (*p != ' ' && *p != '\t'))) {
			if (*p == '"') q = !q;
			p++;
		}
	}
	while (*p == ' ' || *p == '\t') p++;
	return p;
}

static void exe_dir(const char *exe, char *dir)
{
	char *s;
	DWORD n = GetFullPathNameA(exe, MAX_PATH, dir, &s);
	if (!n || n >= MAX_PATH) { GetCurrentDirectoryA(MAX_PATH, dir); return; }
	s = strrchr(dir, '\\');
	if (s) { if (s == dir + 2 && dir[1] == ':') s[1] = 0; else *s = 0; }
}

static int cmd_info(void)
{
	char a[MAX_PATH];
	OSVERSIONINFOA v; MEMORYSTATUS m; SYSTEMTIME t;
	DWORD spc, bps, fc, tc;
	v.dwOSVersionInfoSize = sizeof(v);
	GetVersionExA(&v);
	out("windows %lu.%lu build %lu %s\n", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
	       (unsigned long)(v.dwBuildNumber & 0xffff), v.szCSDVersion);
	GetModuleFileNameA(NULL, a, sizeof(a)); out("helper   %s\n", a);
	GetCurrentDirectoryA(sizeof(a), a); out("cwd      %s\n", a);
	GetWindowsDirectoryA(a, sizeof(a)); out("windir   %s\n", a);
	GetSystemDirectoryA(a, sizeof(a)); out("sysdir   %s\n", a);
	m.dwLength = sizeof(m);
	GlobalMemoryStatus(&m);
	out("memory   %lu MB total, %lu MB free\n", (unsigned long)(m.dwTotalPhys >> 20), (unsigned long)(m.dwAvailPhys >> 20));
	if (GetDiskFreeSpaceA("C:\\", &spc, &bps, &fc, &tc))
		out("disk C:  %lu MB free (2 GB cap on 9x)\n", (unsigned long)(((double)spc * bps * fc) / 1048576.0));
	GetLocalTime(&t);
	out("time     %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	out("screen   %dx%d, %d bpp\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
	       GetDeviceCaps(GetDC(NULL), BITSPIXEL));
	return 0;
}

static int cmd_dir(const char *path)
{
	char pat[MAX_PATH + 8];
	WIN32_FIND_DATAA fd; HANDLE h; SYSTEMTIME st; FILETIME lt;
	DWORD a = GetFileAttributesA(path);
	int n = 0;
	if (strlen(path) > MAX_PATH) return 2;
	strcpy(pat, path);
	if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
		size_t l = strlen(pat);
		if (l && pat[l - 1] != '\\') strcat(pat, "\\");
		strcat(pat, "*.*");
	}
	h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return fail("dir", pat);
	do {
		FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt);
		FileTimeToSystemTime(&lt, &st);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			out("%-40s %12s %04u-%02u-%02u %02u:%02u\n", fd.cFileName, "<DIR>", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
		else
			out("%-40s %12lu %04u-%02u-%02u %02u:%02u\n", fd.cFileName, (unsigned long)fd.nFileSizeLow, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
		n++;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	out("%d entries\n", n);
	return 0;
}

static HANDLE open_read(const char *p)
{
	return CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static int cmd_type(const char *p)
{
	DWORD n, w;
	HANDLE f = open_read(p);
	if (f == INVALID_HANDLE_VALUE) return fail("open", p);
	while (ReadFile(f, buf, sizeof(buf), &n, NULL) && n) WriteFile(hout, buf, n, &w, NULL);
	CloseHandle(f);
	return 0;
}

static int cmd_crc(const char *p)
{
	DWORD n, total = 0;
	unsigned long c = 0xffffffffUL;
	HANDLE f = open_read(p);
	if (f == INVALID_HANDLE_VALUE) return fail("open", p);
	while (ReadFile(f, buf, sizeof(buf), &n, NULL) && n) { c = crc_add(c, (unsigned char *)buf, n); total += n; }
	CloseHandle(f);
	out("%lu %lu\n", (unsigned long)total, c ^ 0xffffffffUL);
	return 0;
}

/* Copy len bytes (all if len == 0xffffffff) from src at off, appending to an open handle. */
static int copy_range(const char *src, DWORD off, DWORD len, HANDLE out)
{
	DWORD n, w, want;
	HANDLE f = open_read(src);
	if (f == INVALID_HANDLE_VALUE) return fail("open", src);
	if (off && SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) == 0xffffffff) { CloseHandle(f); return fail("seek", src); }
	while (len) {
		want = len < sizeof(buf) ? len : sizeof(buf);
		if (!ReadFile(f, buf, want, &n, NULL)) { CloseHandle(f); return fail("read", src); }
		if (!n) break;
		if (!WriteFile(out, buf, n, &w, NULL) || w != n) { CloseHandle(f); return fail("write", ""); }
		if (len != 0xffffffffUL) len -= n;
	}
	CloseHandle(f);
	return 0;
}

static HANDLE open_write(const char *p)
{
	return CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int cmd_part(const char *src, const char *off, const char *len, const char *dst)
{
	int r;
	HANDLE o = open_write(dst);
	if (o == INVALID_HANDLE_VALUE) return fail("create", dst);
	r = copy_range(src, strtoul(off, NULL, 10), strtoul(len, NULL, 10), o);
	if (!FlushFileBuffers(o)) r = 1;
	CloseHandle(o);
	if (r) DeleteFileA(dst); else out("part %s ok\n", dst);
	return r;
}

static int cmd_join(int n, char **parts, const char *dst)
{
	int i, r = 0;
	HANDLE o = open_write(dst);
	if (o == INVALID_HANDLE_VALUE) return fail("create", dst);
	for (i = 0; i < n && !r; i++) r = copy_range(parts[i], 0, 0xffffffffUL, o);
	if (!FlushFileBuffers(o)) r = 1;
	CloseHandle(o);
	if (r) DeleteFileA(dst); else out("joined %d parts into %s\n", n, dst);
	return r;
}

static int cmd_del(const char *name)
{
	if (strpbrk(name, "\\/:") || !strcmp(name, ".") || !strcmp(name, "..")) {
		out("t61util: del only takes a file name in the agent folder\n");
		return 2;
	}
	if (!DeleteFileA(name)) return fail("del", name);
	out("deleted %s\n", name);
	return 0;
}

/* Run cmdline in dir. wait: wait up to secs (0 = forever), kill on timeout.
 * capture: the child's stdout/stderr go to t61exec.out, printed afterwards
 * (a fresh name if an orphan still holds the old one). Otherwise the child
 * inherits nothing. after: a file to print once the child is done. */
static int spawn(const char *app, char *cmdline, const char *dir, DWORD secs, int wait, int capture, const char *after)
{
	STARTUPINFOA si; PROCESS_INFORMATION pi; SECURITY_ATTRIBUTES sa;
	DWORD code = 0, r; HANDLE cap = INVALID_HANDLE_VALUE, nul = INVALID_HANDLE_VALUE;
	char capname[32]; int i, rc;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (capture) {
		sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
		for (i = 0; i < 10 && cap == INVALID_HANDLE_VALUE; i++) {
			sprintf(capname, i ? "t61exec%d.out" : "t61exec.out", i);
			cap = CreateFileA(capname, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		}
		nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
		if (cap == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) return fail("create", "t61exec.out");
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = nul; si.hStdOutput = cap; si.hStdError = cap;
	}
	if (!CreateProcessA(app, cmdline, NULL, NULL, capture, 0, NULL, dir, &si, &pi)) {
		if (capture) { CloseHandle(cap); CloseHandle(nul); }
		return fail("start", cmdline);
	}
	CloseHandle(pi.hThread);
	if (capture) { CloseHandle(cap); CloseHandle(nul); }
	if (!wait) {
		out("started pid 0x%lx: %s\n", (unsigned long)pi.dwProcessId, cmdline);
		CloseHandle(pi.hProcess);
		return 0;
	}
	r = WaitForSingleObject(pi.hProcess, secs ? secs * 1000 : INFINITE);
	if (r == WAIT_TIMEOUT) {
		TerminateProcess(pi.hProcess, 124);
		WaitForSingleObject(pi.hProcess, 5000);
		rc = 124;
	} else {
		GetExitCodeProcess(pi.hProcess, &code);
		rc = code ? 1 : 0;
	}
	CloseHandle(pi.hProcess);
	if (capture) cmd_type(capname);
	if (after) cmd_type(after);
	if (rc == 124) out("\nt61util: timed out after %lu s, child terminated\n", (unsigned long)secs);
	else out("\nt61util: exit code %lu\n", (unsigned long)code);
	return rc;
}

static char cmdbuf[4096];

static int cmd_exec(int skip, const char *exe, DWORD secs, int wait)
{
	char dir[MAX_PATH], full[MAX_PATH], *fp;
	const char *args = rest_of_line(skip);
	if (!SearchPathA(NULL, exe, NULL, MAX_PATH, full, &fp)) return fail("find", exe);
	exe_dir(full, dir);
	if (strlen(full) + strlen(args) + 4 >= sizeof(cmdbuf)) return 2;
	snprintf(cmdbuf, sizeof(cmdbuf), "\"%s\" %s", full, args);
	return spawn(full, cmdbuf, dir, secs, wait, wait, NULL);
}

/* The line runs from a batch file, so .bat helpers (nvapp, nvlogs) and DOS
 * redirection work and the 127-byte DOS command-line limit does not apply. */
static int cmd_sh(DWORD secs, int argc, char **argv)
{
	char comspec[MAX_PATH], dir[MAX_PATH], line[2048];
	HANDLE b; DWORD w; int i;
	line[0] = 0;
	for (i = 0; i < argc; i++) {
		if (strlen(line) + strlen(argv[i]) + 3 >= sizeof(line)) return 2;
		if (i) strcat(line, "\r\n");   /* one argument per batch line */
		strcat(line, argv[i]);
	}
	if (!GetEnvironmentVariableA("COMSPEC", comspec, sizeof(comspec))) strcpy(comspec, "C:\\COMMAND.COM");
	GetCurrentDirectoryA(sizeof(dir), dir);
	b = open_write("t61sh.bat");
	if (b == INVALID_HANDLE_VALUE) return fail("create", "t61sh.bat");
	WriteFile(b, "@echo off\r\n", 11, &w, NULL);
	WriteFile(b, line, (DWORD)strlen(line), &w, NULL);
	WriteFile(b, "\r\n", 2, &w, NULL);
	CloseHandle(b);
	DeleteFileA("t61sh.out");
	/* COMMAND.COM ignores a redirect on a batch file call (the file stays
	 * empty), so redirect a second COMMAND.COM that runs the batch. */
	sprintf(cmdbuf, "\"%s\" /c %s /c t61sh.bat > t61sh.out", comspec, comspec);
	return spawn(NULL, cmdbuf, dir, secs, 1, 0, "t61sh.out");
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '\\');
	return s ? s + 1 : p;
}

static int cmd_ps(const char *kill)
{
	PROCESSENTRY32 pe; HANDLE s, h; int n = 0;
	s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (s == INVALID_HANDLE_VALUE) return fail("snapshot", "");
	pe.dwSize = sizeof(pe);
	if (Process32First(s, &pe)) do {
		if (!kill) { out("%08lx %3lu %s\n", (unsigned long)pe.th32ProcessID, (unsigned long)pe.cntThreads, pe.szExeFile); continue; }
		if (lstrcmpiA(base_name(pe.szExeFile), kill)) continue;
		h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
		if (h && TerminateProcess(h, 1)) { out("killed %08lx %s\n", (unsigned long)pe.th32ProcessID, pe.szExeFile); n++; }
		else out("could not kill %08lx %s (error %lu)\n", (unsigned long)pe.th32ProcessID, pe.szExeFile, (unsigned long)GetLastError());
		if (h) CloseHandle(h);
	} while (Process32Next(s, &pe));
	CloseHandle(s);
	if (kill && !n) { out("no process named %s\n", kill); return 1; }
	return 0;
}

static int cmd_shot(const char *dst)
{
	int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
	DWORD stride = ((DWORD)w * 3 + 3) & ~3u, size = stride * (DWORD)h, wr;
	BITMAPINFO bi; BITMAPFILEHEADER fh; void *bits = NULL;
	HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
	HBITMAP bm, old; HANDLE f; int ok;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bm || !bits) return fail("CreateDIBSection", "");
	old = SelectObject(mdc, bm);
	ok = BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
	GdiFlush();
	SelectObject(mdc, old);
	DeleteDC(mdc);
	ReleaseDC(NULL, sdc);
	if (!ok) { DeleteObject(bm); return fail("BitBlt", ""); }
	memset(&fh, 0, sizeof(fh));
	fh.bfType = 0x4d42;
	fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
	fh.bfSize = fh.bfOffBits + size;
	f = open_write(dst);
	if (f == INVALID_HANDLE_VALUE) { DeleteObject(bm); return fail("create", dst); }
	WriteFile(f, &fh, sizeof(fh), &wr, NULL);
	WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, NULL);
	WriteFile(f, bits, size, &wr, NULL);
	CloseHandle(f);
	DeleteObject(bm);
	out("screen %dx%d saved to %s\n", w, h, dst);
	return 0;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE), se = GetStdHandle(STD_ERROR_HANDLE), si = GetStdHandle(STD_INPUT_HANDLE);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	if (!DuplicateHandle(GetCurrentProcess(), so, GetCurrentProcess(), &hout, 0, FALSE, DUPLICATE_SAME_ACCESS)) hout = so;
	else {
		CloseHandle(so);
		if (se != so) CloseHandle(se);
		CloseHandle(si);
	}
	crc_init();
	if (!strcmp(c, "info")) return cmd_info();
	if (!strcmp(c, "dir") && argc == 3) return cmd_dir(argv[2]);
	if (!strcmp(c, "type") && argc == 3) return cmd_type(argv[2]);
	if (!strcmp(c, "crc") && argc == 3) return cmd_crc(argv[2]);
	if (!strcmp(c, "copy") && argc == 4) return CopyFileA(argv[2], argv[3], FALSE) ? (out("copied %s -> %s\n", argv[2], argv[3]), 0) : fail("copy", argv[2]);
	if (!strcmp(c, "part") && argc == 6) return cmd_part(argv[2], argv[3], argv[4], argv[5]);
	if (!strcmp(c, "join") && argc >= 4) return cmd_join(argc - 3, argv + 3, argv[2]);
	if (!strcmp(c, "del") && argc == 3) return cmd_del(argv[2]);
	if (!strcmp(c, "mkdir") && argc == 3) return (CreateDirectoryA(argv[2], NULL) || GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : fail("mkdir", argv[2]);
	if (!strcmp(c, "exec") && argc >= 4) return cmd_exec(4, argv[3], strtoul(argv[2], NULL, 10), 1);
	if (!strcmp(c, "sh") && argc >= 4) return cmd_sh(strtoul(argv[2], NULL, 10), argc - 3, argv + 3);
	if (!strcmp(c, "start") && argc >= 3) return cmd_exec(3, argv[2], 0, 0);
	if (!strcmp(c, "ps") && argc == 2) return cmd_ps(NULL);
	if (!strcmp(c, "kill") && argc == 3) return cmd_ps(argv[2]);
	if (!strcmp(c, "shot") && argc == 3) return cmd_shot(argv[2]);
	if (!strcmp(c, "reboot") && argc == 2) {
		out("rebooting\n");
		CloseHandle(hout);
			return ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0) ? 0 : fail("ExitWindowsEx", "");
	}
	out("usage: t61util info | dir P | type P | crc P | copy S D | part S OFF LEN D | join D P... |\n"
	       "               del NAME | mkdir P | exec SECS EXE [ARGS] | sh SECS LINE | start EXE [ARGS] |\n"
	       "               ps | kill NAME | shot FILE.bmp | reboot\n");
	return 2;
}
