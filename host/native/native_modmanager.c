#include "native_modmanager.h"
#include "native_modstate.h"
#include "native_steamproto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define RECORD_MAX 1024
#define TREE_DEPTH 32

static int fail(char err[WORKSHOP_ERR], const char *message) {
    snprintf(err, WORKSHOP_ERR, "%s", message);
    return -1;
}

static int join(char out[MOD_PATH], const char *root, const char *name) {
    int n = snprintf(out, MOD_PATH, "%s/%s", root, name);
    return n < 0 || n >= MOD_PATH ? -1 : 0;
}

static int folder_name(char folder[MOD_FOLDER], uint64_t id) {
    return snprintf(folder, MOD_FOLDER, "workshop_%llu", (unsigned long long)id);
}

/* Windows names map to a single, relative FAT path. */
int mod_safe_path(char *path) {
    if (!*path || strlen(path) >= MOD_PATH) return -1;
    for (char *p = path; *p; ++p) {
        unsigned char c = *p;
        if (c == '\\') *p = '/';
        else if (c < 32 || c == 127 || strchr(":*?<>|\"", c)) return -1;
    }
    unsigned depth = 0;
    for (char *p = path; ; ) {
        char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!n || n > 255 || p[n - 1] == '.' || p[n - 1] == ' ' || ++depth > TREE_DEPTH) return -1;
        const char *dot = memchr(p, '.', n);
        size_t stem = dot ? (size_t)(dot - p) : n;
        if ((n == sizeof MOD_RECORD - 1 && !strncasecmp(p, MOD_RECORD, n)) ||
            (stem == 3 && (!strncasecmp(p, "CON", 3) || !strncasecmp(p, "AUX", 3) ||
                           !strncasecmp(p, "PRN", 3) || !strncasecmp(p, "NUL", 3))) ||
            (stem == 4 && (!strncasecmp(p, "COM", 3) || !strncasecmp(p, "LPT", 3)) &&
             p[3] >= '0' && p[3] <= '9')) return -1;
        if (!slash) return 0;
        p = slash + 1;
    }
}

