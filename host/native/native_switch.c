/* Nintendo Switch entry for the native host of the browsing-of-isaac lift.
 *
 * Layout on the console:
 *   romfs:/isaac.segs.bin                      memory image (embedded)
 *   romfs:/instance_index.json                 files the web build mounts
 *   sdmc:/switch/isaac-recomp/guest/           the PC install (resources/...)
 *   sdmc:/switch/isaac-recomp/saves/           saves (saves-autotest/ for tests)
 *   sdmc:/switch/isaac-recomp/guest/mods/      mods, mounted when playing (--mods in tests)
 *
 * Interactive: a launch screen first (the Steam Cloud pull and a Play button,
 * native_launcher.c; settings.ini launcher=0 skips it), then the game runs
 * until it exits or HOME closes it; the pad is
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
 *   audio      PASS when an OpenAL device was opened
 *
 * --bench <room|horde|swarm|tears> [--bench-from N] [--bench-to N]
 * [--profile-hz N] [--vsync] [--game-core C], with --autotest: the bench mod
 * from RomFS instead of the player's mods, no vsync (unless --vsync), and a
 * BENCH line for frames N..M (native_bench.c); --profile-hz samples the game
 * thread (native_prof_switch.c); --game-core pins it; --env K=V sets an
 * environment variable (a driver option, say) before anything starts.
 * tools/bench_native.py drives it. */
#ifdef __SWITCH__
#include <switch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "native_gamefiles.h"
#include "native_launcher.h"
#ifndef ISAAC_NO_STEAM
#include "native_qr.h"
#include "native_steamauth.h"
#include "native_steamsync.h"
#endif

int isaac_native_main(int argc, char **argv);
uint32_t isaac_native_game_tick_max(void);
void isaac_native_request_quit(void);
int isaac_native_gl_state(void);
int isaac_native_gl_frame_count(void);
int isaac_native_audio_state(void);
void isaac_native_gl_shutdown(void);
void isaac_native_gl_release_thread(void);
void isaac_native_audio_shutdown(void);
void isaac_native_prof_config(int hz, int game_core);
void isaac_native_prof_game_thread(void);

#define ROOT "sdmc:/switch/isaac-recomp"

/* ---- pad -> XINPUT_STATE -------------------------------------------------- */

static PadState g_pad;
static int g_pad_ready;
static uint32_t g_packet;

/* When HOME -> Close asked the game to quit (see appletLockExit in main). */
static uint64_t g_exit_tick;

void isaac_native_frame_hook(unsigned n) {
    (void)n;
    if (!appletMainLoop()) {
        if (!g_exit_tick) g_exit_tick = armGetSystemTick();
        isaac_native_request_quit();
    }
    if (g_pad_ready) padUpdate(&g_pad);
}

/* Milliseconds since HOME -> Close, 0 when it was not used. */
static unsigned since_exit_request(void) {
    return g_exit_tick ? (unsigned)(armTicksToNs(armGetSystemTick() - g_exit_tick) / 1000000u) : 0;
}

/* Menus or play: the game's tick only advances in an unpaused run (measured:
 * it holds still while paused and reads 0 in the menus). Mod Config Menu
 * draws over a running game, so it is asked for separately
 * (native_lua_state.c). */
uint32_t isaac_native_game_tick(void);
int isaac_native_input_key(const char *name, int down);
int isaac_native_mod_menu_open(void);
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
 * it into another action.
 *
 * Mod Config Menu selects with the game's button 4 and goes back with 5,
 * which it calls A and B. The game numbers an Xbox pad X 4, B 5, Y 6, A 7
 * (a PC's inputconfigs.dat for an Xbox 360 pad; on the console Switch A, sent
 * as Xbox A, reached MCM's rebind screen as its "Y", 7), so with an Xbox pad
 * MCM selects with X, on PC too. While MCM is open, A goes as Xbox X (4,
 * select) and B as Xbox B (5, back); with a_accepts=0, by position, the
 * bottom button selects and the right one goes back. */
static const struct { u64 sw; uint16_t play, menu, mcm_label, mcm_place; } FACE[] = {
    { HidNpadButton_B, 0x1000, 0x2000, 0x2000, 0x4000 },   /* bottom: A / B (back)   */
    { HidNpadButton_A, 0x2000, 0x1000, 0x4000, 0x2000 },   /* right:  B / A (accept) */
    { HidNpadButton_Y, 0x4000, 0x8000, 0x1000, 0x1000 },   /* left:   X / Y          */
    { HidNpadButton_X, 0x8000, 0x4000, 0x8000, 0x8000 },   /* top:    Y / X          */
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
static int g_frame_skip = 1;      /* in play: 0 every frame, 1 when late, 2 always (ISAAC_RENDER_GATE) */
static int g_render_thread = 1;   /* GL on its own core (native_glq.c, ISAAC_NATIVE_GLQ) */
#ifndef ISAAC_NO_STEAM
static int g_steam_sync = 1;      /* sync the saves with Steam Cloud          */
static int g_sync_prompt = 1;     /* ask on a conflict instead of newest-wins */
#endif
static int g_launcher = 1;        /* the launch screen, with its Play button  */
static int g_check_files = 1;     /* no Play while the game's files are incomplete */
static const char SETTINGS_DEFAULT[] =
    "# Isaac (browsing lift) on Switch. 1 = on, 0 = off.\n"
    "\n"
    "# Draw Nintendo Switch buttons instead of the Xbox ones.\n"
    "switch_glyphs=1\n"
    "\n"
    "# In menus A accepts and B goes back. 0: by position like an Xbox pad\n"
    "# (the bottom button, B, accepts). In play the buttons always shoot by\n"
    "# position.\n"
    "a_accepts=1\n"
    "\n"
    "# The game updates 30 times a second and draws a second frame between\n"
    "# two updates. Where that does not fit in the time (heavy rooms, the\n"
    "# water floors) the whole game slows down. 1: skip the in-between frame\n"
    "# only then, so the game keeps its speed. 2: always skip it (30 fps,\n"
    "# the most headroom). 0: always draw it.\n"
    "frame_skip=1\n"
    "\n"
    "# Feed the GPU from a second CPU core while the game runs on the first.\n"
    "# 0: everything on one core, as older builds did.\n"
    "render_thread=1\n"
    "\n"
    "# Before the game, a check that its files are on the card and none was\n"
    "# cut short by a copy; Play stays off when they are not. 0: no check.\n"
    "check_game_files=1\n"
    "\n"
#ifndef ISAAC_NO_STEAM
    "# Save sync with Steam Cloud, straight from the console. The launch\n"
    "# screen signs in with a QR code and keeps steam-token.txt next to this\n"
    "# file.\n"
    "steam_sync=1\n"
    "\n"
    "# When the console and the cloud both changed a save, ask on screen.\n"
    "# 0: the newest copy wins without asking.\n"
    "sync_prompt=1\n"
    "\n"
    "# Before the game, a screen with the Steam Cloud sync and a Play button.\n"
    "# 0: sync without a screen (the newest copy wins a conflict) and start\n"
    "# the game straight away.\n"
#else
    "# Before the game, a screen with the file check and a Play button.\n"
    "# 0: start the game straight away.\n"
#endif
    "launcher=1\n";

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
        else if (!strcmp(k, "frame_skip")) g_frame_skip = atoi(v);
        else if (!strcmp(k, "render_thread")) g_render_thread = atoi(v) != 0;
#ifndef ISAAC_NO_STEAM
        else if (!strcmp(k, "steam_sync")) g_steam_sync = atoi(v) != 0;
        else if (!strcmp(k, "sync_prompt")) g_sync_prompt = atoi(v) != 0;
#endif
        else if (!strcmp(k, "check_game_files")) g_check_files = atoi(v) != 0;
        else if (!strcmp(k, "launcher")) g_launcher = atoi(v) != 0;
    }
    fclose(f);
}
#define PLUS_TAP_MS 400

