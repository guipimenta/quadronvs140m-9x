/*
 * tlvtest.c - draw pre-transformed (XYZRHW) Direct3D triangles through mesa3d.dll's
 * OSMesa exactly the way vmhal3d.dll does (identity matrices, glVertex4fv of
 * ((x-vx)*2/vw-1)/rhw, ((y-vy)*2/vh-1)/rhw, z*zscale/rhw, 1/rhw), so a rendering bug
 * in Mesa's handling of such vertices can be reproduced without DirectDraw.
 *
 *   tlvtest [tris.bin|-] [out.ppm] [dll mesa3d.dll] [flat 0|1] [hal 0|1] [fan 0|1]
 *           [fbo 0|1] [tex 0|1]
 *
 * tris.bin (format 2, from a DP2 capture, analysis/t61-remote/hl-d3d/): records of
 * { DWORD flags, DWORD rgb, float v[3][6] = x y z rhw u v } in vmhal3d's vertex
 * order. flags: bit 0-1 cull (0 none, 1 GL_FRONT, 2 GL_BACK), bit 2 indexed list
 * (cull face swapped, as the HAL reverses the order), bit 3 z test, bit 4 z write,
 * bit 5 blended (skipped), bit 6 continues the previous row's fan, bit 7 first
 * triangle of a fan, bits 8-11 D3DCMPFUNC. "-" = a built-in set of Half-Life c1a0
 * near-plane-clipped triangles.
 *
 * Switches, each adding one thing vmhal3d does:
 *   flat 1  constant colour per triangle (else a little per-vertex variation)
 *   hal  1  per vertex glMultiTexCoord4f(unit 0) + glColor4fv + glNormal3f before
 *           glVertex4fv, like MesaVertexBuffer (2 = only the texcoord, 3 = only the
 *           normal)
 *   fan  1  fans as GL_TRIANGLE_FAN (D3DDP2OP_TRIANGLEFAN_IMM), else GL_TRIANGLES
 *   fbo  1  render into an FBO: RGBA8 colour texture + DEPTH24_STENCIL8 texture,
 *           like the HAL's "plane"
 *   tex  1  a 64x64 checker texture on unit 0, GL_MODULATE, GL_REPEAT
 *   mip  1  give it a full mip chain, GL_NEAREST_MIPMAP_NEAREST (D3DTFP_POINT); levels
 *           1..6 are solid red, green, blue, magenta, cyan, yellow, so the picture shows
 *           which level the GPU sampled
 *   count N draw only the first N records
 *   flush N glFlush after every N-th draw (nouveau kicks, like upstream nv50 does per draw)
 *
 * Writes the picture as a PPM (top row = screen row 0) and prints the renderer and
 * how many pixels got each built-in triangle's colour.
 *
 * Build: i686-w64-mingw32-gcc -O2 -s tools/tlvtest.c -o tools/tlvtest.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLbitfield;
typedef float         GLfloat;
typedef double        GLclampd;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;

#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_STENCIL_BUFFER_BIT   0x00000400
#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_TRIANGLES            0x0004
#define GL_TRIANGLE_FAN         0x0006
#define GL_NEVER                0x0200
#define GL_LESS                 0x0201
#define GL_EQUAL                0x0202
#define GL_LEQUAL               0x0203
#define GL_GREATER              0x0204
#define GL_NOTEQUAL             0x0205
#define GL_GEQUAL               0x0206
#define GL_ALWAYS               0x0207
#define GL_FRONT                0x0404
#define GL_BACK                 0x0405
#define GL_CCW                  0x0901
#define GL_CULL_FACE            0x0B44
#define GL_DEPTH_TEST           0x0B71
#define GL_TEXTURE_2D           0x0DE1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_MODELVIEW            0x1700
#define GL_PROJECTION           0x1701
#define GL_RGBA                 0x1908
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02
#define GL_MODULATE             0x2100
#define GL_TEXTURE_ENV_MODE     0x2200
#define GL_TEXTURE_ENV          0x2300
#define GL_NEAREST              0x2600
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_REPEAT               0x2901
#define GL_RGBA8                0x8058
#define GL_TEXTURE0             0x84C0
#define GL_DEPTH_STENCIL        0x84F9
#define GL_UNSIGNED_INT_24_8    0x84FA
#define GL_DEPTH24_STENCIL8     0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER          0x8D40
#define OSMESA_RGBA             GL_RGBA
#define OSMESA_Y_UP             0x11

typedef void *OSMesaContext;
typedef void (*PROC_t)(void);
typedef OSMesaContext (__stdcall *OSMesaCreateContextExt_t)(GLenum, GLint, GLint, GLint, OSMesaContext);
typedef GLboolean (__stdcall *OSMesaMakeCurrent_t)(OSMesaContext, void *, GLenum, GLsizei, GLsizei);
typedef void (__stdcall *OSMesaPixelStore_t)(GLint, GLint);
typedef PROC_t (__stdcall *OSMesaGetProcAddress_t)(const char *);

static struct
{
	GLenum (__stdcall *GetError)(void);
	const GLubyte *(__stdcall *GetString)(GLenum);
	void (__stdcall *Enable)(GLenum);
	void (__stdcall *Disable)(GLenum);
	void (__stdcall *Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (__stdcall *Clear)(GLbitfield);
	void (__stdcall *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (__stdcall *ClearDepth)(GLclampd);
	void (__stdcall *MatrixMode)(GLenum);
	void (__stdcall *LoadIdentity)(void);
	void (__stdcall *Begin)(GLenum);
	void (__stdcall *End)(void);
	void (__stdcall *Vertex4fv)(const GLfloat *);
	void (__stdcall *Color4fv)(const GLfloat *);
	void (__stdcall *Normal3f)(GLfloat, GLfloat, GLfloat);
	void (__stdcall *MultiTexCoord4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
	void (__stdcall *DepthFunc)(GLenum);
	void (__stdcall *DepthMask)(GLboolean);
	void (__stdcall *CullFace)(GLenum);
	void (__stdcall *FrontFace)(GLenum);
	void (__stdcall *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
	void (__stdcall *Finish)(void);
	void (__stdcall *Flush)(void);
	void (__stdcall *GenTextures)(GLsizei, GLuint *);
	void (__stdcall *BindTexture)(GLenum, GLuint);
	void (__stdcall *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (__stdcall *TexParameteri)(GLenum, GLenum, GLint);
	void (__stdcall *TexEnvi)(GLenum, GLenum, GLint);
	void (__stdcall *ActiveTexture)(GLenum);
	void (__stdcall *GenFramebuffers)(GLsizei, GLuint *);
	void (__stdcall *BindFramebuffer)(GLenum, GLuint);
	void (__stdcall *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	GLenum (__stdcall *CheckFramebufferStatus)(GLenum);
} gl;

static OSMesaGetProcAddress_t pGetProc;
static int g_missing;
#define LOAD(_n) do { *(PROC_t*)&gl._n = pGetProc("gl" #_n); if(!gl._n) { printf("missing gl" #_n "\n"); g_missing++; } } while(0)

typedef struct { DWORD fl, rgb; float v[3][6]; } tri_t;

/* Half-Life c1a0, first frame: one near-plane-clipped fan of the tram (x/y far
   outside the 640x480 screen, rhw 0.25 at the near plane), one clipped triangle
   and one ordinary triangle. */
