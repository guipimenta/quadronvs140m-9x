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
 *   reboot [force]              restart like the Start menu, a detached watcher answers the
 *                               agent console's quit/End Task prompts (force: EWX_FORCE)
 *   debug <exe> [args]          run exe detached under the debug API; DLL loads, exceptions with
 *                               module+offset, registers and stack go to t61debug.txt
 *   endtasks                    only the watcher (a shutdown already stuck on those prompts)
 *   wins [title]                visible top-level windows (title substring, case-insensitive)
 *   kids <title>                those windows and all their child controls
 *   focus|close <title>         bring to front / post WM_CLOSE
 *   press <title> <button>      click a button by its text (WM_COMMAND BN_CLICKED to its parent)
 *   waitwin <title> <secs>      wait until such a window is visible
 *   click <x> <y> [right|double] mouse click at screen coordinates
 *   key <combo>...              keys: enter, tab*3, ctrl+tab, alt+f4, shift+a ...
 *   text <string>               type a string
 *   autostart on|off|show       HKLM Run key "W98Remote" = <this folder>\t61boot.exe supervise
 *   supervise                   (from the Run key) keep w98agent.exe running, log t61boot.log
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
	if (!CreateProcessA(app, cmdline, NULL, NULL, capture, CREATE_DEFAULT_ERROR_MODE, NULL, dir, &si, &pi)) {
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

/* ---- windows, keyboard and mouse (build 3116 remote testing) ---------------- */

static int title_match(HWND w, const char *want)
{
	char a[256], b[256];
	if (!strcmp(want, "*")) return 1;
	if (!GetWindowTextA(w, a, sizeof(a))) return 0;
	lstrcpynA(b, want, sizeof(b));
	CharLowerBuffA(a, (DWORD)strlen(a));
	CharLowerBuffA(b, (DWORD)strlen(b));
	return strstr(a, b) != NULL;
}

/* Control text without '&' accelerators, compared case-insensitively. */
static int text_equal(HWND w, const char *want)
{
	char t[256], u[256]; int i, j;
	GetWindowTextA(w, t, sizeof(t));
	for (i = j = 0; t[i]; i++) if (t[i] != '&') u[j++] = t[i];
	u[j] = 0;
	return !lstrcmpiA(u, want);
}

static void print_window(HWND w, int indent)
{
	char t[200], c[64]; RECT r;
	GetWindowTextA(w, t, sizeof(t));
	GetClassNameA(w, c, sizeof(c));
	GetWindowRect(w, &r);
	out("%*s%08lx %-20s id %-5d %s%s (%ld,%ld)-(%ld,%ld) \"%s\"\n", indent, "", (unsigned long)(DWORD_PTR)w, c,
	    GetDlgCtrlID(w), IsWindowVisible(w) ? "" : "hidden ", IsWindowEnabled(w) ? "" : "disabled ",
	    r.left, r.top, r.right, r.bottom, t);
}

static BOOL CALLBACK enum_top(HWND w, LPARAM lp)
{
	if (IsWindowVisible(w) && title_match(w, (const char *)lp)) print_window(w, 0);
	return TRUE;
}
static BOOL CALLBACK enum_kid(HWND w, LPARAM lp)
{
	(void)lp;
	print_window(w, 2);
	return TRUE;
}
static BOOL CALLBACK enum_top_kids(HWND w, LPARAM lp)
{
	if (IsWindowVisible(w) && title_match(w, (const char *)lp)) {
		print_window(w, 0);
		EnumChildWindows(w, enum_kid, 0);
	}
	return TRUE;
}

struct find { const char *title, *text; HWND top, hit; };
static BOOL CALLBACK find_kid(HWND w, LPARAM lp)
{
	struct find *f = (struct find *)lp;
	char c[32];
	GetClassNameA(w, c, sizeof(c));
	if (!f->hit && IsWindowVisible(w) && IsWindowEnabled(w) && !lstrcmpiA(c, "Button") && text_equal(w, f->text)) f->hit = w;
	return !f->hit;
}
static BOOL CALLBACK find_top(HWND w, LPARAM lp)
{
	struct find *f = (struct find *)lp;
	if (!IsWindowVisible(w) || !title_match(w, f->title)) return TRUE;
	if (!f->text) { f->top = w; return FALSE; }
	EnumChildWindows(w, find_kid, lp);
	if (f->hit) { f->top = w; return FALSE; }
	return TRUE;
}

/* Windows 98 only lets the foreground thread hand the foreground on, so join
 * its input queue for the switch. (Not the usual synthetic Alt tap: an Alt
 * press+release puts the target's menu bar in menu mode and eats the next keys.) */
static void bring_front(HWND w)
{
	HWND fg = GetForegroundWindow();
	DWORD me = GetCurrentThreadId(), him = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
	if (fg == w) return;
	if (him && him != me) AttachThreadInput(me, him, TRUE);
	SetForegroundWindow(w);
	BringWindowToTop(w);
	if (him && him != me) AttachThreadInput(me, him, FALSE);
	Sleep(150);
	if (GetForegroundWindow() != w) out("warning: could not bring window to the front\n");
}

static int cmd_focus(const char *title, int close)
{
	struct find f = { title, NULL, NULL, NULL };
	EnumWindows(find_top, (LPARAM)&f);
	if (!f.top) { out("no visible window titled *%s*\n", title); return 1; }
	if (close) PostMessageA(f.top, WM_CLOSE, 0, 0); else bring_front(f.top);
	out("%s ", close ? "closed" : "focused"); print_window(f.top, 0);
	return 0;
}

/* Click a push button by its text inside a window whose title contains title:
 * WM_COMMAND/BN_CLICKED to the button's parent, which is what a real click
 * delivers to a dialog, and needs neither focus nor coordinates. */
static int cmd_press(const char *title, const char *text)
{
	struct find f = { title, text, NULL, NULL };
	HWND parent;
	EnumWindows(find_top, (LPARAM)&f);
	if (!f.hit) { out("no enabled button \"%s\" in a window titled *%s*\n", text, title); return 1; }
	parent = GetParent(f.hit);
	PostMessageA(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(f.hit), BN_CLICKED), (LPARAM)f.hit);
	out("pressed "); print_window(f.hit, 0);
	return 0;
}

