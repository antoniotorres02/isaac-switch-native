/* A Steam CM session and the cloud calls the save sync needs.
 *
 * The Switch talks to Steam the way the Steam client does: a websocket to a CM
 * (Connection Manager) over TLS, a logon carrying the account's *refresh* token
 * (iss=steam; the access tokens derived from it are rejected by the CM), and
 * then Cloud.* service calls. File bodies move over plain HTTPS to Steam's S3
 * URLs, so nothing here proxies through a PC.
 *
 * On PC the build has its own Steam, so this transport reports itself
 * unavailable and the sync is a no-op.
 */
#ifndef ISAAC_NATIVE_STEAMCM_H
#define ISAAC_NATIVE_STEAMCM_H

#include <stddef.h>
#include <stdint.h>

/* One entry of Cloud.EnumerateUserFiles. */
typedef struct {
    char name[64];          /* e.g. rep+persistentgamedata1.dat */
    char sha_hex[41];       /* SHA-1 of the raw (unzipped) bytes */
    uint64_t timestamp;     /* server-side upload time, unix seconds */
    uint32_t size;          /* raw size */
} isaac_cloud_file;

/* What the sync engine needs from Steam, injected so the engine can be tested
 * on the host without a session. */
typedef struct isaac_steam_transport {
    void *state;
    int (*enumerate)(void *state, uint32_t appid, isaac_cloud_file *out, size_t max,
                     size_t *count, char *err, size_t errcap);
    int (*download)(void *state, uint32_t appid, const char *name, void *raw, size_t cap,
                    size_t *raw_len, char sha_hex[41], uint64_t *timestamp,
                    char *err, size_t errcap);
    /* 0 on success; a duplicate (Steam already has these exact bytes) is success. */
    int (*upload)(void *state, uint32_t appid, const char *name, const void *raw, size_t raw_len,
                  uint64_t *timestamp, char *err, size_t errcap);
    /* Does the account own this app (a game or DLC)? 1 yes, 0 no, -1 no answer.
     * Optional: without it the sync does not check. */
    int (*owns)(void *state, uint32_t appid, char *err, size_t errcap);
    void (*close)(void *state);
} isaac_steam_transport;

/* 1 when this build can talk to Steam at all. */
int isaac_steamcm_available(void);

/* Progress lines: CM list, TLS, logon, each cloud file moved. The sync passes
 * the same callback it uses for its own log, so a console run keeps them. */
void isaac_steamcm_set_log(void (*fn)(void *user, const char *line), void *user);

/* Opens a CM session: CM list, TLS, websocket, logon. The caller must have
 * initialised sockets (the Switch entry does it around pull/push). `steamid`
 * is the account id from the token file; it goes in the logon header the way
 * the Steam client sends it. */
int isaac_steamcm_open(uint64_t steamid, const char *token, int timeout_ms,
                       isaac_steam_transport *out, char *err, size_t errcap);

/* 1 when the last isaac_steamcm_open failed because Steam turned the token
 * down (revoked or expired), which another try will not change. */
int isaac_steamcm_token_refused(void);

/* Ends this token's session on Steam: logs on with it and revokes it, so the
 * device leaves the account's list. 0 revoked, 1 Steam had already refused it,
 * -1 Steam could not be told (see err). Sockets as for isaac_steamcm_open. */
int isaac_steamcm_revoke(uint64_t steamid, const char *token, int timeout_ms, char *err, size_t errcap);

/* Web API calls without a CM session, for the QR sign-in: open once around
 * them (sockets already up). isaac_steamcm_webapi is an isaac_webapi_fn. Like
 * the session, these share their buffers: one network user at a time. */
int isaac_steamcm_web_open(char *err, size_t errcap);
void isaac_steamcm_web_close(void);
int isaac_steamcm_webapi(void *user, const char *iface, const char *method,
                         const void *req, size_t req_len,
                         void *resp, size_t cap, size_t *resp_len, int *eresult,
                         char *err, size_t errcap);

/* Largest save the transport accepts, and the buffers it needs. */
#define ISAAC_STEAMCM_MAX_FILE (1u << 20)

#endif
