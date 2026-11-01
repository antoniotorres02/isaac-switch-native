/* The launch screen. See native_launcher.h.
 *
 * Laid out like a game page of the Steam library: a hero banner with the
 * title, a play bar (the green Play button and the cloud status beside it),
 * then the sync steps and the six save files, and a footer with the buttons.
 * A conflict opens a dialog over all of it, as Steam's own cloud conflict
 * does. Everything is drawn here, shapes by coverage and text through
 * FreeType; the banner and the page background are drawn once and copied.
 *
 * Only the Switch build and the PC preview compile it: the PC game build has
 * no use for it and does not link FreeType. */
#if defined(__SWITCH__) || defined(ISAAC_LAUNCHER_PREVIEW)
#include "native_launcher.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#define W ISAAC_LAUNCHER_W
#define H ISAAC_LAUNCHER_H

/* Steam's palette. */
#define C_NAV        0x171a21
#define C_PAGE_TOP   0x1b2838
#define C_PAGE_BOT   0x121a24
#define C_TEXT       0xdcdedf
#define C_TEXT2      0xa7b0b8
#define C_DIM        0x6f7a85
#define C_ACCENT     0x66c0f4
#define C_BLUE       0x1a9fff
#define C_GREEN_L    0x75b022
#define C_GREEN_R    0x588a1b
#define C_OK         0x8bc53f
#define C_WARN       0xe8b33a
#define C_BAD        0xe0574a
#define C_PANEL      0x000000

#define HERO_BOTTOM 328
#define BAR_TOP     328
#define BAR_H       88
#define FOOT_TOP    680

static uint32_t *g_px;
static int g_stride;

/* ---- pixels ---------------------------------------------------------------- */

static inline uint32_t pack(uint32_t rgb) {
    return ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16) | 0xff000000u;
}

static inline void blend(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || (unsigned)x >= W || (unsigned)y >= H) return;
    uint32_t *d = &g_px[y * g_stride + x];
    if (a >= 255) { *d = pack(rgb); return; }
    int dr = *d & 0xff, dg = (*d >> 8) & 0xff, db = (*d >> 16) & 0xff;
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    dr += ((r - dr) * a) / 255;
    dg += ((g - dg) * a) / 255;
    db += ((b - db) * a) / 255;
    *d = (uint32_t)dr | (uint32_t)dg << 8 | (uint32_t)db << 16 | 0xff000000u;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        float ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void rect(int x, int y, int w, int h, uint32_t rgb, int a) {
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i) blend(i, j, rgb, a);
}

/* A rounded rectangle; with `c2` != c1 a gradient, left to right or top to
 * bottom. Coverage comes from the box's signed distance. */
static void rrect_grad(float x, float y, float w, float h, float r, uint32_t c1, uint32_t c2,
                       int vertical, int a) {
    float cx = x + w / 2, cy = y + h / 2, hw = w / 2, hh = h / 2;
    for (int j = (int)floorf(y); j < (int)ceilf(y + h); ++j) {
        for (int i = (int)floorf(x); i < (int)ceilf(x + w); ++i) {
            float qx = fabsf(i + 0.5f - cx) - (hw - r), qy = fabsf(j + 0.5f - cy) - (hh - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
            float cov = clamp01(0.5f - d);
            if (cov <= 0) continue;
            uint32_t c = c1 == c2 ? c1 : mix(c1, c2, vertical ? (j - y) / h : (i - x) / w);
            blend(i, j, c, (int)(cov * a));
        }
    }
}

static void rrect(float x, float y, float w, float h, float r, uint32_t rgb, int a) {
    rrect_grad(x, y, w, h, r, rgb, rgb, 0, a);
}

/* An outline `t` wide just inside the box. */
static void rrect_line(float x, float y, float w, float h, float r, float t, uint32_t rgb, int a) {
    float cx = x + w / 2, cy = y + h / 2, hw = w / 2, hh = h / 2;
    for (int j = (int)floorf(y); j < (int)ceilf(y + h); ++j) {
        for (int i = (int)floorf(x); i < (int)ceilf(x + w); ++i) {
            float qx = fabsf(i + 0.5f - cx) - (hw - r), qy = fabsf(j + 0.5f - cy) - (hh - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
            float cov = clamp01(0.5f - d) * clamp01(0.5f + d + t);
            if (cov > 0) blend(i, j, rgb, (int)(cov * a));
        }
    }
}

static void circle(float cx, float cy, float r, uint32_t rgb, int a) {
    for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); ++j)
        for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); ++i) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            float cov = clamp01(r + 0.5f - sqrtf(dx * dx + dy * dy));
            if (cov > 0) blend(i, j, rgb, (int)(cov * a));
        }
}

/* A stroke from (x0,y0) to (x1,y1), `t` wide, round ends. */
static void line(float x0, float y0, float x1, float y1, float t, uint32_t rgb, int a) {
    float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t;
    float miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int j = (int)miny; j <= (int)maxy; ++j)
        for (int i = (int)minx; i <= (int)maxx; ++i) {
            float px = i + 0.5f - x0, py = j + 0.5f - y0;
            float k = len2 > 0 ? clamp01((px * vx + py * vy) / len2) : 0;
            float dx = px - vx * k, dy = py - vy * k;
            float cov = clamp01(t / 2 + 0.5f - sqrtf(dx * dx + dy * dy));
            if (cov > 0) blend(i, j, rgb, (int)(cov * a));
        }
}

