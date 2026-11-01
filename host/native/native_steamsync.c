/* The save sync state machine. See native_steamsync.h for the rules. */
#include "native_steamsync.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>

#include "native_sha1.h"

#define APPID 250900u
#define STATE_FILE "steam-sync-state.txt"
#define PULL_MARKER "steam-sync-pull.txt"
#define PULL_MARKER_MAX_AGE (12 * 3600)
#define DOCS_SUBDIR "documents/my games/binding of isaac repentance+"

static const struct {
    const char *cloud;
    const char *local;
} SLOTS[ISAAC_STEAMSYNC_SLOTS] = {
    { "rep+persistentgamedata1.dat", "persistentgamedata1.dat" },
    { "rep+persistentgamedata2.dat", "persistentgamedata2.dat" },
    { "rep+persistentgamedata3.dat", "persistentgamedata3.dat" },
    { "rep+gamestate1.dat", "gamestate1.dat" },
    { "rep+gamestate2.dat", "gamestate2.dat" },
    { "rep+gamestate3.dat", "gamestate3.dat" },
};

/* What the last sync left on each side. `pending` means the console's copy won
 * a conflict, so the next push owes Steam an upload even if the game did not
 * touch the file. */
typedef struct {
    char local_sha[41];
    char cloud_sha[41];
    uint64_t cloud_ts;
    int pending;
} sync_state;

static sync_state g_state[ISAAC_STEAMSYNC_SLOTS];
static uint8_t g_raw[ISAAC_STEAMCM_MAX_FILE];

static isaac_steamsync_ask_fn g_ask;
static void *g_ask_user;
static void (*g_log)(void *user, const char *line);
static void *g_log_user;
static void (*g_slot_fn)(void *user, int slot, int status);
static void *g_slot_user;

void isaac_steamsync_set_ask(isaac_steamsync_ask_fn fn, void *user) {
    g_ask = fn;
    g_ask_user = user;
}

void isaac_steamsync_set_log(void (*fn)(void *user, const char *line), void *user) {
    g_log = fn;
    g_log_user = user;
}

void isaac_steamsync_set_slot_fn(void (*fn)(void *user, int slot, int status), void *user) {
    g_slot_fn = fn;
    g_slot_user = user;
}

static void slot_status(int slot, int status) {
    if (g_slot_fn) g_slot_fn(g_slot_user, slot, status);
}

const char *isaac_steamsync_cloud_name(int slot) {
    return (slot >= 0 && slot < ISAAC_STEAMSYNC_SLOTS) ? SLOTS[slot].cloud : "";
}

const char *isaac_steamsync_local_name(int slot) {
    return (slot >= 0 && slot < ISAAC_STEAMSYNC_SLOTS) ? SLOTS[slot].local : "";
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log) g_log(g_log_user, line);
}

/* ---- paths and files ------------------------------------------------------ */

static int join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int save_path(const isaac_steamsync_cfg *cfg, int slot, char *out, size_t cap) {
    char base[512];
    if (join(base, sizeof base, cfg->save_dir, DOCS_SUBDIR) != 0) return -1;
    return join(out, cap, base, SLOTS[slot].local);
}

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    /* Every component is tried and its error ignored: on the Switch the
     * device prefix ("sdmc:") is one, and mkdir fails on it with an errno
     * other than EEXIST. What counts is that the whole path is a folder. */
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        mkdir(tmp, 0777);
        tmp[i] = '/';
    }
    mkdir(tmp, 0777);
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int mkdir_parent(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = 0;
    return mkdir_p(tmp);
}

static int read_file(const char *path, void *buf, size_t cap, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int short_read = !feof(f);
    fclose(f);
    if (short_read) return -1;
    *len = n;
    return 0;
}

