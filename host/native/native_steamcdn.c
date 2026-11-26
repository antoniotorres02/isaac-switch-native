#include "native_steamcdn.h"
#include "native_sha1.h"
#include "native_steamproto.h"
#include "native_zip.h"
#include "native_workshop_media.h"

#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jpeglib.h>
#include <lzma.h>
#include <png.h>
#include <zlib.h>
#include <zstd.h>

#ifdef __SWITCH__
#include <switch/crypto/aes.h>
#include <switch/crypto/aes_cbc.h>
#else
#include <openssl/evp.h>
#endif

#define MANIFEST_ZIP_CAP (8u << 20)
#define MANIFEST_CAP (16u << 20)
#define CHUNK_CAP (2u << 20)
#define FILE_CAP UINT32_MAX
#define MAX_FILES 100000u
#define MAX_SERVERS 8
#define FILE_DIRECTORY 64u
#define FILE_SYMLINK 512u

typedef struct {
    char name[128], token[1024];
} cdn_server;

typedef struct {
    const workshop_net *net;
    uint8_t key[32];
    cdn_server servers[MAX_SERVERS];
    unsigned count, preferred;
} cdn;

typedef struct {
    uint64_t offset;
    uint32_t size, packed, adler;
    uint8_t id[20];
} depot_chunk;

typedef struct {
    uint64_t size;
    unsigned flags, has_digest;
    uint8_t digest[20];
    char path[MOD_PATH];
    depot_chunk *chunks;
    size_t count;
} depot_file;

typedef struct {
    const uint8_t *payload;
    size_t len;
    unsigned encrypted;
} depot_manifest;

static int fail(char err[WORKSHOP_ERR], const char *message) {
    snprintf(err, WORKSHOP_ERR, "%s", message);
    return -1;
}

static uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static int cancelled(const workshop_net *net) {
    return net->cancelled && net->cancelled(net->ctx);
}

static int request(const workshop_net *net, const char *method, const isaac_pbw *w, void *resp,
                   size_t cap, size_t *len, char err[WORKSHOP_ERR]) {
    if (cancelled(net)) return fail(err, "Cancelled");
    return net->call(net->ctx, method, w->buf, w->len, resp, cap, len, err);
}

static int host_domain(const char *host, const char *suffix) {
    size_t n = strlen(host), s = strlen(suffix);
    return n > s && !strcmp(host + n - s, suffix);
}

static int steam_host(const char *name, int image) {
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
            return 0;
    if (image)
        return host_domain(name, ".steamstatic.com") || host_domain(name, ".steamusercontent.com") ||
               !strcmp(name, "steamuserimages-a.akamaihd.net") || !strcmp(name, "i.imgur.com");
    return host_domain(name, ".steamcontent.com");
}

static int visible_url_part(const char *s, const char *forbidden) {
    for (; *s; ++s)
        if ((unsigned char)*s < 33 || (unsigned char)*s > 126 || strchr(forbidden, *s))
            return 0;
    return 1;
}

static int servers(cdn *c, char err[WORKSHOP_ERR]) {
    uint8_t req[32], reply[8192];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, c->net->cell);
    isaac_pbw_varint(&w, 2, 32);
    size_t n = 0;
    if (request(c->net, "ContentServerDirectory.GetServersForSteamPipe#1", &w, reply,
                sizeof reply, &n, err)) return -1;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, reply, n);
    int rc;
    while ((rc = isaac_pbr_next(&r, &f)) > 0 && c->count < MAX_SERVERS) {
        if (f.field != 1 || f.wire != 2) continue;
        char host[128], support[32];
        if (isaac_pbr_uint(f.bytes, f.len, 7, 0) || isaac_pbr_uint(f.bytes, f.len, 10, 0)) continue;
        if (isaac_pbr_str(f.bytes, f.len, 12, support, sizeof support) > 0 &&
            !strcmp(support, "none")) continue;
        isaac_pbr inner;
        isaac_pbfield member;
        int restricted = 0, allowed = 0;
        isaac_pbr_init(&inner, f.bytes, f.len);
        while (isaac_pbr_next(&inner, &member) > 0) {
            if (member.field != 13) continue;
            restricted = 1;
            if (member.wire == 0 && member.value == WORKSHOP_APP) allowed = 1;
        }
        if (restricted && !allowed) continue;
        if (isaac_pbr_str(f.bytes, f.len, 9, host, sizeof host) <= 0 &&
            isaac_pbr_str(f.bytes, f.len, 8, host, sizeof host) <= 0) continue;
        if (!steam_host(host, 0)) continue;
        int duplicate = 0;
        for (unsigned i = 0; i < c->count; ++i)
            if (!strcmp(c->servers[i].name, host)) duplicate = 1;
        if (!duplicate) snprintf(c->servers[c->count++].name, sizeof host, "%s", host);
    }
    return rc >= 0 && c->count ? 0 : fail(err, "Steam returned no supported HTTPS content servers");
}

