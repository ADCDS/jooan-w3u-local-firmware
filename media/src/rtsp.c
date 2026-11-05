/* SPDX-License-Identifier: MIT
 * Local RTSP 1.0 server. RTP: RFC 3550; H.264 packetization: RFC 6184.
 * Each connection has one joinable worker; that worker owns all network I/O.
 */
#include "jipc.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define RTSP_HEADER_MAX 16384u
#define RTSP_BUFFER_SIZE 32768u
#define RTP_PAYLOAD_MAX 1200u
#define RTSP_CLIENT_LIMIT 64u
#define RTSP_TIMEOUT_US UINT64_C(120000000)

typedef struct {
    int setup, tcp, rtp_fd, rtcp_fd;
    unsigned rtp_channel, rtcp_channel, client_rtp, client_rtcp;
    unsigned server_rtp, server_rtcp;
    uint16_t sequence;
    uint32_t ssrc, timestamp_base, packets, octets;
    uint64_t last_report_us;
} rtsp_track;

typedef struct rtsp_server rtsp_server;
typedef struct {
    rtsp_server *server;
    pthread_t thread;
    int fd, done, channel, playing, need_keyframe;
    struct sockaddr_in peer;
    rtsp_track tracks[2];
    unsigned char input[RTSP_BUFFER_SIZE];
    size_t input_size;
    char nonce[33], session[33], base_uri[2048];
    uint64_t nonce_us, last_activity_us, cursor[2];
    uint64_t pts_origin_us, mono_origin_us;
    int clock_started;
    unsigned long nonce_count;
} rtsp_client;

struct rtsp_server {
    jipc_app *app;
    int listener;
    unsigned capacity;
    pthread_mutex_t mutex;
    atomic_int stopping;
    rtsp_client **clients;
};

typedef struct {
    char method[32], uri[2048], authorization[3072], transport[1024], session[128], range[128];
    unsigned cseq;
    size_t content_length;
} rtsp_request;

static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;

static int stopped(rtsp_client *client)
{
    return atomic_load(&client->server->stopping) || client->server->app->stop;
}

static int random_hex(char *out, size_t bytes)
{
    unsigned char data[32];
    static const char hex[] = "0123456789abcdef";
    if (bytes > sizeof(data) || jipc_random(data, bytes)) return -1;
    for (size_t i = 0; i < bytes; ++i) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 15];
    }
    out[bytes * 2] = '\0';
    return 0;
}

static void put16(unsigned char *out, unsigned value)
{
    out[0] = (unsigned char)(value >> 8);
    out[1] = (unsigned char)value;
}

static void put32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int send_all(rtsp_client *client, const void *data, size_t size)
{
    const unsigned char *cursor = data;
    uint64_t deadline = jipc_now_us() + UINT64_C(2000000);
    while (size) {
        ssize_t count = send(client->fd, cursor, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) {
            cursor += (size_t)count;
            size -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
        if (stopped(client) || jipc_now_us() >= deadline) return -1;
        struct pollfd pollfd = {0};
        pollfd.fd = client->fd;
        pollfd.events = POLLOUT;
        int result = poll(&pollfd, 1, 100);
        if (result < 0 && errno != EINTR) return -1;
        if (result > 0 && (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    }
    return 0;
}

static int response(rtsp_client *client, unsigned cseq, int code,
                    const char *reason, const char *extra, const char *body)
{
    char header[8192];
    size_t length = body ? strlen(body) : 0;
    int count = snprintf(header, sizeof(header),
                         "RTSP/1.0 %d %s\r\nCSeq: %u\r\n"
                         "Server: jooanipc-open/%s\r\n%sContent-Length: %zu\r\n\r\n",
                         code, reason, cseq, JIPC_VERSION, extra ? extra : "", length);
    if (count < 0 || (size_t)count >= sizeof(header)) return -1;
    if (send_all(client, header, (size_t)count)) return -1;
    return length ? send_all(client, body, length) : 0;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t'))
        text[--length] = '\0';
    return text;
}

static int parse_unsigned(const char *text, unsigned long limit, unsigned long *value)
{
    char *end;
    if (!text[0] || !isdigit((unsigned char)text[0])) return -1;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > limit) return -1;
    *value = parsed;
    return 0;
}

/* Parse only after the complete header is present. Every body is consumed before
 * dispatch, including rejected authenticated requests, so pipelining stays framed.
 */
static int parse_request(char *header, rtsp_request *request)
{
    char version[16], extra;
    char *line_end = strstr(header, "\r\n");
    int have_cseq = 0, have_length = 0;
    memset(request, 0, sizeof(*request));
    if (!line_end) return -1;
    *line_end = '\0';
    if (sscanf(header, "%31s %2047s %15s %c", request->method, request->uri,
               version, &extra) != 3 || strcmp(version, "RTSP/1.0")) return -1;
    for (const unsigned char *p = (const unsigned char *)request->uri; *p; ++p)
        if (*p <= 32 || *p == 127) return -1;
    char *line = line_end + 2;
    unsigned fields = 0;
    while (*line) {
        line_end = strstr(line, "\r\n");
        if (!line_end || ++fields > 128 || *line == ' ' || *line == '\t') return -1;
        *line_end = '\0';
        char *colon = strchr(line, ':');
        if (!colon) return -1;
        *colon++ = '\0';
        char *value = trim(colon);
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127) return -1;
        unsigned long number;
        if (!strcasecmp(line, "CSeq")) {
            if (have_cseq++ || parse_unsigned(value, INT_MAX, &number)) return -1;
            request->cseq = (unsigned)number;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (have_length++ || parse_unsigned(value, RTSP_HEADER_MAX, &number)) return -1;
            request->content_length = (size_t)number;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            return -1;
        } else {
            char *target = NULL;
            size_t capacity = 0;
            if (!strcasecmp(line, "Authorization")) {
                target = request->authorization; capacity = sizeof(request->authorization);
            } else if (!strcasecmp(line, "Transport")) {
                target = request->transport; capacity = sizeof(request->transport);
            } else if (!strcasecmp(line, "Session")) {
                target = request->session; capacity = sizeof(request->session);
            } else if (!strcasecmp(line, "Range")) {
                target = request->range; capacity = sizeof(request->range);
            }
            if (target) {
                if (target[0] || strlen(value) >= capacity) return -1;
                memcpy(target, value, strlen(value) + 1);
            }
        }
        line = line_end + 2;
    }
    return have_cseq ? 0 : -1;
}

/* Strict quoted-string/token parser. Reject duplicate digest fields rather than
 * letting our interpretation differ from a client's authentication library.
 */
static int digest_field(const char *authorization, const char *wanted,
                        char *out, size_t capacity)
{
    const char *cursor = authorization;
    int found = 0;
    out[0] = '\0';
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        if (!*cursor) break;
        const char *key = cursor;
        while (isalnum((unsigned char)*cursor) || *cursor == '-' || *cursor == '_') ++cursor;
        size_t key_size = (size_t)(cursor - key);
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!key_size || *cursor++ != '=') return -1;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        int quoted = *cursor == '"';
        if (quoted) ++cursor;
        const char *value = cursor;
        if (quoted) {
            while (*cursor && *cursor != '"') {
                if (*cursor == '\\' || (unsigned char)*cursor < 32) return -1;
                ++cursor;
            }
            if (*cursor != '"') return -1;
        } else {
            while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != '\t') ++cursor;
        }
        size_t value_size = (size_t)(cursor - value);
        if (quoted) ++cursor;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor && *cursor != ',') return -1;
        if (strlen(wanted) == key_size && !strncasecmp(key, wanted, key_size)) {
            if (found++ || value_size >= capacity) return -1;
            memcpy(out, value, value_size);
            out[value_size] = '\0';
        }
    }
    return found;
}

