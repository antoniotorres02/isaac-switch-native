/* Nintendo Switch button glyphs, drawn from the game's own sheets.
 *
 * The PC game only knows the XboxOne/PS4/PS5 glyph sets (it picks one by
 * controller type, 0x0095afb8), but its buttons.anm2 and
 * controls_buttons.anm2 also carry Nicalis's Switch frames ("Switch",
 * "Switch_Pro") on the same sprite sheets. The .anm2 loader (sub_0040ed50)
 * hands each file's XML text to rapidxml; build_native.py's lift hook calls
 * isaac_hook_anm2_text at 0x40ee80, where the text pointer is at [ESP]. For
 * those two files the parser gets a copy whose XboxOne frames crop the Switch
 * glyphs instead; everything else in the file, and every other file, is left
 * alone. Nothing of the game's data is kept here: the copy is made from the
 * text the game itself just loaded. The copy lives in the guest heap and is
 * not freed (the loader frees only its own buffer): some 60-100 KB each time
 * one of the two files is loaded.
 *
 * ISAAC_NATIVE_SWITCH_GLYPHS: 0/unset off; 1 the Switch pad of native_switch.c
 * with A accepting in menus (prompts by label); 2 face-button prompts by
 * position (the bottom button is drawn where Xbox A is).
 *
 * Mapping. Nicalis's "Switch" frames go by position (Xbox Y -> Switch X,
 * A -> B, B -> A, X -> Y); mode 1 reverses the four face frames so the accept
 * prompt shows A. controls_buttons.anm2 is the drawing on the first room's
 * floor, about shooting, which stays by position in play; its "Switch_Pro"
 * frames are used as they are. In both, View/Back and Menu/Start swap: Nicalis
 * has "+" where Xbox has View, while here Minus is BACK and Plus is START, and
 * controls_buttons frames 24/25 (Start/Back arrows) take "+"/"-". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *recomp_mem_base;
extern uint32_t isaac_guest_alloc(uint32_t n);

typedef struct { int from, to; } remap;
static const remap BUTTONS_LABEL[] = { {0, 3}, {1, 2}, {2, 1}, {3, 0}, {14, 15}, {15, 14}, {-1, -1} };
static const remap BUTTONS_PLACE[] = { {14, 15}, {15, 14}, {-1, -1} };
static const remap CONTROLS[] = { {14, 15}, {15, 14}, {24, 14}, {25, 15}, {-1, -1} };
static const char *const KEYS[] = { "XCrop", "YCrop", "Width", "Height", "XPivot", "YPivot" };
#define NKEYS 6
#define MAX_FRAMES 64

static int g_mode = -1;
static unsigned g_rewritten;

typedef struct { char *p; size_t n, cap; } sbuf;
static void put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n + 1) cap *= 2;
        b->p = realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* [*start, *end) of the layer frames of animation `name`: from its first
 * <LayerAnimation to the matching </LayerAnimation>. */
static int layer_frames(const char *text, const char *name, const char **start, const char **end) {
    char pat[64];
    snprintf(pat, sizeof pat, "<Animation Name=\"%s\"", name);
    const char *a = strstr(text, pat);
    if (!a) return 0;
    const char *a_end = strstr(a, "</Animation>");
    const char *l = strstr(a, "<LayerAnimation ");
    if (!a_end || !l || l > a_end) return 0;
    const char *l_end = strstr(l, "</LayerAnimation>");
    if (!l_end || l_end > a_end) return 0;
    *start = l;
    *end = l_end;
    return 1;
}

/* The frame tags in [start, end): their positions, up to MAX_FRAMES. */
static int frame_tags(const char *start, const char *end, const char **tags) {
    int n = 0;
    for (const char *f = strstr(start, "<Frame "); f && f < end && n < MAX_FRAMES; f = strstr(f + 1, "<Frame "))
        tags[n++] = f;
    return n;
}