static uint64_t manifest_code(cdn *c, uint64_t id, char err[WORKSHOP_ERR]) {
    uint8_t req[64], reply[256];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, WORKSHOP_APP);
    isaac_pbw_varint(&w, 2, WORKSHOP_DEPOT);
    isaac_pbw_varint(&w, 3, id);
    /* SteamKit omits app_branch for the default public branch. */
    size_t n = 0;
    if (request(c->net, "ContentServerDirectory.GetManifestRequestCode#1", &w, reply,
                sizeof reply, &n, err)) return 0;
    uint64_t code = isaac_pbr_uint(reply, n, 1, 0);
    if (!code) fail(err, "Steam did not authorize this Workshop manifest");
    return code;
}

static int cdn_token(cdn *c, cdn_server *server, char err[WORKSHOP_ERR]) {
    uint8_t req[192], reply[1152];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, WORKSHOP_DEPOT);
    isaac_pbw_str(&w, 2, server->name);
    isaac_pbw_varint(&w, 3, WORKSHOP_APP);
    size_t n = 0;
    if (request(c->net, "ContentServerDirectory.GetCDNAuthToken#1", &w, reply,
                sizeof reply, &n, err)) return -1;
    int len = isaac_pbr_str(reply, n, 1, server->token, sizeof server->token);
    return len > 0 && (size_t)len == strlen(server->token) &&
           visible_url_part(server->token, "#") ? 0 : -1;
}

static int cdn_get(cdn *c, const char *endpoint, void *out, size_t cap, size_t *n,
                   char err[WORKSHOP_ERR]) {
    unsigned start = c->preferred;
    unsigned retries = c->count < 3 ? 3 : c->count;
    for (unsigned i = 0; i < retries; ++i) {
        unsigned pos = (start + i) % c->count;
        cdn_server *server = &c->servers[pos];
        for (unsigned auth = 0; auth < 2; ++auth) {
            if (cancelled(c->net)) return fail(err, "Cancelled");
            char path[1280];
            const char *token = server->token;
            snprintf(path, sizeof path, "%s%s%s", endpoint,
                     *token && *token != '?' ? "?" : "", token);
            int status = 0;
            int rc = c->net->get(c->net->ctx, server->name, path, out, cap, n, &status, err);
            if (!rc && status == 200) { c->preferred = pos; return 0; }
            if ((status != 401 && status != 403) || auth || cdn_token(c, server, err)) break;
        }
    }
    return fail(err, cancelled(c->net) ? "Cancelled" : "Steam CDN download failed or access was denied");
}

/* Steam encrypts the IV with ECB and the padded payload with CBC. */
static int decrypt(const uint8_t key[32], const uint8_t *in, size_t len,
                   uint8_t *out, size_t *plain) {
    if (len < 32 || len % 16) return -1;
    uint8_t iv[16];
#ifdef __SWITCH__
    Aes256Context ecb;
    Aes256CbcContext cbc;
    aes256ContextCreate(&ecb, key, false);
    aes256DecryptBlock(&ecb, iv, in);
    aes256CbcContextCreate(&cbc, key, iv, false);
    if (aes256CbcDecrypt(&cbc, out, in + 16, len - 16) != len - 16) return -1;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ivlen = 0, body = 0;
    int ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key, NULL) &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) && EVP_DecryptUpdate(ctx, iv, &ivlen, in, 16) &&
        ivlen == 16 && EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) && EVP_DecryptUpdate(ctx, out, &body, in + 16, (int)(len - 16)) &&
        body == (int)(len - 16);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;