static void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, int a) {
    float xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    float sign = area < 0 ? -1.0f : 1.0f;
    for (int j = (int)fminf(y0, fminf(y1, y2)) - 1; j <= (int)fmaxf(y0, fmaxf(y1, y2)) + 1; ++j)
        for (int i = (int)fminf(x0, fminf(x1, x2)) - 1; i <= (int)fmaxf(x0, fmaxf(x1, x2)) + 1; ++i) {
            float d = 1e9f;
            for (int e = 0; e < 3; ++e) {
                float ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
                float ex = bx - ax, ey = by - ay, l = sqrtf(ex * ex + ey * ey);
                float de = sign * (ex * (j + 0.5f - ay) - ey * (i + 0.5f - ax)) / l;
                if (de < d) d = de;
            }
            float cov = clamp01(d + 0.5f);
            if (cov > 0) blend(i, j, rgb, (int)(cov * a));
        }
}

/* An arc of a ring: `start` and `span` in turns, clockwise from twelve. */
static void arc(float cx, float cy, float r, float t, float start, float span, uint32_t rgb, int a) {
    const float TAU = 6.2831853f;
    for (int j = (int)(cy - r - t); j <= (int)(cy + r + t); ++j)
        for (int i = (int)(cx - r - t); i <= (int)(cx + r + t); ++i) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy, dist = sqrtf(dx * dx + dy * dy);
            float cov = clamp01(t / 2 + 0.5f - fabsf(dist - r));
            if (cov <= 0) continue;
            float turn = atan2f(dx, -dy) / TAU;
            float rel = turn - start;
            rel -= floorf(rel);
            if (rel > span) continue;
            /* fade the tail in */
            blend(i, j, rgb, (int)(cov * a * (0.25f + 0.75f * rel / span)));
        }
}

static void spinner(float cx, float cy, float r, float t, uint32_t rgb) {
    arc(cx, cy, r, r * 0.28f, t * 1.3f, 0.72f, rgb, 255);
}

static void check(float cx, float cy, float s, uint32_t rgb) {
    line(cx - s * 0.45f, cy + s * 0.02f, cx - s * 0.12f, cy + s * 0.35f, s * 0.2f, rgb, 255);
    line(cx - s * 0.12f, cy + s * 0.35f, cx + s * 0.5f, cy - s * 0.35f, s * 0.2f, rgb, 255);
}

static void cross(float cx, float cy, float s, uint32_t rgb) {
    line(cx - s * 0.36f, cy - s * 0.36f, cx + s * 0.36f, cy + s * 0.36f, s * 0.2f, rgb, 255);
    line(cx - s * 0.36f, cy + s * 0.36f, cx + s * 0.36f, cy - s * 0.36f, s * 0.2f, rgb, 255);
}

/* An arrow pointing down (dir 1) or up (dir -1). */
static void arrow(float cx, float cy, float s, int dir, uint32_t rgb) {
    line(cx, cy - dir * s * 0.45f, cx, cy + dir * s * 0.1f, s * 0.2f, rgb, 255);
    triangle(cx - s * 0.38f, cy + dir * s * 0.02f, cx + s * 0.38f, cy + dir * s * 0.02f,
             cx, cy + dir * s * 0.48f, rgb, 255);
}

static void cloud_icon(float x, float y, float s, uint32_t rgb, int a) {
    circle(x + s * 0.32f, y + s * 0.62f, s * 0.24f, rgb, a);
    circle(x + s * 0.55f, y + s * 0.46f, s * 0.32f, rgb, a);
    circle(x + s * 0.78f, y + s * 0.64f, s * 0.22f, rgb, a);
    rrect(x + s * 0.3f, y + s * 0.6f, s * 0.5f, s * 0.26f, 1, rgb, a);
}

/* ---- text ------------------------------------------------------------------ */

static FT_Library g_ft;
static FT_Face g_face;

typedef struct {
    uint32_t key;
    int16_t w, h, left, top, adv;
    uint8_t *bmp;
} glyph;
#define GLYPHS 4096
static glyph g_glyphs[GLYPHS];

int isaac_launcher_set_font(const void *data, size_t size) {
    if (!g_ft && FT_Init_FreeType(&g_ft)) return -1;
    if (g_face) { FT_Done_Face(g_face); g_face = NULL; }
    for (int i = 0; i < GLYPHS; ++i) { free(g_glyphs[i].bmp); g_glyphs[i] = (glyph){ 0 }; }
    return FT_New_Memory_Face(g_ft, data, (FT_Long)size, 0, &g_face) ? -1 : 0;
}

/* One glyph at `px` pixels, bold by emboldening the outline (the console's
 * shared font has a single weight). */
static const glyph *get_glyph(uint32_t cp, int px, int bold) {
    static glyph none;
    if (!g_face || cp > 0x1fffff || px > 127) return &none;
    uint32_t key = cp | (uint32_t)px << 21 | (uint32_t)bold << 28;
    uint32_t h = (key * 2654435761u) >> 20;
    for (int probe = 0; probe < GLYPHS; ++probe) {
        glyph *g = &g_glyphs[(h + probe) & (GLYPHS - 1)];
        if (g->key == key) return g;
        if (g->key) continue;
        g->key = key;
        if (FT_Set_Pixel_Sizes(g_face, 0, px) ||
            FT_Load_Char(g_face, cp, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT))
            return g;
        FT_GlyphSlot slot = g_face->glyph;
        FT_Pos strength = bold ? px * 64 / 22 : 0;
        if (bold && slot->format == FT_GLYPH_FORMAT_OUTLINE) FT_Outline_EmboldenXY(&slot->outline, strength, 0);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return g;
        FT_Bitmap *b = &slot->bitmap;
        g->w = (int16_t)b->width;
        g->h = (int16_t)b->rows;
        g->left = (int16_t)slot->bitmap_left;
        g->top = (int16_t)slot->bitmap_top;
        g->adv = (int16_t)((slot->advance.x + strength + 32) >> 6);
        if (g->w && g->h && (g->bmp = malloc((size_t)g->w * g->h)))
            for (int r = 0; r < g->h; ++r) memcpy(g->bmp + r * g->w, b->buffer + r * b->pitch, (size_t)g->w);
        return g;
    }
    return &none;
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
    *s = (const char *)p;
    return c;
}

