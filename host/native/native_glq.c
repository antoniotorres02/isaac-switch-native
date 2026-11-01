/* The render thread: GL runs on its own core, behind a command queue.
 *
 * The game thread is CPU-bound and does everything on one core; the A57
 * profile of a heavy room charges about a fifth of it to Mesa, nouveau and
 * the swap's IPC. Here the EGL context is current on a second thread instead,
 * and every GLES call the GL backend makes (native_glq.h renames them) is
 * recorded into a ring that thread replays:
 *
 *   no result       queued with a copy of whatever it reads (uniform values,
 *                   buffer and texture bytes), so the caller's memory may
 *                   change as soon as it returns
 *   a result        (glGet*, glGen*, glCreate*, a map, a readback) queued, and
 *                   the caller waits until the render thread has run it
 *   the swap        queued like a draw; the next swap waits for it, so the
 *                   game runs at most one frame ahead of the GPU feed
 *
 * Mesa's own glthread does the same, but its worker never started on libnx.
 * A call made on the render thread itself (a closure posted with
 * isaac_glq_post that uses GL) goes straight to GL.
 *
 * ISAAC_NATIVE_GLQ=0 keeps GL on the game thread (the A/B); on the Switch
 * ISAAC_NATIVE_GLQ_CORE=<n> picks the render thread's core (default 1). */
#define NATIVE_GLQ_IMPL
#include "native_glq.h"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t isaac_native_now_ns(void);

#define RING_BYTES (32u << 20)
#define BIG_BYTES  (RING_BYTES / 4u)      /* larger data is not copied: the call waits instead */
#define SPIN_NS    200000ull              /* how long a waiting side spins before it sleeps */
#define PUBLISH_BYTES 4096u

typedef void (*cmd_fn)(const void *args);
typedef struct { cmd_fn fn; uint32_t size, pad; } cmd_hdr;   /* fn NULL: skip to the ring's start */

/* The two sides write different words, so each one has a cache line of its
 * own (A57 and desktop lines are 64 bytes): the game thread's write position
 * and the published head it hands over, what the render thread consumed, and
 * the flag the render thread raises when it sleeps. Before, the render
 * thread's store of the tail after every command and the game thread's loads
 * of it shared lines with the game thread's own stores. */
#define LINE 64
static uint8_t *g_ring;
static struct {
    _Alignas(LINE) uint64_t head_local;             /* the game thread's write position */
    uint64_t published;                             /* ... and what it published */
    uint64_t tail_seen;                             /* the render thread's tail as last read */
    uint64_t fast_end;                              /* while head_local + need <= this, a command needs no check */
    uint64_t cmds, waits, sleeps, spins;
    _Atomic uint64_t wait_ns;                       /* time the game thread spent waiting */
} G;
#define g_head_local G.head_local
#define g_published  G.published
/* published and the sleep target: the game thread writes them, the render thread reads */
static struct { _Alignas(LINE) _Atomic uint64_t head, want; char pad[LINE - 16]; } H;
/* consumed: the render thread writes it after every command */
static struct { _Alignas(LINE) _Atomic uint64_t tail; char pad[LINE - 8]; } T;
/* raised by the render thread when it sleeps; the game thread reads it at every publish */
static struct { _Alignas(LINE) _Atomic int sleeps; char pad[LINE - 4]; } RS;
/* the render thread's time with nothing to run; it alone writes it */
static struct { _Alignas(LINE) _Atomic uint64_t idle_ns; char pad[LINE - 8]; } RI;
#define g_head H.head
#define g_want H.want        /* the tail the game thread sleeps for (0: none) */
#define g_tail T.tail
#define g_render_sleeps RS.sleeps
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_game_cv = PTHREAD_COND_INITIALIZER, g_render_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_on, g_quit;
static _Thread_local int t_render;
#define g_cmds G.cmds
#define g_waits G.waits
#define g_sleeps G.sleeps
#define g_spins G.spins

static inline int direct(void) { return !g_on || t_render; }
static void publish(void);

static inline void relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

