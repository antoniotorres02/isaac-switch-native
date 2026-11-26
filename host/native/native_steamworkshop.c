#include "native_steamworkshop.h"
#include "native_steamproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLY_CAP (64u << 10)

static int fail(char err[WORKSHOP_ERR], const char *message) {
    snprintf(err, WORKSHOP_ERR, "%s", message);
    return -1;
}

static void copy_text(const isaac_pbfield *f, char *out, size_t cap) {
    size_t n = f->len < cap ? f->len : cap - 1;
    if (n < f->len)
        while (n && (f->bytes[n] & 0xc0) == 0x80) --n;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = f->bytes[i];
        out[i] = (c < 32 && c != '\n' && c != '\t') || c == 127 ? ' ' : (char)c;
    }
    out[n] = 0;
}

static int read_child(const isaac_pbfield *record, uint64_t *id) {
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, record->bytes, record->len);
    int rc;
    *id = 0;
    while ((rc = isaac_pbr_next(&r, &f)) > 0)
        if (f.field == 1 && f.wire == 0) *id = f.value;
    return rc || !*id ? -1 : 0;
}

static int parse_item(const isaac_pbfield *record, workshop_item *item) {
    isaac_pbr reader;
    isaac_pbfield f;
    unsigned app = 0, result = 1, blocked = 0, reported_deps = 0;
    memset(item, 0, sizeof *item);
    isaac_pbr_init(&reader, record->bytes, record->len);
    int rc;
    while ((rc = isaac_pbr_next(&reader, &f)) > 0) {
        if (f.wire == 0) {
            switch (f.field) {
            case 1: result = f.value; break;
            case 2: item->id = f.value; break;
            case 5: app = f.value; break;
            case 8: item->size = f.value; break;
            case 20: item->updated = f.value; break;
            case 28: case 32: blocked |= f.value != 0; break;
            case 34: item->type = f.value; break;
            case 36: item->subscribers = f.value; break;
            case 49: reported_deps = f.value; break;
            case 56: item->subscribed_at = f.value; break;
            }
        } else if (f.wire == 1) {
            if (f.field == 3) item->author = f.value;
            if (f.field == 14) item->manifest = f.value;
        } else if (f.wire == 2) {
            switch (f.field) {
            case 10:
                if (f.len) item->has_legacy_url = 1;
                break;
            case 11:
                if (f.len < sizeof item->preview && !memchr(f.bytes, 0, f.len))
                    copy_text(&f, item->preview, sizeof item->preview);
                break;
            case 45:
                if (f.len < sizeof item->preview_alt && !memchr(f.bytes, 0, f.len))
                    copy_text(&f, item->preview_alt, sizeof item->preview_alt);
                break;
            case 16: copy_text(&f, item->title, sizeof item->title); break;
            case 17: copy_text(&f, item->description, sizeof item->description); break;
            case 18:
                if (!item->description[0])
                    copy_text(&f, item->description, sizeof item->description);
                break;
            case 52: {
                char tag[64];
                if (isaac_pbr_str(f.bytes, f.len, 1, tag, sizeof tag) > 0) {
                    size_t used = strlen(item->tags);
                    snprintf(item->tags + used, sizeof item->tags - used, "%s%s", used ? ", " : "", tag);
                }
                break;
            }
            case 53: {
                uint64_t child;
                if (read_child(&f, &child)) return -1;
                if (item->dep_count < WORKSHOP_DEPS)
                    item->deps[item->dep_count++] = child;
                else
                    item->deps_incomplete = 1;
                break;
            }
            }
        }
    }
    if (rc < 0 || !item->id || (result == 1 && app != WORKSHOP_APP) ||
        (app && app != WORKSHOP_APP))
        return -1;
    item->deps_incomplete |= reported_deps > item->dep_count;
    item->unavailable = result != 1 || blocked || item->type != 0;
    if (!item->title[0]) snprintf(item->title, sizeof item->title, "Workshop item %llu",
                                  (unsigned long long)item->id);
    return 0;
}

