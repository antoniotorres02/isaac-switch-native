/* EGL/GLES3 context for the native build of the real GL backend
 * (host_gl_webgl.c with ISAAC_NATIVE_GL). The web build draws into the
 * page's WebGL2 canvas; here the game draws into an off-screen pbuffer the
 * size of its window, and selected frames are read back as screenshots.
 *
 *   ISAAC_NATIVE_SHOTS=600,1200,1800   presented frames to save
 *   ISAAC_NATIVE_SHOT_DIR=<dir>         where (default: the working dir)
 *   ISAAC_NATIVE_FPS=60                 pace presents to a frame rate
 *                                       (default: unpaced, for measuring)
 *
 * Only compiled into the ISAAC_NATIVE_GL profile. The Switch build will
 * replace the pbuffer with an nwindow surface; the rest stays. */
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SURFACE_W 960
#define SURFACE_H 540

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static int g_state;            /* 0 untried, 1 ready, -1 failed */
static int g_window;           /* PC: the GLFW window owns the context (ISAAC_NATIVE_WINDOW=1) */

#ifndef __SWITCH__
int isaac_native_window_open(int w, int h);
void isaac_native_window_swap(void);
void isaac_native_window_close(void);
#endif

int isaac_native_bench_on(void);
uint64_t isaac_native_now_ns(void);
void isaac_native_bench_frame(unsigned n, uint64_t swap_ns);
static unsigned g_frames_seen;


/* the render thread (native_glq.c) */
int isaac_glq_start(void);
void isaac_glq_stop(void);
int isaac_glq_active(void);
void isaac_glq_run(void (*fn)(void *), void *ctx);
void isaac_glq_post(void (*fn)(const void *), const void *data, size_t bytes);
uint64_t isaac_glq_mark(void);
void isaac_glq_wait(uint64_t mark);
static uint64_t g_swap_mark;

uint32_t isaac_native_game_tick(void);
uint32_t isaac_render_gate_skipped(void);
void isaac_native_stage(const char *fmt, ...) __attribute__((weak));

/* ---- frame pacing record (Switch) ------------------------------------------ */
/* How many vblanks each presented frame and each game tick took, counted by a
 * thread on core 2 that waits on the display's vsync event. With vsync a
 * steady 60 fps holds every frame one vblank and every 30 Hz tick two; any
 * other count is a frame shown too early or too late, which the eye sees as a
 * jump even when the frame rate is right. The counts are kept per 10 s window;
 * each finished window goes to stderr (nxlink), and all of them to the stage
 * log at shutdown. */
#ifdef __SWITCH__
#include <pthread.h>
#include <stdatomic.h>
#define VB_WINDOW 600u           /* vblanks per window: 10 s */
#define VB_WINDOWS 360
#define VB_BINS 5                /* 0, 1, 2, 3, 4+ vblanks */
typedef struct {
    uint32_t frame[VB_BINS], tick[VB_BINS], skipped0, skipped;
    uint32_t shown_tick[VB_BINS], queued[VB_BINS];
    uint64_t swap_ns;            /* time spent inside eglSwapBuffers */
    uint64_t wait0, wait_ns;     /* the game thread waiting for the render thread */
    uint64_t idle0, idle_ns;     /* the render thread with nothing to run */
    double need_ms;              /* the in-between decisions' tick + render estimates, summed */
    uint32_t decides, skips;      /* decisions, and how many skipped the in-between frame */
} vb_window;
uint64_t isaac_glq_game_wait_ns(void);
uint64_t isaac_glq_render_idle_ns(void);
static Event g_vb_ev;
static pthread_t g_vb_thread;
static int g_vb_on;
static atomic_int g_vb_quit;
static atomic_uint g_vblank;
static atomic_uint g_tick_late;  /* ticks that took three vblanks or more */
static vb_window g_vb_w[VB_WINDOWS];
static unsigned g_vb_nw, g_vb_last, g_vb_tick_at, g_vb_shown, g_vb_shown_tick_at;
static uint32_t g_vb_tick;
static int g_vb_seen;
static pthread_mutex_t g_vb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_vb_cv = PTHREAD_COND_INITIALIZER;

static void *vb_main(void *arg) {
    (void)arg;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    while (!atomic_load(&g_vb_quit))
        if (R_SUCCEEDED(eventWait(&g_vb_ev, 100000000ull))) {
            eventClear(&g_vb_ev);
            atomic_fetch_add(&g_vblank, 1u);
            pthread_mutex_lock(&g_vb_mu);
            pthread_cond_broadcast(&g_vb_cv);
            pthread_mutex_unlock(&g_vb_mu);
        }
    return NULL;
}