static int authenticated(rtsp_client *client, const rtsp_request *request,
                         const jipc_config *config)
{
    char username[129], realm[129], nonce[129], uri[2048], answer[129];
    char qop[32], nc[32], cnonce[256], algorithm[64];
    char material[4096], ha1[33], ha2[33], expected[33];
    if (!config->username[0]) return 1;
    if (strncasecmp(request->authorization, "Digest ", 7)) return 0;
    const char *auth = request->authorization + 7;
    if (digest_field(auth, "username", username, sizeof(username)) != 1 ||
        digest_field(auth, "realm", realm, sizeof(realm)) != 1 ||
        digest_field(auth, "nonce", nonce, sizeof(nonce)) != 1 ||
        digest_field(auth, "uri", uri, sizeof(uri)) != 1 ||
        digest_field(auth, "response", answer, sizeof(answer)) != 1 ||
        digest_field(auth, "qop", qop, sizeof(qop)) < 0 ||
        digest_field(auth, "nc", nc, sizeof(nc)) < 0 ||
        digest_field(auth, "cnonce", cnonce, sizeof(cnonce)) < 0 ||
        digest_field(auth, "algorithm", algorithm, sizeof(algorithm)) < 0) return 0;
    if (strcmp(username, config->username) || strcmp(realm, config->realm) ||
        !jipc_equal(nonce, client->nonce) || strcmp(uri, request->uri) ||
        strlen(answer) != 32 || (algorithm[0] && strcasecmp(algorithm, "MD5")) ||
        jipc_now_us() - client->nonce_us > UINT64_C(300000000)) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!isxdigit((unsigned char)answer[i])) return 0;
        answer[i] = (char)tolower((unsigned char)answer[i]);
    }
    snprintf(material, sizeof(material), "%s:%s:%s", config->username,
             config->realm, config->password);
    jipc_md5(material, strlen(material), ha1);
    snprintf(material, sizeof(material), "%s:%s", request->method, request->uri);
    jipc_md5(material, strlen(material), ha2);
    unsigned long count = 0;
    if (qop[0]) {
        if (strcmp(qop, "auth") || !cnonce[0] || strlen(nc) != 8) return 0;
        for (unsigned i = 0; i < 8; ++i) if (!isxdigit((unsigned char)nc[i])) return 0;
        count = strtoul(nc, NULL, 16);
        if (!count || count <= client->nonce_count) return 0;
        snprintf(material, sizeof(material), "%s:%s:%s:%s:auth:%s", ha1,
                 nonce, nc, cnonce, ha2);
    } else {
        /* RFC 2069 compatibility for older camera viewers. */
        if (nc[0] || cnonce[0]) return 0;
        snprintf(material, sizeof(material), "%s:%s:%s", ha1, nonce, ha2);
    }
    jipc_md5(material, strlen(material), expected);
    if (!jipc_equal(answer, expected)) return 0;
    if (count) client->nonce_count = count;
    return 1;
}

static int challenge(rtsp_client *client, unsigned cseq, const jipc_config *config)
{
    char header[512];
    if (jipc_now_us() - client->nonce_us > UINT64_C(300000000)) {
        if (random_hex(client->nonce, 16)) return -1;
        client->nonce_us = jipc_now_us();
        client->nonce_count = 0;
    }
    /* Realm is validated as a printable string without quotes by configuration. */
    snprintf(header, sizeof(header),
             "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", "
             "algorithm=MD5, qop=\"auth\"\r\n", config->realm, client->nonce);
    return response(client, cseq, 401, "Unauthorized", header, NULL);
}

