#include "native_launcher.h"
#include "native_modsync.h"
#include "native_steamcm.h"
#include "native_steamsync.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>

#define CURSOR_HISTORY 128
#define THUMBS 64
#define CACHED_PAGES 8
#define ANIMATED_THUMBS 6
#define MEDIA_CACHE 3
#define INSTALL_QUEUE 12

enum job {
    JOB_NONE, JOB_LOCAL, JOB_BROWSE, JOB_DETAILS, JOB_THUMB, JOB_ILLUSTRATION, JOB_INSTALL,
    JOB_REMOVE, JOB_TOGGLE, JOB_SUBSCRIPTIONS, JOB_UPDATES, JOB_DEPENDENCIES, JOB_APPLY, JOB_QUICK_INSTALL
};

typedef struct {
    uint64_t id;
    workshop_animation animation;
} preview_cache;

typedef struct {
    unsigned number;
    workshop_page *page;
} page_cache;

typedef struct {
    unsigned id;
    workshop_animation animation;
} media_cache;

typedef struct {
    uint64_t id;
    char title[WORKSHOP_TITLE];
} install_item;

static struct {
    mod_store store;
    const char *token;
    workshop_view view;
    workshop_page page, next_page;
    page_cache pages[CACHED_PAGES];
    workshop_item detail;
    local_mods local, quick_local;
    install_item queue[INSTALL_QUEUE], active_install;
    unsigned queued, installed_count, failed_count;
    char install_error[WORKSHOP_ERR];
    int quick_reloaded;
    mod_plan plan;
    mod_summary summary;
    char cursor[CURSOR_HISTORY][WORKSHOP_CURSOR];
    char search[96], tag[64];
    char message[WORKSHOP_ERR], error[WORKSHOP_ERR];
    char folder[MOD_FOLDER];
    preview_cache thumbs[THUMBS];
    unsigned animated_thumbs[ANIMATED_THUMBS], animation_slot;
    char thumb_url[WORKSHOP_MEDIA_URL], thumb_alt[WORKSHOP_MEDIA_URL];
    char description[4096], media_url[WORKSHOP_MEDIA_IMAGES][WORKSHOP_MEDIA_URL];
    media_cache media[MEDIA_CACHE];
    workshop_animation next_media;
    unsigned media_count, media_id, media_slot, media_visible;
    int media_page;
    char query_cursor[WORKSHOP_CURSOR];
    unsigned thumbnail_slot, page_slot;
    uint64_t target, thumb_id, deferred;
    size_t index, list_start;
    unsigned page_no, requested_page, oldest_page;
    int tab, signed_in, expired, detail_open, confirming;
    int browse_loaded, select_last, show_plan, net_failed, toggle_on, description_page, description_pages;
    enum workshop_sort sort;
    enum mod_sync_mode mode;
    enum job job;
    int rc;
    pthread_t thread;
    atomic_int finished, cancel;
    atomic_uint_fast64_t bytes, total;
    atomic_uint operation, operations;
} b;

static const char *sort_name[] = {"Most popular", "Most subscribed", "Recently updated",
                                  "Most recent", "Search relevance"};
static const char *mode_name[] = {"Steam to Switch", "Switch to Steam", "Two-way"};
static const char *action_name[] = {"Install on Switch", "Update on Switch", "Remove from Switch",
                                    "Subscribe on Steam", "Unsubscribe on Steam", "Unavailable"};

static int cancelled(void *unused) {
    (void)unused;
    return atomic_load(&b.cancel);
}

static void progress(void *unused, uint64_t bytes, uint64_t total) {
    (void)unused;
    atomic_store(&b.total, total);
    atomic_store(&b.bytes, bytes);
}

static void operation(void *unused, unsigned current, unsigned total) {
    (void)unused;
    progress(NULL, 0, 0);
    atomic_store(&b.operations, total);
    atomic_store(&b.operation, current);
}

static int service(void *unused, const char *method, const void *req, size_t req_len,
                   void *out, size_t cap, size_t *len, char err[WORKSHOP_ERR]) {
    (void)unused;
    if (cancelled(NULL)) {
        snprintf(err, WORKSHOP_ERR, "Cancelled");
        return -1;
    }
    return isaac_steamcm_service(method, req, req_len, out, cap, len, err, WORKSHOP_ERR);
}

static int get(void *unused, const char *host, const char *path, void *out, size_t cap,
               size_t *len, int *status, char err[WORKSHOP_ERR]) {
    (void)unused;
    return isaac_steamcm_https_get(host, path, out, cap, len, status, cancelled, NULL, err, WORKSHOP_ERR);
}

static int key(void *unused, uint32_t app, uint32_t depot, uint8_t out[32], char err[WORKSHOP_ERR]) {
    (void)unused;
    return isaac_steamcm_depot_key(app, depot, out, err, WORKSHOP_ERR);
}

static void wipe(void *ptr, size_t len) {
    volatile unsigned char *p = ptr;
    while (len--) *p++ = 0;
}

static int image_job(void) { return b.job == JOB_THUMB || b.job == JOB_ILLUSTRATION; }

