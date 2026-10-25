/* Native driver for the browsing-of-isaac lift: the C counterpart of
 * scripts/recomp/lift/boot_integration.mjs (node profile) and play.mjs's
 * seeding, without a JS engine.
 *
 *   isaac_native <isaac.segs.bin> <instance-dir> [layout|boot|main]
 *
 * The instance dir is the bundle the web build ships (resources/packed/*.a,
 * resources/scripts, savedatapath.txt). Every file is registered lazily in
 * the host RAM-FS and read from disk on first open; large archives are read
 * through 1 MiB windows. The run's frame budget comes from the host's own
 * ISAAC_MAX_FRAMES / ISAAC_EXIT_AFTER. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int isaac_native_mem_init(void);
int isaac_native_input_init(void);
unsigned isaac_native_input_delivered(void);
size_t isaac_native_input_pending(void);

int isaac_place_image(const uint8_t *blob, uint32_t len);
int isaac_layout_check(void);
void isaac_guard_arm(void);
uint32_t isaac_guard_check(void);
int isaac_run_boot(int verbose);
int isaac_run_main(void);

int isaac_fs_seed(const char *path, const uint8_t *data, uint32_t len);
int isaac_fs_seed_lazy(const char *path, uint32_t len);
typedef int (*isaac_fs_lazy_reader)(const char *src, uint8_t *dst, uint32_t len);
typedef int (*isaac_fs_lazy_preader)(const char *src, uint8_t *dst, uint32_t off, uint32_t len);
void isaac_fs_set_lazy_reader(isaac_fs_lazy_reader fn);
void isaac_fs_set_lazy_preader(isaac_fs_lazy_preader fn);

void isaac_stub_report(void) __attribute__((weak));
void isaac_heap_report(void) __attribute__((weak));
void isaac_module_report(void) __attribute__((weak));
void isaac_native_audio_report(void) __attribute__((weak));
void isaac_native_gl_shutdown(void) __attribute__((weak));

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* The audio model (host_audio.c) retires stream buffers on this clock. Its
 * non-Emscripten default is a constant 0 for the selftest, which would keep
 * every queued buffer unprocessed and starve the music stream. */
double isaac_audio_clock_ms(void) { return now_ms(); }

/* ---- lazy file reads ---------------------------------------------------- */

static uint64_t g_reads, g_read_bytes, g_preads, g_pread_bytes;

static int lazy_read(const char *src, uint8_t *dst, uint32_t len) {
    int fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[native] lazy read of %s failed: %s\n", src, strerror(errno));
        return 0;
    }
    uint32_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, dst + got, len - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);
    if (got < len) {
        fprintf(stderr, "[native] lazy read of %s: %u of %u bytes\n", src, got, len);
        return 0;
    }
    ++g_reads;
    g_read_bytes += len;
    return 1;
}

#define MAX_FDS 32
static struct { char *src; int fd; } g_fds[MAX_FDS];

static int lazy_fd(const char *src) {
    for (int i = 0; i < MAX_FDS; ++i)
        if (g_fds[i].src && strcmp(g_fds[i].src, src) == 0) return g_fds[i].fd;
    int fd = open(src, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (!g_fds[i].src) {
            g_fds[i].src = strdup(src);
            g_fds[i].fd = fd;
            return fd;
        }
    }
    return fd;   /* table full: still usable, never cached */
}

static int lazy_pread(const char *src, uint8_t *dst, uint32_t off, uint32_t len) {
    int fd = lazy_fd(src);
    if (fd < 0) {
        fprintf(stderr, "[native] lazy pread of %s failed: %s\n", src, strerror(errno));
        return -1;
    }
#ifdef __SWITCH__
    /* newlib has no pread; the descriptor is only ever read from here. */
    ssize_t n = lseek(fd, (off_t)off, SEEK_SET) == (off_t)off ? read(fd, dst, len) : -1;
#else
    ssize_t n = pread(fd, dst, len, off);
#endif
    if (n < 0) return -1;
    ++g_preads;
    g_pread_bytes += (uint64_t)n;
    return (int)n;
}