/* Until vblank `target` has begun. The display keeps its vsync while the
 * application is in the background, but the wait still gives up after
 * 100 ms without one rather than hold the game. */
static void vb_wait(unsigned target) {
    unsigned v = atomic_load(&g_vblank);
    if ((int)(target - v) <= 0) return;
    uint64_t give_up = isaac_native_now_ns() + 100000000ull;
    pthread_mutex_lock(&g_vb_mu);
    while ((int)(target - (v = atomic_load(&g_vblank))) > 0 && isaac_native_now_ns() < give_up) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ++ts.tv_sec; }
        pthread_cond_timedwait(&g_vb_cv, &g_vb_mu, &ts);
    }
    pthread_mutex_unlock(&g_vb_mu);
}

/* libnx's default window (default_window.o), restated so the display it
 * opens can give the vsync event: a second open of "Default" fails (0x1272).
 * Same steps, mode and abort as the library's; defining all three symbols
 * keeps the linker from pulling the original. */
static ViDisplay g_win_disp;
static ViLayer g_win_layer;
static NWindow g_win;
NWindow *nwindowGetDefault(void) { return &g_win; }
void __nx_win_init(void) {
    if (R_SUCCEEDED(viInitialize(ViServiceType_Default))) {
        if (R_SUCCEEDED(viOpenDefaultDisplay(&g_win_disp))) {
            if (R_SUCCEEDED(viCreateLayer(&g_win_disp, &g_win_layer))) {
                if (R_SUCCEEDED(viSetLayerScalingMode(&g_win_layer, ViScalingMode_FitToLayer)) &&
                    R_SUCCEEDED(nwindowCreateFromLayer(&g_win, &g_win_layer))) {
                    nwindowSetDimensions(&g_win, 1280, 720);
                    return;
                }
                viCloseLayer(&g_win_layer);
            }
            viCloseDisplay(&g_win_disp);
        }
        viExit();
    }
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
}
void __nx_win_exit(void) {
    nwindowClose(&g_win);
    viCloseLayer(&g_win_layer);
    viCloseDisplay(&g_win_disp);
    viExit();
}

/* After the context exists; any failure leaves the record off. */
static void vb_start(void) {
    Result rc = viGetDisplayVsyncEvent(&g_win_disp, &g_vb_ev);
    if (R_FAILED(rc)) { fprintf(stderr, "[native][gl] vsync record off: viGetDisplayVsyncEvent 0x%x\n", rc); return; }
    if (pthread_create(&g_vb_thread, NULL, vb_main, NULL) != 0) { eventClose(&g_vb_ev); return; }
    g_vb_on = 1;
    fprintf(stderr, "[native][gl] vsync record on\n");
}

static void vb_print(unsigned i, int to_stage) {
    const vb_window *w = &g_vb_w[i];
    uint32_t nf = 0, nt = 0;
    for (int b = 0; b < VB_BINS; ++b) { nf += w->frame[b]; nt += w->tick[b]; }
    if (!nf) return;
    char line[448];
    snprintf(line, sizeof line, "vsync %us: frames %u (vblanks 0:%u 1:%u 2:%u 3:%u 4+:%u) "
             "ticks %u (0:%u 1:%u 2:%u 3:%u 4+:%u) shown ticks (1:%u 2:%u 3:%u 4+:%u) "
             "queued (0:%u 1:%u 2+:%u) skipped %u swap %.2f ms game wait %.2f render idle %.2f ms/frame "
             "need %.1f ms skipped %u/%u",
             i * VB_WINDOW / 60u, nf, w->frame[0], w->frame[1], w->frame[2], w->frame[3], w->frame[4],
             nt, w->tick[0], w->tick[1], w->tick[2], w->tick[3], w->tick[4],
             w->shown_tick[1], w->shown_tick[2], w->shown_tick[3], w->shown_tick[4],
             w->queued[0], w->queued[1], w->queued[2] + w->queued[3] + w->queued[4],
             w->skipped, w->swap_ns / 1e6 / nf, w->wait_ns / 1e6 / nf, w->idle_ns / 1e6 / nf,
             w->decides ? w->need_ms / w->decides : 0.0, w->skips, w->decides);
    if (to_stage) { if (isaac_native_stage) isaac_native_stage("%s", line); }
    else fprintf(stderr, "[native][gl] %s\n", line);
}