static int mkdir_one(const char *path) {
    if (!mkdir(path, 0755)) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    return lstat(path, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}

static int directory(const char *path, int create) {
    if (create) return mkdir_one(path);
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    return S_ISDIR(st.st_mode) ? 0 : -1;
}

static int dir_chain(const char *path, int create) {
    char tmp[MOD_PATH];
    if (strlen(path) >= sizeof tmp) return -1;
    strcpy(tmp, path);
    char *volume = strchr(tmp, ':');
    char *p = volume ? volume + 1 : tmp;
    if (*p == '/') ++p;
    for (; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (directory(tmp, create)) return -1;
        *p = '/';
    }
    return directory(tmp, create);
}

int mod_mkdir_tree(const char *path) { return dir_chain(path, 1); }

static int path_exists(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static int walk(const char *path, uint64_t *size, unsigned depth) {
    struct stat st;
    if (depth > TREE_DEPTH + 2) return -1;
    if (lstat(path, &st)) return errno == ENOENT && !size ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) {
        if (!size) return unlink(path);
        if (S_ISREG(st.st_mode) && st.st_size >= 0) *size += (uint64_t)st.st_size;
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[MOD_PATH];
        if (join(child, path, e->d_name) || walk(child, size, depth + 1)) rc = -1;
    }
    closedir(dir);
    return !size && !rc ? rmdir(path) : rc;
}

static int path_set(const mod_store *store, uint64_t id, mod_install *install) {
    char folder[MOD_FOLDER], name[MOD_FOLDER + 16];
    if (!id) return -1;
    install->id = id;
    folder_name(folder, id);
    if (join(install->final, store->mods, folder)) return -1;
    snprintf(name, sizeof name, "%s.stage", folder);
    if (join(install->stage, store->work, name)) return -1;
    snprintf(name, sizeof name, "%s.backup", folder);
    if (join(install->backup, store->work, name)) return -1;
    snprintf(name, sizeof name, "%s.removed", folder);
    return join(install->trash, store->work, name);
}

static int hex_digest(const char *s) {
    return strlen(s) == 40 && strspn(s, "0123456789abcdef") == 40;
}

static int record_read(const char *dir, uint64_t expected, local_mod *mod) {
    char path[MOD_PATH];
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || join(path, dir, MOD_RECORD) ||
        lstat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t buf[RECORD_MAX];
    size_t n = fread(buf, 1, sizeof buf, f);
    int complete = feof(f) && !ferror(f);
    fclose(f);
    if (!complete) return -1;

    local_mod value = {0};
    unsigned app = 0;
    isaac_pbr r;
    isaac_pbfield field;
    isaac_pbr_init(&r, buf, n);
    int rc;
    while ((rc = isaac_pbr_next(&r, &field)) > 0) {
        if (field.wire != 0) continue;
        switch (field.field) {
        case 1: app = field.value; break;
        case 2: value.id = field.value; break;
        case 3: value.manifest = field.value; break;
        case 4: value.updated = field.value; break;
        case 5: value.installed = field.value; break;
        case 6: value.size = field.value; break;
        case 7:
            if (value.dep_count == WORKSHOP_DEPS || !field.value) return -1;
            value.deps[value.dep_count++] = field.value;
            break;
        case 11: value.deps_incomplete = field.value != 0; break;
        }
    }
    if (rc || app != WORKSHOP_APP || value.id != expected || !value.manifest ||
        isaac_pbr_str(buf, n, 8, value.title, sizeof value.title) <= 0 ||
        isaac_pbr_str(buf, n, 9, value.digest, sizeof value.digest) != 40 ||
        !hex_digest(value.digest)) return -1;
    folder_name(value.folder, expected);
    char recorded[MOD_FOLDER];
    if (isaac_pbr_str(buf, n, 10, recorded, sizeof recorded) <= 0 ||
        strcmp(recorded, value.folder)) return -1;
    if (mod) *mod = value;
    return 0;
}

static int record_write(const mod_install *install, const workshop_item *item, uint64_t bytes,
                        const char *digest) {
    uint8_t buf[RECORD_MAX];
    isaac_pbw w;
    isaac_pbw_init(&w, buf, sizeof buf);
    isaac_pbw_varint(&w, 1, WORKSHOP_APP);
    isaac_pbw_varint(&w, 2, item->id);
    isaac_pbw_varint(&w, 3, item->manifest);
    isaac_pbw_varint(&w, 4, item->updated);
    isaac_pbw_varint(&w, 5, (uint64_t)time(NULL));
    isaac_pbw_varint(&w, 6, bytes);
    for (size_t i = 0; i < item->dep_count; ++i) isaac_pbw_varint(&w, 7, item->deps[i]);
    isaac_pbw_str(&w, 8, item->title);
    isaac_pbw_str(&w, 9, digest);
    char folder[MOD_FOLDER], path[MOD_PATH];
    folder_name(folder, item->id);
    isaac_pbw_str(&w, 10, folder);
    isaac_pbw_varint(&w, 11, item->deps_incomplete);
    if (w.overflow || join(path, install->stage, MOD_RECORD)) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); return -1; }
    int ok = fwrite(buf, 1, w.len, f) == w.len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

static uint64_t folder_id(const char *name, const char **suffix) {
    if (strncmp(name, "workshop_", 9) || name[9] < '1' || name[9] > '9') return 0;
    char *end;
    errno = 0;
    unsigned long long id = strtoull(name + 9, &end, 10);
    if (errno) return 0;
    *suffix = end;
    return id;
}

int mod_store_init(mod_store *store, const char *root, const char *saves, int installed) {
    store->saves = saves;
    return join(store->mods, root, installed ? "mods" : "guest/mods") ||
           join(store->work, root, installed ? "workshop-title" : "workshop") ? -1 : 0;
}

/* Adopt an empty state directory once. Recovery only traverses directories
 * under this marker, so a pre-existing user folder is left alone. */