enum { REG = 0, BOLD = 1 };

/* Text from `x` on the baseline `y`; `track` adds pixels between letters.
 * Returns the width. With `rgb` ~0 it only measures. */
static int text_ex(int x, int y, const char *s, int px, int bold, int track, uint32_t rgb, int a) {
    int pen = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const glyph *g = get_glyph(cp, px, bold);
        if (rgb != ~0u && g->bmp)
            for (int r = 0; r < g->h; ++r)
                for (int c = 0; c < g->w; ++c) {
                    int cov = g->bmp[r * g->w + c];
                    if (cov) blend(pen + g->left + c, y - g->top + r, rgb, cov * a / 255);
                }
        pen += g->adv + track;
    }
    return pen - x - (pen > x ? track : 0);
}

static int text(int x, int y, const char *s, int px, int bold, uint32_t rgb) {
    return text_ex(x, y, s, px, bold, 0, rgb, 255);
}

static int text_w(const char *s, int px, int bold, int track) {
    return text_ex(0, 0, s, px, bold, track, ~0u, 0);
}

/* Small capitals the way Steam labels its columns. */
static int label(int x, int y, const char *s, uint32_t rgb) {
    return text_ex(x, y, s, 12, BOLD, 1, rgb, 255);
}

/* Cut to `max` pixels with an ellipsis. */
static void fit(char *out, size_t cap, const char *s, int px, int bold, int max) {
    snprintf(out, cap, "%s", s);
    if (text_w(out, px, bold, 0) <= max) return;
    size_t n = strlen(out);
    while (n > 0) {
        do --n; while (n > 0 && ((unsigned char)out[n] & 0xc0) == 0x80);
        snprintf(out + n, cap - n, "\xe2\x80\xa6");
        if (text_w(out, px, bold, 0) <= max) return;
    }
}

/* Wraps on spaces inside `w`; returns the y after the last line. */
static int paragraph(int x, int y, int w, const char *s, int px, int lh, uint32_t rgb, int max_lines) {
    char line_buf[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        size_t best = 0, n = 0;
        while (s[n]) {
            size_t m = n;
            while (s[m] && s[m] != ' ') ++m;
            if (m >= sizeof line_buf) break;
            memcpy(line_buf, s, m);
            line_buf[m] = 0;
            if (text_w(line_buf, px, REG, 0) > w && best) break;
            best = m;
            if (!s[m]) break;
            n = m + 1;
        }
        if (!best) best = strlen(s) < sizeof line_buf - 1 ? strlen(s) : sizeof line_buf - 1;
        memcpy(line_buf, s, best);
        line_buf[best] = 0;
        if (++lines == max_lines && s[best]) {
            char cut[256];
            fit(cut, sizeof cut, s, px, REG, w);
            snprintf(line_buf, sizeof line_buf, "%s", cut);
        }
        text(x, y, line_buf, px, REG, rgb);
        y += lh;
        s += best;
        while (*s == ' ') ++s;
    }
    return y;
}

/* A pad button as Switch hints draw it: a disc with its letter. */
static int pad_button(int x, int cy, const char *letter) {
    circle(x + 12, cy, 12, C_TEXT, 255);
    int lw = text_w(letter, 15, BOLD, 0);
    text(x + 12 - lw / 2, cy + 6, letter, 15, BOLD, C_NAV);
    return 24;
}

/* ---- the background, drawn once ----------------------------------------------- */

static uint32_t *g_bg;

static float hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

/* The banner: a dim basement glow behind the title, a few tears drifting in
 * it, darker towards the title and fading into the page. */