/* From the in-between decision, on the game thread: added to the window the
 * render thread is filling (a stat; the race is harmless). */
static void vb_decided(double need, int skips) {
    if (!g_vb_nw) return;
    vb_window *w = &g_vb_w[g_vb_nw - 1];
    w->need_ms += need;
    ++w->decides;
    w->skips += (unsigned)skips;
}

/* Right after a swap returns, on the thread that swapped. */
static void vb_note(uint32_t tick, uint64_t swap_ns) {
    if (!g_vb_on) return;
    unsigned v = atomic_load(&g_vblank);
    if (!g_vb_seen) {
        g_vb_seen = 1;
        g_vb_last = g_vb_tick_at = v;
        g_vb_shown = g_vb_shown_tick_at = v + 1;
        g_vb_tick = tick;
        return;
    }
    unsigned wi = v / VB_WINDOW;
    if (wi >= VB_WINDOWS) return;
    if (wi + 1 > g_vb_nw) {
        if (g_vb_nw) vb_print(g_vb_nw - 1, 0);
        for (unsigned i = g_vb_nw; i <= wi; ++i) {
            g_vb_w[i].skipped0 = isaac_render_gate_skipped();
            g_vb_w[i].wait0 = isaac_glq_game_wait_ns();
            g_vb_w[i].idle0 = isaac_glq_render_idle_ns();
        }
        g_vb_nw = wi + 1;
    }
    vb_window *w = &g_vb_w[wi];
    unsigned d = v - g_vb_last;
    ++w->frame[d < VB_BINS ? d : VB_BINS - 1];
    g_vb_last = v;
    w->swap_ns += swap_ns;
    w->skipped = isaac_render_gate_skipped() - w->skipped0;
    w->wait_ns = isaac_glq_game_wait_ns() - w->wait0;
    w->idle_ns = isaac_glq_render_idle_ns() - w->idle0;
    /* The swap interval 1 queue shows each frame at the first vblank after it
     * was queued that the frame before it has not taken: the vblank on screen,
     * as opposed to the swap's return above. */
    unsigned shown = v + 1 > g_vb_shown + 1 ? v + 1 : g_vb_shown + 1;
    d = shown - (v + 1);
    ++w->queued[d < VB_BINS ? d : VB_BINS - 1];
    g_vb_shown = shown;
    if (tick != g_vb_tick) {     /* the first frame of a new tick */
        d = v - g_vb_tick_at;
        ++w->tick[d < VB_BINS ? d : VB_BINS - 1];
        if (d >= 3) atomic_fetch_add(&g_tick_late, 1u);
        g_vb_tick_at = v;
        d = shown - g_vb_shown_tick_at;
        ++w->shown_tick[d < VB_BINS ? d : VB_BINS - 1];
        g_vb_shown_tick_at = shown;
        g_vb_tick = tick;
    }
}

static void vb_stop(void) {
    if (!g_vb_on) return;
    atomic_store(&g_vb_quit, 1);
    pthread_mutex_lock(&g_vb_mu);
    pthread_cond_broadcast(&g_vb_cv);
    pthread_mutex_unlock(&g_vb_mu);
    pthread_join(g_vb_thread, NULL);
    eventClose(&g_vb_ev);
    g_vb_on = 0;
    for (unsigned i = 0; i < g_vb_nw; ++i) vb_print(i, 1);
}
#else
static inline void vb_note(uint32_t tick, uint64_t swap_ns) { (void)tick; (void)swap_ns; }
static inline void vb_decided(double need, int skips) { (void)need; (void)skips; }
static inline void vb_stop(void) {}
#endif

/* ---- one display slot per pass of the game's loop (Switch, vsync) ---------
 * The game ticks on every other pass of its main loop, not on a clock. So
 * when every pass takes exactly one vblank, every tick takes exactly two,
 * whatever the passes cost, as long as two of them fit in two vblanks. Each
 * pass therefore gets a slot: the vblank its frame shows on, one after the
 * previous pass's. A drawn frame is swapped right after the vblank before its
 * slot; a pass whose in-between render the gate skipped takes its slot
 * without a swap, and the frame before stays on screen. A pass that comes
 * late presents at once, and the slots continue from there. With the render
 * thread the slot waits there, and the game thread, which never gets more
 * than one pass ahead, waits for the slot before last. This replaces the
 * render gate's sleeps on the wall clock, which did not know where the
 * vblank was: one tick in five took one or three vblanks (horde:3b at
 * 1020 MHz). ISAAC_NATIVE_VBLANK_PACE=0 turns it off. */
