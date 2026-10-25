/* Native stand-in for <emscripten/threading.h>: the host only asks for the
 * logical core count (GetSystemInfo). */
#ifndef ISAAC_NATIVE_COMPAT_EMSCRIPTEN_THREADING_H
#define ISAAC_NATIVE_COMPAT_EMSCRIPTEN_THREADING_H

#include <unistd.h>

static inline int emscripten_num_logical_cores(void) {
#ifdef __SWITCH__
    return 3;                    /* the cores an application may run on */
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

#endif
