/* Native stand-in for <emscripten.h> used by the browsing-of-isaac host
 * sources that include it unconditionally. Only the calls the node profile
 * makes outside ISAAC_WEB are provided; EM_JS bodies are web-only and must
 * never be compiled natively. */
#ifndef ISAAC_NATIVE_COMPAT_EMSCRIPTEN_H
#define ISAAC_NATIVE_COMPAT_EMSCRIPTEN_H

#include <stdlib.h>
#include <time.h>

static inline double emscripten_get_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static inline void emscripten_sleep(unsigned int ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

static inline void emscripten_force_exit(int status) { exit(status); }

#define EM_JS(ret, name, args, ...) \
    _Static_assert(0, "EM_JS " #name " compiled in a native build")
#define EM_ASYNC_JS(ret, name, args, ...) \
    _Static_assert(0, "EM_ASYNC_JS " #name " compiled in a native build")
#define EM_ASM(...) ((void)0)
#define EMSCRIPTEN_KEEPALIVE

#endif
