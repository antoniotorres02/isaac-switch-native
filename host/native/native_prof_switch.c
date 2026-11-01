/* A57 sampler and clock readout for benchmarks on the Switch
 * (native_bench.c's window; native_switch.c --profile-hz N).
 *
 * A thread on another core pauses the game thread every 1/N s, reads its PC
 * and LR (svcSetThreadActivity + svcGetThreadContext3, both allowed on a
 * thread of the same process) and lets it run again. The samples stay in a
 * preallocated buffer -- the sampler takes no lock the paused thread could
 * hold -- and when the window closes they go out over nxlink as
 *   PROFBASE main=0x<runtime address of main>
 *   PROF <pc> <lr> <count>
 * rows, which tools/bench_native.py resolves against the ELF's symbols. A PC
 * inside a wait (the swap's fence, a futex) is the time the thread spent
 * blocked there, so blocking shows up as well as work.
 *
 * The clocks (clkrst) say what the measurement ran at: sys-clk may have
 * overclocked the CPU, GPU or memory.
 *
 * ISAAC_NATIVE_PROF_RENDER=1 samples the GL render thread instead.
 *
 * ISAAC_NATIVE_WATCHDOG=<s> (native_switch.c --env): from the game
 * thread's start, every S seconds the same pause prints
 *   WATCHDOG <pc> <lr> main=0x<runtime address of main>
 * so a hang names where the game thread is, whatever the log says last. */
#ifdef __SWITCH__
#include <switch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv);

static Handle g_target;
static int g_hz, g_game_core = -1, g_sampler_core = -1;
static Thread g_thread;
static volatile int g_alive, g_sampling;
static uint64_t *g_buf;               /* pc, lr pairs */
static size_t g_cap, g_n;
static uint32_t g_failed, g_dropped;
static uint32_t g_mhz_begin[3], g_mhz_end[3];

static const PcvModuleId CLOCKS[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };

/* The service is opened and closed around each read. An NRO runs inside the
 * loader's process, so a session it leaves open outlives it: one per run
 * exhausted pcv's session memory after a few benchmarks in a row, and pcv's
 * fatal (2011-0102) froze the console. */
static void read_clocks(uint32_t *mhz) {
    int ready = R_SUCCEEDED(clkrstInitialize());
    for (int i = 0; i < 3; ++i) {
        mhz[i] = 0;
        ClkrstSession s;
        u32 hz = 0;
        if (ready && R_SUCCEEDED(clkrstOpenSession(&s, CLOCKS[i], 3))) {
            if (R_SUCCEEDED(clkrstGetClockRate(&s, &hz))) mhz[i] = hz / 1000000u;
            clkrstCloseSession(&s);
        }
    }
    if (ready) clkrstExit();
}

static void sampler(void *arg) {
    (void)arg;
    const u64 period = 1000000000ull / (u64)g_hz;
    while (g_alive) {
        svcSleepThread((s64)period);
        if (!g_sampling) continue;
        if (R_FAILED(svcSetThreadActivity(g_target, ThreadActivity_Paused))) { ++g_failed; continue; }
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, g_target);
        svcSetThreadActivity(g_target, ThreadActivity_Runnable);
        if (R_FAILED(rc)) { ++g_failed; continue; }
        if (g_n == g_cap) { ++g_dropped; continue; }
        g_buf[2 * g_n] = ctx.pc.x;
        g_buf[2 * g_n + 1] = ctx.lr;
        ++g_n;
    }
}

static Thread g_dog;
static int g_dog_s;
static volatile int g_dog_alive;

static void watchdog(void *arg) {
    (void)arg;
    while (g_dog_alive) {
        for (int i = 0; i < g_dog_s * 10 && g_dog_alive; ++i) svcSleepThread(100000000ll);
        if (!g_dog_alive) break;
        if (R_FAILED(svcSetThreadActivity(g_target, ThreadActivity_Paused))) continue;
        ThreadContext ctx;
        Result rc = svcGetThreadContext3(&ctx, g_target);
        svcSetThreadActivity(g_target, ThreadActivity_Runnable);
        if (R_FAILED(rc)) continue;
        /* write(), not stdio: a hung thread may hold stderr's lock */
        char line[96];
        int n = snprintf(line, sizeof line, "WATCHDOG %lx %lx main=0x%lx\n", (unsigned long)ctx.pc.x,
                         (unsigned long)ctx.lr, (unsigned long)(uintptr_t)&main);
        if (n > 0) write(STDERR_FILENO, line, (size_t)n);
    }
}

