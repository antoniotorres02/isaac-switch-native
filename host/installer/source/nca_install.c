/* See nca_install.h. The NCA layout written here for the meta NCA is the
 * one payload.py writes for the other two; keep them in step. */
#include "nca_install.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 0xC00
#define MEDIA 0x200
#define META_BLOCK 0x1000
#define CHUNK (4u << 20)
#define SDK_VERSION 0x000C1100u

static void fail(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void sha256(const void *data, size_t len, uint8_t out[32]) {
    inst_sha256 *s = inst_sha256_new();
    inst_sha256_update(s, data, len);
    inst_sha256_final(s, out);
}

static int hexbytes(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return s[2 * n] == 0 || s[2 * n] == '\n' || s[2 * n] == ' ' ? 0 : -1;
}

/* ---- install.txt ------------------------------------------------------------- */

int inst_read_manifest(const char *dir, inst_manifest *m, char *err, size_t cap) {
    char path[512], line[512];
    memset(m, 0, sizeof *m);
    snprintf(path, sizeof path, "%s/install.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { fail(err, cap, "%s not found", path); return -1; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (line[0] == '#' || !sp) continue;
        *sp = 0;
        const char *k = line, *v = sp + 1;
        if (!strcmp(k, "title_id")) m->title_id = strtoull(v, NULL, 16);
        else if (!strcmp(k, "version")) m->version = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "display_version")) snprintf(m->display_version, sizeof m->display_version, "%s", v);
        else if (!strcmp(k, "name")) snprintf(m->name, sizeof m->name, "%s", v);
        else if (!strcmp(k, "content")) {
            char type[16], file[64], id[40];
            unsigned long long size;
            if (m->count >= INST_MAX_CONTENTS - 1 || sscanf(v, "%15s %63s %llu %39s", type, file, &size, id) != 4) {
                fclose(f);
                fail(err, cap, "install.txt: bad content line '%s'", v);
                return -1;
            }
            inst_content *c = &m->c[m->count++];
            snprintf(c->file, sizeof c->file, "%s", file);
            c->size = size;
            c->type = !strcmp(type, "program") ? INST_TYPE_PROGRAM : !strcmp(type, "control") ? INST_TYPE_CONTROL : 0xFF;
            if (c->type == 0xFF || hexbytes(id, c->id, 16) != 0) {
                fclose(f);
                fail(err, cap, "install.txt: bad content '%s'", v);
                return -1;
            }
            m->total += size;
        }
    }
    fclose(f);
    if (!m->title_id || m->count != 2) {
        fail(err, cap, "install.txt: needs a title_id and two contents");
        return -1;
    }
    return 0;
}

/* ---- one NCA: encrypt the header, stream, hash ---------------------------------- */

static int check_header(const uint8_t *h, const inst_manifest *m, const inst_content *c, char *err, size_t cap) {
    if (memcmp(h + 0x200, "NCA3", 4) != 0) {
        fail(err, cap, "%s: not a plaintext NCA3 (already encrypted, or damaged)", c->file);
        return -1;
    }
    if (get64(h + 0x208) != c->size || get64(h + 0x210) != m->title_id) {
        fail(err, cap, "%s: header does not match install.txt", c->file);
        return -1;
    }
    return 0;
}

