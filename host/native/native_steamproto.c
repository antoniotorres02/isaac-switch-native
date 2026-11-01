/* Hand-rolled protobuf and CM framing. See native_steamproto.h. */
#include "native_steamproto.h"

#include <string.h>

#include <zlib.h>

/* ---- writer --------------------------------------------------------------- */

void isaac_pbw_init(isaac_pbw *w, void *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

static void put(isaac_pbw *w, const void *data, size_t len) {
    if (w->overflow || w->len + len > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
}

static void put_varint(isaac_pbw *w, uint64_t v) {
    uint8_t tmp[10];
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        tmp[n++] = (uint8_t)(b | (v ? 0x80 : 0));
    } while (v);
    put(w, tmp, n);
}

static void put_key(isaac_pbw *w, unsigned field, unsigned wire) {
    put_varint(w, (uint64_t)field << 3 | wire);
}

void isaac_pbw_varint(isaac_pbw *w, unsigned field, uint64_t value) {
    put_key(w, field, 0);
    put_varint(w, value);
}

void isaac_pbw_fixed64(isaac_pbw *w, unsigned field, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(value >> (8 * i));
    put_key(w, field, 1);
    put(w, b, sizeof b);
}

void isaac_pbw_fixed32(isaac_pbw *w, unsigned field, uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(value >> (8 * i));
    put_key(w, field, 5);
    put(w, b, sizeof b);
}

void isaac_pbw_bytes(isaac_pbw *w, unsigned field, const void *data, size_t len) {
    put_key(w, field, 2);
    put_varint(w, len);
    put(w, data, len);
}

void isaac_pbw_str(isaac_pbw *w, unsigned field, const char *s) {
    isaac_pbw_bytes(w, field, s, strlen(s));
}

/* ---- reader --------------------------------------------------------------- */

void isaac_pbr_init(isaac_pbr *r, const void *data, size_t len) {
    r->p = data;
    r->end = r->p + len;
}

static int get_varint(isaac_pbr *r, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end && shift <= 63) {
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int isaac_pbr_next(isaac_pbr *r, isaac_pbfield *f) {
    if (r->p >= r->end) return 0;
    uint64_t key;
    if (get_varint(r, &key) != 0) return -1;
    f->field = (unsigned)(key >> 3);
    f->wire = (unsigned)(key & 7);
    f->value = 0;
    f->bytes = NULL;
    f->len = 0;
    switch (f->wire) {
    case 0:
        return get_varint(r, &f->value) == 0 ? 1 : -1;
    case 1:
        if (r->end - r->p < 8) return -1;
        f->value = 0;
        for (int i = 0; i < 8; ++i) f->value |= (uint64_t)r->p[i] << (8 * i);
        r->p += 8;
        return 1;
    case 2: {
        uint64_t n;
        if (get_varint(r, &n) != 0 || n > (uint64_t)(r->end - r->p)) return -1;
        f->bytes = r->p;
        f->len = (size_t)n;
        r->p += n;
        return 1;
    }
    case 5:
        if (r->end - r->p < 4) return -1;
        f->value = (uint64_t)r->p[0] | (uint64_t)r->p[1] << 8 |
                   (uint64_t)r->p[2] << 16 | (uint64_t)r->p[3] << 24;
        r->p += 4;
        return 1;
    default:
        return -1;
    }
}

uint64_t isaac_pbr_uint(const void *data, size_t len, unsigned field, uint64_t fallback) {
    isaac_pbr r;
    isaac_pbfield f;
    uint64_t value = fallback;
    isaac_pbr_init(&r, data, len);
    while (isaac_pbr_next(&r, &f) == 1)
        if (f.field == field) value = f.value;
    return value;
}

int isaac_pbr_str(const void *data, size_t len, unsigned field, char *out, size_t cap) {
    isaac_pbr r;
    isaac_pbfield f;
    int found = -1;
    isaac_pbr_init(&r, data, len);
    while (isaac_pbr_next(&r, &f) == 1) {
        if (f.field != field || f.wire != 2) continue;
        size_t n = f.len < cap - 1 ? f.len : cap - 1;
        memcpy(out, f.bytes, n);
        out[n] = 0;
        found = (int)n;
    }
    return found;
}

/* ---- framing -------------------------------------------------------------- */

size_t isaac_cm_frame(void *out, size_t cap, unsigned emsg,
                      const void *hdr, size_t hlen, const void *body, size_t blen) {
    uint8_t *o = out;
    if (!o || cap < 8 + hlen + blen) return 0;
    uint32_t raw = emsg | 0x80000000u;
    o[0] = (uint8_t)raw; o[1] = (uint8_t)(raw >> 8);
    o[2] = (uint8_t)(raw >> 16); o[3] = (uint8_t)(raw >> 24);
    uint32_t h = (uint32_t)hlen;
    o[4] = (uint8_t)h; o[5] = (uint8_t)(h >> 8);
    o[6] = (uint8_t)(h >> 16); o[7] = (uint8_t)(h >> 24);
    if (hlen) memcpy(o + 8, hdr, hlen);
    if (blen) memcpy(o + 8 + hlen, body, blen);
    return 8 + hlen + blen;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int gunzip(const void *src, size_t len, void *dst, size_t cap, size_t *out_len) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, 15 + 16) != Z_OK) return -1;   /* gzip wrapper */
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)len;
    s.next_out = dst;
    s.avail_out = (uInt)cap;
    int rc = inflate(&s, Z_FINISH);
    *out_len = cap - s.avail_out;
    inflateEnd(&s);
    return rc == Z_STREAM_END ? 0 : -1;
}

