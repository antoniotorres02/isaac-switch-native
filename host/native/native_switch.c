/* Nintendo Switch entry for the native host of the browsing-of-isaac lift.
 *
 * Layout on the console:
 *   romfs:/isaac.segs.bin                      memory image (embedded)
 *   romfs:/instance_index.json                 files the web build mounts
 *   sdmc:/switch/isaac-recomp/guest/           the PC install (resources/...)
 *   sdmc:/switch/isaac-recomp/saves/           saves (saves-autotest/ for tests)
 *   sdmc:/switch/isaac-recomp/guest/mods/      mods, mounted when playing (--mods in tests)
 *
 * Interactive: the game runs until it exits or HOME closes it; the pad is
 * reported to the game's own XInput path. Face buttons go by position in play
 * (the top one is Xbox Y, so shooting follows the layout) and by label in the
 * menus (A accepts, B goes back; settings.ini a_accepts=0 keeps them by
 * position); holding + in play restarts the run. settings.ini switch_glyphs
 * draws Switch buttons instead of Xbox ones (native_glyphs.c).
 *
 * --autotest --run-id <32 hex> [--return-to-sphaira] [--frames N]
 * [--no-script]: the tools/run_switch_tests.py protocol. stdout/stderr go to
 * nxlink; scripted keys start a run (--no-script leaves it to the pad);
 * begin/end SWITCH_TEST receipts report the result:
 *   synthetic  PASS when the game's tick counter shows a running game
 *   graphics   PASS when the EGL context drew every budgeted frame
 *   audio      PASS when an OpenAL device was opened */
#ifdef __SWITCH__
#include <switch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int isaac_native_main(int argc, char **argv);
uint32_t isaac_native_game_tick_max(void);
void isaac_native_request_quit(void);
int isaac_native_gl_state(void);
int isaac_native_gl_frame_count(void);
int isaac_native_audio_state(void);
void isaac_native_gl_shutdown(void);
void isaac_native_gl_release_thread(void);
void isaac_native_audio_shutdown(void);

#define ROOT "sdmc:/switch/isaac-recomp"

/* ---- pad -> XINPUT_STATE -------------------------------------------------- */

static PadState g_pad;
static int g_pad_ready;
static uint32_t g_packet;

void isaac_native_frame_hook(unsigned n) {
    (void)n;
    if (!appletMainLoop()) isaac_native_request_quit();
    if (g_pad_ready) padUpdate(&g_pad);
}

/* Menus or play: the game's tick only advances in an unpaused run (measured:
 * it holds still while paused and reads 0 in the menus). */
uint32_t isaac_native_game_tick(void);
int isaac_native_input_key(const char *name, int down);
static uint64_t now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000u; }
static int in_play(uint32_t tick, uint64_t now) {
    static uint32_t last;
    static uint64_t moved;
    if (tick != last) { last = tick; moved = now; }
    return tick && now - moved < 250;
}

/* Face buttons. In play they keep their place, since they shoot in four
 * directions (bottom shoots down like Xbox A); in the menus they go by label,
 * so A accepts and B goes back as on the console. A held button keeps the
 * meaning it had when pressed, so pausing or resuming mid-press does not turn
 * it into another action. */
static const struct { u64 sw; uint16_t play, menu; } FACE[] = {
    { HidNpadButton_B, 0x1000, 0x2000 },          /* bottom: A / B (back)   */
    { HidNpadButton_A, 0x2000, 0x1000 },          /* right:  B / A (accept) */
    { HidNpadButton_Y, 0x4000, 0x8000 },          /* left:   X / Y          */
    { HidNpadButton_X, 0x8000, 0x4000 },          /* top:    Y / X          */
};
static uint16_t g_face[4];

/* + in play: a tap pauses (START, sent on release), holding it restarts the
 * run through the game's own restart key, R held (measured on PC: a restart
 * after ~70 frames, and another if R stays down, so R goes up as soon as the
 * tick starts over). Outside play + is START as usual. */
