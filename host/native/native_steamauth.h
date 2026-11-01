/* Signing in to Steam from the console, the way the Steam Deck does: Steam
 * hands out a challenge URL, the screen shows it as a QR code, the account's
 * Steam app approves it, and the next poll answers with the refresh token the
 * save sync logs on with.
 *
 * Two IAuthenticationService calls of the Web API, neither needs an account.
 * The HTTP goes through a callback so the host tests can stand in for Steam;
 * on the console it is isaac_steamcm_webapi. */
#ifndef ISAAC_NATIVE_STEAMAUTH_H
#define ISAAC_NATIVE_STEAMAUTH_H

#include <stddef.h>
#include <stdint.h>

/* 0 when Steam answered at all, with `eresult` from its X-eresult header. */
typedef int (*isaac_webapi_fn)(void *user, const char *iface, const char *method,
                               const void *req, size_t req_len,
                               void *resp, size_t cap, size_t *resp_len, int *eresult,
                               char *err, size_t errcap);

typedef struct {
    uint64_t client_id;
    uint8_t request_id[64];
    size_t request_id_len;
    char url[256];              /* what the QR code says */
    int interval_ms;            /* how often Steam wants to be asked */
} isaac_steamauth_session;

/* The longest token the token file can hold (isaac_steamsync_read_token reads
 * lines of 1024 bytes), plus its terminator. */
#define ISAAC_STEAMAUTH_TOKEN_MAX 1000

typedef struct {
    uint64_t steamid;
    char token[ISAAC_STEAMAUTH_TOKEN_MAX];
    char account[64];
} isaac_steamauth_result;

enum {
    ISAAC_STEAMAUTH_ERROR = -1, /* see err */
    ISAAC_STEAMAUTH_PENDING,    /* not approved yet */
    ISAAC_STEAMAUTH_NEW_CODE,   /* session->url changed: redraw the code */
    ISAAC_STEAMAUTH_EXPIRED,    /* this session is over: begin another */
    ISAAC_STEAMAUTH_APPROVED,   /* `result` is filled */
};

/* 0 when the session is open. `device` is the name the Steam app shows, and
 * later Steam's list of devices. */
int isaac_steamauth_begin(isaac_webapi_fn fn, void *user, const char *device,
                          isaac_steamauth_session *session, char *err, size_t errcap);

int isaac_steamauth_poll(isaac_webapi_fn fn, void *user, isaac_steamauth_session *session,
                         isaac_steamauth_result *result, char *err, size_t errcap);

/* A refresh token's subject, the account's steamid: 0 when it has none. */
uint64_t isaac_steamauth_token_steamid(const char *token);

/* The form body for a Web API call: input_protobuf_encoded=<base64, escaped>.
 * The length written, 0 when it does not fit. */
size_t isaac_steamauth_form(const void *req, size_t len, char *out, size_t cap);

#endif