/* The loader maps the next NRO over this one's code: the thread ends first. */
static void watchdog_stop(void) {
    if (!g_dog_alive) return;
    g_dog_alive = 0;
    threadWaitForExit(&g_dog);
    threadClose(&g_dog);
}

/* From native_switch.c's argument parsing: 0 Hz leaves the sampler off. */
void isaac_native_prof_config(int hz, int game_core) {
    g_hz = hz;
    g_game_core = game_core;
}

/* On the game thread, as it starts: pin it if asked, and remember it. */
void isaac_native_prof_game_thread(void) {
    if (g_game_core >= 0) svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_game_core, 1u << g_game_core);
    g_game_core = (int)svcGetCurrentProcessorNumber();
    g_target = threadGetCurHandle();
    const char *e = getenv("ISAAC_NATIVE_WATCHDOG");
    g_dog_s = e ? atoi(e) : 0;
    if (g_dog_s > 0 && R_SUCCEEDED(threadCreate(&g_dog, watchdog, NULL, NULL, 0x4000, 0x28, g_game_core == 2 ? 1 : 2))) {
        g_dog_alive = 1;
        if (R_SUCCEEDED(threadStart(&g_dog))) atexit(watchdog_stop);
        else { g_dog_alive = 0; threadClose(&g_dog); }
    }
}

/* On the render thread (native_glq.c), as it starts: with
 * ISAAC_NATIVE_PROF_RENDER=1 the sampler follows it instead of the game
 * thread. */
void isaac_native_prof_render_thread(void) {
    const char *e = getenv("ISAAC_NATIVE_PROF_RENDER");
    if (e && *e == '1') g_target = threadGetCurHandle();
}

static int cmp_pair(const void *a, const void *b) {
    const uint64_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

void isaac_native_bench_window(int begin) {
    if (begin) {
        read_clocks(g_mhz_begin);
        if (g_hz <= 0 || !g_target) return;
        g_cap = (size_t)g_hz * 240;              /* four minutes of samples */
        g_buf = malloc(g_cap * 2 * sizeof *g_buf);
        if (!g_buf) return;
        g_sampler_core = g_game_core == 2 ? 1 : 2;
        g_alive = 1;
        if (R_FAILED(threadCreate(&g_thread, sampler, NULL, NULL, 0x4000, 0x28, g_sampler_core)) ||
            R_FAILED(threadStart(&g_thread))) {
            g_alive = 0;
            fprintf(stderr, "[prof] sampler thread did not start\n");
            return;
        }
        g_sampling = 1;
        return;
    }
    read_clocks(g_mhz_end);
    if (!g_alive) return;
    g_sampling = 0;
    g_alive = 0;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    /* Sort the pairs and print each distinct one once with its count. */
    qsort(g_buf, g_n, 2 * sizeof *g_buf, cmp_pair);
    fprintf(stderr, "PROFBASE main=0x%lx\n", (unsigned long)(uintptr_t)&main);
    for (size_t i = 0; i < g_n;) {
        size_t j = i;
        while (j < g_n && g_buf[2 * j] == g_buf[2 * i] && g_buf[2 * j + 1] == g_buf[2 * i + 1]) ++j;
        fprintf(stderr, "PROF %lx %lx %zu\n", (unsigned long)g_buf[2 * i], (unsigned long)g_buf[2 * i + 1], j - i);
        i = j;
    }
    fflush(stderr);
}

/* Extra fields for the BENCH line. */
void isaac_native_bench_extra(FILE *out) {
    fprintf(out, ",\"cpu_mhz\":%u,\"gpu_mhz\":%u,\"emc_mhz\":%u,\"cpu_mhz_begin\":%u,\"gpu_mhz_begin\":%u,"
            "\"emc_mhz_begin\":%u,\"game_core\":%d,\"sampler_core\":%d,\"prof_hz\":%d,\"prof_samples\":%zu,"
            "\"prof_failed\":%u,\"prof_dropped\":%u",
            g_mhz_end[0], g_mhz_end[1], g_mhz_end[2], g_mhz_begin[0], g_mhz_begin[1], g_mhz_begin[2],
            g_game_core, g_sampler_core, g_hz, g_n, g_failed, g_dropped);
}
#endif
