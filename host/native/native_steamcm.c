/* Steam CM session + cloud calls over the console's own TLS stack.
 *
 * See native_steamcm.h for the shape of the protocol. The interesting parts:
 *
 *  - the CM accepts only a *refresh* token in CMsgClientLogon.access_token
 *    (iss=steam); the access tokens derived from it are for the Web API and
 *    the CM answers AccessDenied for them;
 *  - a websocket payload is [emsg|0x80000000][hlen][header][body] and replies
 *    often arrive bundled in a gzipped CMsgMulti, which native_steamproto
 *    unwraps;
 *  - a cloud body is a ZIP with one entry "z" when Steam compressed it, and
 *    the file itself otherwise; Steam identifies it by the SHA-1 of the *raw*
 *    bytes. Cloud.BeginHTTPUpload has no raw-size field, so whatever the PUT
 *    carries *is* the file for every other client: uploads send the save
 *    as-is (a ZIP sent there reached the PC game as a ZIP: "corrupt save");
 *  - Steam deduplicates by digest: uploading bytes it already has answers
 *    eresult 29 (DuplicateRequest), which the sync treats as "already there".
 */
#include "native_steamcm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "native_sha1.h"
#include "native_steamauth.h"
#include "native_steamproto.h"
#include "native_zip.h"

int isaac_steamcm_available(void) {
#ifdef __SWITCH__
    return 1;
#else
    return 0;
#endif
}

/* Progress the caller can show and log: one line per step that takes time or
 * can fail (CM list, TLS, logon, each file that moves). On a console this is
 * the only account of a sync that could not finish. */
static void (*g_log)(void *user, const char *line);
static void *g_log_user;

void isaac_steamcm_set_log(void (*fn)(void *user, const char *line), void *user) {
    g_log = fn;
    g_log_user = user;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2), unused));
static void say(const char *fmt, ...) {
    if (!g_log) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_log(g_log_user, line);
}

#ifdef __SWITCH__

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <switch.h>

#define WS_RECV_MAX  (64u << 10)
#define GUNZIP_MAX   (256u << 10)
#define SEND_MAX     (64u << 10)
#define REPLY_MAX    (64u << 10)
#define HTTP_HDR_MAX (8u << 10)

#define ERESULT_OK        1
#define ERESULT_DUPLICATE 29
#define ERESULT_INVALID_PASSWORD 5
#define ERESULT_ACCESS_DENIED 15
#define ERESULT_EXPIRED 27

#define REVOKE_PERMANENT 1     /* EAuthTokenRevokeAction: the device goes from the account's list */

static uint8_t g_ws_buf[WS_RECV_MAX];
static uint8_t g_gunzip_buf[GUNZIP_MAX];
static uint8_t g_send_buf[SEND_MAX];
static uint8_t g_hdr_buf[512];
static uint8_t g_msg_buf[4096];
static uint8_t g_reply_buf[REPLY_MAX];
static uint8_t g_http_hdr[HTTP_HDR_MAX];
static uint8_t g_zip_buf[ISAAC_STEAMCM_MAX_FILE + 4096];

static void set_err(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- TLS through libnx's ssl service ------------------------------------- */

/* `fd` is our socket; `ssl_fd` the descriptor the ssl service hands back for
 * it, which has to be closed before the connection is. */
typedef struct {
    int fd, ssl_fd;
    SslContext ctx;
    SslConnection conn;
    int have_ctx, have_conn;
} isaac_tls;

static void tls_init(isaac_tls *t) {
    memset(t, 0, sizeof *t);
    t->fd = -1;
    t->ssl_fd = -1;
}

static void tls_close(isaac_tls *t) {
    if (t->ssl_fd >= 0) {
        close(t->ssl_fd);
        t->ssl_fd = -1;
    }
    if (t->have_conn) {
        sslConnectionClose(&t->conn);
        t->have_conn = 0;
    }
    if (t->have_ctx) {
        sslContextClose(&t->ctx);
        t->have_ctx = 0;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

static int tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errcap) {
    char service[8];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0 || !res) {
        set_err(err, errcap, "cannot resolve %s (%d)", host, rc);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            if (poll(&pfd, 1, timeout_ms) == 1) {
                int soerr = 0;
                socklen_t len = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                rc = soerr ? -1 : 0;
            } else {
                rc = -1;
            }
        }
        fcntl(fd, F_SETFL, flags);
        if (rc == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) set_err(err, errcap, "cannot connect %s:%d", host, port);
    return fd;
}

static int tls_open(isaac_tls *t, const char *host, int port, int timeout_ms, char *err, size_t errcap) {
    tls_init(t);
    t->fd = tcp_connect(host, port, timeout_ms, err, errcap);
    if (t->fd < 0) return -1;
    if (R_FAILED(sslCreateContext(&t->ctx, SslVersion_Auto))) {
        set_err(err, errcap, "sslCreateContext failed");
        tls_close(t);
        return -1;
    }
    t->have_ctx = 1;
    if (R_FAILED(sslContextCreateConnection(&t->ctx, &t->conn))) {
        set_err(err, errcap, "sslContextCreateConnection failed");
        tls_close(t);
        return -1;
    }
    t->have_conn = 1;
    /* The ssl service takes the socket by its BSD descriptor, not newlib's:
     * the libnx wrapper translates it (the raw call failed on hardware), and
     * ENOENT only means the service handed no descriptor back. */
    errno = 0;
    t->ssl_fd = socketSslConnectionSetSocketDescriptor(&t->conn, t->fd);
    if (t->ssl_fd < 0 && errno != ENOENT) {
        set_err(err, errcap, "TLS socket setup failed (errno %d)", errno);
        tls_close(t);
        return -1;
    }
    if (R_FAILED(sslConnectionSetHostName(&t->conn, host, (u32)strlen(host) + 1))) {
        set_err(err, errcap, "sslConnectionSetHostName failed");
        tls_close(t);
        return -1;
    }
    /* PeerCa | HostName: the console's own root store decides, and the name has
     * to match, so a hijacked DNS answer is not silently accepted. */
    sslConnectionSetVerifyOption(&t->conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName);
    u32 cert_size = 0, total_certs = 0;
    Result hs = sslConnectionDoHandshake(&t->conn, &cert_size, &total_certs, NULL, 0);
    if (R_FAILED(hs)) {
        set_err(err, errcap, "TLS handshake with %s failed (0x%x)", host, (unsigned)hs);
        tls_close(t);
        return -1;
    }
    return 0;
}

static int tls_write_all(isaac_tls *t, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        u32 chunk = len > 16384 ? 16384 : (u32)len, wrote = 0;
        if (R_FAILED(sslConnectionWrite(&t->conn, p, chunk, &wrote)) || !wrote) return -1;
        p += wrote;
        len -= wrote;
    }
    return 0;
}