int isaac_native_pad_state(int slot, uint8_t *out) {
    if (slot != 0 || !g_pad_ready || !padIsConnected(&g_pad)) return 0;
    u64 b = padGetButtons(&g_pad);
    uint64_t now = now_ms();
    uint32_t tick = isaac_native_game_tick();
    int play = in_play(tick, now), mcm = isaac_native_mod_menu_open();
    if (mcm) play = 0;
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
        if (!g_face[i])
            g_face[i] = mcm ? (g_a_accepts ? FACE[i].mcm_label : FACE[i].mcm_place)
                      : play || !g_a_accepts ? FACE[i].play : FACE[i].menu;
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

/* ---- launch screen ---------------------------------------------------------- */
/* Before the game creates its EGL context the NRO draws its own screen
 * (native_launcher.c) on a libnx framebuffer: the Steam Cloud pull runs on a
 * thread while the screen shows each step and save file, a conflict is asked
 * there, and the game starts only when the player presses A on Play.
 * framebufferClose hands the window back, which is why this runs before
 * run_game(). The push after the game draws nothing: the game's EGL context
 * still owns the window then, and the stage log keeps what came of it. */
static pthread_mutex_t g_ln_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ln_cond = PTHREAD_COND_INITIALIZER;
static isaac_launcher_view g_ln;    /* under g_ln_lock */
#ifndef ISAAC_NO_STEAM
static int g_ln_on;                 /* a screen is up: the sync reports to it */
static int g_ln_busy;               /* the sync thread is running */
static int g_ln_answer = -1;        /* the conflict answer, from the pad */
static int g_ln_quit;               /* leaving: a pending question keeps the console's copy */
static int g_ln_cancel;             /* the sign-in dialog was closed */
static int g_ln_has_token;          /* steam-token.txt is on the card */
static int g_ln_token_bad;          /* ...but it is unusable, or Steam turned it down */
static int g_ln_start_pull;         /* signed in: the screen starts the pull */
#endif

static void ln_reset(int phase) {
    pthread_mutex_lock(&g_ln_lock);
    g_ln.phase = phase;
    for (int i = 0; i < ISAAC_LAUNCHER_STEPS; ++i) {
        g_ln.step[i] = phase == ISAAC_LAUNCHER_OFF ? ISAAC_STEP_SKIP : ISAAC_STEP_WAIT;
        g_ln.step_detail[i][0] = 0;
    }
    for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) g_ln.slot[i] = ISAAC_LAUNCHER_SLOT_PENDING;
    if (phase == ISAAC_LAUNCHER_SIGNED_OUT) g_ln.step[ISAAC_LAUNCHER_STEP_TOKEN] = ISAAC_STEP_SKIP;
    g_ln.error[0] = 0;
    g_ln.conflict = 0;
    pthread_mutex_unlock(&g_ln_lock);
}

/* ---- the game's files ------------------------------------------------------- */
/* Runs with or without a screen and in every build: Steam or not, the game
 * cannot start from an incomplete copy. */
