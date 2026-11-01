/* The ZIP container Steam's cloud uses. See native_zip.h. */
#include "native_zip.h"

#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define LOCAL_SIG  0x04034b50u
#define CENTRAL_SIG 0x02014b50u
#define EOCD_SIG   0x06054b50u

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Deflate `len` bytes with raw (headerless) deflate, as ZIP wants. */
static size_t deflate_raw(const void *src, size_t len, void *dst, size_t cap) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)len;
    s.next_out = dst;
    s.avail_out = (uInt)cap;
    int rc = deflate(&s, Z_FINISH);
    size_t written = cap - s.avail_out;
    deflateEnd(&s);
    return rc == Z_STREAM_END ? written : 0;
}

static int inflate_raw(const void *src, size_t len, void *dst, size_t cap, size_t *written) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return -1;
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)len;
    s.next_out = dst;
    s.avail_out = (uInt)cap;
    int rc = inflate(&s, Z_FINISH);
    *written = cap - s.avail_out;
    inflateEnd(&s);
    return rc == Z_STREAM_END ? 0 : -1;
}

size_t isaac_zip_pack(void *out, size_t cap, const void *raw, size_t raw_len) {
    uint8_t *o = out;
    const size_t name_len = sizeof(ISAAC_ZIP_ENTRY) - 1;
    if (!o || !raw || raw_len > 0x7fffffffu) return 0;
    /* One allocation for the compressed body; saves are tens of KB. */
    size_t bound = compressBound((uLong)raw_len) + 64;
    uint8_t *body = malloc(bound);
    if (!body) return 0;
    size_t body_len = deflate_raw(raw, raw_len, body, bound);
    int method = 8;
    if (!body_len || body_len >= raw_len) {          /* store when deflate loses */
        memcpy(body, raw, raw_len);
        body_len = raw_len;
        method = 0;
    }
    uint32_t crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)raw, (uInt)raw_len);
    size_t local = 30 + name_len, central = 46 + name_len, eocd = 22;
    size_t total = local + body_len + central + eocd;
    if (total > cap) {
        free(body);
        return 0;
    }

    uint8_t *p = o;
    put32(p + 0, LOCAL_SIG);
    put16(p + 4, 20);                 /* version needed: 2.0 (deflate) */
    put16(p + 6, 0);                  /* flags */
    put16(p + 8, (uint16_t)method);
    put16(p + 10, 0);                 /* mod time: fixed, the mtime is Steam's */
    put16(p + 12, 0x21);              /* mod date: 1980-01-01, as the spec wants */
    put32(p + 14, crc);
    put32(p + 18, (uint32_t)body_len);
    put32(p + 22, (uint32_t)raw_len);
    put16(p + 26, (uint16_t)name_len);
    put16(p + 28, 0);
    memcpy(p + 30, ISAAC_ZIP_ENTRY, name_len);
    memcpy(p + local, body, body_len);

    p = o + local + body_len;
    put32(p + 0, CENTRAL_SIG);
    put16(p + 4, 20);                 /* version made by */
    put16(p + 6, 20);                 /* version needed */
    put16(p + 8, 0);
    put16(p + 10, (uint16_t)method);
    put16(p + 12, 0);
    put16(p + 14, 0x21);
    put32(p + 16, crc);
    put32(p + 20, (uint32_t)body_len);
    put32(p + 24, (uint32_t)raw_len);
    put16(p + 28, (uint16_t)name_len);
    put16(p + 30, 0);                 /* extra */
    put16(p + 32, 0);                 /* comment */
    put16(p + 34, 0);                 /* disk */
    put16(p + 36, 0);                 /* internal attributes */
    put32(p + 38, 0);                 /* external attributes */
    put32(p + 42, 0);                 /* local header offset */
    memcpy(p + 46, ISAAC_ZIP_ENTRY, name_len);

    p = o + local + body_len + central;
    put32(p + 0, EOCD_SIG);
    put16(p + 4, 0);
    put16(p + 6, 0);
    put16(p + 8, 1);
    put16(p + 10, 1);
    put32(p + 12, (uint32_t)central);
    put32(p + 16, (uint32_t)(local + body_len));
    put16(p + 20, 0);

    free(body);
    return total;
}

int isaac_zip_looks_like_zip(const void *data, size_t len) {
    const uint8_t *p = data;
    return p && len >= 4 && p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4;
}

int isaac_zip_unpack(const void *zip, size_t zip_len, void *out, size_t cap, size_t *out_len) {
    const uint8_t *z = zip;
    if (!z || !out || zip_len < 30 + sizeof(ISAAC_ZIP_ENTRY) - 1 + 22) return -1;
    if (get32(z) != LOCAL_SIG) return -1;
    uint16_t method = get16(z + 8);
    uint32_t crc = get32(z + 14);
    uint32_t packed = get32(z + 18);
    uint32_t raw = get32(z + 22);
    uint16_t name_len = get16(z + 26), extra_len = get16(z + 28);
    size_t data = 30u + name_len + extra_len;
    if (data + packed > zip_len || raw > cap) return -1;

    size_t written = 0;
    if (method == 0) {
        memcpy(out, z + data, packed);
        written = packed;
    } else if (method == 8) {
        if (inflate_raw(z + data, packed, out, cap, &written) != 0) return -1;
    } else {
        return -1;
    }
    if (written != raw) return -1;
    if ((uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)out, (uInt)written) != crc) return -1;
    *out_len = written;
    return 0;
}
