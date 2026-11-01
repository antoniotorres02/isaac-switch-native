/* QR encoder (ISO/IEC 18004). See native_qr.h. tools/test_switch_steamsync.py
 * checks every length against qrencode, module by module. */
#include "native_qr.h"

#include <stdlib.h>
#include <string.h>

#define MAX_CODEWORDS 346
#define MAX_ECC 26

/* Level M: all codewords, error correction codewords per block, blocks. */
static const struct {
    uint16_t total;
    uint8_t ecc, blocks;
} VERSIONS[10] = {
    { 26, 10, 1 }, { 44, 16, 1 }, { 70, 26, 1 }, { 100, 18, 2 }, { 134, 24, 2 },
    { 172, 16, 4 }, { 196, 18, 4 }, { 242, 22, 4 }, { 292, 22, 5 }, { 346, 26, 5 },
};

/* ---- Reed-Solomon over GF(256), polynomial 0x11d --------------------------- */

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    unsigned r = 0;
    for (int i = 7; i >= 0; --i) {
        r = (r << 1) ^ ((r >> 7) * 0x11d);
        r ^= ((b >> i) & 1u) * a;
    }
    return (uint8_t)r;
}

static void rs_generator(uint8_t *gen, int n) {
    memset(gen, 0, (size_t)n);
    gen[n - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < n) gen[j] ^= gen[j + 1];
        }
        root = gf_mul(root, 2);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *gen, int n, uint8_t *out) {
    memset(out, 0, (size_t)n);
    for (int i = 0; i < len; ++i) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)n - 1);
        out[n - 1] = 0;
        for (int j = 0; j < n; ++j) out[j] ^= gf_mul(gen[j], factor);
    }
}

/* ---- codewords -------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    int bits;
} bitbuf;

static void put_bits(bitbuf *b, unsigned value, int count) {
    for (int i = count - 1; i >= 0; --i, ++b->bits)
        if ((value >> i) & 1u) b->buf[b->bits >> 3] |= (uint8_t)(0x80 >> (b->bits & 7));
}

/* Data with its error correction, blocks interleaved. */
static void build_codewords(const uint8_t *data, size_t len, int version, uint8_t *out) {
    const int total = VERSIONS[version - 1].total, ecc = VERSIONS[version - 1].ecc;
    const int blocks = VERSIONS[version - 1].blocks;
    const int data_cw = total - ecc * blocks;
    uint8_t stream[MAX_CODEWORDS];
    memset(stream, 0, sizeof stream);
    bitbuf b = { stream, 0 };
    put_bits(&b, 4, 4);                                 /* byte mode */
    put_bits(&b, (unsigned)len, version < 10 ? 8 : 16);
    for (size_t i = 0; i < len; ++i) put_bits(&b, data[i], 8);
    int room = data_cw * 8 - b.bits;
    put_bits(&b, 0, room < 4 ? room : 4);               /* terminator */
    b.bits = (b.bits + 7) & ~7;
    for (uint8_t pad = 0xec; b.bits < data_cw * 8; pad ^= 0xec ^ 0x11) put_bits(&b, pad, 8);

    /* The first blocks may be one data codeword shorter than the rest. */
    const int short_blocks = blocks - data_cw % blocks;
    const int short_len = data_cw / blocks;
    uint8_t gen[MAX_ECC], rem[5][MAX_ECC];
    rs_generator(gen, ecc);
    const uint8_t *start[5];
    int off = 0;
    for (int i = 0; i < blocks; ++i) {
        int n = short_len + (i >= short_blocks);
        start[i] = stream + off;
        rs_remainder(stream + off, n, gen, ecc, rem[i]);
        off += n;
    }
    int o = 0;
    for (int k = 0; k <= short_len; ++k)
        for (int i = 0; i < blocks; ++i)
            if (k < short_len + (i >= short_blocks)) out[o++] = start[i][k];
    for (int k = 0; k < ecc; ++k)
        for (int i = 0; i < blocks; ++i) out[o++] = rem[i][k];
}

/* ---- the matrix ------------------------------------------------------------- */

typedef struct {
    int size;
    uint8_t *m;
    uint8_t func[ISAAC_QR_MAX_SIZE * ISAAC_QR_MAX_SIZE];
} qr;

static void put_func(qr *q, int x, int y, int dark) {
    if (x < 0 || y < 0 || x >= q->size || y >= q->size) return;
    q->m[y * q->size + x] = (uint8_t)dark;
    q->func[y * q->size + x] = 1;
}

static void draw_finder(qr *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            put_func(q, cx + dx, cy + dy, d != 2 && d != 4);
        }
}

static void draw_alignment(qr *q, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            put_func(q, cx + dx, cy + dy, d != 1);
        }
}