static void check_game_files(void) {
    if (!g_check_files) {
        stage("game files: check off");
        return;
    }
    isaac_gamefiles f;
    if (isaac_gamefiles_check("romfs:/instance_index.json", ROOT "/guest", &f) != 0) {
        stage("game files: this build's list could not be read, not checked");
        return;
    }
    stage("game files: %d listed, %d missing, %d cut short, %d of another size", f.total, f.missing, f.truncated,
          f.different);
    for (int i = 0; i < ISAAC_GAMEFILES_NAMES && f.bad[i][0]; ++i) stage("game files: %s", f.bad[i]);
    pthread_mutex_lock(&g_ln_lock);
    if (isaac_gamefiles_blocking(&f)) {
        int bad = f.missing + f.truncated, shown = 0;
        g_ln.files = ISAAC_FILES_BAD;
        snprintf(g_ln.files_summary, sizeof g_ln.files_summary, "%d of %d %s", bad, f.total,
                 f.missing ? "missing" : "cut short");
        int n = 0;
        for (int i = 0; i < ISAAC_GAMEFILES_NAMES && f.bad[i][0] && i < 2; ++i, ++shown) {
            const char *base = strrchr(f.bad[i], '/');
            n += snprintf(g_ln.files_detail + n, sizeof g_ln.files_detail - n, "%s%s", i ? ", " : "",
                          base ? base + 1 : f.bad[i]);
        }
        if (bad > shown) n += snprintf(g_ln.files_detail + n, sizeof g_ln.files_detail - n, " and %d more", bad - shown);
        snprintf(g_ln.files_detail + n, sizeof g_ln.files_detail - n,
                 ". Copy them from your PC install to sdmc:/switch/isaac-recomp/guest.");
    } else {
        g_ln.files = ISAAC_FILES_OK;
        snprintf(g_ln.files_summary, sizeof g_ln.files_summary, "%d of %d found", f.total, f.total);
    }
    pthread_mutex_unlock(&g_ln_lock);
}

#ifndef ISAAC_NO_STEAM
/* One step's state, with a detail on the right when `fmt` is given. */
static void ln_step(int step, int state, const char *fmt, ...) {
    if (!g_ln_on) return;
    pthread_mutex_lock(&g_ln_lock);
    g_ln.step[step] = state;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_ln.step_detail[step], sizeof g_ln.step_detail[step], fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_ln_lock);
}

static void ln_error(const char *fmt, ...) {
    if (!g_ln_on) return;
    pthread_mutex_lock(&g_ln_lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ln.error, sizeof g_ln.error, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_ln_lock);
}

/* ---- Steam Cloud save sync ------------------------------------------------- */
/* The token file comes from the launch screen's QR sign-in, or from the PC
 * provisioning step (host/steam_cloud_cm.py token-file).
 * Pull runs before the game opens its saves, push after it closed them; any
 * failure is logged and the game runs anyway. */
#define TOKEN_PATH ROOT "/steam-token.txt"
#define SIGNED_OUT_TEXT "Not signed in to Steam: your saves stay on this console and nothing is uploaded."

/* The last thing the sync said that was not progress: why it stopped. */
static char g_sync_last[160];

/* A pull of this run that ran to the end. The push needs it: the pull marker
 * on the SD can be left by an earlier run (one that never played, say), and
 * after "Play anyway" this run compared nothing. */
static int g_pulled;

/* Every line goes to the stage log; with a screen up, the lines native_steamcm.c
 * writes as it gets through the connection also move the steps along. */
static void sync_log(void *user, const char *line) {
    (void)user;
    stage("%s", line);
    if (!g_ln_on) return;
    const char *s = !strncmp(line, "steam sync: ", 12) ? line + 12 : line;
    int n, cell;
    char host[96];
    pthread_mutex_lock(&g_ln_lock);
    snprintf(g_ln.activity, sizeof g_ln.activity, "%s", s);
    pthread_mutex_unlock(&g_ln_lock);
    if (sscanf(s, "CM list: %d host", &n) == 1) {
        ln_step(ISAAC_LAUNCHER_STEP_CONNECT, ISAAC_STEP_RUN, "%d server%s", n, n == 1 ? "" : "s");
    } else if (sscanf(s, "websocket + TLS to %95s", host) == 1) {
        ln_step(ISAAC_LAUNCHER_STEP_CONNECT, ISAAC_STEP_OK, "%s", host);
        ln_step(ISAAC_LAUNCHER_STEP_LOGON, ISAAC_STEP_RUN, NULL);
    } else if (sscanf(s, "logged on (cell %d", &cell) == 1) {
        ln_step(ISAAC_LAUNCHER_STEP_LOGON, ISAAC_STEP_OK, "cell %d", cell);
        ln_step(ISAAC_LAUNCHER_STEP_OWNED, ISAAC_STEP_RUN, NULL);
    } else if (sscanf(s, "cloud: %d file", &n) == 1) {
        ln_step(ISAAC_LAUNCHER_STEP_COMPARE, ISAAC_STEP_RUN, "%d file%s in the cloud", n, n == 1 ? "" : "s");
    } else if (strncmp(s, "downloaded ", 11) && strncmp(s, "uploaded ", 9) && !strstr(s, ": downloaded ")) {
        snprintf(g_sync_last, sizeof g_sync_last, "%s", s);
    }
}

static void sync_slot(void *user, int slot, int status) {
    (void)user;
    pthread_mutex_lock(&g_ln_lock);
    g_ln.slot[slot] = status;
    pthread_mutex_unlock(&g_ln_lock);
}

/* What Steam said the account owns: a warning on the page, never a reason to
 * stop the game. Without the game itself the sync stops (the pull returns
 * ISAAC_STEAMSYNC_NOT_OWNED). */
static int g_own_status;
static char g_own_missing[128];

static void sync_owned(void *user, int status, const char *missing) {
    (void)user;
    g_own_status = status;
    snprintf(g_own_missing, sizeof g_own_missing, "%s", missing);
    if (status == ISAAC_OWN_ALL)
        ln_step(ISAAC_LAUNCHER_STEP_OWNED, ISAAC_STEP_OK, "game and all expansions");
    else if (status == ISAAC_OWN_PART)
        ln_step(ISAAC_LAUNCHER_STEP_OWNED, ISAAC_STEP_WARN, "not all expansions");
    else if (status == ISAAC_OWN_NONE)
        ln_step(ISAAC_LAUNCHER_STEP_OWNED, ISAAC_STEP_FAIL, "not on this account");
    else
        ln_step(ISAAC_LAUNCHER_STEP_OWNED, ISAAC_STEP_WARN, "not verified");
    if (status != ISAAC_OWN_NONE) ln_step(ISAAC_LAUNCHER_STEP_COMPARE, ISAAC_STEP_RUN, NULL);
}