static int workspace(const mod_store *store, int create) {
    char marker[MOD_PATH];
    struct stat st;
    if (dir_chain(store->work, create) || join(marker, store->work, ".isaac-workshop-state-v1"))
        return -1;
    if (!lstat(marker, &st)) return S_ISREG(st.st_mode) && st.st_size == 0 ? 1 : -1;
    if (errno != ENOENT) return -1;
    if (!create) return 0;
    DIR *dir = opendir(store->work);
    if (!dir) return -1;
    int empty = 1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            empty = 0;
            break;
        }
    }
    closedir(dir);
    if (!empty) return -1;
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd)) rc = -1;
    return rc ? -1 : 1;
}

static int recover_one(const mod_install *install) {
    int final = path_exists(install->final);
    int backup = path_exists(install->backup);
    if (final < 0 || backup < 0) return -1;
    if (backup) {
        if (!final) {
            if (record_read(install->backup, install->id, NULL) ||
                rename(install->backup, install->final)) return -1;
        } else {
            if (record_read(install->final, install->id, NULL) || walk(install->backup, NULL, 0)) return -1;
        }
    }
    if (walk(install->stage, NULL, 0)) return -1;
    return walk(install->trash, NULL, 0);
}

int mod_recover(const mod_store *store) {
    if (dir_chain(store->mods, 0)) return -1;
    int owned = workspace(store, 0);
    if (owned <= 0) return owned;
    DIR *dir = opendir(store->work);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *tail;
        uint64_t id = folder_id(e->d_name, &tail);
        if (!id || (strcmp(tail, ".stage") && strcmp(tail, ".backup") && strcmp(tail, ".removed"))) continue;
        mod_install install;
        if (path_set(store, id, &install) || recover_one(&install)) rc = -1;
    }
    closedir(dir);
    return rc;
}

int mod_lookup(const mod_store *store, uint64_t id, local_mod *mod) {
    mod_install install;
    return dir_chain(store->mods, 0) || path_set(store, id, &install)
        ? -1 : record_read(install.final, id, mod);
}

const local_mod *mod_find(const local_mods *list, uint64_t id) {
    if (id)
        for (size_t i = 0; i < list->count; ++i)
            if (list->items[i].id == id) return &list->items[i];
    return NULL;
}

int mod_update_needed(const local_mod *mod, const workshop_item *item) {
    return !item->unavailable && !item->has_legacy_url && item->manifest && mod->manifest != item->manifest;
}

static int local_order(const void *a, const void *b) {
    const local_mod *left = a, *right = b;
    int title = strcasecmp(left->title, right->title);
    return title ? title : strcmp(left->folder, right->folder);
}

int mod_list(const mod_store *store, local_mods *list, char err[WORKSHOP_ERR]) {
    local_mods result = {0};
    size_t cap = 0;
    struct stat root;
    if (dir_chain(store->mods, 0)) return fail(err, "Invalid mods directory");
    if (lstat(store->mods, &root)) {
        if (errno != ENOENT) return fail(err, "Cannot read the mods directory");
        free(list->items);
        *list = result;
        return 0;
    }
    if (!S_ISDIR(root.st_mode)) return fail(err, "Invalid mods directory");
    DIR *dir = opendir(store->mods);
    if (!dir) return fail(err, "Cannot open the mods directory");
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *name = e->d_name;
        char path[MOD_PATH];
        struct stat st;
        if (name[0] == '.' || strlen(name) >= MOD_FOLDER || join(path, store->mods, name) ||
            lstat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        if (result.count == MOD_LIMIT) { rc = fail(err, "Too many local mod folders"); break; }
        if (result.count == cap) {
            cap = cap ? cap * 2 : 32;
            local_mod *more = realloc(result.items, cap * sizeof *more);
            if (!more) { rc = fail(err, "Out of memory"); break; }
            result.items = more;
        }
        local_mod *mod = &result.items[result.count++];
        const char *tail;
        uint64_t id = folder_id(name, &tail);
        if (!id || *tail || record_read(path, id, mod)) {
            memset(mod, 0, sizeof *mod);
            snprintf(mod->title, sizeof mod->title, "%.*s", (int)sizeof mod->title - 1, name);
            snprintf(mod->folder, sizeof mod->folder, "%s", name);
            (void)walk(path, &mod->size, 0);
        }
        int chosen = isaac_modstate_get(store->saves, mod->folder);
        char off[MOD_PATH];
        mod->enabled = chosen >= 0 ? chosen : !join(off, path, "disable.it") && path_exists(off) == 0;
    }
    closedir(dir);
    if (rc) { free(result.items); return rc; }
    if (result.count > 1) qsort(result.items, result.count, sizeof *result.items, local_order);
    free(list->items);
    *list = result;
    return 0;
}