static struct { int held, play, r_down, restarted; uint64_t since, start_until; uint32_t tick; } g_plus;

/* ---- settings.ini ---------------------------------------------------------- */
/* sdmc:/switch/isaac-recomp/settings.ini, written with the defaults when it
 * is missing; edit it to change them (key=0 or key=1). */
static int g_switch_glyphs = 1;   /* Switch button glyphs (native_glyphs.c)  */
static int g_a_accepts = 1;       /* menus by label: A accepts, B goes back */
static const char SETTINGS_DEFAULT[] =
    "# Isaac (browsing lift) on Switch. 1 = on, 0 = off.\n"
    "\n"
    "# Draw Nintendo Switch buttons instead of the Xbox ones.\n"
    "switch_glyphs=1\n"
    "\n"
    "# In menus A accepts and B goes back. 0: by position like an Xbox pad\n"
    "# (the bottom button, B, accepts). In play the buttons always shoot by\n"
    "# position.\n"
    "a_accepts=1\n";

static void load_settings(void) {
    FILE *f = fopen(ROOT "/settings.ini", "r");
    if (!f) {
        if ((f = fopen(ROOT "/settings.ini", "w"))) { fputs(SETTINGS_DEFAULT, f); fclose(f); }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (line[0] == '#' || !v) continue;
        *v++ = 0;
        char *k = line + strspn(line, " \t");
        k[strcspn(k, " \t")] = 0;
        if (!strcmp(k, "switch_glyphs")) g_switch_glyphs = atoi(v) != 0;
        else if (!strcmp(k, "a_accepts")) g_a_accepts = atoi(v) != 0;
    }
    fclose(f);
}
#define PLUS_TAP_MS 400

int isaac_native_pad_state(int slot, uint8_t *out) {
    if (slot != 0 || !g_pad_ready || !padIsConnected(&g_pad)) return 0;
    u64 b = padGetButtons(&g_pad);
    uint64_t now = now_ms();
    uint32_t tick = isaac_native_game_tick();
    int play = in_play(tick, now);
    uint16_t w = 0;
    if (b & HidNpadButton_Up) w |= 0x0001;
    if (b & HidNpadButton_Down) w |= 0x0002;
    if (b & HidNpadButton_Left) w |= 0x0004;
    if (b & HidNpadButton_Right) w |= 0x0008;
    if (b & HidNpadButton_Minus) w |= 0x0020;     /* BACK */
    if (b & HidNpadButton_StickL) w |= 0x0040;
    if (b & HidNpadButton_StickR) w |= 0x0080;
    if (b & HidNpadButton_L) w |= 0x0100;
    if (b & HidNpadButton_R) w |= 0x0200;
    for (int i = 0; i < 4; ++i) {
        if (!(b & FACE[i].sw)) { g_face[i] = 0; continue; }
        if (!g_face[i]) g_face[i] = play || !g_a_accepts ? FACE[i].play : FACE[i].menu;
        w |= g_face[i];
    }

    if (b & HidNpadButton_Plus) {
        if (!g_plus.held) {
            g_plus.held = 1;
            g_plus.play = play;
            g_plus.since = now;
            g_plus.tick = tick;
            g_plus.restarted = 0;
        }
        if (!g_plus.play) {
            w |= 0x0010;                              /* START */
        } else if (!g_plus.restarted) {
            if (!g_plus.r_down && isaac_native_input_key("r", 1)) g_plus.r_down = 1;
            if (tick < g_plus.tick) g_plus.restarted = 1;  /* a new run */
            else g_plus.tick = tick;
        }
    } else if (g_plus.held) {
        g_plus.held = 0;
        if (g_plus.play && !g_plus.restarted && now - g_plus.since < PLUS_TAP_MS)
            g_plus.start_until = now + 100;
    }
    if (g_plus.r_down && (!g_plus.held || g_plus.restarted) && isaac_native_input_key("r", 0))
        g_plus.r_down = 0;
    if (now < g_plus.start_until) w |= 0x0010;

    HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    ++g_packet;
    memcpy(out + 0, &g_packet, 4);
    memcpy(out + 4, &w, 2);
    out[6] = (b & HidNpadButton_ZL) ? 255 : 0;
    out[7] = (b & HidNpadButton_ZR) ? 255 : 0;
    int16_t s[4] = { (int16_t)l.x, (int16_t)l.y, (int16_t)r.x, (int16_t)r.y };
    memcpy(out + 8, s, 8);
    return 1;
}

