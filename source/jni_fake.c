/* jni_fake.c -- fake JNI environment for Swordigo's Native/MusicPlayer layer
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "jni_fake.h"
#include "util.h"
#include "music_player.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model: tagged heap structs handed back as jobject/jstring/etc.
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_ID     = 0x4d494431, // 'MID1'
};

typedef struct {
  uint32_t tag;
  char label[64];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  char name[64];
  char sig[64];
} FakeID;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "<not-a-string>";
}

// ---------------------------------------------------------------------------
// method/field ID pool: IDs are pointers to records; calls dispatch by name
// ---------------------------------------------------------------------------

#define MAX_IDS 128
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

#define TEXT_INPUT_MAX 256
static int text_input_pending = 0;
static char text_input_initial[TEXT_INPUT_MAX];
static JniTextChangedCallback text_input_changed = NULL;
static JniTextFinishedCallback text_input_finished = NULL;

void jni_configure_text_input(JniTextChangedCallback changed,
                              JniTextFinishedCallback finished) {
  text_input_changed = changed;
  text_input_finished = finished;
}

void jni_update(void) {
  if (!text_input_pending)
    return;

  text_input_pending = 0;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  char result[TEXT_INPUT_MAX] = { 0 };
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetInitialText(&kbd, text_input_initial);
    swkbdConfigSetStringLenMax(&kbd, TEXT_INPUT_MAX - 1);
    rc = swkbdShow(&kbd, result, sizeof(result));
    swkbdClose(&kbd);
  }

  if (R_SUCCEEDED(rc) && text_input_changed)
    text_input_changed(fake_env, NULL, jni_make_string(result));

  if (text_input_finished)
    text_input_finished(fake_env, NULL);
}

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS)
    return &id_pool[0];
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// method dispatch (shared by instance and static variants)
// ---------------------------------------------------------------------------

static juint call_boolean(const char *name, va_list va) {
  if (!strcmp(name, "loadFile")) {
    const char *track = obj_str(va_arg(va, void *));
    debugPrintf("JNI: MusicPlayer.loadFile(%s)\n", track);
    music_load(track);
    return 1;
  }
  // age/consent gates: behave like a known-adult, consented platform so the
  // game skips the Android age dialogs entirely
  if (!strcmp(name, "isAgeKnown") || !strcmp(name, "getPlatformConsentState"))
    return 1;
  if (!strcmp(name, "isGoogleGameServicesAvailable"))
    return 0;
  debugPrintf("JNI: CallBooleanMethod(%s) -> 0\n", name);
  return 0;
}

static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "play"))       { music_play();  return; }
  if (!strcmp(name, "pause"))      { music_pause(); return; }
  if (!strcmp(name, "stop"))       { music_stop();  return; }
  if (!strcmp(name, "setLooping")) { music_set_loop(va_arg(va, int)); return; }
  if (!strcmp(name, "setVolume"))  { music_set_volume((float)va_arg(va, double)); return; }
  if (!strcmp(name, "startTextInput")) {
    const char *initial = obj_str(va_arg(va, void *));
    snprintf(text_input_initial, sizeof(text_input_initial), "%s", initial);
    text_input_pending = 1;
    return;
  }
  if (!strcmp(name, "stopTextInput")) {
    text_input_pending = 0;
    return;
  }
  if (!strcmp(name, "reportAchievementProgress")) {
    // no trophy system on Switch homebrew; the args are (name, progress, locked)
    const char *ach = obj_str(va_arg(va, void *));
    debugPrintf("JNI: achievement '%s' (ignored)\n", ach);
    return;
  }
  debugPrintf("JNI: CallVoidMethod(%s) ignored\n", name);
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  return get_id(name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return jni_make_object("class"); }
static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static void  j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return call_boolean(id->name, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = call_boolean(id->name, va);
  va_end(va); return r;
}
static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  call_void(id->name, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  call_void(id->name, va);
  va_end(va);
}
static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallObjectMethod(%s) -> null\n", id->name);
  return NULL;
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  debugPrintf("JNI: CallObjectMethod(%s) -> null\n", id->name);
  return NULL;
}
static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallIntMethod(%s) -> 0\n", id->name);
  return 0;
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj; (void)id; return 0;
}

static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; (void)id; (void)va;
  return jni_make_object("newobject");
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls; (void)id;
  return jni_make_object("newobject");
}

// fields: Swordigo's native layer reads none we care about; return zero/empty
static void *j_GetObjectField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return NULL; }
static juint j_GetField0(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0; }
static float j_GetFloatField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0.0f; }

// strings
static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }
static juint j_RegisterNatives(void *env, void *cls, void *m, int n) { (void)env; (void)cls; (void)m; (void)n; return 0; }

static juint j_unimplemented(void) {
  debugPrintf("JNI: call to unimplemented slot\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI spec)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_ret0(void *vm) { (void)vm; return JNI_OK; }

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  id_count = 0;
  text_input_pending = 0;
  text_input_initial[0] = '\0';
  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef;  // DeleteGlobalRef
  env_table[23]  = (void *)j_DeleteRef;  // DeleteLocalRef
  env_table[24]  = (void *)j_ret0_3;     // IsSameObject
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_ret0_2;     // EnsureLocalCapacity
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetMethodID; // GetFieldID
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetField0;   // GetBooleanField
  env_table[100] = (void *)j_GetField0;   // GetIntField
  env_table[101] = (void *)j_GetField0;   // GetLongField
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID; // GetStaticMethodID
  env_table[114] = (void *)j_CallObjectMethod;   // CallStaticObjectMethod
  env_table[115] = (void *)j_CallObjectMethodV;  // CallStaticObjectMethodV
  env_table[117] = (void *)j_CallBooleanMethod;  // CallStaticBooleanMethod
  env_table[118] = (void *)j_CallBooleanMethodV; // CallStaticBooleanMethodV
  env_table[129] = (void *)j_CallIntMethod;      // CallStaticIntMethod
  env_table[130] = (void *)j_CallIntMethodV;     // CallStaticIntMethodV
  env_table[141] = (void *)j_CallVoidMethod;     // CallStaticVoidMethod
  env_table[142] = (void *)j_CallVoidMethodV;    // CallStaticVoidMethodV
  env_table[144] = (void *)j_GetMethodID; // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField; // GetStaticObjectField
  env_table[146] = (void *)j_GetField0;   // GetStaticBooleanField
  env_table[150] = (void *)j_GetField0;   // GetStaticIntField
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_ret0;                 // DestroyJavaVM
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_ret0;                 // DetachCurrentThread
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;  // AttachCurrentThreadAsDaemon

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
