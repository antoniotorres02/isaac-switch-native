/* Scripted input for headless native runs: the ISAAC_INPUT timeline of the
 * node driver (boot_integration.mjs), delivered through the host's poll hook
 * so the game sees ordinary Win32 key messages.
 *
 *   ISAAC_INPUT="300:enter,360:enter:4,420:d:90,520:right:30"
 *
 * Each item is frame:key[:hold] (held for `hold` presented frames, default
 * 2), frame:mouse:x:y or frame:click. Key codes are explore.mjs's KEYS table
 * (virtual key, set-1 scancode, extended bit). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "isaac_host.h"

typedef int (*isaac_input_poll_fn)(int frame, int32_t *out);
void isaac_input_set_poll_hook(isaac_input_poll_fn fn);

static const struct { const char *name; uint8_t vk, sc, ext; } KEYS[] = {
    {"enter", 0x0D, 0x1C, 0}, {"escape", 0x1B, 0x01, 0}, {"space", 0x20, 0x39, 0},
    {"tab", 0x09, 0x0F, 0}, {"backspace", 0x08, 0x0E, 0}, {"delete", 0x2E, 0x53, 1},
    {"insert", 0x2D, 0x52, 1}, {"home", 0x24, 0x47, 1}, {"end", 0x23, 0x4F, 1},
    {"pageup", 0x21, 0x49, 1}, {"pagedown", 0x22, 0x51, 1},
    {"up", 0x26, 0x48, 1}, {"down", 0x28, 0x50, 1}, {"left", 0x25, 0x4B, 1}, {"right", 0x27, 0x4D, 1},
    {"shift", 0x10, 0x2A, 0}, {"ctrl", 0x11, 0x1D, 0}, {"alt", 0x12, 0x38, 0},
    {"a", 0x41, 0x1E, 0}, {"b", 0x42, 0x30, 0}, {"c", 0x43, 0x2E, 0}, {"d", 0x44, 0x20, 0},
    {"e", 0x45, 0x12, 0}, {"f", 0x46, 0x21, 0}, {"g", 0x47, 0x22, 0}, {"h", 0x48, 0x23, 0},
    {"i", 0x49, 0x17, 0}, {"j", 0x4A, 0x24, 0}, {"k", 0x4B, 0x25, 0}, {"l", 0x4C, 0x26, 0},
    {"m", 0x4D, 0x32, 0}, {"n", 0x4E, 0x31, 0}, {"o", 0x4F, 0x18, 0}, {"p", 0x50, 0x19, 0},
    {"q", 0x51, 0x10, 0}, {"r", 0x52, 0x13, 0}, {"s", 0x53, 0x1F, 0}, {"t", 0x54, 0x14, 0},
    {"u", 0x55, 0x16, 0}, {"v", 0x56, 0x2F, 0}, {"w", 0x57, 0x11, 0}, {"x", 0x58, 0x2D, 0},
    {"y", 0x59, 0x15, 0}, {"z", 0x5A, 0x2C, 0},
    {"0", 0x30, 0x0B, 0}, {"1", 0x31, 0x02, 0}, {"2", 0x32, 0x03, 0}, {"3", 0x33, 0x04, 0},
    {"4", 0x34, 0x05, 0}, {"5", 0x35, 0x06, 0}, {"6", 0x36, 0x07, 0}, {"7", 0x37, 0x08, 0},
    {"8", 0x38, 0x09, 0}, {"9", 0x39, 0x0A, 0},
    {"grave", 0xC0, 0x29, 0}, {"minus", 0xBD, 0x0C, 0}, {"equals", 0xBB, 0x0D, 0},
    {"f1", 0x70, 0x3B, 0}, {"f2", 0x71, 0x3C, 0}, {"f3", 0x72, 0x3D, 0}, {"f4", 0x73, 0x3E, 0},
};

typedef struct { int frame; int32_t ev[4]; } timed_event;

static timed_event *g_events;
static size_t g_count, g_cap, g_next;
static unsigned g_delivered;

static void push(int frame, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (g_count == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_events = realloc(g_events, g_cap * sizeof *g_events);
    }
    g_events[g_count++] = (timed_event){frame, {a, b, c, d}};
}

static int by_frame(const void *x, const void *y) {
    const timed_event *a = x, *b = y;
    return (a->frame > b->frame) - (a->frame < b->frame);
}

/* The Game object (pointer at 0x00c71678) counts simulation ticks at
 * +0x264f8 -- the counter the web render gate paces on. Sampling it shows
 * whether presented frames are game updates or a paused/idle loop. */