static int cmd_waitwin(const char *title, DWORD secs)
{
	DWORD t0 = GetTickCount();
	do {
		struct find f = { title, NULL, NULL, NULL };
		EnumWindows(find_top, (LPARAM)&f);
		if (f.top) { out("found "); print_window(f.top, 0); return 0; }
		Sleep(250);
	} while (GetTickCount() - t0 < secs * 1000);
	out("no window titled *%s* after %lu s\n", title, (unsigned long)secs);
	return 1;
}

static int cmd_click(int x, int y, const char *how)
{
	int n = how && !strcmp(how, "double") ? 2 : 1, right = how && !strcmp(how, "right");
	DWORD dn = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN, up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
	HWND w;
	SetCursorPos(x, y);
	Sleep(50);
	w = WindowFromPoint((POINT){ x, y });
	if (w) { HWND top = GetAncestor(w, GA_ROOT); if (top && top != GetForegroundWindow()) bring_front(top); SetCursorPos(x, y); }
	while (n--) { mouse_event(dn, 0, 0, 0, 0); mouse_event(up, 0, 0, 0, 0); Sleep(60); }
	out("clicked %d,%d%s%s on ", x, y, how ? " " : "", how ? how : "");
	if (w) print_window(w, 0); else out("nothing\n");
	return 0;
}