/* >0 bytes read, 0 on a clean close, -1 on error/timeout. */
static int tls_read(isaac_tls *t, void *buf, size_t cap) {
    u32 got = 0;
    if (R_FAILED(sslConnectionRead(&t->conn, buf, (u32)cap, &got))) return -1;
    return (int)got;
}

/* ---- WebSocket (RFC 6455, binary frames, client side) --------------------- */

static void b64(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}

static int ws_open(isaac_tls *t, const char *host, const char *path, int port,
                   int timeout_ms, char *err, size_t errcap) {
    if (tls_open(t, host, port, timeout_ms, err, errcap) != 0) return -1;
    uint8_t key[16];
    randomGet(key, sizeof key);
    char key_b64[32];
    b64(key, sizeof key, key_b64);
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "User-Agent: Valve/Steam HTTP Client 1.0\r\n"
                     "\r\n", path, host, port, key_b64);
    if (n <= 0 || (size_t)n >= sizeof req || tls_write_all(t, req, (size_t)n) != 0) {
        set_err(err, errcap, "websocket request failed");
        return -1;
    }
    char head[1024];
    size_t used = 0;
    while (used < sizeof head - 1) {
        int got = tls_read(t, head + used, sizeof head - 1 - used);
        if (got <= 0) {
            set_err(err, errcap, "websocket handshake closed");
            return -1;
        }
        used += (size_t)got;
        head[used] = 0;
        if (strstr(head, "\r\n\r\n")) break;
    }
    if (!strstr(head, " 101 ")) {
        char line[80];
        size_t n = strcspn(head, "\r");
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, head, n);
        line[n] = 0;
        set_err(err, errcap, "websocket handshake: %s", line);
        return -1;
    }
    return 0;
}

static int ws_send(isaac_tls *t, const void *payload, size_t len) {
    uint8_t hdr[14];
    size_t hlen = 0;
    uint8_t mask[4];
    randomGet(mask, sizeof mask);
    hdr[0] = 0x82;                              /* FIN + binary */
    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; ++i) hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hlen = 10;
    }
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;
    if (tls_write_all(t, hdr, hlen) != 0) return -1;
    const uint8_t *p = payload;
    uint8_t chunk[4096];
    while (len) {
        size_t take = len < sizeof chunk ? len : sizeof chunk;
        for (size_t i = 0; i < take; ++i) chunk[i] = (uint8_t)(p[i] ^ mask[i & 3]);
        if (tls_write_all(t, chunk, take) != 0) return -1;
        p += take;
        len -= take;
    }
    return 0;
}

/* One message into `buf`. -1 error, -2 message larger than the buffer. */
static int ws_recv(isaac_tls *t, void *buf, size_t cap) {
    uint8_t h[2];
    for (;;) {
        size_t got = 0;
        while (got < 2) {
            int n = tls_read(t, h + got, 2 - got);
            if (n <= 0) return -1;
            got += (size_t)n;
        }
        int opcode = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            got = 0;
            while (got < 2) {
                int n = tls_read(t, e + got, 2 - got);
                if (n <= 0) return -1;
                got += (size_t)n;
            }
            len = (uint64_t)e[0] << 8 | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            got = 0;
            while (got < 8) {
                int n = tls_read(t, e + got, 8 - got);
                if (n <= 0) return -1;
                got += (size_t)n;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) len = len << 8 | e[i];
        }
        uint8_t mask[4] = { 0, 0, 0, 0 };
        int masked = h[1] & 0x80;
        if (masked) {
            got = 0;
            while (got < 4) {
                int n = tls_read(t, mask + got, 4 - got);
                if (n <= 0) return -1;
                got += (size_t)n;
            }
        }
        if (opcode == 0x8) return -1;           /* close */
        if (opcode == 0x9) {                    /* ping: drain, answer, keep reading */
            uint8_t sink[128];
            uint64_t left = len;
            while (left) {
                size_t take = left < sizeof sink ? (size_t)left : sizeof sink;
                if (tls_read(t, sink, take) <= 0) return -1;
                left -= take;
            }
            uint8_t pong[6] = { 0x8a, 0x80, 0, 0, 0, 0 };
            randomGet(pong + 2, 4);
            if (tls_write_all(t, pong, sizeof pong) != 0) return -1;
            continue;
        }
        if (opcode != 0x1 && opcode != 0x2) {   /* pong/continuation: skip */
            uint8_t sink[128];
            uint64_t left = len;
            while (left) {
                size_t take = left < sizeof sink ? (size_t)left : sizeof sink;
                if (tls_read(t, sink, take) <= 0) return -1;
                left -= take;
            }
            continue;
        }
        if (len > cap) {                        /* too big for us: discard */
            uint8_t sink[512];
            uint64_t left = len;
            while (left) {
                size_t take = left < sizeof sink ? (size_t)left : sizeof sink;
                if (tls_read(t, sink, take) <= 0) return -1;
                left -= take;
            }
            return -2;
        }
        size_t off = 0;
        while (off < len) {
            int n = tls_read(t, (uint8_t *)buf + off, (size_t)len - off);
            if (n <= 0) return -1;
            off += (size_t)n;
        }
        if (masked)
            for (size_t i = 0; i < len; ++i) ((uint8_t *)buf)[i] ^= mask[i & 3];
        return (int)len;
    }
}