static int g_slot_paced;
#ifdef __SWITCH__
static unsigned g_slot_last;     /* the vblank the last slot's frame shows on */
static int g_slot_started;
static atomic_uint g_slot_late;  /* slots that came after their vblank */
#endif
typedef struct { uint32_t tick; int draw; } slot_args;

static void present(const slot_args *a) {
#ifdef __SWITCH__
    if (g_slot_paced) {
        unsigned v = atomic_load(&g_vblank), slot = g_slot_last + 1;
        if (!g_slot_started || (int)(v + 1 - slot) > 0) {          /* first, or late: the next vblank */
            if (g_slot_started) atomic_fetch_add(&g_slot_late, 1u);
            slot = v + 1;
        } else vb_wait(slot - 1);
        g_slot_started = 1;
        g_slot_last = slot;
        if (!a->draw) return;
    }
#endif
    uint64_t t0 = isaac_native_now_ns();
    eglSwapBuffers(g_dpy, g_surf);
    vb_note(a->tick, isaac_native_now_ns() - t0);
}

static void pace(void);
/* the pbuffer's pacing (ISAAC_NATIVE_FPS) stands for vsync: it waits where
 * the swap does, on the render thread */
static void swap_cmd(const void *p) { pace(); present(p); }

/* ---- the in-between frame, decided on the display's terms ------------------
 * With the passes on slots, a tick holds its two vblanks while the tick's
 * pass (T: its update and render, without the waits for the display) and the
 * in-between render (r) fit in them. The gate (patch 0003) asks here on the
 * pass after each tick. The display stays at 60: there is no held 30 fps
 * mode (the user's choice). Only a tick whose estimate does not fit -- within
 * 0.5 ms of the budget, or 6 ms when the last tick already took three
 * vblanks -- skips its in-between frame, so that one frame is shown for two
 * vblanks and the game keeps its speed; the next tick draws it again. */
#define PACE_BUDGET_MS (1000.0 / 30.0)
static uint64_t g_pass_end, g_render_begin;
static double g_pace_tick_ms, g_pace_render_ms;
static int g_pass_decides, g_pace_skipped;
static unsigned g_pace_late_seen;
static uint32_t g_pace_switches;

/* ISAAC_NATIVE_PACE_MARGIN=<ms>: how close to the budget an estimate counts
 * as not fitting. */
static double g_pace_miss_ms = 0.5;
static int g_pace_margin_read;
static void pace_margins(void) {
    g_pace_margin_read = 1;
    const char *e = getenv("ISAAC_NATIVE_PACE_MARGIN");
    if (e) g_pace_miss_ms = atof(e);
}

static double ema(double old, double x) { return old > 0.0 ? old * 0.8 + x * 0.2 : x; }

/* every pass, when its render begins */
void isaac_native_pace_render(void) { g_render_begin = isaac_native_now_ns(); }

/* the pass after a tick: 1 to skip its render */
int isaac_native_pace_decide(int rule_skip) {
    (void)rule_skip;
    g_pass_decides = 1;
    unsigned late = 0;
#ifdef __SWITCH__
    late = atomic_load(&g_tick_late);
#endif
    /* A late tick counts only near the budget: a stall (a load, a sweep of
     * the heap) is not something skipping the in-between frame would fix. */
    if (!g_pace_margin_read) pace_margins();
    double need = g_pace_tick_ms + g_pace_render_ms;
    int skip = need + g_pace_miss_ms >= PACE_BUDGET_MS || (late != g_pace_late_seen && need + 6.0 >= PACE_BUDGET_MS);
    g_pace_late_seen = late;
    if (skip != g_pace_skipped) { g_pace_skipped = skip; ++g_pace_switches; }
    vb_decided(need, skip);
    return skip;
}
uint32_t isaac_native_pace_switches(void) { return g_pace_switches; }

/* One slot per pass. With the render thread it is queued behind the frame's
 * draws, once the previous one has run: the game builds pass n+1 while pass n
 * waits for its vblank there, and never gets further ahead. */
