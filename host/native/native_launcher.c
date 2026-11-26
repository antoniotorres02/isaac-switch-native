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
static int g_cw = W, g_ch = H;     /* the buffer being drawn: the page, or the panel */

/* ---- pixels ---------------------------------------------------------------- */

static inline uint32_t pack(uint32_t rgb) {
    return ((rgb >> 16) & 0xff) | (rgb & 0xff00) | ((rgb & 0xff) << 16) | 0xff000000u;
}

static inline void blend(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || (unsigned)x >= (unsigned)g_cw || (unsigned)y >= (unsigned)g_ch) return;
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

/* Draw Switch buttons with the launcher's stock shapes. */
static int pad_button_width(const char *key) {
    int width = text_w(key, 15, BOLD, 0) + 12;
    return width < 24 ? 24 : width;
}

static int pad_button(int x, int cy, const char *key) {
    int width = pad_button_width(key);
    rrect(x, cy - 12, width, 24, 12, C_TEXT, 255);
    text(x + (width - text_w(key, 15, BOLD, 0)) / 2, cy + 6, key, 15, BOLD, C_NAV);
    return width;
}

/* ---- the background, drawn once ----------------------------------------------- */

static uint32_t *g_bg;
static int g_bg_workshop = -1;

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
    text_ex(50, 293, "Native port for Nintendo Switch \xc2\xb7 v1.9.7.19", 15, REG, 0, 0xb9aea4, 255);
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
const char *isaac_switch_game_files_label(void);   /* native_switch.c */