static int write_file(const char *path, const void *data, size_t len) {
    if (mkdir_parent(path) != 0) return -1;
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) == 0) return 0;
    /* Horizon's file system does not replace an existing file on rename:
     * move the old one aside, put the new one in place, and put the old one
     * back if that fails. */
    char old[512];
    if (snprintf(old, sizeof old, "%s.old", path) >= (int)sizeof old) {
        remove(tmp);
        return -1;
    }
    remove(old);
    if (rename(path, old) != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        rename(old, path);
        remove(tmp);
        return -1;
    }
    remove(old);
    return 0;
}

static void set_mtime(const char *path, int64_t when) {
    struct utimbuf times;
    times.actime = (time_t)when;
    times.modtime = (time_t)when;
    utime(path, &times);
}

/* 0 when the file exists; fills the digest and the mtime. */
static int file_digest(const char *path, char hex[41], int64_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    size_t len = 0;
    if (read_file(path, g_raw, sizeof g_raw, &len) != 0) return -1;
    uint8_t digest[ISAAC_SHA1_DIGEST];
    isaac_sha1_buf(g_raw, len, digest);
    isaac_sha1_hex(digest, hex);
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
}

static void describe_time(int64_t when, char *out, size_t cap) {
    time_t t = (time_t)when;
    struct tm tm;
    if (!when || !localtime_r(&t, &tm)) {
        snprintf(out, cap, "unknown");
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M", &tm);
}

/* ---- state file ----------------------------------------------------------- */

static void state_path(const isaac_steamsync_cfg *cfg, char *out, size_t cap) {
    join(out, cap, cfg->save_dir, STATE_FILE);
}

static void marker_path(const isaac_steamsync_cfg *cfg, char *out, size_t cap) {
    join(out, cap, cfg->save_dir, PULL_MARKER);
}

/* The console must never push saves it could not compare first: a pull that
 * worked leaves this marker, and push refuses to run without a recent one. */
static void marker_write(const isaac_steamsync_cfg *cfg, int64_t now) {
    char path[512], text[32];
    marker_path(cfg, path, sizeof path);
    snprintf(text, sizeof text, "%lld\n", (long long)now);
    if (write_file(path, text, strlen(text)) != 0) say("steam sync: cannot write %s", path);
}

static int marker_age(const isaac_steamsync_cfg *cfg, int64_t now) {
    char path[512], text[32];
    marker_path(cfg, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int64_t when = 0;
    if (fgets(text, sizeof text, f)) when = strtoll(text, NULL, 10);
    fclose(f);
    if (when <= 0) return -1;
    return (int)(now - when);
}

static void state_reset(void) {
    memset(g_state, 0, sizeof g_state);
    for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) {
        snprintf(g_state[i].local_sha, sizeof g_state[i].local_sha, "-");
        snprintf(g_state[i].cloud_sha, sizeof g_state[i].cloud_sha, "-");
    }
}

static void state_read(const isaac_steamsync_cfg *cfg) {
    state_reset();
    char path[512], line[256];
    state_path(cfg, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char name[64], local[48], cloud[48], pending[8] = "";
        unsigned long long ts = 0;
        if (line[0] == '#') continue;
        int fields = sscanf(line, "%63s %47s %47s %llu %7s", name, local, cloud, &ts, pending);
        if (fields < 4) continue;
        for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) {
            if (strcmp(name, SLOTS[i].local) != 0) continue;
            snprintf(g_state[i].local_sha, sizeof g_state[i].local_sha, "%s", local);
            snprintf(g_state[i].cloud_sha, sizeof g_state[i].cloud_sha, "%s", cloud);
            g_state[i].cloud_ts = ts;
            g_state[i].pending = (fields >= 5 && pending[0] == '1');
        }
    }
    fclose(f);
}

