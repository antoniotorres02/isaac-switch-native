/* SHA-1 (FIPS 180-4). See native_sha1.h for why the port needs it. */
#include "native_sha1.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(isaac_sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8f1bbcdcu; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void isaac_sha1_init(isaac_sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xefcdab89u; s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u; s->h[4] = 0xc3d2e1f0u;
    s->bits = 0;
    s->used = 0;
}

void isaac_sha1_update(isaac_sha1 *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->bits += (uint64_t)len * 8u;
    if (s->used) {
        size_t take = 64 - s->used;
        if (take > len) take = len;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64) {
            block(s, s->block);
            s->used = 0;
        }
    }
    while (len >= 64) {
        block(s, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(s->block, p, len);
        s->used = len;
    }
}

void isaac_sha1_final(isaac_sha1 *s, uint8_t out[ISAAC_SHA1_DIGEST]) {
    uint64_t bits = s->bits;
    uint8_t pad[72];
    size_t padlen = (s->used < 56) ? 56 - s->used : 120 - s->used;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (int i = 0; i < 8; ++i) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    isaac_sha1_update(s, pad, padlen + 8);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void isaac_sha1_buf(const void *data, size_t len, uint8_t out[ISAAC_SHA1_DIGEST]) {
    isaac_sha1 s;
    isaac_sha1_init(&s);
    isaac_sha1_update(&s, data, len);
    isaac_sha1_final(&s, out);
}

void isaac_sha1_hex(const uint8_t digest[ISAAC_SHA1_DIGEST], char out[41]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < ISAAC_SHA1_DIGEST; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[40] = 0;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int isaac_sha1_from_hex(const char *hex, uint8_t out[ISAAC_SHA1_DIGEST]) {
    if (!hex || strlen(hex) < 40) return -1;
    for (int i = 0; i < ISAAC_SHA1_DIGEST; ++i) {
        int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}
