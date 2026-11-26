#ifndef ISAAC_NATIVE_MODMANAGER_H
#define ISAAC_NATIVE_MODMANAGER_H

#include "native_steamworkshop.h"

#define MOD_PATH 512
#define MOD_FOLDER 256
#define MOD_LIMIT 4096
#define MOD_RECORD ".isaac-workshop-v1"

typedef struct {
    uint64_t id, manifest, updated, installed, size;
    uint64_t deps[WORKSHOP_DEPS];
    size_t dep_count;
    int enabled, deps_incomplete;
    char digest[41];
    char title[WORKSHOP_TITLE], folder[MOD_FOLDER];
} local_mod;

typedef struct {
    local_mod *items;
    size_t count;
} local_mods;

typedef struct {
    char mods[MOD_PATH], work[MOD_PATH];
    const char *saves;
} mod_store;

typedef struct {
    char final[MOD_PATH], stage[MOD_PATH], backup[MOD_PATH], trash[MOD_PATH];
    uint64_t id;
} mod_install;

int mod_store_init(mod_store *, const char *root, const char *saves, int installed);
int mod_recover(const mod_store *);
int mod_list(const mod_store *, local_mods *, char [WORKSHOP_ERR]);
const local_mod *mod_find(const local_mods *, uint64_t id);
int mod_lookup(const mod_store *, uint64_t id, local_mod *);
int mod_update_needed(const local_mod *, const workshop_item *);
int mod_set_enabled(const mod_store *, const char *folder, int on, char [WORKSHOP_ERR]);
int mod_remove(const mod_store *, uint64_t id, char [WORKSHOP_ERR]);
int mod_safe_path(char *path);
int mod_mkdir_tree(const char *path);
int mod_install_begin(const mod_store *, uint64_t id, uint64_t bytes, mod_install *, char [WORKSHOP_ERR]);
int mod_install_commit(const mod_install *, const workshop_item *, uint64_t bytes,
                       const char digest[41], char [WORKSHOP_ERR]);
void mod_install_abort(const mod_install *);

#endif