#endif
    uint8_t pad = out[len - 17];
    if (!pad || pad > 16) return -1;
    for (unsigned i = 0; i < pad; ++i)
        if (out[len - 17 - i] != pad) return -1;
    *plain = len - 16 - pad;
    return 0;
}

static const char *filename_decode(const isaac_pbfield *f, const cdn *c, int encrypted,
                                  char out[MOD_PATH]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t plain[MOD_PATH + 32];
    const uint8_t *name = f->bytes;
    size_t len = f->len;
    if (encrypted) {
        uint8_t data[sizeof plain];
        size_t n = 0;
        unsigned acc = 0, bits = 0, pad = 0;
        while (len && !name[len - 1]) --len;
        for (size_t i = 0; i < len; ++i) {
            unsigned char b = name[i];
            if (b == ' ' || b == '\t' || b == '\r' || b == '\n') continue;
            if (b == '=') {
                if (++pad > 2) return "Invalid Base64 in Workshop filename";
                continue;
            }
            const char *p = b ? strchr(alphabet, b) : NULL;
            if (!p || pad) return "Invalid Base64 in Workshop filename";
            acc = (acc << 6) | (unsigned)(p - alphabet);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n == sizeof data) return "Encoded Workshop filename is too long";
                data[n++] = (uint8_t)(acc >> bits);
            }
        }
        if (bits > 4 || (pad && bits != pad * 2) || (acc & ((1u << bits) - 1)))
            return "Invalid Base64 in Workshop filename";
        if (n < 32 || n % 16) return "Invalid encrypted Workshop filename length";
        if (decrypt(c->key, data, n, plain, &len)) return "Workshop depot key could not decrypt filename";
        name = plain;
    }
    while (len && !name[len - 1]) --len;
    if (!len || len >= MOD_PATH || memchr(name, 0, len))
        return encrypted ? "Invalid decrypted Workshop filename" : "Invalid Workshop filename";
    memcpy(out, name, len);
    out[len] = 0;
    return NULL;
}

static int manifest_parse(const uint8_t *buf, size_t len, uint64_t id, depot_manifest *m) {
    static const uint32_t sections[] = {0x71f617d0, 0x1f4812be, 0x1b81b817};
    memset(m, 0, sizeof *m);
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; ++i) {
        if (len < 8 || le32(buf) != sections[i]) return -1;
        size_t n = le32(buf + 4);
        if (n > len - 8) return -1;
        const uint8_t *data = buf + 8;
        if (i == 0) {
            m->payload = data;
            m->len = n;
        } else if (i == 1) {
            isaac_pbr r;
            isaac_pbfield f;
            uint64_t depot = 0, manifest = 0;
            int rc;
            isaac_pbr_init(&r, data, n);
            while ((rc = isaac_pbr_next(&r, &f)) > 0) {
                if (f.field == 1 && f.wire == 0) depot = f.value;
                if (f.field == 2 && f.wire == 0) manifest = f.value;
                if (f.field == 4 && f.wire == 0) m->encrypted = f.value != 0;
            }
            if (rc || depot != WORKSHOP_DEPOT || manifest != id) return -1;
        }
        buf += 8 + n;
        len -= 8 + n;
    }
    return m->len && len == 4 && le32(buf) == 0x32c415ab ? 0 : -1;
}