/* A build without Steam (build_native.py --no-steam): only the files to report. */
static void draw_files_panel(const isaac_launcher_view *v) {
    const int x = 40, y = BAR_TOP + BAR_H + 20, w = W - 80, h = FOOT_TOP - y - 12;
    panel(x, y, w, h, "ON THIS CONSOLE");
    text(x + 20, y + 76, "Game files", 16, BOLD, C_TEXT);
    text(x + 150, y + 76, isaac_switch_game_files_label(), 15, REG, C_TEXT2);
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

/* Draw a controller hint with stock launcher graphics. */
static int controller_hint(int x, int cy, const char *key, const char *second, const char *what) {
    x -= text_w(what, 16, REG, 0);
    text(x, cy + 6, what, 16, REG, C_TEXT);
    x -= 8;
    if (second) {
        x -= pad_button_width(second);
        pad_button(x, cy, second);
        x -= 5;
    }
    x -= pad_button_width(key);
    pad_button(x, cy, key);
    return x - 28;
}

static int dialog_hint(int x, int cy, const char *key, const char *what) {
    return controller_hint(x, cy, key, NULL, what);
}

static void draw_footer(const isaac_launcher_view *v) {
    int cy = FOOT_TOP + 20;
    /* hints, right to left */
    int x = W - 40;
    struct { const char *key, *what; int on; } hints[] = {
        { "+", "Exit", 1 },
#ifndef ISAAC_NO_STEAM
        { "R", "Workshop", v->workshop_available },
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
        x -= pad_button_width(hints[i].key) + 8;
        pad_button(x, cy, hints[i].key);
        x -= 28;
    }
    if (x > 40) {
        char act[160];
        fit(act, sizeof act, v->activity, 13, REG, x - 40);
        text(40, cy + 5, act, 13, REG, C_DIM);
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

#ifndef ISAAC_NO_STEAM
static inline void rgba_pixel(int x, int y, uint32_t rgba) {
    blend(x, y, ((rgba & 255) << 16) | (rgba & 0xff00) | ((rgba >> 16) & 255), rgba >> 24);
}

static void workshop_text(int x, int y, const char *message, int px, int bold,
                          uint32_t color, int width) {
    char fit_text[420];
    fit(fit_text, sizeof fit_text, message, px, bold, width);
    text(x, y, fit_text, px, bold, color);
}

static void workshop_image(const workshop_thumb *thumb, int x, int y) {
    rrect(x, y, WORKSHOP_THUMB_W + 12, WORKSHOP_THUMB_H + 12, 4, C_PANEL, 90);
    if (!thumb || !thumb->width || thumb->width > WORKSHOP_THUMB_W ||
        !thumb->height || thumb->height > WORKSHOP_THUMB_H) return;
    x += (WORKSHOP_THUMB_W + 12 - (int)thumb->width) / 2;
    y += (WORKSHOP_THUMB_H + 12 - (int)thumb->height) / 2;
    for (unsigned row = 0; row < thumb->height; ++row)
        for (unsigned col = 0; col < thumb->width; ++col)
            rgba_pixel(x + col, y + row, thumb->pixels[row * thumb->width + col]);
}

static void workshop_tile(const workshop_thumb *thumb, int x, int y, int w, int h, int frame) {
    if (frame) rrect(x, y, w, h, 4, C_PAGE_BOT, 210);
    if (!thumb || !thumb->width || thumb->width > WORKSHOP_THUMB_W ||
        !thumb->height || thumb->height > WORKSHOP_THUMB_H) return;
    int sw = w, sh = w * thumb->height / thumb->width;
    if (sh > h) {
        sh = h;
        sw = h * thumb->width / thumb->height;
    }
    if (sw < 1 || sh < 1) return;
    x += (w - sw) / 2;
    y += (h - sh) / 2;
    for (int row = 0; row < sh; ++row) {
        const uint32_t *src = thumb->pixels + (size_t)(row * thumb->height / sh) * thumb->width;
        for (int col = 0; col < sw; ++col)
            rgba_pixel(x + col, y + row, src[col * thumb->width / sw]);
    }
}

typedef struct {
    const char *text;
    unsigned style;
} description_cursor;

enum description_style {
    DESC_BOLD = 1, DESC_ITALIC = 2, DESC_UNDERLINE = 4, DESC_STRIKE = 8,
    DESC_H1 = 16, DESC_H2 = 32, DESC_H3 = 64, DESC_QUOTE = 128,
    DESC_CODE = 256, DESC_LINK = 512,
};

static void description_style(description_cursor *cursor) {
    static const char codes[] = WORKSHOP_STYLE_CODES;
    while (*cursor->text == WORKSHOP_STYLE_MARKER && cursor->text[1]) {
        char code = cursor->text[1];
        int closing = (code >= '4' && code <= '6') || (code >= 'a' && code <= 'z');
        if (closing) code -= code <= '6' ? 3 : 'a' - 'A';
        const char *key = strchr(codes, code);
        if (key && *key) {
            unsigned flag = 1u << (key - codes);
            if (closing) cursor->style &= ~flag;
            else cursor->style |= flag;
        }
        cursor->text += 2;
    }
}

static int description_px(unsigned style) {
    if (style & DESC_H1) return 29;
    if (style & DESC_H2) return 24;
    if (style & DESC_H3) return 21;
    return 18;
}

static int description_weight(unsigned style) {
    int bold = (style & (DESC_BOLD | DESC_H1 | DESC_H2 | DESC_H3)) != 0;
    return bold | (style & DESC_ITALIC);
}

static void description_glyph(const glyph *g, int x, int y, unsigned style) {
    uint32_t color = style & (DESC_H1 | DESC_H2 | DESC_H3) ? C_TEXT :
                     style & (DESC_LINK | DESC_CODE) ? C_ACCENT : C_TEXT2;
    if (g->bmp)
        for (int row = 0; row < g->h; ++row)
            for (int col = 0; col < g->w; ++col)
                blend(x + g->left + col, y - g->top + row, color, g->bmp[row * g->w + col]);
    if (style & (DESC_UNDERLINE | DESC_LINK)) rect(x, y + 2, g->adv, 1, color, 230);
    if (style & DESC_STRIKE) rect(x, y - 5, g->adv, 1, color, 230);
}

static description_cursor description_line_end(description_cursor start, int *height) {
    description_cursor p = start, space = {0};
    int width = 0, available = start.style & DESC_QUOTE ? 860 : 880;
    *height = description_px(start.style) + 7;
    while (*p.text && *p.text != '\n') {
        description_style(&p);
        if (!*p.text || *p.text == '\n') break;
        description_cursor before = p;
        uint32_t cp = utf8_next(&p.text);
        int px = description_px(p.style);
        width += get_glyph(cp, px, description_weight(p.style))->adv;
        if (width > available && before.text != start.text) return space.text ? space : before;
        if (cp == ' ' || cp == '\t') space = before;
        if (px + 7 > *height) *height = px + 7;
    }
    return p;
}

static void draw_description_line(description_cursor p, const char *end, int y, int height) {
    int x = 42, baseline = y + height - 6;
    if (p.style & DESC_QUOTE) {
        rect(x, y, 2, height, C_ACCENT, 160);
        x += 16;
    }
    while (p.text < end) {
        description_style(&p);
        if (p.text >= end) break;
        uint32_t cp = utf8_next(&p.text);
        const glyph *g = get_glyph(cp, description_px(p.style), description_weight(p.style));
        description_glyph(g, x, baseline, p.style);
        x += g->adv;
    }
}

static const workshop_thumb *workshop_animation_frame(const workshop_animation *media, float t) {
    if (!media || !media->count) return NULL;
    if (media->count == 1) return media->frames[0];
    unsigned duration = 0;
    for (unsigned i = 0; i < media->count; ++i) duration += media->delay[i];
    if (!duration) return media->frames[0];
    unsigned at = (uint64_t)(t * 1000) % duration;
    for (unsigned i = 0; i + 1 < media->count; ++i) {
        if (at < media->delay[i]) return media->frames[i];
        at -= media->delay[i];
    }
    return media->frames[media->count - 1];
}

static description_cursor description_page(description_cursor p, const workshop_view *view,
                                           float t, unsigned *mask) {
    const int top = 199, bottom = 551, media_height = 148;
    for (int y = top; *p.text && y < bottom;) {
        description_style(&p);
        if (!*p.text) break;
        int index = p.text[0] == WORKSHOP_MEDIA_MARKER ? p.text[1] - 'A' : -1;
        if (index >= 0 && index < WORKSHOP_MEDIA_IMAGES && p.text[2] == '\n') {
            if (y + media_height > bottom) break;
            if (mask) *mask |= 1u << index;
            if (view) {
                const workshop_thumb *image = workshop_animation_frame(view->media[index], t);
                if (image) {
                    int width = media_height * image->width / image->height;
                    if (width < 1) width = 1;
                    workshop_tile(image, 42, y, width < 880 ? width : 880, media_height, 0);
                } else {
                    const char *label = view->media_failed & (1u << index) ? "Image unavailable" : "Loading image…";
                    text(42, y + 27, label, 16, REG, C_TEXT2);
                }
            }
            y += media_height + 7;
            p.text += 3;
            continue;
        }
        int height;
        description_cursor end = description_line_end(p, &height);
        if (y + height > bottom) break;
        if (view) draw_description_line(p, end.text, y, height);
        p = end;
        if (*p.text == '\n') ++p.text;
        while (*p.text == ' ' || *p.text == '\t') ++p.text;
        y += height;
    }
    return p;
}

static description_cursor workshop_description(const char *s, int page, const workshop_view *view,
                                                float t, unsigned *mask) {
    description_cursor cursor = {s, 0};
    for (int current = 0; *cursor.text && current <= page; ++current)
        cursor = description_page(cursor, current == page ? view : NULL, t, current == page ? mask : NULL);
    return cursor;
}

int isaac_launcher_workshop_description_pages(const char *description) {
    int pages = 0;
    description_cursor cursor = {description, 0};
    while (*cursor.text) {
        cursor = description_page(cursor, NULL, 0, NULL);
        ++pages;
    }
    return pages;
}

unsigned isaac_launcher_workshop_description_media(const char *description, int page) {
    unsigned mask = 0;
    workshop_description(description, page, NULL, 0, &mask);
    return mask;
}

static void draw_workshop_background(void) {
    rect(0, 0, W, 50, C_NAV, 255);
    for (int y = 50; y < FOOT_TOP; ++y)
        rect(0, y, W, 1, mix(C_PAGE_TOP, C_PAGE_BOT, (y - 50) / (float)(FOOT_TOP - 50)), 255);
    rect(0, FOOT_TOP, W, H - FOOT_TOP, C_NAV, 255);
    rect(0, FOOT_TOP, W, 1, 0x2a3442, 255);
}

static void draw_workshop_hints(const workshop_view *v) {
    static const struct { const char *key, *second; } buttons[WORKSHOP_HINT_COUNT] = {
        [WORKSHOP_HINT_A] = {"A"},
        [WORKSHOP_HINT_X] = {"X"},
        [WORKSHOP_HINT_Y] = {"Y"},
        [WORKSHOP_HINT_MINUS] = {"−"},
        [WORKSHOP_HINT_PLUS] = {"+"},
        [WORKSHOP_HINT_SCROLL] = {"↑", "↓"},
        [WORKSHOP_HINT_PAGES] = {"ZL", "ZR"},
        [WORKSHOP_HINT_TABS] = {"L", "R"},
        [WORKSHOP_HINT_B] = {"B"},
    };
    int x = W - 40;
    for (int i = WORKSHOP_HINT_COUNT - 1; i >= 0; --i) {
        if (!v->hints[i]) continue;
        if (i == WORKSHOP_HINT_SCROLL && v->grid)
            x = dialog_hint(x, FOOT_TOP + 20, "D-pad", v->hints[i]);
        else x = controller_hint(x, FOOT_TOP + 20, buttons[i].key, buttons[i].second, v->hints[i]);
    }
}

static void draw_workshop_grid(const workshop_view *v, float t) {
    const int w = 586, h = 93, y0 = 195, step = 102;
    for (int i = 0; i < v->row_count; ++i) {
        int x = 40 + (i % WORKSHOP_GRID_COLUMNS) * (w + 14);
        int y = y0 + (i / WORKSHOP_GRID_COLUMNS) * step;
        int active = i == v->selected;
        rrect(x, y, w, h, 6, active ? 0x285985 : C_PANEL, active ? 255 : 180);
        if (active) rrect_line(x, y, w, h, 6, 1, C_ACCENT, 220);
        const workshop_thumb *image = workshop_animation_frame(v->rows[i].animation, t);
        workshop_tile(image ? image : v->rows[i].thumb, x + 7, y + 7, 145, h - 14, 1);
        workshop_text(x + 166, y + 34, v->rows[i].title, 19, BOLD, C_TEXT, w - 182);
        paragraph(x + 166, y + 58, w - 182, v->rows[i].detail, 15, 18, C_TEXT2, 2);
    }
    size_t count = (v->list_count + WORKSHOP_GRID_COLUMNS - 1) / WORKSHOP_GRID_COLUMNS;
    size_t visible = WORKSHOP_GRID_CARDS / WORKSHOP_GRID_COLUMNS;
    if (count > visible) {
        int track = h + step * ((int)visible - 1);
        int bar = track * visible / count;
        int y = y0 + (track - bar) * (v->list_start / WORKSHOP_GRID_COLUMNS) / (count - visible);
        rrect(1234, y0, 4, track, 2, C_PANEL, 210);
        rrect(1234, y, 4, bar, 2, C_ACCENT, 220);
    }
}

static void draw_workshop(const workshop_view *v, float t) {
    static const char *tabs[] = { "Browse", "Installed", "Sync" };
    int nav = 40;
    text(40, 31, "STEAM WORKSHOP", 17, BOLD, C_TEXT);
    for (int i = 0; i < WORKSHOP_TAB_COUNT; ++i) {
        int active = v->tab == i;
        int w = text_w(tabs[i], 20, BOLD, 0) + 44;
        rrect(nav, 64, w, 38, 3, active ? 0x285985 : C_PANEL, active ? 255 : 80);
        text(nav + 22, 90, tabs[i], 20, BOLD, active ? C_TEXT : C_TEXT2);
        nav += w + 8;
    }
    workshop_text(40, 147, v->heading, 32, BOLD, C_TEXT, 1184);
    workshop_text(40, 180, v->context, 17, REG, C_ACCENT, 1184);
    if (!v->detail && *v->search) workshop_text(790, 180, v->search, 16, REG, C_TEXT2, 440);

    if (v->detail) {
        const workshop_thumb *image = workshop_animation_frame(v->thumb_animation, t);
        workshop_image(image ? image : v->thumb, 960, 212);
        if (*v->search) paragraph(960, 400, 268, v->search, 16, 22, C_TEXT2, 6);
        if (*v->description)
            workshop_description(v->description, v->description_page, v, t, NULL);
        else text(42, 220, "Description unavailable", 19, REG, C_TEXT2);
        if (v->description_pages > 1) {
            int track = 350, height = track / v->description_pages;
            if (height < 20) height = 20;
            int offset = (track - height) * v->description_page / (v->description_pages - 1);
            rrect(930, 200, 5, track, 2, C_PANEL, 210);
            rrect(930, 200 + offset, 5, height, 2, C_ACCENT, 220);
        }
        const char *line_end = strchr(v->dependencies, '\n');
        char line[384];
        snprintf(line, sizeof line, "%.*s", line_end ? (int)(line_end - v->dependencies) :
                 (int)strlen(v->dependencies), v->dependencies);
        workshop_text(40, 575, line, 16, BOLD, C_ACCENT, 1200);
        if (line_end) workshop_text(40, 599, line_end + 1, 15, REG, C_TEXT2, 1200);
    } else if (v->grid && v->row_count) {
        draw_workshop_grid(v, t);
    } else if (v->row_count) {
        const int list_y = 196, row_step = 40, row_height = 37;
        int w = v->tab == WORKSHOP_BROWSE ? 884 : 1184;
        for (int i = 0; i < v->row_count; ++i) {
            int y = list_y + row_step * i;
            int active = i == v->selected;
            rrect(40, y, w, row_height, 3, active ? 0x285985 : C_PANEL, active ? 255 : 70);
            workshop_text(54, y + 16, v->rows[i].title, 17, BOLD, C_TEXT, w - 28);
            workshop_text(54, y + 32, v->rows[i].detail, 14, REG, C_TEXT2, w - 28);
        }
        if (v->list_count > (size_t)v->row_count) {
            int track = (v->row_count - 1) * row_step + row_height;
            int height = track * (size_t)v->row_count / v->list_count;
            if (height < 18) height = 18;
            int offset = (track - height) * v->list_start / (v->list_count - v->row_count);
            rrect(48 + w, list_y, 4, track, 2, C_PANEL, 210);
            rrect(48 + w, list_y + offset, 4, height, 2, C_ACCENT, 220);
        }
        if (v->thumb) workshop_image(v->thumb, 960, 212);
    } else if (*v->description) {
        paragraph(40, 232, 930, v->description, 21, 29, C_TEXT2, 4);
    }
    if (v->total && v->busy) {
        char bytes[96];
        const double mib = 1024.0 * 1024.0;
        float ratio = clamp01((float)((double)v->done / (double)v->total));
        snprintf(bytes, sizeof bytes, "%.0f%%   %.2f / %.2f MiB verified",
                 floor(ratio * 100.0), v->done / mib, v->total / mib);
        rrect(34, 604, 1172, 38, 4, C_NAV, 245);
        text(42, 620, bytes, 16, REG, C_ACCENT);
        rrect(42, 627, 800, 7, 3, C_PANEL, 255);
        if (ratio > 0) rrect(42, 627, ratio * 800, 7, 3, C_ACCENT, 255);
    }
    if (v->notice[0] && !v->confirming)
        workshop_text(40, 656, v->notice, 17, BOLD, C_TEXT, 1160);
    if (v->busy) spinner(1222, 644, 12, t, C_ACCENT);
    if (v->confirming) {
        rect(0, 0, W, H, 0, 174);
        const int dw = 680, dh = 220, dx = (W - dw) / 2, dy = (H - dh) / 2;
        rrect_grad(dx, dy, dw, dh, 8, C_PAGE_TOP, C_PAGE_BOT, 1, 255);
        rrect_line(dx, dy, dw, dh, 8, 1, C_ACCENT, 180);
        text(dx + 30, dy + 50, "Confirm Workshop changes", 24, BOLD, C_TEXT);
        paragraph(dx + 30, dy + 94, dw - 60, v->notice, 20, 28, C_TEXT2, 3);
        int cy = dy + dh - 32;
        int x = dialog_hint(dx + dw - 30, cy, "B", "Back");
        dialog_hint(x, cy, "A", "Continue");
    } else draw_workshop_hints(v);
}
#endif

void isaac_launcher_release(void) {
    free(g_bg);
    g_bg = NULL;
    for (int i = 0; i < GLYPHS; ++i) { free(g_glyphs[i].bmp); g_glyphs[i] = (glyph){ 0 }; }
    if (g_face) { FT_Done_Face(g_face); g_face = NULL; }
    if (g_ft) { FT_Done_FreeType(g_ft); g_ft = NULL; }
}

void isaac_launcher_draw(uint32_t *pixels, int stride, const isaac_launcher_view *v) {
    int workshop = 0;
#ifndef ISAAC_NO_STEAM
    workshop = v->workshop != NULL;
#endif
    if (!g_bg) {
        g_bg = malloc((size_t)W * H * 4);
        g_bg_workshop = -1;
    }
    if (g_bg && g_bg_workshop != workshop) {
        g_px = g_bg;
        g_stride = W;
        g_cw = W;
        g_ch = H;
#ifndef ISAAC_NO_STEAM
        if (workshop) draw_workshop_background();
        else
#endif
            draw_background();
        g_bg_workshop = workshop;
    }
    for (int y = 0; y < H; ++y) {
        if (g_bg) memcpy(pixels + (size_t)y * stride, g_bg + (size_t)y * W, (size_t)W * 4);
        else for (int x = 0; x < W; ++x) pixels[(size_t)y * stride + x] = pack(C_PAGE_TOP);
    }
    g_px = pixels;
    g_stride = stride;
    g_cw = W;
    g_ch = H;
#ifndef ISAAC_NO_STEAM
    if (v->workshop) {
        draw_workshop(v->workshop, v->t);
        return;
    }
#endif
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

/* ---- the in-game Steam Cloud panel -------------------------------------------- */
/* Drawn into its own small buffer, which the GL side lays over the game. Pixels
 * the panel does not cover stay transparent (alpha 0). */
#ifndef ISAAC_NO_STEAM
void isaac_panel_draw(uint32_t *pixels, const isaac_panel_view *v) {
    const int pw = ISAAC_PANEL_W, ph = ISAAC_PANEL_H;
    memset(pixels, 0, (size_t)pw * ph * 4);
    g_px = pixels;
    g_stride = pw;
    g_cw = pw;
    g_ch = ph;
    rrect_grad(0, 0, pw, ph, 8, 0x2b3240, 0x1d2129, 1, 255);
    rrect_line(0, 0, pw, ph, 8, 1, 0x3a4b5e, 255);
    cloud_icon(26, 22, 30, C_ACCENT, 255);
    text(68, 48, "Steam Cloud", 24, BOLD, 0xffffff);
    if (v->account[0]) {
        int aw = text_w(v->account, 15, REG, 0);
        text(pw - 26 - aw, 46, v->account, 15, REG, C_TEXT2);
    }
    rect(26, 70, pw - 52, 1, 0x3a4b5e, 255);

    /* the state: an icon and a headline, the reason under it */
    float cx = 42, cy = 112;
    switch (v->state) {
    case ISAAC_PANEL_BUSY: spinner(cx, cy, 12, v->t, C_ACCENT); break;
    case ISAAC_PANEL_DONE: circle(cx, cy, 13, C_OK, 255); check(cx, cy, 13, 0xffffff); break;
    case ISAAC_PANEL_FAILED: circle(cx, cy, 13, C_BAD, 255); cross(cx, cy, 13, 0xffffff); break;
    case ISAAC_PANEL_UNAVAILABLE: circle(cx, cy, 13, C_WARN, 255); cross(cx, cy, 13, 0x1d2129); break;
    default: cloud_icon(cx - 14, cy - 13, 28, C_TEXT2, 255); break;
    }
    char head[160];
    fit(head, sizeof head, v->message, 19, BOLD, pw - 70 - 26);
    text(70, cy + 7, head, 19, BOLD, v->state == ISAAC_PANEL_FAILED ? 0xffb3ab : C_TEXT);
    paragraph(70, cy + 34, pw - 70 - 26, v->detail, 15, 21, C_TEXT2, 3);

    /* upload at exit */
    int ry = 214;
    rrect(26, ry, pw - 52, 40, 4, 0x232832, 255);
    text(42, ry + 26, "Upload when the game closes", 16, REG, C_TEXT);
    const char *onoff = v->auto_upload ? "ON" : "OFF";
    int ow = text_w(onoff, 14, BOLD, 1) + 22;
    rrect(pw - 42 - ow, ry + 9, ow, 22, 11, v->auto_upload ? C_GREEN_R : 0x4a5361, 255);
    text_ex(pw - 42 - ow + 11, ry + 25, onoff, 14, BOLD, 1, 0xffffff, 255);

    /* the buttons, right to left */
    int x = pw - 26, fy = ph - 22;
    x = dialog_hint(x, fy, "B", "Close");
    x = dialog_hint(x, fy, "X", v->auto_upload ? "Don't upload at exit" : "Upload at exit");
    if (v->state != ISAAC_PANEL_BUSY && v->state != ISAAC_PANEL_UNAVAILABLE)
        dialog_hint(x, fy, "A", "Upload now");
    g_cw = W;
    g_ch = H;
}
#endif
#endif
