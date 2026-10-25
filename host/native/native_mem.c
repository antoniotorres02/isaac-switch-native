/* Guest memory window for the native host of the browsing-of-isaac lift.
 *
 * The lifted code addresses guest memory as recomp_mem_base + (uint32_t)va
 * (RECOMP_MEM_IDENTITY=0). A full 4 GiB window is reserved so every 32-bit
 * guest address, including a wild one, stays inside the reservation. Only
 * the guest map below ISAAC_HOST_BASE_VA is readable and writable; the rest
 * faults, which is where wasm32 put host-only memory. The low pages are
 * mapped like wasm linear memory (address 0 reads zero) unless
 * ISAAC_NATIVE_NULL_GUARD=1 asks for a trapping null page. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __SWITCH__
#include <sys/mman.h>
#include <execinfo.h>
#endif

#include "isaac_host.h"

uint8_t *recomp_mem_base;

#define WINDOW_BYTES (1ull << 32)
#define NULL_GUARD_BYTES 0x10000u

#ifdef __SWITCH__
/* Horizon has no mmap: the guest map comes from the application heap,
 * zeroed like fresh wasm memory. Nothing traps above it, so a wild guest
 * address past ISAAC_HOST_BASE_VA reaches other heap memory; the PC build keeps the
 * 4 GiB reservation that catches those. */
int isaac_native_mem_init(void) {
    extern int isaac_native_switch_mapped(const void *p, size_t n);
    extern void isaac_native_stage(const char *fmt, ...);
    void *w = aligned_alloc(0x10000, ISAAC_HOST_BASE_VA);
    if (!w || !isaac_native_switch_mapped(w, ISAAC_HOST_BASE_VA)) {
        isaac_native_stage("guest map: %u MiB at %p %s", ISAAC_HOST_BASE_VA >> 20, w,
                           w ? "is not all mapped" : "could not be allocated");
        fprintf(stderr, "[native] allocating the %u MiB guest map failed\n", ISAAC_HOST_BASE_VA >> 20);
        return -1;
    }
    memset(w, 0, ISAAC_HOST_BASE_VA);
    recomp_mem_base = (uint8_t *)w;
    fprintf(stderr, "[native] guest map at %p, 0x00000000..0x%08x (heap)\n", w, ISAAC_HOST_BASE_VA);
    return 0;
}

uint32_t isaac_va_escape(const void *p, const void *caller) {
    if (!p) return 0;
    fprintf(stderr, "[native] FATAL: host pointer %p handed to the guest (outside the "
            "0x%08x guest map, base %p) from %p\n", p, ISAAC_HOST_BASE_VA,
            (void *)recomp_mem_base, caller);
    fflush(stderr);
    abort();
}
#else
int isaac_native_mem_init(void) {
    void *w = mmap(NULL, WINDOW_BYTES, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (w == MAP_FAILED) {
        fprintf(stderr, "[native] reserving the 4 GiB guest window failed: %s\n", strerror(errno));
        return -1;
    }
    const char *g = getenv("ISAAC_NATIVE_NULL_GUARD");
    uint32_t lo = (g && *g == '1') ? NULL_GUARD_BYTES : 0u;
    if (mprotect((uint8_t *)w + lo, (size_t)ISAAC_HOST_BASE_VA - lo, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "[native] committing the guest map failed: %s\n", strerror(errno));
        munmap(w, WINDOW_BYTES);
        return -1;
    }
    recomp_mem_base = (uint8_t *)w;
    fprintf(stderr, "[native] guest window at %p, map 0x%08x..0x%08x read/write%s\n",
            w, lo, ISAAC_HOST_BASE_VA, lo ? " (null page traps)" : "");
    return 0;
}

/* A host pointer reached a guest-visible slot. NULL is the guest's 0; any
 * other address outside the map is a hand-off that needs a guest-side copy,
 * so name the call site and stop. */
uint32_t isaac_va_escape(const void *p, const void *caller) {
    if (!p) return 0;
    fprintf(stderr, "[native] FATAL: host pointer %p handed to the guest (outside the "
            "0x%08x guest map, base %p) from %p\n", p, ISAAC_HOST_BASE_VA,
            (void *)recomp_mem_base, caller);
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}
#endif