static int uri_channel(const char *uri, int *track, char *base, size_t capacity)
{
    const char *path = uri;
    if (!strncasecmp(uri, "rtsp://", 7)) {
        path = strchr(uri + 7, '/');
        if (!path) return -1;
    }
    if (*path != '/' || strchr(path, '?') || strchr(path, '#')) return -1;
    const char *suffix = strstr(path, "/trackID=");
    size_t path_length = suffix ? (size_t)(suffix - path) : strlen(path);
    while (path_length && path[path_length - 1] == '/') --path_length;
    *track = -1;
    if (suffix) {
        if (!strcmp(suffix, "/trackID=0")) *track = 0;
        else if (!strcmp(suffix, "/trackID=1")) *track = 1;
        else return -1;
    }
    int channel = -1;
    if (path_length == 8 && !memcmp(path, "/stream", 7) &&
        path[7] >= '0' && path[7] <= '3') channel = path[7] - '0';
    if (path_length == 7 && !memcmp(path, "/live/", 6) &&
        path[6] >= '0' && path[6] <= '3') channel = path[6] - '0';
    if (path_length == 9 && !memcmp(path, "/live/ch", 8) &&
        path[8] >= '0' && path[8] <= '3') channel = path[8] - '0';
    if (channel < 0) return -1;
    size_t base_length = (size_t)(path - uri) + path_length;
    if (base_length + 2 > capacity) return -1;
    memcpy(base, uri, base_length);
    base[base_length] = '/';
    base[base_length + 1] = '\0';
    return channel;
}

static int session_matches(const rtsp_client *client, const char *session)
{
    size_t length = strcspn(session, "; \t");
    return length == strlen(client->session) &&
           !memcmp(client->session, session, length);
}

static int describe(rtsp_client *client, const rtsp_request *request,
                    int channel, const char *base, const jipc_config *config)
{
    unsigned char sps[512], pps[512];
    size_t sps_size, pps_size;
    int ready = 0;
    for (unsigned attempt = 0; attempt < 20 && !stopped(client); ++attempt) {
        sps_size = sizeof(sps); pps_size = sizeof(pps);
        if (!jipc_codec_parameters(client->server->app, (unsigned)channel,
                                    sps, &sps_size, pps, &pps_size) &&
            sps_size >= 4 && pps_size) { ready = 1; break; }
        jipc_sleep_ms(50);
    }
    if (!ready) return response(client, request->cseq, 503, "Service Unavailable",
                                "Retry-After: 1\r\n", NULL);
    char sps64[700], pps64[700], sdp[4096], extra[2300];
    if (jipc_base64(sps, sps_size, sps64, sizeof(sps64)) < 0 ||
        jipc_base64(pps, pps_size, pps64, sizeof(pps64)) < 0) return -1;
    int length = snprintf(sdp, sizeof(sdp),
        "v=0\r\no=- %llu 1 IN IP4 0.0.0.0\r\ns=jooanipc-open stream %d\r\n"
        "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\na=range:npt=0-\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=%02x%02x%02x;"
        "sprop-parameter-sets=%s,%s\r\na=control:trackID=0\r\n",
        (unsigned long long)client->server->app->started_us, channel,
        sps[1], sps[2], sps[3], sps64, pps64);
    if (length < 0 || (size_t)length >= sizeof(sdp)) return -1;
    if (config->audio) {
        int added = snprintf(sdp + length, sizeof(sdp) - (size_t)length,
            "m=audio 0 RTP/AVP 8\r\na=rtpmap:8 PCMA/8000/1\r\na=control:trackID=1\r\n");
        if (added < 0 || (size_t)added >= sizeof(sdp) - (size_t)length) return -1;
    }
    snprintf(extra, sizeof(extra), "Content-Type: application/sdp\r\nContent-Base: %s\r\n", base);
    return response(client, request->cseq, 200, "OK", extra, sdp);
}

static void close_track(rtsp_track *track)
{
    if (track->rtp_fd >= 0) close(track->rtp_fd);
    if (track->rtcp_fd >= 0) close(track->rtcp_fd);
    track->rtp_fd = track->rtcp_fd = -1;
    track->setup = 0;
}

static int port_pair(const char *text, unsigned limit, unsigned *first, unsigned *second)
{
    char left[24];
    const char *dash = strchr(text, '-');
    if (!dash || dash == text || (size_t)(dash - text) >= sizeof(left)) return -1;
    memcpy(left, text, (size_t)(dash - text));
    left[dash - text] = '\0';
    unsigned long a, b;
    if (parse_unsigned(left, limit, &a) || parse_unsigned(dash + 1, limit, &b) || a == b)
        return -1;
    *first = (unsigned)a; *second = (unsigned)b;
    return 0;
}

static int udp_pair(rtsp_client *client, rtsp_track *track)
{
    struct sockaddr_in local;
    socklen_t local_size = sizeof(local);
    if (getsockname(client->fd, (struct sockaddr *)&local, &local_size)) return -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        int rtp = socket(AF_INET, SOCK_DGRAM, 0);
        int rtcp = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp < 0 || rtcp < 0) {
            if (rtp >= 0) close(rtp);
            if (rtcp >= 0) close(rtcp);
            return -1;
        }
        local.sin_port = 0;
        if (bind(rtp, (struct sockaddr *)&local, sizeof(local))) {
            close(rtp); close(rtcp); return -1;
        }
        local_size = sizeof(local);
        if (getsockname(rtp, (struct sockaddr *)&local, &local_size)) {
            close(rtp); close(rtcp); return -1;
        }
        unsigned port = ntohs(local.sin_port);
        if ((port & 1u) || port == 65535) { close(rtp); close(rtcp); continue; }
        local.sin_port = htons((uint16_t)(port + 1));
        if (bind(rtcp, (struct sockaddr *)&local, sizeof(local))) {
            close(rtp); close(rtcp); continue;
        }
        struct sockaddr_in remote = client->peer;
        remote.sin_port = htons((uint16_t)track->client_rtp);
        if (connect(rtp, (struct sockaddr *)&remote, sizeof(remote))) {
            close(rtp); close(rtcp); return -1;
        }
        remote.sin_port = htons((uint16_t)track->client_rtcp);
        if (connect(rtcp, (struct sockaddr *)&remote, sizeof(remote))) {
            close(rtp); close(rtcp); return -1;
        }
        track->rtp_fd = rtp; track->rtcp_fd = rtcp;
        track->server_rtp = port; track->server_rtcp = port + 1;
        return 0;
    }
    return -1;
}

