#include "native_modsync.h"
#include "native_steamcdn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_LIMIT (2 * MOD_LIMIT)

static int fail(char err[WORKSHOP_ERR], const char *message) {
    snprintf(err, WORKSHOP_ERR, "%s", message);
    return -1;
}

static int stopped(const workshop_net *net) {
    return net->cancelled && net->cancelled(net->ctx);
}

void mod_plan_clear(mod_plan *plan) {
    free(plan->rows);
    memset(plan, 0, sizeof *plan);
}

static int add(mod_plan *plan, uint64_t id, uint64_t manifest, enum mod_action action, const char *name,
               char err[WORKSHOP_ERR]) {
    if (plan->count == PLAN_LIMIT) return fail(err, "Too many Workshop changes for one review");
    if (plan->count == plan->cap) {
        size_t cap = plan->cap ? plan->cap * 2 : 32;
        mod_change *more = realloc(plan->rows, cap * sizeof *more);
        if (!more) return fail(err, "Out of memory");
        plan->rows = more;
        plan->cap = cap;
    }
    mod_change *row = &plan->rows[plan->count++];
    *row = (mod_change){id, manifest, action, action == MOD_INSTALL || action == MOD_UPDATE, ""};
    snprintf(row->title, sizeof row->title, "%s", name);
    return 0;
}

void mod_plan_toggle(mod_plan *plan, size_t index) {
    if (index >= plan->count || plan->rows[index].action == MOD_UNAVAILABLE) return;
    mod_change *row = &plan->rows[index];
    row->selected = !row->selected;
    if (!row->selected) return;
    for (size_t i = 0; i < plan->count; ++i)
        if (i != index && plan->rows[i].id == row->id) plan->rows[i].selected = 0;
}

int mod_plan_updates(const workshop_net *net, const local_mods *local, mod_plan *plan,
                     char err[WORKSHOP_ERR]) {
    mod_plan_clear(plan);
    plan->mode = MOD_SYNC_UPDATES;
    plan->steamid = net->steamid;
    for (size_t i = 0; i < local->count; ++i) {
        if (stopped(net)) { fail(err, "Cancelled"); goto error; }
        const local_mod *installed = &local->items[i];
        if (!installed->id) continue;
        workshop_item item;
        if (workshop_details(net, installed->id, &item, err)) goto error;
        if (item.unavailable || !item.manifest || item.has_legacy_url) {
            if (add(plan, installed->id, 0, MOD_UNAVAILABLE, installed->title, err)) goto error;
        } else if (mod_update_needed(installed, &item) &&
                   add(plan, item.id, item.manifest, MOD_UPDATE, item.title, err)) goto error;
    }
    return 0;
error:
    mod_plan_clear(plan);
    return -1;
}

int mod_plan_subscriptions(const workshop_net *net, const local_mods *local, enum mod_sync_mode mode,
                           mod_plan *plan, char err[WORKSHOP_ERR]) {
    mod_plan_clear(plan);
    plan->mode = mode;
    plan->steamid = net->steamid;
    workshop_page *page = malloc(sizeof *page);
    uint64_t *seen = malloc(MOD_LIMIT * sizeof *seen);
    if (!page || !seen) { free(page); free(seen); return fail(err, "Out of memory"); }
    unsigned downloaded = 0, total = 0;
    int rc = -1;
    for (unsigned p = 1; ; ++p) {
        if (stopped(net)) { fail(err, "Cancelled"); goto end; }
        if (workshop_subscriptions(net, p, page, err)) goto end;
        if (p == 1) total = page->total;
        if (page->total != total || total > MOD_LIMIT || downloaded + page->count > total ||
            (page->count == 0 && downloaded < total)) {
            fail(err, "Subscriptions changed or Steam returned an incomplete list");
            goto end;
        }
        for (size_t i = 0; i < page->count; ++i) {
            workshop_item *item = &page->items[i];
            for (unsigned j = 0; j < downloaded; ++j) {
                if (seen[j] != item->id) continue;
                fail(err, "Steam returned a duplicate subscription");
                goto end;
            }
            seen[downloaded++] = item->id;
            if (mode == MOD_SYNC_TO_STEAM) continue;
            const local_mod *installed = mod_find(local, item->id);
            if (!item->unavailable && !item->has_legacy_url && !item->manifest &&
                workshop_details(net, item->id, item, err)) goto end;
            if (item->unavailable || !item->manifest || item->has_legacy_url) {
                if (add(plan, item->id, 0, MOD_UNAVAILABLE, item->title, err)) goto end;
            } else if ((!installed || mod_update_needed(installed, item)) &&
                       add(plan, item->id, item->manifest, installed ? MOD_UPDATE : MOD_INSTALL, item->title, err))
                goto end;
            /* Only two-way review offers this alternative. Simply being
               absent locally never selects a change to Steam. */
            if (mode == MOD_SYNC_TWO_WAY && !installed &&
                add(plan, item->id, item->manifest, MOD_UNSUBSCRIBE, item->title, err)) goto end;
        }
        if (downloaded == total) break;
    }
    for (size_t i = 0; i < local->count; ++i) {
        const local_mod *installed = &local->items[i];
        if (!installed->id) continue;
        int found = 0;
        for (unsigned j = 0; j < downloaded; ++j)
            if (seen[j] == installed->id) { found = 1; break; }
        if (found) continue;
        if (stopped(net)) { fail(err, "Cancelled"); goto end; }
        int subscribed = 0;
        if (workshop_membership(net, installed->id, &subscribed, err)) goto end;
        if (subscribed) {
            fail(err, "Steam subscription list changed during review. Try again.");
            goto end;
        }
        if (mode != MOD_SYNC_TO_STEAM &&
            add(plan, installed->id, installed->manifest, MOD_REMOVE, installed->title, err)) goto end;
        if (mode != MOD_SYNC_FROM_STEAM &&
            add(plan, installed->id, installed->manifest, MOD_SUBSCRIBE, installed->title, err)) goto end;
    }
    rc = 0;
end:
    free(seen);
    free(page);
    if (rc) mod_plan_clear(plan);
    return rc;
}

