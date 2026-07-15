/* jni_fake.h -- fake JNI environment for Swordigo's Native/MusicPlayer layer
 *
 * Swordigo is driven natively by the wrapper (we call its Java_..._Native_*
 * entry points directly), but those entry points still call back through a
 * JNIEnv for a handful of things: the MusicPlayer object (loadFile/play/
 * pause/stop/setLooping/setVolume), text input, achievement reporting, and
 * age/consent gates. We emulate just enough of a JavaVM/JNIEnv to service
 * those.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

// passed to every Java_..._Native_*/MusicPlayer_* entry point
extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

void jni_init(void);

typedef void (*JniTextChangedCallback)(void *env, void *obj, void *text);
typedef void (*JniTextFinishedCallback)(void *env, void *obj);
void jni_configure_text_input(JniTextChangedCallback changed,
                              JniTextFinishedCallback finished);
void jni_update(void);

// fake Java object/string constructors used by the boot code
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

#endif
