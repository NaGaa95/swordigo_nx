/* config.h -- compile-time settings
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// libswordigo.so statically links its C++ runtime, Boost, Lua and protobuf,
// so a single module is loaded.
#define SO_NAME   "libswordigo.so"
#define LOG_NAME  "debug.log"

// newlib heap reserved before the .so load zone
#define MEMORY_MB 384

// enable per-line on-SD logging via debugPrintf (slow); only when chasing a bug
// #define DEBUG_LOG 1

// the render resolution in use, picked at startup from the operation mode
extern int screen_width;
extern int screen_height;

#endif