static const struct { const char *n; BYTE vk; } keynames[] = {
	{"enter",VK_RETURN},{"return",VK_RETURN},{"tab",VK_TAB},{"esc",VK_ESCAPE},{"escape",VK_ESCAPE},
	{"space",VK_SPACE},{"back",VK_BACK},{"backspace",VK_BACK},{"del",VK_DELETE},{"delete",VK_DELETE},
	{"ins",VK_INSERT},{"home",VK_HOME},{"end",VK_END},{"pgup",VK_PRIOR},{"pgdn",VK_NEXT},
	{"up",VK_UP},{"down",VK_DOWN},{"left",VK_LEFT},{"right",VK_RIGHT},{"win",VK_LWIN},{"apps",VK_APPS},
	{"ctrl",VK_CONTROL},{"alt",VK_MENU},{"shift",VK_SHIFT},
	{"f1",VK_F1},{"f2",VK_F2},{"f3",VK_F3},{"f4",VK_F4},{"f5",VK_F5},{"f6",VK_F6},{"f7",VK_F7},{"f8",VK_F8},
	{"f9",VK_F9},{"f10",VK_F10},{"f11",VK_F11},{"f12",VK_F12},
};

static int key_code(const char *n, BYTE *vk)
{
	unsigned i;
	for (i = 0; i < sizeof(keynames) / sizeof(keynames[0]); i++)
		if (!lstrcmpiA(n, keynames[i].n)) { *vk = keynames[i].vk; return 1; }
	if (n[0] && !n[1]) { SHORT s = VkKeyScanA(n[0]); if (s != -1) { *vk = (BYTE)(s & 0xff); return 1; } }
	return 0;
}

/* One combo like ctrl+tab, alt+f4, shift+a, enter, tab*3. */
static int cmd_key(const char *spec)
{
	char tmp[64], *parts[6], *star; BYTE vks[6]; int n = 0, i, rep = 1;
	if (strlen(spec) >= sizeof(tmp)) return 2;
	strcpy(tmp, spec);
	if ((star = strchr(tmp, '*')) != NULL) { *star = 0; rep = atoi(star + 1); if (rep < 1 || rep > 100) rep = 1; }
	parts[n++] = strtok(tmp, "+");
	while (n < 6 && (parts[n] = strtok(NULL, "+")) != NULL) n++;
	for (i = 0; i < n; i++) if (!parts[i] || !key_code(parts[i], &vks[i])) { out("unknown key in %s\n", spec); return 2; }
	while (rep--) {
		for (i = 0; i < n; i++) keybd_event(vks[i], (BYTE)MapVirtualKeyA(vks[i], 0), 0, 0);
		for (i = n - 1; i >= 0; i--) keybd_event(vks[i], (BYTE)MapVirtualKeyA(vks[i], 0), KEYEVENTF_KEYUP, 0);
		Sleep(40);
	}
	out("key %s\n", spec);
	return 0;
}

static int cmd_text(const char *t)
{
	for (; *t; t++) {
		SHORT s = VkKeyScanA(*t);
		BYTE vk = (BYTE)(s & 0xff);
		if (s == -1) continue;
		if (s & 0x100) keybd_event(VK_SHIFT, 0, 0, 0);
		keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), 0, 0);
		keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), KEYEVENTF_KEYUP, 0);
		if (s & 0x100) keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
		Sleep(15);
	}
	out("typed text\n");
	return 0;
}

/* A dialog (any title) holding a control whose text contains `has` and an
 * enabled button `button`: press it. */
struct find2 { const char *has, *button; HWND hit; int seen; };
static BOOL CALLBACK find2_kid(HWND w, LPARAM lp)
{
	struct find2 *f = (struct find2 *)lp;
	char t[512], c[32];
	GetWindowTextA(w, t, sizeof(t));
	GetClassNameA(w, c, sizeof(c));
	if (strstr(t, f->has)) f->seen = 1;
	if (!f->hit && IsWindowEnabled(w) && !lstrcmpiA(c, "Button") && text_equal(w, f->button)) f->hit = w;
	return TRUE;
}
static BOOL CALLBACK find2_top(HWND w, LPARAM lp)
{
	struct find2 *f = (struct find2 *)lp;
	if (!IsWindowVisible(w)) return TRUE;
	f->hit = NULL; f->seen = 0;
	EnumChildWindows(w, find2_kid, lp);
	if (f->seen && f->hit) {
		PostMessageA(GetParent(f->hit), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(f->hit), BN_CLICKED), (LPARAM)f->hit);
		return FALSE;
	}
	f->hit = NULL;
	return TRUE;
}
static int press_where(const char *has, const char *button)
{
	struct find2 f = { has, button, NULL, 0 };
	EnumWindows(find2_top, (LPARAM)&f);
	return f.hit != NULL;
}