static void take_slot(int draw) {
    uint64_t t0 = isaac_native_now_ns();
    if (g_slot_paced && g_pass_end) {
        if (draw && g_render_begin > g_pass_end) g_pace_render_ms = ema(g_pace_render_ms, (t0 - g_render_begin) / 1e6);
        if (!g_pass_decides) g_pace_tick_ms = ema(g_pace_tick_ms, (t0 - g_pass_end) / 1e6);
    }
    g_pass_decides = 0;
    slot_args a = { isaac_native_game_tick(), draw };
#ifndef __SWITCH__
    if (g_window) isaac_native_window_swap();
    else
#endif
    if (isaac_glq_active()) {
        isaac_glq_wait(g_swap_mark);
        isaac_glq_post(swap_cmd, &a, sizeof a);
        g_swap_mark = isaac_glq_mark();
    } else
    present(&a);
    g_pass_end = isaac_native_now_ns();
    if (draw) isaac_native_bench_frame(g_frames_seen, g_pass_end - t0);
}
static void swap(void) { take_slot(1); }

/* For the gate: 1 when the display paces the passes. */
int isaac_native_slot_paced(void) { return g_slot_paced; }
/* A pass whose in-between render was skipped: its slot, without a frame.
 * 0 when the display does not pace the passes (the gate then sleeps). */
int isaac_native_skip_slot(void) {
    if (!g_slot_paced || g_state <= 0) return 0;
    take_slot(0);
    return 1;
}

static EGLDisplay open_display(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#ifdef __SWITCH__
    if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) return d;
    return EGL_NO_DISPLAY;
#endif
    if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) return d;
    /* No window system: the first EGL device, when the driver exposes one. */
    PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDeviceEXT dev[4];
    EGLint n = 0;
    if (query && get && query(4, dev, &n) && n > 0) {
        d = get(EGL_PLATFORM_DEVICE_EXT, dev[0], NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) return d;
    }
    return EGL_NO_DISPLAY;
}

static int open_context(void);
static void open_context_cmd(void *ok) { *(int *)ok = open_context(); }

int isaac_native_gl_ready(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
#ifndef __SWITCH__
    const char *win = getenv("ISAAC_NATIVE_WINDOW");
    if (win && *win == '1') {
        if (!isaac_native_window_open(SURFACE_W, SURFACE_H)) {
            fprintf(stderr, "[native][gl] GLFW window failed\n");
            return 0;
        }
        g_window = g_state = 1;
        fprintf(stderr, "[native][gl] %s | %s | %s, window %dx%d\n",
                (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION), SURFACE_W, SURFACE_H);
        return 1;
    }
#endif
    /* The context is made, and stays current, on the thread that will draw. */
    int ok = 0;
    if (isaac_glq_start()) {
        isaac_glq_run(open_context_cmd, &ok);
        if (!ok) isaac_glq_stop();
    } else {
        ok = open_context();
    }
    if (ok) g_state = 1;
    return ok;
}

static int open_context(void) {
    g_dpy = open_display();
    if (g_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "[native][gl] no EGL display\n");
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return 0;
#ifdef __SWITCH__
    const EGLint surface_bit = EGL_WINDOW_BIT;
#else
    const EGLint surface_bit = EGL_PBUFFER_BIT;
#endif
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, surface_bit, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "[native][gl] no GLES3 pbuffer config\n");
        return 0;
    }
#ifdef __SWITCH__
    /* The game draws a 960x540 window; the compositor scales the layer. */
    NWindow *win = nwindowGetDefault();
    nwindowSetDimensions(win, SURFACE_W, SURFACE_H);
    g_surf = eglCreateWindowSurface(g_dpy, cfg, win, NULL);
#else
    const EGLint surf_attr[] = { EGL_WIDTH, SURFACE_W, EGL_HEIGHT, SURFACE_H, EGL_NONE };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, surf_attr);
#endif
    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_surf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        fprintf(stderr, "[native][gl] EGL surface/context failed (0x%x)\n", eglGetError());
        return 0;
    }
#ifdef __SWITCH__
    /* vsync paces the frame on the console; a benchmark measures the frame
     * itself unless ISAAC_NATIVE_VSYNC=1 asks for the pacing players get. */
    const char *vs = getenv("ISAAC_NATIVE_VSYNC");
    int interval = isaac_native_bench_on() && !(vs && *vs == '1') ? 0 : 1;
    eglSwapInterval(g_dpy, interval);
    vb_start();
    const char *vp = getenv("ISAAC_NATIVE_VBLANK_PACE");
    g_slot_paced = interval == 1 && g_vb_on && !(vp && *vp == '0');
    fprintf(stderr, "[native][gl] passes paced by %s\n", g_slot_paced ? "the vblank" : "the render gate's clock");
#endif
    fprintf(stderr, "[native][gl] %s | %s | %s, surface %dx%d\n",
            (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), SURFACE_W, SURFACE_H);
    return 1;
}