/* Conflict: both copies moved since the last sync. The dialog on the launch
 * screen answers it (0 keeps this console's copy, 1 Steam's); the sync thread
 * waits here for the pad. */
static int ask_conflict(void *user, const char *slot, const char *local_desc, const char *cloud_desc) {
    (void)user;
    int n = 0, choice;
    pthread_mutex_lock(&g_ln_lock);
    if (sscanf(slot, "persistentgamedata%d", &n) == 1)
        snprintf(g_ln.conflict_slot, sizeof g_ln.conflict_slot, "Slot %d (progress)", n);
    else if (sscanf(slot, "gamestate%d", &n) == 1)
        snprintf(g_ln.conflict_slot, sizeof g_ln.conflict_slot, "Slot %d (run in progress)", n);
    else
        snprintf(g_ln.conflict_slot, sizeof g_ln.conflict_slot, "%s", slot);
    snprintf(g_ln.conflict_local, sizeof g_ln.conflict_local, "%s", local_desc);
    snprintf(g_ln.conflict_cloud, sizeof g_ln.conflict_cloud, "%s", cloud_desc);
    g_ln.conflict_choice = 0;
    g_ln_answer = -1;
    g_ln.conflict = 1;
    while (g_ln_answer < 0 && !g_ln_quit) pthread_cond_wait(&g_ln_cond, &g_ln_lock);
    choice = g_ln_answer > 0;
    g_ln.conflict = 0;
    pthread_mutex_unlock(&g_ln_lock);
    return choice;
}

/* 0 when the pull or push ran to the end. */
static int steam_sync_do(int push) {
    if (!isaac_steamcm_available()) {
        stage("steam sync: this build has no Steam transport");
        ln_step(ISAAC_LAUNCHER_STEP_TOKEN, ISAAC_STEP_FAIL, NULL);
        ln_error("This build has no Steam transport.");
        return -1;
    }
    char token[1024];
    char err[160];
    uint64_t steamid = 0;
    ln_step(ISAAC_LAUNCHER_STEP_TOKEN, ISAAC_STEP_RUN, NULL);
    if (isaac_steamsync_read_token(TOKEN_PATH, &steamid, token, sizeof token, err, sizeof err) != 0) {
        stage("steam sync: %s", err);
        ln_step(ISAAC_LAUNCHER_STEP_TOKEN, ISAAC_STEP_FAIL, "no token");
        if (!strncmp(err, "no token file", 13)) {
            ln_error(SIGNED_OUT_TEXT);
        } else {
            g_ln_token_bad = 1;
            ln_error("steam-token.txt is not usable: %s. X signs in again.", err);
        }
        return -1;
    }
    if (g_ln_on) {
        pthread_mutex_lock(&g_ln_lock);
        snprintf(g_ln.account, sizeof g_ln.account, "Steam \xc2\xb7 \xe2\x80\xa6%07llu",
                 (unsigned long long)(steamid % 10000000u));
        pthread_mutex_unlock(&g_ln_lock);
    }
    ln_step(ISAAC_LAUNCHER_STEP_TOKEN, ISAAC_STEP_OK, "steamid \xe2\x80\xa6%07llu",
            (unsigned long long)(steamid % 10000000u));
    /* TLS verifies certificates against the console's clock: without a real
     * date every handshake would fail, so say so instead of trying. */
    time_t now = time(NULL);
    if (now < 1735689600) {
        stage("steam sync: the clock is not set, skipping");
        ln_step(ISAAC_LAUNCHER_STEP_CLOCK, ISAAC_STEP_FAIL, "not set");
        ln_error("The console's clock is not set, and Steam's certificates are checked "
                 "against it. Set the date in System Settings.");
        return -1;
    }
    char when[32];
    struct tm tm;
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime_r(&now, &tm));
    ln_step(ISAAC_LAUNCHER_STEP_CLOCK, ISAAC_STEP_OK, "%s", when);
    ln_step(ISAAC_LAUNCHER_STEP_CONNECT, ISAAC_STEP_RUN, NULL);
    if (R_FAILED(socketInitializeDefault())) {
        stage("steam sync: socketInitializeDefault failed");
        ln_step(ISAAC_LAUNCHER_STEP_CONNECT, ISAAC_STEP_FAIL, "no network");
        ln_error("The network could not be started (socketInitializeDefault).");
        return -1;
    }
    isaac_steamsync_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.save_dir = ROOT "/saves";
    cfg.steamid = steamid;
    cfg.token = token;
    cfg.prompt = g_sync_prompt;
    cfg.timeout_ms = 15000;
    g_sync_last[0] = 0;
    isaac_steamsync_set_log(sync_log, NULL);
    isaac_steamcm_set_log(sync_log, NULL);
    isaac_steamsync_set_slot_fn(g_ln_on ? sync_slot : NULL, NULL);
    g_own_status = ISAAC_OWN_UNKNOWN;
    g_own_missing[0] = 0;
    isaac_steamsync_set_own_fn(sync_owned, NULL);
    /* Without a screen there is nobody to ask: the newest copy wins. */
    isaac_steamsync_set_ask(g_sync_prompt && g_ln_on ? ask_conflict : NULL, NULL);
    stage("steam sync: %s", push ? "push" : "pull");
    int rc = push ? isaac_steamsync_push(&cfg) : isaac_steamsync_pull(&cfg);
    stage("steam sync: %s %s", push ? "push" : "pull", rc == 0 ? "done" : "skipped");
    socketExit();
    if (!push) g_pulled = rc == 0;
    if (rc == 0) {
        ln_step(ISAAC_LAUNCHER_STEP_COMPARE, ISAAC_STEP_OK, NULL);
        if (!push && g_own_status == ISAAC_OWN_PART)
            ln_error("This Steam account does not have %s. Saves sync, but this console runs Repentance+.",
                     g_own_missing);
    } else if (rc == ISAAC_STEAMSYNC_NOT_OWNED) {
        ln_error("This Steam account does not have %s, so nothing syncs. "
                 "You can still play with the files on the SD card.", g_own_missing);
    } else if (g_ln_on) {
        /* the step that was running is the one that failed */
        pthread_mutex_lock(&g_ln_lock);
        for (int i = 0; i < ISAAC_LAUNCHER_STEPS; ++i)
            if (g_ln.step[i] == ISAAC_STEP_RUN) g_ln.step[i] = ISAAC_STEP_FAIL;
        pthread_mutex_unlock(&g_ln_lock);
        if (!push && isaac_steamcm_token_refused()) {
            g_ln_token_bad = 1;
            ln_error("Steam no longer accepts this console's sign-in: it was removed or has expired. "
                     "X signs in again. Your saves stay on this console.");
        } else {
            ln_error("%s. Your saves stay on this console and nothing is uploaded at exit.",
                     g_sync_last[0] ? g_sync_last : "The sync could not finish");
        }
    }
    return rc;
}