/* ---- instance seeding --------------------------------------------------- */

static unsigned g_seeded, g_seed_failed;
static uint64_t g_seed_bytes;

static void seed_tree(const char *rel) {
    DIR *d = opendir(*rel ? rel : ".");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(path, "page-assets") == 0) continue;   /* web page art only */
            if (strcmp(path, "mods") == 0) continue;          /* seed_mods decides what is mounted */
            seed_tree(path);
        } else if (S_ISREG(st.st_mode)) {
            if (isaac_fs_seed_lazy(path, (uint32_t)st.st_size)) {
                ++g_seeded;
                g_seed_bytes += (uint64_t)st.st_size;
            } else {
                ++g_seed_failed;
                fprintf(stderr, "[native] FAIL seeding %s\n", path);
            }
        }
    }
    closedir(d);
}

/* ISAAC_NATIVE_INSTANCE_LIST=<instance_index.json>: seed exactly the files
 * the web distribution mounts ({"p":"<path>","s":<size>} rows) instead of
 * the whole directory. A full Steam resources/ tree also holds the language
 * packs, which would shadow English assets once mounted. Listed files that
 * are absent are skipped; the size on disk wins over the listed one. */
static int seed_list(const char *list_path) {
    uint32_t len = 0;
    FILE *f = fopen(list_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) { text[n] = 0; len = (uint32_t)n; }
    fclose(f);
    if (!len) { free(text); return -1; }
    unsigned missing = 0;
    for (char *p = text; (p = strstr(p, "\"p\":\"")); ) {
        p += 5;
        char *end = strchr(p, '"');
        if (!end) break;
        *end = 0;
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            if (isaac_fs_seed_lazy(p, (uint32_t)st.st_size)) {
                ++g_seeded;
                g_seed_bytes += (uint64_t)st.st_size;
            } else {
                ++g_seed_failed;
            }
        } else {
            ++missing;
        }
        p = end + 1;
    }
    free(text);
    if (missing) printf("[native] instance list: %u listed file(s) absent, skipped\n", missing);
    return 0;
}

/* ISAAC_NATIVE_MODS=1: the instance's mods/ folder, registered the way the
 * web build's mods.mjs seeds its store. Each enabled mod is seeded under both
 * roots the engine reads -- c:/isaac/mods/ (the resource layer) and the
 * Documents mods/ (the mod manager) -- and a mod with disable.it is left out,
 * because the engine does not honour that file when it lists mods again. */
static const char *MOD_ROOTS[] = {
    "c:/isaac/mods/",
    "c:/isaac/documents/my games/binding of isaac repentance+/mods/",
};
static unsigned g_mods, g_mod_files, g_mod_refused, g_mods_off;
static uint64_t g_mod_bytes;
static uint8_t *read_file(const char *path, uint32_t *len);

static void seed_mod_dir(const char *disk, const char *rel) {
    DIR *d = opendir(disk);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024], r[1024];
        snprintf(path, sizeof path, "%s/%s", disk, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { seed_mod_dir(path, r); continue; }
        /* A mounted mod is an enabled one: its on/off state is decided in
         * seed_mods, and a disable.it the engine saw would flip it again. */
        if (!strchr(rel, '/') && !strcmp(e->d_name, "disable.it")) continue;
        uint32_t len = 0;
        uint8_t *buf = st.st_size ? read_file(path, &len) : NULL;
        if (st.st_size && !buf) { ++g_mod_refused; continue; }
        for (size_t i = 0; i < sizeof MOD_ROOTS / sizeof MOD_ROOTS[0]; ++i) {
            char key[1400];
            snprintf(key, sizeof key, "%s%s", MOD_ROOTS[i], r);
            if (isaac_fs_seed(key, buf, st.st_size ? len : 0)) ++g_mod_files;
            else ++g_mod_refused;   /* the engine's file table is full */
        }
        g_mod_bytes += (uint64_t)st.st_size;
        free(buf);
    }
    closedir(d);
}