/* ---- the game thread's side ------------------------------------------------ */

/* until the render thread has consumed everything before `pos` */
void isaac_glq_wait(uint64_t pos) {
    if (atomic_load_explicit(&g_tail, memory_order_acquire) >= pos) return;
    if (g_published < pos) publish();
    ++g_waits;
    uint64_t t0 = isaac_native_now_ns();
    for (unsigned i = 0;; ++i) {
        if (atomic_load_explicit(&g_tail, memory_order_acquire) >= pos) {
            atomic_fetch_add_explicit(&G.wait_ns, isaac_native_now_ns() - t0, memory_order_relaxed);
            return;
        }
        relax();
        ++g_spins;
        if ((i & 63u) == 63u && isaac_native_now_ns() - t0 > SPIN_NS) break;
    }
    ++g_sleeps;
    pthread_mutex_lock(&g_mu);
    atomic_store(&g_want, pos);
    while (atomic_load(&g_tail) < pos) pthread_cond_wait(&g_game_cv, &g_mu);
    atomic_store(&g_want, 0);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add_explicit(&G.wait_ns, isaac_native_now_ns() - t0, memory_order_relaxed);
}

/* The game thread keeps the tail it last read and writes ahead of it without
 * looking at the render thread again: the tail only grows, so the copy is
 * conservative. g_fast_end is the first position a command may not reach
 * without a look: the end of the ring's lap, or the tail's lap-ahead. */
static void set_fast_end(void) {
    uint64_t lap_end = g_head_local - (g_head_local % RING_BYTES) + RING_BYTES;
    uint64_t room_end = G.tail_seen + RING_BYTES;
    G.fast_end = lap_end < room_end ? lap_end : room_end;
}

/* room for `need` more bytes after the write position */
static void wait_room(uint32_t need) {
    G.tail_seen = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (g_head_local + need - G.tail_seen > RING_BYTES) {
        isaac_glq_wait(g_head_local + need - RING_BYTES);
        G.tail_seen = atomic_load_explicit(&g_tail, memory_order_acquire);
    }
}

/* The slow side of cmd_alloc: the command would cross the lap's end or the
 * room the last look found. */
static void __attribute__((noinline)) alloc_slow(uint32_t need) {
    uint32_t at = (uint32_t)(g_head_local % RING_BYTES);
    if (at + need > RING_BYTES) {
        uint32_t rest = RING_BYTES - at;
        wait_room(rest);
        cmd_hdr *w = (cmd_hdr *)(g_ring + at);
        w->fn = NULL;
        w->size = rest;
        g_head_local += rest;
        publish();
    }
    wait_room(need);
    set_fast_end();
}

/* A command of `bytes` argument bytes; its arguments follow the header. */
static inline void *cmd_alloc(cmd_fn fn, size_t bytes) {
    uint32_t need = (uint32_t)((sizeof(cmd_hdr) + bytes + 15u) & ~(size_t)15u);
    if (__builtin_expect(g_head_local + need > G.fast_end, 0)) alloc_slow(need);
    cmd_hdr *h = (cmd_hdr *)(g_ring + (uint32_t)(g_head_local % RING_BYTES));
    h->fn = fn;
    h->size = need;
    g_head_local += need;
    ++g_cmds;
    return h + 1;
}