static void draw_background(void) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t c;
            if (y < HERO_BOTTOM + 60) {
                float fy = y / (float)HERO_BOTTOM;
                c = mix(0x2c1913, 0x140d0c, fy);
                float dx = (x - 900) / 560.0f, dy = (y - 120) / 360.0f;
                float glow = clamp01(1 - sqrtf(dx * dx + dy * dy));
                c = mix(c, 0x7a4027, glow * glow * 0.75f);
                float dx2 = (x - 240) / 700.0f, dy2 = (y - 360) / 300.0f;
                float cool = clamp01(1 - sqrtf(dx2 * dx2 + dy2 * dy2));
                c = mix(c, 0x1d2a3c, cool * 0.5f);
                c = mix(c, 0x000000, clamp01((620 - x) / 900.0f) * 0.45f);
                float n = (hash2(x, y) - 0.5f) * 0.06f;
                c = mix(c, n > 0 ? 0xffffff : 0x000000, fabsf(n));
                float fade = clamp01((y - (HERO_BOTTOM - 140)) / 200.0f);
                c = mix(c, C_PAGE_TOP, fade * fade);
            } else {
                c = mix(C_PAGE_TOP, C_PAGE_BOT, (y - HERO_BOTTOM) / (float)(H - HERO_BOTTOM));
            }
            g_px[y * g_stride + x] = pack(c);
        }
    }
    static const struct { short x, y, r; } TEARS[] = {
        { 720, 70, 38 }, { 860, 200, 22 }, { 1010, 96, 56 }, { 1150, 236, 30 },
        { 1210, 60, 18 }, { 930, 300, 14 }, { 640, 250, 12 }, { 1080, 300, 9 },
    };
    for (size_t i = 0; i < sizeof TEARS / sizeof TEARS[0]; ++i) {
        float x = TEARS[i].x, y = TEARS[i].y, r = TEARS[i].r;
        circle(x, y, r, 0x8fb6e8, 26);
        circle(x - r * 0.3f, y - r * 0.32f, r * 0.28f, 0xffffff, 28);
    }
    /* the title on the banner, bottom left like a library logo */
    text_ex(52, 222, "The Binding of Isaac", 60, BOLD, 0, 0x000000, 120);
    text(48, 218, "The Binding of Isaac", 60, BOLD, 0xf3ece4);
    text_ex(51, 264, "REPENTANCE+", 26, BOLD, 6, 0x000000, 140);
    text_ex(50, 262, "REPENTANCE+", 26, BOLD, 6, 0xe2b464, 255);
    text_ex(50, 293, "Native port for Nintendo Switch \xc2\xb7 v1.9.7.17", 15, REG, 0, 0xb9aea4, 255);
    /* the bars */
    for (int y = 0; y < 52; ++y)
        for (int x = 0; x < W; ++x) blend(x, y, C_NAV, y < 48 ? 235 : 255 - (y - 48) * 50);
    rect(0, BAR_TOP, W, BAR_H, 0x0f1620, 190);
    rect(0, BAR_TOP, W, 1, 0x3a4b5e, 120);
    rect(0, FOOT_TOP, W, H - FOOT_TOP, C_NAV, 255);
    rect(0, FOOT_TOP, W, 1, 0x2a3442, 255);
}

/* ---- the screen --------------------------------------------------------------- */

#ifndef ISAAC_NO_STEAM
static const char *const STEP_NAMES[ISAAC_LAUNCHER_STEPS] = {
    "Steam sign-in", "System clock", "Connect to Steam", "Sign in", "Game on this account", "Compare saves",
};

static const char *slot_text(int status, int phase, uint32_t *rgb) {
    switch (status) {
    case ISAAC_SYNC_SLOT_SAME: *rgb = C_OK; return "Up to date";
    case ISAAC_SYNC_SLOT_DOWNLOADED: *rgb = C_ACCENT; return "Downloaded";
    case ISAAC_SYNC_SLOT_UPLOAD: *rgb = C_WARN; return "Uploads on exit";
    case ISAAC_SYNC_SLOT_LOCAL_ONLY: *rgb = C_TEXT2; return "On this console";
    case ISAAC_SYNC_SLOT_FAILED: *rgb = C_BAD; return "Download failed";
    case ISAAC_SYNC_SLOT_EMPTY: *rgb = C_DIM; return "Empty";
    }
    *rgb = C_DIM;
    return phase == ISAAC_LAUNCHER_SYNCING ? "Checking\xe2\x80\xa6" : "Not checked";
}

static void slot_icon(float cx, float cy, int status, int phase, float t) {
    uint32_t rgb;
    slot_text(status, phase, &rgb);
    switch (status) {
    case ISAAC_SYNC_SLOT_SAME: check(cx, cy, 12, rgb); break;
    case ISAAC_SYNC_SLOT_DOWNLOADED: arrow(cx, cy, 13, 1, rgb); break;
    case ISAAC_SYNC_SLOT_UPLOAD: arrow(cx, cy, 13, -1, rgb); break;
    case ISAAC_SYNC_SLOT_FAILED: cross(cx, cy, 12, rgb); break;
    case ISAAC_SYNC_SLOT_LOCAL_ONLY: circle(cx, cy, 3.5f, rgb, 255); break;
    case ISAAC_SYNC_SLOT_EMPTY: line(cx - 5, cy, cx + 5, cy, 2, rgb, 255); break;
    default:
        if (phase == ISAAC_LAUNCHER_SYNCING) spinner(cx, cy, 6, t, C_DIM);
        else line(cx - 5, cy, cx + 5, cy, 2, rgb, 255);
    }
}

#endif /* !ISAAC_NO_STEAM */

static void draw_nav(const isaac_launcher_view *v) {
#ifdef ISAAC_NO_STEAM
    (void)v;
    text_ex(40, 32, "ISAAC ON SWITCH", 16, BOLD, 2, C_TEXT, 255);
#else
    cloud_icon(40, 12, 28, C_ACCENT, 255);
    text_ex(80, 32, "ISAAC ON SWITCH", 16, BOLD, 2, C_TEXT, 255);
    /* the account, top right */
    char who[64];
    if (v->account[0]) snprintf(who, sizeof who, "%s", v->account);
    else snprintf(who, sizeof who, "Not signed in");
    int ww = text_w(who, 15, REG, 0);
    int ax = W - 40 - ww;
    text(ax, 31, who, 15, REG, v->account[0] ? C_TEXT : C_DIM);
    rrect_grad(ax - 42, 10, 30, 30, 3, 0x2f6fa8, 0x1a3c62, 1, 255);
    circle(ax - 27, 21, 5.5f, 0xb7d4ef, 255);
    rrect(ax - 37, 29, 20, 8, 4, 0xb7d4ef, 255);
    circle(ax - 14, 36, 4, v->account[0] ? 0x57cbde : 0x6f7a85, 255);
#endif
}