/* ---- stage log ------------------------------------------------------------- */
/* sdmc:/switch/isaac-recomp/browsing-last-run.txt: one line per stage,
 * closed after each, so an exit before the nxlink channel opens still says
 * where it happened. */
#include <stdarg.h>
void isaac_native_stage(const char *fmt, ...);
#define stage isaac_native_stage
void isaac_native_stage(const char *fmt, ...) {
    mkdir(ROOT, 0777);
    FILE *f = fopen(ROOT "/browsing-last-run.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- heap ------------------------------------------------------------------ */
/* newlib's heap is the range hbloader hands over (or libnx sets up); a range
 * malloc returns has been seen to fault part way through on hardware, so the
 * boot logs what the process was given and the guest map checks its range. */
extern char *fake_heap_start, *fake_heap_end;

/* 1 when [p, p+n) is mapped read/write heap memory. */
int isaac_native_switch_mapped(const void *p, size_t n) {
    uint64_t a = (uint64_t)(uintptr_t)p, end = a + n;
    while (a < end) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
        if ((mi.perm & Perm_Rw) != Perm_Rw || (mi.type & MemState_Type) == MemType_Unmapped) {
            stage("heap: %010lx..%010lx not usable (type 0x%x perm %u attr %u), wanted up to %010lx",
                  (unsigned long)mi.addr, (unsigned long)(mi.addr + mi.size), mi.type, mi.perm, mi.attr,
                  (unsigned long)end);
            return 0;
        }
        a = mi.addr + mi.size;
    }
    return 1;
}

/* hbloader keeps one process and one heap for every NRO it runs. Mesa's
 * GPU buffers live in that heap, mapped to the GPU and marked uncached; if
 * they are not released at exit (eglTerminate never returned while the game
 * thread kept the context bound), the next NRO's malloc hands them out: memset's
 * `dc zva` and malloc's own chunk writes then fault on uncached memory
 * ("Se ha cerrado el programa a causa de un error", measured: the second
 * launch in one hbloader process found 60+ such regions above the old guest
 * map). The exit releases them; the boot fences off any it still finds. */
static unsigned heap_scan(uint64_t from, uint64_t *lo, uint64_t *hi, uint64_t *bytes) {
    uint64_t a = from, end = (uint64_t)(uintptr_t)fake_heap_end;
    unsigned n = 0;
    *lo = *hi = *bytes = 0;
    while (a < end) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !mi.size) break;
        uint64_t r0 = mi.addr < from ? from : mi.addr, r1 = mi.addr + mi.size > end ? end : mi.addr + mi.size;
        if (mi.attr || (mi.perm & Perm_Rw) != Perm_Rw) {
            if (!n++) *lo = r0;
            *hi = r1;
            *bytes += r1 - r0;
        }
        a = mi.addr + mi.size;
    }
    return n;
}

/* Clears the uncached attribute libnx's nvMapCreate leaves on buffers that
 * were never closed; regions still mapped to the GPU refuse and stay. */
static unsigned heap_clear_uncached(void) {
    uint64_t a = (uint64_t)(uintptr_t)fake_heap_start, end = (uint64_t)(uintptr_t)fake_heap_end;
    unsigned cleared = 0;
    while (a < end) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !mi.size) break;
        if ((mi.attr & MemAttr_IsUncached) && !(mi.attr & MemAttr_IsDeviceMapped) &&
            R_SUCCEEDED(svcSetMemoryAttribute((void *)(uintptr_t)mi.addr, mi.size, MemAttr_IsUncached, 0)))
            ++cleared;
        a = mi.addr + mi.size;
    }
    return cleared;
}