/* What the game thread wrote becomes visible to the render thread. */
static void publish(void) {
    g_published = g_head_local;
    atomic_store(&g_head, g_head_local);
    if (atomic_load(&g_render_sleeps)) {
        pthread_mutex_lock(&g_mu);
        pthread_cond_signal(&g_render_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

/* A command is published with the ones after it, a few KB at a time (the
 * store and the check that wakes the render thread cost the game thread 2%
 * of a frame one command at a time); a wait, a post and the swap publish
 * at once. */
static inline void cmd_commit(void) {
    if (g_head_local - g_published >= PUBLISH_BYTES) publish();
}

uint64_t isaac_glq_mark(void) { return g_head_local; }
int isaac_glq_active(void) { return g_on; }

typedef struct { void (*fn)(void *); void *ctx; } run_args;
static void run_cmd(const void *p) { const run_args *r = p; r->fn(r->ctx); }

void isaac_glq_run(void (*fn)(void *), void *ctx) {
    if (direct()) { fn(ctx); return; }
    run_args *r = cmd_alloc(run_cmd, sizeof *r);
    r->fn = fn;
    r->ctx = ctx;
    isaac_glq_wait(g_head_local);
}

/* A command whose arguments the caller writes itself, in place (the draw
 * packet of host_gl_clientarrays.c): begin gives room for `bytes`, end
 * queues it; with the queue off, or on the render thread, end runs fn at
 * once on the same bytes. */
static _Thread_local struct { void (*fn)(const void *); void *buf; size_t cap; } t_inline;
void *isaac_glq_begin(void (*fn)(const void *), size_t bytes) {
    if (direct()) {
        if (bytes > t_inline.cap) {
            free(t_inline.buf);
            t_inline.cap = bytes + bytes / 2u + 256u;
            t_inline.buf = malloc(t_inline.cap);
        }
        t_inline.fn = fn;
        return t_inline.buf;
    }
    return cmd_alloc(fn, bytes);
}
void isaac_glq_end(void) {
    if (direct()) t_inline.fn(t_inline.buf);
    else cmd_commit();
}

typedef struct { void (*fn)(const void *); } post_args;
static void post_cmd(const void *p) { const post_args *a = p; a->fn(a + 1); }

void isaac_glq_post(void (*fn)(const void *), const void *data, size_t bytes) {
    if (direct()) { fn(data); return; }
    post_args *a = cmd_alloc(post_cmd, sizeof *a + bytes);
    a->fn = fn;
    if (bytes) memcpy(a + 1, data, bytes);
    publish();
}

/* ---- the render thread ----------------------------------------------------- */

static void quit_cmd(const void *p) { (void)p; g_quit = 1; }

#ifdef __SWITCH__
void isaac_native_prof_render_thread(void);
#endif
static void *render_main(void *arg) {
    (void)arg;
    t_render = 1;
#ifdef __SWITCH__
    const char *e = getenv("ISAAC_NATIVE_GLQ_CORE");
    int core = e ? atoi(e) : 1;
    if (core >= 0 && core <= 2) svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    isaac_native_prof_render_thread();
#endif
    uint64_t t = 0;
    while (!g_quit) {
        uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire);
        if (h == t) {
            uint64_t t0 = isaac_native_now_ns();
            for (unsigned i = 0; h == t; ++i) {
                relax();
                h = atomic_load_explicit(&g_head, memory_order_acquire);
                if (h == t && (i & 63u) == 63u && isaac_native_now_ns() - t0 > SPIN_NS) {
                    pthread_mutex_lock(&g_mu);
                    atomic_store(&g_render_sleeps, 1);
                    while ((h = atomic_load(&g_head)) == t) pthread_cond_wait(&g_render_cv, &g_mu);
                    atomic_store(&g_render_sleeps, 0);
                    pthread_mutex_unlock(&g_mu);
                }
            }
            atomic_fetch_add_explicit(&RI.idle_ns, isaac_native_now_ns() - t0, memory_order_relaxed);
        }
        while (t != h && !g_quit) {
            const cmd_hdr *c = (const cmd_hdr *)(g_ring + t % RING_BYTES);
            if (c->fn) c->fn(c + 1);
            t += c->size;
            atomic_store(&g_tail, t);
            uint64_t w = atomic_load(&g_want);
            if (w && t >= w) {
                pthread_mutex_lock(&g_mu);
                pthread_cond_signal(&g_game_cv);
                pthread_mutex_unlock(&g_mu);
            }
        }
    }
    return NULL;
}

/* From native_gl.c, before the context exists: 1 when GL now goes through
 * the queue (the context is then made on the render thread). */
int isaac_glq_start(void) {
    const char *e = getenv("ISAAC_NATIVE_GLQ");
    if (e && *e == '0') return 0;
    g_ring = malloc(RING_BYTES);
    if (!g_ring) return 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);   /* Mesa's GLSL compiler recurses */
    if (pthread_create(&g_thread, &attr, render_main, NULL) != 0) {
        free(g_ring);
        g_ring = NULL;
        return 0;
    }
    g_on = 1;
    fprintf(stderr, "[native][glq] GL on the render thread\n");
    return 1;
}

/* For the pacing record: where the two threads' time went. */
uint64_t isaac_glq_game_wait_ns(void) { return atomic_load_explicit(&G.wait_ns, memory_order_relaxed); }
uint64_t isaac_glq_render_idle_ns(void) { return atomic_load_explicit(&RI.idle_ns, memory_order_relaxed); }

/* fewest functions named so far, with how often each one waited */
#define NOTE_MAX 64
static const char *g_note_name[NOTE_MAX];
static uint32_t g_note_n[NOTE_MAX];
static int g_notes;
static void note(int *slot, const char *name) {
    if (*slot < 0) {
        if (g_notes == NOTE_MAX) return;
        *slot = g_notes++;
        g_note_name[*slot] = name;
    }
    ++g_note_n[*slot];
}

/* After the last GL call: the thread runs what is queued and ends. */
void isaac_glq_stop(void) {
    if (!g_on) return;
    cmd_alloc(quit_cmd, 0);
    publish();
    pthread_join(g_thread, NULL);
    g_on = 0;
    char line[1024];
    int n = snprintf(line, sizeof line, "[native][glq] %llu commands, %llu waits (%llu slept, %llu spins); waited for:",
                     (unsigned long long)g_cmds, (unsigned long long)g_waits, (unsigned long long)g_sleeps, (unsigned long long)g_spins);
    for (int i = 0; i < g_notes && n > 0 && n < (int)sizeof line - 48; ++i)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s %u", g_note_name[i], g_note_n[i]);
    fprintf(stderr, "%s\n", line);
    free(g_ring);
    g_ring = NULL;
}

/* ---- the wrappers ---------------------------------------------------------- */

/* Calls without a result: the arguments travel by value. */
#define GLQ_A1(name, T1) \
    typedef struct { T1 a1; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1); } \
    void glq_##name(T1 a1) { \
        if (direct()) { name(a1); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); x->a1 = a1; cmd_commit(); }