/* ---- HTTPS (S3 and the CM directory) -------------------------------------- */

typedef struct {
    int status;
    size_t len;
    int eresult;    /* X-eresult, which Web API calls answer with */
} isaac_http;

static int http_request(const char *host, const char *method, const char *path,
                        const char *const *extra, size_t nextra,
                        const void *body, size_t body_len,
                        void *resp, size_t cap, isaac_http *out,
                        int timeout_ms, char *err, size_t errcap) {
    isaac_tls t;
    if (tls_open(&t, host, 443, timeout_ms, err, errcap) != 0) return -1;
    char req[2048];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path, host);
    if (n <= 0 || (size_t)n >= sizeof req) {
        tls_close(&t);
        set_err(err, errcap, "request too long");
        return -1;
    }
    size_t used = (size_t)n;
    for (size_t i = 0; i < nextra; ++i) {
        /* Steam hands some download URLs a "host" header of their own; a
         * second Host line makes the server answer 400 (measured on the
         * console), and ours already names the same host. */
        if (!strncasecmp(extra[i], "host:", 5)) continue;
        n = snprintf(req + used, sizeof req - used, "%s\r\n", extra[i]);
        if (n <= 0 || (size_t)n >= sizeof req - used) {
            tls_close(&t);
            set_err(err, errcap, "request headers too long");
            return -1;
        }
        used += (size_t)n;
    }
    if (body_len || !strcmp(method, "PUT") || !strcmp(method, "POST")) {
        n = snprintf(req + used, sizeof req - used, "Content-Length: %zu\r\n", body_len);
        if (n <= 0 || (size_t)n >= sizeof req - used) {
            tls_close(&t);
            set_err(err, errcap, "request headers too long");
            return -1;
        }
        used += (size_t)n;
    }
    n = snprintf(req + used, sizeof req - used, "Connection: close\r\n\r\n");
    if (n <= 0 || (size_t)n >= sizeof req - used) {
        tls_close(&t);
        set_err(err, errcap, "request headers too long");
        return -1;
    }
    used += (size_t)n;
    if (tls_write_all(&t, req, used) != 0 || (body_len && tls_write_all(&t, body, body_len) != 0)) {
        tls_close(&t);
        set_err(err, errcap, "%s %s: write failed", method, host);
        return -1;
    }

    /* Headers, then the body (Content-Length or chunked). */
    size_t hlen = 0;
    int header_end = -1;
    while (hlen < HTTP_HDR_MAX) {
        int got = tls_read(&t, g_http_hdr + hlen, HTTP_HDR_MAX - hlen - 1);
        if (got <= 0) break;
        hlen += (size_t)got;
        g_http_hdr[hlen] = 0;
        char *end = strstr((char *)g_http_hdr, "\r\n\r\n");
        if (end) {
            header_end = (int)(end - (char *)g_http_hdr) + 4;
            break;
        }
    }
    if (header_end < 0) {
        tls_close(&t);
        set_err(err, errcap, "%s %s: no response headers", method, host);
        return -1;
    }
    int status = 0;
    if (sscanf((char *)g_http_hdr, "HTTP/%*d.%*d %d", &status) != 1) {
        tls_close(&t);
        set_err(err, errcap, "%s %s: bad status line", method, host);
        return -1;
    }
    long content_length = -1;
    int chunked = 0, eresult = ERESULT_OK;
    for (char *line = strstr((char *)g_http_hdr, "\r\n"); line && line < (char *)g_http_hdr + header_end; ) {
        line += 2;
        if (!strncasecmp(line, "Content-Length:", 15)) content_length = strtol(line + 15, NULL, 10);
        else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked")) chunked = 1;
        else if (!strncasecmp(line, "X-eresult:", 10)) eresult = atoi(line + 10);
        char *next = strstr(line, "\r\n");
        if (!next || next >= (char *)g_http_hdr + header_end) break;
        line = next;
    }

    size_t body_have = hlen - (size_t)header_end;
    uint8_t *rest = g_http_hdr + header_end;
    size_t total = 0;
    if (chunked) {
        /* De-chunk into resp; S3 uses this for some objects. */
        uint8_t *chunk = g_ws_buf;
        size_t chunk_len = body_have;
        if (chunk_len > WS_RECV_MAX) chunk_len = WS_RECV_MAX;
        memcpy(chunk, rest, chunk_len);
        size_t pos = 0;
        for (;;) {
            while (pos < chunk_len && chunk[pos] != '\n') ++pos;
            if (pos >= chunk_len) break;
            size_t size = strtoul((char *)chunk + pos, NULL, 16);
            ++pos;
            while (pos + size + 2 > chunk_len) {          /* need more bytes */
                if (chunk_len >= WS_RECV_MAX) break;
                int got = tls_read(&t, chunk + chunk_len, WS_RECV_MAX - chunk_len);
                if (got <= 0) break;
                chunk_len += (size_t)got;
            }
            if (size == 0) break;
            if (total + size > cap) {
                tls_close(&t);
                set_err(err, errcap, "%s %s: response too large", method, host);
                return -1;
            }
            memcpy((uint8_t *)resp + total, chunk + pos, size);
            total += size;
            pos += size;
            while (pos < chunk_len && (chunk[pos] == '\r' || chunk[pos] == '\n')) ++pos;
        }
    } else {
        if (content_length >= 0 && (size_t)content_length > cap) {
            tls_close(&t);
            set_err(err, errcap, "%s %s: response too large (%ld)", method, host, content_length);
            return -1;
        }
        if (body_have > cap) body_have = cap;
        memcpy(resp, rest, body_have);
        total = body_have;
        while ((content_length < 0 || total < (size_t)content_length) && total < cap) {
            int got = tls_read(&t, (uint8_t *)resp + total, cap - total);
            if (got <= 0) break;
            total += (size_t)got;
        }
    }
    tls_close(&t);
    if (content_length >= 0 && !chunked && total != (size_t)content_length) {
        set_err(err, errcap, "%s %s: short body (%zu of %ld)", method, host, total, content_length);
        return -1;
    }
    out->status = status;
    out->len = total;
    out->eresult = eresult;
    return 0;
}