static int stream_content(const char *dir, inst_manifest *m, inst_content *c, const uint8_t key[32],
                          const inst_io *io, uint64_t *done, char *err, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, c->file);
    FILE *f = fopen(path, "rb");
    if (!f) { fail(err, cap, "%s not found", path); return -1; }
    uint8_t *buf = malloc(CHUNK);
    int rc = -1;
    if (!buf) { fail(err, cap, "out of memory"); goto out; }
    if (fread(buf, 1, HEADER_SIZE, f) != HEADER_SIZE) { fail(err, cap, "%s: too short", path); goto out; }
    if (check_header(buf, m, c, err, cap) != 0) goto out;
    inst_xts_encrypt(key, buf, HEADER_SIZE, 0);
    if (io->create(io->user, c) != 0) { fail(err, cap, "%s: cannot create the content", c->file); goto out; }
    inst_sha256 *s = inst_sha256_new();
    uint64_t off = 0;
    size_t n = HEADER_SIZE;
    for (;;) {
        inst_sha256_update(s, buf, n);
        if (io->write(io->user, c, off, buf, n) != 0) {
            inst_sha256_final(s, c->hash);
            fail(err, cap, "%s: write failed at %llu", c->file, (unsigned long long)off);
            goto out;
        }
        off += n;
        *done += n;
        if (io->progress) io->progress(io->user, c->file, *done, m->total);
        if (off == c->size) break;
        size_t want = c->size - off < CHUNK ? (size_t)(c->size - off) : CHUNK;
        n = fread(buf, 1, want, f);
        if (n != want) {
            inst_sha256_final(s, c->hash);
            fail(err, cap, "%s: read failed at %llu (the file is shorter than install.txt says)", c->file,
                 (unsigned long long)off);
            goto out;
        }
    }
    inst_sha256_final(s, c->hash);
    if (fgetc(f) != EOF) { fail(err, cap, "%s: longer than install.txt says", c->file); goto out; }
    if (io->commit(io->user, c) != 0) { fail(err, cap, "%s: cannot register the content", c->file); goto out; }
    rc = 0;
out:
    free(buf);
    fclose(f);
    return rc;
}

/* ---- the meta NCA ---------------------------------------------------------------- */

static size_t make_cnmt(const inst_manifest *m, uint8_t *out) {
    size_t n = 0x20 + 0x10 + 0x38 * (size_t)m->count + 0x20;
    memset(out, 0, n);
    put64(out, m->title_id);
    put32(out + 0x8, m->version);
    out[0xC] = 0x80;                          /* Application */
    put16(out + 0xE, 0x10);                   /* extended header size */
    put16(out + 0x10, (uint16_t)m->count);
    put64(out + 0x20, m->title_id + 0x800);   /* patch id */
    for (int i = 0; i < m->count; ++i) {
        uint8_t *r = out + 0x30 + 0x38 * i;
        memcpy(r, m->c[i].hash, 32);
        memcpy(r + 0x20, m->c[i].id, 16);
        for (int b = 0; b < 6; ++b) r[0x30 + b] = (uint8_t)(m->c[i].size >> (8 * b));
        r[0x36] = m->c[i].type;
    }
    return n;
}

/* A one-file PFS0 padded to META_BLOCK; returns its size. */
static size_t make_pfs0(const char *name, const uint8_t *data, size_t len, uint8_t *out, size_t cap) {
    size_t names = strlen(name) + 1, head = 0x10 + 0x18 + names;
    size_t names_padded = names + (align_up(head, 0x20) - head);
    size_t total = align_up(0x10 + 0x18 + names_padded + len, META_BLOCK);
    if (total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "PFS0", 4);
    put32(out + 4, 1);
    put32(out + 8, (uint32_t)names_padded);
    put64(out + 0x10, 0);
    put64(out + 0x18, len);
    memcpy(out + 0x28, name, names - 1);
    memcpy(out + 0x10 + 0x18 + names_padded, data, len);
    return total;
}

