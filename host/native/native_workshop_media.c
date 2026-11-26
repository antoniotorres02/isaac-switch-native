#include "native_workshop_media.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int image_suffix(const char *url, size_t length) {
    size_t n = strcspn(url, "?#");
    if (n > length) n = length;
    return (n >= 4 && (!strncasecmp(url + n - 4, ".png", 4) ||
                       !strncasecmp(url + n - 4, ".jpg", 4) ||
                       !strncasecmp(url + n - 4, ".gif", 4))) ||
           (n >= 5 && !strncasecmp(url + n - 5, ".jpeg", 5));
}

static size_t formatting_tag(const char *p, char *code) {
    static const struct { const char *tag; char style; } tags[] = {
        {"b", 'B'}, {"i", 'I'}, {"u", 'U'}, {"strike", 'S'},
        {"h1", '1'}, {"h2", '2'}, {"h3", '3'}, {"quote", 'Q'}, {"code", 'C'},
        {"url", 'L'}, {"*", '*'}, {"list", 0}, {"olist", 0}, {"spoiler", 0}, {"noparse", 0},
    };
    if (*p != '[') return 0;
    const char *end = strchr(p, ']');
    if (!end) return 0;
    int closing = p[1] == '/';
    const char *name = p + (closing ? 2 : 1);
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; ++i) {
        size_t n = strlen(tags[i].tag);
        if ((size_t)(end - name) < n || strncasecmp(name, tags[i].tag, n) ||
            (name[n] != ']' && name[n] != '=')) continue;
        *code = tags[i].style;
        if (*code && closing) *code = *code >= 'A' ? *code + ('a' - 'A') : *code + 3;
        return (size_t)(end + 1 - p);
    }
    return 0;
}

static int media_scheme(const char *url) {
    return !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
}

unsigned workshop_media_description(const char *source, char *text, size_t capacity,
                                    char urls[WORKSHOP_MEDIA_IMAGES][WORKSHOP_MEDIA_URL]) {
    if (!capacity) return 0;
    size_t used = 0;
    unsigned count = 0;
    while (*source && used + 1 < capacity) {
        const char *url = source, *end = source, *after = source;
        if (!strncasecmp(source, "[img]", 5)) {
            url += 5;
            end = url;
            while (*end && strncasecmp(end, "[/img]", 6)) ++end;
            if (!*end) {
                source = url;
                continue;
            }
            after = end + 6;
        } else if (media_scheme(source)) {
            end = source + strcspn(source, " \t\r\n[]<>\"");
            if (image_suffix(source, (size_t)(end - source))) after = end;
        }
        if (after != source) {
            while (url < end && strchr(" \r\n\t", *url)) ++url;
            while (end > url && strchr(" \r\n\t", end[-1])) --end;
            size_t n = (size_t)(end - url);
            if (count < WORKSHOP_MEDIA_IMAGES && used + 4 < capacity && n < WORKSHOP_MEDIA_URL &&
                n >= 8 && media_scheme(url)) {
                memcpy(urls[count], url, n);
                urls[count][n] = 0;
                if (used && text[used - 1] != '\n') text[used++] = '\n';
                text[used++] = WORKSHOP_MEDIA_MARKER;
                text[used++] = (char)('A' + count++);
                text[used++] = '\n';
                if (*after == '\n') ++after;
            }
            source = after;
        } else {
            char code = 0;
            size_t tag = formatting_tag(source, &code);
            if (tag) {
                if (code && used + 5 < capacity) {
                    int block = (code >= '1' && code <= '6') || code == 'Q' || code == 'q' || code == '*';
                    if (block && used && text[used - 1] != '\n') text[used++] = '\n';
                    if (code == '*') {
                        memcpy(text + used, "• ", 4);
                        used += 4;
                    } else {
                        text[used++] = WORKSHOP_STYLE_MARKER;
                        text[used++] = code;
                    }
                }
                source += tag;
            } else {
                unsigned char c = *source++;
                if (c == '\r' && *source == '\n') continue;
                text[used++] = c < 32 && c != '\n' && c != '\t' ? ' ' : (char)c;
            }
        }
    }
    while (used && (unsigned char)text[used - 1] >= 0x80 &&
           ((unsigned char)text[used - 1] & 0xc0) == 0x80) {
        if (!*source || ((unsigned char)*source & 0xc0) != 0x80) break;
        --used;
    }
    if (*source && used && ((unsigned char)*source & 0xc0) == 0x80 &&
        (unsigned char)text[used - 1] >= 0xc0) --used;
    text[used] = 0;
    return count;
}

