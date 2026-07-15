/* music_player.c -- streaming MP3 music via mpg123 + OpenAL
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>

#include "music_player.h"
#include "util.h"

// logical track name (as the game passes to MusicPlayer.loadFile) -> the
// obfuscated raw resource file in the APK. Pulled from resources.arsc for
// Swordigo 1.4.12-47 (parse_arsc.py). The user copies the APK's res/ folder
// verbatim, so these names are matched as-is.
typedef struct { const char *name; const char *file; } MusicEntry;
static const MusicEntry music_table[] = {
  { "1-boss23",       "res/7c.mp3" },
  { "1-dung73",       "res/s7.mp3" },
  { "1-hero2",        "res/jy.mp3" },
  { "1-plaintest2",   "res/3H.mp3" },
  { "2cave2",         "res/Fc.mp3" },
  { "gameover",       "res/md.mp3" },
  { "heartbeat",      "res/R2.mp3" },
  { "momentofwonder", "res/LM.mp3" },
  { "squire_new2",    "res/lU.mp3" },
};

#define NUM_BUFFERS 4
#define BUFFER_SIZE (32 * 1024)

static char s_base[256];
static int s_inited = 0;

// everything below the lock is shared with the streaming thread
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static volatile int s_thread_run = 0;

static mpg123_handle *s_mh = NULL;
static ALuint s_source = 0;
static ALuint s_buffers[NUM_BUFFERS];
static char s_pending_path[256];   // file to (re)open; empty when none
static int  s_want_play = 0;
static int  s_want_pause = 0;
static int  s_loop = 0;
static float s_volume = 1.0f;
static int  s_rate = 44100;
static int  s_channels = 2;
static ALenum s_format = AL_FORMAT_STEREO16;
static int  s_open = 0;            // an mpg123 stream is currently open

static const char *resolve_path(const char *logical) {
  for (size_t i = 0; i < sizeof(music_table) / sizeof(*music_table); i++)
    if (!strcmp(music_table[i].name, logical))
      return music_table[i].file;
  return NULL;
}

// decode up to BUFFER_SIZE bytes into buf; returns bytes produced, looping or
// stopping at EOF according to s_loop. caller holds s_lock.
static size_t fill_buffer(unsigned char *buf) {
  size_t total = 0;
  while (total < BUFFER_SIZE) {
    size_t done = 0;
    int err = mpg123_read(s_mh, buf + total, BUFFER_SIZE - total, &done);
    total += done;
    if (err == MPG123_OK)
      continue;
    if (err == MPG123_DONE) {
      if (s_loop) {
        mpg123_seek(s_mh, 0, SEEK_SET);
        if (done == 0 && total == 0)
          continue; // empty read at EOF, retry from the top
        continue;
      }
      break; // not looping: stop feeding
    }
    break; // MPG123_NEED_MORE / error: stop for now
  }
  return total;
}

static void open_stream(const char *path) {
  char full[512];
  snprintf(full, sizeof(full), "%s/%s", s_base, path);

  if (s_open) {
    mpg123_close(s_mh);
    s_open = 0;
  }
  if (mpg123_open(s_mh, full) != MPG123_OK) {
    debugPrintf("music: mpg123_open(%s) failed\n", full);
    return;
  }
  long rate = 44100;
  int channels = 2, encoding = 0;
  if (mpg123_getformat(s_mh, &rate, &channels, &encoding) != MPG123_OK) {
    debugPrintf("music: getformat failed for %s\n", full);
    mpg123_close(s_mh);
    return;
  }
  // lock the output format so mpg123 never renegotiates mid-stream
  mpg123_format_none(s_mh);
  mpg123_format(s_mh, rate, channels, MPG123_ENC_SIGNED_16);
  s_rate = (int)rate;
  s_channels = channels;
  s_format = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
  s_open = 1;
  debugPrintf("music: opened %s (%d hz, %d ch)\n", full, s_rate, s_channels);
}

static void stop_source(void) {
  alSourceStop(s_source);
  // drain any queued buffers
  ALint queued = 0;
  alGetSourcei(s_source, AL_BUFFERS_QUEUED, &queued);
  while (queued-- > 0) {
    ALuint b;
    alSourceUnqueueBuffers(s_source, 1, &b);
  }
}

static void prime_and_play(void) {
  unsigned char buf[BUFFER_SIZE];
  stop_source();
  int primed = 0;
  for (int i = 0; i < NUM_BUFFERS; i++) {
    size_t n = fill_buffer(buf);
    if (n == 0)
      break;
    alBufferData(s_buffers[i], s_format, buf, n, s_rate);
    alSourceQueueBuffers(s_source, 1, &s_buffers[i]);
    primed++;
  }
  if (primed > 0)
    alSourcePlay(s_source);
}

static void *stream_thread(void *arg) {
  (void)arg;
  unsigned char buf[BUFFER_SIZE];

  while (s_thread_run) {
    pthread_mutex_lock(&s_lock);

    // 1. honour a pending load
    if (s_pending_path[0]) {
      open_stream(s_pending_path);
      s_pending_path[0] = '\0';
      if (s_want_play && s_open)
        prime_and_play();
    }

    // 2. start request
    if (s_want_play && s_open) {
      ALint state = 0;
      alGetSourcei(s_source, AL_SOURCE_STATE, &state);
      if (state != AL_PLAYING && state != AL_PAUSED)
        prime_and_play();
      s_want_play = 0;
    }

    // 3. pause/resume edge
    if (s_open) {
      ALint state = 0;
      alGetSourcei(s_source, AL_SOURCE_STATE, &state);
      if (s_want_pause && state == AL_PLAYING)
        alSourcePause(s_source);
      else if (!s_want_pause && state == AL_PAUSED)
        alSourcePlay(s_source);
    }

    // 4. recycle processed buffers
    if (s_open && !s_want_pause) {
      ALint processed = 0;
      alGetSourcei(s_source, AL_BUFFERS_PROCESSED, &processed);
      while (processed-- > 0) {
        ALuint b = 0;
        alSourceUnqueueBuffers(s_source, 1, &b);
        size_t n = fill_buffer(buf);
        if (n > 0) {
          alBufferData(b, s_format, buf, n, s_rate);
          alSourceQueueBuffers(s_source, 1, &b);
        }
      }
      // requeue underran source that still has data
      ALint state = 0, queued = 0;
      alGetSourcei(s_source, AL_SOURCE_STATE, &state);
      alGetSourcei(s_source, AL_BUFFERS_QUEUED, &queued);
      if (state == AL_STOPPED && queued > 0)
        alSourcePlay(s_source);
    }

    alSourcef(s_source, AL_GAIN, s_volume);

    pthread_mutex_unlock(&s_lock);
    usleep(10 * 1000); // ~10ms; buffers hold ~180ms each so this is plenty
  }
  return NULL;
}

void music_init(const char *base_dir) {
  if (s_inited)
    return;
  snprintf(s_base, sizeof(s_base), "%s", base_dir ? base_dir : ".");

  if (mpg123_init() != MPG123_OK) {
    debugPrintf("music: mpg123_init failed\n");
    return;
  }
  int err = 0;
  s_mh = mpg123_new(NULL, &err);
  if (!s_mh) {
    debugPrintf("music: mpg123_new failed: %s\n", mpg123_plain_strerror(err));
    return;
  }

  // the game already created the OpenAL context by the time it inits music;
  // our source/buffers live on that current context
  alGenSources(1, &s_source);
  alGenBuffers(NUM_BUFFERS, s_buffers);
  alSourcef(s_source, AL_GAIN, 1.0f);

  s_inited = 1;
  s_thread_run = 1;
  pthread_create(&s_thread, NULL, stream_thread, NULL);
  debugPrintf("music: initialized (base=%s)\n", s_base);
}

void music_load(const char *logical_name) {
  if (!s_inited || !logical_name)
    return;
  const char *file = resolve_path(logical_name);
  if (!file) {
    debugPrintf("music: unknown track '%s'\n", logical_name);
    return;
  }
  pthread_mutex_lock(&s_lock);
  snprintf(s_pending_path, sizeof(s_pending_path), "%s", file);
  pthread_mutex_unlock(&s_lock);
}

void music_play(void) {
  if (!s_inited) return;
  pthread_mutex_lock(&s_lock);
  s_want_play = 1;
  s_want_pause = 0;
  pthread_mutex_unlock(&s_lock);
}

void music_pause(void) {
  if (!s_inited) return;
  pthread_mutex_lock(&s_lock);
  // Java MediaPlayer.pause() is idempotent.
  s_want_pause = 1;
  pthread_mutex_unlock(&s_lock);
}

void music_stop(void) {
  if (!s_inited) return;
  pthread_mutex_lock(&s_lock);
  if (s_open)
    stop_source();
  s_want_play = 0;
  pthread_mutex_unlock(&s_lock);
}

void music_set_loop(int looping) {
  if (!s_inited) return;
  pthread_mutex_lock(&s_lock);
  s_loop = looping;
  pthread_mutex_unlock(&s_lock);
}

void music_set_volume(float vol) {
  if (!s_inited) return;
  pthread_mutex_lock(&s_lock);
  s_volume = vol;
  pthread_mutex_unlock(&s_lock);
}

void music_deinit(void) {
  if (!s_inited) return;
  s_thread_run = 0;
  pthread_join(s_thread, NULL);
  pthread_mutex_lock(&s_lock);
  if (s_open) { mpg123_close(s_mh); s_open = 0; }
  alDeleteSources(1, &s_source);
  alDeleteBuffers(NUM_BUFFERS, s_buffers);
  mpg123_delete(s_mh);
  mpg123_exit();
  s_inited = 0;
  pthread_mutex_unlock(&s_lock);
}