static int parse_chunk(const isaac_pbfield *field, depot_chunk *chunk) {
    memset(chunk, 0, sizeof *chunk);
    isaac_pbr r;
    isaac_pbfield f;
    int rc, has_digest = 0, has_adler = 0;
    isaac_pbr_init(&r, field->bytes, field->len);
    while ((rc = isaac_pbr_next(&r, &f)) > 0) {
        if (f.field == 1 && f.wire == 2 && f.len == 20) {
            memcpy(chunk->id, f.bytes, 20);
            has_digest = 1;
        } else if (f.field == 2 && f.wire == 5) {
            chunk->adler = f.value;
            has_adler = 1;
        } else if (f.wire == 0) {
            if (f.field == 3) chunk->offset = f.value;
            if (f.field == 4 && f.value <= CHUNK_CAP) chunk->size = f.value;
            if (f.field == 5 && f.value <= CHUNK_CAP) chunk->packed = f.value;
        }
    }
    return !rc && has_digest && has_adler && chunk->size && chunk->packed ? 0 : -1;
}

static int chunk_order(const void *a, const void *b) {
    uint64_t aa = ((const depot_chunk *)a)->offset, bb = ((const depot_chunk *)b)->offset;
    return aa < bb ? -1 : aa > bb;
}

static const char *parse_file(const isaac_pbfield *field, const cdn *c, int encrypted, depot_file *file) {
    isaac_pbr r;
    isaac_pbfield f;
    int rc;
    size_t cap = 0;
    memset(file, 0, sizeof *file);
    isaac_pbr_init(&r, field->bytes, field->len);
    while ((rc = isaac_pbr_next(&r, &f)) > 0) {
        if (f.field == 1 && f.wire == 2) {
            const char *error = filename_decode(&f, c, encrypted, file->path);
            if (error) return error;
            if (mod_safe_path(file->path)) return "Unsafe Workshop path";
        } else if (f.field == 2 && f.wire == 0) file->size = f.value;
        else if (f.field == 3 && f.wire == 0) file->flags = f.value;
        else if (f.field == 5 && f.wire == 2 && f.len == 20) {
            memcpy(file->digest, f.bytes, 20);
            file->has_digest = 1;
        } else if (f.field == 6 && f.wire == 2) {
            if (file->count == MAX_FILES) return "Too many chunks in Workshop file";
            if (file->count == cap) {
                cap = cap ? cap * 2 : 8;
                depot_chunk *more = realloc(file->chunks, cap * sizeof *more);
                if (!more) return "Out of memory reading Workshop chunks";
                file->chunks = more;
            }
            if (parse_chunk(&f, &file->chunks[file->count++])) return "Invalid Workshop chunk record";
        } else if (f.field == 7 && f.wire == 2 && f.len) return "Unsupported Workshop symlink";
    }
    if (rc || !file->path[0]) return "Malformed Workshop file record";
    if (file->size > FILE_CAP) return "Workshop file exceeds the SD card file size limit";
    if (file->flags & FILE_SYMLINK) return "Unsupported Workshop symlink";
    if (file->flags & FILE_DIRECTORY)
        return file->size || file->count ? "Unexpected data in Workshop directory" : NULL;
    if (file->count > 1) qsort(file->chunks, file->count, sizeof *file->chunks, chunk_order);
    uint64_t size = 0;
    for (size_t i = 0; i < file->count; ++i) {
        const depot_chunk *chunk = &file->chunks[i];
        if (chunk->offset != size) return "Invalid Workshop chunk offset";
        size += chunk->size;
    }
    if (size != file->size) return "Workshop file size does not match its chunks";
    return file->has_digest || !size ? NULL : "Workshop file SHA-1 is missing";
}