/* ---- CM session ----------------------------------------------------------- */

typedef struct {
    isaac_tls tls;
    uint64_t steamid;
    uint64_t session_id;
    int heartbeat_seconds;
    int timeout_ms;
    time_t last_heartbeat;
    size_t reply_len;
    int reply_eresult;
} isaac_steamcm;

static isaac_steamcm g_cm;
static int g_ssl_ready;
static int g_token_refused;     /* the last logon: Steam turned the token down */

static uint64_t rand64(void) {
    uint64_t v = 0;
    randomGet(&v, sizeof v);
    return v & 0x7fffffffffffffffull;
}

static int cm_send(unsigned emsg, const void *hdr, size_t hlen, const void *body, size_t blen) {
    size_t n = isaac_cm_frame(g_send_buf, sizeof g_send_buf, emsg, hdr, hlen, body, blen);
    if (!n) return -1;
    return ws_send(&g_cm.tls, g_send_buf, n);
}

static size_t cm_header(void *out, size_t cap, const char *method, uint64_t job) {
    isaac_pbw w;
    isaac_pbw_init(&w, out, cap);
    isaac_pbw_fixed64(&w, 1, g_cm.steamid);            /* steamid */
    isaac_pbw_varint(&w, 2, g_cm.session_id);          /* client_sessionid */
    if (job) isaac_pbw_fixed64(&w, 10, job);           /* jobid_source */
    if (method) isaac_pbw_str(&w, 12, method);         /* target_job_name */
    return w.overflow ? 0 : w.len;
}

typedef struct {
    unsigned want_emsg;
    const char *want_method;
    int found;
    int eresult;
    size_t body_len;
} cm_wait;

static void cm_on_msg(void *user, const isaac_cmmsg *m) {
    cm_wait *w = user;
    if (w->found) return;
    if (m->emsg != w->want_emsg) {
        if (!(w->want_emsg == ISAAC_EMSG_SERVICE_METHOD_RESPONSE &&
              m->emsg == ISAAC_EMSG_SERVICE_METHOD))
            return;
    }
    if (w->want_method) {
        char name[80];
        if (isaac_pbr_str(m->hdr, m->hlen, 12, name, sizeof name) <= 0) return;
        if (strcmp(name, w->want_method) != 0) return;
    }
    if (m->blen > REPLY_MAX) return;
    memcpy(g_reply_buf, m->body, m->blen);
    w->body_len = m->blen;
    w->eresult = (int)isaac_pbr_uint(m->hdr, m->hlen, 13, 2);
    if (!w->want_method) {
        g_cm.session_id = isaac_pbr_uint(m->hdr, m->hlen, 2, g_cm.session_id);
        if (!g_cm.session_id) g_cm.session_id = isaac_pbr_uint(m->body, m->blen, 2, 0);
    }
    w->found = 1;
}

/* Reads until the wanted message arrives, answering heartbeats meanwhile. */
static int cm_wait_for(unsigned emsg, const char *method, int timeout_ms, cm_wait *out) {
    cm_wait w = { emsg, method, 0, 2, 0 };
    time_t deadline = time(NULL) + timeout_ms / 1000;
    while (!w.found) {
        if (time(NULL) > deadline) return -1;
        if (g_cm.heartbeat_seconds && time(NULL) - g_cm.last_heartbeat >= 5) {
            size_t hlen = cm_header(g_hdr_buf, sizeof g_hdr_buf, NULL, 0);
            if (hlen && cm_send(ISAAC_EMSG_CLIENT_HEARTBEAT, g_hdr_buf, hlen, NULL, 0) == 0)
                g_cm.last_heartbeat = time(NULL);
        }
        int n = ws_recv(&g_cm.tls, g_ws_buf, sizeof g_ws_buf);
        if (n == -2) continue;                    /* oversized: skipped */
        if (n <= 0) return -1;
        if (isaac_cm_split(g_ws_buf, (size_t)n, g_gunzip_buf, sizeof g_gunzip_buf, cm_on_msg, &w) != 0)
            continue;                             /* unparsable frame: ignore */
    }
    if (out) *out = w;
    g_cm.reply_len = w.body_len;
    g_cm.reply_eresult = w.eresult;
    return 0;
}

static int cm_call(const char *method, const void *req, size_t req_len, int timeout_ms) {
    size_t hlen = cm_header(g_hdr_buf, sizeof g_hdr_buf, method, rand64());
    if (!hlen) return -1;
    if (cm_send(ISAAC_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT, g_hdr_buf, hlen, req, req_len) != 0)
        return -1;
    if (cm_wait_for(ISAAC_EMSG_SERVICE_METHOD_RESPONSE, method, timeout_ms, NULL) != 0) return -1;
    return g_cm.reply_eresult;
}

/* The CM list: one HTTPS call, then the first host that answers a websocket.
 * `err` carries why it failed (resolve, connect, handshake, HTTP status), which
 * is the only thing a console run leaves behind. */
