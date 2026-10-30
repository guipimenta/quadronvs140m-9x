/* d3d8test.exe - the Direct3D 8 path dxdiag's "Direct3D 8" test takes, one
 * logged step at a time (build 3116 investigation: dxdiag's DX8 test ends the
 * process with code -1 right after Mesa creates its context, with no
 * exception). Every step is written and flushed to d3d8test.txt BEFORE the
 * call, so the last line names the call that never returned.
 *
 *   d3d8test [fullscreen] [frames N] [sw] [repeat N]
 *     sw = software vertex processing; repeat = create/draw/release the whole
 *     device N times in one process (dxdiag destroys its DX7 device and creates
 *     the DX8 one in the same process).
 *   cube = also what dxdiag's spinning cube uses: vertex + index buffers in the
 *     default pool, a 256x256 managed texture filled through LockRect,
 *     transforms, a light and DrawIndexedPrimitive.
 *
 * Build: i686-w64-mingw32-gcc -O2 -s -Wall driver/vmdisp9x-nv/tools/d3d8test.c -o driver/vmdisp9x-nv/tools/d3d8test.exe -ld3d8 -lgdi32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static HANDLE g_log;
static void say(const char *fmt, ...)
{
	char b[512]; va_list ap; DWORD w; int n;
	va_start(ap, fmt);
	n = _vsnprintf(b, sizeof(b) - 3, fmt, ap);
	va_end(ap);
	if (n < 0) n = sizeof(b) - 3;
	b[n++] = '\r'; b[n++] = '\n';
	WriteFile(g_log, b, (DWORD)n, &w, NULL);
	FlushFileBuffers(g_log);
	fwrite(b, 1, (size_t)n, stdout);
	fflush(stdout);
}
#define STEP(desc, call) do { HRESULT _hr; say("-> %s", desc); _hr = (call); say("   %s = 0x%08lx", desc, (unsigned long)_hr); if (FAILED(_hr)) fails++; } while (0)

struct vtx { float x, y, z, rhw; DWORD c; };

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

int main(int argc, char **argv)
{
	int i, full = 0, frames = 30, sw = 0, fails = 0, repeat = 1, rep, cube = 0;
	IDirect3DVertexBuffer8 *vb = NULL; IDirect3DIndexBuffer8 *ib = NULL; IDirect3DTexture8 *tex = NULL;
	IDirect3D8 *d3d; IDirect3DDevice8 *dev = NULL;
	D3DPRESENT_PARAMETERS pp; D3DDISPLAYMODE dm; D3DCAPS8 caps; D3DADAPTER_IDENTIFIER8 id;
	WNDCLASSA wc; HWND wnd; MSG msg;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "fullscreen")) full = 1;
		else if (!strcmp(argv[i], "sw")) sw = 1;
		else if (!strcmp(argv[i], "frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
		else if (!strcmp(argv[i], "cube")) cube = 1;
	}
	g_log = CreateFileA("d3d8test.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	say("d3d8test (build 3116): %s, %d frames, %s vertex processing", full ? "fullscreen 640x480" : "windowed 640x480", frames, sw ? "software" : "hardware if offered");

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d8test";
	wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	RegisterClassA(&wc);
	wnd = CreateWindowA("d3d8test", "d3d8test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 640 + 8, 480 + 27, NULL, NULL, wc.hInstance, NULL);

	for (rep = 0; rep < repeat; rep++) {
	say("=== round %d of %d", rep + 1, repeat);
	say("-> Direct3DCreate8(%u)", D3D_SDK_VERSION);
	d3d = Direct3DCreate8(D3D_SDK_VERSION);
	say("   Direct3DCreate8 = %p", (void *)d3d);
	if (!d3d) return 1;
	STEP("GetAdapterIdentifier", IDirect3D8_GetAdapterIdentifier(d3d, 0, 0, &id));
	say("   adapter: %s / %s", id.Description, id.Driver);
	STEP("GetAdapterDisplayMode", IDirect3D8_GetAdapterDisplayMode(d3d, 0, &dm));
	say("   mode %ux%u format %u", dm.Width, dm.Height, (unsigned)dm.Format);
	STEP("GetDeviceCaps(HAL)", IDirect3D8_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps));
	say("   DevCaps %08lx, TnL %s, PrimitiveMiscCaps %08lx, MaxTextureWidth %lu, VertexShaderVersion %08lx, PixelShaderVersion %08lx",
	    (unsigned long)caps.DevCaps, (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? "yes" : "no", (unsigned long)caps.PrimitiveMiscCaps,
	    (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion);
	STEP("CheckDeviceType(HAL, X8R8G8B8)", IDirect3D8_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, dm.Format, full ? D3DFMT_X8R8G8B8 : dm.Format, !full));
	STEP("CheckDepthStencilMatch(D16)", IDirect3D8_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, dm.Format, dm.Format, D3DFMT_D16));

	memset(&pp, 0, sizeof(pp));
	pp.Windowed = !full;
	pp.SwapEffect = full ? D3DSWAPEFFECT_FLIP : D3DSWAPEFFECT_DISCARD;
	pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
	pp.BackBufferFormat = full ? D3DFMT_X8R8G8B8 : dm.Format;
	pp.BackBufferCount = 1;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D16;
	pp.hDeviceWindow = wnd;
	STEP("CreateDevice(HAL)", IDirect3D8_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd,
	     (sw || !(caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
	if (!dev) { say("RESULT: FAIL (no device)"); return 1; }

	if (cube) {
		struct cv { float x, y, z, nx, ny, nz, u, v; } *vp;
		WORD *ip; D3DLOCKED_RECT lr; BYTE *bp; D3DMATRIX m; D3DLIGHT8 l; D3DMATERIAL8 mat; int f, k, y, x;
		static const float fn[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
		STEP("CreateVertexBuffer(24 x pos/normal/uv, WRITEONLY, DEFAULT)", IDirect3DDevice8_CreateVertexBuffer(dev, 24 * sizeof(*vp),
		     D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vb));
		STEP("VB Lock", IDirect3DVertexBuffer8_Lock(vb, 0, 0, (BYTE **)&vp, 0));
		for (f = 0; f < 6; f++) for (k = 0; k < 4; k++) {
			float u = (k & 1) ? 1.f : -1.f, v = (k & 2) ? 1.f : -1.f, *n = (float *)fn[f];
			struct cv *c = &vp[f * 4 + k];
			c->nx = n[0]; c->ny = n[1]; c->nz = n[2];
			c->x = n[0] ? n[0] : u; c->y = n[1] ? n[1] : (n[0] ? u : v); c->z = n[2] ? n[2] : v;
			c->u = (k & 1); c->v = (k & 2) >> 1;
		}
		STEP("VB Unlock", IDirect3DVertexBuffer8_Unlock(vb));
		STEP("CreateIndexBuffer(36 x 16-bit, WRITEONLY, DEFAULT)", IDirect3DDevice8_CreateIndexBuffer(dev, 36 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib));
		STEP("IB Lock", IDirect3DIndexBuffer8_Lock(ib, 0, 0, (BYTE **)&ip, 0));
		for (f = 0; f < 6; f++) { WORD b = (WORD)(f * 4); ip[f*6+0]=b; ip[f*6+1]=b+1; ip[f*6+2]=b+2; ip[f*6+3]=b+2; ip[f*6+4]=b+1; ip[f*6+5]=b+3; }
		STEP("IB Unlock", IDirect3DIndexBuffer8_Unlock(ib));
		STEP("CreateTexture(256x256 X8R8G8B8, MANAGED)", IDirect3DDevice8_CreateTexture(dev, 256, 256, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex));
		STEP("Texture LockRect", IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0));
		for (y = 0; y < 256; y++) for (x = 0, bp = (BYTE *)lr.pBits + y * lr.Pitch; x < 256; x++)
			((DWORD *)bp)[x] = ((x ^ y) & 32) ? 0xff40c040 : 0xff202020;
		STEP("Texture UnlockRect", IDirect3DTexture8_UnlockRect(tex, 0));
		memset(&m, 0, sizeof(m)); m._11 = m._22 = m._33 = m._44 = 1;
		STEP("SetTransform(WORLD)", IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &m));
		m._43 = 5;
		STEP("SetTransform(VIEW)", IDirect3DDevice8_SetTransform(dev, D3DTS_VIEW, &m));
		memset(&m, 0, sizeof(m)); m._11 = 1.8f; m._22 = 2.4f; m._33 = 1.0f; m._34 = 1; m._43 = -0.1f;
		STEP("SetTransform(PROJECTION)", IDirect3DDevice8_SetTransform(dev, D3DTS_PROJECTION, &m));
		memset(&l, 0, sizeof(l)); l.Type = D3DLIGHT_DIRECTIONAL; l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = 1; l.Direction.z = 1;
		STEP("SetLight", IDirect3DDevice8_SetLight(dev, 0, &l));
		STEP("LightEnable", IDirect3DDevice8_LightEnable(dev, 0, TRUE));
		memset(&mat, 0, sizeof(mat)); mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1; mat.Ambient = mat.Diffuse;
		STEP("SetMaterial", IDirect3DDevice8_SetMaterial(dev, &mat));
		STEP("SetRenderState(LIGHTING)", IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, TRUE));
		STEP("SetRenderState(AMBIENT)", IDirect3DDevice8_SetRenderState(dev, D3DRS_AMBIENT, 0xff404040));
		STEP("SetTexture", IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex));
		STEP("SetVertexShader(XYZ|NORMAL|TEX1)", IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1));
		STEP("SetStreamSource", IDirect3DDevice8_SetStreamSource(dev, 0, vb, sizeof(*vp)));
		STEP("SetIndices", IDirect3DDevice8_SetIndices(dev, ib, 0));
	}

	for (i = 0; i < frames; i++) {
		float a = i * 0.1f;
		struct vtx v[3] = {
			{ 320 + 200 * (float)__builtin_cos(a),        240 + 200 * (float)__builtin_sin(a),        0.5f, 1, 0xffff0000 },
			{ 320 + 200 * (float)__builtin_cos(a + 2.09f), 240 + 200 * (float)__builtin_sin(a + 2.09f), 0.5f, 1, 0xff00ff00 },
			{ 320 + 200 * (float)__builtin_cos(a + 4.19f), 240 + 200 * (float)__builtin_sin(a + 4.19f), 0.5f, 1, 0xff0000ff },
		};
		int log = i < 2 || i == frames - 1;
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
		if (log) say("frame %d", i);
		if (cube) {
			D3DMATRIX w; float c = (float)__builtin_cos(a), sn = (float)__builtin_sin(a);
			memset(&w, 0, sizeof(w)); w._11 = c; w._13 = -sn; w._22 = 1; w._31 = sn; w._33 = c; w._44 = 1;
			IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &w);
			if (log) {
				STEP("Clear", IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0));
				STEP("BeginScene", IDirect3DDevice8_BeginScene(dev));
				STEP("DrawIndexedPrimitive", IDirect3DDevice8_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 24, 0, 12));
				STEP("EndScene", IDirect3DDevice8_EndScene(dev));
				STEP("Present", IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL));
			} else {
				IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
				IDirect3DDevice8_BeginScene(dev);
				if (FAILED(IDirect3DDevice8_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 24, 0, 12))) fails++;
				IDirect3DDevice8_EndScene(dev);
				if (FAILED(IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL))) fails++;
			}
			continue;
		}
		if (log) {
			STEP("Clear", IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202040, 1.0f, 0));
			STEP("BeginScene", IDirect3DDevice8_BeginScene(dev));
			STEP("SetVertexShader(XYZRHW|DIFFUSE)", IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
			STEP("DrawPrimitiveUP", IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(v[0])));
			STEP("EndScene", IDirect3DDevice8_EndScene(dev));
			STEP("Present", IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL));
		} else {
			if (FAILED(IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202040, 1.0f, 0))) fails++;
			IDirect3DDevice8_BeginScene(dev);
			IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
			if (FAILED(IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(v[0])))) fails++;
			IDirect3DDevice8_EndScene(dev);
			if (FAILED(IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL))) fails++;
		}
	}
	if (tex) { say("-> Release texture"); IDirect3DTexture8_Release(tex); tex = NULL; }
	if (ib) { say("-> Release index buffer"); IDirect3DIndexBuffer8_Release(ib); ib = NULL; }
	if (vb) { say("-> Release vertex buffer"); IDirect3DVertexBuffer8_Release(vb); vb = NULL; }
	say("-> Release device");
	IDirect3DDevice8_Release(dev);
	say("-> Release d3d");
	IDirect3D8_Release(d3d);
	}
	DestroyWindow(wnd);
	say("RESULT: %s (%d failed calls)", fails ? "FAIL" : "PASS", fails);
	CloseHandle(g_log);
	return fails ? 1 : 0;
}