static int chunk_unpack(const uint8_t *packed, size_t n, uint8_t *out, size_t size) {
    if (n >= 23 && !memcmp(packed, "VSZa", 4)) {
        if (memcmp(packed + n - 3, "zsv", 3) || le32(packed + n - 11) != size) return -1;
        return ZSTD_decompress(out, size, packed + 8, n - 23) == size &&
            crc32(0, out, size) == le32(packed + n - 15) ? 0 : -1;
    }
    if (n >= 22 && !memcmp(packed, "VZa", 3)) {
        if (memcmp(packed + n - 2, "zv", 2) || le32(packed + n - 6) != size) return -1;
        lzma_filter filters[] = {{LZMA_FILTER_LZMA1, NULL}, {LZMA_VLI_UNKNOWN, NULL}};
        if (lzma_properties_decode(&filters[0], NULL, packed + 7, 5) != LZMA_OK) return -1;
        lzma_options_lzma *options = filters[0].options;
        lzma_stream stream = LZMA_STREAM_INIT;
        lzma_ret init = options->dict_size <= 16u * CHUNK_CAP ? lzma_raw_decoder(&stream, filters) : LZMA_OPTIONS_ERROR;
        free(options);
        if (init != LZMA_OK) return -1;
        stream.next_in = packed + 12;
        stream.avail_in = n - 22;
        stream.next_out = out;
        stream.avail_out = size;
        /* Steam also emits LZMA streams without an EOS marker. The exact output
           size and both manifest hashes still have to match. */
        lzma_ret rc = lzma_code(&stream, LZMA_FINISH);
        int valid = (rc == LZMA_OK || rc == LZMA_STREAM_END) && stream.total_out == size;
        lzma_end(&stream);
        return valid && crc32(0, out, size) == le32(packed + n - 10) ? 0 : -1;
    }
    size_t out_len = 0;
    return isaac_zip_unpack(packed, n, out, size, &out_len) == 0 && out_len == size ? 0 : -1;
}

typedef struct {
    uint8_t *wire, *compressed, *raw;
} download_buffers;

static int download_chunk(cdn *c, const depot_chunk *chunk, download_buffers *buf,
                          char err[WORKSHOP_ERR]) {
    char hex[41], endpoint[100];
    isaac_sha1_hex(chunk->id, hex);
    snprintf(endpoint, sizeof endpoint, "/depot/%u/chunk/%s", WORKSHOP_DEPOT, hex);
    size_t n = 0, packed = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (cdn_get(c, endpoint, buf->wire, CHUNK_CAP, &n, err)) return -1;
        if (n != chunk->packed || decrypt(c->key, buf->wire, n, buf->compressed, &packed) ||
            chunk_unpack(buf->compressed, packed, buf->raw, chunk->size)) continue;
        isaac_sha1 sha;
        uint8_t digest[20];
        isaac_sha1_init(&sha);
        isaac_sha1_update(&sha, buf->raw, chunk->size);
        isaac_sha1_final(&sha, digest);
        if (!memcmp(digest, chunk->id, sizeof digest) &&
            adler32(0, buf->raw, chunk->size) == chunk->adler) return 0;
        c->preferred = (c->preferred + 1) % c->count;
    }
    return fail(err, "Workshop chunk failed integrity verification");
}

static int write_file(cdn *c, const char *stage, const depot_file *file, download_buffers *buf,
                      uint64_t *done, uint64_t total, char err[WORKSHOP_ERR]) {
    char path[MOD_PATH];
    int n = snprintf(path, sizeof path, "%s/%s", stage, file->path);
    if (n < 0 || n >= MOD_PATH) return fail(err, "Workshop path too long");
    if (file->flags & FILE_DIRECTORY)
        return mod_mkdir_tree(path) == 0 ? 0 : fail(err, "Invalid Workshop directory");
    char *slash = strrchr(path, '/');
    *slash = 0;
    if (mod_mkdir_tree(path)) return fail(err, "Cannot create Workshop file directory");
    *slash = '/';
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return fail(err, "Could not create Workshop file on the SD card");
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); return fail(err, "Could not open Workshop file"); }
    isaac_sha1 sha;
    isaac_sha1_init(&sha);
    int rc = 0;
    for (size_t i = 0; i < file->count && !rc; ++i) {
        const depot_chunk *chunk = &file->chunks[i];
        rc = download_chunk(c, chunk, buf, err);
        if (rc) break;
        if (fwrite(buf->raw, 1, chunk->size, out) != chunk->size) {
            rc = fail(err, "SD card write failed or storage is full");
            break;
        }
        isaac_sha1_update(&sha, buf->raw, chunk->size);
        *done += chunk->size;
        if (c->net->progress) c->net->progress(c->net->ctx, *done, total);
    }
    uint8_t digest[20];
    isaac_sha1_final(&sha, digest);
    /* Steam uses a zero hash for some empty regular files. */
    static const uint8_t empty_hash[20];
    int empty_sentinel = !file->size && !file->count &&
                         !memcmp(file->digest, empty_hash, sizeof empty_hash);
    if (!rc && file->has_digest && !empty_sentinel && memcmp(digest, file->digest, sizeof digest))
        rc = fail(err, "Workshop file failed SHA-1 verification");
    if (!rc && (fflush(out) || fsync(fileno(out)))) rc = fail(err, "Could not flush Workshop file to SD");
    if (fclose(out) && !rc) rc = fail(err, "Could not close Workshop file");
    return rc;
}