static int cm_hosts(char hosts[][64], int max, char *err, size_t errcap) {
    isaac_http http;
    if (http_request("api.steampowered.com", "GET",
                     "/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets",
                     NULL, 0, NULL, 0, g_reply_buf, REPLY_MAX, &http, 15000, err, errcap) != 0)
        return 0;
    if (http.status != 200) {
        set_err(err, errcap, "CM list: HTTP %d", http.status);
        return 0;
    }
    int count = 0;
    const char *p = (const char *)g_reply_buf;
    const char *end = p + http.len;
    while (count < max) {
        const char *hit = strstr(p, "\"endpoint\":\"");
        if (!hit || hit >= end) break;
        hit += 12;
        const char *close = memchr(hit, '"', (size_t)(end - hit));
        if (!close) break;
        size_t n = (size_t)(close - hit);
        if (n < sizeof hosts[0]) {
            memcpy(hosts[count], hit, n);
            hosts[count][n] = 0;
            ++count;
        }
        p = close;
    }
    if (!count) set_err(err, errcap, "CM list: no endpoint in %zu bytes", (size_t)http.len);
    return count;
}

static int cm_logon(const char *token, char *err, size_t errcap) {
    /* ClientHello first, as the Steam client does. */
    size_t hlen = cm_header(g_hdr_buf, sizeof g_hdr_buf, NULL, 0);
    isaac_pbw w;
    isaac_pbw_init(&w, g_msg_buf, sizeof g_msg_buf);
    isaac_pbw_varint(&w, 1, 65580);
    if (!hlen || w.overflow || cm_send(ISAAC_EMSG_CLIENT_HELLO, g_hdr_buf, hlen, g_msg_buf, w.len) != 0) {
        set_err(err, errcap, "hello failed");
        return -1;
    }
    uint8_t machine[60];
    randomGet(machine, sizeof machine);
    static const char name[] = "Isaac on Switch (NRO)";
    isaac_pbw_init(&w, g_msg_buf, sizeof g_msg_buf);
    isaac_pbw_varint(&w, 1, 65580);                    /* protocol_version */
    isaac_pbw_varint(&w, 3, 0);                        /* cell_id */
    isaac_pbw_varint(&w, 5, 1771);                     /* client_package_version */
    isaac_pbw_str(&w, 6, "english");                   /* client_language */
    isaac_pbw_varint(&w, 7, 16);                       /* client_os_type: Windows 10 */
    isaac_pbw_varint(&w, 8, 1);                        /* should_remember_password */
    isaac_pbw_fixed64(&w, 22, g_cm.steamid);           /* client_supplied_steam_id */
    isaac_pbw_bytes(&w, 30, machine, sizeof machine);  /* machine_id */
    isaac_pbw_varint(&w, 33, 2);                       /* chat_mode */
    isaac_pbw_str(&w, 96, name);
    isaac_pbw_str(&w, 97, name);
    isaac_pbw_fixed64(&w, 100, rand64());              /* client_instance_id */
    isaac_pbw_varint(&w, 102, 1);                      /* supports_rate_limit_response */
    isaac_pbw_str(&w, 108, token);                     /* access_token = refresh token */
    if (w.overflow) {
        set_err(err, errcap, "logon message too large");
        return -1;
    }
    hlen = cm_header(g_hdr_buf, sizeof g_hdr_buf, NULL, 0);
    if (!hlen || cm_send(ISAAC_EMSG_CLIENT_LOGON, g_hdr_buf, hlen, g_msg_buf, w.len) != 0) {
        set_err(err, errcap, "logon send failed");
        return -1;
    }
    cm_wait reply;
    if (cm_wait_for(ISAAC_EMSG_CLIENT_LOGON_RESPONSE, NULL, 30000, &reply) != 0) {
        set_err(err, errcap, "no logon response");
        return -1;
    }
    /* A logon answers in its body (field 1); the header's eresult (13), which
     * service replies use, is absent here and would read as the default. */
    int eresult = (int)isaac_pbr_uint(g_reply_buf, reply.body_len, 1, 2);
    if (eresult != ERESULT_OK) {
        g_token_refused = eresult == ERESULT_INVALID_PASSWORD || eresult == ERESULT_ACCESS_DENIED ||
                          eresult == ERESULT_EXPIRED;
        set_err(err, errcap, "logon refused (eresult %d, extended %d)", eresult,
                (int)isaac_pbr_uint(g_reply_buf, reply.body_len, 10, 0));
        return -1;
    }
    g_cm.heartbeat_seconds = (int)isaac_pbr_uint(g_reply_buf, g_cm.reply_len, 3, 9);
    g_cm.last_heartbeat = time(NULL);
    return 0;
}

/* ---- Cloud service calls -------------------------------------------------- */

static void trim_name(const uint8_t *src, size_t len, char *out, size_t cap) {
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, src, n);
    out[n] = 0;
}

static int cloud_enumerate(void *state, uint32_t appid, isaac_cloud_file *out, size_t max,
                           size_t *count, char *err, size_t errcap) {
    (void)state;
    uint8_t req[32];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, appid);
    isaac_pbw_varint(&w, 2, 1);            /* extended_details: we want file_sha */
    isaac_pbw_varint(&w, 3, 100);          /* count */
    isaac_pbw_varint(&w, 4, 0);            /* start_index */
    int rc = cm_call("Cloud.EnumerateUserFiles#1", req, w.len, 20000);
    if (rc != ERESULT_OK) {
        set_err(err, errcap, "EnumerateUserFiles: eresult %d", rc);
        return -1;
    }
    size_t found = 0;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, g_reply_buf, g_cm.reply_len);
    while (isaac_pbr_next(&r, &f) == 1) {
        if (f.field != 1 || f.wire != 2 || found >= max) continue;
        isaac_cloud_file *file = &out[found];
        memset(file, 0, sizeof *file);
        isaac_pbr fr;
        isaac_pbfield ff;
        isaac_pbr_init(&fr, f.bytes, f.len);
        while (isaac_pbr_next(&fr, &ff) == 1) {
            switch (ff.field) {
            case 3: if (ff.wire == 2) trim_name(ff.bytes, ff.len, file->name, sizeof file->name); break;
            case 4: file->timestamp = ff.value; break;
            case 5: file->size = (uint32_t)ff.value; break;
            case 10: if (ff.wire == 2) trim_name(ff.bytes, ff.len, file->sha_hex, sizeof file->sha_hex); break;
            default: break;
            }
        }
        if (file->name[0]) {
            say("cloud: %s %u B at %llu sha %.12s", file->name, file->size,
                (unsigned long long)file->timestamp, file->sha_hex);
            ++found;
        }
    }
    *count = found;
    say("cloud: %zu file(s)", found);
    return 0;
}