#define GLQ_A2(name, T1, T2) \
    typedef struct { T1 a1; T2 a2; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1, x->a2); } \
    void glq_##name(T1 a1, T2 a2) { \
        if (direct()) { name(a1, a2); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); x->a1 = a1; x->a2 = a2; cmd_commit(); }
#define GLQ_A3(name, T1, T2, T3) \
    typedef struct { T1 a1; T2 a2; T3 a3; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1, x->a2, x->a3); } \
    void glq_##name(T1 a1, T2 a2, T3 a3) { \
        if (direct()) { name(a1, a2, a3); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); x->a1 = a1; x->a2 = a2; x->a3 = a3; cmd_commit(); }
#define GLQ_A4(name, T1, T2, T3, T4) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1, x->a2, x->a3, x->a4); } \
    void glq_##name(T1 a1, T2 a2, T3 a3, T4 a4) { \
        if (direct()) { name(a1, a2, a3, a4); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); \
        x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; cmd_commit(); }
#define GLQ_A5(name, T1, T2, T3, T4, T5) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1, x->a2, x->a3, x->a4, x->a5); } \
    void glq_##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5) { \
        if (direct()) { name(a1, a2, a3, a4, a5); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); \
        x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5; cmd_commit(); }
#define GLQ_A6(name, T1, T2, T3, T4, T5, T6) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->a1, x->a2, x->a3, x->a4, x->a5, x->a6); } \
    void glq_##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6) { \
        if (direct()) { name(a1, a2, a3, a4, a5, a6); return; } \
        name##_q *x = cmd_alloc(name##_r, sizeof *x); \
        x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5; x->a6 = a6; cmd_commit(); }