/* The on/off choice made in the game's mods menu. The engine writes
 * disable.it into c:/isaac/mods/<folder>/ to turn a mod off and deletes it to
 * turn it back on; the choice applies at the next start. The instance's mods/
 * is read-only here, so the choice is kept in the save store, as
 * <ISAAC_SAVE_DIR>/mod-state/<folder>.off or .on, and wins over the
 * instance's own disable.it. */
static char g_save_dir[PATH_MAX];
/* 1: a disable.it under the game's own mods root -- the player's toggle,
 * with the mod's folder in `folder`. 2: one under the Documents root, which
 * the engine deletes on its own at every start while it syncs the two roots
 * (measured); that is not a choice and is ignored. 0: anything else. */
static int mod_toggle_folder(const char *key, char *folder, size_t cap) {
    for (size_t i = 0; i < 2; ++i) {
        size_t n = strlen(MOD_ROOTS[i]);
        if (strncmp(key, MOD_ROOTS[i], n) != 0) continue;
        const char *rest = key + n, *slash = strchr(rest, '/');
        if (!slash || strcmp(slash, "/disable.it") != 0 || (size_t)(slash - rest) >= cap) return 0;
        memcpy(folder, rest, (size_t)(slash - rest));
        folder[slash - rest] = 0;
        return i == 0 ? 1 : 2;
    }
    return 0;
}
static int mod_state(const char *folder) {        /* -1 none, 0 off, 1 on */
    if (!g_save_dir[0]) return -1;
    char p[PATH_MAX + 300];
    struct stat st;
    snprintf(p, sizeof p, "%s/mod-state/%s.off", g_save_dir, folder);
    if (stat(p, &st) == 0) return 0;
    snprintf(p, sizeof p, "%s/mod-state/%s.on", g_save_dir, folder);
    if (stat(p, &st) == 0) return 1;
    return -1;
}
static void mod_state_set(const char *folder, int on) {
    char dir[PATH_MAX + 20], p[PATH_MAX + 300];
    snprintf(dir, sizeof dir, "%s/mod-state", g_save_dir);
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/%s.%s", dir, folder, on ? "off" : "on");
    remove(p);
    snprintf(p, sizeof p, "%s/%s.%s", dir, folder, on ? "on" : "off");
    FILE *f = fopen(p, "wb");
    if (f) fclose(f);
    printf("[native] mods menu: '%s' turned %s (applies at the next start)\n", folder, on ? "on" : "off");
}