static int cloud_download(void *state, uint32_t appid, const char *name, void *raw, size_t cap,
                          size_t *raw_len, char sha_hex[41], uint64_t *timestamp,
                          char *err, size_t errcap) {
    (void)state;
    uint8_t req[128];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, appid);
    isaac_pbw_str(&w, 2, name);
    int rc = cm_call("Cloud.ClientFileDownload#1", req, w.len, 20000);
    if (rc != ERESULT_OK) {
        set_err(err, errcap, "ClientFileDownload: eresult %d", rc);
        return -1;
    }
    char host[128] = "", path[1024] = "";
    uint8_t want[ISAAC_SHA1_DIGEST];
    int have_sha = 0;
    uint32_t raw_size = 0;
    char hdrs[4][256];
    size_t nhdrs = 0;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, g_reply_buf, g_cm.reply_len);
    while (isaac_pbr_next(&r, &f) == 1) {
        switch (f.field) {
        case 3: raw_size = (uint32_t)f.value; break;
        case 4:
            if (f.len == ISAAC_SHA1_DIGEST) {
                memcpy(want, f.bytes, sizeof want);
                have_sha = 1;
            }
            break;
        case 5: if (timestamp) *timestamp = f.value; break;
        case 7: if (f.wire == 2) trim_name(f.bytes, f.len, host, sizeof host); break;
        case 8: if (f.wire == 2) trim_name(f.bytes, f.len, path, sizeof path); break;
        case 10: {
            if (f.wire != 2 || nhdrs >= 4) break;
            char hn[64] = "", hv[192] = "";
            isaac_pbr hr;
            isaac_pbfield hf;
            isaac_pbr_init(&hr, f.bytes, f.len);
            while (isaac_pbr_next(&hr, &hf) == 1) {
                if (hf.field == 1 && hf.wire == 2) trim_name(hf.bytes, hf.len, hn, sizeof hn);
                else if (hf.field == 2 && hf.wire == 2) trim_name(hf.bytes, hf.len, hv, sizeof hv);
            }
            if (hn[0]) snprintf(hdrs[nhdrs++], sizeof hdrs[0], "%s: %s", hn, hv);
            break;
        }
        default: break;
        }
    }
    if (!host[0] || !path[0]) {
        set_err(err, errcap, "download: no URL");
        return -1;
    }
    const char *list[4];
    for (size_t i = 0; i < nhdrs; ++i) list[i] = hdrs[i];
    isaac_http http;
    if (http_request(host, "GET", path, list, nhdrs, NULL, 0, g_zip_buf, sizeof g_zip_buf,
                     &http, 30000, err, errcap) != 0)
        return -1;
    if (http.status != 200) {
        set_err(err, errcap, "download: HTTP %d", http.status);
        return -1;
    }
    /* Steam stores a cloud file compressed or as-is (file_size == raw_file_size
     * then), so the ZIP wrapper is only there when it actually is one. */
    size_t got = 0;
    if (isaac_zip_looks_like_zip(g_zip_buf, http.len)) {
        if (isaac_zip_unpack(g_zip_buf, http.len, raw, cap, &got) != 0) {
            set_err(err, errcap, "download: bad ZIP");
            return -1;
        }
    } else {
        if (http.len > cap) {
            set_err(err, errcap, "download: %zu bytes do not fit", http.len);
            return -1;
        }
        memcpy(raw, g_zip_buf, http.len);
        got = http.len;
    }
    if (raw_size && got != raw_size) {
        set_err(err, errcap, "download: %zu bytes, expected %u", got, raw_size);
        return -1;
    }
    uint8_t digest[ISAAC_SHA1_DIGEST];
    isaac_sha1_buf(raw, got, digest);
    if (have_sha && memcmp(digest, want, sizeof want) != 0) {
        set_err(err, errcap, "download: SHA-1 mismatch");
        return -1;
    }
    if (sha_hex) isaac_sha1_hex(digest, sha_hex);
    *raw_len = got;
    say("downloaded %s (%zu bytes)", name, got);
    return 0;
}

/* After a failed PUT: tell Steam the transfer is over, so the file is not
 * left with an upload open (the PC client reports that as a cloud error). */
static void cloud_upload_abort(uint32_t appid, const char *name, const char *hex) {
    uint8_t req[512];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, 0);                  /* transfer_succeeded */
    isaac_pbw_varint(&w, 2, appid);
    isaac_pbw_str(&w, 3, hex);
    isaac_pbw_str(&w, 4, name);
    int rc = cm_call("Cloud.CommitHTTPUpload#1", req, w.len, 20000);
    say("upload of %s called off (eresult %d)", name, rc);
}

/* The start of an HTTP error's body (S3 answers with an XML <Code>), for the log. */
static void say_http_body(const char *what, const isaac_http *http) {
    char text[161];
    size_t n = http->len < sizeof text - 1 ? http->len : sizeof text - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = (char)g_reply_buf[i];
        text[i] = c >= 32 && c < 127 ? c : ' ';
    }
    text[n] = 0;
    say("%s: HTTP %d: %s", what, http->status, text);
}