static int do_steam(workshop_net *net) {
    switch (b.job) {
    case JOB_BROWSE:
        if (workshop_query(net, b.sort, b.search, b.tag, b.query_cursor, &b.next_page, b.error)) return -1;
        workshop_authors(net, b.next_page.items, b.next_page.count);
        return 0;
    case JOB_DETAILS:
        if (workshop_details(net, b.target, &b.detail, b.error)) return -1;
        workshop_authors(net, &b.detail, 1);
        return 0;
    case JOB_INSTALL: case JOB_QUICK_INSTALL: {
        workshop_item item;
        if (workshop_details(net, b.target, &item, b.error)) return -1;
        if (b.job == JOB_INSTALL && b.detail.manifest && item.manifest != b.detail.manifest) {
            snprintf(b.error, sizeof b.error, "The item's version changed. Reopen its details.");
            return -1;
        }
        const local_mod *local = mod_find(&b.local, item.id);
        if (b.job == JOB_QUICK_INSTALL && local && !mod_update_needed(local, &item)) return 0;
        return workshop_install(net, &b.store, &item, b.error);
    }
    case JOB_SUBSCRIPTIONS:
        return mod_plan_subscriptions(net, &b.local, b.mode, &b.plan, b.error);
    case JOB_UPDATES:
        return mod_plan_updates(net, &b.local, &b.plan, b.error);
    case JOB_DEPENDENCIES:
        return mod_plan_dependencies(net, &b.local, &b.detail, &b.plan, b.error);
    case JOB_APPLY:
        return mod_plan_apply(net, &b.store, &b.plan, &b.summary, b.error);
    default:
        return -1;
    }
}

static int network_job(void) {
    if (time(NULL) < 1735689600) {
        snprintf(b.error, sizeof b.error, "Set the Switch clock before connecting to Steam.");
        return -1;
    }
    char token[1024];
    workshop_net net = {.call = service, .get = get, .key = key,
                        .cancelled = cancelled, .progress = progress, .operation = operation};
    if (!image_job() &&
        isaac_steamsync_read_token(b.token, &net.steamid, token, sizeof token, b.error, sizeof b.error)) {
        b.expired = 1;
        return -1;
    }
    if (R_FAILED(socketInitializeDefault())) {
        wipe(token, sizeof token);
        snprintf(b.error, sizeof b.error, "Could not start the network.");
        return -1;
    }
    isaac_steamcm_set_cancel(cancelled, NULL);
    int rc = -1;
    if (image_job()) {
        if (!isaac_steamcm_web_open(b.error, sizeof b.error)) {
            if (b.job == JOB_ILLUSTRATION)
                rc = workshop_illustration(&net, b.thumb_url, &b.next_media, b.error);
            else {
                rc = workshop_preview(&net, b.thumb_url, &b.next_media, b.error);
                if (rc && *b.thumb_alt && strcmp(b.thumb_url, b.thumb_alt))
                    rc = workshop_preview(&net, b.thumb_alt, &b.next_media, b.error);
            }
        }
        isaac_steamcm_web_close();
    } else {
        isaac_steam_transport steam;
        if (!isaac_steamcm_open(net.steamid, token, 5000, &steam, b.error, sizeof b.error)) {
            wipe(token, sizeof token);
            net.cell = isaac_steamcm_cell();
            int owns = 1;
            if (b.job == JOB_INSTALL || b.job == JOB_APPLY || b.job == JOB_QUICK_INSTALL)
                owns = steam.owns(steam.state, WORKSHOP_APP, b.error, sizeof b.error);
            if (owns == 1 && !cancelled(NULL)) rc = do_steam(&net);
            else if (owns == 0) snprintf(b.error, sizeof b.error, "This Steam account does not own Isaac.");
            steam.close(steam.state);
        } else if (isaac_steamcm_token_refused()) {
            b.expired = 1;
            snprintf(b.error, sizeof b.error, "Steam session expired. Sign in again with QR.");
        }
        wipe(token, sizeof token);
    }
    isaac_steamcm_set_cancel(NULL, NULL);
    socketExit();
    return rc;
}

static void *worker(void *unused) {
    (void)unused;
    int rc;
    switch (b.job) {
    case JOB_LOCAL: rc = 0; break;
    case JOB_REMOVE: rc = mod_remove(&b.store, b.target, b.error); break;
    case JOB_TOGGLE: rc = mod_set_enabled(&b.store, b.folder, b.toggle_on, b.error); break;
    default: rc = network_job(); break;
    }
    if (b.job == JOB_LOCAL || b.job == JOB_REMOVE || b.job == JOB_TOGGLE || b.job == JOB_INSTALL ||
        b.job == JOB_APPLY || b.job == JOB_QUICK_INSTALL) {
        char err[WORKSHOP_ERR] = "";
        local_mods *list = b.job == JOB_QUICK_INSTALL ? &b.quick_local : &b.local;
        int failed = mod_list(&b.store, list, err);
        if (b.job == JOB_QUICK_INSTALL) b.quick_reloaded = !failed;
        if (failed && !rc) {
            snprintf(b.error, sizeof b.error, "%s", err);
            rc = -1;
        }
    }
    b.rc = rc;
    atomic_store(&b.finished, 1);
    return NULL;
}

static int post(enum job job, const char *message) {
    if (b.job != JOB_NONE) return -1;
    b.job = job;
    b.error[0] = 0;
    atomic_store(&b.cancel, 0);
    atomic_store(&b.finished, 0);
    operation(NULL, 0, 0);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
    int rc = pthread_create(&b.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        b.job = JOB_NONE;
        snprintf(b.message, sizeof b.message, "Could not start the Workshop worker.");
        return -1;
    }
    if (message) snprintf(b.message, sizeof b.message, "%s", message);
    return 0;
}

static int online(void) {
    if (b.signed_in) return 1;
    snprintf(b.message, sizeof b.message, "Sign in with Steam using QR.");
    return 0;
}

static void select_page(const workshop_page *page, unsigned number, int select_last) {
    b.page = *page;
    b.page_no = number;
    b.browse_loaded = 1;
    b.net_failed = 0;
    b.message[0] = 0;
    b.index = select_last && page->count ? page->count - 1 : 0;
}

static void clear_pages(void) {
    for (unsigned i = 0; i < CACHED_PAGES; ++i) {
        free(b.pages[i].page);
        b.pages[i].page = NULL;
    }
    b.page_slot = 0;
}