/* The files' state, in one column of the play bar. */
static void draw_files_status(int x, int y1, int y2, const isaac_launcher_view *v) {
    label(x, y1, "GAME FILES", C_DIM);
    uint32_t rgb = v->files == ISAAC_FILES_OK ? C_OK : v->files == ISAAC_FILES_BAD ? C_BAD : C_DIM;
    if (v->files == ISAAC_FILES_OK) check(x + 8, y2 - 6, 13, rgb);
    else if (v->files == ISAAC_FILES_BAD) cross(x + 8, y2 - 6, 13, rgb);
    else circle(x + 8, y2 - 6, 4, rgb, 255);
    text(x + 24, y2, v->files_summary[0] ? v->files_summary : "Not checked", 18, REG, C_TEXT);
}

/* Why Play is off, over the banner. */
static void draw_files_notice(const isaac_launcher_view *v) {
    if (v->files != ISAAC_FILES_BAD) return;
    const int x = 660, y = BAR_TOP - 96, w = W - 40 - x, h = 84;
    rrect(x, y, w, h, 4, 0x10161e, 225);
    rect(x, y, 3, h, C_BAD, 255);
    text(x + 16, y + 26, "The game's files are not complete", 17, BOLD, C_TEXT);
    paragraph(x + 16, y + 48, w - 32, v->files_detail, 13, 17, C_TEXT2, 2);
}

static void draw_play_bar(const isaac_launcher_view *v) {
    const float bx = 40, by = BAR_TOP + 14, bw = 236, bh = 60;
    int ready = v->phase != ISAAC_LAUNCHER_SYNCING;
    if (v->files == ISAAC_FILES_BAD) {
        rrect(bx, by, bw, bh, 3, 0x3a414b, 255);
        cross(bx + 34, by + bh / 2, 20, C_TEXT2);
        text(bx + 62, by + 39, "NO GAME FILES", 18, BOLD, C_TEXT2);
    } else if (v->phase == ISAAC_LAUNCHER_LAUNCHING) {
        rrect_grad(bx, by, bw, bh, 3, 0x4e7a17, 0x3d6114, 0, 255);
        spinner(bx + 36, by + bh / 2, 11, v->t, C_TEXT);
        text(bx + 62, by + 39, "LAUNCHING", 22, BOLD, C_TEXT);
    } else if (ready) {
        float pulse = 0.5f + 0.5f * sinf(v->t * 3.2f);
        rrect(bx - 4, by - 4, bw + 8, bh + 8, 6, 0xffffff, (int)(40 + 50 * pulse));
        rrect_grad(bx, by, bw, bh, 3, mix(C_GREEN_L, 0xa6e040, 0.25f * pulse), C_GREEN_R, 0, 255);
        triangle(bx + 30, by + 18, bx + 30, by + 42, bx + 50, by + 30, 0xffffff, 255);
        text_ex(bx + 70, by + 40, "PLAY", 26, BOLD, 2, 0xffffff, 255);
    } else {
        rrect(bx, by, bw, bh, 3, 0x3a414b, 255);
        spinner(bx + 36, by + bh / 2, 11, v->t, C_TEXT2);
        text(bx + 62, by + 39, "SYNCING", 22, BOLD, C_TEXT2);
    }

    int x = 312, y1 = BAR_TOP + 34, y2 = BAR_TOP + 62;
#ifdef ISAAC_NO_STEAM
    draw_files_status(x, y1, y2, v);
#else
    /* cloud status */
    label(x, y1, "CLOUD STATUS", C_DIM);
    const char *st;
    uint32_t sc;
    switch (v->phase) {
    case ISAAC_LAUNCHER_SYNCING: st = v->conflict ? "Sync conflict" : "Syncing\xe2\x80\xa6"; sc = v->conflict ? C_WARN : C_ACCENT; break;
    case ISAAC_LAUNCHER_FAILED: st = "Unable to sync"; sc = C_BAD; break;
    case ISAAC_LAUNCHER_OFF: st = "Off"; sc = C_DIM; break;
    case ISAAC_LAUNCHER_SIGNED_OUT: st = "Not signed in"; sc = C_DIM; break;
    default: st = "Up to date"; sc = C_OK;
    }
    cloud_icon(x - 2, y2 - 19, 24, sc, 255);
    text(x + 28, y2, st, 18, REG, C_TEXT);

    /* what moved */
    int down = 0, up = 0, same = 0, failed = 0, known = 0;
    for (int i = 0; i < ISAAC_STEAMSYNC_SLOTS; ++i) {
        int s = v->slot[i];
        if (s != ISAAC_LAUNCHER_SLOT_PENDING) ++known;
        down += s == ISAAC_SYNC_SLOT_DOWNLOADED;
        up += s == ISAAC_SYNC_SLOT_UPLOAD;
        same += s == ISAAC_SYNC_SLOT_SAME;
        failed += s == ISAAC_SYNC_SLOT_FAILED;
    }
    x = 560;
    label(x, y1, "SAVE FILES", C_DIM);
    char summary[96];
    if (!known) snprintf(summary, sizeof summary, "%s", v->phase == ISAAC_LAUNCHER_SYNCING ? "Waiting for Steam" : "Kept on this console");
    else if (!down && !up && !failed) snprintf(summary, sizeof summary, "%d in sync", same);
    else {
        int n = 0;
        summary[0] = 0;
        if (down) n += snprintf(summary + n, sizeof summary - n, "%d downloaded", down);
        if (up) n += snprintf(summary + n, sizeof summary - n, "%s%d to upload", n ? " \xc2\xb7 " : "", up);
        if (failed) snprintf(summary + n, sizeof summary - n, "%s%d failed", n ? " \xc2\xb7 " : "", failed);
    }
    text(x, y2, summary, 18, REG, C_TEXT);

    x = 820;
    label(x, y1, "ON EXIT", C_DIM);
    text(x, y2, v->phase == ISAAC_LAUNCHER_DONE || v->phase == ISAAC_LAUNCHER_LAUNCHING ? "Uploads your progress"
                : v->phase == ISAAC_LAUNCHER_SYNCING ? "Waiting for the sync" : "Nothing is uploaded",
         18, REG, C_TEXT);

    draw_files_status(1056, y1, y2, v);
#endif
}