static int setup(rtsp_client *client, const rtsp_request *request, int channel,
                 int track_id, const char *base, const jipc_config *config)
{
    if (track_id < 0) track_id = 0;
    if (track_id == 1 && !config->audio)
        return response(client, request->cseq, 404, "Not Found", NULL, NULL);
    if (client->playing)
        return response(client, request->cseq, 455, "Method Not Valid in This State", NULL, NULL);
    if ((request->session[0] && !session_matches(client, request->session)) ||
        (client->channel >= 0 && client->channel != channel))
        return response(client, request->cseq, 454, "Session Not Found", NULL, NULL);
    char options[1024], *save = NULL;
    if (!request->transport[0])
        return response(client, request->cseq, 461, "Unsupported Transport", NULL, NULL);
    memcpy(options, request->transport, strlen(request->transport) + 1);
    /* Single offered transport is intentional. Reject rather than misinterpret
     * alternative transports, multicast, SRTP, record mode or destination hosts.
     */
    if (strchr(options, ','))
        return response(client, request->cseq, 461, "Unsupported Transport", NULL, NULL);
    char *token = strtok_r(options, ";", &save);
    rtsp_track proposed = {0};
    proposed.rtp_fd = proposed.rtcp_fd = -1;
    if (token && !strcasecmp(trim(token), "RTP/AVP/TCP")) proposed.tcp = 1;
    else if (!token || (strcasecmp(trim(token), "RTP/AVP") &&
                       strcasecmp(trim(token), "RTP/AVP/UDP")))
        return response(client, request->cseq, 461, "Unsupported Transport", NULL, NULL);
    int have_pair = 0, invalid = 0;
    while ((token = strtok_r(NULL, ";", &save))) {
        token = trim(token);
        if (!strcasecmp(token, "unicast")) continue;
        if (!strcasecmp(token, "mode=\"PLAY\"") || !strcasecmp(token, "mode=PLAY")) continue;
        if (proposed.tcp && !strncasecmp(token, "interleaved=", 12)) {
            if (have_pair++ || port_pair(token + 12, 255, &proposed.rtp_channel,
                                         &proposed.rtcp_channel)) invalid = 1;
        } else if (!proposed.tcp && !strncasecmp(token, "client_port=", 12)) {
            if (have_pair++ || port_pair(token + 12, 65535, &proposed.client_rtp,
                                         &proposed.client_rtcp) || !proposed.client_rtp ||
                !proposed.client_rtcp) invalid = 1;
        } else invalid = 1;
    }
    if (proposed.tcp && !have_pair) {
        proposed.rtp_channel = (unsigned)track_id * 2;
        proposed.rtcp_channel = proposed.rtp_channel + 1;
        have_pair = 1;
    }
    rtsp_track *other = &client->tracks[1 - track_id];
    if (proposed.tcp && other->setup && other->tcp &&
        (proposed.rtp_channel == other->rtp_channel || proposed.rtp_channel == other->rtcp_channel ||
         proposed.rtcp_channel == other->rtp_channel || proposed.rtcp_channel == other->rtcp_channel))
        invalid = 1;
    if (invalid || !have_pair)
        return response(client, request->cseq, 461, "Unsupported Transport", NULL, NULL);
    if (!proposed.tcp && udp_pair(client, &proposed))
        return response(client, request->cseq, 500, "Internal Server Error", NULL, NULL);
    uint32_t seed[3];
    if (jipc_random(seed, sizeof(seed))) { close_track(&proposed); return -1; }
    proposed.sequence = (uint16_t)seed[0];
    proposed.ssrc = seed[1] ? seed[1] : 1;
    proposed.timestamp_base = seed[2];
    proposed.setup = 1;
    close_track(&client->tracks[track_id]);
    client->tracks[track_id] = proposed;
    client->channel = channel;
    snprintf(client->base_uri, sizeof(client->base_uri), "%s", base);
    char extra[1024];
    if (proposed.tcp)
        snprintf(extra, sizeof(extra), "Session: %s;timeout=120\r\n"
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\n",
                 client->session, proposed.rtp_channel, proposed.rtcp_channel, proposed.ssrc);
    else
        snprintf(extra, sizeof(extra), "Session: %s;timeout=120\r\n"
                 "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08X\r\n",
                 client->session, proposed.client_rtp, proposed.client_rtcp,
                 proposed.server_rtp, proposed.server_rtcp, proposed.ssrc);
    return response(client, request->cseq, 200, "OK", extra, NULL);
}