static void state_write(const isaac_steamsync_cfg *cfg) {
    char path[512], text[1024];
    size_t used = 0;
    state_path(cfg, path, sizeof path);
    used += (size_t)snprintf(text + used, sizeof text - used,
                             "# isaac switch steam sync v1: local_sha cloud_sha cloud_ts pending\n");
    for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) {
        used += (size_t)snprintf(text + used, sizeof text - used, "%s %s %s %llu %d\n",
                                 SLOTS[i].local,
                                 g_state[i].local_sha[0] ? g_state[i].local_sha : "-",
                                 g_state[i].cloud_sha[0] ? g_state[i].cloud_sha : "-",
                                 (unsigned long long)g_state[i].cloud_ts,
                                 g_state[i].pending);
        if (used >= sizeof text) break;
    }
    if (write_file(path, text, used) != 0) say("steam sync: cannot write %s", path);
}

/* ---- backups -------------------------------------------------------------- */

static void backup_slot(const isaac_steamsync_cfg *cfg, int slot, int64_t now, const char *why) {
    char path[512], dir[512], dest[512], stamp[32];
    if (save_path(cfg, slot, path, sizeof path) != 0) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    time_t t = (time_t)now;
    struct tm tm;
    if (localtime_r(&t, &tm)) strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    else snprintf(stamp, sizeof stamp, "%lld", (long long)now);
    snprintf(dir, sizeof dir, "%s/save_backups/steam-%s", cfg->save_dir, stamp);
    if (mkdir_p(dir) != 0 || join(dest, sizeof dest, dir, SLOTS[slot].local) != 0) {
        say("steam sync: cannot back up %s", SLOTS[slot].local);
        return;
    }
    size_t len = 0;
    if (read_file(path, g_raw, sizeof g_raw, &len) == 0 && write_file(dest, g_raw, len) == 0)
        say("steam sync: backed up %s (%s) to %s", SLOTS[slot].local, why, dir);
    else
        say("steam sync: backup of %s failed", SLOTS[slot].local);
}

/* ---- the two directions --------------------------------------------------- */

static const isaac_cloud_file *find_cloud(const isaac_cloud_file *files, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(files[i].name, name)) return &files[i];
    return NULL;
}

static int pull_download(const isaac_steamsync_cfg *cfg, const isaac_steam_transport *t, int slot,
                         const isaac_cloud_file *cloud, int64_t now) {
    char path[512];
    if (save_path(cfg, slot, path, sizeof path) != 0) return -1;
    /* Back up first: g_raw is the transport's buffer too, and the console copy
     * has to survive a download that then fails. */
    backup_slot(cfg, slot, now, "replaced by the cloud copy");
    char err[160] = "";
    size_t len = 0;
    char sha[41] = "";
    uint64_t ts = cloud->timestamp;
    if (t->download(t->state, APPID, cloud->name, g_raw, sizeof g_raw, &len, sha, &ts,
                    err, sizeof err) != 0) {
        say("steam sync: %s: download failed: %s", cloud->name, err);
        return -1;
    }
    if (write_file(path, g_raw, len) != 0) {
        say("steam sync: %s: cannot write %s", cloud->name, path);
        return -1;
    }
    /* The cloud timestamp is the meaningful one for the next comparison. */
    set_mtime(path, (int64_t)(ts ? ts : cloud->timestamp));
    snprintf(g_state[slot].local_sha, sizeof g_state[slot].local_sha, "%s", sha);
    snprintf(g_state[slot].cloud_sha, sizeof g_state[slot].cloud_sha, "%s", sha);
    g_state[slot].cloud_ts = ts;
    g_state[slot].pending = 0;
    char when[32];
    describe_time((int64_t)ts, when, sizeof when);
    say("steam sync: %s: downloaded %zu bytes (%s)", cloud->name, len, when);
    return 0;
}

/* Steam's copy was wanted but did not arrive. The state takes the console's
 * bytes as already synced, so the push leaves the slot alone (uploading it
 * would overwrite the copy that was chosen), and keeps the old cloud digest,
 * so the next pull sees Steam's copy as new and downloads it again. */
static void download_failed(int slot, const char *local_sha) {
    snprintf(g_state[slot].local_sha, sizeof g_state[slot].local_sha, "%s", local_sha);
    g_state[slot].pending = 0;
    slot_status(slot, ISAAC_SYNC_SLOT_FAILED);
}