static int build_meta(inst_manifest *m, const uint8_t key[32], uint8_t **nca_out, char *err, size_t cap) {
    uint8_t cnmt[0x20 + 0x10 + 0x38 * INST_MAX_CONTENTS + 0x20], pfs0[META_BLOCK * 2];
    char name[64];
    size_t cnmt_len = make_cnmt(m, cnmt);
    snprintf(name, sizeof name, "Application_%016llx.cnmt", (unsigned long long)m->title_id);
    size_t pfs0_len = make_pfs0(name, cnmt, cnmt_len, pfs0, sizeof pfs0);
    if (!pfs0_len) { fail(err, cap, "cnmt too large"); return -1; }

    size_t blocks = pfs0_len / META_BLOCK, table_len = 32 * blocks;
    size_t pfs0_off = (size_t)align_up(table_len, MEDIA);
    size_t body = (size_t)align_up(pfs0_off + pfs0_len, MEDIA), size = HEADER_SIZE + body;
    uint8_t *nca = calloc(1, size);
    if (!nca) { fail(err, cap, "out of memory"); return -1; }
    uint8_t *sec = nca + HEADER_SIZE;
    for (size_t i = 0; i < blocks; ++i) sha256(pfs0 + i * META_BLOCK, META_BLOCK, sec + 32 * i);
    memcpy(sec + pfs0_off, pfs0, pfs0_len);

    uint8_t *fs = nca + 0x400;                  /* fs header 0 */
    put16(fs, 2);
    fs[2] = 1;                                  /* PFS0 */
    fs[3] = 2;                                  /* HierarchicalSha256 */
    fs[4] = 1;                                  /* no encryption */
    sha256(sec, table_len, fs + 8);             /* master hash */
    put32(fs + 0x28, META_BLOCK);
    put32(fs + 0x2C, 2);
    put64(fs + 0x30, 0);
    put64(fs + 0x38, table_len);
    put64(fs + 0x40, pfs0_off);
    put64(fs + 0x48, pfs0_len);

    memcpy(nca + 0x200, "NCA3", 4);
    nca[0x205] = 1;                             /* Meta */
    put64(nca + 0x208, size);
    put64(nca + 0x210, m->title_id);
    put32(nca + 0x21C, SDK_VERSION);
    put32(nca + 0x240, HEADER_SIZE / MEDIA);
    put32(nca + 0x244, (uint32_t)(size / MEDIA));
    nca[0x248] = 1;
    sha256(fs, 0x200, nca + 0x280);

    inst_content *c = &m->c[m->count];
    memset(c, 0, sizeof *c);
    snprintf(c->file, sizeof c->file, "meta.nca");
    c->type = INST_TYPE_META;
    c->size = size;
    inst_xts_encrypt(key, nca, HEADER_SIZE, 0);
    sha256(nca, size, c->hash);
    memcpy(c->id, c->hash, 16);
    *nca_out = nca;
    return 0;
}

/* ncm's content meta record: NcmContentMetaHeader, the application's
 * extended header, then one NcmContentInfo per content, the meta NCA too. */
static size_t make_record(const inst_manifest *m, int total, uint8_t *out, size_t cap) {
    size_t n = 8 + 0x10 + 0x18 * (size_t)total;
    if (n > cap) return 0;
    memset(out, 0, n);
    put16(out, 0x10);
    put16(out + 2, (uint16_t)total);
    put64(out + 8, m->title_id + 0x800);
    for (int i = 0; i < total; ++i) {
        uint8_t *r = out + 0x18 + 0x18 * i;
        memcpy(r, m->c[i].id, 16);
        put32(r + 0x10, (uint32_t)m->c[i].size);
        r[0x14] = (uint8_t)(m->c[i].size >> 32);
        r[0x16] = m->c[i].type;
    }
    return n;
}

int inst_install(const char *dir, inst_manifest *m, const uint8_t key[32], const inst_io *io, uint8_t *record,
                 size_t record_cap, size_t *record_len, char *err, size_t cap) {
    uint64_t done = 0;
    for (int i = 0; i < m->count; ++i)
        if (stream_content(dir, m, &m->c[i], key, io, &done, err, cap) != 0) return -1;
    uint8_t *meta = NULL;
    if (build_meta(m, key, &meta, err, cap) != 0) return -1;
    inst_content *c = &m->c[m->count];
    int rc = -1;
    if (io->create(io->user, c) != 0 || io->write(io->user, c, 0, meta, (size_t)c->size) != 0 ||
        io->commit(io->user, c) != 0) {
        fail(err, cap, "cannot install the meta NCA");
        goto out;
    }
    *record_len = make_record(m, m->count + 1, record, record_cap);
    if (!*record_len) { fail(err, cap, "record buffer too small"); goto out; }
    rc = 0;
out:
    free(meta);
    return rc;
}