static int send_transport(rtsp_client *client, rtsp_track *track, int rtcp,
                          const unsigned char *packet, size_t size)
{
    if (track->tcp) {
        unsigned char interleaved[RTP_PAYLOAD_MAX + 16];
        if (size > sizeof(interleaved) - 4) return -1;
        interleaved[0] = '$';
        interleaved[1] = (unsigned char)(rtcp ? track->rtcp_channel : track->rtp_channel);
        put16(interleaved + 2, (unsigned)size);
        memcpy(interleaved + 4, packet, size);
        /* One worker serializes responses and interleaved packet boundaries. */
        return send_all(client, interleaved, size + 4);
    }
    ssize_t count;
    do count = send(rtcp ? track->rtcp_fd : track->rtp_fd, packet, size,
                    MSG_NOSIGNAL | MSG_DONTWAIT);
    while (count < 0 && errno == EINTR);
    /* A temporarily full datagram queue drops a packet without wedging a client.
     * ICMP port-unreachable is also transient during client socket startup.
     */
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED)) return 0;
    return count == (ssize_t)size ? 0 : -1;
}

static uint32_t media_timestamp(rtsp_client *client, rtsp_track *track,
                                uint64_t pts_us, unsigned rate)
{
    int64_t delta = (int64_t)pts_us - (int64_t)client->pts_origin_us;
    return track->timestamp_base + (uint32_t)(delta / 1000000 * rate +
                                             delta % 1000000 * rate / 1000000);
}

static int rtp(rtsp_client *client, rtsp_track *track, unsigned payload_type,
                uint32_t timestamp, int marker, const unsigned char *data, size_t size)
{
    unsigned char packet[RTP_PAYLOAD_MAX + 12];
    if (size > RTP_PAYLOAD_MAX) return -1;
    packet[0] = 0x80;
    packet[1] = (unsigned char)(payload_type | (marker ? 0x80u : 0));
    put16(packet + 2, track->sequence++);
    put32(packet + 4, timestamp);
    put32(packet + 8, track->ssrc);
    memcpy(packet + 12, data, size);
    if (send_transport(client, track, 0, packet, size + 12)) return -1;
    ++track->packets;
    track->octets += (uint32_t)size;
    return 0;
}

static int h264_nal(rtsp_client *client, rtsp_track *track, uint32_t timestamp,
                     int marker, const unsigned char *nal, size_t size)
{
    if (!size) return 0;
    unsigned type = nal[0] & 31u;
    if ((nal[0] & 0x80) || !type || type >= 24) return -1;
    if (size <= RTP_PAYLOAD_MAX) return rtp(client, track, 96, timestamp, marker, nal, size);
    unsigned char fragment[RTP_PAYLOAD_MAX];
    fragment[0] = (unsigned char)((nal[0] & 0xe0) | 28);
    size_t offset = 1;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > RTP_PAYLOAD_MAX - 2) chunk = RTP_PAYLOAD_MAX - 2;
        int last = offset + chunk == size;
        fragment[1] = (unsigned char)(type | (offset == 1 ? 0x80u : 0) | (last ? 0x40u : 0));
        memcpy(fragment + 2, nal + offset, chunk);
        if (rtp(client, track, 96, timestamp, last && marker, fragment, chunk + 2)) return -1;
        offset += chunk;
        if (stopped(client)) return -1;
    }
    return 0;
}

static size_t start_code(const unsigned char *data, size_t size, size_t offset, size_t *prefix)
{
    for (size_t i = offset; i + 3 <= size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { *prefix = 3; return i; }
        if (i + 4 <= size && !data[i + 2] && data[i + 3] == 1) {
            *prefix = 4; return i;
        }
    }
    *prefix = 0;
    return size;
}

static int video_frame(rtsp_client *client, const jipc_frame *frame)
{
    rtsp_track *track = &client->tracks[0];
    uint32_t timestamp = media_timestamp(client, track, frame->pts_us, 90000);
    if (frame->keyframe) {
        unsigned char sps[512], pps[512];
        size_t sps_size = sizeof(sps), pps_size = sizeof(pps);
        if (!jipc_codec_parameters(client->server->app, (unsigned)client->channel,
                                   sps, &sps_size, pps, &pps_size)) {
            if (h264_nal(client, track, timestamp, 0, sps, sps_size) ||
                h264_nal(client, track, timestamp, 0, pps, pps_size)) return -1;
        }
    }
    size_t prefix, position = start_code(frame->data, frame->size, 0, &prefix);
    if (!prefix) return h264_nal(client, track, timestamp, 1, frame->data, frame->size);
    while (prefix) {
        size_t begin = position + prefix;
        size_t next_prefix, next = start_code(frame->data, frame->size, begin, &next_prefix);
        size_t end = next;
        while (end > begin && !frame->data[end - 1]) --end;
        if (end > begin && h264_nal(client, track, timestamp, !next_prefix,
                                    frame->data + begin, end - begin)) return -1;
        position = next; prefix = next_prefix;
    }
    return 0;
}

static int audio_frame(rtsp_client *client, const jipc_frame *frame)
{
    rtsp_track *track = &client->tracks[1];
    uint32_t timestamp = media_timestamp(client, track, frame->pts_us, 8000);
    for (size_t offset = 0; offset < frame->size;) {
        size_t size = frame->size - offset;
        if (size > RTP_PAYLOAD_MAX) size = RTP_PAYLOAD_MAX;
        if (rtp(client, track, 8, timestamp + (uint32_t)offset, 0, frame->data + offset, size)) return -1;
        offset += size;
    }
    return 0;
}

