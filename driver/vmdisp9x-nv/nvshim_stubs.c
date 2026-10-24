/*****************************************************************************
 * nvshim_stubs.c - unresolved-symbol stubs for the Win9x nouveau build (M5.3c)
 *
 * We build only the nv50 driver (the G86 is an NV50/Tesla part) and exclude the
 * video-decode (VP2/VP3) sources, since GL rendering needs none of it. But a
 * few of those entry points are REFERENCED by core code we do build:
 *   - nouveau_drm_winsys.c dispatches by chipset to nv30/nv50/nvc0_screen_create
 *   - nouveau_screen/context init call nouveau_screen_init_vdec / _context_init_vdec
 *   - nv50_screen.c wires the video query/codec hooks (nv84/nv98/vp3 ...)
 * Provide stubs so the link resolves; none of these is reached for GL on the G86
 * (video returns "unsupported", non-nv50 screens return NULL). Compiled only
 * into mesa3d.dll, never the standalone nvprobe. Licensed MIT.
 *****************************************************************************/
#ifndef NVSHIM_STANDALONE

#include "nouveau.h"
#include "nv50/nv50_context.h"   /* nv84/nv98 decls; pulls nouveau_screen/context */
#include "nouveau_vp3_video.h"   /* nouveau_vp3_screen_* decls                    */

/* --- chipset dispatch: only nv50 is built --------------------------------- */
struct nouveau_screen *nv30_screen_create(struct nouveau_device *dev) { (void)dev; return 0; }
struct nouveau_screen *nvc0_screen_create(struct nouveau_device *dev) { (void)dev; return 0; }

/* --- video decode: not built (GL-only driver) ----------------------------- */
void nouveau_screen_init_vdec(struct nouveau_screen *s)  { (void)s; }
void nouveau_context_init_vdec(struct nouveau_context *c) { (void)c; }

int
nouveau_vp3_screen_get_video_param(struct pipe_screen *pscreen,
                                   enum pipe_video_profile profile,
                                   enum pipe_video_entrypoint entrypoint,
                                   enum pipe_video_cap param)
{ (void)pscreen; (void)profile; (void)entrypoint; (void)param; return 0; }

bool
nouveau_vp3_screen_video_supported(struct pipe_screen *screen,
                                   enum pipe_format format,
                                   enum pipe_video_profile profile,
                                   enum pipe_video_entrypoint entrypoint)
{ (void)screen; (void)format; (void)profile; (void)entrypoint; return false; }

struct pipe_video_codec *
nv84_create_decoder(struct pipe_context *context,
                    const struct pipe_video_codec *templ)
{ (void)context; (void)templ; return 0; }

struct pipe_video_buffer *
nv84_video_buffer_create(struct pipe_context *pipe,
                         const struct pipe_video_buffer *template)
{ (void)pipe; (void)template; return 0; }

int
nv84_screen_get_video_param(struct pipe_screen *pscreen,
                            enum pipe_video_profile profile,
                            enum pipe_video_entrypoint entrypoint,
                            enum pipe_video_cap param)
{ (void)pscreen; (void)profile; (void)entrypoint; (void)param; return 0; }

bool
nv84_screen_video_supported(struct pipe_screen *screen,
                            enum pipe_format format,
                            enum pipe_video_profile profile,
                            enum pipe_video_entrypoint entrypoint)
{ (void)screen; (void)format; (void)profile; (void)entrypoint; return false; }

struct pipe_video_codec *
nv98_create_decoder(struct pipe_context *context,
                    const struct pipe_video_codec *templ)
{ (void)context; (void)templ; return 0; }

struct pipe_video_buffer *
nv98_video_buffer_create(struct pipe_context *pipe,
                         const struct pipe_video_buffer *template)
{ (void)pipe; (void)template; return 0; }

#endif /* !NVSHIM_STANDALONE */