static int request_page(const workshop_net *net, const char *method, const isaac_pbw *w,
                        unsigned item_field, unsigned mode, workshop_page *page,
                        char err[WORKSHOP_ERR]) {
    uint8_t *reply = malloc(REPLY_CAP);
    if (!reply) return fail(err, "Out of memory");
    size_t len = 0;
    int rc = net->call(net->ctx, method, w->buf, w->len, reply, REPLY_CAP, &len, err);
    if (rc == 0 && len > REPLY_CAP) rc = fail(err, "Workshop response too large");
    if (rc == 0) {
        isaac_pbfield f;
        isaac_pbr reader;
        memset(page, 0, sizeof *page);
        isaac_pbr_init(&reader, reply, len);
        while ((rc = isaac_pbr_next(&reader, &f)) > 0) {
            if (f.field == item_field && f.wire == 2) {
                if (page->count == WORKSHOP_PAGE ||
                    parse_item(&f, &page->items[page->count]) != 0) break;
                ++page->count;
            } else if (mode && f.field == 1 && f.wire == 0 && f.value <= UINT32_MAX) {
                page->total = f.value;
            } else if (mode == 2 && f.field == 2 && f.wire == 0 && f.value <= UINT32_MAX) {
                page->start = f.value;
            } else if (mode == 1 && f.field == 3 && f.wire == 2) {
                if (f.len >= sizeof page->cursor || memchr(f.bytes, 0, f.len)) break;
                memcpy(page->cursor, f.bytes, f.len);
                page->cursor[f.len] = 0;
            }
        }
        if (rc != 0) rc = fail(err, "Malformed or unexpected Workshop response");
    }
    free(reply);
    return rc;
}

int workshop_query(const workshop_net *net, enum workshop_sort sort, const char *search,
                   const char *tag, const char *cursor, workshop_page *page, char err[WORKSHOP_ERR]) {
    /* EPublishedFileQueryType differs from the ISteamUGC SDK enum. */
    static const unsigned order[] = {0, 9, 21, 1, 12};
    unsigned char req[1024];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    if ((unsigned)sort >= WORKSHOP_SORT_COUNT) return fail(err, "Invalid Workshop sort");
    isaac_pbw_varint(&w, 1, order[sort]);
    isaac_pbw_varint(&w, 3, WORKSHOP_PAGE);
    isaac_pbw_varint(&w, 5, WORKSHOP_APP);
    if (*tag) isaac_pbw_str(&w, 6, tag);
    if (*search) isaac_pbw_str(&w, 11, search);
    isaac_pbw_varint(&w, 12, 0); /* MatchingFileType_Items */
    /* Tags and descriptions are fetched when the item is opened. */
    isaac_pbw_varint(&w, 37, 1);
    isaac_pbw_str(&w, 39, *cursor ? cursor : "*");
    if (w.overflow) return fail(err, "Workshop search is too long");
    return request_page(net, "PublishedFile.QueryFiles#1", &w, 2, 1, page, err);
}

int workshop_details(const workshop_net *net, uint64_t id, workshop_item *item,
                     char err[WORKSHOP_ERR]) {
    unsigned char req[64];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_fixed64(&w, 1, id);
    isaac_pbw_varint(&w, 2, 1);
    isaac_pbw_varint(&w, 4, 1);
    isaac_pbw_varint(&w, 14, WORKSHOP_APP);
    workshop_page *page = malloc(sizeof *page);
    if (!page) return fail(err, "Out of memory");
    int rc = request_page(net, "PublishedFile.GetDetails#1", &w, 1, 0, page, err);
    if (!rc) {
        if (page->count == 1 && page->items[0].id == id) *item = page->items[0];
        else rc = fail(err, "Workshop item is unavailable");
    }
    free(page);
    return rc;
}

int workshop_subscriptions(const workshop_net *net, unsigned page_no, workshop_page *page,
                           char err[WORKSHOP_ERR]) {
    unsigned char req[128];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    if (!net->steamid || !page_no) return fail(err, "Steam sign-in required");
    isaac_pbw_fixed64(&w, 1, net->steamid);
    isaac_pbw_varint(&w, 2, WORKSHOP_APP);
    isaac_pbw_varint(&w, 4, page_no);
    isaac_pbw_varint(&w, 5, WORKSHOP_SUBS_PAGE);
    isaac_pbw_str(&w, 6, "mysubscriptions");
    isaac_pbw_str(&w, 7, "subscriptiondate");
    isaac_pbw_varint(&w, 19, 0);
    isaac_pbw_varint(&w, 21, 0);
    isaac_pbw_varint(&w, 24, 1);
    isaac_pbw_varint(&w, 32, 1);
    return request_page(net, "PublishedFile.GetUserFiles#1", &w, 3, 2, page, err);
}