static void query(unsigned page, int select_last) {
    if (!online()) return;
    for (unsigned i = 0; i < CACHED_PAGES; ++i) {
        if (!b.pages[i].page || b.pages[i].number != page) continue;
        select_page(b.pages[i].page, page, select_last);
        return;
    }
    if (b.job) return;
    b.requested_page = page;
    b.select_last = select_last;
    snprintf(b.query_cursor, sizeof b.query_cursor, "%s", b.cursor[page % CURSOR_HISTORY]);
    post(JOB_BROWSE, "Loading Workshop items…");
}

static void reset_query(void) {
    clear_pages();
    memset(b.cursor, 0, sizeof b.cursor);
    memset(&b.page, 0, sizeof b.page);
    b.browse_loaded = 0;
    b.page_no = 0;
    b.oldest_page = 0;
    b.index = 0;
    query(0, 0);
}

static size_t item_count(void) {
    if (b.show_plan) return b.plan.count;
    if (b.tab == WORKSHOP_BROWSE) return b.page.count;
    if (b.tab == WORKSHOP_INSTALLED) return b.local.count;
    return 0;
}

static void clear_media(void) {
    for (unsigned i = 0; i < MEDIA_CACHE; ++i) {
        workshop_animation_clear(&b.media[i].animation);
        b.media[i].id = 0;
    }
    b.media_page = -1;
}

static int quick_available(const workshop_item *item) {
    if (!item || item->unavailable || !item->id || item->has_legacy_url) return 0;
    const local_mod *local = mod_find(&b.local, item->id);
    if (local && item->manifest && !mod_update_needed(local, item)) return 0;
    if (b.job == JOB_QUICK_INSTALL && b.active_install.id == item->id) return 0;
    for (unsigned i = 0; i < b.queued; ++i)
        if (b.queue[i].id == item->id) return 0;
    return 1;
}

static void queue_install(const workshop_item *item) {
    if (!b.signed_in || !quick_available(item) || b.queued == INSTALL_QUEUE) return;
    if (!b.queued && b.job != JOB_QUICK_INSTALL) {
        b.installed_count = b.failed_count = 0;
        b.install_error[0] = 0;
    }
    install_item *pending = &b.queue[b.queued++];
    pending->id = item->id;
    snprintf(pending->title, sizeof pending->title, "%s", item->title);
}

static void start_queued_install(void) {
    if (b.job || !b.queued || !b.signed_in) return;
    b.active_install = b.queue[0];
    b.target = b.active_install.id;
    b.quick_reloaded = 0;
    --b.queued;
    memmove(b.queue, b.queue + 1, b.queued * sizeof b.queue[0]);
    if (post(JOB_QUICK_INSTALL, "") && b.queued)
        b.queued = 0;
}

static void finish_queued_install(void) {
    if (b.quick_reloaded) {
        local_mods previous = b.local;
        b.local = b.quick_local;
        b.quick_local = previous;
    }
    if (cancelled(NULL)) {
        snprintf(b.message, sizeof b.message, "Queue cancelled. %u installed.", b.installed_count);
        return;
    }
    if (b.rc) {
        ++b.failed_count;
        snprintf(b.install_error, sizeof b.install_error, "%s",
                 *b.error ? b.error : "Workshop request failed");
    } else ++b.installed_count;
    if (b.expired) b.queued = 0;
    if (!b.queued)
        snprintf(b.message, sizeof b.message, "%u installed, %u failed%s%.145s",
                 b.installed_count, b.failed_count, *b.install_error ? " • " : "", b.install_error);
}

static void confirm_install(void);

static void complete_job(void) {
    if (!b.job || !atomic_load(&b.finished)) return;
    pthread_join(b.thread, NULL);
    enum job job = b.job;
    b.job = JOB_NONE;
    if (b.expired) b.signed_in = 0;
    if (job == JOB_QUICK_INSTALL) {
        finish_queued_install();
        return;
    }
    if (job == JOB_ILLUSTRATION) {
        if (b.rc && cancelled(NULL)) {
            workshop_animation_clear(&b.next_media);
        } else {
            media_cache *slot = &b.media[b.media_slot++ % MEDIA_CACHE];
            workshop_animation_clear(&slot->animation);
            slot->id = b.media_id;
            slot->animation = b.next_media;
            memset(&b.next_media, 0, sizeof b.next_media);
        }
        return;
    }
    if (job == JOB_THUMB) {
        if (b.rc && cancelled(NULL)) {
            workshop_animation_clear(&b.next_media);
            return;
        }
        unsigned i = b.thumbnail_slot++ % THUMBS;
        preview_cache *slot = &b.thumbs[i];
        workshop_animation_clear(&slot->animation);
        for (unsigned j = 0; j < ANIMATED_THUMBS; ++j)
            if (b.animated_thumbs[j] == i + 1) b.animated_thumbs[j] = 0;
        if (b.next_media.count > 1) {
            unsigned *old = &b.animated_thumbs[b.animation_slot++ % ANIMATED_THUMBS];
            if (*old) {
                workshop_animation *animation = &b.thumbs[*old - 1].animation;
                for (unsigned j = 1; j < animation->count; ++j) free(animation->frames[j]);
                animation->count = 1;
            }
            *old = i + 1;
        }
        slot->id = b.thumb_id;
        slot->animation = b.next_media;
        memset(&b.next_media, 0, sizeof b.next_media);
        return;
    }
    if (b.rc) {
        snprintf(b.message, sizeof b.message, "%s",
                 cancelled(NULL) ? "Cancelled" : *b.error ? b.error : "Workshop request failed");
        if (job == JOB_BROWSE) b.net_failed = 1;
        if (job == JOB_DETAILS) b.detail_open = 0;
        return;
    }
    b.message[0] = 0;
    switch (job) {
    case JOB_BROWSE: {
        select_page(&b.next_page, b.requested_page, b.select_last);
        if (b.page_no - b.oldest_page >= CURSOR_HISTORY - 1)
            b.oldest_page = b.page_no - (CURSOR_HISTORY - 2);
        snprintf(b.cursor[(b.page_no + 1) % CURSOR_HISTORY], WORKSHOP_CURSOR, "%s",
                 strcmp(b.query_cursor, b.page.cursor) ? b.page.cursor : "");
        page_cache *entry = &b.pages[b.page_slot++ % CACHED_PAGES];
        if (!entry->page) entry->page = malloc(sizeof *entry->page);
        if (entry->page) {
            entry->number = b.page_no;
            *entry->page = b.page;
        }
        break;
    }
    case JOB_DETAILS:
        clear_media();
        b.media_count = workshop_media_description(b.detail.description, b.description, sizeof b.description, b.media_url);
        b.detail_open = 1;
        b.description_page = 0;
        b.description_pages = isaac_launcher_workshop_description_pages(b.description);
        break;
    case JOB_SUBSCRIPTIONS: case JOB_UPDATES: case JOB_DEPENDENCIES:
        b.show_plan = 1;
        b.detail_open = 0;
        b.index = 0;
        break;
    case JOB_INSTALL:
        snprintf(b.message, sizeof b.message, "Mod installed on this Switch.");
        break;
    case JOB_REMOVE:
        b.detail_open = 0;
        snprintf(b.message, sizeof b.message, "Mod removed from this Switch.");
        break;
    case JOB_TOGGLE:
        if (b.index >= b.local.count && b.local.count) b.index = b.local.count - 1;
        snprintf(b.message, sizeof b.message, "Mod setting saved for the next game launch.");
        break;
    case JOB_APPLY:
        b.show_plan = 0;
        b.index = 0;
        snprintf(b.message, sizeof b.message, "%u succeeded, %u skipped, %u failed. %.126s",
                 b.summary.done, b.summary.skipped, b.summary.failed, b.summary.last_error);
        mod_plan_clear(&b.plan);
        break;
    default: break;
    }
    size_t count = item_count();
    if (b.index >= count) b.index = count ? count - 1 : 0;
}