static void heap_report(void) {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    stage("memory: total %lu MiB, used %lu MiB; heap override %d (%p, %lu MiB); newlib heap %p..%p (%lu MiB)",
          (unsigned long)(total >> 20), (unsigned long)(used >> 20), envHasHeapOverride(),
          envGetHeapOverrideAddr(), (unsigned long)(envGetHeapOverrideSize() >> 20),
          fake_heap_start, fake_heap_end, (unsigned long)((fake_heap_end - fake_heap_start) >> 20));
    unsigned cleared = heap_clear_uncached();
    uint64_t brk = (uint64_t)(uintptr_t)sbrk(0), lo, hi, bytes;
    unsigned below = heap_scan((uint64_t)(uintptr_t)fake_heap_start, &lo, &hi, &bytes) -
                     heap_scan(brk, &lo, &hi, &bytes);
    unsigned n = heap_scan(brk, &lo, &hi, &bytes);
    stage("heap: %u uncached region(s) restored; %u left by an earlier NRO above the break "
          "(%lu MiB in %010lx..%010lx), %u below it",
          cleared, n, (unsigned long)(bytes >> 20), (unsigned long)lo, (unsigned long)hi, below);
    if (!n) return;
    /* Fence them off: a pad up to just below the first region, a block over
     * all of them that is never freed, then the pad goes back to malloc. */
    void *pad = lo - brk > (2u << 20) ? malloc((size_t)(lo - brk - (2u << 20))) : NULL;
    size_t span = (size_t)(hi - lo) + (4u << 20);
    uint8_t *fence = malloc(span);
    free(pad);
    if (fence && (uint64_t)(uintptr_t)fence <= lo && (uint64_t)(uintptr_t)fence + span >= hi)
        stage("heap: fenced %lu MiB at %p", (unsigned long)(span >> 20), (void *)fence);
    else
        stage("heap: fence failed (%p, %lu MiB); this boot may fault", (void *)fence, (unsigned long)(span >> 20));
}

/* At exit, after eglTerminate: drop any reference to the nvdrv session
 * still held, which unmaps this run's buffers from the GPU, then put the heap
 * back to cached memory for the next NRO. */
static void heap_release(void) {
    uint64_t lo, hi, bytes;
    unsigned n = heap_scan((uint64_t)(uintptr_t)fake_heap_start, &lo, &hi, &bytes);
    stage("heap release: %u region(s) marked after eglTerminate (%lu MiB); closing nvdrv",
          n, (unsigned long)(bytes >> 20));
    for (int i = 0; i < 64; ++i) nvExit();
    unsigned cleared = heap_clear_uncached();
    n = heap_scan((uint64_t)(uintptr_t)fake_heap_start, &lo, &hi, &bytes);
    stage("heap release: nvdrv closed, %u region(s) restored, %u still marked (%lu MiB)",
          cleared, n, (unsigned long)(bytes >> 20));
}

/* ---- the game thread ------------------------------------------------------ */

typedef struct { int argc; char **argv; int rc; } game_args;

static void *game_thread(void *p) {
    game_args *a = p;
    a->rc = isaac_native_main(a->argc, a->argv);
    isaac_native_gl_release_thread();
    return NULL;
}