int mod_set_enabled(const mod_store *store, const char *folder, int on, char err[WORKSHOP_ERR]) {
    char path[MOD_PATH];
    struct stat st;
    if (dir_chain(store->mods, 0) || !*folder || folder[0] == '.' || strchr(folder, '/') || strchr(folder, '\\') ||
        join(path, store->mods, folder) || lstat(path, &st) || !S_ISDIR(st.st_mode) ||
        isaac_modstate_set(store->saves, folder, on))
        return fail(err, "Could not write the mod's enable setting");
    return 0;
}

int mod_remove(const mod_store *store, uint64_t id, char err[WORKSHOP_ERR]) {
    mod_install install;
    if (dir_chain(store->mods, 0) || workspace(store, 1) != 1 ||
        path_set(store, id, &install) || recover_one(&install) ||
        record_read(install.final, id, NULL))
        return fail(err, "This installation is not Workshop-managed");
    if (rename(install.final, install.trash)) return fail(err, "Could not move the installed mod");
    if (walk(install.trash, NULL, 0)) return fail(err, "Removed from the game; SD cleanup pending");
    return 0;
}

int mod_install_begin(const mod_store *store, uint64_t id, uint64_t bytes, mod_install *install,
                      char err[WORKSHOP_ERR]) {
    if (path_set(store, id, install) || mod_mkdir_tree(store->mods) || workspace(store, 1) != 1 ||
        recover_one(install)) return fail(err, "Cannot prepare Workshop staging on the SD card");
    int exists = path_exists(install->final);
    if (exists < 0 || (exists && record_read(install->final, id, NULL)))
        return fail(err, "Install folder belongs to a manual mod");
    struct statvfs space;
    if (statvfs(store->work, &space)) return fail(err, "Cannot check SD card space");
    uint64_t unit = space.f_frsize ? space.f_frsize : space.f_bsize;
    if (!unit || (bytes / unit + (bytes % unit != 0)) > space.f_bavail)
        return fail(err, "Insufficient SD card space");
    if (mkdir(install->stage, 0755)) return fail(err, "Cannot create Workshop staging folder");
    return 0;
}

int mod_install_commit(const mod_install *install, const workshop_item *item, uint64_t bytes,
                        const char digest[41], char err[WORKSHOP_ERR]) {
    if (item->id != install->id || !item->manifest || !hex_digest(digest) ||
        record_write(install, item, bytes, digest)) return fail(err, "Cannot write the mod registry");
    int exists = path_exists(install->final);
    if (exists < 0 || (exists && record_read(install->final, install->id, NULL)))
        return fail(err, "Install folder belongs to a manual mod");
    if (exists && rename(install->final, install->backup))
        return fail(err, "Could not move the previous installation");
    if (rename(install->stage, install->final)) {
        if (exists && rename(install->backup, install->final))
            return fail(err, "SD card error restoring previous install; recovery pending");
        return fail(err, "Could not commit the verified installation");
    }
    if (exists) (void)walk(install->backup, NULL, 0);
    return 0;
}

void mod_install_abort(const mod_install *install) {
    (void)walk(install->stage, NULL, 0);
}