static void panel(int x, int y, int w, int h, const char *title) {
    rrect(x, y, w, h, 6, C_PANEL, 70);
    label(x + 20, y + 30, title, C_TEXT2);
    rect(x + 20, y + 42, w - 40, 1, 0x3a4b5e, 160);
}

#ifdef ISAAC_NO_STEAM
/* A build without Steam (build_native.py --no-steam): only the files to report. */
static void draw_files_panel(const isaac_launcher_view *v) {
    const int x = 40, y = BAR_TOP + BAR_H + 20, w = W - 80, h = FOOT_TOP - y - 12;
    panel(x, y, w, h, "ON THIS CONSOLE");
    text(x + 20, y + 76, "Game files", 16, BOLD, C_TEXT);
    text(x + 150, y + 76, "sdmc:/switch/isaac-recomp/guest", 15, REG, C_TEXT2);
    text(x + 20, y + 108, "Saves", 16, BOLD, C_TEXT);
    text(x + 150, y + 108, "sdmc:/switch/isaac-recomp/saves", 15, REG, C_TEXT2);
    text(x + 20, y + 140, "This build keeps everything on the SD card and does not use the network.", 14, REG, C_DIM);
}
#else
static void draw_steps(const isaac_launcher_view *v) {
    const int x = 40, y = BAR_TOP + BAR_H + 20, w = 580, h = FOOT_TOP - y - 12;
    panel(x, y, w, h, "STEAM CLOUD SYNC");
    int failed = -1;
    for (int i = 0; i < ISAAC_LAUNCHER_STEPS; ++i) {
        int ry = y + 70 + i * 23, s = v->step[i];
        if (s == ISAAC_STEP_FAIL && failed < 0) failed = i;
        float cx = x + 32, cy = ry - 6;
        switch (s) {
        case ISAAC_STEP_RUN: spinner(cx, cy, 7, v->t, C_ACCENT); break;
        case ISAAC_STEP_OK: check(cx, cy, 13, C_OK); break;
        case ISAAC_STEP_FAIL: cross(cx, cy, 13, C_BAD); break;
        case ISAAC_STEP_WARN: circle(cx, cy, 5, C_WARN, 255); break;
        default: circle(cx, cy, 3, 0x46525f, 255);
        }
        uint32_t c = s == ISAAC_STEP_WAIT || s == ISAAC_STEP_SKIP ? C_DIM : s == ISAAC_STEP_FAIL ? C_BAD : C_TEXT;
        text(x + 52, ry, STEP_NAMES[i], 16, s == ISAAC_STEP_RUN ? BOLD : REG, c);
        char d[80];
        fit(d, sizeof d, v->step_detail[i], 14, REG, 300);
        text(x + w - 20 - text_w(d, 14, REG, 0), ry, d, 14, REG, C_DIM);
    }
    if (v->error[0] && v->phase != ISAAC_LAUNCHER_SYNCING && v->phase != ISAAC_LAUNCHER_LAUNCHING) {
        int ey = y + 70 + ISAAC_LAUNCHER_STEPS * 23 - 8;
        uint32_t ec = v->phase == ISAAC_LAUNCHER_FAILED ? C_BAD
                    : v->phase == ISAAC_LAUNCHER_DONE ? C_WARN : C_DIM;
        rect(x + 20, ey, 3, 34, ec, 255);
        paragraph(x + 32, ey + 13, w - 60, v->error, 13, 17, C_TEXT2, 2);
    }
}

static void draw_slots(const isaac_launcher_view *v) {
    const int x = 660, y = BAR_TOP + BAR_H + 20, w = W - 40 - x, h = FOOT_TOP - y - 12;
    panel(x, y, w, h, "SAVE SLOTS");
    const int c1 = x + 110, c2 = x + 330;
    label(c1, y + 68, "PROGRESS", C_DIM);
    label(c2, y + 68, "RUN IN PROGRESS", C_DIM);
    for (int r = 0; r < 3; ++r) {
        int ry = y + 104 + r * 36;
        char name[16];
        snprintf(name, sizeof name, "Slot %d", r + 1);
        text(x + 20, ry, name, 16, BOLD, C_TEXT);
        for (int k = 0; k < 2; ++k) {
            int s = v->slot[r + 3 * k], cx = k ? c2 : c1;
            uint32_t rgb;
            const char *t = slot_text(s, v->phase, &rgb);
            slot_icon(cx + 8, ry - 6, s, v->phase, v->t);
            text(cx + 24, ry, t, 15, REG, s == ISAAC_LAUNCHER_SLOT_PENDING || s == ISAAC_SYNC_SLOT_EMPTY ? C_DIM : C_TEXT);
        }
    }
    text(x + 20, y + h - 18, "Steam Cloud app 250900 \xc2\xb7 saves in sdmc:/switch/isaac-recomp/saves", 13, REG, C_DIM);
}
#endif /* ISAAC_NO_STEAM */

