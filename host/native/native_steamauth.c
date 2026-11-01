/* Steam's QR sign-in. See native_steamauth.h; the field numbers come from
 * SteamDatabase/Protobufs (steammessages_auth.steamclient.proto) and
 * steam_cloud_cm.py is the same flow in Python. */
#include "native_steamauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_steamproto.h"

#define IFACE "IAuthenticationService"
#define ERESULT_OK 1
#define ERESULT_EXPIRED 27
#define PLATFORM_STEAM_CLIENT 1     /* the only kind of token the CM takes */

static uint8_t g_resp[4096];

static int call(isaac_webapi_fn fn, void *user, const char *method, const void *req, size_t req_len,
                size_t *resp_len, int *eresult, char *err, size_t errcap) {
    *eresult = 0;
    *resp_len = 0;
    return fn(user, IFACE, method, req, req_len, g_resp, sizeof g_resp, resp_len, eresult, err, errcap);
}

static void copy_str(const isaac_pbfield *f, char *out, size_t cap) {
    size_t n = f->len < cap - 1 ? f->len : cap - 1;
    memcpy(out, f->bytes, n);
    out[n] = 0;
}

int isaac_steamauth_begin(isaac_webapi_fn fn, void *user, const char *device,
                          isaac_steamauth_session *session, char *err, size_t errcap) {
    uint8_t details[128], req[256];
    isaac_pbw w;
    isaac_pbw_init(&w, details, sizeof details);
    isaac_pbw_str(&w, 1, device);                       /* device_friendly_name */
    isaac_pbw_varint(&w, 2, PLATFORM_STEAM_CLIENT);     /* platform_type */
    size_t details_len = w.len;
    int too_long = w.overflow;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_str(&w, 1, device);
    isaac_pbw_varint(&w, 2, PLATFORM_STEAM_CLIENT);
    isaac_pbw_bytes(&w, 3, details, details_len);       /* device_details */
    if (too_long || w.overflow) {
        snprintf(err, errcap, "device name too long");
        return -1;
    }
    size_t len;
    int eresult;
    if (call(fn, user, "BeginAuthSessionViaQR", req, w.len, &len, &eresult, err, errcap) != 0) return -1;
    if (eresult != ERESULT_OK) {
        snprintf(err, errcap, "Steam refused the QR session (eresult %d)", eresult);
        return -1;
    }
    memset(session, 0, sizeof *session);
    session->interval_ms = 5000;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, g_resp, len);
    while (isaac_pbr_next(&r, &f) == 1) {
        if (f.field == 1 && f.wire == 0) {
            session->client_id = f.value;
        } else if (f.field == 2 && f.wire == 2) {
            copy_str(&f, session->url, sizeof session->url);
        } else if (f.field == 3 && f.wire == 2 && f.len <= sizeof session->request_id) {
            memcpy(session->request_id, f.bytes, f.len);
            session->request_id_len = f.len;
        } else if (f.field == 4 && f.wire == 5) {
            uint32_t raw = (uint32_t)f.value;
            float seconds;
            memcpy(&seconds, &raw, sizeof seconds);
            if (seconds >= 1.0f && seconds <= 30.0f) session->interval_ms = (int)(seconds * 1000.0f);
        }
    }
    if (!session->client_id || !session->url[0] || !session->request_id_len) {
        snprintf(err, errcap, "Steam's QR session came back incomplete");
        return -1;
    }
    return 0;
}

