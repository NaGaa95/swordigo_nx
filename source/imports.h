/* imports.h -- .so import resolution table
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

// pre-creates the OpenAL device + context the game's alcOpenDevice/
// alcCreateContext hooks hand back, and makes the context current
void init_openal(void);
void deinit_openal(void);

#endif