/* The value of attribute `key` in the tag at `tag`, into out. */
static int attr(const char *tag, const char *key, char *out, size_t cap) {
    const char *close = strstr(tag, "/>");
    char pat[32];
    snprintf(pat, sizeof pat, " %s=\"", key);
    const char *v = strstr(tag, pat);
    if (!v || !close || v > close) return 0;
    v += strlen(pat);
    const char *q = strchr(v, '"');
    if (!q || (size_t)(q - v) >= cap) return 0;
    memcpy(out, v, (size_t)(q - v));
    out[q - v] = 0;
    return 1;
}

/* Copies the tag at `tag` with the NKEYS attributes taken from `src`. */
static void put_frame(sbuf *b, const char *tag, const char *src) {
    const char *close = strstr(tag, "/>") + 2, *p = tag;
    while (p < close) {
        const char *best = NULL;
        size_t best_len = 0;
        int best_k = -1;
        for (int k = 0; k < NKEYS; ++k) {
            char pat[32];
            snprintf(pat, sizeof pat, " %s=\"", KEYS[k]);
            const char *v = strstr(p, pat);
            if (v && v < close && (!best || v < best)) { best = v; best_len = strlen(pat); best_k = k; }
        }
        if (!best) break;
        char val[32];
        put(b, p, (size_t)(best - p) + best_len);
        if (!attr(src, KEYS[best_k], val, sizeof val)) attr(tag, KEYS[best_k], val, sizeof val);
        put(b, val, strlen(val));
        p = strchr(best + best_len, '"');
    }
    put(b, p, (size_t)(close - p));
}

static char *rewrite(const char *text, const char *src_name, const remap *map) {
    const char *ds, *de, *ss, *se, *dst[MAX_FRAMES], *src[MAX_FRAMES];
    if (!layer_frames(text, "XboxOne", &ds, &de) || !layer_frames(text, src_name, &ss, &se)) return NULL;
    int nd = frame_tags(ds, de, dst), ns = frame_tags(ss, se, src);
    if (!nd || !ns) return NULL;
    sbuf b = {0};
    const char *p = text;
    for (int i = 0; i < nd; ++i) {
        int j = i;
        for (const remap *m = map; m->from >= 0; ++m)
            if (m->from == i) j = m->to;
        put(&b, p, (size_t)(dst[i] - p));
        if (j < ns) put_frame(&b, dst[i], src[j]);
        else put(&b, dst[i], (size_t)(strstr(dst[i], "/>") + 2 - dst[i]));
        p = strstr(dst[i], "/>") + 2;
    }
    put(&b, p, strlen(p));
    return b.p;
}

void isaac_hook_anm2_text(uint32_t esp) {
    if (g_mode < 0) {
        const char *e = getenv("ISAAC_NATIVE_SWITCH_GLYPHS");
        g_mode = e ? atoi(e) : 0;
    }
    if (g_mode <= 0) return;
    uint32_t text_va;
    memcpy(&text_va, recomp_mem_base + esp, 4);
    if (!text_va) return;
    const char *text = (const char *)(recomp_mem_base + text_va);
    /* The sprite sheet is named in the first lines; the rest is not read
     * unless it is one of the two. */
    char head[1025];
    size_t n = strnlen(text, sizeof head - 1);
    memcpy(head, text, n);
    head[n] = 0;
    const char *src_name;
    const remap *map;
    if (strstr(head, "Path=\"buttons.png\"")) {
        src_name = "Switch";
        map = g_mode == 2 ? BUTTONS_PLACE : BUTTONS_LABEL;
    } else if (strstr(head, "Path=\"controls_buttons.png\"")) {
        src_name = "Switch_Pro";
        map = CONTROLS;
    } else {
        return;
    }
    char *out = rewrite(text, src_name, map);
    if (!out) {
        fprintf(stderr, "[native] switch glyphs: %s frames not found, sheet left as is\n", src_name);
        return;
    }
    size_t len = strlen(out) + 1;
    uint32_t va = isaac_guest_alloc((uint32_t)len);
    if (va) {
        memcpy(recomp_mem_base + va, out, len);
        memcpy(recomp_mem_base + esp, &va, 4);       /* the parser's argument */
        if (g_rewritten++ < 8)
            fprintf(stderr, "[native] switch glyphs: XboxOne frames of the %s sheet now crop %s\n",
                    src_name[6] ? "controls" : "buttons", src_name);
    }
    free(out);
}
