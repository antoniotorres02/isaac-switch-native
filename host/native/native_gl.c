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

static void swap(void) {
#ifndef __SWITCH__
    if (g_window) { isaac_native_window_swap(); return; }
#endif
    eglSwapBuffers(g_dpy, g_surf);
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
    g_state = 1;
#ifdef __SWITCH__
    eglSwapInterval(g_dpy, 1);           /* vsync paces the frame on the console */
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
static unsigned g_frames_seen;
int isaac_native_gl_frame_count(void) { return (int)g_frames_seen; }
int isaac_native_gl_state(void) { return g_state; }

int isaac_native_wants_frame(unsigned n) {
    g_frames_seen = n;
    if (isaac_native_frame_hook) isaac_native_frame_hook(n);
    pace();
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
void isaac_native_gl_release_thread(void) {
    if (g_state <= 0) return;
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglReleaseThread();
}

void isaac_native_gl_shutdown(void) {
    if (g_state <= 0) return;
#ifndef __SWITCH__
    if (g_window) { isaac_native_window_close(); g_state = 0; return; }
#endif
    STAGE("gl: release current");
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
