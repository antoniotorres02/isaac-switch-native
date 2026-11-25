/* The console half of the install (payload.py is the PC half).
 *
 * The payload is two NCAs complete except for header encryption, plus
 * install.txt. For each, this encrypts the 0xC00-byte header with the
 * console's header key (AES-XTS, Nintendo's big-endian tweak), streams the
 * NCA to the platform while hashing it, then builds the meta NCA (a PFS0
 * holding the cnmt with each content's SHA-256) and the content meta record
 * ncm stores for the title. Portable: the platform supplies the crypto and
 * where the bytes go (switch_backend.c on the console, the PC test writes
 * files and checks them with hactool). */
#ifndef ISAAC_NCA_INSTALL_H
#define ISAAC_NCA_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#define INST_MAX_CONTENTS 3   /* program, control, meta */

/* ncm's content types (NcmContentType), as the cnmt and the record use them */
enum { INST_TYPE_META = 0, INST_TYPE_PROGRAM = 1, INST_TYPE_CONTROL = 3 };

typedef struct {
    char file[64];
    uint8_t type;
    uint64_t size;
    uint8_t id[16];          /* the content id ncm files it under */
    uint8_t hash[32];        /* SHA-256 of the NCA as installed (header encrypted) */
} inst_content;

typedef struct {
    uint64_t title_id;
    uint32_t version;
    char display_version[16];
    char name[64];
    int count;               /* contents in install.txt; the meta NCA is added after them */
    inst_content c[INST_MAX_CONTENTS];
    uint64_t total;          /* bytes to stream, for progress */
} inst_manifest;

/* Where the NCAs go. Each returns 0 on success. */
typedef struct {
    void *user;
    int (*create)(void *user, const inst_content *c);
    int (*write)(void *user, const inst_content *c, uint64_t offset, const void *data, size_t len);
    int (*commit)(void *user, const inst_content *c);
    void (*progress)(void *user, const char *what, uint64_t done, uint64_t total);
} inst_io;

/* Platform crypto. */
void inst_xts_encrypt(const uint8_t key[32], uint8_t *buf, size_t len, uint64_t first_sector);
typedef struct inst_sha256 inst_sha256;
inst_sha256 *inst_sha256_new(void);
void inst_sha256_update(inst_sha256 *s, const void *data, size_t len);
void inst_sha256_final(inst_sha256 *s, uint8_t out[32]);   /* frees s */

/* install.txt in `dir`; fills `m`. On failure, `err` says why. */
int inst_read_manifest(const char *dir, inst_manifest *m, char *err, size_t errcap);

/* Streams the payload's NCAs through `io`, then the meta NCA (appended to
 * m->c), and writes the record for ncm's content meta database into
 * `record` (*record_len bytes; 0x8 + 0x10 + 0x18 per content). */
int inst_install(const char *dir, inst_manifest *m, const uint8_t header_key[32], const inst_io *io,
                 uint8_t *record, size_t record_cap, size_t *record_len, char *err, size_t errcap);

#endif
