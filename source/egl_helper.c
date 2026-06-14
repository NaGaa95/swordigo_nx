/* egl_helper.c -- EGL/GLES1 context setup over the default NWindow
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <EGL/egl.h>

#include "egl_helper.h"
#include "util.h"
#include "error.h"

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

void egl_init(int w, int h) {
  // the default window must be sized before the surface is created or mesa
  // picks up the wrong framebuffer dimensions
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, w, h);

  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_display == EGL_NO_DISPLAY)
    fatal_error("egl: eglGetDisplay failed: 0x%x", eglGetError());

  if (!eglInitialize(s_display, NULL, NULL))
    fatal_error("egl: eglInitialize failed: 0x%x", eglGetError());

  if (!eglBindAPI(EGL_OPENGL_ES_API))
    fatal_error("egl: eglBindAPI failed: 0x%x", eglGetError());

  // Swordigo is a fixed-function ES1 renderer; request an ES1-capable config
  // with a 24/8 depth-stencil (the engine uses depth test + stencil masks)
  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      24,
    EGL_STENCIL_SIZE,    8,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num_config = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num_config) || num_config == 0)
    fatal_error("egl: no suitable ES1 config (0x%x)", eglGetError());

  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (s_surface == EGL_NO_SURFACE)
    fatal_error("egl: eglCreateWindowSurface failed: 0x%x", eglGetError());

  const EGLint ctx_attr[] = {
    EGL_CONTEXT_CLIENT_VERSION, 1, // OpenGL ES 1.x
    EGL_NONE
  };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (s_context == EGL_NO_CONTEXT)
    fatal_error("egl: eglCreateContext failed: 0x%x", eglGetError());

  if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context))
    fatal_error("egl: eglMakeCurrent failed: 0x%x", eglGetError());

  // present as soon as a frame is ready; the engine paces itself off dt
  eglSwapInterval(s_display, 1);

  debugPrintf("egl: ES1 context up at %dx%d\n", w, h);
}

void egl_swap(void) {
  eglSwapBuffers(s_display, s_surface);
}

void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context != EGL_NO_CONTEXT) eglDestroyContext(s_display, s_context);
  if (s_surface != EGL_NO_SURFACE) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
  s_context = EGL_NO_CONTEXT;
  s_surface = EGL_NO_SURFACE;
}