static int keyboard(char *out, size_t cap, const char *guide) {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) return -1;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetGuideText(&config, guide);
    swkbdConfigSetInitialText(&config, out);
    swkbdConfigSetStringLenMax(&config, (u32)(cap / 4 - 1));
    char text[96] = "";
    Result rc = swkbdShow(&config, text, cap);
    swkbdClose(&config);
    if (R_FAILED(rc)) return -1;
    snprintf(out, cap, "%s", text);
    return 0;
}

static const workshop_item *focused(void) {
    if (b.show_plan) return NULL;
    if (b.detail_open) return &b.detail;
    if (b.tab == WORKSHOP_BROWSE && b.index < b.page.count) return &b.page.items[b.index];
    return NULL;
}

static preview_cache *cache_get(uint64_t id) {
    for (unsigned i = 0; i < THUMBS; ++i)
        if (b.thumbs[i].id == id) return &b.thumbs[i];
    return NULL;
}

static int start_thumbnail(const workshop_item *item) {
    if (!item || !item->id || (!item->preview[0] && !item->preview_alt[0]) || cache_get(item->id)) return 0;
    b.thumb_id = item->id;
    snprintf(b.thumb_url, sizeof b.thumb_url, "%s", item->preview[0] ? item->preview : item->preview_alt);
    snprintf(b.thumb_alt, sizeof b.thumb_alt, "%s", item->preview_alt);
    return post(JOB_THUMB, NULL) == 0;
}

static int start_illustration(void) {
    if (!b.detail_open || !b.media_count) return 0;
    if (b.media_page != b.description_page) {
        b.media_visible = isaac_launcher_workshop_description_media(b.description, b.description_page);
        b.media_page = b.description_page;
    }
    unsigned pending = b.media_visible;
    for (unsigned i = 0; i < MEDIA_CACHE; ++i)
        if (b.media[i].id) pending &= ~(1u << (b.media[i].id - 1));
    for (unsigned i = 0; i < b.media_count; ++i) {
        if (!(pending & (1u << i))) continue;
        b.media_id = i + 1;
        snprintf(b.thumb_url, sizeof b.thumb_url, "%s", b.media_url[i]);
        return post(JOB_ILLUSTRATION, NULL) == 0;
    }
    return 0;
}

static void lazy_preview(void) {
    if (start_illustration() || start_thumbnail(focused()) || b.detail_open ||
        b.show_plan || b.tab != WORKSHOP_BROWSE) return;
    for (size_t i = b.list_start; i < b.page.count && i < b.list_start + WORKSHOP_GRID_CARDS; ++i)
        if (start_thumbnail(&b.page.items[i])) break;
}

static size_t moved_index(uint64_t down, size_t count, int grid) {
    size_t step = grid ? WORKSHOP_GRID_COLUMNS : 1;
    if ((down & HidNpadButton_AnyUp) && b.index >= step) return b.index - step;
    if ((down & HidNpadButton_AnyDown) && b.index + step < count) return b.index + step;
    if (grid && (down & HidNpadButton_AnyDown) && b.index % step && b.index + 1 < count)
        return b.index + 1;
    if (grid && (down & HidNpadButton_AnyLeft) && b.index % step) return b.index - 1;
    if (grid && (down & HidNpadButton_AnyRight) && b.index % step + 1 < step && b.index + 1 < count)
        return b.index + 1;
    return b.index;
}

static void show_details(uint64_t id) {
    if (!online()) return;
    b.target = id;
    post(JOB_DETAILS, "Loading item details…");
}

static int selected_count(void) {
    int count = 0;
    for (size_t i = 0; i < b.plan.count; ++i) count += b.plan.rows[i].selected;
    return count;
}