/* ---- ownership -------------------------------------------------------------- */

/* The game and the expansions this port's data comes from; the depots of a Steam
 * install name the same ids (appmanifest_250900.acf, "dlcappid"). */
static const struct { uint32_t appid; const char *name; } GAME_APPS[] = {
    { 250900, "The Binding of Isaac: Rebirth" },
    { 401920, "Afterbirth" },
    { 570660, "Afterbirth+" },
    { 1426300, "Repentance" },
    { 3353470, "Repentance+" },
};

static isaac_steamsync_own_fn g_own;
static void *g_own_user;

void isaac_steamsync_set_own_fn(isaac_steamsync_own_fn fn, void *user) {
    g_own = fn;
    g_own_user = user;
}

/* ISAAC_OWN_*. One unanswered question ends the check: the connection is the
 * likely cause, and each one would otherwise wait out its own timeout. */
static int check_ownership(const isaac_steam_transport *t) {
    char missing[128] = "", err[160] = "";
    int status = ISAAC_OWN_ALL;
    for (size_t i = 0; i < sizeof GAME_APPS / sizeof GAME_APPS[0]; ++i) {
        int owned = t->owns(t->state, GAME_APPS[i].appid, err, sizeof err);
        if (owned < 0) {
            say("steam sync: ownership not verified: %s", err);
            status = ISAAC_OWN_UNKNOWN;
            missing[0] = 0;
            break;
        }
        if (owned) continue;
        size_t used = strlen(missing);
        snprintf(missing + used, sizeof missing - used, "%s%s", used ? ", " : "", GAME_APPS[i].name);
        status = i == 0 ? ISAAC_OWN_NONE : ISAAC_OWN_PART;
        if (i == 0) break;
    }
    if (status == ISAAC_OWN_ALL) say("steam sync: ownership: the game and all expansions");
    else if (status != ISAAC_OWN_UNKNOWN) say("steam sync: ownership: this account lacks %s", missing);
    if (g_own) g_own(g_own_user, status, missing);
    return status;
}