static tri_t builtin[] = {
	{ 0x098, 0x0000ff, {{6831.43f, -5507.50f, 0.0f, 0.25f, 3.75f, -2.50f}, {581.18f, 48.47f, 0.9676f, 0.0083f, 2.84f, -2.6f}, {495.87f, 48.47f, 0.9676f, 0.0083f, 2.84f, -2.4f}} },
	{ 0x058, 0x00ff00, {{6831.43f, -5507.50f, 0.0f, 0.25f, 3.75f, -2.50f}, {495.87f, 48.47f, 0.9676f, 0.0083f, 2.84f, -2.4f}, {5597.50f, -5507.50f, 0.0f, 0.25f, 3.75f, -2.4f}} },
	{ 0x098, 0xff0000, {{461.40f, 6.70f, 0.9625f, 0.0096f, 6.0f, -2.02f}, {166.10f, 6.70f, 0.9625f, 0.0096f, 6.0f, -3.02f}, {-1673.46f, -2781.94f, 0.5024f, 0.1245f, 0.33f, 0.85f}} },
	{ 0x098, 0xffff00, {{300.0f, 300.0f, 0.5f, 0.1f, 0, 0}, {400.0f, 300.0f, 0.5f, 0.1f, 1, 0}, {350.0f, 400.0f, 0.5f, 0.1f, 0, 1}} },
};

static int hal, flat;
static const float vpn[4] = { 0, 0, 2.0f / 640, 2.0f / 480 };