/* Without the screen (launcher=0), and for the push after the game. */
static void steam_sync_run(int push) {
    if (!g_steam_sync) return;
    if (push && !g_pulled) {
        stage("steam sync: no pull completed in this run, not uploading");
        return;
    }
    steam_sync_do(push);
}

static void *sync_thread(void *p) {
    (void)p;
    int rc = steam_sync_do(0);
    pthread_mutex_lock(&g_ln_lock);
    g_ln.phase = rc == 0 ? ISAAC_LAUNCHER_DONE : ISAAC_LAUNCHER_FAILED;
    g_ln_busy = 0;
    pthread_mutex_unlock(&g_ln_lock);
    return NULL;
}

/* The network has one user at a time: the pull, the sign-in or the sign-out
 * runs on this thread, and g_ln_busy keeps the others from starting. */
static int worker_start(pthread_t *t, void *(*fn)(void *)) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    g_ln_busy = 1;
    if (pthread_create(t, &attr, fn, NULL) == 0) return 0;
    g_ln_busy = 0;
    return -1;
}

static int sync_thread_start(pthread_t *t) {
    ln_reset(ISAAC_LAUNCHER_SYNCING);
    if (worker_start(t, sync_thread) == 0) return 0;
    ln_reset(ISAAC_LAUNCHER_FAILED);
    ln_error("The sync thread could not be started.");
    return -1;
}

/* ---- Steam sign-in and sign-out ----------------------------------------------- */

static int signin_stopped(void) {
    pthread_mutex_lock(&g_ln_lock);
    int stop = g_ln_cancel || g_ln_quit;
    pthread_mutex_unlock(&g_ln_lock);
    return stop;
}

static void signin_failed(const char *fmt, ...) {
    pthread_mutex_lock(&g_ln_lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_ln.signin_error, sizeof g_ln.signin_error, fmt, ap);
    va_end(ap);
    if (g_ln.signin != ISAAC_SIGNIN_NONE) g_ln.signin = ISAAC_SIGNIN_FAILED;
    pthread_mutex_unlock(&g_ln_lock);
}

/* Puts the session's URL on screen. 0 when it fits in a code. */
static int signin_show_code(const char *url) {
    static uint8_t modules[ISAAC_QR_MAX_SIZE * ISAAC_QR_MAX_SIZE];
    int size = isaac_qr_encode(url, strlen(url), -1, modules);
    if (!size) return -1;
    pthread_mutex_lock(&g_ln_lock);
    memcpy(g_ln.qr, modules, sizeof modules);
    g_ln.qr_size = size;
    if (g_ln.signin != ISAAC_SIGNIN_NONE) g_ln.signin = ISAAC_SIGNIN_SCAN;
    pthread_mutex_unlock(&g_ln_lock);
    return 0;
}

/* Sessions until one is approved, the dialog closes or something fails. The
 * polls keep going through a brief network fault: only three in a row stop it. */
static void signin_run(void) {
    static isaac_steamauth_session session;
    static isaac_steamauth_result result;
    char err[160] = "";
    int misses = 0;
    while (!signin_stopped()) {
        if (isaac_steamauth_begin(isaac_steamcm_webapi, NULL, "Isaac on Switch (NRO)", &session,
                                  err, sizeof err) != 0) {
            stage("steam sign-in: %s", err);
            signin_failed("%s.", err);
            return;
        }
        stage("steam sign-in: code ready, polling every %d ms", session.interval_ms);
        if (signin_show_code(session.url) != 0) {
            signin_failed("Steam's code is too long to draw.");
            return;
        }
        for (;;) {
            for (int waited = 0; waited < session.interval_ms; waited += 100) {
                if (signin_stopped()) return;
                usleep(100000);
            }
            int state = isaac_steamauth_poll(isaac_steamcm_webapi, NULL, &session, &result, err, sizeof err);
            if (state == ISAAC_STEAMAUTH_ERROR) {
                stage("steam sign-in: %s", err);
                if (++misses < 3) continue;
                signin_failed("%s.", err);
                return;
            }
            misses = 0;
            if (state == ISAAC_STEAMAUTH_EXPIRED) break;
            if (state == ISAAC_STEAMAUTH_NEW_CODE && signin_show_code(session.url) != 0) {
                signin_failed("Steam's code is too long to draw.");
                return;
            }
            if (state != ISAAC_STEAMAUTH_APPROVED) continue;
            if (signin_stopped()) return;
            if (isaac_steamsync_write_token(TOKEN_PATH, result.steamid, result.token, err, sizeof err) != 0) {
                stage("steam sign-in: %s", err);
                signin_failed("Approved, but %s.", err);
                return;
            }
            isaac_steamsync_forget(ROOT "/saves");
            g_pulled = 0;
            stage("steam sign-in: signed in, steamid ...%07llu", (unsigned long long)(result.steamid % 10000000u));
            pthread_mutex_lock(&g_ln_lock);
            g_ln.signin = ISAAC_SIGNIN_NONE;
            g_ln_has_token = 1;
            g_ln_token_bad = 0;
            g_ln_start_pull = 1;
            pthread_mutex_unlock(&g_ln_lock);
            return;
        }
    }
}