static int manifest_files(cdn *c, const depot_manifest *manifest, const char *stage,
                          download_buffers *buf, uint64_t *size, char err[WORKSHOP_ERR]) {
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, manifest->payload, manifest->len);
    unsigned count = 0;
    int rc, metadata = 0;
    uint64_t bytes = 0;
    while ((rc = isaac_pbr_next(&r, &f)) > 0) {
        if (cancelled(c->net)) return fail(err, "Cancelled");
        if (f.field != 1 || f.wire != 2) continue;
        if (++count > MAX_FILES) return fail(err, "Too many files in Workshop manifest");
        depot_file file;
        const char *error = parse_file(&f, c, manifest->encrypted, &file);
        if (error) {
            free(file.chunks);
            snprintf(err, WORKSHOP_ERR, "%s (file %u)", error, count);
            return -1;
        }
        if (!strcmp(file.path, "metadata.xml") && file.size > 0) metadata = 1;
        if (stage) rc = write_file(c, stage, &file, buf, &bytes, *size, err);
        else bytes += file.size;
        free(file.chunks);
        if (rc < 0) return -1;
    }
    if (rc || !metadata) return fail(err, "Workshop item has no root metadata.xml or has an invalid manifest");
    if (stage && bytes != *size) return fail(err, "Workshop installation size mismatch");
    *size = bytes;
    return 0;
}

int workshop_install(const workshop_net *net, const mod_store *store, const workshop_item *item,
                     char err[WORKSHOP_ERR]) {
    if (item->unavailable || !item->id || !item->manifest || item->has_legacy_url)
        return fail(err, "This item uses an unsupported or unavailable Workshop format");
    cdn c = {.net = net};
    int rc = -1, staged = 0;
    uint8_t *zip = NULL, *bytes = NULL;
    download_buffers buf = {0};
    mod_install install;
    uint64_t code = manifest_code(&c, item->manifest, err);
    if (!code || servers(&c, err)) goto end;

    zip = malloc(MANIFEST_ZIP_CAP);
    bytes = malloc(MANIFEST_CAP);
    if (!zip || !bytes) { fail(err, "Out of memory"); goto end; }
    char endpoint[128];
    snprintf(endpoint, sizeof endpoint, "/depot/%u/manifest/%llu/5/%llu", WORKSHOP_DEPOT,
             (unsigned long long)item->manifest, (unsigned long long)code);
    size_t n, len;
    if (cdn_get(&c, endpoint, zip, MANIFEST_ZIP_CAP, &n, err)) goto end;
    depot_manifest manifest;
    if (isaac_zip_unpack(zip, n, bytes, MANIFEST_CAP, &len) || manifest_parse(bytes, len, item->manifest, &manifest)) {
        fail(err, "Steam returned an invalid Workshop manifest");
        goto end;
    }
    free(zip);
    zip = NULL;
    if (cancelled(net) || net->key(net->ctx, WORKSHOP_APP, WORKSHOP_DEPOT, c.key, err)) goto end;
    uint64_t size = 0;
    if (manifest_files(&c, &manifest, NULL, NULL, &size, err) ||
        mod_install_begin(store, item->id, size, &install, err)) goto end;
    staged = 1;
    if (net->progress) net->progress(net->ctx, 0, size);
    buf.wire = malloc(CHUNK_CAP);
    buf.compressed = malloc(CHUNK_CAP);
    buf.raw = malloc(CHUNK_CAP);
    if (!buf.wire || !buf.compressed || !buf.raw) { fail(err, "Out of memory"); goto end; }
    if (manifest_files(&c, &manifest, install.stage, &buf, &size, err) || cancelled(net)) goto end;
    isaac_sha1 sha;
    uint8_t digest[20];
    char hex[41];
    isaac_sha1_init(&sha);
    isaac_sha1_update(&sha, bytes, len);
    isaac_sha1_final(&sha, digest);
    isaac_sha1_hex(digest, hex);
    rc = mod_install_commit(&install, item, size, hex, err);
end:
    if (staged && rc) mod_install_abort(&install);
    free(zip);
    free(bytes);
    free(buf.wire);
    free(buf.compressed);
    free(buf.raw);
    if (rc && cancelled(net)) fail(err, "Cancelled");
    return rc;
}

