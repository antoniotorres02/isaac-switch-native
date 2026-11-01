/* The little bit of protobuf the Steam client protocol needs, plus its framing.
 *
 * Everything here is written by hand on purpose: the port only speaks a handful
 * of messages, and pulling in a protobuf runtime (with its .proto compiler) for
 * six fields would be far more machinery than the protocol deserves. Field
 * numbers come from SteamDatabase/Protobufs (MIT), which mirrors what the Steam
 * client sends:
 *
 *   CMsgProtoBufHeader  steamid(1,f64) client_sessionid(2,v) jobid_source(10,f64)
 *                       jobid_target(11,f64) target_job_name(12,s) eresult(13,v)
 *   CMsgClientLogon     protocol_version(1) cell_id(3) client_package_version(5)
 *                       client_language(6) client_os_type(7) should_remember_password(8)
 *                       client_supplied_steam_id(22,f64) machine_id(30,b) chat_mode(33)
 *                       machine_name(96) machine_name_userchosen(97)
 *                       client_instance_id(100,f64) supports_rate_limit_response(102)
 *                       access_token(108)   <- the *refresh* token, iss=steam
 *   CMsgClientLogOnResponse  eresult(1) heartbeat_seconds(3) cell_id(7)
 *   CMsgMulti           size_unzipped(1) message_body(2)   [gzip when 1 is set]
 *
 * A websocket payload is [emsg|0x80000000][header_len][header][body] with no
 * body length (SteamKit2's MsgHdrProtoBuf + ClientMsgProtobuf.Serialize);
 * struct messages instead carry a fixed 20-byte header (emsg + two job ids).
 */
#ifndef ISAAC_NATIVE_STEAMPROTO_H
#define ISAAC_NATIVE_STEAMPROTO_H

#include <stddef.h>
#include <stdint.h>

/* ---- writer --------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int overflow;
} isaac_pbw;

void isaac_pbw_init(isaac_pbw *w, void *buf, size_t cap);
void isaac_pbw_varint(isaac_pbw *w, unsigned field, uint64_t value);
void isaac_pbw_fixed64(isaac_pbw *w, unsigned field, uint64_t value);
void isaac_pbw_fixed32(isaac_pbw *w, unsigned field, uint32_t value);
void isaac_pbw_bytes(isaac_pbw *w, unsigned field, const void *data, size_t len);
void isaac_pbw_str(isaac_pbw *w, unsigned field, const char *s);

/* ---- reader --------------------------------------------------------------- */

typedef struct {
    unsigned field;
    unsigned wire;
    uint64_t value;         /* varint and fixed32/64 fields */
    const uint8_t *bytes;   /* length-delimited fields */
    size_t len;
} isaac_pbfield;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} isaac_pbr;

void isaac_pbr_init(isaac_pbr *r, const void *data, size_t len);
/* 1: a field was read, 0: end of message, -1: malformed. */
int isaac_pbr_next(isaac_pbr *r, isaac_pbfield *f);
/* Last value of `field` as a varint, or `fallback` when absent. */
uint64_t isaac_pbr_uint(const void *data, size_t len, unsigned field, uint64_t fallback);
/* Length-delimited `field` as text; returns length or -1. */
int isaac_pbr_str(const void *data, size_t len, unsigned field, char *out, size_t cap);

/* ---- messages ------------------------------------------------------------- */

/* EMsg values the client side of the cloud needs. */
#define ISAAC_EMSG_MULTI                         1u
#define ISAAC_EMSG_CLIENT_HELLO                  9805u
#define ISAAC_EMSG_CLIENT_LOGON                  5514u
#define ISAAC_EMSG_CLIENT_LOGON_RESPONSE         751u
#define ISAAC_EMSG_CLIENT_LOGGED_OFF             757u
#define ISAAC_EMSG_CLIENT_HEARTBEAT              703u
#define ISAAC_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT 151u
#define ISAAC_EMSG_SERVICE_METHOD_RESPONSE       147u
#define ISAAC_EMSG_SERVICE_METHOD                146u
#define ISAAC_EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET 857u
#define ISAAC_EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET_RESPONSE 858u

typedef struct {
    unsigned emsg;
    const uint8_t *hdr;
    size_t hlen;
    const uint8_t *body;
    size_t blen;
} isaac_cmmsg;

typedef void (*isaac_cm_msg_fn)(void *user, const isaac_cmmsg *msg);

/* Split one websocket payload into its messages (unwrapping CMsgMulti, gunzipping
 * when the multi says so). `scratch` holds the inflated bytes. */
int isaac_cm_split(const void *payload, size_t len, void *scratch, size_t scratch_cap,
                   isaac_cm_msg_fn fn, void *user);

/* [emsg|0x80000000][header_len][header][body]; 0 when it does not fit. */
size_t isaac_cm_frame(void *out, size_t cap, unsigned emsg,
                      const void *hdr, size_t hlen, const void *body, size_t blen);

#endif