GLQ_A1(glActiveTexture, GLenum)
GLQ_A2(glAttachShader, GLuint, GLuint)
GLQ_A2(glBindBuffer, GLenum, GLuint)
GLQ_A2(glBindFramebuffer, GLenum, GLuint)
GLQ_A2(glBindRenderbuffer, GLenum, GLuint)
GLQ_A2(glBindTexture, GLenum, GLuint)
GLQ_A1(glBlendEquation, GLenum)
GLQ_A4(glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)
GLQ_A1(glClear, GLbitfield)
GLQ_A4(glClearColor, GLfloat, GLfloat, GLfloat, GLfloat)
GLQ_A1(glClearDepthf, GLfloat)
GLQ_A1(glCompileShader, GLuint)
GLQ_A1(glCullFace, GLenum)
GLQ_A1(glDeleteProgram, GLuint)
GLQ_A1(glDeleteShader, GLuint)
GLQ_A1(glDeleteSync, GLsync)
GLQ_A1(glDepthFunc, GLenum)
GLQ_A1(glDisable, GLenum)
GLQ_A1(glDisableVertexAttribArray, GLuint)
GLQ_A3(glDrawArrays, GLenum, GLint, GLsizei)
GLQ_A4(glDrawArraysInstanced, GLenum, GLint, GLsizei, GLsizei)
/* the backend always has an element buffer bound: `indices` is an offset */
GLQ_A4(glDrawElements, GLenum, GLsizei, GLenum, const void *)
GLQ_A1(glEnable, GLenum)
GLQ_A1(glEnableVertexAttribArray, GLuint)
GLQ_A4(glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)
GLQ_A5(glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLQ_A1(glLinkProgram, GLuint)
GLQ_A4(glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)
GLQ_A3(glTexParameteri, GLenum, GLenum, GLint)
GLQ_A2(glUniform1i, GLint, GLint)
GLQ_A1(glUseProgram, GLuint)
/* and an array buffer: `pointer` is an offset too */
GLQ_A6(glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)
GLQ_A4(glViewport, GLint, GLint, GLsizei, GLsizei)

/* Calls with a result, or that write into the caller's memory: the caller
 * waits, so its pointers stay valid while the render thread uses them. */
#define GLQ_SYNC(name, ...) \
    do { static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &(name##_s){ __VA_ARGS__ }); } while (0)
#define GLQ_S0(R, name) \
    typedef struct { R r; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; s->r = name(); } \
    R glq_##name(void) { \
        if (direct()) return name(); \
        name##_s s; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); return s.r; }
#define GLQ_S1(R, name, T1) \
    typedef struct { T1 a1; R r; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; s->r = name(s->a1); } \
    R glq_##name(T1 a1) { \
        if (direct()) return name(a1); \
        name##_s s = { a1 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); return s.r; }
#define GLQ_S2(R, name, T1, T2) \
    typedef struct { T1 a1; T2 a2; R r; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; s->r = name(s->a1, s->a2); } \
    R glq_##name(T1 a1, T2 a2) { \
        if (direct()) return name(a1, a2); \
        name##_s s = { a1, a2 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); return s.r; }
#define GLQ_S3(R, name, T1, T2, T3) \
    typedef struct { T1 a1; T2 a2; T3 a3; R r; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; s->r = name(s->a1, s->a2, s->a3); } \
    R glq_##name(T1 a1, T2 a2, T3 a3) { \
        if (direct()) return name(a1, a2, a3); \
        name##_s s = { a1, a2, a3 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); return s.r; }
#define GLQ_S4(R, name, T1, T2, T3, T4) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; R r; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; s->r = name(s->a1, s->a2, s->a3, s->a4); } \
    R glq_##name(T1 a1, T2 a2, T3 a3, T4 a4) { \
        if (direct()) return name(a1, a2, a3, a4); \
        name##_s s = { a1, a2, a3, a4 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); return s.r; }
#define GLQ_V2(name, T1, T2) \
    typedef struct { T1 a1; T2 a2; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; name(s->a1, s->a2); } \
    void glq_##name(T1 a1, T2 a2) { \
        if (direct()) { name(a1, a2); return; } \
        name##_s s = { a1, a2 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); }
#define GLQ_V3(name, T1, T2, T3) \
    typedef struct { T1 a1; T2 a2; T3 a3; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; name(s->a1, s->a2, s->a3); } \
    void glq_##name(T1 a1, T2 a2, T3 a3) { \
        if (direct()) { name(a1, a2, a3); return; } \
        name##_s s = { a1, a2, a3 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); }
#define GLQ_V4(name, T1, T2, T3, T4) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; name(s->a1, s->a2, s->a3, s->a4); } \
    void glq_##name(T1 a1, T2 a2, T3 a3, T4 a4) { \
        if (direct()) { name(a1, a2, a3, a4); return; } \
        name##_s s = { a1, a2, a3, a4 }; static int slot = -1; note(&slot, #name); isaac_glq_run(name##_x, &s); }
#define GLQ_V7(name, T1, T2, T3, T4, T5, T6, T7) \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; T7 a7; } name##_s; \
    static void name##_x(void *p) { name##_s *s = p; name(s->a1, s->a2, s->a3, s->a4, s->a5, s->a6, s->a7); } \
    void glq_##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7) { \
        if (direct()) { name(a1, a2, a3, a4, a5, a6, a7); return; } \
        name##_s s = { a1, a2, a3, a4, a5, a6, a7 }; static int slot = -1; note(&slot, #name); \
        isaac_glq_run(name##_x, &s); }

GLQ_S1(GLenum, glCheckFramebufferStatus, GLenum)
GLQ_S3(GLenum, glClientWaitSync, GLsync, GLbitfield, GLuint64)
GLQ_S0(GLuint, glCreateProgram)
GLQ_S1(GLuint, glCreateShader, GLenum)
GLQ_S2(GLsync, glFenceSync, GLenum, GLbitfield)
GLQ_V2(glGenBuffers, GLsizei, GLuint *)
GLQ_V2(glGenFramebuffers, GLsizei, GLuint *)
GLQ_V2(glGenRenderbuffers, GLsizei, GLuint *)
GLQ_V2(glGenTextures, GLsizei, GLuint *)
GLQ_S2(GLint, glGetAttribLocation, GLuint, const GLchar *)
GLQ_S0(GLenum, glGetError)
GLQ_V2(glGetIntegerv, GLenum, GLint *)
GLQ_V4(glGetProgramInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)
GLQ_V3(glGetProgramiv, GLuint, GLenum, GLint *)
GLQ_V3(glGetRenderbufferParameteriv, GLenum, GLenum, GLint *)
GLQ_V4(glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)
GLQ_V3(glGetShaderiv, GLuint, GLenum, GLint *)
GLQ_S1(const GLubyte *, glGetString, GLenum)
GLQ_S2(const GLubyte *, glGetStringi, GLenum, GLuint)
GLQ_S2(GLint, glGetUniformLocation, GLuint, const GLchar *)
GLQ_S4(void *, glMapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield)
GLQ_V7(glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)
GLQ_V4(glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *)
GLQ_S1(GLboolean, glUnmapBuffer, GLenum)

/* Calls that read the caller's memory: queued with a copy of it. Larger than
 * BIG_BYTES, the call waits for the render thread instead. */
typedef struct { GLenum target; GLsizeiptr size; GLenum usage; int has; } bufdata_q;
static void bufdata_r(const void *p) {
    const bufdata_q *x = p;
    glBufferData(x->target, x->size, x->has ? (const void *)(x + 1) : NULL, x->usage);
}
typedef struct { GLenum target; GLsizeiptr size; const void *data; GLenum usage; } bufdata_s;
static void bufdata_x(void *p) { bufdata_s *s = p; glBufferData(s->target, s->size, s->data, s->usage); }
void glq_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if (direct()) { glBufferData(target, size, data, usage); return; }
    if (data && (size_t)size > BIG_BYTES) { GLQ_SYNC(bufdata, target, size, data, usage); return; }
    size_t bytes = data ? (size_t)size : 0;
    bufdata_q *x = cmd_alloc(bufdata_r, sizeof *x + bytes);
    x->target = target; x->size = size; x->usage = usage; x->has = data != NULL;
    if (bytes) memcpy(x + 1, data, bytes);
    cmd_commit();
}

typedef struct { GLenum target; GLintptr offset; GLsizeiptr size; } bufsub_q;
static void bufsub_r(const void *p) { const bufsub_q *x = p; glBufferSubData(x->target, x->offset, x->size, x + 1); }
typedef struct { GLenum target; GLintptr offset; GLsizeiptr size; const void *data; } bufsub_s;
static void bufsub_x(void *p) { bufsub_s *s = p; glBufferSubData(s->target, s->offset, s->size, s->data); }
void glq_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    if (direct()) { glBufferSubData(target, offset, size, data); return; }
    if ((size_t)size > BIG_BYTES) { GLQ_SYNC(bufsub, target, offset, size, data); return; }
    bufsub_q *x = cmd_alloc(bufsub_r, sizeof *x + (size_t)size);
    x->target = target; x->offset = offset; x->size = size;
    memcpy(x + 1, data, (size_t)size);
    cmd_commit();
}

/* The bytes an upload reads with the default unpack alignment of 4 (the
 * backend never changes it); 0 for a layout this does not know. */
static size_t pixel_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type) {
    size_t bpp = 0;
    if (type == GL_UNSIGNED_BYTE)
        bpp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 :
              (format == GL_LUMINANCE || format == GL_ALPHA || format == GL_RED) ? 1 : 0;
    else if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        bpp = 2;
    if (!bpp || w <= 0 || h <= 0) return 0;
    size_t row = ((size_t)w * bpp + 3u) & ~(size_t)3u;
    return row * (size_t)(h - 1) + (size_t)w * bpp;
}

typedef struct { GLenum target; GLint level, ifmt; GLsizei w, h; GLint border; GLenum format, type; int has; } teximg_q;
static void teximg_r(const void *p) {
    const teximg_q *x = p;
    glTexImage2D(x->target, x->level, x->ifmt, x->w, x->h, x->border, x->format, x->type, x->has ? (const void *)(x + 1) : NULL);
}
typedef struct { GLenum target; GLint level, ifmt; GLsizei w, h; GLint border; GLenum format, type; const void *px; } teximg_s;
static void teximg_x(void *p) {
    teximg_s *s = p;
    glTexImage2D(s->target, s->level, s->ifmt, s->w, s->h, s->border, s->format, s->type, s->px);
}
void glq_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                      GLenum format, GLenum type, const void *px) {
    if (direct()) { glTexImage2D(target, level, ifmt, w, h, border, format, type, px); return; }
    size_t bytes = px ? pixel_bytes(w, h, format, type) : 0;
    if (px && (!bytes || bytes > BIG_BYTES)) { GLQ_SYNC(teximg, target, level, ifmt, w, h, border, format, type, px); return; }
    teximg_q *x = cmd_alloc(teximg_r, sizeof *x + bytes);
    *x = (teximg_q){ target, level, ifmt, w, h, border, format, type, px != NULL };
    if (bytes) memcpy(x + 1, px, bytes);
    cmd_commit();
}