int mod_plan_dependencies(const workshop_net *net, const local_mods *local, const workshop_item *item,
                          mod_plan *plan, char err[WORKSHOP_ERR]) {
    mod_plan_clear(plan);
    plan->mode = MOD_SYNC_UPDATES;
    plan->steamid = net->steamid;
    workshop_item current = *item;
    uint64_t seen[MOD_LIMIT] = {item->id};
    size_t count = 1, next = 1;
    int rc = -1;
    for (;;) {
        if (stopped(net)) { fail(err, "Cancelled"); break; }
        if (current.deps_incomplete) { fail(err, "Steam returned an incomplete dependency list"); break; }
        for (size_t i = 0; i < current.dep_count; ++i) {
            uint64_t id = current.deps[i];
            int found = 0;
            for (size_t j = 0; j < count; ++j) if (seen[j] == id) { found = 1; break; }
            if (found) continue;
            if (count == MOD_LIMIT) { fail(err, "Too many Workshop dependencies"); goto end; }
            seen[count++] = id;
        }
        if (next == count) { rc = 0; break; }
        uint64_t id = seen[next++];
        if (mod_find(local, id)) {
            const local_mod *installed = mod_find(local, id);
            memset(&current, 0, sizeof current);
            current.dep_count = installed->dep_count;
            current.deps_incomplete = installed->deps_incomplete;
            memcpy(current.deps, installed->deps, current.dep_count * sizeof *current.deps);
            continue;
        }
        if (workshop_details(net, id, &current, err)) break;
        enum mod_action action = current.unavailable || !current.manifest || current.has_legacy_url
            ? MOD_UNAVAILABLE : MOD_INSTALL;
        if (add(plan, id, current.manifest, action, current.title, err)) break;
    }
end:
    if (rc) mod_plan_clear(plan);
    return rc;
}

int mod_remove_checked(const mod_store *store, uint64_t id, char err[WORKSHOP_ERR]) {
    local_mods current = {0};
    if (mod_list(store, &current, err)) return -1;
    int rc = 0;
    for (size_t i = 0; i < current.count && !rc; ++i) {
        const local_mod *mod = &current.items[i];
        if (!mod->id || mod->id == id) continue;
        for (size_t d = 0; d < mod->dep_count; ++d) {
            if (mod->deps[d] != id) continue;
            snprintf(err, WORKSHOP_ERR, "Required by %s", mod->title);
            rc = -1;
            break;
        }
    }
    free(current.items);
    return rc ? rc : mod_remove(store, id, err);
}

static int apply(const workshop_net *net, const mod_store *store, const mod_plan *plan,
                 const mod_change *row, char err[WORKSHOP_ERR]) {
    local_mod current;
    int installed = mod_lookup(store, row->id, &current) == 0;
    if (row->action == MOD_REMOVE || row->action == MOD_SUBSCRIBE) {
        if (!installed || current.manifest != row->manifest)
            return fail(err, "Local installation changed since the preview");
        int subscribed;
        if (workshop_membership(net, row->id, &subscribed, err)) return -1;
        if (subscribed) return row->action == MOD_REMOVE ? fail(err, "This item is now subscribed") : 1;
        return row->action == MOD_REMOVE ? mod_remove_checked(store, row->id, err)
                                         : workshop_set_subscription(net, row->id, 1, err);
    }
    if (row->action == MOD_UNSUBSCRIBE) {
        if (installed) return fail(err, "Local installation changed since the preview");
        int subscribed;
        if (workshop_membership(net, row->id, &subscribed, err)) return -1;
        return subscribed ? workshop_set_subscription(net, row->id, 0, err) : 1;
    }
    if (installed && current.manifest == row->manifest) return 1;
    if (plan->mode != MOD_SYNC_UPDATES) {
        int subscribed;
        if (workshop_membership(net, row->id, &subscribed, err)) return -1;
        if (!subscribed) return fail(err, "This item is no longer subscribed");
    }
    workshop_item item;
    if (workshop_details(net, row->id, &item, err)) return -1;
    if (row->manifest != item.manifest) return fail(err, "Steam item changed since the preview");
    return workshop_install(net, store, &item, err);
}

int mod_plan_apply(const workshop_net *net, const mod_store *store, const mod_plan *plan,
                   mod_summary *summary, char err[WORKSHOP_ERR]) {
    memset(summary, 0, sizeof *summary);
    if (!plan->steamid || plan->steamid != net->steamid)
        return fail(err, "Steam account changed. Review the plan again.");
    unsigned current = 0, total = 0;
    for (size_t i = 0; i < plan->count; ++i)
        if (plan->rows[i].selected && plan->rows[i].action != MOD_UNAVAILABLE) ++total;
    for (size_t i = 0; i < plan->count; ++i) {
        const mod_change *row = &plan->rows[i];
        if (!row->selected || row->action == MOD_UNAVAILABLE || stopped(net)) {
            ++summary->skipped;
            continue;
        }
        ++current;
        if (net->operation) net->operation(net->ctx, current, total);
        char error[WORKSHOP_ERR] = "";
        int rc = apply(net, store, plan, row, error);
        if (rc < 0) {
            if (stopped(net)) ++summary->skipped;
            else {
                ++summary->failed;
                snprintf(summary->last_error, sizeof summary->last_error, "%s", error);
            }
        } else if (rc == 0) ++summary->done;
        else ++summary->skipped;
    }
    return 0;
}