static void vertex(const tri_t *t, int k)
{
	const float *s = t->v[k];
	float o[4], c[4];
	DWORD rgb = flat ? t->rgb : (k == 0 ? t->rgb : t->rgb ^ (k == 1 ? 0x404040 : 0x202020));
	c[0] = (rgb & 255) / 255.0f; c[1] = ((rgb >> 8) & 255) / 255.0f; c[2] = ((rgb >> 16) & 255) / 255.0f; c[3] = 1.0f;
	if(hal == 1 || hal == 2) gl.MultiTexCoord4f(GL_TEXTURE0, s[4], s[5], 0.0f, 1.0f);
	gl.Color4fv(c);
	if(hal == 1 || hal == 3) gl.Normal3f(0.0f, 0.0f, 1.0f);
	o[3] = 1.0f / s[3];
	o[0] = (((s[0] - vpn[0]) * vpn[2]) - 1.0f) * o[3];
	o[1] = (((s[1] - vpn[1]) * vpn[3]) - 1.0f) * o[3];
	o[2] = (s[2] * 0.99f) * o[3];
	gl.Vertex4fv(o);
}

static void state(DWORD fl)
{
	static const GLenum cmp[9] = { GL_NEVER, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
	int cull = fl & 3, zf = (fl >> 8) & 15;
	if(cull == 0) gl.Disable(GL_CULL_FACE);
	else
	{
		GLenum c = cull == 1 ? GL_FRONT : GL_BACK;
		if(fl & 4) c = c == GL_FRONT ? GL_BACK : GL_FRONT;
		gl.Enable(GL_CULL_FACE); gl.CullFace(c);
	}
	if(fl & 8) gl.Enable(GL_DEPTH_TEST); else gl.Disable(GL_DEPTH_TEST);
	gl.DepthMask((fl & 16) ? 1 : 0);
	gl.DepthFunc(cmp[zf < 9 ? zf : 4]);
}

int main(int argc, char **argv)
{
	const char *in = argc > 1 ? argv[1] : "-", *out = argc > 2 ? argv[2] : "tlvtest.ppm";
	const char *dll = "mesa3d.dll";
	int i, W = 640, H = 480, fan = 0, fbo = 0, tex = 0, flush = 0, draws = 0, count = 0, mip = 0;
	for(i = 3; i + 1 < argc; i += 2)
	{
		if(!strcmp(argv[i], "dll")) dll = argv[i + 1];
		else if(!strcmp(argv[i], "fan")) fan = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "flat")) flat = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "hal")) hal = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "fbo")) fbo = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "tex")) tex = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "flush")) flush = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "count")) count = atoi(argv[i + 1]);
		else if(!strcmp(argv[i], "mip")) mip = atoi(argv[i + 1]);
	}

	tri_t *t = builtin; long n = sizeof(builtin) / sizeof(builtin[0]);
	if(strcmp(in, "-"))
	{
		FILE *f = fopen(in, "rb");
		if(!f) { printf("cannot open %s\n", in); return 2; }
		fseek(f, 0, SEEK_END); n = ftell(f) / sizeof(tri_t); fseek(f, 0, SEEK_SET);
		t = malloc(n * sizeof(tri_t));
		if(fread(t, sizeof(tri_t), n, f) != (size_t)n) { printf("short read\n"); return 2; }
		fclose(f);
	}

	if(count > 0 && count < n) n = count;

	HMODULE lib = LoadLibraryA(dll);
	if(!lib) { printf("LoadLibrary(%s) failed, error %lu\n", dll, (unsigned long)GetLastError()); return 2; }
	pGetProc = (OSMesaGetProcAddress_t)GetProcAddress(lib, "OSMesaGetProcAddress");
	OSMesaCreateContextExt_t pCreate = (OSMesaCreateContextExt_t)GetProcAddress(lib, "OSMesaCreateContextExt");
	OSMesaMakeCurrent_t pMake = (OSMesaMakeCurrent_t)GetProcAddress(lib, "OSMesaMakeCurrent");
	OSMesaPixelStore_t pPixelStore = (OSMesaPixelStore_t)GetProcAddress(lib, "OSMesaPixelStore");
	if(!pGetProc || !pCreate || !pMake || !pPixelStore) { printf("%s has no OSMesa exports\n", dll); return 2; }

	unsigned char *buf = calloc(W * H, 4);
	OSMesaContext ctx = pCreate(OSMESA_RGBA, 24, 8, 0, NULL);
	if(!ctx || !pMake(ctx, buf, GL_UNSIGNED_BYTE, W, H)) { printf("OSMesa context failed\n"); return 2; }
	pPixelStore(OSMESA_Y_UP, 0);   /* row 0 = top, as the HAL's surfaces */

	LOAD(GetError); LOAD(GetString); LOAD(Enable); LOAD(Disable); LOAD(Viewport); LOAD(Clear);
	LOAD(ClearColor); LOAD(ClearDepth); LOAD(MatrixMode); LOAD(LoadIdentity); LOAD(Begin); LOAD(End);
	LOAD(Vertex4fv); LOAD(Color4fv); LOAD(Normal3f); LOAD(MultiTexCoord4f); LOAD(DepthFunc);
	LOAD(DepthMask); LOAD(CullFace); LOAD(FrontFace); LOAD(ReadPixels); LOAD(Finish);
	LOAD(Flush); LOAD(GenTextures); LOAD(BindTexture); LOAD(TexImage2D); LOAD(TexParameteri); LOAD(TexEnvi);
	LOAD(ActiveTexture); LOAD(GenFramebuffers); LOAD(BindFramebuffer); LOAD(FramebufferTexture2D);
	LOAD(CheckFramebufferStatus);
	if(g_missing) return 2;
	printf("renderer: %s\nversion:  %s\nflat %d hal %d fan %d fbo %d tex %d mip %d flush %d\n", gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION), flat, hal, fan, fbo, tex, mip, flush);

	if(fbo)
	{
		GLuint fb, ct, dt;
		gl.GenTextures(1, &ct); gl.BindTexture(GL_TEXTURE_2D, ct);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		gl.GenTextures(1, &dt); gl.BindTexture(GL_TEXTURE_2D, dt);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, W, H, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
		gl.BindTexture(GL_TEXTURE_2D, 0);
		gl.GenFramebuffers(1, &fb); gl.BindFramebuffer(GL_FRAMEBUFFER, fb);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, 0);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dt, 0);
		printf("FBO status 0x%X\n", gl.CheckFramebufferStatus(GL_FRAMEBUFFER));
	}
	if(tex)
	{
		static DWORD chk[64 * 64];
		GLuint tt;
		for(i = 0; i < 64 * 64; i++) chk[i] = (((i & 63) ^ (i >> 6)) & 8) ? 0xffffffff : 0xff808080;
		gl.ActiveTexture(GL_TEXTURE0);
		gl.GenTextures(1, &tt); gl.BindTexture(GL_TEXTURE_2D, tt);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, chk);
		if(mip)
		{
			static const DWORD lc[7] = { 0, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffff00ff, 0xffffff00, 0xff00ffff };
			static DWORD lv[32 * 32];
			int l, k;
			for(l = 1; l <= 6; l++)
			{
				int sz = 64 >> l;
				for(k = 0; k < sz * sz; k++) lv[k] = lc[l];
				gl.TexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv);
			}
		}
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? 0x2700 /* GL_NEAREST_MIPMAP_NEAREST */ : GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		gl.Enable(GL_TEXTURE_2D);
	}

	gl.Viewport(0, 0, W, H);
	gl.FrontFace(GL_CCW);
	gl.ClearColor(0, 0, 0, 1); gl.ClearDepth(1.0);
	gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
	gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity();

	for(i = 0; i < n; i++)
	{
		DWORD fl = t[i].fl;
		if(fl & 32) continue;
		if(fan && (fl & 0x80))
		{
			/* a whole TRIANGLEFAN_IMM: this row's 3 vertices, then each continuation row's v2 */
			state(fl);
			gl.Begin(GL_TRIANGLE_FAN);
			vertex(&t[i], 0); vertex(&t[i], 1); vertex(&t[i], 2);
			while(i + 1 < n && (t[i + 1].fl & 0x40)) { i++; vertex(&t[i], 2); }
			gl.End();
			if(flush && ++draws % flush == 0) gl.Flush();
			continue;
		}
		state(fl);
		gl.Begin(GL_TRIANGLES);
		vertex(&t[i], 0); vertex(&t[i], 1); vertex(&t[i], 2);
		gl.End();
		if(flush && ++draws % flush == 0) gl.Flush();
	}
	gl.Finish();
	printf("glGetError 0x%x\n", gl.GetError());
	if(fbo) gl.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);   /* GL row 0 = screen row 0 */

	FILE *o = fopen(out, "wb");
	fprintf(o, "P6\n%d %d\n255\n", W, H);
	for(i = 0; i < W * H; i++) fwrite(buf + i * 4, 3, 1, o);
	fclose(o);
	if(t == builtin && !tex)
	{
		long cnt[4] = { 0 };
		for(i = 0; i < W * H; i++)
		{
			DWORD p = buf[i * 4] | buf[i * 4 + 1] << 8 | buf[i * 4 + 2] << 16;
			int j;
			for(j = 0; j < 4; j++) if(p == builtin[j].rgb) cnt[j]++;
		}
		printf("pixels per triangle: %ld %ld %ld %ld\n", cnt[0], cnt[1], cnt[2], cnt[3]);
	}
	printf("wrote %s\n", out);
	return 0;
}