int isaac_cm_split(const void *payload, size_t len, void *scratch, size_t scratch_cap,
                   isaac_cm_msg_fn fn, void *user) {
    const uint8_t *p = payload;
    if (len < 8) return -1;
    uint32_t raw = rd32(p);
    unsigned emsg = raw & 0x7fffffffu;
    if (!(raw & 0x80000000u)) {
        /* Struct message: 20-byte header, then the body. */
        if (len < 20) return -1;
        isaac_cmmsg m = { emsg, NULL, 0, p + 20, len - 20 };
        fn(user, &m);
        return 0;
    }
    uint32_t hlen = rd32(p + 4);
    if (len < 8u + hlen) return -1;
    const uint8_t *hdr = p + 8;
    const uint8_t *body = p + 8 + hlen;
    size_t blen = len - 8 - hlen;

    if (emsg == ISAAC_EMSG_MULTI) {
        /* size_unzipped(1) message_body(2); gzip when 1 is present. */
        isaac_pbr r;
        isaac_pbfield f;
        const uint8_t *blob = NULL;
        size_t blob_len = 0;
        int compressed = 0;
        isaac_pbr_init(&r, body, blen);
        while (isaac_pbr_next(&r, &f) == 1) {
            if (f.field == 1 && f.wire == 0 && f.value) compressed = 1;
            else if (f.field == 2 && f.wire == 2) {
                blob = f.bytes;
                blob_len = f.len;
            }
        }
        if (!blob) return -1;
        if (compressed) {
            size_t inflated = 0;
            if (!scratch) return -1;
            if (gunzip(blob, blob_len, scratch, scratch_cap, &inflated) != 0) return -1;
            blob = scratch;
            blob_len = inflated;
        }
        size_t off = 0;
        while (off + 4 <= blob_len) {
            uint32_t n = rd32(blob + off);
            off += 4;
            if (n > blob_len - off) return -1;
            if (isaac_cm_split(blob + off, n, NULL, 0, fn, user) != 0) return -1;
            off += n;
        }
        return 0;
    }

    isaac_cmmsg m = { emsg, hdr, hlen, body, blen };
    fn(user, &m);
    return 0;
}
