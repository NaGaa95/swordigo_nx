/* music_player.h -- streaming MP3 music via mpg123 + OpenAL
 *
 * Swordigo drives background music through its Java MusicPlayer class
 * (initMusicPlayer + loadFile/play/pause/stop/setLooping/setVolume). We
 * receive those calls through the fake JNI layer and implement them here by
 * streaming the matching res/ mp3 through a dedicated OpenAL source on its
 * own thread, so a long updateApplication() frame can't starve playback.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __MUSIC_PLAYER_H__
#define __MUSIC_PLAYER_H__

// the absolute base directory; res/*.mp3 are read from <base>/res/
void music_init(const char *base_dir);
// logical track name as passed by the game, e.g. "1-boss23", "gameover"
void music_load(const char *logical_name);
void music_play(void);
void music_pause(void);
void music_stop(void);
void music_set_loop(int looping);
void music_set_volume(float vol);
void music_deinit(void);

#endif