static int rtcp_report(rtsp_client *client, unsigned index, int bye)
{
    rtsp_track *track = &client->tracks[index];
    if (!track->setup || !track->packets || !client->clock_started) return 0;
    struct timespec wall;
    if (clock_gettime(CLOCK_REALTIME, &wall)) return -1;
    uint64_t now = jipc_now_us();
    unsigned char packet[128] = {0};
    packet[0] = 0x80; packet[1] = 200; put16(packet + 2, 6);
    put32(packet + 4, track->ssrc);
    put32(packet + 8, (uint32_t)((uint64_t)wall.tv_sec + UINT64_C(2208988800)));
    put32(packet + 12, (uint32_t)((uint64_t)wall.tv_nsec * UINT64_C(4294967296) / 1000000000));
    uint64_t mapped_pts = client->pts_origin_us + now - client->mono_origin_us;
    put32(packet + 16, media_timestamp(client, track, mapped_pts, index ? 8000 : 90000));
    put32(packet + 20, track->packets); put32(packet + 24, track->octets);
    char cname[64];
    snprintf(cname, sizeof(cname), "jipc-%s", client->session);
    size_t cname_size = strlen(cname);
    size_t sdes_size = (8 + 2 + cname_size + 1 + 3) & ~(size_t)3;
    packet[28] = 0x81; packet[29] = 202;
    put16(packet + 30, (unsigned)(sdes_size / 4 - 1));
    put32(packet + 32, track->ssrc);
    packet[36] = 1; packet[37] = (unsigned char)cname_size;
    memcpy(packet + 38, cname, cname_size);
    size_t total = 28 + sdes_size;
    if (bye) {
        packet[total] = 0x81; packet[total + 1] = 203;
        put16(packet + total + 2, 1); put32(packet + total + 4, track->ssrc);
        total += 8;
    }
    track->last_report_us = now;
    return send_transport(client, track, 1, packet, total);
}

static void request_idr(rtsp_client *client)
{
    char error[128];
    /* The natural next IDR remains a fallback if the backend declines. */
    (void)jipc_backend_control(client->server->app, "idr", client->channel,
                               0, error, sizeof(error));
}

static int handle(rtsp_client *client, const rtsp_request *request)
{
    static const char public_methods[] =
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n";
    if (!strcmp(request->method, "OPTIONS"))
        return response(client, request->cseq, 200, "OK", public_methods, NULL);
    jipc_config config;
    jipc_config_copy(client->server->app, &config);
    if (!authenticated(client, request, &config)) return challenge(client, request->cseq, &config);
    int track;
    char base[2048];
    int channel = uri_channel(request->uri, &track, base, sizeof(base));
    if (channel < 0 || (unsigned)channel >= config.sensors * 2)
        return response(client, request->cseq, 404, "Not Found", NULL, NULL);
    if (!strcmp(request->method, "DESCRIBE"))
        return describe(client, request, channel, base, &config);
    if (!strcmp(request->method, "SETUP")) return setup(client, request, channel, track, base, &config);
    if (!strcmp(request->method, "GET_PARAMETER") && client->channel < 0)
        return response(client, request->cseq, 200, "OK", NULL, NULL);
    if (client->channel < 0 || channel != client->channel ||
        !session_matches(client, request->session))
        return response(client, request->cseq, 454, "Session Not Found", NULL, NULL);
    char extra[4600];
    snprintf(extra, sizeof(extra), "Session: %s\r\n", client->session);
    if (!strcmp(request->method, "PLAY")) {
        if (request->range[0] && strcmp(request->range, "npt=now-") &&
            strcmp(request->range, "npt=0-") && strcmp(request->range, "npt=0.000-"))
            return response(client, request->cseq, 457, "Invalid Range", NULL, NULL);
        if (!client->tracks[0].setup && !client->tracks[1].setup)
            return response(client, request->cseq, 455, "Method Not Valid in This State", NULL, NULL);
        size_t used = strlen(extra);
        int count = snprintf(extra + used, sizeof(extra) - used, "Range: npt=now-\r\nRTP-Info: ");
        if (count < 0 || (size_t)count >= sizeof(extra) - used) return -1;
        used += (size_t)count;
        int added = 0;
        for (unsigned i = 0; i < 2; ++i) if (client->tracks[i].setup) {
            count = snprintf(extra + used, sizeof(extra) - used,
                             "%surl=%strackID=%u;seq=%u", added ? "," : "",
                             client->base_uri, i, client->tracks[i].sequence);
            if (count < 0 || (size_t)count >= sizeof(extra) - used) return -1;
            used += (size_t)count; added = 1;
        }
        if (used + 3 > sizeof(extra)) return -1;
        memcpy(extra + used, "\r\n", 3);
        if (!client->playing) {
            client->need_keyframe = 1;
            /* Joining viewers should not wait an entire encoder GOP. */
            if (client->tracks[0].setup) request_idr(client);
        }
        client->playing = 1;
        return response(client, request->cseq, 200, "OK", extra, NULL);
    }
    if (!strcmp(request->method, "PAUSE")) {
        client->playing = 0;
        return response(client, request->cseq, 200, "OK", extra, NULL);
    }
    if (!strcmp(request->method, "GET_PARAMETER"))
        return response(client, request->cseq, 200, "OK", extra, NULL);
    if (!strcmp(request->method, "TEARDOWN")) {
        int result = response(client, request->cseq, 200, "OK", extra, NULL);
        rtcp_report(client, 0, 1); rtcp_report(client, 1, 1);
        client->playing = 0;
        return result ? -1 : 1;
    }
    return response(client, request->cseq, 405, "Method Not Allowed", public_methods, NULL);
}