/* Detached from the agent's console: optionally start the Start-menu restart,
 * then answer the two dialogs the agent's console window raises ("You must
 * quit this program before you quit Windows" -> OK, "This program is not
 * responding" -> End Task) until Windows is gone or 120 s pass. */
static int cmd_rebootwatch(int start)
{
	char cmd[] = "rundll32.exe shell32.dll,SHExitWindowsEx 2";
	STARTUPINFOA si; PROCESS_INFORMATION pi; DWORD t0;
	Sleep(1500);                         /* let the agent send its reply */
	if (start) {
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
	}
	for (t0 = GetTickCount(); GetTickCount() - t0 < 120000; Sleep(500)) {
		press_where("You must quit this program", "OK");
		press_where("This program is not responding", "End Task");
	}
	return 0;
}

static int spawn_detached(const char *mode)
{
	char self[MAX_PATH];
	STARTUPINFOA si; PROCESS_INFORMATION pi;
	GetModuleFileNameA(NULL, self, sizeof(self));
	snprintf(cmdbuf, sizeof(cmdbuf), "\"%s\" %s", self, mode);
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessA(self, cmdbuf, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) return fail("start", cmdbuf);
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	out("%s: started detached watcher\n", mode);
	return 0;
}

/* ---- debug: run a program under the Win32 debug API, detached --------------- */

#define DBG_MAXMOD 160
static struct { DWORD base, size; char name[64]; } dmod[DBG_MAXMOD];
static int ndmod;
static struct { DWORD id; HANDLE h; } dthr[128];
static int ndthr;
static HANDLE dproc, dlog;

static void dout(const char *fmt, ...)
{
	va_list ap; DWORD w; int n;
	va_start(ap, fmt);
	n = _vsnprintf(obuf, sizeof(obuf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) n = sizeof(obuf) - 1;
	WriteFile(dlog, obuf, (DWORD)n, &w, NULL);
	FlushFileBuffers(dlog);
}

static int rd(DWORD a, void *p, DWORD n)
{
	DWORD got = 0;
	return ReadProcessMemory(dproc, (LPCVOID)(DWORD_PTR)a, p, n, &got) && got == n;
}

/* Name and size from the image itself: export directory name, SizeOfImage. */
static void add_module(DWORD base, const char *fallback)
{
	DWORD pe = 0, size = 0, exp = 0, nm = 0; char name[64];
	int i;
	strcpy(name, fallback);
	if (rd(base + 0x3c, &pe, 4) && rd(base + pe + 0x50, &size, 4) && rd(base + pe + 0x78, &exp, 4) && exp &&
	    rd(base + exp + 12, &nm, 4) && nm && rd(base + nm, name, sizeof(name) - 1))
		name[sizeof(name) - 1] = 0;
	for (i = 0; name[i]; i++) if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) { name[i] = 0; break; }
	if (ndmod < DBG_MAXMOD) { dmod[ndmod].base = base; dmod[ndmod].size = size ? size : 0x1000; strcpy(dmod[ndmod].name, name); ndmod++; }
	dout("load %08lx-%08lx %s\r\n", (unsigned long)base, (unsigned long)(base + size), name);
}

/* Toolhelp also sees DLLs mapped in the shared arena by other processes
 * (the DirectDraw/Direct3D HAL), which send no LOAD_DLL event here. */