extern uint8_t *recomp_mem_base;
static int g_last_sample = -1;
static int g_tick_every = 300;              /* ISAAC_NATIVE_TICK_EVERY */
static void sample_tick(int frame) {
    if (frame % g_tick_every != 0 || frame == g_last_sample) return;
    g_last_sample = frame;
    uint32_t game;
    memcpy(&game, recomp_mem_base + 0x00c71678u, 4);
    uint32_t tick = 0;
    if (game && game < ISAAC_GUEST_LIMIT_VA - 0x26500u) memcpy(&tick, recomp_mem_base + game + 0x264f8u, 4);
    fprintf(stderr, "[native] frame %d: game 0x%08x tick %u\n", frame, game, tick);
}

static uint32_t g_tick_max;
uint32_t isaac_native_game_tick(void);
/* The highest tick seen while the game ran: the Game object is gone after
 * main() returns, so a read then says 0. */
uint32_t isaac_native_game_tick_max(void) { return g_tick_max; }

/* Keys a platform layer presses itself (the Switch pad's hold-+ restart),
 * delivered ahead of the scripted timeline. Same thread as the poll. */
static int32_t g_live[16][4];
static unsigned g_live_head, g_live_tail;
int isaac_native_input_key(const char *name, int down) {
    size_t k = 0;
    while (k < sizeof KEYS / sizeof KEYS[0] && strcasecmp(KEYS[k].name, name) != 0) ++k;
    if (k == sizeof KEYS / sizeof KEYS[0] || g_live_head - g_live_tail >= 16) return 0;
    int32_t *ev = g_live[g_live_head++ % 16];
    ev[0] = 1;
    ev[1] = KEYS[k].vk;
    ev[2] = KEYS[k].sc | (KEYS[k].ext << 8);
    ev[3] = down;
    return 1;
}

static int poll(int frame, int32_t *out) {
    sample_tick(frame);
    uint32_t t = isaac_native_game_tick();
    if (t > g_tick_max) g_tick_max = t;
    if (g_live_tail != g_live_head) {
        memcpy(out, g_live[g_live_tail++ % 16], sizeof g_live[0]);
        return 1;
    }
    if (g_next >= g_count || g_events[g_next].frame > frame) return 0;
    memcpy(out, g_events[g_next].ev, sizeof g_events[g_next].ev);
    ++g_next;
    ++g_delivered;
    return 1;
}

/* A platform quit (Switch HOME -> close) leaves through the same WM_QUIT the
 * frame budget posts, so main() returns and the game shuts down normally. */
static volatile int g_quit;
void isaac_native_request_quit(void) { g_quit = 1; }
int isaac_native_quit_requested(void) { return g_quit; }

/* The Game object's tick counter, 0 before a run starts. */
uint32_t isaac_native_game_tick(void) {
    uint32_t game, tick = 0;
    memcpy(&game, recomp_mem_base + 0x00c71678u, 4);
    if (game && game < ISAAC_GUEST_LIMIT_VA - 0x26500u) memcpy(&tick, recomp_mem_base + game + 0x264f8u, 4);
    return tick;
}

unsigned isaac_native_input_delivered(void) { return g_delivered; }
size_t isaac_native_input_pending(void) { return g_count - g_next; }

int isaac_native_input_init(void) {
    const char *spec = getenv("ISAAC_INPUT"), *every = getenv("ISAAC_NATIVE_TICK_EVERY");
    if (every && atoi(every) > 0) g_tick_every = atoi(every);
    isaac_input_set_poll_hook(poll);
    if (!spec || !*spec) return 0;
    char *copy = strdup(spec), *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char *f[4] = {0}, *s2 = NULL;
        int n = 0;
        for (char *p = strtok_r(item, ":", &s2); p && n < 4; p = strtok_r(NULL, ":", &s2)) f[n++] = p;
        if (n < 2) continue;
        int frame = atoi(f[0]);
        if (strcasecmp(f[1], "mouse") == 0) {
            push(frame, 2, f[2] ? atoi(f[2]) : 0, f[3] ? atoi(f[3]) : 0, 0);
        } else if (strcasecmp(f[1], "click") == 0 || strcasecmp(f[1], "rclick") == 0) {
            int btn = strcasecmp(f[1], "rclick") == 0;
            push(frame, 3, btn, 1, 0);
            push(frame + 2, 3, btn, 0, 0);
        } else {
            size_t k = 0;
            while (k < sizeof KEYS / sizeof KEYS[0] && strcasecmp(KEYS[k].name, f[1]) != 0) ++k;
            if (k == sizeof KEYS / sizeof KEYS[0]) {
                fprintf(stderr, "[native] ISAAC_INPUT: unknown key '%s'\n", f[1]);
                continue;
            }
            int hold = f[2] ? atoi(f[2]) : 2;
            if (hold < 1) hold = 1;
            int32_t sc = KEYS[k].sc | (KEYS[k].ext << 8);
            push(frame, 1, KEYS[k].vk, sc, 1);
            push(frame + hold, 1, KEYS[k].vk, sc, 0);
        }
    }
    free(copy);
    qsort(g_events, g_count, sizeof *g_events, by_frame);
    isaac_input_set_poll_hook(poll);
    return (int)g_count;
}