typedef struct { GLenum target; GLint level, x, y; GLsizei w, h; GLenum format, type; } texsub_q;
static void texsub_r(const void *p) {
    const texsub_q *q = p;
    glTexSubImage2D(q->target, q->level, q->x, q->y, q->w, q->h, q->format, q->type, q + 1);
}
typedef struct { GLenum target; GLint level, x, y; GLsizei w, h; GLenum format, type; const void *px; } texsub_s;
static void texsub_x(void *p) {
    texsub_s *s = p;
    glTexSubImage2D(s->target, s->level, s->x, s->y, s->w, s->h, s->format, s->type, s->px);
}
void glq_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                         GLenum format, GLenum type, const void *px) {
    if (direct()) { glTexSubImage2D(target, level, x, y, w, h, format, type, px); return; }
    size_t bytes = px ? pixel_bytes(w, h, format, type) : 0;
    if (!bytes || bytes > BIG_BYTES) { GLQ_SYNC(texsub, target, level, x, y, w, h, format, type, px); return; }
    texsub_q *q = cmd_alloc(texsub_r, sizeof *q + bytes);
    *q = (texsub_q){ target, level, x, y, w, h, format, type };
    memcpy(q + 1, px, bytes);
    cmd_commit();
}

#define GLQ_DELETE(name) \
    typedef struct { GLsizei n; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->n, (const GLuint *)(x + 1)); } \
    void glq_##name(GLsizei n, const GLuint *names) { \
        if (direct()) { name(n, names); return; } \
        if (n <= 0 || !names) return; \
        name##_q *x = cmd_alloc(name##_r, sizeof *x + (size_t)n * sizeof *names); \
        x->n = n; memcpy(x + 1, names, (size_t)n * sizeof *names); cmd_commit(); }