static void confirm_plan(void) {
    unsigned remove = 0, subscribe = 0, unsubscribe = 0;
    for (size_t i = 0; i < b.plan.count; ++i) {
        const mod_change *change = &b.plan.rows[i];
        if (!change->selected) continue;
        remove += change->action == MOD_REMOVE;
        subscribe += change->action == MOD_SUBSCRIBE;
        unsubscribe += change->action == MOD_UNSUBSCRIBE;
    }
    snprintf(b.message, sizeof b.message,
             "Apply %d selected changes? %u local removals, %u Steam subscriptions, %u Steam unsubscriptions.",
             selected_count(), remove, subscribe, unsubscribe);
    b.confirming = JOB_APPLY;
}

static void begin_confirmation(int type, const char *message) {
    snprintf(b.message, sizeof b.message, "%s", message);
    b.confirming = type;
}

static void confirm_install(void) {
    const local_mod *installed = mod_find(&b.local, b.detail.id);
    if (b.detail.unavailable || !b.detail.manifest || b.detail.has_legacy_url ||
        (installed && !mod_update_needed(installed, &b.detail))) return;
    b.target = b.detail.id;
    begin_confirmation(JOB_INSTALL, installed ? "Update this mod on the Switch?" : "Install this mod locally?");
}

static void confirm_remove(const local_mod *mod) {
    b.target = mod->id;
    for (size_t i = 0; i < b.local.count; ++i) {
        const local_mod *dependent = &b.local.items[i];
        if (dependent->id == mod->id) continue;
        for (size_t d = 0; d < dependent->dep_count; ++d) {
            if (dependent->deps[d] != mod->id) continue;
            snprintf(b.message, sizeof b.message, "Remove this mod from the Switch? Required by %.120s.",
                     dependent->title);
            b.confirming = JOB_REMOVE;
            return;
        }
    }
    begin_confirmation(JOB_REMOVE, "Remove this Workshop mod from the Switch?");
}

int workshop_browser_open(const char *root, const char *saves, const char *token, int installed) {
    memset(&b, 0, sizeof b);
    b.token = token;
    if (mod_store_init(&b.store, root, saves, installed)) return -1;
    return post(JOB_LOCAL, "Reading installed mods…");
}