static int cloud_upload_once(uint32_t appid, const char *name, const void *raw, size_t raw_len,
                             uint64_t *timestamp, int *retry, char *err, size_t errcap) {
    uint8_t digest[ISAAC_SHA1_DIGEST];
    char hex[41];
    isaac_sha1_buf(raw, raw_len, digest);
    isaac_sha1_hex(digest, hex);

    uint8_t req[512];
    isaac_pbw w;
    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, appid);
    isaac_pbw_varint(&w, 2, raw_len);            /* file_size: the bytes the PUT carries */
    isaac_pbw_str(&w, 3, name);
    isaac_pbw_str(&w, 4, hex);
    isaac_pbw_varint(&w, 5, 0);                  /* is_public */
    isaac_pbw_str(&w, 6, "All");                 /* platforms_to_sync */
    isaac_pbw_fixed64(&w, 9, 0);                 /* upload_batch_id */
    int rc = cm_call("Cloud.BeginHTTPUpload#1", req, w.len, 20000);
    if (rc == ERESULT_DUPLICATE) {               /* Steam already has these bytes */
        if (timestamp) *timestamp = (uint64_t)time(NULL);
        return 0;
    }
    if (rc != ERESULT_OK) {
        set_err(err, errcap, "BeginHTTPUpload: eresult %d", rc);
        return -1;
    }
    char host[128] = "", path[1024] = "";
    char hdrs[4][256];
    size_t nhdrs = 0;
    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, g_reply_buf, g_cm.reply_len);
    while (isaac_pbr_next(&r, &f) == 1) {
        switch (f.field) {
        case 3: if (f.wire == 2) trim_name(f.bytes, f.len, host, sizeof host); break;
        case 4: if (f.wire == 2) trim_name(f.bytes, f.len, path, sizeof path); break;
        case 6: {
            if (f.wire != 2 || nhdrs >= 4) break;
            char hn[64] = "", hv[192] = "";
            isaac_pbr hr;
            isaac_pbfield hf;
            isaac_pbr_init(&hr, f.bytes, f.len);
            while (isaac_pbr_next(&hr, &hf) == 1) {
                if (hf.field == 1 && hf.wire == 2) trim_name(hf.bytes, hf.len, hn, sizeof hn);
                else if (hf.field == 2 && hf.wire == 2) trim_name(hf.bytes, hf.len, hv, sizeof hv);
            }
            if (hn[0]) snprintf(hdrs[nhdrs++], sizeof hdrs[0], "%s: %s", hn, hv);
            break;
        }
        default: break;
        }
    }
    if (!host[0] || !path[0]) {
        set_err(err, errcap, "upload: no URL");
        return -1;
    }
    const char *list[4];
    for (size_t i = 0; i < nhdrs; ++i) list[i] = hdrs[i];
    isaac_http http;
    if (http_request(host, "PUT", path, list, nhdrs, raw, raw_len, g_reply_buf, REPLY_MAX,
                     &http, 60000, err, errcap) != 0) {
        cloud_upload_abort(appid, name, hex);
        *retry = 1;
        return -1;
    }
    if (http.status != 200 && http.status != 201) {
        say_http_body(name, &http);
        set_err(err, errcap, "upload: HTTP %d", http.status);
        cloud_upload_abort(appid, name, hex);
        *retry = 1;
        return -1;
    }

    isaac_pbw_init(&w, req, sizeof req);
    isaac_pbw_varint(&w, 1, 1);                  /* transfer_succeeded */
    isaac_pbw_varint(&w, 2, appid);
    isaac_pbw_str(&w, 3, hex);
    isaac_pbw_str(&w, 4, name);
    rc = cm_call("Cloud.CommitHTTPUpload#1", req, w.len, 20000);
    if (rc != ERESULT_OK) {
        set_err(err, errcap, "CommitHTTPUpload: eresult %d", rc);
        return -1;
    }
    if (timestamp) *timestamp = (uint64_t)time(NULL);
    say("uploaded %s (%zu bytes)", name, raw_len);
    return 0;
}

/* A failed transfer is called off and tried once more from the start (a new
 * URL); Steam's answers to the CM calls are not retried. */
static int cloud_upload(void *state, uint32_t appid, const char *name, const void *raw, size_t raw_len,
                        uint64_t *timestamp, char *err, size_t errcap) {
    (void)state;
    int retry = 0;
    if (cloud_upload_once(appid, name, raw, raw_len, timestamp, &retry, err, errcap) == 0) return 0;
    if (!retry) return -1;
    say("upload of %s: trying again (%s)", name, err);
    retry = 0;
    return cloud_upload_once(appid, name, raw, raw_len, timestamp, &retry, err, errcap);
}

/* The call the Steam client makes before it runs an app. Steam issues the ticket
 * only to an account with a licence for it, and answers AccessDenied otherwise;
 * the ticket itself is not kept. */
static int cm_owns(void *state, uint32_t appid, char *err, size_t errcap) {
    (void)state;
    uint8_t body[16];
    isaac_pbw w;
    isaac_pbw_init(&w, body, sizeof body);
    isaac_pbw_varint(&w, 1, appid);
    size_t hlen = cm_header(g_hdr_buf, sizeof g_hdr_buf, NULL, 0);
    if (!hlen || w.overflow ||
        cm_send(ISAAC_EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET, g_hdr_buf, hlen, body, w.len) != 0) {
        set_err(err, errcap, "ownership request failed");
        return -1;
    }
    cm_wait reply;
    if (cm_wait_for(ISAAC_EMSG_CLIENT_GET_APP_OWNERSHIP_TICKET_RESPONSE, NULL, 10000, &reply) != 0) {
        set_err(err, errcap, "no answer to the ownership request");
        return -1;
    }
    int eresult = (int)isaac_pbr_uint(g_reply_buf, reply.body_len, 1, 2);
    if (eresult == ERESULT_OK) return 1;
    if (eresult == ERESULT_ACCESS_DENIED) return 0;
    set_err(err, errcap, "ownership of %u: eresult %d", (unsigned)appid, eresult);
    return -1;
}

static void cm_transport_close(void *state) {
    (void)state;
    tls_close(&g_cm.tls);
    if (g_ssl_ready) {
        sslExit();
        g_ssl_ready = 0;
    }
    memset(&g_cm, 0, sizeof g_cm);
    tls_init(&g_cm.tls);
}