/* Preview downloads carry no session credentials. Decode only bounded still
   images and scale them into one small slot owned by the browser. */
void workshop_thumbnail_scale(workshop_thumb *thumb, const uint8_t *pixels, unsigned w, unsigned h,
                              unsigned channels) {
    thumb->width = WORKSHOP_THUMB_W;
    thumb->height = WORKSHOP_THUMB_W * h / w;
    if (thumb->height > WORKSHOP_THUMB_H) {
        thumb->height = WORKSHOP_THUMB_H;
        thumb->width = WORKSHOP_THUMB_H * w / h;
    }
    if (!thumb->width) thumb->width = 1;
    if (!thumb->height) thumb->height = 1;
    for (unsigned y = 0; y < thumb->height; ++y)
        for (unsigned x = 0; x < thumb->width; ++x) {
            const uint8_t *p = pixels + ((y * h / thumb->height) * w + x * w / thumb->width) * channels;
            thumb->pixels[y * thumb->width + x] = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)(channels == 4 ? p[3] : 255) << 24;
        }
}

#define IMAGE_DIM 2048u
#define IMAGE_CAP (2u << 20)

static int thumb_png(workshop_thumb *thumb, const uint8_t *bytes, size_t len) {
    png_image image = {.version = PNG_IMAGE_VERSION};
    if (!png_image_begin_read_from_memory(&image, bytes, len)) return -1;
    int rc = -1;
    if (image.width && image.height && image.width <= IMAGE_DIM && image.height <= IMAGE_DIM) {
        image.format = PNG_FORMAT_RGBA;
        uint8_t *pixels = malloc(PNG_IMAGE_SIZE(image));
        if (pixels && png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
            workshop_thumbnail_scale(thumb, pixels, image.width, image.height, 4);
            rc = 0;
        }
        free(pixels);
    }
    png_image_free(&image);
    return rc;
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    struct jpeg_decompress_struct jpeg;
    uint8_t *pixels;
} jpeg_preview;

static void jpeg_error(j_common_ptr jpeg) {
    jpeg_preview *state = (jpeg_preview *)jpeg->err;
    longjmp(state->jump, 1);
}

static int thumb_jpeg(workshop_thumb *thumb, const uint8_t *bytes, size_t len) {
    jpeg_preview *state = calloc(1, sizeof *state);
    if (!state) return -1;
    struct jpeg_decompress_struct *jpeg = &state->jpeg;
    jpeg->err = jpeg_std_error(&state->pub);
    state->pub.error_exit = jpeg_error;
    volatile int rc = -1;
    if (!setjmp(state->jump)) {
        jpeg_create_decompress(jpeg);
        jpeg_mem_src(jpeg, bytes, len);
        if (jpeg_read_header(jpeg, TRUE) == JPEG_HEADER_OK &&
            jpeg->image_width <= IMAGE_DIM && jpeg->image_height <= IMAGE_DIM) {
            jpeg->out_color_space = JCS_RGB;
            jpeg->scale_num = 1;
            jpeg->scale_denom = jpeg->image_width > 1024 ? 4 : jpeg->image_width > 512 ? 2 : 1;
            jpeg_start_decompress(jpeg);
            unsigned stride = jpeg->output_width * 3;
            state->pixels = malloc((size_t)stride * jpeg->output_height);
            if (state->pixels) {
                while (jpeg->output_scanline < jpeg->output_height) {
                    JSAMPROW row = state->pixels + jpeg->output_scanline * stride;
                    if (!jpeg_read_scanlines(jpeg, &row, 1)) break;
                }
                if (jpeg_finish_decompress(jpeg)) {
                    workshop_thumbnail_scale(thumb, state->pixels, jpeg->output_width, jpeg->output_height, 3);
                    rc = 0;
                }
            }
        }
    }
    jpeg_destroy_decompress(jpeg);
    free(state->pixels);
    free(state);
    return rc;
}

