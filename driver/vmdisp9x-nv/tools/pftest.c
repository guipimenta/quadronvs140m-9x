/*
 * pftest - reproduce Half-Life's OpenGL pixel-format selection on Win9x.
 *
 * hw.dll (GL_Init) does: GDI ChoosePixelFormat() with flags
 * DOUBLEBUFFER|DRAW_TO_WINDOW|SUPPORT_OPENGL, 24 colour bits, 32 depth bits;
 * SetPixelFormat(); DescribePixelFormat(); then REFUSES (silently, the
 * launcher reports "The selected OpenGL mode is not supported by your video
 * card") when the format has PFD_GENERIC_FORMAT and not
 * PFD_GENERIC_ACCELERATED, unless gl_allowsoftware is set.
 *
 * Usage: pftest [W H BPP]   (switches the display with ChangeDisplaySettings
 *                           first, like a fullscreen game, restores on exit)
 * Output goes to the console and is appended to pftest.txt.
 *
 * Build: i686-w64-mingw32-gcc -O2 -s -o pftest.exe pftest.c -lgdi32 -lopengl32
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdarg.h>

static FILE *logf;

static void out(const char *fmt, ...)
{
	va_list ap;
	char buf[1024];
	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	if(logf) { fputs(buf, logf); fflush(logf); }
}

static void describe(HDC dc, int idx, PIXELFORMATDESCRIPTOR *pfd)
{
	DWORD f = pfd->dwFlags;
	out("  #%2d flags=%08lX%s%s%s%s%s%s type=%s color=%u(%u/%u/%u/%u) depth=%u stencil=%u accum=%u aux=%u",
		idx, f,
		(f & PFD_GENERIC_FORMAT) ? " GENERIC" : "",
		(f & PFD_GENERIC_ACCELERATED) ? " GEN_ACCEL" : "",
		(f & PFD_DOUBLEBUFFER) ? " DBL" : "",
		(f & PFD_DRAW_TO_WINDOW) ? " WIN" : "",
		(f & PFD_SUPPORT_OPENGL) ? " GL" : "",
		(f & PFD_SUPPORT_GDI) ? " GDI" : "",
		pfd->iPixelType == PFD_TYPE_RGBA ? "RGBA" : "INDEX",
		pfd->cColorBits, pfd->cRedBits, pfd->cGreenBits, pfd->cBlueBits, pfd->cAlphaBits,
		pfd->cDepthBits, pfd->cStencilBits, pfd->cAccumBits, pfd->cAuxBuffers);
	if(dc) { /* nothing */ }
	out("\n");
}

static int try_choose(HDC dc, const char *label, DWORD flags, BYTE color, BYTE depth, BYTE stencil)
{
	PIXELFORMATDESCRIPTOR pfd;
	int idx;
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = flags;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = color;
	pfd.cDepthBits = depth;
	pfd.cStencilBits = stencil;
	pfd.iLayerType = PFD_MAIN_PLANE;
	idx = ChoosePixelFormat(dc, &pfd);
	out("ChoosePixelFormat(%s: flags=%02lX color=%u depth=%u stencil=%u) -> %d (err %lu)\n",
		label, flags, color, depth, stencil, idx, idx ? 0 : GetLastError());
	if(idx > 0)
	{
		memset(&pfd, 0, sizeof(pfd));
		if(DescribePixelFormat(dc, idx, sizeof(pfd), &pfd))
		{
			describe(dc, idx, &pfd);
			if((pfd.dwFlags & PFD_GENERIC_FORMAT) && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED))
				out("  => Half-Life would REFUSE this format (generic, not accelerated)\n");
			else
				out("  => Half-Life would ACCEPT this format\n");
		}
	}
	return idx;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if(m == WM_CLOSE) { PostQuitMessage(0); return 0; }
	return DefWindowProc(h, m, w, l);
}

int main(int argc, char **argv)
{
	WNDCLASSA wc;
	HWND hwnd;
	HDC dc;
	int w = 640, h = 480, bpp = 0, n, i, idx, switched = 0;
	DEVMODEA dm;
	PIXELFORMATDESCRIPTOR pfd;
	HGLRC rc;
	SYSTEMTIME st;

	logf = fopen("pftest.txt", "a");
	GetLocalTime(&st);
	out("\n==== pftest %04d-%02d-%02d %02d:%02d:%02d ====\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	if(argc >= 4)
	{
		w = atoi(argv[1]); h = atoi(argv[2]); bpp = atoi(argv[3]);
		memset(&dm, 0, sizeof(dm));
		dm.dmSize = sizeof(dm);
		dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = bpp;
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		n = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
		out("ChangeDisplaySettings(%dx%dx%d) -> %d\n", w, h, bpp, n);
		switched = (n == DISP_CHANGE_SUCCESSFUL);
	}
	{
		HDC sdc = GetDC(NULL);
		out("screen: %dx%d, %d bpp, planes %d\n", GetDeviceCaps(sdc, HORZRES), GetDeviceCaps(sdc, VERTRES),
			GetDeviceCaps(sdc, BITSPIXEL), GetDeviceCaps(sdc, PLANES));
		ReleaseDC(NULL, sdc);
	}

	memset(&wc, 0, sizeof(wc));
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = "pftestcls";
	RegisterClassA(&wc);
	hwnd = CreateWindowA("pftestcls", "pftest",
		switched ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE),
		0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
	if(!hwnd) { out("CreateWindow failed %lu\n", GetLastError()); return 1; }
	dc = GetDC(hwnd);

	memset(&pfd, 0, sizeof(pfd));
	n = DescribePixelFormat(dc, 1, sizeof(pfd), &pfd);
	out("DescribePixelFormat: %d pixel formats\n", n);
	for(i = 1; i <= n && i <= 64; i++)
	{
		memset(&pfd, 0, sizeof(pfd));
		if(DescribePixelFormat(dc, i, sizeof(pfd), &pfd)) describe(dc, i, &pfd);
	}

	/* exactly what hw.dll asks for */
	idx = try_choose(dc, "Half-Life", PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL, 24, 32, 0);
	try_choose(dc, "16/16", PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL, 16, 16, 0);
	try_choose(dc, "32/24/8", PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL, 32, 24, 8);
	try_choose(dc, "24/24", PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL, 24, 24, 0);

	if(idx > 0)
	{
		memset(&pfd, 0, sizeof(pfd));
		DescribePixelFormat(dc, idx, sizeof(pfd), &pfd);
		if(!SetPixelFormat(dc, idx, &pfd)) out("SetPixelFormat(%d) FAILED %lu\n", idx, GetLastError());
		else
		{
			out("SetPixelFormat(%d) ok\n", idx);
			rc = wglCreateContext(dc);
			if(!rc) out("wglCreateContext FAILED %lu\n", GetLastError());
			else if(!wglMakeCurrent(dc, rc)) out("wglMakeCurrent FAILED %lu\n", GetLastError());
			else
			{
				out("GL_VENDOR:   %s\n", (const char*)glGetString(GL_VENDOR));
				out("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));
				out("GL_VERSION:  %s\n", (const char*)glGetString(GL_VERSION));
				glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
				SwapBuffers(dc);
				Sleep(500);
				wglMakeCurrent(NULL, NULL);
			}
			if(rc) wglDeleteContext(rc);
		}
	}

	ReleaseDC(hwnd, dc);
	DestroyWindow(hwnd);
	if(switched) ChangeDisplaySettingsA(NULL, 0);
	out("done\n");
	if(logf) fclose(logf);
	return 0;
}