GLQ_DELETE(glDeleteBuffers)
GLQ_DELETE(glDeleteFramebuffers)
GLQ_DELETE(glDeleteRenderbuffers)
GLQ_DELETE(glDeleteTextures)

/* glUniform*v: count values of n components */
#define GLQ_UNIFORM(name, T, n) \
    typedef struct { GLint loc; GLsizei count; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->loc, x->count, (const T *)(x + 1)); } \
    void glq_##name(GLint loc, GLsizei count, const T *v) { \
        if (direct()) { name(loc, count, v); return; } \
        if (count <= 0 || !v) return; \
        size_t bytes = (size_t)count * (n) * sizeof(T); \
        name##_q *x = cmd_alloc(name##_r, sizeof *x + bytes); \
        x->loc = loc; x->count = count; memcpy(x + 1, v, bytes); cmd_commit(); }
GLQ_UNIFORM(glUniform1fv, GLfloat, 1)
GLQ_UNIFORM(glUniform2fv, GLfloat, 2)
GLQ_UNIFORM(glUniform3fv, GLfloat, 3)
GLQ_UNIFORM(glUniform4fv, GLfloat, 4)
GLQ_UNIFORM(glUniform1iv, GLint, 1)
GLQ_UNIFORM(glUniform2iv, GLint, 2)
GLQ_UNIFORM(glUniform3iv, GLint, 3)
GLQ_UNIFORM(glUniform4iv, GLint, 4)
GLQ_UNIFORM(glUniform1uiv, GLuint, 1)
GLQ_UNIFORM(glUniform2uiv, GLuint, 2)
GLQ_UNIFORM(glUniform3uiv, GLuint, 3)
GLQ_UNIFORM(glUniform4uiv, GLuint, 4)