static uint8_t *image_fetch(const workshop_net *net, const char *url, size_t cap, int resize,
                            size_t *n, char err[WORKSHOP_ERR]) {
    const char *authority;
    if (!strncmp(url, "https://", 8)) authority = url + 8;
    else if (!strncmp(url, "http://", 7)) authority = url + 7;
    else return NULL;
    const char *path = strchr(authority, '/');
    char host[128];
    if (!path || path - authority >= (ptrdiff_t)sizeof host || !visible_url_part(path, "#\\")) return NULL;
    memcpy(host, authority, (size_t)(path - authority));
    host[path - authority] = 0;
    if (!steam_host(host, 1)) return NULL;
    char variant[1200];
    if (resize) {
        if (strncmp(path, "/ugc/", 5) || !strcmp(host, "i.imgur.com")) return NULL;
        const char *query = strchr(path, '?');
        int size = snprintf(variant, sizeof variant,
                            "%.*s?ima=fit&imcolor=%%23000000&imh=268&impolicy=Letterbox&imw=268&letterbox=true",
                            query ? (int)(query - path) : (int)strlen(path), path);
        if (size < 0 || (size_t)size >= sizeof variant) return NULL;
        path = variant;
        snprintf(host, sizeof host, "images.steamusercontent.com");
    }
    uint8_t *bytes = malloc(cap);
    if (!bytes) return NULL;
    int status = 0;
    if (!net->get(net->ctx, host, path, bytes, cap, n, &status, err) &&
        status == 200 && *n >= 8 && *n <= cap) return bytes;
    free(bytes);
    return NULL;
}

static int still_image(workshop_thumb *thumb, const uint8_t *bytes, size_t n) {
    if (!png_sig_cmp(bytes, 0, 8)) return thumb_png(thumb, bytes, n);
    if (bytes[0] == 0xff && bytes[1] == 0xd8) return thumb_jpeg(thumb, bytes, n);
    return -1;
}

void workshop_animation_clear(workshop_animation *animation) {
    for (unsigned i = 0; i < animation->count; ++i) free(animation->frames[i]);
    memset(animation, 0, sizeof *animation);
}

static int decode_image(const workshop_net *net, const char *url, workshop_animation *animation,
                        unsigned frames, int resize, char err[WORKSHOP_ERR]) {
    if (net->cancelled && net->cancelled(net->ctx)) return -1;
    size_t n = 0;
    uint8_t *bytes = image_fetch(net, url, IMAGE_CAP * 2u, resize, &n, err);
    if (!bytes) return -1;
    int rc = -1;
    if (!memcmp(bytes, "GIF8", 4)) rc = workshop_gif(bytes, n, animation, frames);
    else {
        workshop_thumb *image = malloc(sizeof *image);
        if (image && !still_image(image, bytes, n)) {
            animation->frames[0] = image;
            animation->count = 1;
            rc = 0;
        } else free(image);
    }
    free(bytes);
    if (rc) workshop_animation_clear(animation);
    return rc;
}

int workshop_preview(const workshop_net *net, const char *url, workshop_animation *animation,
                     char err[WORKSHOP_ERR]) {
    if (!decode_image(net, url, animation, WORKSHOP_MEDIA_FRAMES, 0, err)) return 0;
    return decode_image(net, url, animation, WORKSHOP_MEDIA_FRAMES, 1, err);
}

int workshop_illustration(const workshop_net *net, const char *url, workshop_animation *animation,
                          char err[WORKSHOP_ERR]) {
    return decode_image(net, url, animation, WORKSHOP_MEDIA_FRAMES, 0, err);
}
