#ifndef ISAAC_NATIVE_MODSYNC_H
#define ISAAC_NATIVE_MODSYNC_H

#include "native_modmanager.h"

enum mod_sync_mode { MOD_SYNC_FROM_STEAM, MOD_SYNC_TO_STEAM, MOD_SYNC_TWO_WAY, MOD_SYNC_UPDATES };
enum mod_action { MOD_INSTALL, MOD_UPDATE, MOD_REMOVE, MOD_SUBSCRIBE, MOD_UNSUBSCRIBE, MOD_UNAVAILABLE };

typedef struct {
    uint64_t id, manifest;
    enum mod_action action;
    int selected;
    char title[WORKSHOP_TITLE];
} mod_change;

typedef struct {
    mod_change *rows;
    size_t count, cap;
    uint64_t steamid;
    enum mod_sync_mode mode;
} mod_plan;

typedef struct {
    unsigned done, skipped, failed;
    char last_error[WORKSHOP_ERR];
} mod_summary;

void mod_plan_clear(mod_plan *);
void mod_plan_toggle(mod_plan *, size_t);
int mod_plan_updates(const workshop_net *, const local_mods *, mod_plan *, char [WORKSHOP_ERR]);
int mod_plan_subscriptions(const workshop_net *, const local_mods *, enum mod_sync_mode,
                           mod_plan *, char [WORKSHOP_ERR]);
int mod_plan_dependencies(const workshop_net *, const local_mods *, const workshop_item *,
                          mod_plan *, char [WORKSHOP_ERR]);
int mod_plan_apply(const workshop_net *, const mod_store *, const mod_plan *, mod_summary *,
                   char [WORKSHOP_ERR]);
int mod_remove_checked(const mod_store *, uint64_t id, char [WORKSHOP_ERR]);

#endif
