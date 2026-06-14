/* imports.c -- .so import resolution for libswordigo.so (arm64-v8a)
 *
 * Swordigo statically links its C++ runtime / Boost / Lua / protobuf, so a
 * single module is loaded and this is the only resolution table. It covers
 * the ~238 undefined symbols of the 1.4.12 arm64 binary: the GLES1 fixed-
 * function API (served by mesa's libGLESv1_CM), OpenAL (native openal-soft),
 * bionic libc (newlib + the shims in libc_shim.c), a tiny pthread subset, and
 * the AAsset / file layer below.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <locale.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <zlib.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>

#include "imports.h"
#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// odds and ends
// ---------------------------------------------------------------------------

extern int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
extern int *__errno(void);
extern void __stack_chk_fail(void);

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;
static char *__ctype_ptr_ = (char *)&_ctype_;

// the game probes a couple of Google/Apportable profiling hooks; no-op them
static void google_region_noop(void) {}

static void abort_hook(void) {
  debugPrintf("abort() called from %p\n", __builtin_return_address(0));
  exit(1);
}

// bionic struct iovec; sys/uio.h isn't in newlib
struct bionic_iovec { void *iov_base; size_t iov_len; };

static ssize_t writev_fake(int fd, const struct bionic_iovec *iov, int cnt) {
  ssize_t total = 0;
  for (int i = 0; i < cnt; i++) {
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0)
      return -1;
    total += n;
  }
  return total;
}

static int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds; (void)timeout;
  return 0; // nothing is ever ready; the game only polls optional fds
}

static void perror_fake(const char *s) {
  debugPrintf("%s: %s\n", s ? s : "", strerror(errno));
}

// keep stdio redirection requests harmless (the game's stdout/stderr are the
// fake __sF FILEs from libc_shim.c)
static FILE *freopen_fake(const char *path, const char *mode, FILE *f) {
  (void)path; (void)mode;
  return f;
}

// ---------------------------------------------------------------------------
// pthread: the bionic structs differ from newlib's, so the handful the game
// uses go through indirection wrappers (a newlib object is allocated lazily
// and its pointer stashed in the bionic storage)
// ---------------------------------------------------------------------------

// every game thread also reads its stack-protector canary from TPIDR_EL0+0x28
// (see main.c). Install a fresh zeroed bionic TLS block on entry; it is leaked
// on thread exit on purpose (it stays live in TPIDR_EL0 until teardown).
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *pthread_tls_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  uint8_t *tls = calloc(1, 0x100);
  armSetTlsRw(tls);
  return s.entry(s.arg);
}

static int pthread_create_fake(pthread_t *thread, const void *attr_unused, void *(*entry)(void *), void *arg) {
  (void)attr_unused;
  ThreadStart *s = malloc(sizeof(*s));
  if (!s)
    return -1;
  s->entry = entry;
  s->arg = arg;
  debugPrintf("pthread_create: entry=%p\n", (void *)entry);
  // generous stacks: Swordigo's Lua/loader threads recurse deeply
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024);
  int r = pthread_create(thread, &a, pthread_tls_trampoline, s);
  pthread_attr_destroy(&a);
  if (r != 0)
    free(s);
  return r;
}

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m)
    return -1;
  const int recursive = (attr && *attr == 1);
  *m = recursive ? (pthread_mutex_t)PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
                  : (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
  if (pthread_mutex_init(m, NULL) != 0) {
    free(m);
    return -1;
  }
  *uid = m;
  return 0;
}

// bionic static initializers leave the storage zeroed (normal) or set to a
// small sentinel (recursive); materialize a real mutex on first use
static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid)
    return pthread_mutex_init_fake(uid, NULL);
  if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    return pthread_mutex_init_fake(uid, &attr);
  }
  return 0;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid);
  return r ? r : pthread_mutex_lock(*uid);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid);
  return r ? r : pthread_mutex_unlock(*uid);
}

static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    init_routine();
  return 0;
}

// ---------------------------------------------------------------------------
// file path resolution
//
// the wrapper hands the game an absolute files/cache dir (the .nro directory),
// so the saves it builds come back as absolute paths and are opened verbatim.
// everything relative is read-only game data and lives under assets/.
// ---------------------------------------------------------------------------

static int is_absolute(const char *p) {
  return p && (p[0] == '/' || strchr(p, ':') != NULL);
}

static void resolve_path(const char *in, char *out, size_t n) {
  if (is_absolute(in))
    snprintf(out, n, "%s", in);
  else
    snprintf(out, n, "assets/%s", in);
}

static FILE *fopen_hook(const char *path, const char *mode) {
  if (path && !strcmp(path, "/dev/urandom")) {
    // no /dev/urandom on Switch: synthesize a small random file once
    FILE *f = fopen("urandom.tmp", "wb");
    if (f) {
      for (int i = 0; i < 256; i++) {
        unsigned char b = (unsigned char)rand();
        fwrite(&b, 1, 1, f);
      }
      fclose(f);
    }
    return fopen("urandom.tmp", mode);
  }
  char real[512];
  resolve_path(path, real, sizeof(real));
  FILE *f = fopen(real, mode);
  if (f && mode && strchr(mode, 'r'))
    setvbuf(f, NULL, _IOFBF, 64 * 1024); // fewer fsdev round trips
  return f;
}

// bionic open() flag bits differ from newlib's; translate then prefix
#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int open_hook(const char *path, int flags, ...) {
  int out_flags = flags & 3;
  if (flags & LINUX_O_CREAT)  out_flags |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out_flags |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out_flags |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out_flags |= O_APPEND;

  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  char real[512];
  resolve_path(path, real, sizeof(real));
  return open(real, out_flags, mode);
}

// ---------------------------------------------------------------------------
// AAsset emulation: the game's AssetManager calls read straight from the
// assets/ tree on disk. We keep both a FILE* (for read/seek) and the resolved
// path (so AAsset_openFileDescriptor can hand out an independent fd).
// ---------------------------------------------------------------------------

typedef struct {
  FILE *f;
  long size;
  char path[512];
} Asset;

static void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr;
  return (void *)1;
}

static void *AAssetManager_open_fake(void *mgr, const char *name, int mode) {
  (void)mgr; (void)mode;
  char real[512];
  snprintf(real, sizeof(real), "assets/%s", name);
  FILE *f = fopen(real, "rb");
  if (!f) {
    // benign: the engine probes for optional loose-file overrides and falls
    // back to the packed asset. Logging every miss floods the log, so stay
    // quiet here.
    return NULL;
  }
  setvbuf(f, NULL, _IOFBF, 32 * 1024);
  Asset *a = malloc(sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", real);
  return a;
}

static void AAsset_close_fake(void *asset) {
  Asset *a = asset;
  if (a) { fclose(a->f); free(a); }
}

static int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  return a ? (int)fread(buf, 1, count, a->f) : -1;
}

static long AAsset_getLength_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size : 0;
}

// returns an independent fd positioned at the asset start; the engine uses
// this to hand a descriptor to decoders. start/length describe the region.
static int AAsset_openFileDescriptor_fake(void *asset, off_t *out_start, off_t *out_len) {
  Asset *a = asset;
  if (!a)
    return -1;
  int fd = open(a->path, O_RDONLY);
  if (fd < 0)
    return -1;
  if (out_start) *out_start = 0;
  if (out_len)   *out_len = a->size;
  return fd;
}

// ---------------------------------------------------------------------------
// OpenAL: device + context are pre-created here so the game's alcOpenDevice /
// alcCreateContext just hand back the existing objects (same approach as the
// Vita port); the rest of the API is the native openal-soft.
// ---------------------------------------------------------------------------

static ALCdevice *g_al_device = NULL;
static ALCcontext *g_al_context = NULL;

void init_openal(void) {
  g_al_device = alcOpenDevice(NULL);
  if (!g_al_device) {
    debugPrintf("openal: alcOpenDevice failed\n");
    return;
  }
  const ALCint attr[] = { ALC_FREQUENCY, 48000, 0 };
  g_al_context = alcCreateContext(g_al_device, attr);
  alcMakeContextCurrent(g_al_context);
  debugPrintf("openal: device=%p context=%p\n", g_al_device, g_al_context);
}

void deinit_openal(void) {
  if (g_al_context) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(g_al_context);
    g_al_context = NULL;
  }
  if (g_al_device) {
    alcCloseDevice(g_al_device);
    g_al_device = NULL;
  }
}

static ALCdevice *alcOpenDevice_hook(const ALCchar *name) {
  (void)name;
  return g_al_device;
}
static ALCcontext *alcCreateContext_hook(ALCdevice *dev, const ALCint *attr) {
  (void)dev; (void)attr;
  return g_al_context;
}
// Apportable's openal-soft has device suspend/resume extensions native
// openal-soft lacks; no-op them
static void alc_noop(void) {}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- runtime / misc ---
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "_ctype_", (uintptr_t)&__ctype_ptr_ },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&google_region_noop },
  { "__google_potentially_blocking_region_end", (uintptr_t)&google_region_noop },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "syscall", (uintptr_t)&syscall_fake },
  { "abort", (uintptr_t)&abort_hook },
  { "exit", (uintptr_t)&exit },
  { "setlocale", (uintptr_t)&setlocale },

  // --- AAsset / ANativeWindow ---
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },

  // --- pthread (the subset the game actually imports) ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },

  // --- stdio / files ---
  { "fopen", (uintptr_t)&fopen_hook },
  { "open", (uintptr_t)&open_hook },
  { "freopen", (uintptr_t)&freopen_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },
  { "fgetc", (uintptr_t)&fgetc },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "getwc", (uintptr_t)&getwc },
  { "putwc", (uintptr_t)&putwc },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "putc", (uintptr_t)&putc },
  { "puts", (uintptr_t)&puts },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "perror", (uintptr_t)&perror_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "mkdir", (uintptr_t)&mkdir },
  { "read", (uintptr_t)&read },
  { "write", (uintptr_t)&write },
  { "writev", (uintptr_t)&writev_fake },
  { "close", (uintptr_t)&close },
  { "lseek", (uintptr_t)&lseek },
  { "ioctl", (uintptr_t)&retm1 },
  { "poll", (uintptr_t)&poll_fake },
  { "opendir", (uintptr_t)&opendir },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },

  // --- zlib (gz* stream API) ---
  { "gzopen", (uintptr_t)&gzopen },
  { "gzdopen", (uintptr_t)&gzdopen },
  { "gzclose", (uintptr_t)&gzclose },
  { "gzread", (uintptr_t)&gzread },
  { "gzwrite", (uintptr_t)&gzwrite },

  // --- printf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "vsprintf", (uintptr_t)&vsprintf },

  // --- string / mem ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strerror", (uintptr_t)&strerror },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtold", (uintptr_t)&strtold },
  { "strtoul", (uintptr_t)&strtoul },
  { "strxfrm", (uintptr_t)&strxfrm },

  // --- wide char ---
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "iswctype", (uintptr_t)&iswctype },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },

  // --- ctype ---
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "islower", (uintptr_t)&islower },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },

  // --- memory / stdlib ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "rand", (uintptr_t)&rand },

  // --- time ---
  { "clock", (uintptr_t)&clock },
  { "time", (uintptr_t)&time },
  { "localtime", (uintptr_t)&localtime },
  { "gettimeofday", (uintptr_t)&gettimeofday },

  // --- math ---
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "atan2f", (uintptr_t)&atan2f },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "fmodf", (uintptr_t)&fmodf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },

  // --- EGL (only this one is imported; the wrapper owns the context) ---
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },

  // --- OpenGL ES 1.1 (mesa libGLESv1_CM) ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColor4f", (uintptr_t)&glColor4f },
  { "glColor4ub", (uintptr_t)&glColor4ub },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glColorPointer", (uintptr_t)&glColorPointer },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glFlush", (uintptr_t)&glFlush },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glLightModelfv", (uintptr_t)&glLightModelfv },
  { "glLightf", (uintptr_t)&glLightf },
  { "glLightfv", (uintptr_t)&glLightfv },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
  { "glMaterialfv", (uintptr_t)&glMaterialfv },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glNormalPointer", (uintptr_t)&glNormalPointer },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glScalef", (uintptr_t)&glScalef },
  { "glScissor", (uintptr_t)&glScissor },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glTexEnvfv", (uintptr_t)&glTexEnvfv },
  { "glTexEnvi", (uintptr_t)&glTexEnvi },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTranslatef", (uintptr_t)&glTranslatef },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // --- OpenAL (native openal-soft; create calls are hooked) ---
  { "alBufferData", (uintptr_t)&alBufferData },
  { "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
  { "alDeleteSources", (uintptr_t)&alDeleteSources },
  { "alDistanceModel", (uintptr_t)&alDistanceModel },
  { "alGenBuffers", (uintptr_t)&alGenBuffers },
  { "alGenSources", (uintptr_t)&alGenSources },
  { "alGetError", (uintptr_t)&alGetError },
  { "alGetSourcef", (uintptr_t)&alGetSourcef },
  { "alGetSourcei", (uintptr_t)&alGetSourcei },
  { "alListener3f", (uintptr_t)&alListener3f },
  { "alListenerf", (uintptr_t)&alListenerf },
  { "alListenerfv", (uintptr_t)&alListenerfv },
  { "alSource3f", (uintptr_t)&alSource3f },
  { "alSourcePause", (uintptr_t)&alSourcePause },
  { "alSourcePlay", (uintptr_t)&alSourcePlay },
  { "alSourceRewind", (uintptr_t)&alSourceRewind },
  { "alSourceStop", (uintptr_t)&alSourceStop },
  { "alSourcef", (uintptr_t)&alSourcef },
  { "alSourcei", (uintptr_t)&alSourcei },
  { "alcCloseDevice", (uintptr_t)&alcCloseDevice },
  { "alcCreateContext", (uintptr_t)&alcCreateContext_hook },
  { "alcDestroyContext", (uintptr_t)&alcDestroyContext },
  { "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
  { "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
  { "alcOpenDevice", (uintptr_t)&alcOpenDevice_hook },
  { "alcProcessContext", (uintptr_t)&alcProcessContext },
  { "alcSuspendContext", (uintptr_t)&alcSuspendContext },
  { "alcSuspend", (uintptr_t)&alc_noop },
  { "alcResume", (uintptr_t)&alc_noop },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);