static int run_game(void) {
    static char *argv[] = { "isaac", "romfs:/isaac.segs.bin", ROOT "/guest", "main", NULL };
    game_args a = { 4, argv, -1 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    if (pthread_create(&t, &attr, game_thread, &a) != 0) return -1;
    pthread_join(t, NULL);
    return a.rc;
}

/* ---- autotest protocol ---------------------------------------------------- */

static const char *g_return_status = "NOT_REQUESTED";

static int prepare_sphaira_return(void) {
    static const char *paths[] = { "sdmc:/switch/sphaira/sphaira.nro", "sdmc:/switch/sphaira.nro" };
    if (!envHasNextLoad()) return 0;
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        unsigned char header[20];
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        size_t count = fread(header, 1, sizeof header, f);
        fclose(f);
        if (count != sizeof header || memcmp(header + 16, "NRO0", 4)) continue;
        if (R_SUCCEEDED(envSetNextLoad(paths[i], paths[i]))) { g_return_status = "SPHAIRA"; return 1; }
        return 0;
    }
    return 0;
}

static void receipt(const char *event, const char *run_id, const char *result,
                    const char *synthetic, const char *graphics, const char *audio,
                    uint32_t tick, int frames) {
    fprintf(stderr, "SWITCH_TEST {\"event\":\"%s\",\"run_id\":\"%s\",\"result\":\"%s\","
            "\"synthetic\":\"%s\",\"current_pe\":\"NOT_BUILT\",\"frontier\":\"BROWSING_NATIVE\","
            "\"return_to\":\"%s\",\"graphics\":\"%s\",\"filesystem\":\"NOT_BUILT\",\"audio\":\"%s\","
            "\"game_tick\":%u,\"frames\":%d,\"applet_type\":%u}\n",
            event, run_id, result, synthetic, g_return_status, graphics, audio,
            (unsigned)tick, frames, (unsigned)appletGetAppletType());
    fflush(stderr);
}

/* Enter until a run starts, then walk and shoot (the PC smoke timeline). */
static void autotest_input(char *buf, size_t cap, int frames) {
    size_t n = 0;
    for (int f = 120; f <= 840 && n < cap; f += 40) n += snprintf(buf + n, cap - n, "%d:enter,", f);
    for (int f = 900; f + 160 <= frames && n < cap; f += 160)
        n += snprintf(buf + n, cap - n, "%d:d:40,%d:w:40,%d:a:40,%d:s:40,%d:right:30,%d:up:30,",
                      f, f + 40, f + 80, f + 120, f, f + 80);
}