int workshop_membership(const workshop_net *net, uint64_t id, int *subscribed,
                        char err[WORKSHOP_ERR]) {
    uint8_t req[32], reply[128];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, WORKSHOP_APP);
    isaac_pbw_fixed64(&w, 2, id);
    size_t len = 0;
    if (net->call(net->ctx, "PublishedFile.AreFilesInSubscriptionList#1", req, w.len,
                  reply, sizeof reply, &len, err)) return -1;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, reply, len);
    if (isaac_pbr_next(&r, &f) != 1 || f.field != 1 || f.wire != 2)
        return fail(err, "Steam did not confirm subscription status");
    isaac_pbr item;
    isaac_pbr_init(&item, f.bytes, f.len);
    uint64_t found = 0;
    int subscribed_value = 0, rc;
    while ((rc = isaac_pbr_next(&item, &f)) > 0) {
        if (f.field == 1 && f.wire == 1) found = f.value;
        if (f.field == 2 && f.wire == 0) {
            if (f.value > 1) return fail(err, "Invalid Steam subscription status");
            subscribed_value = f.value != 0;
        }
    }
    if (rc || found != id || isaac_pbr_next(&r, &f) != 0)
        return fail(err, "Steam did not confirm subscription status");
    *subscribed = subscribed_value;
    return 0;
}

int workshop_set_subscription(const workshop_net *net, uint64_t id, int subscribed,
                             char err[WORKSHOP_ERR]) {
    uint8_t req[32], reply[128];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, id);
    isaac_pbw_varint(&w, 3, WORKSHOP_APP);
    if (subscribed) isaac_pbw_varint(&w, 5, 0); /* Dependencies require a separate choice. */
    const char *method = subscribed ? "PublishedFile.Subscribe#1" : "PublishedFile.Unsubscribe#1";
    size_t len = 0;
    if (net->call(net->ctx, method, req, w.len, reply, sizeof reply, &len, err))
        return -1;
    int actual = 0;
    if (workshop_membership(net, id, &actual, err)) return -1;
    return actual == subscribed ? 0 : fail(err, "Steam did not confirm the subscription change");
}

void workshop_authors(const workshop_net *net, workshop_item *items, size_t count) {
    if (count > WORKSHOP_PAGE || (net->cancelled && net->cancelled(net->ctx))) return;
    uint8_t req[WORKSHOP_PAGE * 11], reply[16384];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    for (size_t i = 0; i < count; ++i)
        if (items[i].author) isaac_pbw_varint(&w, 1, items[i].author);
    if (!w.len) return;
    char err[WORKSHOP_ERR];
    size_t len;
    if (net->call(net->ctx, "Player.GetPlayerLinkDetails#1", req, w.len,
                  reply, sizeof reply, &len, err)) return;
    isaac_pbr outer;
    isaac_pbfield entry;
    isaac_pbr_init(&outer, reply, len);
    while (isaac_pbr_next(&outer, &entry) == 1) {
        if (entry.field != 1 || entry.wire != 2) continue;
        isaac_pbr inner;
        isaac_pbfield pub;
        isaac_pbr_init(&inner, entry.bytes, entry.len);
        while (isaac_pbr_next(&inner, &pub) == 1) {
            if (pub.field != 1 || pub.wire != 2) continue;
            uint64_t id = isaac_pbr_uint(pub.bytes, pub.len, 1, 0);
            if (!id) continue;
            isaac_pbr profile;
            isaac_pbfield f;
            isaac_pbr_init(&profile, pub.bytes, pub.len);
            while (isaac_pbr_next(&profile, &f) == 1) {
                if (f.field != 10 || f.wire != 2) continue;
                for (size_t i = 0; i < count; ++i)
                    if (items[i].author == id) copy_text(&f, items[i].author_name, sizeof items[i].author_name);
            }
        }
    }
}
