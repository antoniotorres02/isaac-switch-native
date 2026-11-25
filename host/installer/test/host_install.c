/* PC driver for nca_install.c (tools/test_switch_installer.py builds and runs
 * it): OpenSSL for the crypto, files for ncm. Writes <out>/<content id>.nca
 * for each content and <out>/record.bin (the content meta record), so hactool
 * can check what the console would install.
 *
 *     host_install <payload dir> <out dir> <header key, 64 hex>
 */
#include "../source/nca_install.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void inst_xts_encrypt(const uint8_t key[32], uint8_t *buf, size_t len, uint64_t sector) {
    for (size_t off = 0; off < len; off += 0x200, ++sector) {
        uint8_t tweak[16] = {0};
        for (int i = 0; i < 8; ++i) tweak[15 - i] = (uint8_t)(sector >> (8 * i));   /* big endian */
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int n = 0;
        if (!EVP_EncryptInit_ex(ctx, EVP_aes_128_xts(), NULL, key, tweak) ||
            !EVP_EncryptUpdate(ctx, buf + off, &n, buf + off, 0x200) || n != 0x200) {
            fprintf(stderr, "xts failed\n");
            exit(1);
        }
        EVP_CIPHER_CTX_free(ctx);
    }
}

struct inst_sha256 { EVP_MD_CTX *ctx; };
inst_sha256 *inst_sha256_new(void) {
    inst_sha256 *s = malloc(sizeof *s);
    s->ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(s->ctx, EVP_sha256(), NULL);
    return s;
}
void inst_sha256_update(inst_sha256 *s, const void *data, size_t len) { EVP_DigestUpdate(s->ctx, data, len); }
void inst_sha256_final(inst_sha256 *s, uint8_t out[32]) {
    unsigned n = 32;
    EVP_DigestFinal_ex(s->ctx, out, &n);
    EVP_MD_CTX_free(s->ctx);
    free(s);
}

static const char *g_out;
static FILE *g_file;
static uint64_t g_written;

static void id_path(const inst_content *c, char *path, size_t cap) {
    int n = snprintf(path, cap, "%s/", g_out);
    for (int i = 0; i < 16; ++i) n += snprintf(path + n, cap - n, "%02x", c->id[i]);
    snprintf(path + n, cap - n, ".nca");
}
static int create(void *u, const inst_content *c) {
    (void)u;
    char path[600];
    id_path(c, path, sizeof path);
    g_file = fopen(path, "wb");
    g_written = 0;
    return g_file ? 0 : -1;
}
static int write_(void *u, const inst_content *c, uint64_t off, const void *data, size_t len) {
    (void)u; (void)c;
    if (off != g_written) return -1;   /* the console writes in order too */
    g_written += len;
    return fwrite(data, 1, len, g_file) == len ? 0 : -1;
}
static int commit(void *u, const inst_content *c) {
    (void)u;
    int ok = fclose(g_file) == 0 && g_written == c->size;
    g_file = NULL;
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 4 || strlen(argv[3]) != 64) {
        fprintf(stderr, "usage: host_install <payload dir> <out dir> <header key, 64 hex>\n");
        return 2;
    }
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) sscanf(argv[3] + 2 * i, "%2hhx", &key[i]);
    g_out = argv[2];
    char err[256];
    inst_manifest m;
    if (inst_read_manifest(argv[1], &m, err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    inst_io io = { NULL, create, write_, commit, NULL };
    uint8_t record[256];
    size_t record_len = 0;
    if (inst_install(argv[1], &m, key, &io, record, sizeof record, &record_len, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/record.bin", g_out);
    FILE *f = fopen(path, "wb");
    fwrite(record, 1, record_len, f);
    fclose(f);
    for (int i = 0; i <= m.count; ++i) {
        printf("content %d type %u size %llu id ", i, m.c[i].type, (unsigned long long)m.c[i].size);
        for (int b = 0; b < 16; ++b) printf("%02x", m.c[i].id[b]);
        printf(" sha256 ");
        for (int b = 0; b < 32; ++b) printf("%02x", m.c[i].hash[b]);
        printf("\n");
    }
    return 0;
}
