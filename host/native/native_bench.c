/* Frame timing for benchmarks (ISAAC_NATIVE_BENCH=<scenario>).
 *
 *   ISAAC_NATIVE_BENCH=room|horde|swarm|tears   the bench mod's load (native_main.c
 *                                               seeds it); turns the timing on
 *   ISAAC_NATIVE_BENCH_FROM=1500                first presented frame measured
 *   ISAAC_NATIVE_BENCH_TO=2700                  last one
 *
 * A frame is the time from the end of one swap to the end of the next, so it
 * holds the game's update and draw, the GL backend, the driver and the swap.
 * The swap is also timed alone: without vsync it only blocks when the GPU is
 * behind, so a large share there means the GPU, not the CPU, sets the pace.
 * The deterministic clock gives every run the same game ticks per frame (two
 * frames per 30 Hz tick), so the same scenario is the same work on every
 * build and console; only how long it takes changes.
 *
 * The result is one line, BENCH {...}, on stderr (nxlink on the Switch). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The platform's sampler (native_prof_switch.c) follows the window. */
void isaac_native_bench_window(int begin) __attribute__((weak));
void isaac_native_bench_extra(FILE *out) __attribute__((weak));
/* The render gate (host_shims_win.c): with it on, a presented frame may hold
 * a whole tick, and the frame counts below are ticks. */
uint32_t isaac_render_gate_skipped(void);
uint32_t isaac_render_gate_drawn(void);
uint32_t isaac_native_game_tick(void);

static struct {
    int state;                /* 0 unread, 1 on, -1 off */
    const char *scenario;
    unsigned from, to;
    uint64_t last;
    uint32_t skipped0, drawn0, tick0;
    float *frame, *swap;
    size_t n;
} B;

uint64_t isaac_native_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int isaac_native_bench_on(void) {
    if (!B.state) {
        const char *s = getenv("ISAAC_NATIVE_BENCH"), *f = getenv("ISAAC_NATIVE_BENCH_FROM"),
                   *t = getenv("ISAAC_NATIVE_BENCH_TO");
        B.state = s && *s ? 1 : -1;
        B.scenario = s;
        B.from = f ? (unsigned)atoi(f) : 1500;
        B.to = t ? (unsigned)atoi(t) : 2700;
        if (B.state > 0 && B.to > B.from) {
            B.frame = calloc(B.to - B.from, sizeof *B.frame);
            B.swap = calloc(B.to - B.from, sizeof *B.swap);
        }
        if (!B.frame || !B.swap) B.state = -1;
    }
    return B.state > 0;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void report(void) {
    size_t n = B.n;
    double sum = 0, swap = 0, over16 = 0, over33 = 0, worst = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += B.frame[i];
        swap += B.swap[i];
        if (B.frame[i] > 1000.0 / 60.0) ++over16;
        if (B.frame[i] > 1000.0 / 30.0) ++over33;
        if (B.frame[i] > worst) worst = B.frame[i];
    }
    /* A 30 Hz tick spans two frames, and only one of them updates the game:
     * the pair is the unit that has to fit in 33.3 ms. */
    size_t np = n / 2;
    float *pairs = calloc(np ? np : 1, sizeof *pairs);
    for (size_t i = 0; i < np; ++i) pairs[i] = B.frame[2 * i] + B.frame[2 * i + 1];
    qsort(B.frame, n, sizeof *B.frame, cmp_float);
    qsort(pairs, np, sizeof *pairs, cmp_float);
#define PCT(a, k, p) ((k) ? (a)[(size_t)((double)((k) - 1) * (p))] : 0.0f)
    fprintf(stderr, "BENCH {\"scenario\":\"%s\",\"from\":%u,\"to\":%u,\"frames\":%zu,"
            "\"mean_ms\":%.3f,\"p50_ms\":%.3f,\"p90_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.3f,"
            "\"swap_mean_ms\":%.3f,\"work_mean_ms\":%.3f,\"over_60fps_pct\":%.1f,\"over_30fps_pct\":%.1f,"
            "\"pair_p50_ms\":%.3f,\"pair_p90_ms\":%.3f,\"pair_max_ms\":%.3f,\"fps\":%.2f",
            B.scenario, B.from, B.to, n, n ? sum / n : 0, PCT(B.frame, n, 0.5), PCT(B.frame, n, 0.9),
            PCT(B.frame, n, 0.99), worst, n ? swap / n : 0, n ? (sum - swap) / n : 0,
            n ? 100.0 * over16 / n : 0, n ? 100.0 * over33 / n : 0,
            PCT(pairs, np, 0.5), PCT(pairs, np, 0.9), np ? pairs[np - 1] : 0.0f, sum > 0 ? 1000.0 * n / sum : 0);
#undef PCT
    /* Game ticks in the window and the time each took: the number that says
     * whether the game holds its speed, with the gate on or off. */
    uint32_t ticks = isaac_native_game_tick() - B.tick0;
    const char *gate = getenv("ISAAC_RENDER_GATE");
    fprintf(stderr, ",\"render_gate\":%d,\"gate_skipped\":%u,\"gate_drawn\":%u,\"ticks\":%u,\"tick_ms\":%.3f",
            gate ? atoi(gate) : 0, isaac_render_gate_skipped() - B.skipped0, isaac_render_gate_drawn() - B.drawn0,
            ticks, ticks ? sum / ticks : 0.0);
    if (isaac_native_bench_extra) isaac_native_bench_extra(stderr);
    fputs("}\n", stderr);
    fflush(stderr);
    free(pairs);
}

/* After every presented frame's swap; n counts presented frames from 1. */
void isaac_native_bench_frame(unsigned n, uint64_t swap_ns) {
    if (!isaac_native_bench_on()) return;
    uint64_t now = isaac_native_now_ns();
    if (n == B.from) {
        B.skipped0 = isaac_render_gate_skipped();
        B.drawn0 = isaac_render_gate_drawn();
        B.tick0 = isaac_native_game_tick();
        if (isaac_native_bench_window) isaac_native_bench_window(1);
    }
    if (n > B.from && n <= B.to && B.last && B.n < B.to - B.from) {
        B.frame[B.n] = (float)((now - B.last) / 1e6);
        B.swap[B.n] = (float)(swap_ns / 1e6);
        ++B.n;
        if (n == B.to) {
            if (isaac_native_bench_window) isaac_native_bench_window(0);
            report();
        }
    }
    B.last = now;
}
