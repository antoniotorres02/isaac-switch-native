#ifndef ISAAC_NATIVE_WORKSHOP_BROWSER_H
#define ISAAC_NATIVE_WORKSHOP_BROWSER_H

#include "native_workshop_media.h"

enum { WORKSHOP_BROWSE, WORKSHOP_INSTALLED, WORKSHOP_SYNC, WORKSHOP_TAB_COUNT };
enum { WORKSHOP_SIGNIN = 1, WORKSHOP_EXIT = 2 };
#define WORKSHOP_VISIBLE_ROWS 11
#define WORKSHOP_GRID_COLUMNS 2
#define WORKSHOP_GRID_CARDS 8

enum workshop_hint {
    WORKSHOP_HINT_A, WORKSHOP_HINT_X, WORKSHOP_HINT_Y, WORKSHOP_HINT_MINUS,
    WORKSHOP_HINT_PLUS, WORKSHOP_HINT_SCROLL, WORKSHOP_HINT_PAGES, WORKSHOP_HINT_TABS, WORKSHOP_HINT_B,
    WORKSHOP_HINT_COUNT
};

typedef struct {
    char title[WORKSHOP_TITLE], detail[160];
    const workshop_thumb *thumb;
    const workshop_animation *animation;
} workshop_row;

typedef struct {
    int tab, selected, row_count, grid, detail, busy, confirming, signed_in;
    int description_page, description_pages;
    char heading[WORKSHOP_TITLE], context[160], notice[WORKSHOP_ERR];
    const char *hints[WORKSHOP_HINT_COUNT];
    char description[4096], dependencies[384], search[160];
    workshop_row rows[WORKSHOP_VISIBLE_ROWS];
    size_t list_start, list_count;
    const workshop_thumb *thumb;
    const workshop_animation *thumb_animation;
    const workshop_animation *media[WORKSHOP_MEDIA_IMAGES];
    unsigned media_failed;
    uint64_t done, total;
} workshop_view;

int workshop_browser_open(const char *root, const char *saves, const char *token, int installed);
int workshop_browser_input(uint64_t down, int signed_in);
const workshop_view *workshop_browser_view(void);
void workshop_browser_close(void);

#endif