int isaac_steamcm_open(uint64_t steamid, const char *token, int timeout_ms,
                       isaac_steam_transport *out, char *err, size_t errcap) {
    if (!token || !*token) {
        set_err(err, errcap, "no Steam token");
        return -1;
    }
    if (!out) return -1;
    g_token_refused = 0;
    memset(&g_cm, 0, sizeof g_cm);
    tls_init(&g_cm.tls);
    g_cm.timeout_ms = timeout_ms;
    g_cm.steamid = steamid;
    if (!g_ssl_ready) {
        if (R_FAILED(sslInitialize(1))) {
            set_err(err, errcap, "sslInitialize failed");
            return -1;
        }
        g_ssl_ready = 1;
    }
    char hosts[8][64];
    int count = cm_hosts(hosts, 8, err, errcap);
    if (!count) {
        if (!err || !err[0]) set_err(err, errcap, "no CM list");
        cm_transport_close(NULL);
        return -1;
    }
    say("CM list: %d host(s)", count);
    char last[160] = "no CM accepted the connection";
    for (int i = 0; i < count; ++i) {
        char host[64];
        snprintf(host, sizeof host, "%s", hosts[i]);
        char *colon = strrchr(host, ':');
        int port = 443;
        if (colon) {
            *colon = 0;
            port = atoi(colon + 1);
        }
        if (ws_open(&g_cm.tls, host, "/cmsocket/", port, timeout_ms, err, errcap) != 0) {
            snprintf(last, sizeof last, "%s: %s", host, err ? err : "connect failed");
            say("%s", last);
            tls_close(&g_cm.tls);
            continue;
        }
        say("websocket + TLS to %s:%d", host, port);
        if (cm_logon(token, err, errcap) != 0) {
            snprintf(last, sizeof last, "%s: %s", host, err ? err : "logon failed");
            say("%s", last);
            tls_close(&g_cm.tls);
            if (g_token_refused) break;     /* the other CMs would say the same */
            continue;
        }
        say("logged on (cell %d, heartbeat %d s)",
            (int)isaac_pbr_uint(g_reply_buf, g_cm.reply_len, 7, 0), g_cm.heartbeat_seconds);
        out->state = &g_cm;
        out->enumerate = cloud_enumerate;
        out->download = cloud_download;
        out->upload = cloud_upload;
        out->owns = cm_owns;
        out->close = cm_transport_close;
        return 0;
    }
    set_err(err, errcap, "%s", last);
    cm_transport_close(NULL);
    return -1;
}

int isaac_steamcm_token_refused(void) {
    return g_token_refused;
}

int isaac_steamcm_revoke(uint64_t steamid, const char *token, int timeout_ms, char *err, size_t errcap) {
    isaac_steam_transport session;
    if (isaac_steamcm_open(steamid, token, timeout_ms, &session, err, errcap) != 0)
        return g_token_refused ? 1 : -1;
    isaac_pbw w;
    isaac_pbw_init(&w, g_msg_buf, sizeof g_msg_buf);
    isaac_pbw_str(&w, 1, token);
    isaac_pbw_varint(&w, 2, REVOKE_PERMANENT);
    int rc = w.overflow ? -1 : cm_call("Authentication.RevokeToken#1", g_msg_buf, w.len, 20000);
    say("revoke: %d", rc);
    session.close(session.state);
    if (rc == ERESULT_OK) return 0;
    if (rc < 0) set_err(err, errcap, "Steam did not answer the sign-out");
    else set_err(err, errcap, "RevokeToken: eresult %d", rc);
    return -1;
}

int isaac_steamcm_web_open(char *err, size_t errcap) {
    if (g_ssl_ready) return 0;
    if (R_FAILED(sslInitialize(1))) {
        set_err(err, errcap, "sslInitialize failed");
        return -1;
    }
    g_ssl_ready = 1;
    return 0;
}

void isaac_steamcm_web_close(void) {
    if (!g_ssl_ready) return;
    sslExit();
    g_ssl_ready = 0;
}

int isaac_steamcm_webapi(void *user, const char *iface, const char *method,
                         const void *req, size_t req_len,
                         void *resp, size_t cap, size_t *resp_len, int *eresult,
                         char *err, size_t errcap) {
    (void)user;
    static char form[4096];
    char path[128];
    size_t form_len = isaac_steamauth_form(req, req_len, form, sizeof form);
    if (!form_len || snprintf(path, sizeof path, "/%s/%s/v1/", iface, method) >= (int)sizeof path) {
        set_err(err, errcap, "%s: request too long", method);
        return -1;
    }
    static const char *const headers[] = {
        "Content-Type: application/x-www-form-urlencoded",
        "User-Agent: isaac-switch-native",
    };
    isaac_http http;
    if (http_request("api.steampowered.com", "POST", path, headers, 2, form, form_len,
                     resp, cap, &http, 15000, err, errcap) != 0)
        return -1;
    if (http.status != 200) {
        set_err(err, errcap, "%s: HTTP %d", method, http.status);
        return -1;
    }
    *resp_len = http.len;
    *eresult = http.eresult;
    return 0;
}

#else /* !__SWITCH__: the PC build has its own Steam client. */

int isaac_steamcm_token_refused(void) {
    return 0;
}

int isaac_steamcm_revoke(uint64_t steamid, const char *token, int timeout_ms, char *err, size_t errcap) {
    (void)steamid;
    (void)token;
    (void)timeout_ms;
    if (err && errcap) snprintf(err, errcap, "the Steam transport is Switch-only");
    return -1;
}

int isaac_steamcm_open(uint64_t steamid, const char *token, int timeout_ms,
                       isaac_steam_transport *out, char *err, size_t errcap) {
    (void)steamid;
    (void)token;
    (void)timeout_ms;
    (void)out;
    if (err && errcap) snprintf(err, errcap, "the Steam transport is Switch-only");
    return -1;
}

#endif