int isaac_steamauth_poll(isaac_webapi_fn fn, void *user, isaac_steamauth_session *session,
                         isaac_steamauth_result *result, char *err, size_t errcap) {
    uint8_t req[128];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, session->client_id);
    isaac_pbw_bytes(&w, 2, session->request_id, session->request_id_len);
    if (w.overflow) {
        snprintf(err, errcap, "poll request too long");
        return ISAAC_STEAMAUTH_ERROR;
    }
    size_t len;
    int eresult;
    if (call(fn, user, "PollAuthSessionStatus", req, w.len, &len, &eresult, err, errcap) != 0)
        return ISAAC_STEAMAUTH_ERROR;
    if (eresult == ERESULT_EXPIRED) return ISAAC_STEAMAUTH_EXPIRED;
    if (eresult != ERESULT_OK) {
        snprintf(err, errcap, "Steam refused the sign-in (eresult %d)", eresult);
        return ISAAC_STEAMAUTH_ERROR;
    }
    memset(result, 0, sizeof *result);
    int state = ISAAC_STEAMAUTH_PENDING;
    size_t token_len = 0;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, g_resp, len);
    while (isaac_pbr_next(&r, &f) == 1) {
        if (f.field == 1 && f.wire == 0 && f.value) {
            session->client_id = f.value;               /* the next polls ask for this one */
        } else if (f.field == 2 && f.wire == 2 && f.len) {
            char url[sizeof session->url];
            copy_str(&f, url, sizeof url);
            if (strcmp(url, session->url)) {
                memcpy(session->url, url, sizeof url);
                state = ISAAC_STEAMAUTH_NEW_CODE;
            }
        } else if (f.field == 3 && f.wire == 2) {
            token_len = f.len;
            copy_str(&f, result->token, sizeof result->token);
        } else if (f.field == 6 && f.wire == 2) {
            copy_str(&f, result->account, sizeof result->account);
        }
    }
    if (!token_len) return state;
    if (token_len >= sizeof result->token) {
        snprintf(err, errcap, "Steam's token is too long for the token file (%zu bytes)", token_len);
        return ISAAC_STEAMAUTH_ERROR;
    }
    result->steamid = isaac_steamauth_token_steamid(result->token);
    if (!result->steamid) {
        snprintf(err, errcap, "Steam's token names no account");
        return ISAAC_STEAMAUTH_ERROR;
    }
    return ISAAC_STEAMAUTH_APPROVED;
}

/* ---- the token ---------------------------------------------------------------- */

static int b64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

uint64_t isaac_steamauth_token_steamid(const char *token) {
    const char *start = strchr(token, '.');
    if (!start) return 0;
    ++start;
    const char *end = strchr(start, '.');
    if (!end) return 0;
    char json[ISAAC_STEAMAUTH_TOKEN_MAX];
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (const char *p = start; p < end && n + 1 < sizeof json; ++p) {
        int v = b64url_value(*p);
        if (v < 0) return 0;
        acc = (acc << 6 | (unsigned)v) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            json[n++] = (char)((acc >> bits) & 0xff);
        }
    }
    json[n] = 0;
    const char *sub = strstr(json, "\"sub\"");
    if (!sub) return 0;
    sub += 5;
    while (*sub == ' ' || *sub == ':') ++sub;
    if (*sub != '"') return 0;
    return strtoull(sub + 1, NULL, 10);
}

size_t isaac_steamauth_form(const void *req, size_t len, char *out, size_t cap) {
    static const char TBL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char PREFIX[] = "input_protobuf_encoded=";
    const uint8_t *in = req;
    size_t o = sizeof PREFIX - 1;
    /* Every base64 character may take three bytes once escaped. */
    if (cap < o + (len + 2) / 3 * 4 * 3 + 1) return 0;
    memcpy(out, PREFIX, o);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        char c[4] = { TBL[(v >> 18) & 63], TBL[(v >> 12) & 63],
                      i + 1 < len ? TBL[(v >> 6) & 63] : '=', i + 2 < len ? TBL[v & 63] : '=' };
        for (int k = 0; k < 4; ++k) {
            if (c[k] == '+' || c[k] == '/' || c[k] == '=') {
                static const char HEX[] = "0123456789ABCDEF";
                out[o++] = '%';
                out[o++] = HEX[(unsigned char)c[k] >> 4];
                out[o++] = HEX[(unsigned char)c[k] & 15];
            } else {
                out[o++] = c[k];
            }
        }
    }
    out[o] = 0;
    return o;
}
