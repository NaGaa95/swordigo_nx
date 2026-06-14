/* egl_helper.h -- EGL/GLES1 context setup over the default NWindow
 *
 * Swordigo links libGLESv1_CM/libEGL but only imports eglGetProcAddress: it
 * expects a current OpenGL ES 1.1 context to already exist, exactly like the
 * Vita port created with vitaGL. We bring one up here through devkitPro's
 * mesa (EGL + GLESv1_CM + nouveau).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __EGL_HELPER_H__
#define __EGL_HELPER_H__

// brings up an ES1 context on the default window at w x h; aborts on failure
void egl_init(int w, int h);
// posts the current backbuffer (called once per frame after drawApplication)
void egl_swap(void);
void egl_deinit(void);

#endif