static void draw_format(qr *q, int mask) {
    /* Level M is 00, so the five data bits are the mask. */
    unsigned rem = (unsigned)mask;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    unsigned bits = (((unsigned)mask << 10) | rem) ^ 0x5412;
#define BIT(i) ((int)((bits >> (i)) & 1u))
    for (int i = 0; i < 6; ++i) put_func(q, 8, i, BIT(i));
    put_func(q, 8, 7, BIT(6));
    put_func(q, 8, 8, BIT(7));
    put_func(q, 7, 8, BIT(8));
    for (int i = 9; i < 15; ++i) put_func(q, 14 - i, 8, BIT(i));
    for (int i = 0; i < 8; ++i) put_func(q, q->size - 1 - i, 8, BIT(i));
    for (int i = 8; i < 15; ++i) put_func(q, 8, q->size - 15 + i, BIT(i));
#undef BIT
    put_func(q, 8, q->size - 8, 1);
}

static void draw_function_patterns(qr *q, int version) {
    for (int i = 0; i < q->size; ++i) {
        put_func(q, 6, i, i % 2 == 0);
        put_func(q, i, 6, i % 2 == 0);
    }
    draw_finder(q, 3, 3);
    draw_finder(q, q->size - 4, 3);
    draw_finder(q, 3, q->size - 4);
    if (version >= 2) {
        static const uint8_t MIDDLE[] = { 22, 24, 26, 28 };     /* versions 7-10 */
        int pos[3], n = 0;
        pos[n++] = 6;
        if (version >= 7) pos[n++] = MIDDLE[version - 7];
        pos[n++] = q->size - 7;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
                draw_alignment(q, pos[i], pos[j]);
            }
    }
    draw_format(q, 0);                                  /* reserves the area */
    if (version >= 7) {
        unsigned rem = (unsigned)version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
        unsigned bits = ((unsigned)version << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            int dark = (bits >> i) & 1u, a = q->size - 11 + i % 3, c = i / 3;
            put_func(q, a, c, dark);
            put_func(q, c, a, dark);
        }
    }
}

static void place_codewords(qr *q, const uint8_t *cw, int count) {
    int i = 0, bits = count * 8;
    for (int right = q->size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < q->size; ++vert)
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                int y = ((right + 1) & 2) == 0 ? q->size - 1 - vert : vert;
                if (q->func[y * q->size + x] || i >= bits) continue;
                q->m[y * q->size + x] = (cw[i >> 3] >> (7 - (i & 7))) & 1u;
                ++i;
            }
    }
}

static void apply_mask(qr *q, int mask) {
    for (int y = 0; y < q->size; ++y)
        for (int x = 0; x < q->size; ++x) {
            int on;
            switch (mask) {
            case 0: on = (x + y) % 2 == 0; break;
            case 1: on = y % 2 == 0; break;
            case 2: on = x % 3 == 0; break;
            case 3: on = (x + y) % 3 == 0; break;
            case 4: on = (x / 3 + y / 2) % 2 == 0; break;
            case 5: on = x * y % 2 + x * y % 3 == 0; break;
            case 6: on = (x * y % 2 + x * y % 3) % 2 == 0; break;
            default: on = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if (on && !q->func[y * q->size + x]) q->m[y * q->size + x] ^= 1;
        }
}

/* The standard's four penalty rules; the lowest score reads best. */
static long penalty(const qr *q) {
    const int n = q->size;
    const uint8_t *m = q->m;
    long score = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a) {
            int run = 0, last = -1;
            unsigned window = 0;
            for (int b = 0; b < n; ++b) {
                int v = pass ? m[b * n + a] : m[a * n + b];
                if (v == last) {
                    if (++run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    last = v;
                    run = 1;
                }
                window = ((window << 1) | (unsigned)v) & 0x7ff;
                if (b >= 10 && (window == 0x5d0 || window == 0x05d)) score += 40;
            }
        }
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            int v = m[y * n + x];
            if (v == m[y * n + x + 1] && v == m[(y + 1) * n + x] && v == m[(y + 1) * n + x + 1]) score += 3;
        }
    int dark = 0;
    for (int i = 0; i < n * n; ++i) dark += m[i];
    int total = n * n, k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    return score + k * 10;
}

int isaac_qr_encode(const void *data, size_t len, int mask, uint8_t *modules) {
    int version = 0;
    for (int v = 1; v <= 10 && !version; ++v) {
        int data_cw = VERSIONS[v - 1].total - VERSIONS[v - 1].ecc * VERSIONS[v - 1].blocks;
        size_t head_bits = 4 + (v < 10 ? 8 : 16);
        if (head_bits + len * 8 <= (size_t)data_cw * 8 && (v == 10 || len < 256)) version = v;
    }
    if (!version || mask < -1 || mask > 7) return 0;

    uint8_t cw[MAX_CODEWORDS];
    build_codewords(data, len, version, cw);

    qr q;
    q.size = 17 + 4 * version;
    q.m = modules;
    memset(q.func, 0, sizeof q.func);
    memset(modules, 0, (size_t)q.size * q.size);
    draw_function_patterns(&q, version);
    place_codewords(&q, cw, VERSIONS[version - 1].total);

    if (mask < 0) {
        long best = 0;
        for (int i = 0; i < 8; ++i) {
            apply_mask(&q, i);
            draw_format(&q, i);
            long s = penalty(&q);
            if (mask < 0 || s < best) {
                best = s;
                mask = i;
            }
            apply_mask(&q, i);                          /* a mask undoes itself */
        }
    }
    apply_mask(&q, mask);
    draw_format(&q, mask);
    return q.size;
}