static int clock_is_set(void) {
    return time(NULL) >= 1735689600;
}

static void *signin_thread(void *p) {
    (void)p;
    char err[160] = "";
    if (!clock_is_set()) {
        signin_failed("The console's clock is not set, and Steam's certificates are checked against it. "
                      "Set the date in System Settings.");
    } else if (R_FAILED(socketInitializeDefault())) {
        signin_failed("The network could not be started.");
    } else {
        if (isaac_steamcm_web_open(err, sizeof err) != 0) signin_failed("%s.", err);
        else signin_run();
        isaac_steamcm_web_close();
        socketExit();
    }
    pthread_mutex_lock(&g_ln_lock);
    g_ln_busy = 0;
    pthread_mutex_unlock(&g_ln_lock);
    return NULL;
}

static void signin_open(pthread_t *t, int *joinable) {
    if (*joinable) pthread_join(*t, NULL);
    pthread_mutex_lock(&g_ln_lock);
    g_ln_cancel = 0;
    g_ln.signin = ISAAC_SIGNIN_WAIT;
    g_ln.signin_error[0] = 0;
    g_ln.qr_size = 0;
    pthread_mutex_unlock(&g_ln_lock);
    *joinable = worker_start(t, signin_thread) == 0;
    if (!*joinable) signin_failed("The sign-in thread could not be started.");
}

/* Steam is told first, while the token still works; the console forgets it
 * either way, so signing out works without a network. */
static void *signout_thread(void *p) {
    (void)p;
    static char token[1024];
    char err[160] = "", when_failed[160] = "";
    uint64_t steamid = 0;
    int told = 0;
    if (isaac_steamsync_read_token(TOKEN_PATH, &steamid, token, sizeof token, err, sizeof err) != 0) {
        told = 1;                       /* nothing there that Steam would take */
    } else if (!clock_is_set()) {
        snprintf(when_failed, sizeof when_failed, "the console's clock is not set");
    } else if (R_FAILED(socketInitializeDefault())) {
        snprintf(when_failed, sizeof when_failed, "the network could not be started");
    } else {
        isaac_steamcm_set_log(sync_log, NULL);
        int rc = isaac_steamcm_revoke(steamid, token, 15000, err, sizeof err);
        socketExit();
        told = rc >= 0;
        stage("steam sign-out: revoke %s%s", rc == 0 ? "done" : rc == 1 ? "already refused" : "failed: ",
              rc < 0 ? err : "");
        if (rc < 0) snprintf(when_failed, sizeof when_failed, "%s", err);
    }
    memset(token, 0, sizeof token);
    int removed = remove(TOKEN_PATH) == 0;
    if (removed) {
        isaac_steamsync_forget(ROOT "/saves");
        g_pulled = 0;
    }
    stage("steam sign-out: token %s", removed ? "removed" : "NOT removed");
    ln_reset(removed ? ISAAC_LAUNCHER_SIGNED_OUT : ISAAC_LAUNCHER_FAILED);
    if (!removed)
        ln_error("Could not delete " TOKEN_PATH ": this console is still signed in.");
    else if (told)
        ln_error("Signed out. " SIGNED_OUT_TEXT);
    else
        ln_error("Signed out here, but Steam was not told (%.60s). Remove the device in Steam's security settings.",
                 when_failed);
    pthread_mutex_lock(&g_ln_lock);
    if (removed) {
        g_ln.account[0] = 0;
        g_ln_has_token = 0;
        g_ln_token_bad = 0;
    }
    g_ln.signout = ISAAC_SIGNOUT_NONE;
    g_ln_busy = 0;
    pthread_mutex_unlock(&g_ln_lock);
    return NULL;
}

#else /* ISAAC_NO_STEAM */
static void steam_sync_run(int push) {
    (void)push;
}
#endif

/* The screen until the player decides: 1 Play, 0 left (+ or HOME), -1 the
 * screen could not be set up (nothing ran: the caller syncs without it). */
static int launcher_run(void) {
    PlFontData font;
    Framebuffer fb;
    if (R_FAILED(plInitialize(PlServiceType_User))) {
        stage("launcher: pl:u unavailable");
        return -1;
    }
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) ||
        isaac_launcher_set_font(font.address, font.size) != 0) {
        stage("launcher: no shared font");
        plExit();
        return -1;
    }
    if (R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), ISAAC_LAUNCHER_W, ISAAC_LAUNCHER_H,
                                   PIXEL_FORMAT_RGBA_8888, 2))) {
        stage("launcher: framebufferCreate failed");
        isaac_launcher_release();
        plExit();
        return -1;
    }
    framebufferMakeLinear(&fb);
#ifdef ISAAC_NO_STEAM
    stage("launcher: shown (built without Steam)");
    g_ln.phase = ISAAC_LAUNCHER_OFF;
    static isaac_launcher_view view;
    uint64_t t0 = armGetSystemTick();
    int choice = -1, last_frames = 0;
    while (last_frames < 3) {
        if (choice < 0 && !appletMainLoop()) choice = 0;
        padUpdate(&g_pad);
        u64 down = choice < 0 ? padGetButtonsDown(&g_pad) : 0;
        if (down & HidNpadButton_Plus) {
            choice = 0;
        } else if ((down & HidNpadButton_A) && g_ln.files != ISAAC_FILES_BAD) {
            choice = 1;
            g_ln.phase = ISAAC_LAUNCHER_LAUNCHING;
        }
        g_ln.t = armTicksToNs(armGetSystemTick() - t0) / 1e9f;
        view = g_ln;
        u32 stride;
        uint32_t *px = framebufferBegin(&fb, &stride);
        isaac_launcher_draw(px, (int)(stride / 4), &view);
        framebufferEnd(&fb);
        if (choice >= 0) ++last_frames;
    }