static DWORD dpid;
static void refresh_modules(void)
{
	MODULEENTRY32 me; HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dpid);
	int i, known;
	if (s == INVALID_HANDLE_VALUE) return;
	me.dwSize = sizeof(me);
	if (Module32First(s, &me)) do {
		DWORD b = (DWORD)(DWORD_PTR)me.modBaseAddr;
		for (known = 0, i = 0; i < ndmod; i++) if (dmod[i].base == b) known = 1;
		if (!known && ndmod < DBG_MAXMOD) {
			dmod[ndmod].base = b; dmod[ndmod].size = me.modBaseSize;
			lstrcpynA(dmod[ndmod].name, me.szModule, sizeof(dmod[ndmod].name));
			dout("module %08lx-%08lx %s (toolhelp)\r\n", (unsigned long)b, (unsigned long)(b + me.modBaseSize), dmod[ndmod].name);
			ndmod++;
		}
	} while (Module32Next(s, &me));
	CloseHandle(s);
}

static const char *where(DWORD a, DWORD *off)
{
	int i;
	for (i = 0; i < ndmod; i++) if (a >= dmod[i].base && a < dmod[i].base + dmod[i].size) { *off = a - dmod[i].base; return dmod[i].name; }
	return NULL;
}

static void dump_thread(DWORD tid)
{
	CONTEXT c; HANDLE th = NULL; DWORD off, stk[512], v, fp, ret; const char *m; int i, n;
	for (i = 0; i < ndthr; i++) if (dthr[i].id == tid) th = dthr[i].h;
	if (!th) { dout("no handle for thread %lx\r\n", (unsigned long)tid); return; }
	refresh_modules();
	c.ContextFlags = CONTEXT_FULL;
	if (!GetThreadContext(th, &c)) { dout("GetThreadContext failed %lu\r\n", (unsigned long)GetLastError()); return; }
	m = where(c.Eip, &off);
	dout("eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx\r\nebp %08lx esp %08lx eip %08lx (%s+%lx) efl %08lx\r\n",
	     c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.Eip, m ? m : "?", m ? (unsigned long)off : 0UL, c.EFlags);
	dout("ebp chain:\r\n");
	for (fp = c.Ebp, n = 0; n < 24 && fp && rd(fp + 4, &ret, 4); n++) {
		m = where(ret, &off);
		dout("  %08lx %s+%lx\r\n", (unsigned long)ret, m ? m : "?", m ? (unsigned long)off : 0UL);
		if (!rd(fp, &v, 4) || v <= fp) break;
		fp = v;
	}
	dout("stack words that point into modules (esp+offset):\r\n");
	for (i = 0; i < 512; i++) if (!rd(c.Esp + i * 4, &stk[i], 4)) break;
	n = i;
	for (i = 0; i < n; i++) {
		m = where(stk[i], &off);
		if (m) dout("  +%03x %08lx %s+%lx\r\n", i * 4, (unsigned long)stk[i], m, (unsigned long)off);
	}
}

static void dump_crash(DWORD tid, EXCEPTION_RECORD *er)
{
	DWORD off; const char *m;
	refresh_modules();
	m = where((DWORD)(DWORD_PTR)er->ExceptionAddress, &off);
	dout("EXCEPTION %08lx at %08lx = %s+%lx, thread %lx", (unsigned long)er->ExceptionCode,
	     (unsigned long)(DWORD_PTR)er->ExceptionAddress, m ? m : "?", m ? (unsigned long)off : 0UL, (unsigned long)tid);
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		dout(", %s %08lx", er->ExceptionInformation[0] ? "writing" : "reading", (unsigned long)er->ExceptionInformation[1]);
	dout("\r\n");
	dump_thread(tid);
}