static void draw_footer(const isaac_launcher_view *v) {
    int cy = FOOT_TOP + 20;
    char act[160];
    fit(act, sizeof act, v->activity, 13, REG, 600);
    text(40, cy + 5, act, 13, REG, C_DIM);
    /* hints, right to left */
    int x = W - 40;
    struct { const char *key, *what; int on; } hints[] = {
        { "+", "Exit", 1 },
#ifndef ISAAC_NO_STEAM
        { "Y", "Retry sync", v->phase == ISAAC_LAUNCHER_FAILED },
        { "X", v->account_action == ISAAC_ACCOUNT_SIGN_IN ? "Sign in" : "Sign out",
          v->account_action != ISAAC_ACCOUNT_NONE },
#endif
        { "A", v->phase == ISAAC_LAUNCHER_FAILED ? "Play anyway" : "Play",
          v->files != ISAAC_FILES_BAD && v->phase != ISAAC_LAUNCHER_SYNCING && v->phase != ISAAC_LAUNCHER_LAUNCHING },
    };
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; ++i) {
        if (!hints[i].on) continue;
        int tw = text_w(hints[i].what, 16, REG, 0);
        x -= tw;
        text(x, cy + 6, hints[i].what, 16, REG, C_TEXT);
        x -= 32;
        pad_button(x, cy, hints[i].key);
        x -= 28;
    }
}

#ifndef ISAAC_NO_STEAM
static void draw_conflict(const isaac_launcher_view *v) {
    rect(0, 0, W, H, 0x000000, 170);
    const int dw = 760, dh = 420, dx = (W - dw) / 2, dy = (H - dh) / 2;
    rrect(dx - 1, dy - 1, dw + 2, dh + 2, 7, 0x000000, 160);
    rrect_grad(dx, dy, dw, dh, 6, 0x2b3240, 0x1d2129, 1, 255);
    cloud_icon(dx + 32, dy + 28, 34, C_WARN, 255);
    text(dx + 78, dy + 56, "Steam Cloud Sync Conflict", 26, BOLD, 0xffffff);
    char body[256];
    snprintf(body, sizeof body,
             "%s changed both on this console and on Steam Cloud since the last sync. "
             "Which copy do you want to keep?", v->conflict_slot);
    paragraph(dx + 34, dy + 104, dw - 68, body, 16, 23, C_TEXT2, 3);

    const int cw = (dw - 34 * 2 - 24) / 2, ch = 150, cyy = dy + 170;
    for (int k = 0; k < 2; ++k) {
        int cx = dx + 34 + k * (cw + 24), sel = v->conflict_choice == k;
        rrect(cx, cyy, cw, ch, 4, sel ? 0x34465c : 0x232832, 255);
        if (sel) rrect_line(cx - 3, cyy - 3, cw + 6, ch + 6, 6, 3, C_BLUE, 255);
        if (k) cloud_icon(cx + 22, cyy + 22, 36, sel ? C_ACCENT : C_TEXT2, 255);
        else {
            /* a console: the screen between two joy-con */
            rrect(cx + 22, cyy + 30, 8, 24, 3, sel ? 0x00c3e3 : C_TEXT2, 255);
            rrect(cx + 31, cyy + 30, 20, 24, 2, sel ? 0xdcdedf : C_TEXT2, 255);
            rrect(cx + 52, cyy + 30, 8, 24, 3, sel ? 0xff4554 : C_TEXT2, 255);
        }
        text(cx + 74, cyy + 50, k ? "Steam Cloud" : "This console", 20, BOLD, sel ? 0xffffff : C_TEXT);
        label(cx + 22, cyy + 94, "LAST MODIFIED", C_DIM);
        text(cx + 22, cyy + 122, k ? v->conflict_cloud : v->conflict_local, 18, REG, C_TEXT);
    }
    paragraph(dx + 34, dy + dh - 46, 400, "The other copy is kept in save_backups on the SD card.", 13, 17, C_DIM, 2);
    int x = dx + dw - 34, cy = dy + dh - 50;
    x -= text_w("Keep this copy", 16, REG, 0);
    text(x, cy + 6, "Keep this copy", 16, REG, C_TEXT);
    pad_button(x - 32, cy, "A");
}

/* The hints at the foot of a dialog, right to left from `x`. */
static int dialog_hint(int x, int cy, const char *key, const char *what) {
    x -= text_w(what, 16, REG, 0);
    text(x, cy + 6, what, 16, REG, C_TEXT);
    x -= 32;
    pad_button(x, cy, key);
    return x - 28;
}