#else
    stage("launcher: shown (steam_sync=%d)", g_steam_sync);
    g_ln_on = 1;
    g_ln_quit = 0;
    pthread_t thread;
    int joinable = 0;
    struct stat token_st;
    g_ln_has_token = stat(TOKEN_PATH, &token_st) == 0;
    g_ln_token_bad = 0;
    if (g_steam_sync && !g_ln_has_token) {
        ln_reset(ISAAC_LAUNCHER_SIGNED_OUT);
        ln_error(SIGNED_OUT_TEXT);
        signin_open(&thread, &joinable);
    } else if (g_steam_sync) {
        joinable = sync_thread_start(&thread) == 0;
    } else {
        ln_reset(ISAAC_LAUNCHER_OFF);
        ln_error("Steam Cloud sync is off (steam_sync=0 in settings.ini): your saves stay on this console.");
    }

    static isaac_launcher_view view;
    uint64_t t0 = armGetSystemTick();
    int choice = -1, last_frames = 0;
    while (last_frames < 3) {
        if (choice < 0 && !appletMainLoop()) choice = 0;
        padUpdate(&g_pad);
        u64 down = choice < 0 ? padGetButtonsDown(&g_pad) : 0;
        pthread_mutex_lock(&g_ln_lock);
        int idle = !g_ln_busy && !g_ln.conflict && g_ln.signin == ISAAC_SIGNIN_NONE &&
                   g_ln.signout == ISAAC_SIGNOUT_NONE && g_ln.phase != ISAAC_LAUNCHER_SYNCING &&
                   g_ln.phase != ISAAC_LAUNCHER_LAUNCHING && choice < 0;
        g_ln.account_action = !idle ? ISAAC_ACCOUNT_NONE
                              : g_ln_has_token && !g_ln_token_bad ? ISAAC_ACCOUNT_SIGN_OUT
                              : g_steam_sync ? ISAAC_ACCOUNT_SIGN_IN : ISAAC_ACCOUNT_NONE;
        if (g_ln_start_pull && !g_ln_busy && choice < 0) {
            g_ln_start_pull = 0;
            pthread_mutex_unlock(&g_ln_lock);
            if (joinable) pthread_join(thread, NULL);
            joinable = sync_thread_start(&thread) == 0;
            pthread_mutex_lock(&g_ln_lock);
        } else if (g_ln.signin != ISAAC_SIGNIN_NONE) {
            if (down & HidNpadButton_B) {
                g_ln_cancel = 1;
                g_ln.signin = ISAAC_SIGNIN_NONE;
                stage("launcher: sign-in cancelled");
            } else if (down & HidNpadButton_Plus) {
                choice = 0;
            } else if ((down & HidNpadButton_Y) && g_ln.signin == ISAAC_SIGNIN_FAILED && !g_ln_busy) {
                pthread_mutex_unlock(&g_ln_lock);
                signin_open(&thread, &joinable);
                pthread_mutex_lock(&g_ln_lock);
            }
        } else if (g_ln.signout != ISAAC_SIGNOUT_NONE) {
            if (g_ln.signout == ISAAC_SIGNOUT_ASK && (down & HidNpadButton_B)) {
                g_ln.signout = ISAAC_SIGNOUT_NONE;
            } else if (g_ln.signout == ISAAC_SIGNOUT_ASK && (down & HidNpadButton_A)) {
                g_ln.signout = ISAAC_SIGNOUT_BUSY;
                pthread_mutex_unlock(&g_ln_lock);
                if (joinable) pthread_join(thread, NULL);
                stage("launcher: sign out");
                joinable = worker_start(&thread, signout_thread) == 0;
                pthread_mutex_lock(&g_ln_lock);
                if (!joinable) g_ln.signout = ISAAC_SIGNOUT_NONE;
            }
        } else if (g_ln.conflict) {
            if (down & (HidNpadButton_AnyLeft | HidNpadButton_L)) g_ln.conflict_choice = 0;
            if (down & (HidNpadButton_AnyRight | HidNpadButton_R)) g_ln.conflict_choice = 1;
            if (down & HidNpadButton_A) {
                g_ln_answer = g_ln.conflict_choice;
                pthread_cond_broadcast(&g_ln_cond);
            }
        } else if (down & HidNpadButton_Plus) {
            choice = 0;
        } else if ((down & HidNpadButton_A) && g_ln.phase != ISAAC_LAUNCHER_SYNCING &&
                   g_ln.files != ISAAC_FILES_BAD) {
            choice = 1;
            g_ln.phase = ISAAC_LAUNCHER_LAUNCHING;
        } else if ((down & HidNpadButton_Y) && g_ln.phase == ISAAC_LAUNCHER_FAILED && !g_ln_busy) {
            pthread_mutex_unlock(&g_ln_lock);
            if (joinable) pthread_join(thread, NULL);
            stage("launcher: retry");
            joinable = sync_thread_start(&thread) == 0;
            pthread_mutex_lock(&g_ln_lock);
        } else if ((down & HidNpadButton_X) && g_ln.account_action == ISAAC_ACCOUNT_SIGN_OUT) {
            g_ln.signout = ISAAC_SIGNOUT_ASK;
        } else if ((down & HidNpadButton_X) && g_ln.account_action == ISAAC_ACCOUNT_SIGN_IN) {
            pthread_mutex_unlock(&g_ln_lock);
            stage("launcher: sign in");
            signin_open(&thread, &joinable);
            pthread_mutex_lock(&g_ln_lock);
        }
        if (choice == 0) {
            snprintf(g_ln.activity, sizeof g_ln.activity, "Exiting\xe2\x80\xa6");
            g_ln_quit = 1;
            pthread_cond_broadcast(&g_ln_cond);
        }
        g_ln.t = armTicksToNs(armGetSystemTick() - t0) / 1e9f;
        view = g_ln;
        pthread_mutex_unlock(&g_ln_lock);
        u32 stride;
        uint32_t *px = framebufferBegin(&fb, &stride);
        isaac_launcher_draw(px, (int)(stride / 4), &view);
        framebufferEnd(&fb);
        if (choice >= 0) ++last_frames;   /* Launching / Exiting stays up a moment */
    }
    /* Leaving mid-sync waits for the thread: it ends within the network timeout. */
    if (joinable) pthread_join(thread, NULL);
    g_ln_on = 0;