int workshop_browser_input(uint64_t down, int signed_in) {
    complete_job();
    int grid = !b.detail_open && !b.show_plan && b.tab == WORKSHOP_BROWSE;
    if (image_job() && down) {
        const uint64_t directions = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                                    HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        if (grid && !(down & ~directions)) {
            size_t next = moved_index(down, b.page.count, 1);
            if (next != b.index) {
                b.index = next;
                return 0;
            }
        }
        if (b.detail_open && !(down & ~directions)) {
            if ((down & HidNpadButton_AnyUp) && b.description_page > 0) --b.description_page;
            if ((down & HidNpadButton_AnyDown) &&
                b.description_page + 1 < b.description_pages) ++b.description_page;
            return 0;
        }
        b.deferred |= down;
        atomic_store(&b.cancel, 1);
        return 0;
    }
    if (b.job == JOB_QUICK_INSTALL) {
        if (down & HidNpadButton_B) {
            b.queued = 0;
            atomic_store(&b.cancel, 1);
            snprintf(b.message, sizeof b.message, "Cancelling queue…");
        } else if (!cancelled(NULL)) {
            b.index = moved_index(down, b.page.count, 1);
            if ((down & HidNpadButton_ZL) && b.page_no > b.oldest_page) query(b.page_no - 1, 0);
            if ((down & HidNpadButton_ZR) && b.cursor[(b.page_no + 1) % CURSOR_HISTORY][0]) query(b.page_no + 1, 0);
            if ((down & HidNpadButton_Plus) && b.index < b.page.count)
                queue_install(&b.page.items[b.index]);
        }
        return 0;
    }
    if (b.job) {
        if (down & HidNpadButton_B) {
            atomic_store(&b.cancel, 1);
            snprintf(b.message, sizeof b.message, "Cancelling…");
        }
        return 0;
    }
    down |= b.deferred;
    b.deferred = 0;
    b.signed_in = signed_in && !b.expired;
    if (b.queued && (down & HidNpadButton_B)) {
        b.queued = 0;
        snprintf(b.message, sizeof b.message, "Queue cancelled. %u installed.", b.installed_count);
        return 0;
    }
    if (b.queued) {
        start_queued_install();
        return 0;
    }
    if (b.confirming) {
        if (down & HidNpadButton_B) {
            b.confirming = 0;
            b.message[0] = 0;
        } else if (down & HidNpadButton_A) {
            enum job job = b.confirming;
            b.confirming = 0;
            post(job, job == JOB_REMOVE ? "Removing mod…" : job == JOB_INSTALL ?
                      "Downloading and verifying mod…" : "Applying confirmed changes…");
        }
        return 0;
    }
    if (down & HidNpadButton_B) {
        if (b.show_plan) {
            mod_plan_clear(&b.plan);
            b.show_plan = 0;
            b.index = 0;
            b.message[0] = 0;
        } else if (b.detail_open) {
            b.detail_open = 0;
            b.message[0] = 0;
        } else return WORKSHOP_EXIT;
        return 0;
    }
    if (!b.signed_in && (down & HidNpadButton_Y)) return WORKSHOP_SIGNIN;
    if (!b.detail_open && !b.show_plan && (down & (HidNpadButton_L | HidNpadButton_R))) {
        b.tab = (b.tab + ((down & HidNpadButton_R) ? 1 : WORKSHOP_TAB_COUNT - 1)) % WORKSHOP_TAB_COUNT;
        b.index = 0;
        b.message[0] = 0;
    }
    if (!b.detail_open) {
        size_t count = item_count();
        size_t next = moved_index(down, count, grid);
        if (next != b.index) b.index = next;
        else if (grid && b.browse_loaded && !b.net_failed) {
            if ((down & HidNpadButton_AnyUp) && b.page_no > b.oldest_page) {
                query(b.page_no - 1, 1);
                return 0;
            } else if ((down & HidNpadButton_AnyDown) && b.cursor[(b.page_no + 1) % CURSOR_HISTORY][0]) {
                query(b.page_no + 1, 0);
                return 0;
            }
        }
    }
    if (b.show_plan) {
        if (down & HidNpadButton_A) mod_plan_toggle(&b.plan, b.index);
        if ((down & HidNpadButton_X) && selected_count())
            confirm_plan();
    } else if (b.detail_open) {
        if ((down & HidNpadButton_AnyUp) && b.description_page > 0) --b.description_page;
        if ((down & HidNpadButton_AnyDown) &&
            b.description_page + 1 < b.description_pages) ++b.description_page;
        const local_mod *installed = mod_find(&b.local, b.detail.id);
        if (down & HidNpadButton_A) {
            confirm_install();
        } else if ((down & HidNpadButton_X) && installed) {
            confirm_remove(installed);
        } else if ((down & HidNpadButton_Y) && (b.detail.dep_count || b.detail.deps_incomplete) && online()) {
            post(JOB_DEPENDENCIES, "Resolving required mods…");
        }
    } else if (b.tab == WORKSHOP_BROWSE) {
        if (down & HidNpadButton_Y) {
            char search[sizeof b.search];
            snprintf(search, sizeof search, "%s", b.search);
            if (keyboard(search, sizeof search, "Search the Isaac Workshop") == 0 && strcmp(search, b.search)) {
                snprintf(b.search, sizeof b.search, "%s", search);
                if (*b.search) b.sort = WORKSHOP_RELEVANT;
                reset_query();
            }
        } else if (down & HidNpadButton_X) {
            b.sort = (b.sort + 1) % WORKSHOP_SORT_COUNT;
            reset_query();
        } else if (down & HidNpadButton_Minus) {
            char tag[sizeof b.tag];
            snprintf(tag, sizeof tag, "%s", b.tag);
            if (keyboard(tag, sizeof tag, "Filter by Steam Workshop tag (blank clears)") == 0 && strcmp(tag, b.tag)) {
                snprintf(b.tag, sizeof b.tag, "%s", tag);
                reset_query();
            }
        } else if ((down & HidNpadButton_ZL) && b.page_no > b.oldest_page) query(b.page_no - 1, 0);
        else if ((down & HidNpadButton_ZR) && b.cursor[(b.page_no + 1) % CURSOR_HISTORY][0])
            query(b.page_no + 1, 0);
        else if ((down & HidNpadButton_Plus) && b.index < b.page.count && !b.net_failed) {
            queue_install(&b.page.items[b.index]);
            start_queued_install();
        } else if (down & HidNpadButton_A) {
            if (!b.browse_loaded || b.net_failed) query(b.page_no, 0);
            else if (b.index < b.page.count) show_details(b.page.items[b.index].id);
        }
    } else if (b.tab == WORKSHOP_INSTALLED) {
        const local_mod *mod = b.index < b.local.count ? &b.local.items[b.index] : NULL;
        if ((down & HidNpadButton_Y) && online()) post(JOB_UPDATES, "Checking Workshop versions…");
        else if ((down & HidNpadButton_X) && mod) {
            snprintf(b.folder, sizeof b.folder, "%s", mod->folder);
            b.toggle_on = !mod->enabled;
            post(JOB_TOGGLE, "Saving mod setting…");
        } else if ((down & HidNpadButton_Minus) && mod && mod->id) {
            confirm_remove(mod);
        } else if ((down & HidNpadButton_A) && mod && mod->id) show_details(mod->id);
    } else if (b.tab == WORKSHOP_SYNC) {
        if (down & HidNpadButton_X) b.mode = (b.mode + 1) % 3;
        if ((down & HidNpadButton_A) && online())
            post(JOB_SUBSCRIPTIONS, "Reading Steam subscriptions for review…");
    }
    if (!down && !b.job && b.tab == WORKSHOP_BROWSE && b.signed_in &&
        !b.browse_loaded && !b.net_failed) query(0, 0);
    else if (!down && !b.job && !b.confirming) lazy_preview();
    return 0;
}