static int cmd_debugrun(const char *exe)
{
	char dir[MAX_PATH], full[MAX_PATH], *fp, msg[512];
	STARTUPINFOA si; PROCESS_INFORMATION pi; DEBUG_EVENT ev; DWORD cont, t0 = GetTickCount();
	int firstbp = 1, dumps = 0, i;
	const char *args = rest_of_line(3);
	dlog = CreateFileA("t61debug.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (dlog == INVALID_HANDLE_VALUE) return 1;
	if (!SearchPathA(NULL, exe, NULL, MAX_PATH, full, &fp)) { dout("cannot find %s\r\n", exe); return 1; }
	exe_dir(full, dir);
	snprintf(cmdbuf, sizeof(cmdbuf), "\"%s\" %s", full, args);
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessA(full, cmdbuf, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_DEFAULT_ERROR_MODE, NULL, dir, &si, &pi)) {
		dout("CreateProcess %s failed %lu\r\n", cmdbuf, (unsigned long)GetLastError());
		return 1;
	}
	dout("debugging pid %lx: %s\r\n", (unsigned long)pi.dwProcessId, cmdbuf);
	dproc = pi.hProcess;
	dpid = pi.dwProcessId;
	for (;;) {
		if (!WaitForDebugEvent(&ev, INFINITE)) break;
		cont = DBG_CONTINUE;
		switch (ev.dwDebugEventCode) {
		case CREATE_PROCESS_DEBUG_EVENT:
			if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
			dthr[ndthr].id = ev.dwThreadId; dthr[ndthr++].h = ev.u.CreateProcessInfo.hThread;
			add_module((DWORD)(DWORD_PTR)ev.u.CreateProcessInfo.lpBaseOfImage, "exe");
			break;
		case CREATE_THREAD_DEBUG_EVENT:
			if (ndthr < 128) { dthr[ndthr].id = ev.dwThreadId; dthr[ndthr++].h = ev.u.CreateThread.hThread; }
			dout("[%lu] thread %lx start %08lx\r\n", (unsigned long)(GetTickCount() - t0), (unsigned long)ev.dwThreadId,
			     (unsigned long)(DWORD_PTR)ev.u.CreateThread.lpStartAddress);
			break;
		case EXIT_THREAD_DEBUG_EVENT:
			dout("[%lu] thread %lx exit %lu\r\n", (unsigned long)(GetTickCount() - t0), (unsigned long)ev.dwThreadId, (unsigned long)ev.u.ExitThread.dwExitCode);
			/* who ended it: the exiting thread's stack still holds the callers */
			if (ev.u.ExitThread.dwExitCode && dumps < 8) { dumps++; dump_thread(ev.dwThreadId); }
			break;
		case LOAD_DLL_DEBUG_EVENT:
			if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
			dout("[%lu] ", (unsigned long)(GetTickCount() - t0));
			add_module((DWORD)(DWORD_PTR)ev.u.LoadDll.lpBaseOfDll, "dll");
			break;
		case UNLOAD_DLL_DEBUG_EVENT:
			for (i = 0; i < ndmod; i++) if (dmod[i].base == (DWORD)(DWORD_PTR)ev.u.UnloadDll.lpBaseOfDll) {
				dout("[%lu] unload %s\r\n", (unsigned long)(GetTickCount() - t0), dmod[i].name);
				dmod[i] = dmod[--ndmod];
				break;
			}
			break;
		case OUTPUT_DEBUG_STRING_EVENT: {
			DWORD n = ev.u.DebugString.nDebugStringLength;
			if (n >= sizeof(msg)) n = sizeof(msg) - 1;
			if (!ev.u.DebugString.fUnicode && rd((DWORD)(DWORD_PTR)ev.u.DebugString.lpDebugStringData, msg, n)) { msg[n] = 0; dout("[%lu] ods: %s\r\n", (unsigned long)(GetTickCount() - t0), msg); }
			break; }
		case EXCEPTION_DEBUG_EVENT: {
			EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord;
			if (er->ExceptionCode == EXCEPTION_BREAKPOINT && firstbp) { firstbp = 0; break; }
			cont = DBG_EXCEPTION_NOT_HANDLED;
			dout("[%lu] %s-chance ", (unsigned long)(GetTickCount() - t0), ev.u.Exception.dwFirstChance ? "first" : "SECOND");
			if (dumps < 8 && (er->ExceptionCode & 0xc0000000) == 0xc0000000) { dumps++; dump_crash(ev.dwThreadId, er); }
			else { DWORD off; const char *m = where((DWORD)(DWORD_PTR)er->ExceptionAddress, &off);
				dout("exception %08lx at %08lx %s+%lx\r\n", (unsigned long)er->ExceptionCode, (unsigned long)(DWORD_PTR)er->ExceptionAddress, m ? m : "?", m ? (unsigned long)off : 0UL); }
			break; }
		case EXIT_PROCESS_DEBUG_EVENT:
			dout("[%lu] process exit code %08lx, thread %lx\r\n", (unsigned long)(GetTickCount() - t0), (unsigned long)ev.u.ExitProcess.dwExitCode, (unsigned long)ev.dwThreadId);
			if (ev.u.ExitProcess.dwExitCode && dumps < 8) { dumps++; dump_thread(ev.dwThreadId); }
			ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
			CloseHandle(dlog);
			return 0;
		}
		ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
	}
	dout("WaitForDebugEvent failed %lu\r\n", (unsigned long)GetLastError());
	CloseHandle(dlog);
	return 1;
}

