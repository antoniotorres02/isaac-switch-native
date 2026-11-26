#ifndef ISAAC_NATIVE_STEAMWORKSHOP_H
#define ISAAC_NATIVE_STEAMWORKSHOP_H

#include <stddef.h>
#include <stdint.h>

#define WORKSHOP_APP 250900u
#define WORKSHOP_DEPOT 250900u
#define WORKSHOP_PAGE 24
#define WORKSHOP_SUBS_PAGE 8
#define WORKSHOP_DEPS 64
#define WORKSHOP_TITLE 160
#define WORKSHOP_CURSOR 256
#define WORKSHOP_ERR 192

typedef struct {
    uint64_t id, manifest, author, size;
    uint32_t updated, subscribers, subscribed_at;
    uint64_t deps[WORKSHOP_DEPS];
    size_t dep_count;
    unsigned unavailable, type, deps_incomplete, has_legacy_url;
    char title[WORKSHOP_TITLE], description[4096], preview[1024], preview_alt[1024];
    char tags[192], author_name[80];
} workshop_item;

typedef struct {
    workshop_item items[WORKSHOP_PAGE];
    size_t count;
    uint32_t total, start;
    char cursor[WORKSHOP_CURSOR];
} workshop_page;

enum workshop_sort {
    WORKSHOP_POPULAR, WORKSHOP_SUBSCRIBED, WORKSHOP_UPDATED, WORKSHOP_NEW,
    WORKSHOP_RELEVANT, WORKSHOP_SORT_COUNT
};

typedef struct {
    void *ctx;
    uint64_t steamid;
    int (*call)(void *, const char *, const void *, size_t, void *, size_t, size_t *,
                char [WORKSHOP_ERR]);
    int (*get)(void *, const char *, const char *, void *, size_t, size_t *, int *,
               char [WORKSHOP_ERR]);
    int (*key)(void *, uint32_t, uint32_t, uint8_t [32], char [WORKSHOP_ERR]);
    int (*cancelled)(void *);
    void (*progress)(void *, uint64_t, uint64_t);
    unsigned cell;
    void (*operation)(void *, unsigned current, unsigned total);
} workshop_net;

int workshop_query(const workshop_net *, enum workshop_sort, const char *search,
                   const char *tag, const char *cursor, workshop_page *, char [WORKSHOP_ERR]);
int workshop_details(const workshop_net *, uint64_t id, workshop_item *, char [WORKSHOP_ERR]);
int workshop_subscriptions(const workshop_net *, unsigned page, workshop_page *,
                           char [WORKSHOP_ERR]);
int workshop_membership(const workshop_net *, uint64_t id, int *, char [WORKSHOP_ERR]);
void workshop_authors(const workshop_net *, workshop_item *, size_t);
int workshop_set_subscription(const workshop_net *, uint64_t id, int subscribed, char [WORKSHOP_ERR]);

#endif
