/* pfprobe - why does GDI ChoosePixelFormat() return 0 / error 126 in a process
 * that has mesa3d.dll loaded? Repeats the call at each step wgltest takes.
 * Usage: pfprobe [mesa3d.dll]   (prints to stdout; redirect to a file)
 * Build: i686-w64-mingw32-gcc -O2 -s -o pfprobe.exe pfprobe.c -lgdi32 -luser32 */
#include <windows.h>
#include <stdio.h>
typedef LONG (APIENTRY *PFN_DPF)(HDC, LONG, ULONG, PIXELFORMATDESCRIPTOR*);
static PIXELFORMATDESCRIPTOR pfd = { sizeof(PIXELFORMATDESCRIPTOR), 1,
	PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32,
	0,0,0,0,0,0, 0, 0, 0, 0,0,0,0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0,0,0 };
static void show(const char *tag, HDC dc)
{
	int r; DWORD e;
	SetLastError(0); r = ChoosePixelFormat(dc, &pfd); e = GetLastError();
	printf("%-34s ChoosePixelFormat -> %d err=%lu   opengl32=%p glu32=%p\n", tag, r, e,
		(void*)GetModuleHandleA("opengl32.dll"), (void*)GetModuleHandleA("glu32.dll"));
	fflush(stdout);
}
int main(int argc, char **argv)
{
	const char *dll = argc > 1 ? argv[1] : "mesa3d.dll";
	WNDCLASSA wc; HWND win; HDC dc; HMODULE gl, m; PFN_DPF dpf; LONG n; int c; PIXELFORMATDESCRIPTOR p2;
	memset(&wc, 0, sizeof(wc)); wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
	wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND); wc.lpszClassName = "pfprobe"; wc.style = CS_OWNDC;
	RegisterClassA(&wc);
	win = CreateWindowA("pfprobe", "pfprobe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 20, 20, 0, 0, NULL, 0);
	dc = GetDC(win);
	printf("pfprobe: dll=%s hwnd=%p dc=%p screen %d bpp\n", dll, (void*)win, (void*)dc, GetDeviceCaps(dc, BITSPIXEL)); fflush(stdout);
	show("A: fresh process", dc);
	SetLastError(0); gl = LoadLibraryA("opengl32.dll");
	printf("B: LoadLibrary(opengl32.dll) = %p err=%lu\n", (void*)gl, gl ? 0 : GetLastError()); fflush(stdout);
	show("B: opengl32 loaded explicitly", dc);
	if (gl) FreeLibrary(gl);
	show("B2: after FreeLibrary(opengl32)", dc);
	SetLastError(0); m = LoadLibraryA(dll);
	printf("C: LoadLibrary(%s) = %p err=%lu\n", dll, (void*)m, m ? 0 : GetLastError()); fflush(stdout);
	show("C: after mesa3d load", dc);
	dpf = m ? (PFN_DPF)GetProcAddress(m, "DrvDescribePixelFormat") : NULL;
	n = dpf ? dpf(dc, 0, 0, NULL) : -1;
	printf("D: DrvDescribePixelFormat(dc,0,0,NULL) = %ld\n", n); fflush(stdout);
	show("D: after Mesa screen create", dc);
	SetLastError(0); gl = LoadLibraryA("opengl32.dll");
	printf("E: LoadLibrary(opengl32.dll) = %p err=%lu\n", (void*)gl, gl ? 0 : GetLastError()); fflush(stdout);
	show("E: opengl32 loaded again", dc);
	show("F: screen DC", GetDC(NULL));
	SetLastError(0); c = DescribePixelFormat(dc, 1, sizeof(p2), &p2);
	printf("G: GDI DescribePixelFormat(dc,1) -> %d err=%lu\n", c, GetLastError());
	SetLastError(0); c = GetPixelFormat(dc);
	printf("H: GDI GetPixelFormat(dc) -> %d err=%lu\n", c, GetLastError());
	printf("done\n"); fflush(stdout);
	return 0;
}