#define GIF_DIM 1024u
#define GIF_CODES 4096u

typedef struct {
    const uint8_t *p, *end;
} gif_reader;

static unsigned gif_u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

static const uint8_t *gif_take(gif_reader *r, size_t length) {
    if ((size_t)(r->end - r->p) < length) return NULL;
    const uint8_t *data = r->p;
    r->p += length;
    return data;
}

static int gif_blocks(gif_reader *r, uint8_t *packed, size_t *length) {
    *length = 0;
    const uint8_t *n;
    while ((n = gif_take(r, 1))) {
        if (!*n) return 0;
        const uint8_t *block = gif_take(r, *n);
        if (!block) return -1;
        if (packed) memcpy(packed + *length, block, *n);
        *length += *n;
    }
    return -1;
}

static int gif_lzw(const uint8_t *bytes, size_t length, unsigned min, uint8_t *pixels, size_t npixels) {
    if (min < 2 || min > 8) return -1;
    uint16_t prefix[GIF_CODES];
    uint8_t suffix[GIF_CODES], stack[GIF_CODES];
    unsigned clear = 1u << min, end = clear + 1, next = end + 1, width = min + 1;
    unsigned previous = GIF_CODES, first = 0, bits = 0, acc = 0;
    size_t offset = 0, pos = 0;
    while (offset < length || bits >= width) {
        while (bits < width && offset < length) {
            acc |= (unsigned)bytes[offset++] << bits;
            bits += 8;
        }
        if (bits < width) break;
        unsigned code = acc & ((1u << width) - 1);
        acc >>= width;
        bits -= width;
        if (code == end) return pos == npixels ? 0 : -1;
        if (code == clear) {
            next = end + 1;
            width = min + 1;
            previous = GIF_CODES;
            continue;
        }
        unsigned current = code, count = 0;
        if (code == next && previous < GIF_CODES) {
            stack[count++] = (uint8_t)first;
            current = previous;
        } else if (code >= next) return -1;
        while (current >= clear) {
            if (current <= end || current >= next || count >= GIF_CODES - 1) return -1;
            stack[count++] = suffix[current];
            current = prefix[current];
        }
        first = current;
        stack[count++] = (uint8_t)first;
        if (count > npixels - pos) return -1;
        while (count) pixels[pos++] = stack[--count];
        if (previous < GIF_CODES && next < GIF_CODES) {
            prefix[next] = (uint16_t)previous;
            suffix[next++] = (uint8_t)first;
            if (next == (1u << width) && width < 12) ++width;
        }
        previous = code;
    }
    return -1;
}