static void pretty_bytes(char *out, size_t cap, uint64_t bytes) {
    if (bytes >= (1u << 20)) snprintf(out, cap, "%.1f MiB", (double)bytes / (1u << 20));
    else if (bytes >= 1024) snprintf(out, cap, "%.1f KiB", (double)bytes / 1024);
    else snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

static void date(char out[16], uint32_t epoch) {
    time_t t = epoch;
    struct tm result;
    if (!epoch || !gmtime_r(&t, &result) || !strftime(out, 16, "%Y-%m-%d", &result))
        snprintf(out, 16, "Unknown");
}

static const char *author(const workshop_item *item, char buf[48]) {
    if (item->author_name[0]) return item->author_name;
    if (item->author) snprintf(buf, 48, "Steam user %llu", (unsigned long long)item->author);
    else snprintf(buf, 48, "Unknown author");
    return buf;
}

static void item_view(workshop_view *v) {
    const workshop_item *item = &b.detail;
    const local_mod *local = mod_find(&b.local, item->id);
    char stamp[16], size[32], by[48];
    date(stamp, item->updated);
    pretty_bytes(size, sizeof size, item->size);
    snprintf(v->heading, sizeof v->heading, "%s", item->title);
    snprintf(v->context, sizeof v->context, "%.65s  •  %s  •  %s  •  #%llu",
             author(item, by), item->size ? size : "Size unavailable", stamp, (unsigned long long)item->id);
    snprintf(v->description, sizeof v->description, "%s", b.description);
    for (unsigned i = 0; i < MEDIA_CACHE; ++i) {
        if (!b.media[i].id) continue;
        unsigned index = b.media[i].id - 1;
        if (b.media[i].animation.count) v->media[index] = &b.media[i].animation;
        else v->media_failed |= 1u << index;
    }
    v->description_page = b.description_page;
    v->description_pages = b.description_pages;
    snprintf(v->search, sizeof v->search, "%.*s", (int)sizeof v->search - 1, item->tags);
    unsigned missing = 0;
    for (size_t i = 0; i < item->dep_count; ++i) if (!mod_find(&b.local, item->deps[i])) ++missing;
    snprintf(v->dependencies, sizeof v->dependencies, "%s   •   %zu required, %u missing%s",
             item->unavailable || !item->manifest || item->has_legacy_url ? "Unavailable" : local ? mod_update_needed(local, item) ? "Update available" : "Installed on Switch"
                   : "Available to install",
             item->dep_count, missing, item->deps_incomplete ? " (partial list)" : "");
    for (size_t i = 0; i < item->dep_count && i < 8; ++i) {
        size_t n = strlen(v->dependencies);
        snprintf(v->dependencies + n, sizeof v->dependencies - n, "%s%llu%s",
                 i ? ", " : "\n", (unsigned long long)item->deps[i],
                 mod_find(&b.local, item->deps[i]) ? "" : " (missing)");
    }
    if (!item->unavailable && item->manifest && !item->has_legacy_url && (!local || mod_update_needed(local, item)))
        v->hints[WORKSHOP_HINT_A] = local ? "Update" : "Install";
    if (local) v->hints[WORKSHOP_HINT_X] = "Remove";
    if (item->dep_count || item->deps_incomplete) v->hints[WORKSHOP_HINT_Y] = "Review dependencies";
    if (b.description_pages > 1) v->hints[WORKSHOP_HINT_SCROLL] = "Description";
    v->hints[WORKSHOP_HINT_B] = "Back";
}

static void list_view(workshop_view *v) {
    size_t count = item_count(), start = b.list_start;
    size_t visible = v->grid ? WORKSHOP_GRID_CARDS : WORKSHOP_VISIBLE_ROWS;
    size_t columns = v->grid ? WORKSHOP_GRID_COLUMNS : 1;
    size_t index = b.index < count ? b.index : count ? count - 1 : 0;
    size_t max_start = (count + columns - 1) / columns * columns;
    max_start = max_start > visible ? max_start - visible : 0;
    start = start / columns * columns;
    if (start > max_start) start = max_start;
    size_t selected_row = index / columns * columns;
    if (selected_row < start) start = selected_row;
    if (selected_row >= start + visible) start = selected_row - visible + columns;
    b.list_start = v->list_start = start;
    v->list_count = count;
    for (size_t i = start; i < count && i < start + visible; ++i) {
        workshop_row *row = &v->rows[v->row_count++];
        if (i == b.index) v->selected = (int)(i - start);
        if (b.show_plan) {
            const mod_change *change = &b.plan.rows[i];
            snprintf(row->title, sizeof row->title, "%s", change->title);
            snprintf(row->detail, sizeof row->detail, "%s  •  %s",
                     change->action == MOD_UNAVAILABLE ? "Skipped" : change->selected ? "Selected" : "Skipped",
                     action_name[change->action]);
        } else if (b.tab == WORKSHOP_BROWSE) {
            const workshop_item *item = &b.page.items[i];
            preview_cache *cache = cache_get(item->id);
            if (cache && cache->animation.count) {
                row->thumb = cache->animation.frames[0];
                if (i == b.index) row->animation = &cache->animation;
            }
            const local_mod *mod = mod_find(&b.local, item->id);
            char stamp[16], size[32], by[48];
            date(stamp, item->updated);
            pretty_bytes(size, sizeof size, item->size);
            snprintf(row->title, sizeof row->title, "%s", item->title);
            snprintf(row->detail, sizeof row->detail, "%.55s  •  %s  •  %s  •  %u subscribed%s",
                     author(item, by), item->size ? size : "Size unavailable", stamp, item->subscribers,
                     mod ? mod_update_needed(mod, item) ? "  •  Update" : "  •  Installed" : "");
        } else {
            const local_mod *mod = &b.local.items[i];
            unsigned missing = 0;
            for (size_t d = 0; d < mod->dep_count; ++d)
                if (!mod_find(&b.local, mod->deps[d])) ++missing;
            char size[32];
            pretty_bytes(size, sizeof size, mod->size);
            snprintf(row->title, sizeof row->title, "%s", mod->title);
            snprintf(row->detail, sizeof row->detail, "%s  •  %s  •  %s",
                     mod->id ? "Workshop managed" : "Manual", mod->enabled ? "Enabled" : "Disabled", size);
            if (missing || mod->deps_incomplete) {
                size_t len = strlen(row->detail);
                snprintf(row->detail + len, sizeof row->detail - len, "  •  %u missing%s",
                         missing, mod->deps_incomplete ? " (partial list)" : "");
            }
        }
    }
}

const workshop_view *workshop_browser_view(void) {
    workshop_view *v = &b.view;
    int busy = b.job != JOB_NONE && !image_job();
    if (!busy || b.job == JOB_QUICK_INSTALL) {
        memset(v, 0, sizeof *v);
        v->tab = b.tab;
        v->confirming = b.confirming;
        v->signed_in = b.signed_in;
        v->detail = b.detail_open;
        if (b.show_plan) {
            snprintf(v->heading, sizeof v->heading, "Review changes");
            snprintf(v->context, sizeof v->context, "%zu changes  •  %d selected  •  Local installs and Steam subscriptions are separate",
                     b.plan.count, selected_count());
            if (!b.plan.count) snprintf(v->description, sizeof v->description, "No changes needed.");
            if (b.plan.count) v->hints[WORKSHOP_HINT_A] = "Select / skip";
            if (selected_count()) v->hints[WORKSHOP_HINT_X] = "Continue";
            v->hints[WORKSHOP_HINT_B] = "Discard";
        } else if (b.detail_open) item_view(v);
        else if (b.tab == WORKSHOP_BROWSE) {
            v->grid = 1;
            snprintf(v->heading, sizeof v->heading, "Browse");
            snprintf(v->context, sizeof v->context, "%s  •  Page %u  •  %u results%s%.38s",
                     sort_name[b.sort], b.page_no + 1, b.page.total, *b.tag ? "  •  Tag " : "", b.tag);
            if (*b.search) snprintf(v->search, sizeof v->search, "Search  %s", b.search);
            if (!b.signed_in) snprintf(v->description, sizeof v->description, "Sign in to browse the Steam Workshop.");
            else if (!b.page.count)
                snprintf(v->description, sizeof v->description, "%s", b.browse_loaded ? "No items match this search." : "Press A to load the Workshop.");
            if (b.signed_in) {
                if (!b.browse_loaded || b.net_failed) v->hints[WORKSHOP_HINT_A] = "Load";
                else if (b.page.count) {
                    v->hints[WORKSHOP_HINT_A] = "Open";
                    const workshop_item *item = focused();
                    if (quick_available(item)) v->hints[WORKSHOP_HINT_PLUS] = "Install";
                }
                v->hints[WORKSHOP_HINT_X] = "Sort";
                v->hints[WORKSHOP_HINT_Y] = "Search";
                v->hints[WORKSHOP_HINT_MINUS] = "Tag";
                if (b.page_no > b.oldest_page || b.cursor[(b.page_no + 1) % CURSOR_HISTORY][0])
                    v->hints[WORKSHOP_HINT_PAGES] = "Pages";
            } else v->hints[WORKSHOP_HINT_Y] = "Steam QR sign-in";
        } else if (b.tab == WORKSHOP_INSTALLED) {
            snprintf(v->heading, sizeof v->heading, "Installed mods");
            snprintf(v->context, sizeof v->context, "%zu mods on this Switch", b.local.count);
            if (!b.local.count) snprintf(v->description, sizeof v->description, "There are no local mods.");
            const local_mod *mod = b.index < b.local.count ? &b.local.items[b.index] : NULL;
            if (mod) {
                v->hints[WORKSHOP_HINT_X] = mod->enabled ? "Disable" : "Enable";
                if (mod->id) {
                    v->hints[WORKSHOP_HINT_A] = "Details";
                    v->hints[WORKSHOP_HINT_MINUS] = "Remove";
                }
            }
            v->hints[WORKSHOP_HINT_Y] = b.signed_in ? "Check updates" : "Steam QR sign-in";
        } else if (b.tab == WORKSHOP_SYNC) {
            snprintf(v->heading, sizeof v->heading, "Sync Steam Mods");
            snprintf(v->context, sizeof v->context, "%s", mode_name[b.mode]);
            snprintf(v->description, sizeof v->description,
                     "Review your Steam subscriptions and local Workshop mods. Choose which changes to apply, then confirm.");
            if (b.signed_in) v->hints[WORKSHOP_HINT_A] = "Review changes";
            else v->hints[WORKSHOP_HINT_Y] = "Steam QR sign-in";
            v->hints[WORKSHOP_HINT_X] = "Change direction";
        }
        if (!b.detail_open) {
            list_view(v);
            if (v->grid && v->row_count) v->hints[WORKSHOP_HINT_SCROLL] = "Navigate";
            else if (v->list_count > (size_t)v->row_count) v->hints[WORKSHOP_HINT_SCROLL] = "Scroll";
            if (!b.show_plan) {
                v->hints[WORKSHOP_HINT_TABS] = "Tabs";
                v->hints[WORKSHOP_HINT_B] = "Go Back";
            }
        }
        const workshop_item *item = focused();
        preview_cache *cache = item ? cache_get(item->id) : NULL;
        if (cache && v->detail) v->thumb_animation = &cache->animation;
    } else if (busy) {
        if (b.job == JOB_APPLY) {
            unsigned total = atomic_load(&b.operations);
            if (total) snprintf(v->context, sizeof v->context, "Change %u of %u",
                                atomic_load(&b.operation), total);
        }
        if (!v->heading[0]) snprintf(v->heading, sizeof v->heading, "Workshop");
        v->confirming = 0;
        memset(v->hints, 0, sizeof v->hints);
        v->hints[WORKSHOP_HINT_B] = "Cancel";
    }
    v->busy = busy;
    v->done = atomic_load(&b.bytes);
    v->total = atomic_load(&b.total);
    snprintf(v->notice, sizeof v->notice, "%s", b.message);
    if (b.job == JOB_QUICK_INSTALL) {
        memset(v->hints, 0, sizeof v->hints);
        v->hints[WORKSHOP_HINT_B] = "Cancel Queue";
        v->hints[WORKSHOP_HINT_SCROLL] = "Navigate";
        v->hints[WORKSHOP_HINT_PAGES] = "Cached pages";
        if (b.queued < INSTALL_QUEUE && quick_available(focused()) && !cancelled(NULL))
            v->hints[WORKSHOP_HINT_PLUS] = "Queue mod";
        if (!cancelled(NULL))
            snprintf(v->notice, sizeof v->notice, "Installing %.100s • %u queued",
                     b.active_install.title, b.queued);
    }
    return v;
}

void workshop_browser_close(void) {
    if (b.job) {
        atomic_store(&b.cancel, 1);
        pthread_join(b.thread, NULL);
    }
    for (unsigned i = 0; i < THUMBS; ++i) workshop_animation_clear(&b.thumbs[i].animation);
    clear_pages();
    clear_media();
    workshop_animation_clear(&b.next_media);
    free(b.local.items);
    free(b.quick_local.items);
    mod_plan_clear(&b.plan);
    memset(&b, 0, sizeof b);
}