int isaac_steamsync_pull_with(const isaac_steamsync_cfg *cfg, const isaac_steam_transport *t,
                              int64_t now) {
    if (!cfg || !cfg->save_dir || !t || !t->enumerate || !t->download) return -1;
    /* Without the game this account's cloud has nothing to do with these saves:
     * leave both sides alone, whichever way a sync would go. */
    if (t->owns && check_ownership(t) == ISAAC_OWN_NONE) return ISAAC_STEAMSYNC_NOT_OWNED;
    state_read(cfg);
    isaac_cloud_file files[32];
    size_t count = 0;
    char err[160] = "";
    if (t->enumerate(t->state, APPID, files, 32, &count, err, sizeof err) != 0) {
        say("steam sync: cannot list the cloud: %s", err);
        return -1;
    }
    say("steam sync: %zu cloud file(s)", count);
    int changed = 0, failed = 0;
    for (int slot = 0; slot < ISAAC_STEAMSYNC_SLOTS; ++slot) {
        const isaac_cloud_file *cloud = find_cloud(files, count, SLOTS[slot].cloud);
        char path[512], local_sha[41] = "-";
        int64_t mtime = 0;
        if (save_path(cfg, slot, path, sizeof path) != 0) continue;
        int have_local = file_digest(path, local_sha, &mtime) == 0;
        int local_changed = g_state[slot].local_sha[0] && strcmp(g_state[slot].local_sha, "-")
                                ? strcmp(g_state[slot].local_sha, have_local ? local_sha : "-") != 0
                                : have_local;
        if (!cloud) {
            if (local_changed) say("steam sync: %s: only on the console, will upload at exit", SLOTS[slot].cloud);
            slot_status(slot, local_changed ? ISAAC_SYNC_SLOT_UPLOAD
                              : have_local ? ISAAC_SYNC_SLOT_LOCAL_ONLY : ISAAC_SYNC_SLOT_EMPTY);
            continue;
        }
        int cloud_changed = strcmp(g_state[slot].cloud_sha, "-")
                                ? strcmp(g_state[slot].cloud_sha, cloud->sha_hex) != 0
                                : 1;
        if (have_local && !strcmp(local_sha, cloud->sha_hex)) {
            snprintf(g_state[slot].local_sha, sizeof g_state[slot].local_sha, "%s", local_sha);
            snprintf(g_state[slot].cloud_sha, sizeof g_state[slot].cloud_sha, "%s", cloud->sha_hex);
            g_state[slot].cloud_ts = cloud->timestamp;
            g_state[slot].pending = 0;
            slot_status(slot, ISAAC_SYNC_SLOT_SAME);
            continue;
        }
        if (!local_changed) {
            if (pull_download(cfg, t, slot, cloud, now) == 0) {
                ++changed;
                slot_status(slot, ISAAC_SYNC_SLOT_DOWNLOADED);
            } else {
                download_failed(slot, have_local ? local_sha : "-");
                ++failed;
            }
            continue;
        }
        if (!cloud_changed) {
            say("steam sync: %s: only on the console, will upload at exit", SLOTS[slot].cloud);
            slot_status(slot, ISAAC_SYNC_SLOT_UPLOAD);
            continue;
        }
        /* Both sides moved: the player decides, or the newest copy wins. */
        char local_when[32], cloud_when[32];
        describe_time(mtime, local_when, sizeof local_when);
        describe_time((int64_t)cloud->timestamp, cloud_when, sizeof cloud_when);
        int use_cloud = (int64_t)cloud->timestamp > mtime;
        if (cfg->prompt && g_ask) use_cloud = g_ask(g_ask_user, SLOTS[slot].local, local_when, cloud_when) != 0;
        say("steam sync: %s: conflict (console %s, cloud %s): keeping %s", SLOTS[slot].cloud,
            local_when, cloud_when, use_cloud ? "the cloud copy" : "the console copy");
        if (use_cloud) {
            if (pull_download(cfg, t, slot, cloud, now) == 0) {
                ++changed;
                slot_status(slot, ISAAC_SYNC_SLOT_DOWNLOADED);
            } else {
                download_failed(slot, local_sha);
                ++failed;
            }
        } else {
            snprintf(g_state[slot].local_sha, sizeof g_state[slot].local_sha, "%s", local_sha);
            snprintf(g_state[slot].cloud_sha, sizeof g_state[slot].cloud_sha, "%s", cloud->sha_hex);
            g_state[slot].cloud_ts = cloud->timestamp;
            g_state[slot].pending = 1;
            slot_status(slot, ISAAC_SYNC_SLOT_UPLOAD);
        }
    }
    state_write(cfg);
    marker_write(cfg, now);
    if (failed) {
        say("steam sync: pull incomplete, %d file(s) replaced, %d could not be downloaded", changed, failed);
        return -1;
    }
    say("steam sync: pull done, %d file(s) replaced", changed);
    return 0;
}