#endif
    framebufferClose(&fb);
    isaac_launcher_release();
    plExit();
    stage("launcher: %s", choice ? "play" : "exit without playing");
    return choice;
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
    isaac_native_prof_game_thread();
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
    int bench_from = 1500, bench_to = 2700, prof_hz = 0, vsync = 0, game_core = -1;
    const char *run_id = "manual", *bench = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--autotest")) automatic = 1;
        else if (!strcmp(argv[i], "--return-to-sphaira")) return_sphaira = 1;
        else if (!strcmp(argv[i], "--run-id") && i + 1 < argc) run_id = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--graphics")) { /* the runner's flag; this NRO always draws */ }
        else if (!strcmp(argv[i], "--mods")) mods = 1;
        else if (!strcmp(argv[i], "--no-script")) no_script = 1;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = argv[++i];
        else if (!strcmp(argv[i], "--bench-from") && i + 1 < argc) bench_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench-to") && i + 1 < argc) bench_to = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile-hz") && i + 1 < argc) prof_hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vsync")) vsync = 1;
        else if (!strcmp(argv[i], "--game-core") && i + 1 < argc) game_core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--env") && i + 1 < argc && strchr(argv[i + 1], '=')) putenv(argv[++i]);
        else { stage("bad argument %s", argv[i]); return 2; }
    }
    stage("start: %d argument(s), autotest=%d", argc, automatic);
    heap_report();
    mkdir(ROOT, 0777);
    load_settings();
    stage("settings: switch_glyphs=%d a_accepts=%d frame_skip=%d render_thread=%d launcher=%d check_game_files=%d",
          g_switch_glyphs, g_a_accepts, g_frame_skip, g_render_thread, g_launcher, g_check_files);
#ifndef ISAAC_NO_STEAM
    stage("settings: steam_sync=%d prompt=%d", g_steam_sync, g_sync_prompt);
#endif
    if (!g_render_thread) setenv("ISAAC_NATIVE_GLQ", "0", 0);
    setenv("ISAAC_NATIVE_SWITCH_GLYPHS", g_switch_glyphs ? (g_a_accepts ? "1" : "2") : "0", 1);
    if (automatic && (strlen(run_id) != 32 || strspn(run_id, "0123456789abcdef") != 32)) return 2;
    if (bench && (!automatic || bench_to <= bench_from || game_core > 2)) { stage("bad bench arguments"); return 2; }
    if (bench) frames = bench_to + 2;
    isaac_native_prof_config(bench ? prof_hz : 0, game_core);
    /* Playing: HOME -> Close asks instead of terminating, so the game quits
     * through its own path and the push uploads what was played. The system
     * waits 15 s after the request, then terminates anyway (an upload cut
     * short is never committed, and the next push sends it again). */
    if (!automatic) appletLockExit();

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
            /* A bench shot lands before the window: the read-back would stall it. */
            snprintf(shots, sizeof shots, "%d", bench ? bench_from - 50 : frames > 1000 ? 1000 : frames / 2);
            setenv("ISAAC_NATIVE_SHOTS", shots, 1);
            snprintf(shot_dir, sizeof shot_dir, ROOT "/shots-%s", run_id);
            mkdir(shot_dir, 0777);
            setenv("ISAAC_NATIVE_SHOT_DIR", shot_dir, 1);
        } else {
            setenv("ISAAC_SAVE_DIR", ROOT "/saves", 1);
            mods = 1;                    /* play mode: the instance's mods/ is mounted */
            setenv("ISAAC_NATIVE_WALL_CLOCK", "1", 1);   /* real time, as the web build plays */
            setenv("ISAAC_RENDER_GATE", g_frame_skip == 2 ? "4" : g_frame_skip == 1 ? "1" : "0", 0);
        }
        if (bench) {
            static char from[16], to[16];
            snprintf(from, sizeof from, "%d", bench_from);
            snprintf(to, sizeof to, "%d", bench_to);
            setenv("ISAAC_NATIVE_BENCH", bench, 1);
            setenv("ISAAC_NATIVE_BENCH_FROM", from, 1);
            setenv("ISAAC_NATIVE_BENCH_TO", to, 1);
            setenv("ISAAC_NATIVE_BENCH_MOD", "romfs:/isaac-bench", 1);
            if (vsync) setenv("ISAAC_NATIVE_VSYNC", "1", 1);
            mods = 0;                    /* the bench mod alone */
        }
        if (mods) setenv("ISAAC_NATIVE_MODS", "1", 1);
        int play = 1;
        if (!automatic) {
            check_game_files();
            /* the screen syncs and waits for Play; without it, sync and go */
            int shown = g_launcher ? launcher_run() : -1;
            if (shown < 0) steam_sync_run(0);
            else play = shown;
        }
        if (!play) {
            status = "EXIT";
        } else {
            stage("running the game");
            int rc = run_game();
            if (g_exit_tick) stage("game closed %u ms after HOME -> Close", since_exit_request());
            if (!automatic) steam_sync_run(1);
            if (g_exit_tick) stage("push over %u ms after HOME -> Close", since_exit_request());
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
    if (g_exit_tick) stage("teardown done %u ms after HOME -> Close", since_exit_request());
    else stage("teardown done");
    if (automatic) {
        dup2(saved_stderr, STDERR_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stderr);
        close(saved_stdout);
        close(nx_socket);
        socketExit();
    }
    if (!automatic) appletUnlockExit();
    return !strcmp(status, "PASS") ? 0 : 1;
}
#endif