static void draw_signin(const isaac_launcher_view *v) {
    rect(0, 0, W, H, 0x000000, 170);
    const int dw = 900, dh = 480, dx = (W - dw) / 2, dy = (H - dh) / 2;
    rrect(dx - 1, dy - 1, dw + 2, dh + 2, 7, 0x000000, 160);
    rrect_grad(dx, dy, dw, dh, 6, 0x2b3240, 0x1d2129, 1, 255);
    cloud_icon(dx + 32, dy + 28, 34, C_ACCENT, 255);
    text(dx + 78, dy + 56, "Sign in to Steam", 26, BOLD, 0xffffff);

    /* the code on the right, with the quiet zone a scanner wants */
    const int box = 344, bx = dx + dw - 34 - box, by = dy + 76;
    if (v->signin == ISAAC_SIGNIN_SCAN && v->qr_size > 0) {
        rrect(bx, by, box, box, 4, 0xffffff, 255);
        int scale = box / (v->qr_size + 8);
        int side = scale * v->qr_size, qx = bx + (box - side) / 2, qy = by + (box - side) / 2;
        for (int r = 0; r < v->qr_size; ++r)
            for (int c = 0; c < v->qr_size; ++c)
                if (v->qr[r * v->qr_size + c]) rect(qx + c * scale, qy + r * scale, scale, scale, 0x000000, 255);
    } else {
        rrect(bx, by, box, box, 4, 0x232832, 255);
        if (v->signin == ISAAC_SIGNIN_WAIT) spinner(bx + box / 2.0f, by + box / 2.0f, 22, v->t, C_TEXT2);
        else cross(bx + box / 2.0f, by + box / 2.0f, 44, C_BAD);
    }

    /* the steps on the left */
    static const char *const STEPS[] = {
        "Open the Steam app on your phone.",
        "Use its QR sign-in (Steam Guard) to scan this code.",
        "Approve the sign-in for Isaac on Switch.",
    };
    const int lx = dx + 34, lw = bx - lx - 30;
    int y = dy + 108;
    for (int i = 0; i < 3; ++i) {
        char n[2] = { (char)('1' + i), 0 };
        circle(lx + 13, y - 6, 13, C_BLUE, 255);
        text(lx + 13 - text_w(n, 15, BOLD, 0) / 2, y, n, 15, BOLD, 0xffffff);
        y = paragraph(lx + 38, y, lw - 38, STEPS[i], 17, 23, C_TEXT, 3) + 16;
    }
    y += 4;
    if (v->signin == ISAAC_SIGNIN_FAILED) {
        rect(lx, y - 14, 3, 56, C_BAD, 255);
        paragraph(lx + 12, y, lw - 12, v->signin_error, 14, 18, C_TEXT2, 3);
    } else {
        spinner(lx + 12, y - 5, 8, v->t, C_ACCENT);
        text(lx + 30, y, v->signin == ISAAC_SIGNIN_WAIT ? "Asking Steam for a code\xe2\x80\xa6"
                                                        : "Waiting for your phone\xe2\x80\xa6", 16, REG, C_TEXT2);
    }
    paragraph(lx, dy + dh - 92, lw, "The sign-in stays on the SD card. Sign out here at any time, or remove "
              "the device in Steam's security settings.", 13, 17, C_DIM, 3);

    int x = dx + dw - 34, cy = dy + dh - 30;
    x = dialog_hint(x, cy, "B", "Cancel");
    if (v->signin == ISAAC_SIGNIN_FAILED) dialog_hint(x, cy, "Y", "Try again");
}

static void draw_signout(const isaac_launcher_view *v) {
    rect(0, 0, W, H, 0x000000, 170);
    const int dw = 680, dh = 270, dx = (W - dw) / 2, dy = (H - dh) / 2;
    rrect(dx - 1, dy - 1, dw + 2, dh + 2, 7, 0x000000, 160);
    rrect_grad(dx, dy, dw, dh, 6, 0x2b3240, 0x1d2129, 1, 255);
    cloud_icon(dx + 32, dy + 28, 34, C_WARN, 255);
    text(dx + 78, dy + 56, "Sign out of Steam?", 26, BOLD, 0xffffff);
    paragraph(dx + 34, dy + 104, dw - 68, "This console forgets its sign-in and the session ends on Steam too. "
              "Your saves stay on the SD card, but they stop syncing until you sign in again.",
              16, 23, C_TEXT2, 4);
    int x = dx + dw - 34, cy = dy + dh - 34;
    if (v->signout == ISAAC_SIGNOUT_BUSY) {
        int tw = text_w("Signing out\xe2\x80\xa6", 16, REG, 0);
        text(x - tw, cy + 6, "Signing out\xe2\x80\xa6", 16, REG, C_TEXT2);
        spinner(x - tw - 18, cy, 8, v->t, C_ACCENT);
    } else {
        x = dialog_hint(x, cy, "B", "Cancel");
        dialog_hint(x, cy, "A", "Sign out");
    }
}
#endif /* !ISAAC_NO_STEAM */

void isaac_launcher_release(void) {
    free(g_bg);
    g_bg = NULL;
    for (int i = 0; i < GLYPHS; ++i) { free(g_glyphs[i].bmp); g_glyphs[i] = (glyph){ 0 }; }
    if (g_face) { FT_Done_Face(g_face); g_face = NULL; }
    if (g_ft) { FT_Done_FreeType(g_ft); g_ft = NULL; }
}

void isaac_launcher_draw(uint32_t *pixels, int stride, const isaac_launcher_view *v) {
    if (!g_bg && (g_bg = malloc((size_t)W * H * 4))) {
        g_px = g_bg;
        g_stride = W;
        draw_background();
    }
    for (int y = 0; y < H; ++y) {
        if (g_bg) memcpy(pixels + (size_t)y * stride, g_bg + (size_t)y * W, (size_t)W * 4);
        else for (int x = 0; x < W; ++x) pixels[(size_t)y * stride + x] = pack(C_PAGE_TOP);
    }
    g_px = pixels;
    g_stride = stride;
    draw_nav(v);
    draw_files_notice(v);
    draw_play_bar(v);
#ifdef ISAAC_NO_STEAM
    draw_files_panel(v);
    draw_footer(v);
#else
    draw_steps(v);
    draw_slots(v);
    draw_footer(v);
    if (v->conflict) draw_conflict(v);
    else if (v->signin != ISAAC_SIGNIN_NONE) draw_signin(v);
    else if (v->signout != ISAAC_SIGNOUT_NONE) draw_signout(v);
#endif
}
#endif