static void seed_mods(void) {
    DIR *d = opendir("mods");
    if (!d) { printf("[native] ISAAC_NATIVE_MODS: no mods/ in the instance\n"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[1024], off[1100];
        snprintf(dir, sizeof dir, "mods/%s", e->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(off, sizeof off, "%s/disable.it", dir);
        int on = stat(off, &st) != 0;
        int chosen = mod_state(e->d_name);
        if (chosen >= 0) on = chosen;
        if (!on) {
            /* Listed, greyed and empty: its metadata and a disable.it, so the
             * menu shows it off and can turn it back on; none of its content
             * is mounted, since the engine would load a listed mod's files. */
            static const char *keep[] = { "metadata.xml", "thumbnail.png" };
            for (size_t k = 0; k < sizeof keep / sizeof keep[0]; ++k) {
                char src[1100];
                uint32_t len = 0;
                snprintf(src, sizeof src, "%s/%s", dir, keep[k]);
                uint8_t *buf = read_file(src, &len);
                if (!buf) continue;
                for (size_t i = 0; i < sizeof MOD_ROOTS / sizeof MOD_ROOTS[0]; ++i) {
                    char key[1400];
                    snprintf(key, sizeof key, "%s%s/%s", MOD_ROOTS[i], e->d_name, keep[k]);
                    isaac_fs_seed(key, buf, len);
                }
                free(buf);
            }
            for (size_t i = 0; i < sizeof MOD_ROOTS / sizeof MOD_ROOTS[0]; ++i) {
                char key[1400];
                snprintf(key, sizeof key, "%s%s/disable.it", MOD_ROOTS[i], e->d_name);
                isaac_fs_seed(key, NULL, 0);
            }
            ++g_mods_off;
            continue;
        }
        seed_mod_dir(dir, e->d_name);
        ++g_mods;
    }
    closedir(d);
    printf("[native] mods: %u enabled (%u off), %u file entries, %.1f MB%s\n", g_mods, g_mods_off,
           g_mod_files, (double)g_mod_bytes / 1048576.0,
           g_mod_refused ? ", some REFUSED (file table full?)" : "");
}

/* play.mjs DEFAULT_OPTIONS: the beta notice and data-collection prompt are
 * options.ini flags, accepted here as the web build does on a first visit. */
static const char OPTIONS_KEY[] =
    "c:/isaac/documents/my games/binding of isaac repentance+/options.ini";
static const char DEFAULT_OPTIONS[] =
    "[Options]\r\nLanguage=0\r\nMusicVolume=0.7000\r\nMusicEnabled=1\r\n"
    "SFXVolume=0.7000\r\nMapOpacity=0.3000\r\nFullscreen=0\r\nFilter=0\r\nExposure=1.0000\r\n"
    "Gamma=1.0000\r\nControllerHotplug=1\r\nPopUps=1\r\nCameraStyle=1\r\nShowRecentItems=0\r\n"
    "HudOffset=1.0000\r\nTryImportSave=0\r\nFoundHUD=0\r\nEnableMods=1\r\nRumbleEnabled=1\r\n"
    "ChargeBars=0\r\nBulletVisibility=0\r\nTouchMode=1\r\nAimLock=1\r\nJacobEsauControls=0\r\n"
    "AscentVoiceOver=1\r\nOnlineHud=0\r\nStreamerMode=0\r\nOnlinePlayerVolume=6\r\n"
    "OnlinePlayerOpacity=10\r\nOnlineChatEnabled=1\r\nOnlineChatFilterEnabled=1\r\n"
    "MultiplayerColorSet=0\r\nOnlineInputDelay=3\r\nItemInfoDisplayEnabled=0\r\n"
    "AcceptedPublicBeta_v1.9.7.17=1\r\nAcceptedDataCollectionDisclaimer=1\r\n"
    "EnableDebugConsole=0\r\nMaxScale=99\r\nMaxRenderScale=2\r\nVSync=1\r\nPauseOnFocusLost=1\r\n"
    "SteamCloud=0\r\nMouseControl=0\r\nBossHpOnBottom=1\r\nAnnouncerVoiceMode=0\r\nConsoleFont=0\r\n"
    "FadedConsoleDisplay=0\r\nSaveCommandHistory=1\r\nWindowWidth=960\r\nWindowHeight=540\r\n"
    "WindowPosX=8\r\nWindowPosY=32\r\nUseExclusiveFullscreen=0\r\nEnableEpicOverlay=0\r\n"
    "EosCrossplay=0\r\n";

/* ---- save persistence (ISAAC_SAVE_DIR) -------------------------------- */
/* The node driver's convention: a file the game writes is handed over when
 * closed, under its RAM-FS key; the key of a created file carries the fake
 * profile root "c:/isaac/", which the store drops. The store is seeded back
 * over the instance at boot. Unset, nothing persists. */

typedef int (*isaac_fs_persist_fn)(const char *key, const char *src, const uint8_t *data, uint32_t len);
typedef int (*isaac_fs_unlink_fn)(const char *key, const char *src);
void isaac_fs_set_persist_hooks(isaac_fs_persist_fn p, isaac_fs_unlink_fn u);

static unsigned g_persisted, g_unlinked, g_restored;

static const char *store_rel(const char *key, const char *src) {
    if (src && *src) return src;
    return strncmp(key, "c:/isaac/", 9) == 0 ? key + 9 : key;
}

static int store_path(const char *rel, char *out, size_t cap) {
    if (strstr(rel, "..") || rel[0] == '/' || strchr(rel, ':')) return 0;
    return snprintf(out, cap, "%s/%s", g_save_dir, rel) < (int)cap;
}

static void mkdirs_for(char *path) {
    for (char *p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
}

static int persist(const char *key, const char *src, const uint8_t *data, uint32_t len) {
    char path[1200], folder[256];
    int toggle = mod_toggle_folder(key, folder, sizeof folder);
    if (toggle) { if (toggle == 1) mod_state_set(folder, 0); return 1; }
    if (!store_path(store_rel(key, src), path, sizeof path)) return 0;
    mkdirs_for(path);
    char tmp[1210];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[native] save write FAILED for %s: %s\n", key, strerror(errno)); return 0; }
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
#ifdef __SWITCH__
    if (ok) remove(path);                      /* the SD's rename does not replace a file */
#endif
    if (ok) ok = rename(tmp, path) == 0;        /* a crash never leaves half a save */
    if (ok) ++g_persisted;
    return ok;
}

static int unlink_saved(const char *key, const char *src) {
    char path[1200], folder[256];
    int toggle = mod_toggle_folder(key, folder, sizeof folder);
    if (toggle) { if (toggle == 1) mod_state_set(folder, 1); return 1; }
    if (!store_path(store_rel(key, src), path, sizeof path)) return 0;
    unlink(path);
    ++g_unlinked;
    return 1;
}

static void restore_tree(const char *rel) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s%s%s", g_save_dir, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".tmp") || !strcmp(e->d_name, "disable.it")) continue;
        /* The engine rewrites every mod's metadata.xml at start; restoring
         * them would list mods that are no longer installed. */
        if (!strncmp(rel, "mods/", 5) && !strcmp(e->d_name, "metadata.xml")) continue;
        char r[1024], abs[1300];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(abs, sizeof abs, "%s/%s", g_save_dir, r);
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { restore_tree(r); continue; }
        uint32_t len = 0;
        uint8_t *buf = read_file(abs, &len);
        if (!buf && st.st_size) continue;
        if (isaac_fs_seed(r, buf, st.st_size ? len : 0)) ++g_restored;
        free(buf);
    }
    closedir(d);
}

/* ---- main --------------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (uint32_t)n;
    return buf;
}

/* The run itself; the platform entry (main() below, or the Switch NRO's)
 * supplies the arguments. */
int isaac_native_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <isaac.segs.bin> <instance-dir> [layout|boot|main]\n", argv[0]);
        return 2;
    }
    const char *stage = argc > 3 ? argv[3] : "main";
    setvbuf(stdout, NULL, _IOLBF, 0);
    double t0 = now_ms();

    if (isaac_native_mem_init() != 0) return 2;

    uint32_t blen = 0;
    uint8_t *blob = read_file(argv[1], &blen);
    if (!blob) { fprintf(stderr, "[native] cannot read %s\n", argv[1]); return 2; }
    int nseg = isaac_place_image(blob, blen);
    free(blob);
    printf("[native] isaac.segs.bin %u bytes -> %d segments\n", blen, nseg);
    if (nseg < 0) return 2;

    int layout_bad = isaac_layout_check();
    isaac_guard_arm();
    if (strcmp(stage, "layout") == 0) return layout_bad ? 1 : 0;

    /* The save store is resolved before entering the instance dir, so a
     * relative ISAAC_SAVE_DIR means the caller's directory. */
    const char *save_dir = getenv("ISAAC_SAVE_DIR");
    if (save_dir && *save_dir) {
        mkdir(save_dir, 0755);
        if (!realpath(save_dir, g_save_dir)) {
            fprintf(stderr, "[native] ISAAC_SAVE_DIR %s: %s\n", save_dir, strerror(errno));
            return 2;
        }
    }
    if (chdir(argv[2]) != 0) {
        fprintf(stderr, "[native] cannot enter instance dir %s: %s\n", argv[2], strerror(errno));
        return 2;
    }
    isaac_fs_set_lazy_reader(lazy_read);
    isaac_fs_set_lazy_preader(lazy_pread);
    const char *list = getenv("ISAAC_NATIVE_INSTANCE_LIST");   /* an absolute path */
    if (list && *list) {
        if (seed_list(list) != 0) {
            fprintf(stderr, "[native] cannot read instance list %s\n", list);
            return 2;
        }
    } else {
        seed_tree("");
    }
    printf("[native] registered %u instance files lazily (%.1f MB)%s\n", g_seeded,
           (double)g_seed_bytes / 1048576.0, g_seed_failed ? ", some FAILED" : "");
    const char *mods = getenv("ISAAC_NATIVE_MODS");
    if (mods && *mods == '1') seed_mods();
    if (!getenv("ISAAC_NATIVE_NO_OPTIONS"))
        printf("[native] seed options.ini -> %s\n",
               isaac_fs_seed(OPTIONS_KEY, (const uint8_t *)DEFAULT_OPTIONS,
                             (uint32_t)(sizeof DEFAULT_OPTIONS - 1)) ? "ok" : "FAIL");

    if (g_save_dir[0]) {
        isaac_fs_set_persist_hooks(persist, unlink_saved);
        restore_tree("");
        printf("[native] ISAAC_SAVE_DIR=%s: %u saved file(s) restored\n", g_save_dir, g_restored);
    }

    /* The host's QueryPerformanceCounter is deterministic by default (100 ns
     * per query, Sleep adds its argument): reproducible, and what the tests
     * use. Anything that times itself -- the intro cutscene's animation
     * against its audio -- then runs at the speed of the queries. The web
     * build switches to the wall clock in interactive play; so does this, with
     * ISAAC_NATIVE_WALL_CLOCK=1 (the PC window and the Switch play mode set it). */
    const char *wall_clock = getenv("ISAAC_NATIVE_WALL_CLOCK");
    if (!wall_clock) wall_clock = getenv("ISAAC_NATIVE_WINDOW");   /* the PC window plays in real time */
    if (wall_clock && *wall_clock == '1') {
        extern void isaac_time_set_mode(int mode);
        isaac_time_set_mode(1);
    }

    int scripted = isaac_native_input_init();
    if (scripted) printf("[native] ISAAC_INPUT: %d scripted events\n", scripted);

    int boot_rc = isaac_run_boot(1);
    uint32_t g = isaac_guard_check();
    printf("[native] isaac_boot_init -> %d, guard %s (%.0f ms)\n", boot_rc,
           g ? "CORRUPTED" : "intact", now_ms() - t0);
    if (strcmp(stage, "boot") == 0) return boot_rc == 1 ? 0 : 1;

    double tm = now_ms();
    int main_rc = isaac_run_main();
    double wall = now_ms() - tm;
    printf("[native] main returned %d after %.1f s\n", main_rc, wall / 1000.0);
    printf("[native] lazy reads: %llu files, %.1f MB; windowed reads: %llu, %.1f MB\n",
           (unsigned long long)g_reads, (double)g_read_bytes / 1048576.0,
           (unsigned long long)g_preads, (double)g_pread_bytes / 1048576.0);
    if (g_save_dir[0])
        printf("[native] saves: %u file(s) persisted, %u unlinked\n", g_persisted, g_unlinked);
    if (scripted)
        printf("[native] scripted input: %u delivered, %zu pending\n",
               isaac_native_input_delivered(), isaac_native_input_pending());
    g = isaac_guard_check();
    printf("[native] guard after main: %s\n", g ? "CORRUPTED" : "intact");
    if (isaac_stub_report) isaac_stub_report();
    if (isaac_heap_report) isaac_heap_report();
    if (isaac_module_report) isaac_module_report();
    if (isaac_native_audio_report) isaac_native_audio_report();
#ifndef __SWITCH__
    if (isaac_native_gl_shutdown) isaac_native_gl_shutdown();    /* closes the PC window */
#endif
    return main_rc;
}

#ifndef __SWITCH__
int main(int argc, char **argv) { return isaac_native_main(argc, argv); }
#endif
