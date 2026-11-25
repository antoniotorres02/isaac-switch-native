/* nca_install.c's crypto on the console, and the header key.
 *
 * The header key is derived the way FS derives it at boot
 * (Atmosphère, libstratosphere/source/fssystem/fssystem_crypto_configuration.cpp,
 * SetUpKekAccessKeys and GenerateNcaKey): spl seals a kek from a public
 * source, then unwraps the two halves of the encrypted header key with it.
 * The sources are the production ones from
 * libstratosphere/source/fssrv/fssrv_nca_crypto_configuration.cpp. Nothing
 * secret is in this file; the console holds what turns them into the key. */
#include "nca_install.h"

#include <stdlib.h>
#include <switch.h>

static const u8 HEADER_KEK_SOURCE[16] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A,
};
static const u8 ENCRYPTED_HEADER_KEY[2][16] = {
    { 0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0 },
    { 0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2 },
};

Result inst_switch_header_key(u8 out[32]) {
    Result rc = splCryptoInitialize();
    if (R_FAILED(rc)) return rc;
    u8 kek[16];
    rc = splCryptoGenerateAesKek(HEADER_KEK_SOURCE, 0, 0, kek);
    if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, ENCRYPTED_HEADER_KEY[0], out);
    if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, ENCRYPTED_HEADER_KEY[1], out + 16);
    splCryptoExit();
    return rc;
}

void inst_xts_encrypt(const uint8_t key[32], uint8_t *buf, size_t len, uint64_t sector) {
    Aes128XtsContext ctx;
    aes128XtsContextCreate(&ctx, key, key + 16, true);
    for (size_t off = 0; off < len; off += 0x200, ++sector) {
        aes128XtsContextResetSector(&ctx, sector, true);
        aes128XtsEncrypt(&ctx, buf + off, buf + off, 0x200);
    }
}

struct inst_sha256 { Sha256Context ctx; };
inst_sha256 *inst_sha256_new(void) {
    inst_sha256 *s = malloc(sizeof *s);
    if (s) sha256ContextCreate(&s->ctx);
    return s;
}
void inst_sha256_update(inst_sha256 *s, const void *data, size_t len) { sha256ContextUpdate(&s->ctx, data, len); }
void inst_sha256_final(inst_sha256 *s, uint8_t out[32]) {
    sha256ContextGetHash(&s->ctx, out);
    free(s);
}