#define GLQ_MATRIX(name, n) \
    typedef struct { GLint loc; GLsizei count; GLboolean transpose; } name##_q; \
    static void name##_r(const void *p) { const name##_q *x = p; name(x->loc, x->count, x->transpose, (const GLfloat *)(x + 1)); } \
    void glq_##name(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) { \
        if (direct()) { name(loc, count, transpose, v); return; } \
        if (count <= 0 || !v) return; \
        size_t bytes = (size_t)count * (n) * sizeof(GLfloat); \
        name##_q *x = cmd_alloc(name##_r, sizeof *x + bytes); \
        x->loc = loc; x->count = count; x->transpose = transpose; memcpy(x + 1, v, bytes); cmd_commit(); }
GLQ_MATRIX(glUniformMatrix2fv, 4)
GLQ_MATRIX(glUniformMatrix3fv, 9)
GLQ_MATRIX(glUniformMatrix4fv, 16)
GLQ_MATRIX(glUniformMatrix2x3fv, 6)
GLQ_MATRIX(glUniformMatrix3x2fv, 6)
GLQ_MATRIX(glUniformMatrix2x4fv, 8)
GLQ_MATRIX(glUniformMatrix4x2fv, 8)
GLQ_MATRIX(glUniformMatrix3x4fv, 12)
GLQ_MATRIX(glUniformMatrix4x3fv, 12)
