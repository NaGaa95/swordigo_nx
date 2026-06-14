/* main.c -- Swordigo .so loader for Nintendo Switch
 *
 * Swordigo (1.4.12) is a touch-only, fixed-function OpenGL ES 1.1 game. The
 * Java side is a thin shell: it forwards lifecycle, view-size, touch and music
 * calls into libswordigo.so's Java_com_touchfoo_swordigo_Native_* /
 * MusicPlayer_* entry points and otherwise does nothing. We replicate that
 * shell here -- bring up an ES1 context, drive the same boot sequence the
 * Android GameView did, then run the update/draw loop ourselves -- and map
 * the Switch pad onto the on-screen touch controls the way the Vita port did.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "egl_helper.h"
#include "music_player.h"

so_module game_mod;

// render resolution, set from the operation mode at startup
int screen_width = 1280;
int screen_height = 720;

static char data_path[256];

// ---------------------------------------------------------------------------
// heap: hand newlib a fixed slice and leave the rest for the .so load zone
// ---------------------------------------------------------------------------

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, (size_t)MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.\nUse a game override (hold R) or a forwarder.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  const char *files[] = {
    SO_NAME,                 // the game binary
    "assets/resources",      // game data tree (from the APK's assets/)
    "res/7c.mp3",            // one representative music track
  };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s\nCheck your data files (see README).", files[i]);
}

static void set_screen_size(void) {
  if (appletGetOperationMode() == AppletOperationMode_Console) {
    screen_width = 1920; screen_height = 1080;
  } else {
    screen_width = 1280; screen_height = 720;
  }
}

// ---------------------------------------------------------------------------
// Native / MusicPlayer entry points
// ---------------------------------------------------------------------------

// path args are jstrings (native unwraps via GetStringUTFChars) -> fake strings
static void (*setFilesDir)(void *env, void *obj, void *jpath);
static void (*setCacheDir)(void *env, void *obj, void *jpath);
static void (*setAssetManager)(void *env, void *obj, void *mgr);
static void (*setupNativeInterface)(void *env, void *obj);
static void (*setupApplication)(void *env, void *obj);
static void (*setApplicationViewSize)(void *env, void *obj, int w, int h, int is_pad);
static void (*handleApplicationLaunch)(void *env, void *obj);
static void (*applicationDidBecomeActive)(void *env, void *obj);
static void (*updateApplication)(void *env, void *obj, float dt);
static void (*drawApplication)(void *env, void *obj);
static void (*handleTouchEvent)(void *env, void *obj, int phase, int id, double time,
                                float x, float y, float old_x, float old_y, int tap_count);
static void (*initMusicPlayer)(void *env, void *obj);
static void (*googleSignInCompleted)(void *env, void *obj, uint8_t logged);
static void (*handleMenuButtonPress)(void *env, void *obj);

static void resolve_entry_points(void) {
  #define E(var, sym) var = (void *)so_find_addr_rx(&game_mod, "Java_com_touchfoo_swordigo_" sym)
  E(setFilesDir,             "Native_setFilesDir");
  E(setCacheDir,             "Native_setCacheDir");
  E(setAssetManager,         "Native_setAssetManager");
  E(setupNativeInterface,    "Native_setupNativeInterface");
  E(setupApplication,        "Native_setupApplication");
  E(setApplicationViewSize,  "Native_setApplicationViewSize");
  E(handleApplicationLaunch, "Native_handleApplicationLaunch");
  E(applicationDidBecomeActive, "Native_applicationDidBecomeActive");
  E(updateApplication,       "Native_updateApplication");
  E(drawApplication,         "Native_drawApplication");
  E(handleTouchEvent,        "Native_handleTouchEvent");
  E(initMusicPlayer,         "MusicPlayer_initMusicPlayer");
  E(googleSignInCompleted,   "Native_googleSignInCompleted");
  #undef E
  // optional: present in 1.4.12, used for the pause/menu button
  handleMenuButtonPress = (void *)so_try_find_addr_rx(&game_mod, "Java_com_touchfoo_swordigo_Native_handleMenuButtonPress");
}

// ---------------------------------------------------------------------------
// input: Switch pad -> the engine's on-screen touch controls
// touch phases match the Android MotionEvent ids the engine expects
// ---------------------------------------------------------------------------

enum { TOUCH_BEGAN = 1, TOUCH_ENDED, TOUCH_CANCELLED, TOUCH_MOVED };

// On-screen control button positions, measured directly off the in-game touch
// layout (1280x720 screenshot). Each is anchored to a screen edge by a fixed
// pixel offset (A_R = from right, A_T = from top) so the touch point is computed
// against the live view size and adapts to handheld/docked. Values are the
// button centers from the screenshot.
#define A_R 1  // x measured from the right edge
#define A_T 2  // y measured from the top edge (our touch space is bottom-origin)

typedef struct {
  u64 btn;
  int id;
  int flags;     // A_R | A_T
  float ax, ay;  // edge-anchor offset in view pixels (to the button center)
} Ctrl;

static const Ctrl button_map[] = {
  { HidNpadButton_B,     5, A_R,       185.0f, 105.0f }, // jump   (~1095,615)
  { HidNpadButton_Y,     6, A_R,       335.0f, 105.0f }, // attack (~945,615 sword)
  { HidNpadButton_A,     7, A_R,       190.0f, 240.0f }, // magic  (~1090,480)
  { HidNpadButton_X,     8, 0,         640.0f,  75.0f }, // item   (~640,645 potion)
  { HidNpadButton_Minus, 9, A_R | A_T, 150.0f,  65.0f }, // magic equip (~1130,65)
};

#define ID_MOVE_LEFT  10
#define ID_MOVE_RIGHT 11

static PadState pad;
static u64 pad_prev = 0;
static double g_time = 0.0;

// resolve an edge-anchored control to an absolute touch point in view space
static void ctrl_point(int flags, float ax, float ay, float *x, float *y) {
  *x = (flags & A_R) ? (float)screen_width  - ax : ax;
  *y = (flags & A_T) ? (float)screen_height - ay : ay;
}

static void emit_touch(int phase, int id, float x, float y) {
  handleTouchEvent(fake_env, NULL, phase, id, g_time, x, y, x, y,
                   phase == TOUCH_BEGAN ? 1 : 0);
}

static void emit_touch_drag(int phase, int id, float x, float y, float ox, float oy) {
  handleTouchEvent(fake_env, NULL, phase, id, g_time, x, y, ox, oy,
                   phase == TOUCH_BEGAN ? 1 : 0);
}

// edge-triggered fake touch for a control: down->BEGAN, held->MOVED, up->ENDED
static void drive_button(int held, int was_held, int id, float x, float y) {
  if (held)
    emit_touch(was_held ? TOUCH_MOVED : TOUCH_BEGAN, id, x, y);
  else if (was_held)
    emit_touch(TOUCH_ENDED, id, x, y);
}

static void update_input(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);

  for (unsigned i = 0; i < sizeof(button_map) / sizeof(*button_map); i++) {
    const Ctrl *c = &button_map[i];
    float x, y;
    ctrl_point(c->flags, c->ax, c->ay, &x, &y);
    drive_button(down & c->btn, pad_prev & c->btn, c->id, x, y);
  }

  // menu/pause goes through the dedicated entry point (no on-screen tap needed)
  if ((down & HidNpadButton_Plus) && !(pad_prev & HidNpadButton_Plus) && handleMenuButtonPress)
    handleMenuButtonPress(fake_env, NULL);

  // movement: the on-screen arrows from the screenshot -- left ~(195,615),
  // right ~(335,615). The old coords put "right" near the left arrow, which is
  // why right went left.
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const int left_now  = (down & HidNpadButton_Left)  || ls.x < -10000;
  const int right_now = (down & HidNpadButton_Right) || ls.x >  10000;
  static int left_prev = 0, right_prev = 0;
  drive_button(left_now,  left_prev,  ID_MOVE_LEFT,  195.0f, 105.0f);
  drive_button(right_now, right_prev, ID_MOVE_RIGHT, 335.0f, 105.0f);
  left_prev = left_now;
  right_prev = right_now;

  pad_prev = down;
}

// ---------------------------------------------------------------------------

#define TOUCH_PANEL_W 1280
#define TOUCH_PANEL_H 720
#define MAX_TOUCH     4

typedef struct {
  int active;
  u32 finger_id;
  float x, y;
} TouchSlot;

static TouchSlot touch_slots[MAX_TOUCH];

static int touch_slot_find(u32 finger_id) {
  for (int i = 0; i < MAX_TOUCH; i++)
    if (touch_slots[i].active && touch_slots[i].finger_id == finger_id)
      return i;
  return -1;
}

static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCH; i++)
    if (!touch_slots[i].active)
      return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState ts = { 0 };
  if (!hidGetTouchScreenStates(&ts, 1))
    return;

  const float sx = (float)screen_width  / (float)TOUCH_PANEL_W;
  const float sy = (float)screen_height / (float)TOUCH_PANEL_H;

  int seen[MAX_TOUCH] = { 0 };
  for (int i = 0; i < ts.count; i++) {
    const HidTouchState *t = &ts.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)screen_height - (float)t->y * sy;

    int s = touch_slot_find(t->finger_id);
    if (s < 0) {
      s = touch_slot_alloc();
      if (s < 0) continue; // more fingers than slots: ignore the extras
      touch_slots[s].active = 1;
      touch_slots[s].finger_id = t->finger_id;
      emit_touch_drag(TOUCH_BEGAN, s, x, y, x, y);
    } else if (x != touch_slots[s].x || y != touch_slots[s].y) {
      emit_touch_drag(TOUCH_MOVED, s, x, y, touch_slots[s].x, touch_slots[s].y);
    }
    touch_slots[s].x = x;
    touch_slots[s].y = y;
    seen[s] = 1;
  }

  for (int s = 0; s < MAX_TOUCH; s++)
    if (touch_slots[s].active && !seen[s]) {
      emit_touch_drag(TOUCH_ENDED, s, touch_slots[s].x, touch_slots[s].y,
                      touch_slots[s].x, touch_slots[s].y);
      touch_slots[s].active = 0;
    }
}

// ---------------------------------------------------------------------------

int main(void) {
  cpu_boost(1); // full clocks through boot; dropped after a few frames

  check_syscalls();
  check_data();

  if (!getcwd(data_path, sizeof(data_path)))
    strcpy(data_path, ".");

  set_screen_size();

  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  // Resolve entry points before so_finalize: so_finalize maps the .so via
  // svcMapProcessCodeMemory, after which the load_base copy that mod->syms /
  // dynstrtab point into is gone. The addresses resolved here are
  // load_virtbase-relative, so they stay valid to call afterwards.
  resolve_entry_points();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // libswordigo.so reads its stack canary from bionic's TLS at TPIDR_EL0+0x28,
  // but the Switch keeps the thread pointer in TPIDRRO_EL0 and leaves
  // TPIDR_EL0 zero. Point it at a zeroed block (game threads get their own in
  // pthread_create_fake) before any game code runs, i.e. before .init_array.
  static uint8_t main_fake_tls[0x100] __attribute__((aligned(16)));
  memset(main_fake_tls, 0, sizeof(main_fake_tls));
  armSetTlsRw(main_fake_tls);

  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);

  // audio + render contexts up before any setup* call touches them
  init_openal();
  egl_init(screen_width, screen_height);

  jni_init();

  // boot sequence, mirroring Android's GameActivity/GameView order
  setFilesDir(fake_env, NULL, jni_make_string(data_path));
  setCacheDir(fake_env, NULL, jni_make_string(data_path));
  setAssetManager(fake_env, NULL, NULL);
  googleSignInCompleted(fake_env, NULL, 0);
  handleApplicationLaunch(fake_env, NULL);

  music_init(data_path);
  initMusicPlayer(fake_env, NULL);

  setupNativeInterface(fake_env, NULL);
  setupApplication(fake_env, NULL);
  setApplicationViewSize(fake_env, NULL, screen_width, screen_height, 1);
  applicationDidBecomeActive(fake_env, NULL);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  u64 last_tick = armGetSystemTick();
  const u64 tick_freq = armGetSystemTickFreq();
  int boot_frames = 0;

  while (appletMainLoop()) {
    update_input();
    update_touch();

    const u64 now = armGetSystemTick();
    float dt = (float)(now - last_tick) / (float)tick_freq;
    last_tick = now;
    // clamp so a long load frame doesn't fling the player off-screen
    if (dt <= 0.0f || dt > 0.1f)
      dt = 1.0f / 60.0f;
    g_time += dt;

    updateApplication(fake_env, NULL, dt);
    drawApplication(fake_env, NULL);
    egl_swap();

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);
  }

  music_deinit();
  egl_deinit();
  deinit_openal();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