void isaac_native_drawable_size(int *w, int *h) {
    *w = SURFACE_W;
    *h = SURFACE_H;
}

/* ---- screenshots ---------------------------------------------------------- */

static unsigned *g_shots;
static size_t g_nshots;
static int g_shots_parsed;
static unsigned g_shot_frame;

static void parse_shots(void) {
    g_shots_parsed = 1;
    const char *s = getenv("ISAAC_NATIVE_SHOTS");
    if (!s || !*s) return;
    for (const char *p = s; *p;) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) break;
        g_shots = realloc(g_shots, (g_nshots + 1) * sizeof *g_shots);
        g_shots[g_nshots++] = (unsigned)v;
        p = *end ? end + 1 : end;
    }
}

/* A pbuffer has no vsync; pace on the monotonic clock when asked to, so the
 * wall-clock audio model and real playback agree. */
static void pace(void) {
    static double period = -1.0, next;
    if (period < 0.0) {
        const char *e = getenv("ISAAC_NATIVE_FPS");
        double fps = e ? atof(e) : (g_window ? 60.0 : 0.0);   /* a window plays at 60 */
        period = fps > 0.0 ? 1e9 / fps : 0.0;
    }
    if (period <= 0.0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
    if (next == 0.0 || now - next > 4.0 * period) next = now;   /* start, or fell far behind */
    next += period;
    if (next > now) {
        double wait = next - now;
        struct timespec d = { (time_t)(wait / 1e9), (long)((long long)wait % 1000000000LL) };
        nanosleep(&d, NULL);
    }
}

/* Once per presented frame, before the swap: the platform layer services
 * its event loop here (the Switch applet and pad). */
void isaac_native_frame_hook(unsigned n) __attribute__((weak));

int isaac_native_gl_frame_count(void);
int isaac_native_gl_frame_count(void) { return (int)g_frames_seen; }
int isaac_native_gl_state(void) { return g_state; }

int isaac_native_wants_frame(unsigned n) {
    g_frames_seen = n;
    if (isaac_native_frame_hook) isaac_native_frame_hook(n);
    if (!isaac_glq_active()) pace();
    if (!g_shots_parsed) parse_shots();
    for (size_t i = 0; i < g_nshots; ++i)
        if (g_shots[i] == n) { g_shot_frame = n; return 1; }
    if (g_state > 0) swap();
    return 0;
}

void isaac_native_present(const uint8_t *px, int w, int h) {
    const char *dir = getenv("ISAAC_NATIVE_SHOT_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05u.ppm", dir ? dir : ".", g_shot_frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y)          /* GL rows are bottom-up */
            for (int x = 0; x < w; ++x)
                fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
        fprintf(stderr, "[native][gl] screenshot %s\n", path);
    }
    if (g_state > 0) swap();
}

/* Release the surface and context before the process returns to the loader. */
void isaac_native_stage(const char *fmt, ...) __attribute__((weak));
#define STAGE(msg) do { if (isaac_native_stage) isaac_native_stage("%s", msg); } while (0)
/* The context is current on the thread that created it -- the game thread,
 * which renders. That thread unbinds it before it ends; a context left bound
 * to a finished thread is what kept Mesa's eglTerminate (and the nvdrv
 * session behind it) from ever returning on the Switch. */
static void release_cmd(void *p) {
    (void)p;
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglReleaseThread();
}

/* With the render thread, that thread unbinds it and ends. */
void isaac_native_gl_release_thread(void) {
    if (g_state <= 0) return;
    if (isaac_glq_active()) {
        isaac_glq_run(release_cmd, NULL);
        isaac_glq_stop();
        return;
    }
    release_cmd(NULL);
}

void isaac_native_gl_shutdown(void) {
    if (g_state <= 0) return;
#ifndef __SWITCH__
    if (g_window) { isaac_native_window_close(); g_state = 0; return; }
#endif
    STAGE("gl: release current");
    if (isaac_glq_active()) isaac_native_gl_release_thread();
    vb_stop();
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    STAGE("gl: destroy context");
    eglDestroyContext(g_dpy, g_ctx);
    STAGE("gl: destroy surface");
    eglDestroySurface(g_dpy, g_surf);
    /* On the Switch this frees Mesa's GPU buffers, which live in the heap
     * hbloader passes on to the next NRO. It never returned while the game
     * thread still had the context bound (runs 1f848d09, d9fd869e). */
    STAGE("gl: terminate");
    eglTerminate(g_dpy);
    STAGE("gl: done");
    g_state = 0;
}