static int process_input(rtsp_client *client)
{
    while (client->input_size) {
        size_t consumed;
        if (client->input[0] == '$') {
            if (client->input_size < 4) return 0;
            size_t length = (size_t)client->input[2] * 256 + client->input[3];
            if (length + 4 > sizeof(client->input)) return -1;
            if (client->input_size < length + 4) return 0;
            /* RTCP feedback is accepted for keepalive, but retransmission and
             * congestion-control extensions are not advertised or implemented.
             */
            unsigned id = client->input[1];
            for (unsigned i = 0; i < 2; ++i)
                if (client->tracks[i].setup && client->tracks[i].tcp &&
                    id == client->tracks[i].rtcp_channel && length >= 4 &&
                    (client->input[4] >> 6) == 2) client->last_activity_us = jipc_now_us();
            consumed = length + 4;
        } else {
            size_t header_size = 0;
            for (size_t i = 0; i + 4 <= client->input_size; ++i) {
                if (!memcmp(client->input + i, "\r\n\r\n", 4)) { header_size = i + 4; break; }
            }
            if (!header_size) {
                if (client->input_size >= RTSP_HEADER_MAX) {
                    response(client, 0, 400, "Bad Request", NULL, NULL);
                    return -1;
                }
                return 0;
            }
            if (header_size > RTSP_HEADER_MAX || memchr(client->input, '\0', header_size)) return -1;
            char header[RTSP_HEADER_MAX + 1];
            memcpy(header, client->input, header_size);
            /* Keep the final header CRLF; replace the empty line with NUL. */
            header[header_size - 2] = '\0';
            rtsp_request request;
            if (parse_request(header, &request)) {
                response(client, 0, 400, "Bad Request", NULL, NULL);
                return -1;
            }
            consumed = header_size + request.content_length;
            if (consumed > sizeof(client->input)) return -1;
            if (client->input_size < consumed) return 0;
            client->last_activity_us = jipc_now_us();
            int result = handle(client, &request);
            if (result) return -1;
        }
        client->input_size -= consumed;
        memmove(client->input, client->input + consumed, client->input_size);
    }
    return 0;
}

static int stream_frames(rtsp_client *client)
{
    if (!client->playing) return 0;
    for (unsigned track = 0; track < 2; ++track) {
        if (!client->tracks[track].setup) continue;
        unsigned channel = track ? JIPC_AUDIO_STREAM : (unsigned)client->channel;
        jipc_frame frame = {0};
        uint64_t previous = client->cursor[track];
        int result = jipc_read(client->server->app, channel, &client->cursor[track], &frame, 0);
        if (result < 0) return -1;
        if (!result) continue;
        if ((track == 0 && frame.codec != JIPC_H264) || (track == 1 && frame.codec != JIPC_PCMA)) {
            jipc_frame_free(&frame); continue;
        }
        if (!track && previous && frame.sequence != previous + 1 &&
            !frame.keyframe && !client->need_keyframe) {
            /* A slow socket can let the source overwrite our four-slot ring.
             * Sending retained P frames would preserve RTP sequence continuity
             * while silently losing decoder references. Wait for an IDR and
             * request one once, rather than emitting an undecodable GOP.
             */
            client->need_keyframe = 1;
            request_idr(client);
        }
        if (!track && client->need_keyframe) {
            if (!frame.keyframe) { jipc_frame_free(&frame); continue; }
            client->need_keyframe = 0;
        }
        if (!client->clock_started) {
            client->clock_started = 1;
            client->pts_origin_us = frame.pts_us;
            client->mono_origin_us = jipc_now_us();
        }
        result = track ? audio_frame(client, &frame) : video_frame(client, &frame);
        jipc_frame_free(&frame);
        if (result) return -1;
        if (jipc_now_us() - client->tracks[track].last_report_us > UINT64_C(5000000) &&
            rtcp_report(client, track, 0)) return -1;
    }
    return 0;
}

static void *client_main(void *argument)
{
    rtsp_client *client = argument;
    while (!stopped(client)) {
        if (jipc_now_us() - client->last_activity_us > RTSP_TIMEOUT_US) break;
        struct pollfd descriptors[3] = {{0}};
        for (unsigned i = 0; i < 3; ++i) {
            descriptors[i].fd = -1;
            descriptors[i].events = POLLIN;
        }
        descriptors[0].fd = client->fd;
        for (unsigned i = 0; i < 2; ++i)
            if (client->tracks[i].setup && !client->tracks[i].tcp)
                descriptors[i + 1].fd = client->tracks[i].rtcp_fd;
        int result = poll(descriptors, 3, client->playing ? 5 : 250);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (result > 0 && (descriptors[0].revents & (POLLERR | POLLNVAL))) break;
        if (result > 0 && (descriptors[0].revents & POLLIN)) {
            if (client->input_size == sizeof(client->input)) break;
            ssize_t count = recv(client->fd, client->input + client->input_size,
                                  sizeof(client->input) - client->input_size, 0);
            if (count <= 0) { if (count < 0 && errno == EINTR) continue; break; }
            client->input_size += (size_t)count;
            if (process_input(client)) break;
        } else if (result > 0 && (descriptors[0].revents & POLLHUP)) break;
        for (unsigned i = 1; i < 3; ++i) if (descriptors[i].revents & (POLLIN | POLLERR)) {
            unsigned char feedback[2048];
            /* Connected UDP sockets accept feedback only from the negotiated
             * peer address/port. RR reception refreshes the live session.
             */
            ssize_t count = recv(descriptors[i].fd, feedback, sizeof(feedback), MSG_DONTWAIT);
            if (count >= 4 && (feedback[0] >> 6) == 2 && feedback[1] >= 200 && feedback[1] <= 204)
                client->last_activity_us = jipc_now_us();
        }
        if (stream_frames(client)) break;
    }
    if (client->playing && !stopped(client)) {
        rtcp_report(client, 0, 1); rtcp_report(client, 1, 1);
    }
    close_track(&client->tracks[0]); close_track(&client->tracks[1]);
    pthread_mutex_lock(&client->server->mutex);
    close(client->fd);
    client->fd = -1;
    client->done = 1;
    pthread_mutex_unlock(&client->server->mutex);
    return NULL;
}

