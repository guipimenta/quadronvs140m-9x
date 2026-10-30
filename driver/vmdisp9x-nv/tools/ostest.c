/*
 * ostest.c - build 3116: test mesa3d.dll's OSMesa path the way the Direct3D HAL
 * (vmhal3d.dll, JHRobotics/vmhal9x mesa3d*.c) uses it, without DirectDraw.
 *
 * vmhal3d loads mesa3d.dll, finds OSMesaGetProcAddress and renders every
 * Direct3D frame through an OSMesa context: a "plane" FBO per render target
 * (RGBA colour texture + DEPTH24_STENCIL8 texture attached as depth AND
 * stencil), the DirectDraw surface uploaded into the colour texture
 * (glTexSubImage2D BGRA for 32 bpp; for 16 bpp an RGB 5-6-5 texture in its own
 * FBO blitted into the plane with glBlitFramebuffer), the scene drawn, then
 * glFinish + glReadPixels back into the surface. ostest does exactly that for
 * N frames at 32 bpp and at 16 bpp, and checks pixels: the uploaded
 * background (upload -> GPU -> readback round trip), a depth-tested opaque
 * quad, a nearest-filtered checker texture, a 50% alpha blend over the
 * background, and a stencil-masked quad.
 *
 *   ostest [dll mesa3d.dll] [frames 60] [width 640] [height 480]
 *
 * Writes ostest.txt in the current folder (and the same to stdout). The
 * renderer line says which screen OSMesa got: NV86 = the GPU (3116), llvmpipe =
 * software (GALLIUM_DRIVER=llvmpipe, NVSHIM_D3D=0, the GPU busy in another
 * process, or a pre-3116 mesa3d.dll). Under the shim's fake GPU (NVSHIM_FAKE=1)
 * nothing is really drawn, so the pixel checks are reported but not judged.
 *
 * Build: i686-w64-mingw32-gcc -O2 -s tools/ostest.c -o tools/ostest.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- the GL / OSMesa subset we need (no GL headers: everything by pointer) ---- */
typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLbitfield;
typedef float         GLfloat;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef void          GLvoid;

#define GL_NO_ERROR                 0
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_STENCIL_BUFFER_BIT       0x00000400
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_QUADS                    0x0007
#define GL_NEVER                    0x0200
#define GL_LESS                     0x0201
#define GL_EQUAL                    0x0202
#define GL_ALWAYS                   0x0207
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DEPTH_TEST               0x0B71
#define GL_STENCIL_TEST             0x0B90
#define GL_BLEND                    0x0BE2
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNSIGNED_BYTE            0x1401
#define GL_UNSIGNED_INT             0x1405
#define GL_MODELVIEW                0x1700
#define GL_PROJECTION               0x1701
#define GL_TEXTURE                  0x1702
#define GL_RGB                      0x1907
#define GL_RGBA                     0x1908
#define GL_KEEP                     0x1E00
#define GL_REPLACE                  0x1E01
#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_NEAREST                  0x2600
#define GL_TEXTURE_ENV_MODE         0x2200
#define GL_TEXTURE_ENV              0x2300
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_BGRA                     0x80E1
#define GL_UNSIGNED_SHORT_5_6_5     0x8363
#define GL_TEXTURE0                 0x84C0
#define GL_DEPTH_STENCIL            0x84F9
#define GL_UNSIGNED_INT_24_8        0x84FA
#define GL_DEPTH24_STENCIL8         0x88F0
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_DEPTH_ATTACHMENT         0x8D00
#define GL_STENCIL_ATTACHMENT       0x8D20
#define GL_FRAMEBUFFER              0x8D40

#define OSMESA_RGBA                 GL_RGBA
#define OSMESA_Y_UP                 0x11

typedef void *OSMesaContext;
typedef void (*PROC_t)(void);
typedef OSMesaContext (__stdcall *OSMesaCreateContextExt_t)(GLenum, GLint, GLint, GLint, OSMesaContext);
typedef GLboolean (__stdcall *OSMesaMakeCurrent_t)(OSMesaContext, void *, GLenum, GLsizei, GLsizei);
typedef void (__stdcall *OSMesaDestroyContext_t)(OSMesaContext);
typedef void (__stdcall *OSMesaPixelStore_t)(GLint, GLint);
typedef PROC_t (__stdcall *OSMesaGetProcAddress_t)(const char *);

