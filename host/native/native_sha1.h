/* SHA-1, for the file_sha field of Steam's cloud service.
 *
 * Steam identifies a cloud file by the SHA-1 of its *raw* (unzipped) bytes:
 * Cloud.ClientFileDownload returns it as sha_file and
 * Cloud.BeginHTTPUpload takes it as a hex string. Nothing else in the port
 * needs SHA-1, so this is a small self-contained implementation (FIPS 180-4).
 */
#ifndef ISAAC_NATIVE_SHA1_H
#define ISAAC_NATIVE_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_SHA1_DIGEST 20

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} isaac_sha1;

void isaac_sha1_init(isaac_sha1 *s);
void isaac_sha1_update(isaac_sha1 *s, const void *data, size_t len);
void isaac_sha1_final(isaac_sha1 *s, uint8_t out[ISAAC_SHA1_DIGEST]);
void isaac_sha1_buf(const void *data, size_t len, uint8_t out[ISAAC_SHA1_DIGEST]);

/* Lowercase hex, 40 characters plus the terminator. */
void isaac_sha1_hex(const uint8_t digest[ISAAC_SHA1_DIGEST], char out[41]);
int isaac_sha1_from_hex(const char *hex, uint8_t out[ISAAC_SHA1_DIGEST]);

#endif