/* ---- boot: supervisor for the agent, started from the Run key --------------- */

/* RunServices starts us before the logon dialog (no wait for a user), Run
 * after logon in case RunServices did not; a mutex keeps one supervisor. */
static const char *const RUNKEYS[2] = {
	"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
	"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
};
static const char RUNVAL[] = "W98Remote";

static void boot_log(const char *dir, const char *msg, DWORD v)
{
	char p[MAX_PATH + 16], line[200]; SYSTEMTIME t; HANDLE f; DWORD w;
	sprintf(p, "%s\\t61boot.log", dir);
	f = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	SetFilePointer(f, 0, NULL, FILE_END);
	GetLocalTime(&t);
	sprintf(line, "%04u-%02u-%02u %02u:%02u:%02u %s %lu\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg, (unsigned long)v);
	WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
	CloseHandle(f);
}

/* Keeps w98agent.exe (same folder) running: it exits at once when the network
 * is not up yet (it binds a fixed IPv4), so retry every 10 s, forever. */
static int cmd_supervise(void)
{
	char dir[MAX_PATH], exe[MAX_PATH + 16], *s; DWORD code, t0, n = 0;
	STARTUPINFOA si; PROCESS_INFORMATION pi;
	CreateMutexA(NULL, TRUE, "W98RemoteSupervisor");
	if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
	GetModuleFileNameA(NULL, dir, sizeof(dir));
	if ((s = strrchr(dir, '\\')) != NULL) *s = 0;
	sprintf(exe, "%s\\w98agent.exe", dir);
	FreeConsole();
	boot_log(dir, "supervisor up, pid", GetCurrentProcessId());
	for (;;) {
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		t0 = GetTickCount();
		if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
			boot_log(dir, "cannot start w98agent.exe, error", GetLastError());
			Sleep(30000);
			continue;
		}
		boot_log(dir, "agent started, attempt", ++n);
		CloseHandle(pi.hThread);
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		boot_log(dir, "agent exited, code", code);
		Sleep(GetTickCount() - t0 < 30000 ? 10000 : 2000);
	}
	return 0;
}

