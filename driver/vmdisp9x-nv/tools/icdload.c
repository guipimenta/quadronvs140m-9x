/* icdload - do by hand what Win9x opengl32.dll does to find and load the
 * OpenGL ICD: OPENGL_GETINFO escape, registry lookup, LoadLibrary,
 * DrvValidateVersion, DrvDescribePixelFormat (format count).
 * Build: i686-w64-mingw32-gcc -O2 -s -o icdload.exe icdload.c -lgdi32 -ladvapi32 */
#include <windows.h>
#include <stdio.h>
typedef BOOL (APIENTRY *PFN_VV)(ULONG);
typedef LONG (APIENTRY *PFN_DPF)(HDC, LONG, ULONG, PIXELFORMATDESCRIPTOR*);
typedef struct { DWORD ver, drvver; char dll[262]; } icdinfo_t;
int main(int argc, char **argv)
{
	HKEY k; char val[260]; DWORD t, n = sizeof(val);
	HDC sdc, dc; HWND w; HMODULE h; PFN_VV vv; PFN_DPF dpf; LONG r; WORD esc = 0x1101;
	icdinfo_t info; PIXELFORMATDESCRIPTOR pfd; const char *dll = "mesa3d.dll";
	if(argc > 1) dll = argv[1];
	if(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\OpenGLDrivers", 0, KEY_READ, &k) == 0)
	{
		if(RegQueryValueExA(k, "SOFTWARE", 0, &t, (BYTE*)val, &n) == 0) printf("registry OpenGLDrivers\\SOFTWARE = %s\n", val);
		else printf("registry: OpenGLDrivers has no SOFTWARE value\n");
		RegCloseKey(k);
	}
	else printf("registry: no OpenGLDrivers key\n");
	sdc = GetDC(NULL);
	printf("screen %dx%d %d bpp\n", GetDeviceCaps(sdc, HORZRES), GetDeviceCaps(sdc, VERTRES), GetDeviceCaps(sdc, BITSPIXEL));
	r = Escape(sdc, QUERYESCSUPPORT, sizeof(esc), (LPCSTR)&esc, NULL);
	printf("QUERYESCSUPPORT(OPENGL_GETINFO) = %ld\n", r);
	memset(&info, 0, sizeof(info));
	r = ExtEscape(sdc, 0x1101, 0, NULL, sizeof(info), (LPSTR)&info);
	printf("OPENGL_GETINFO rc=%ld ver=%lu drvver=%lu name='%s'\n", r, info.ver, info.drvver, info.dll);
	{
		MEMORYSTATUS ms; ms.dwLength = sizeof(ms); GlobalMemoryStatus(&ms);
		printf("memory: phys %lu/%lu MB  pagefile %lu/%lu MB  virtual %lu/%lu MB\n",
			ms.dwAvailPhys >> 20, ms.dwTotalPhys >> 20, ms.dwAvailPageFile >> 20, ms.dwTotalPageFile >> 20,
			ms.dwAvailVirtual >> 20, ms.dwTotalVirtual >> 20);
		h = LoadLibraryExA(dll, NULL, LOAD_LIBRARY_AS_DATAFILE);
		printf("LoadLibraryEx(%s, AS_DATAFILE) = %p err=%lu\n", dll, (void*)h, h ? 0 : GetLastError());
		if(h) FreeLibrary(h);
		h = LoadLibraryExA(dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
		printf("LoadLibraryEx(%s, DONT_RESOLVE) = %p err=%lu\n", dll, (void*)h, h ? 0 : GetLastError());
		if(h) FreeLibrary(h);
	}
	fflush(stdout);
	h = LoadLibraryA(dll);
	printf("LoadLibrary(%s) = %p err=%lu\n", dll, (void*)h, h ? 0 : GetLastError());
	fflush(stdout);
	if(!h) return 1;
	vv = (PFN_VV)GetProcAddress(h, "DrvValidateVersion");
	dpf = (PFN_DPF)GetProcAddress(h, "DrvDescribePixelFormat");
	printf("DrvValidateVersion=%p DrvDescribePixelFormat=%p\n", (void*)vv, (void*)dpf);
	if(vv) { printf("DrvValidateVersion(1) = %d\n", vv(1)); fflush(stdout); }
	if(dpf)
	{
		memset(&pfd, 0, sizeof(pfd));
		r = dpf(sdc, 1, sizeof(pfd), &pfd);
		printf("DrvDescribePixelFormat(screen dc) = %ld  #1 flags=%08lX color=%u depth=%u stencil=%u\n", r, pfd.dwFlags, pfd.cColorBits, pfd.cDepthBits, pfd.cStencilBits);
		fflush(stdout);
		w = CreateWindowA("STATIC", "icdload", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 240, NULL, NULL, GetModuleHandle(NULL), NULL);
		dc = GetDC(w);
		memset(&pfd, 0, sizeof(pfd));
		r = dpf(dc, 1, sizeof(pfd), &pfd);
		printf("DrvDescribePixelFormat(window dc) = %ld  #1 flags=%08lX color=%u depth=%u stencil=%u\n", r, pfd.dwFlags, pfd.cColorBits, pfd.cDepthBits, pfd.cStencilBits);
		ReleaseDC(w, dc); DestroyWindow(w);
	}
	ReleaseDC(NULL, sdc);
	printf("done\n");
	return 0;
}