int main(int argc, char **argv) {
    remove(ROOT "/browsing-last-run.txt");
    int automatic = 0, return_sphaira = 0, frames = 1500, mods = 0, no_script = 0;
    const char *run_id = "manual";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--autotest")) automatic = 1;
        else if (!strcmp(argv[i], "--return-to-sphaira")) return_sphaira = 1;
        else if (!strcmp(argv[i], "--run-id") && i + 1 < argc) run_id = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--graphics")) { /* the runner's flag; this NRO always draws */ }
        else if (!strcmp(argv[i], "--mods")) mods = 1;
        else if (!strcmp(argv[i], "--no-script")) no_script = 1;
        else { stage("bad argument %s", argv[i]); return 2; }
    }
    stage("start: %d argument(s), autotest=%d", argc, automatic);
    heap_report();
    mkdir(ROOT, 0777);
    load_settings();
    stage("settings: switch_glyphs=%d a_accepts=%d", g_switch_glyphs, g_a_accepts);
    setenv("ISAAC_NATIVE_SWITCH_GLYPHS", g_switch_glyphs ? (g_a_accepts ? "1" : "2") : "0", 1);
    if (automatic && (strlen(run_id) != 32 || strspn(run_id, "0123456789abcdef") != 32)) return 2;

    int saved_stderr = -1, saved_stdout = -1, nx_socket = -1;
    if (automatic) {
        saved_stderr = dup(STDERR_FILENO);
        saved_stdout = dup(STDOUT_FILENO);
        if (R_FAILED(socketInitializeDefault())) { stage("socketInitializeDefault failed"); return 2; }
        nx_socket = nxlinkStdioForDebug();
        if (nx_socket < 0) { stage("nxlinkStdioForDebug failed"); socketExit(); return 2; }
        stage("nxlink stdio connected");
        dup2(nx_socket, STDOUT_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_pad_ready = 1;

    if (automatic) receipt("begin", run_id, "RUNNING", "NOT_RUN", "NOT_RUN", "NOT_RUN", 0, 0);
    const char *status = "FAIL", *synthetic = "NOT_RUN", *graphics = "NOT_RUN", *audio = "NOT_RUN";
    uint32_t tick = 0;
    int presented = 0;

    if (return_sphaira && !prepare_sphaira_return()) {
        g_return_status = "FAILED";
        fputs("[switch] could not select Sphaira as the return target\n", stderr);
    } else if (appletGetAppletType() != AppletType_Application) {
        status = "WRONG_MODE";
        fputs("[switch] Application mode is required (launch over a title, or hold R)\n", stderr);
    } else if (R_FAILED(romfsInit())) {
        fputs("[switch] RomFS did not mount\n", stderr);
    } else {
        mkdir(ROOT, 0777);
        setenv("ISAAC_NATIVE_INSTANCE_LIST", "romfs:/instance_index.json", 1);
        static char input[8192], max_frames[16], shots[64], shot_dir[128];
        if (automatic) {
            setenv("ISAAC_SAVE_DIR", ROOT "/saves-autotest", 1);
            snprintf(max_frames, sizeof max_frames, "%d", frames);
            setenv("ISAAC_MAX_FRAMES", max_frames, 1);
            if (!no_script) {               /* --no-script: the pad drives it */
                autotest_input(input, sizeof input, frames);
                setenv("ISAAC_INPUT", input, 1);
            }
            snprintf(shots, sizeof shots, "%d", frames > 1000 ? 1000 : frames / 2);
            setenv("ISAAC_NATIVE_SHOTS", shots, 1);
            snprintf(shot_dir, sizeof shot_dir, ROOT "/shots-%s", run_id);
            mkdir(shot_dir, 0777);
            setenv("ISAAC_NATIVE_SHOT_DIR", shot_dir, 1);
        } else {
            setenv("ISAAC_SAVE_DIR", ROOT "/saves", 1);
            mods = 1;                    /* play mode: the instance's mods/ is mounted */
            setenv("ISAAC_NATIVE_WALL_CLOCK", "1", 1);   /* real time, as the web build plays */
        }
        if (mods) setenv("ISAAC_NATIVE_MODS", "1", 1);
        stage("running the game");
        int rc = run_game();
        tick = isaac_native_game_tick_max();
        presented = isaac_native_gl_frame_count();
        fprintf(stderr, "[switch] game returned %d, game tick %u, %d frames presented\n",
                rc, (unsigned)tick, presented);
        synthetic = (rc == 0 && tick > 100) ? "PASS" : "FAIL";
        graphics = (isaac_native_gl_state() > 0 && (!automatic || presented >= frames)) ? "PASS" : "FAIL";
        audio = isaac_native_audio_state() > 0 ? "PASS" : "FAIL";
        status = (!strcmp(synthetic, "PASS") && !strcmp(graphics, "PASS") && !strcmp(audio, "PASS"))
                 ? "PASS" : "FAIL";
    }

    /* The receipt goes out before the teardown, so a teardown that blocks
     * still leaves the run's result on record. */
    stage("end: %s (synthetic %s, graphics %s, audio %s, tick %u, frames %d, return %s)",
          status, synthetic, graphics, audio, (unsigned)tick, presented, g_return_status);
    if (automatic) receipt("end", run_id, status, synthetic, graphics, audio, tick, presented);
    stage("audio shutdown");
    isaac_native_audio_shutdown();
    stage("gl shutdown");
    isaac_native_gl_shutdown();
    heap_release();
    romfsExit();
    stage("teardown done");
    if (automatic) {
        dup2(saved_stderr, STDERR_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stderr);
        close(saved_stdout);
        close(nx_socket);
        socketExit();
    }
    return !strcmp(status, "PASS") ? 0 : 1;
}
#endif