static int gif_paint(uint32_t *canvas, unsigned width, const uint8_t *indices, const uint8_t *map,
                     unsigned colors, unsigned x, unsigned y, unsigned w, unsigned h,
                     int interlace, int transparent) {
    const unsigned first[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
    for (unsigned pass = 0; pass < (interlace ? 4u : 1u); ++pass)
        for (unsigned row = first[pass]; row < h; row += interlace ? step[pass] : 1) {
            uint32_t *dst = canvas + (y + row) * width + x;
            for (unsigned col = 0; col < w; ++col) {
                unsigned index = *indices++;
                if ((int)index == transparent) continue;
                if (index >= colors) return -1;
                const uint8_t *rgb = map + 3 * index;
                dst[col] = 0xff000000u | rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16;
            }
        }
    return 0;
}

typedef struct {
    const uint8_t *palette;
    unsigned x, y, w, h, colors, delay, disposal, interlaced, code_size;
    int transparent;
    size_t packed_length;
} gif_frame;

/* The same parser measures the complete timeline and then decodes it. */
static int gif_next(gif_reader *r, unsigned width, unsigned height, const uint8_t *global,
                    unsigned global_colors, gif_frame *frame, uint8_t *packed) {
    *frame = (gif_frame){.delay = 100, .transparent = -1};
    const uint8_t *tag;
    while ((tag = gif_take(r, 1))) {
        if (*tag == 0x3b) return 0;
        if (*tag == 0x21) {
            tag = gif_take(r, 1);
            if (!tag) return -1;
            if (*tag == 0xf9) {
                const uint8_t *control = gif_take(r, 6);
                if (!control || control[0] != 4 || control[5] != 0) return -1;
                frame->disposal = (control[1] >> 2) & 7;
                frame->transparent = (control[1] & 1) ? control[4] : -1;
                unsigned delay = gif_u16(control + 2) * 10;
                frame->delay = delay < 40 ? 40 : delay;
            } else {
                size_t length;
                if (gif_blocks(r, NULL, &length)) return -1;
            }
            continue;
        }
        if (*tag != 0x2c) return -1;
        const uint8_t *d = gif_take(r, 9);
        if (!d) return -1;
        frame->x = gif_u16(d);
        frame->y = gif_u16(d + 2);
        frame->w = gif_u16(d + 4);
        frame->h = gif_u16(d + 6);
        frame->interlaced = (d[8] & 0x40) != 0;
        if (!frame->w || !frame->h || frame->x + frame->w > width || frame->y + frame->h > height)
            return -1;
        frame->colors = d[8] & 0x80 ? 2u << (d[8] & 7) : global_colors;
        frame->palette = d[8] & 0x80 ? gif_take(r, frame->colors * 3) : global;
        const uint8_t *min = gif_take(r, 1);
        if (!frame->palette || !min || *min < 2 || *min > 8 ||
            gif_blocks(r, packed, &frame->packed_length)) return -1;
        frame->code_size = *min;
        return 1;
    }
    return -1;
}

int workshop_gif(const uint8_t *bytes, size_t len, workshop_animation *animation, unsigned limit) {
    gif_reader r = {bytes, bytes + len};
    const uint8_t *head = gif_take(&r, 13);
    if (!head || (memcmp(head, "GIF89a", 6) && memcmp(head, "GIF87a", 6)) ||
        !limit || limit > WORKSHOP_MEDIA_FRAMES) return -1;
    unsigned width = gif_u16(head + 6), height = gif_u16(head + 8);
    if (!width || !height || width > GIF_DIM || height > GIF_DIM) return -1;
    unsigned colors = 2u << (head[10] & 7);
    const uint8_t *global = head[10] & 0x80 ? gif_take(&r, colors * 3) : NULL;
    if ((head[10] & 0x80) && !global) return -1;

    gif_reader scan = r;
    gif_frame f;
    unsigned frames = 0, duration = 0;
    uint64_t decoded_pixels = 0;
    int next;
    while ((next = gif_next(&scan, width, height, global, colors, &f, NULL)) > 0) {
        decoded_pixels += (uint64_t)f.w * f.h + (f.disposal == 3 ? 2ull * width * height : 0);
        if (++frames > 2048 || decoded_pixels > (128u << 20)) return -1;
        duration += f.delay;
    }
    if (next || !frames) return -1;

    size_t area = (size_t)width * height;
    uint32_t *canvas = calloc(area, sizeof *canvas), *previous = NULL;
    uint8_t *indices = malloc(area), *packed = malloc(len);
    int rc = -1;
    unsigned time = 0, sample = 0;
    if (!canvas || !indices || !packed) goto out;
    while ((next = gif_next(&r, width, height, global, colors, &f, packed)) > 0) {
        if (gif_lzw(packed, f.packed_length, f.code_size, indices, (size_t)f.w * f.h)) goto out;
        if (f.disposal == 3) {
            if (!previous) previous = malloc(area * sizeof *canvas);
            if (!previous) goto out;
            memcpy(previous, canvas, area * sizeof *canvas);
        }
        if (gif_paint(canvas, width, indices, f.palette, f.colors, f.x, f.y, f.w, f.h,
                      f.interlaced, f.transparent)) goto out;
        time += f.delay;

        /* Keep every frame of short GIFs. For longer ones, spread the cache
         * across the entire cycle and preserve the original loop duration. */
        unsigned delay = f.delay;
        if (frames > limit) {
            unsigned start = (unsigned)((uint64_t)sample * duration / limit);
            while (sample < limit && (uint64_t)sample * duration / limit < time) ++sample;
            delay = (unsigned)((uint64_t)sample * duration / limit) - start;
        }
        if (delay) {
            workshop_thumb *image = malloc(sizeof *image);
            if (!image) goto out;
            workshop_thumbnail_scale(image, (const uint8_t *)canvas, width, height, 4);
            animation->delay[animation->count] = delay;
            animation->frames[animation->count++] = image;
        }
        if (f.disposal == 3) memcpy(canvas, previous, area * sizeof *canvas);
        else if (f.disposal == 2)
            for (unsigned row = 0; row < f.h; ++row)
                memset(canvas + (f.y + row) * width + f.x, 0, f.w * sizeof *canvas);
    }
    rc = next == 0 && animation->count ? 0 : -1;
out:
    free(packed);
    free(indices);
    free(previous);
    free(canvas);
    return rc;
}