static int cmd_autostart(const char *what)
{
	char self[MAX_PATH], dst[MAX_PATH], val[MAX_PATH + 16], *s; HKEY k; DWORD type, len; int i;
	if (!strcmp(what, "on")) {
		GetModuleFileNameA(NULL, self, sizeof(self));
		strcpy(dst, self);
		if ((s = strrchr(dst, '\\')) != NULL) strcpy(s + 1, "t61boot.exe");
		if (lstrcmpiA(self, dst) && !CopyFileA(self, dst, FALSE)) {
			if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) return fail("copy", dst);
			out("t61boot.exe is running: kept that copy\n");
		}
		sprintf(val, "%s supervise", dst);
	}
	for (i = 0; i < 2; i++) {
		if (RegCreateKeyA(HKEY_LOCAL_MACHINE, RUNKEYS[i], &k) != ERROR_SUCCESS) return fail("open", RUNKEYS[i]);
		if (!strcmp(what, "on")) {
			if (RegSetValueExA(k, RUNVAL, 0, REG_SZ, (BYTE *)val, (DWORD)strlen(val) + 1) != ERROR_SUCCESS) { RegCloseKey(k); return fail("set", RUNVAL); }
			out("HKLM\\%s\\%s = %s\n", RUNKEYS[i], RUNVAL, val);
		} else if (!strcmp(what, "off")) {
			RegDeleteValueA(k, RUNVAL);
			out("removed HKLM\\%s\\%s\n", RUNKEYS[i], RUNVAL);
		} else {
			len = sizeof(val);
			if (RegQueryValueExA(k, RUNVAL, NULL, &type, (BYTE *)val, &len) == ERROR_SUCCESS) out("HKLM\\%s\\%s = %s\n", RUNKEYS[i], RUNVAL, val);
			else out("HKLM\\%s\\%s: not set\n", RUNKEYS[i], RUNVAL);
		}
		RegCloseKey(k);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE), se = GetStdHandle(STD_ERROR_HANDLE), si = GetStdHandle(STD_INPUT_HANDLE);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	if (!strcmp(c, "supervise")) return cmd_supervise();   /* own console handling */
	if (!strcmp(c, "rebootwatch")) return cmd_rebootwatch(1);
	if (!strcmp(c, "endwatch")) return cmd_rebootwatch(0);
	if (!strcmp(c, "debugrun") && argc >= 3) return cmd_debugrun(argv[2]);
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
	if (!strcmp(c, "wins") && argc <= 3) { EnumWindows(enum_top, (LPARAM)(argc == 3 ? argv[2] : "*")); return 0; }
	if (!strcmp(c, "kids") && argc == 3) { EnumWindows(enum_top_kids, (LPARAM)argv[2]); return 0; }
	if (!strcmp(c, "focus") && argc == 3) return cmd_focus(argv[2], 0);
	if (!strcmp(c, "close") && argc == 3) return cmd_focus(argv[2], 1);
	if (!strcmp(c, "press") && argc == 4) return cmd_press(argv[2], argv[3]);
	if (!strcmp(c, "waitwin") && argc == 4) return cmd_waitwin(argv[2], strtoul(argv[3], NULL, 10));
	if (!strcmp(c, "click") && (argc == 4 || argc == 5)) return cmd_click(atoi(argv[2]), atoi(argv[3]), argc == 5 ? argv[4] : NULL);
	if (!strcmp(c, "key") && argc >= 3) { int i, r = 0; for (i = 2; i < argc && !r; i++) r = cmd_key(argv[i]); return r; }
	if (!strcmp(c, "text") && argc == 3) return cmd_text(argv[2]);
	if (!strcmp(c, "autostart") && argc == 3) return cmd_autostart(argv[2]);
	if (!strcmp(c, "reboot") && (argc == 2 || argc == 3)) {
		/* Default: the Start menu's restart (SHExitWindowsEx), run by a watcher
		 * detached from the agent's console that answers the console's "quit
		 * this program" / End Task dialogs. "force": ExitWindowsEx EWX_FORCE
		 * from here, which hung the T61 after the display Disable once. */
		if (argc == 3 && !strcmp(argv[2], "force")) {
			out("rebooting (forced)\n");
			if (ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0)) return 0;
			return fail("ExitWindowsEx", "");
		}
		return spawn_detached("rebootwatch");
	}
	if (!strcmp(c, "endtasks") && argc == 2) return spawn_detached("endwatch");
	if (!strcmp(c, "debug") && argc >= 3) {
		char mode[1024];
		snprintf(mode, sizeof(mode), "debugrun %s", rest_of_line(2));
		return spawn_detached(mode);
	}
	out("usage: t61util info | dir P | type P | crc P | copy S D | part S OFF LEN D | join D P... |\n"
	       "               del NAME | mkdir P | exec SECS EXE [ARGS] | sh SECS LINE | start EXE [ARGS] |\n"
	       "               ps | kill NAME | shot FILE.bmp | wins [TITLE] | kids TITLE |\n"
	       "               focus TITLE | close TITLE | press TITLE BUTTON | waitwin TITLE SECS |\n"
	       "               click X Y [right|double] | key COMBO... | text STRING | autostart on|off|show\n"
	       "               reboot [force]\n");
	return 2;
}