/* GL entry points (Mesa's dispatch stubs are __stdcall on 32-bit Windows) */
#define GLAPIENTRY __stdcall
static struct {
	GLenum (GLAPIENTRY *GetError)(void);
	const GLubyte *(GLAPIENTRY *GetString)(GLenum);
	void (GLAPIENTRY *Enable)(GLenum);
	void (GLAPIENTRY *Disable)(GLenum);
	void (GLAPIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (GLAPIENTRY *Clear)(GLbitfield);
	void (GLAPIENTRY *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (GLAPIENTRY *ClearDepth)(double);
	void (GLAPIENTRY *ClearStencil)(GLint);
	void (GLAPIENTRY *MatrixMode)(GLenum);
	void (GLAPIENTRY *LoadIdentity)(void);
	void (GLAPIENTRY *Ortho)(double, double, double, double, double, double);
	void (GLAPIENTRY *PushMatrix)(void);
	void (GLAPIENTRY *PopMatrix)(void);
	void (GLAPIENTRY *Begin)(GLenum);
	void (GLAPIENTRY *End)(void);
	void (GLAPIENTRY *Vertex3f)(GLfloat, GLfloat, GLfloat);
	void (GLAPIENTRY *Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (GLAPIENTRY *TexCoord2f)(GLfloat, GLfloat);
	void (GLAPIENTRY *DepthFunc)(GLenum);
	void (GLAPIENTRY *DepthMask)(GLboolean);
	void (GLAPIENTRY *BlendFunc)(GLenum, GLenum);
	void (GLAPIENTRY *StencilFunc)(GLenum, GLint, GLuint);
	void (GLAPIENTRY *StencilOp)(GLenum, GLenum, GLenum);
	void (GLAPIENTRY *StencilMask)(GLuint);
	void (GLAPIENTRY *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
	void (GLAPIENTRY *GenTextures)(GLsizei, GLuint *);
	void (GLAPIENTRY *DeleteTextures)(GLsizei, const GLuint *);
	void (GLAPIENTRY *BindTexture)(GLenum, GLuint);
	void (GLAPIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
	void (GLAPIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
	void (GLAPIENTRY *TexParameteri)(GLenum, GLenum, GLint);
	void (GLAPIENTRY *TexEnvi)(GLenum, GLenum, GLint);
	void (GLAPIENTRY *PixelStorei)(GLenum, GLint);
	void (GLAPIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
	void (GLAPIENTRY *Finish)(void);
	void (GLAPIENTRY *ActiveTexture)(GLenum);
	void (GLAPIENTRY *GenFramebuffers)(GLsizei, GLuint *);
	void (GLAPIENTRY *DeleteFramebuffers)(GLsizei, const GLuint *);
	void (GLAPIENTRY *BindFramebuffer)(GLenum, GLuint);
	void (GLAPIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	GLenum (GLAPIENTRY *CheckFramebufferStatus)(GLenum);
	void (GLAPIENTRY *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
} gl;

static FILE *g_rep;
static void rep(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	if(g_rep) { va_start(ap, fmt); vfprintf(g_rep, fmt, ap); va_end(ap); fflush(g_rep); }
	fflush(stdout);
}

static OSMesaGetProcAddress_t pGetProc;
static int g_missing = 0;
static void *getp(const char *name)
{
	void *p = (void *)pGetProc(name);
	if(!p) { rep("MISSING GL entry point %s\n", name); g_missing++; }
	return p;
}
#define LOAD(f) *(void **)&gl.f = getp("gl" #f)

static int W = 640, H = 480;
static int g_errors = 0, g_bad = 0, g_checks = 0, g_judge = 1;

static void glerr(const char *where, int frame)
{
	GLenum e;
	while((e = gl.GetError()) != GL_NO_ERROR)
	{
		if(g_errors < 20) rep("GL ERROR 0x%04X at %s (frame %d)\n", e, where, frame);
		g_errors++;
	}
}

static void quad(float x0, float y0, float x1, float y1, float z, int tex)
{
	gl.Begin(GL_QUADS);
	if(tex) gl.TexCoord2f(0, 0); gl.Vertex3f(x0, y0, z);
	if(tex) gl.TexCoord2f(1, 0); gl.Vertex3f(x1, y0, z);
	if(tex) gl.TexCoord2f(1, 1); gl.Vertex3f(x1, y1, z);
	if(tex) gl.TexCoord2f(0, 1); gl.Vertex3f(x0, y1, z);
	gl.End();
}

/* expected colours, 8-bit per channel */
typedef struct { int r, g, b; } rgb_t;

static rgb_t bg32(int x, int y, int f) { rgb_t c; c.r = 0x40; c.g = y & 255; c.b = (x + f) & 255; return c; }
static rgb_t bg16(int x, int y, int f)
{
	/* 5-6-5 values expanded the way GL expands them (replicate the top bits) */
	rgb_t c; int r5 = 8, g6 = y & 63, b5 = (x + f) & 31;
	c.r = (r5 << 3) | (r5 >> 2); c.g = (g6 << 2) | (g6 >> 4); c.b = (b5 << 3) | (b5 >> 2);
	return c;
}
static unsigned short pack565(int x, int y, int f) { return (unsigned short)((8 << 11) | ((y & 63) << 5) | ((x + f) & 31)); }

static const rgb_t TEXA = { 255, 255, 255 }, TEXB = { 0x20, 0x80, 0xE0 };

static rgb_t read_px(const unsigned char *surf, int bpp, int x, int y)
{
	rgb_t c;
	if(bpp == 32)
	{
		const unsigned char *p = surf + ((size_t)y * W + x) * 4;   /* BGRA */
		c.b = p[0]; c.g = p[1]; c.r = p[2];
	}
	else
	{
		unsigned short v = ((const unsigned short *)surf)[(size_t)y * W + x];
		int r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
		c.r = (r5 << 3) | (r5 >> 2); c.g = (g6 << 2) | (g6 >> 4); c.b = (b5 << 3) | (b5 >> 2);
	}
	return c;
}

static void check(const char *what, const unsigned char *surf, int bpp, int x, int y, rgb_t want, int tol, int frame)
{
	rgb_t got = read_px(surf, bpp, x, y);
	g_checks++;
	if(abs(got.r - want.r) > tol || abs(got.g - want.g) > tol || abs(got.b - want.b) > tol)
	{
		if(g_bad < 24 && g_judge)
			rep("  MISMATCH %d bpp frame %d %s at (%d,%d): got %02X%02X%02X want %02X%02X%02X\n",
			    bpp, frame, what, x, y, got.r, got.g, got.b, want.r, want.g, want.b);
		g_bad++;
	}
}

static int run_pass(int bpp, int frames)
{
	GLuint plane_fb = 0, plane_color = 0, plane_depth = 0, c16_fb = 0, c16_tex = 0, tex = 0;
	unsigned char *surf = (unsigned char *)malloc((size_t)W * H * 4);
	unsigned char *texel = (unsigned char *)malloc(64 * 64 * 4);
	DWORD t_up = 0, t_draw = 0, t_read = 0, t0, t1;
	int f, x, y, bad0 = g_bad, chk0 = g_checks, err0 = g_errors;
	GLenum st;

	if(!surf || !texel) { rep("out of memory\n"); return 0; }
	rep("--- %d bpp pass: %dx%d, %d frames ---\n", bpp, W, H, frames);

	/* the plane FBO, as MesaFBOChange creates it */
	gl.GenFramebuffers(1, &plane_fb);
	gl.GenTextures(1, &plane_color);
	gl.GenTextures(1, &plane_depth);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindFramebuffer(GL_FRAMEBUFFER, plane_fb);
	gl.BindTexture(GL_TEXTURE_2D, plane_color);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, plane_color, 0);
	gl.BindTexture(GL_TEXTURE_2D, plane_depth);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, W, H, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, plane_depth, 0);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, plane_depth, 0);
	st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
	rep("plane FBO status 0x%04X%s\n", st, st == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (NOT COMPLETE)");
	if(st != GL_FRAMEBUFFER_COMPLETE) g_bad++;
	glerr("plane FBO", -1);

	if(bpp == 16)
	{
		/* the colour16 FBO the HAL blits 16-bit surfaces through */
		gl.GenTextures(1, &c16_tex);
		gl.GenFramebuffers(1, &c16_fb);
		gl.BindFramebuffer(GL_FRAMEBUFFER, c16_fb);
		gl.BindTexture(GL_TEXTURE_2D, c16_tex);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, W, H, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c16_tex, 0);
		st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
		rep("colour16 FBO status 0x%04X%s\n", st, st == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (NOT COMPLETE)");
		if(st != GL_FRAMEBUFFER_COMPLETE) g_bad++;
		glerr("colour16 FBO", -1);
	}

	/* a 64x64 checker texture, 8x8-texel cells */
	for(y = 0; y < 64; y++) for(x = 0; x < 64; x++)
	{
		const rgb_t *c = (((x >> 3) + (y >> 3)) & 1) ? &TEXB : &TEXA;
		unsigned char *p = texel + (y * 64 + x) * 4;
		p[0] = (unsigned char)c->r; p[1] = (unsigned char)c->g; p[2] = (unsigned char)c->b; p[3] = 255;
	}
	gl.GenTextures(1, &tex);
	gl.BindTexture(GL_TEXTURE_2D, tex);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
	glerr("texture", -1);

	for(f = 0; f < frames; f++)
	{
		/* 1. the DirectDraw surface -> the plane (MesaBufferUploadColor) */
		t0 = GetTickCount();
		for(y = 0; y < H; y++) for(x = 0; x < W; x++)
		{
			if(bpp == 32)
			{
				rgb_t c = bg32(x, y, f);
				unsigned char *p = surf + ((size_t)y * W + x) * 4;
				p[0] = (unsigned char)c.b; p[1] = (unsigned char)c.g; p[2] = (unsigned char)c.r; p[3] = 255;
			}
			else
				((unsigned short *)surf)[(size_t)y * W + x] = pack565(x, y, f);
		}
		gl.Finish();
		if(bpp == 32)
		{
			gl.BindFramebuffer(GL_FRAMEBUFFER, plane_fb);
			gl.BindTexture(GL_TEXTURE_2D, plane_color);
			gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, surf);
		}
		else
		{
			gl.BindFramebuffer(GL_FRAMEBUFFER, c16_fb);
			gl.BindTexture(GL_TEXTURE_2D, c16_tex);
			gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, surf);
			gl.BindFramebuffer(GL_READ_FRAMEBUFFER, c16_fb);
			gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, plane_fb);
			gl.BlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			gl.BindFramebuffer(GL_FRAMEBUFFER, plane_fb);
		}
		glerr("upload", f);
		t1 = GetTickCount(); t_up += t1 - t0; t0 = t1;

		/* 2. the scene: D3D Clear(Z|STENCIL), then draws */
		gl.BindFramebuffer(GL_FRAMEBUFFER, plane_fb);
		gl.Viewport(0, 0, W, H);
		gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity(); gl.Ortho(0, W, 0, H, -1, 1);
		gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
		gl.ClearDepth(1.0); gl.ClearStencil(0);
		gl.DepthMask(1); gl.StencilMask(0xFF);
		gl.Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		gl.Disable(GL_TEXTURE_2D); gl.Disable(GL_BLEND); gl.Disable(GL_STENCIL_TEST);

		/* (a) depth: near red quad first, far blue quad over it - red must stay */
		gl.Enable(GL_DEPTH_TEST); gl.DepthFunc(GL_LESS);
		/* glOrtho(.., -1, 1): window depth = (1 - z) / 2, so z = +0.5 is the NEAR one */
		gl.Color4f(1, 0, 0, 1); quad(40, 300, 160, 420, 0.5f, 0);
		gl.Color4f(0, 0, 1, 1); quad(20, 280, 180, 440, -0.5f, 0);
		gl.Disable(GL_DEPTH_TEST);

		/* (b) the checker texture, 128x128 pixels = 2x2 pixels per texel */
		gl.Enable(GL_TEXTURE_2D); gl.BindTexture(GL_TEXTURE_2D, tex);
		gl.Color4f(1, 1, 1, 1); quad(256, 176, 384, 304, 0, 1);
		gl.Disable(GL_TEXTURE_2D);

		/* (c) 50% green over the uploaded background */
		gl.Enable(GL_BLEND); gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl.Color4f(0, 1, 0, 0.5f); quad(480, 300, 600, 420, 0, 0);
		gl.Disable(GL_BLEND);

		/* (d) stencil: mark 440..540 x 60..160, then yellow over 420..560 x 40..180 only where marked */
		gl.Enable(GL_STENCIL_TEST);
		gl.StencilFunc(GL_ALWAYS, 1, 0xFF); gl.StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		gl.ColorMask(0, 0, 0, 0); quad(440, 60, 540, 160, 0, 0); gl.ColorMask(1, 1, 1, 1);
		gl.StencilFunc(GL_EQUAL, 1, 0xFF); gl.StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		gl.Color4f(1, 1, 0, 1); quad(420, 40, 560, 180, 0, 0);
		gl.Disable(GL_STENCIL_TEST);
		glerr("draw", f);
		t1 = GetTickCount(); t_draw += t1 - t0; t0 = t1;

		/* 3. the plane -> the surface (MesaBufferDownloadColor) */
		gl.Finish();
		memset(surf, 0xCD, (size_t)W * H * 4);
		if(bpp == 32)
			gl.ReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, surf);
		else
			gl.ReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, surf);
		glerr("readback", f);
		t1 = GetTickCount(); t_read += t1 - t0;

		/* 4. checks */
		{
			int tol = bpp == 32 ? 2 : 10;
			rgb_t red = { 255, 0, 0 }, yel = { 255, 255, 0 }, bl, bg;
			check("background (upload round trip)", surf, bpp, 10, 10, bpp == 32 ? bg32(10, 10, f) : bg16(10, 10, f), 0, f);
			check("background (upload round trip)", surf, bpp, 620, 460, bpp == 32 ? bg32(620, 460, f) : bg16(620, 460, f), 0, f);
			check("background (upload round trip)", surf, bpp, 300, 100, bpp == 32 ? bg32(300, 100, f) : bg16(300, 100, f), 0, f);
			check("depth-tested red quad", surf, bpp, 100, 360, red, tol, f);
			check("texture texel A", surf, bpp, 256 + 1, 176 + 1, TEXA, tol, f);           /* texel (0,0) cell (0,0) */
			check("texture texel B", surf, bpp, 256 + 2 * 8 + 1, 176 + 1, TEXB, tol, f);   /* texel (8,0) cell (1,0) */
			check("texture texel A", surf, bpp, 256 + 2 * 9 + 1, 176 + 2 * 9 + 1, TEXA, tol, f);
			bg = bpp == 32 ? bg32(540, 360, f) : bg16(540, 360, f);
			bl.r = bg.r / 2; bl.g = (255 + bg.g) / 2; bl.b = bg.b / 2;
			check("50% blend", surf, bpp, 540, 360, bl, tol + 2, f);
			check("stencil pass", surf, bpp, 490, 110, yel, tol, f);
			check("stencil reject", surf, bpp, 430, 50, bpp == 32 ? bg32(430, 50, f) : bg16(430, 50, f), 0, f);
		}
		if(f == 0 || (f + 1) % 20 == 0)
			rep("frame %d: upload %lu ms, draw %lu ms, readback %lu ms (totals), mismatches so far %d, GL errors %d\n",
			    f + 1, (unsigned long)t_up, (unsigned long)t_draw, (unsigned long)t_read, g_bad - bad0, g_errors - err0);
	}

	rep("%d bpp: %d frames, %d checks, %d mismatches, %d GL errors; per frame upload %.1f / draw %.1f / readback %.1f ms\n",
	    bpp, frames, g_checks - chk0, g_bad - bad0, g_errors - err0,
	    (double)t_up / frames, (double)t_draw / frames, (double)t_read / frames);

	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.DeleteTextures(1, &tex);
	gl.DeleteTextures(1, &plane_color);
	gl.DeleteTextures(1, &plane_depth);
	gl.DeleteFramebuffers(1, &plane_fb);
	if(c16_fb) { gl.DeleteTextures(1, &c16_tex); gl.DeleteFramebuffers(1, &c16_fb); }
	glerr("cleanup", -1);
	free(surf); free(texel);
	return 1;
}

int main(int argc, char **argv)
{
	const char *dll = "mesa3d.dll";
	int frames = 60, i;
	HMODULE lib;
	OSMesaCreateContextExt_t pCreate;
	OSMesaMakeCurrent_t pMake;
	OSMesaDestroyContext_t pDestroy;
	OSMesaPixelStore_t pPixelStore;
	OSMesaContext ctx;
	static unsigned char osbuf[320 * 240 * 4];
	const char *ren, *ver, *ven;
	const char *fake = getenv("NVSHIM_FAKE");

	for(i = 1; i + 1 < argc; i += 2)
	{
		if(!strcmp(argv[i], "dll")) dll = argv[i + 1];
		else if(!strcmp(argv[i], "frames")) frames = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "width")) W = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "height")) H = atoi(argv[i + 1]);
	}
	if(frames < 1) frames = 1;
	if(W < 640) W = 640;
	if(H < 480) H = 480;

	g_rep = fopen("ostest.txt", "w");
	rep("ostest (build 3116): mesa3d.dll's OSMesa path the way vmhal3d.dll's Direct3D uses it\n");

	lib = LoadLibraryA(dll);
	if(!lib) { rep("LoadLibrary(%s) failed, error %lu\nRESULT: FAIL\n", dll, (unsigned long)GetLastError()); return 2; }
	pGetProc = (OSMesaGetProcAddress_t)GetProcAddress(lib, "OSMesaGetProcAddress");
	pCreate = (OSMesaCreateContextExt_t)GetProcAddress(lib, "OSMesaCreateContextExt");
	pMake = (OSMesaMakeCurrent_t)GetProcAddress(lib, "OSMesaMakeCurrent");
	pDestroy = (OSMesaDestroyContext_t)GetProcAddress(lib, "OSMesaDestroyContext");
	pPixelStore = (OSMesaPixelStore_t)GetProcAddress(lib, "OSMesaPixelStore");
	if(!pGetProc || !pCreate || !pMake || !pDestroy || !pPixelStore)
	{
		rep("%s has no OSMesa exports - vmhal3d.dll would use its ICD path\nRESULT: FAIL\n", dll);
		return 2;
	}

	/* exactly vmhal3d's MesaCreateCtx */
	ctx = pCreate(OSMESA_RGBA, 24, 8, 0, NULL);
	if(!ctx) { rep("OSMesaCreateContextExt failed\nRESULT: FAIL\n"); return 2; }
	if(!pMake(ctx, osbuf, GL_UNSIGNED_BYTE, 320, 240)) { rep("OSMesaMakeCurrent failed\nRESULT: FAIL\n"); return 2; }
	pPixelStore(OSMESA_Y_UP, 1);

	LOAD(GetError); LOAD(GetString); LOAD(Enable); LOAD(Disable); LOAD(Viewport); LOAD(Clear);
	LOAD(ClearColor); LOAD(ClearDepth); LOAD(ClearStencil); LOAD(MatrixMode); LOAD(LoadIdentity);
	LOAD(Ortho); LOAD(PushMatrix); LOAD(PopMatrix); LOAD(Begin); LOAD(End); LOAD(Vertex3f);
	LOAD(Color4f); LOAD(TexCoord2f); LOAD(DepthFunc); LOAD(DepthMask); LOAD(BlendFunc);
	LOAD(StencilFunc); LOAD(StencilOp); LOAD(StencilMask); LOAD(ColorMask); LOAD(GenTextures);
	LOAD(DeleteTextures); LOAD(BindTexture); LOAD(TexImage2D); LOAD(TexSubImage2D);
	LOAD(TexParameteri); LOAD(TexEnvi); LOAD(PixelStorei); LOAD(ReadPixels); LOAD(Finish);
	LOAD(ActiveTexture); LOAD(GenFramebuffers); LOAD(DeleteFramebuffers); LOAD(BindFramebuffer);
	LOAD(FramebufferTexture2D); LOAD(CheckFramebufferStatus); LOAD(BlitFramebuffer);
	if(g_missing) { rep("RESULT: FAIL (%d GL entry points missing)\n", g_missing); return 2; }

	ren = (const char *)gl.GetString(GL_RENDERER);
	ver = (const char *)gl.GetString(GL_VERSION);
	ven = (const char *)gl.GetString(GL_VENDOR);
	rep("renderer: %s\nversion:  %s\nvendor:   %s\n", ren ? ren : "(null)", ver ? ver : "(null)", ven ? ven : "(null)");
	{
		int gpu = ren && (strstr(ren, "NV") || strstr(ren, "nouveau"));
		rep("screen:   %s\n", gpu ? "GPU (nouveau) - Direct3D will render on the G86"
		                         : "SOFTWARE - Direct3D renders on the CPU (see the header of ostest.c for why)");
	}
	if(fake && atoi(fake))
	{
		g_judge = 0;
		rep("NVSHIM_FAKE=1: the fake GPU draws nothing - pixel checks are counted, not judged\n");
	}

	run_pass(32, frames);
	run_pass(16, frames);

	pDestroy(ctx);
	if(g_judge)
		rep("RESULT: %s (%d checks, %d mismatches, %d GL errors)\n",
		    (g_bad == 0 && g_errors == 0) ? "PASS" : "FAIL", g_checks, g_bad, g_errors);
	else
		rep("RESULT: RAN under the fake GPU (%d checks not judged, %d GL errors)\n", g_checks, g_errors);
	if(g_rep) fclose(g_rep);
	return (g_bad == 0 && g_errors == 0) || !g_judge ? 0 : 1;
}