static void reap_clients(rtsp_server *server, int all)
{
    for (unsigned i = 0; i < server->capacity; ++i) {
        pthread_mutex_lock(&server->mutex);
        rtsp_client *client = server->clients[i];
        int reap = client && (all || client->done);
        pthread_mutex_unlock(&server->mutex);
        if (reap) {
            pthread_join(client->thread, NULL);
            pthread_mutex_lock(&server->mutex);
            server->clients[i] = NULL;
            pthread_mutex_unlock(&server->mutex);
            free(client);
        }
    }
}

static void *server_main(void *argument)
{
    rtsp_server *server = argument;
    while (!atomic_load(&server->stopping) && !server->app->stop) {
        reap_clients(server, 0);
        struct pollfd pollfd = {0};
        pollfd.fd = server->listener;
        pollfd.events = POLLIN;
        int result = poll(&pollfd, 1, 250);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (!result) continue;
        if (!(pollfd.revents & POLLIN)) break;
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int fd = accept(server->listener, (struct sockaddr *)&peer, &peer_size);
        if (fd < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        unsigned slot;
        for (slot = 0; slot < server->capacity; ++slot) if (!server->clients[slot]) break;
        if (slot == server->capacity || atomic_load(&server->stopping) || server->app->stop) {
            static const char busy[] = "RTSP/1.0 503 Service Unavailable\r\nCSeq: 0\r\nContent-Length: 0\r\n\r\n";
            send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            close(fd); continue;
        }
        rtsp_client *client = calloc(1, sizeof(*client));
        if (!client) { close(fd); continue; }
        client->server = server; client->fd = fd; client->peer = peer;
        client->channel = -1; client->need_keyframe = 1;
        client->last_activity_us = client->nonce_us = jipc_now_us();
        for (unsigned i = 0; i < 2; ++i)
            client->tracks[i].rtp_fd = client->tracks[i].rtcp_fd = -1;
        if (random_hex(client->nonce, 16) || random_hex(client->session, 16)) {
            close(fd); free(client); continue;
        }
        pthread_mutex_lock(&server->mutex);
        server->clients[slot] = client;
        pthread_attr_t attributes;
        int attr_ready = !pthread_attr_init(&attributes);
        int create_result = -1;
        if (attr_ready && !pthread_attr_setstacksize(&attributes, 256u * 1024u))
            create_result = pthread_create(&client->thread, &attributes, client_main, client);
        if (attr_ready) pthread_attr_destroy(&attributes);
        if (create_result) {
            server->clients[slot] = NULL;
            close(fd); free(client);
        }
        pthread_mutex_unlock(&server->mutex);
    }
    atomic_store(&server->stopping, 1);
    pthread_mutex_lock(&server->mutex);
    for (unsigned i = 0; i < server->capacity; ++i)
        if (server->clients[i] && server->clients[i]->fd >= 0)
            shutdown(server->clients[i]->fd, SHUT_RDWR);
    pthread_mutex_unlock(&server->mutex);
    reap_clients(server, 1);
    return NULL;
}

int jipc_rtsp_start(jipc_app *app)
{
    jipc_config config;
    jipc_config_copy(app, &config);
    pthread_mutex_lock(&server_mutex);
    if (app->rtsp_state || !config.max_clients || config.max_clients > RTSP_CLIENT_LIMIT) {
        pthread_mutex_unlock(&server_mutex); return -1;
    }
    rtsp_server *server = calloc(1, sizeof(*server));
    if (!server) { pthread_mutex_unlock(&server_mutex); return -1; }
    server->listener = -1; server->app = app; server->capacity = config.max_clients;
    server->clients = calloc(server->capacity, sizeof(*server->clients));
    if (!server->clients) { free(server); pthread_mutex_unlock(&server_mutex); return -1; }
    if (pthread_mutex_init(&server->mutex, NULL)) {
        free(server->clients); free(server); pthread_mutex_unlock(&server_mutex); return -1;
    }
    atomic_init(&server->stopping, 0);
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    if (server->listener >= 0)
        setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config.rtsp_port);
    if (server->listener < 0 || inet_pton(AF_INET, config.bind, &address.sin_addr) != 1 ||
        bind(server->listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(server->listener, (int)server->capacity) ||
        pthread_create(&app->rtsp_thread, NULL, server_main, server)) {
        if (server->listener >= 0) close(server->listener);
        pthread_mutex_destroy(&server->mutex); free(server->clients); free(server);
        pthread_mutex_unlock(&server_mutex); return -1;
    }
    app->rtsp_state = server;
    app->rtsp_started = 1;
    pthread_mutex_unlock(&server_mutex);
    jipc_log("INFO", "RTSP listening on %s:%u", config.bind, config.rtsp_port);
    return 0;
}

void jipc_rtsp_stop(jipc_app *app)
{
    pthread_mutex_lock(&server_mutex);
    rtsp_server *server = app->rtsp_state;
    if (!server || server->app != app) { pthread_mutex_unlock(&server_mutex); return; }
    atomic_store(&server->stopping, 1);
    shutdown(server->listener, SHUT_RDWR);
    pthread_mutex_lock(&server->mutex);
    for (unsigned i = 0; i < server->capacity; ++i)
        if (server->clients[i] && server->clients[i]->fd >= 0)
            shutdown(server->clients[i]->fd, SHUT_RDWR);
    pthread_mutex_unlock(&server->mutex);
    pthread_join(app->rtsp_thread, NULL);
    close(server->listener);
    pthread_mutex_destroy(&server->mutex);
    free(server->clients); free(server);
    app->rtsp_state = NULL;
    app->rtsp_started = 0;
    pthread_mutex_unlock(&server_mutex);
}