int isaac_steamsync_push_with(const isaac_steamsync_cfg *cfg, const isaac_steam_transport *t,
                              int64_t now) {
    if (!cfg || !cfg->save_dir || !t || !t->upload) return -1;
    int age = marker_age(cfg, now);
    if (age < 0 || age > PULL_MARKER_MAX_AGE) {
        say("steam sync: no pull this session, not uploading");
        return -1;
    }
    state_read(cfg);
    int uploaded = 0, failed = 0;
    for (int slot = 0; slot < ISAAC_STEAMSYNC_SLOTS; ++slot) {
        char path[512], local_sha[41] = "";
        int64_t mtime = 0;
        if (save_path(cfg, slot, path, sizeof path) != 0) continue;
        if (file_digest(path, local_sha, &mtime) != 0) continue;
        int local_changed = strcmp(g_state[slot].local_sha, local_sha) != 0;
        if (!local_changed && !g_state[slot].pending) continue;
        size_t len = 0;
        if (read_file(path, g_raw, sizeof g_raw, &len) != 0) {
            say("steam sync: %s: cannot read the save", SLOTS[slot].cloud);
            ++failed;
            continue;
        }
        char err[160] = "";
        uint64_t ts = 0;
        if (t->upload(t->state, APPID, SLOTS[slot].cloud, g_raw, len, &ts, err, sizeof err) != 0) {
            say("steam sync: %s: upload failed: %s", SLOTS[slot].cloud, err);
            ++failed;
            continue;
        }
        snprintf(g_state[slot].local_sha, sizeof g_state[slot].local_sha, "%s", local_sha);
        snprintf(g_state[slot].cloud_sha, sizeof g_state[slot].cloud_sha, "%s", local_sha);
        g_state[slot].cloud_ts = ts;
        g_state[slot].pending = 0;
        ++uploaded;
        say("steam sync: %s: uploaded %zu bytes", SLOTS[slot].cloud, len);
    }
    state_write(cfg);
    char marker[512];
    marker_path(cfg, marker, sizeof marker);
    remove(marker);
    say("steam sync: push done, %d uploaded, %d failed", uploaded, failed);
    return failed ? -1 : 0;
}

/* ---- token file ----------------------------------------------------------- */

int isaac_steamsync_read_token(const char *path, uint64_t *steamid, char *token, size_t cap,
                               char *err, size_t errcap) {
    if (token && cap) token[0] = 0;
    if (steamid) *steamid = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errcap, "no token file at %s", path);
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "steamid=", 8)) {
            if (steamid) *steamid = strtoull(line + 8, NULL, 10);
        } else if (!strncmp(line, "token=", 6)) {
            snprintf(token, cap, "%s", line + 6);
        }
    }
    fclose(f);
    if (!steamid || !*steamid || !token || !token[0]) {
        snprintf(err, errcap, "token file needs steamid= and token=");
        return -1;
    }
    return 0;
}

int isaac_steamsync_write_token(const char *path, uint64_t steamid, const char *token,
                                char *err, size_t errcap) {
    char text[1200];
    int n = snprintf(text, sizeof text, "steamid=%llu\ntoken=%s\n", (unsigned long long)steamid, token);
    if (n <= 0 || (size_t)n >= sizeof text) {
        snprintf(err, errcap, "the token is too long");
        return -1;
    }
    if (write_file(path, text, (size_t)n) != 0) {
        snprintf(err, errcap, "cannot write %s", path);
        return -1;
    }
    return 0;
}

void isaac_steamsync_forget(const char *save_dir) {
    char path[512];
    if (join(path, sizeof path, save_dir, STATE_FILE) == 0) remove(path);
    if (join(path, sizeof path, save_dir, PULL_MARKER) == 0) remove(path);
}

/* ---- public entry points (open the transport themselves) ------------------ */

static int run(const isaac_steamsync_cfg *cfg, int push) {
    if (!isaac_steamcm_available()) {
        say("steam sync: this build has no Steam transport");
        return -1;
    }
    isaac_steam_transport transport;
    char err[192] = "";
    if (isaac_steamcm_open(cfg->steamid, cfg->token, cfg->timeout_ms ? cfg->timeout_ms : 15000,
                           &transport, err, sizeof err) != 0) {
        say("steam sync: cannot reach Steam: %s", err);
        return -1;
    }
    int rc = push ? isaac_steamsync_push_with(cfg, &transport, (int64_t)time(NULL))
                  : isaac_steamsync_pull_with(cfg, &transport, (int64_t)time(NULL));
    if (transport.close) transport.close(transport.state);
    return rc;
}

int isaac_steamsync_pull(const isaac_steamsync_cfg *cfg) {
    return run(cfg, 0);
}

int isaac_steamsync_push(const isaac_steamsync_cfg *cfg) {
    return run(cfg, 1);
}
