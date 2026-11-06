/* SPDX-License-Identifier: MIT
 * Loopback client for the platform daemon (joan-daemon, port 80), so the
 * media daemon's HTTPS UI signs in once with the platform admin account:
 * /api/joan/<rest> is relayed to http://127.0.0.1/api/<rest>, and the
 * platform session cookie also authorizes the media API.
 */
#define _GNU_SOURCE
#include "jipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define JOAN_TIMEOUT_S 15
#define SESSION_CACHE 8
#define SESSION_CACHE_US UINT64_C(3000000)

static int joan_port(void)
{
    /* A test hook, never a camera.ini key: older binaries reject new keys. */
    const char *value = getenv("JIPC_JOAN_PORT");
    long port = value && *value ? strtol(value, NULL, 10) : 80;
    return port > 0 && port < 65536 ? (int)port : 80;
}

size_t jipc_joan_cookies(const char *header, char *out, size_t capacity)
{
    /* Only the platform's session cookie crosses the relay; siblings stay
     * (the platform picks the live one). */
    size_t used = 0;
    if (capacity) out[0] = 0;
    for (const char *p = header; p && *p;) {
        while (*p == ' ' || *p == ';') ++p;
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > 13 && !strncmp(p, "joan_session=", 13)) {
            if (used + n + 3 > capacity) break;
            if (used) { memcpy(out + used, "; ", 2); used += 2; }
            memcpy(out + used, p, n); used += n; out[used] = 0;
        }
        p = end ? end + 1 : p + n;
    }
    return used;
}

void jipc_joan_reply_free(jipc_joan_reply *reply)
{
    free(reply->raw);
    memset(reply, 0, sizeof *reply);
}

static int send_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; size -= (size_t)n;
    }
    return 0;
}

int jipc_joan_request(const char *method, const char *path, const char *type,
                      const char *cookies, const char *csrf, const char *body,
                      size_t length, jipc_joan_reply *reply)
{
    return jipc_joan_stream(method, path, type, cookies, csrf, body, length, length, NULL, NULL, reply);
}

int jipc_joan_stream(const char *method, const char *path, const char *type,
                     const char *cookies, const char *csrf, const char *body,
                     size_t length, size_t total, jipc_joan_pull pull, void *context,
                     jipc_joan_reply *reply)
{
    memset(reply, 0, sizeof *reply);
    if (length > total || (length < total && !pull)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    /* An upload's reply waits for the platform to verify the package, and
     * joining a network or changing the address for the Wi-Fi trial to start. */
    int slow = !strcmp(method, "POST") && !strcmp(path, "/api/v1/network/wifi");
    slow = slow || (!strcmp(method, "PUT") && !strcmp(path, "/api/v1/network/ip"));
    struct timeval timeout = {pull || slow ? 5 * JOAN_TIMEOUT_S : JOAN_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)joan_port()),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&address, sizeof address)) { close(fd); return -1; }
    /* No Origin: the platform accepts loopback requests without one; the
     * media server checked the browser's Origin before relaying. */
    char head[2600];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                     "%s%s%s%s%s%s%s%s%sContent-Length: %zu\r\n\r\n",
                     method, path,
                     type && *type ? "Content-Type: " : "", type && *type ? type : "", type && *type ? "\r\n" : "",
                     cookies && *cookies ? "Cookie: " : "", cookies && *cookies ? cookies : "", cookies && *cookies ? "\r\n" : "",
                     csrf && *csrf ? "X-CSRF-Token: " : "", csrf && *csrf ? csrf : "", csrf && *csrf ? "\r\n" : "",
                     total);
    if (n < 0 || (size_t)n >= sizeof head || send_all(fd, head, (size_t)n) ||
        (length && send_all(fd, body, length))) { close(fd); return -1; }
    if (pull) {
        /* The rest of the body comes from the browser in pieces. */
        char piece[16384];
        for (size_t sent = length; sent < total;) {
            size_t want = total - sent < sizeof piece ? total - sent : sizeof piece;
            ssize_t got = pull(context, piece, want);
            if (got <= 0 || send_all(fd, piece, (size_t)got)) { close(fd); return -3; }
            sent += (size_t)got;
        }
    }
    size_t capacity = JIPC_JOAN_REPLY_LIMIT, got = 0;
    char *raw = malloc(capacity + 1);
    if (!raw) { close(fd); return -1; }
    for (;;) {
        if (got == capacity) { free(raw); close(fd); return -2; } /* reply too large */
        ssize_t r = recv(fd, raw + got, capacity - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(raw); close(fd); return -1; }
        if (!r) break;
        got += (size_t)r;
    }
    close(fd);
    raw[got] = 0;
    char *end = strstr(raw, "\r\n\r\n");
    int status = 0;
    if (!end || sscanf(raw, "HTTP/1.%*1[01] %3d", &status) != 1 || status < 100 || status > 599) { free(raw); return -1; }
    reply->raw = raw;
    reply->status = status;
    reply->body = end + 4;
    reply->length = got - (size_t)(reply->body - raw);
    *end = 0;
    for (char *line = strstr(raw, "\r\n"); line && line < end;) {
        line += 2;
        char *next = strstr(line, "\r\n");
        if (next) *next = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            char *value = colon + 1;
            while (*value == ' ') ++value;
            if (!strcasecmp(line, "Content-Type")) snprintf(reply->type, sizeof reply->type, "%s", value);
            else if (!strcasecmp(line, "Content-Length")) {
                char *stop; unsigned long declared = strtoul(value, &stop, 10);
                if (*stop || declared > reply->length) { jipc_joan_reply_free(reply); return -1; }
                reply->length = declared;
            } else if (!strcasecmp(line, "Set-Cookie") && reply->cookies < 2 && !strncmp(value, "joan_session=", 13))
                snprintf(reply->set_cookie[reply->cookies++], sizeof reply->set_cookie[0], "%s", value);
        }
        if (!next) break;
        line = next;
    }
    return 0;
}

/* A few seconds of cache keeps status polling from costing a loopback call
 * each time; a logout reaches the media API within that window. */
typedef struct { char key[41], csrf[96]; uint64_t until; } session_entry;
static session_entry cache[SESSION_CACHE];
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

int jipc_joan_session(const char *cookie_header, char *csrf, size_t capacity)
{
    char cookies[1100];
    if (!jipc_joan_cookies(cookie_header, cookies, sizeof cookies)) return 0;
    unsigned char digest[20]; char key[41];
    jipc_sha1(cookies, strlen(cookies), digest);
    for (unsigned i = 0; i < 20; ++i) snprintf(key + i * 2, 3, "%02x", digest[i]);
    uint64_t now = jipc_now_us();
    pthread_mutex_lock(&cache_mutex);
    for (unsigned i = 0; i < SESSION_CACHE; ++i)
        if (cache[i].until > now && !strcmp(cache[i].key, key)) {
            snprintf(csrf, capacity, "%s", cache[i].csrf);
            pthread_mutex_unlock(&cache_mutex);
            return 1;
        }
    pthread_mutex_unlock(&cache_mutex);
    jipc_joan_reply reply;
    int rc = jipc_joan_request("GET", "/api/v1/session", NULL, cookies, NULL, NULL, 0, &reply);
    if (rc) return -1;
    int valid = reply.status == 200 &&
                jipc_json_string(reply.body, "csrf", csrf, capacity) == 0 && csrf[0];
    int unavailable = reply.status >= 500;
    jipc_joan_reply_free(&reply);
    if (!valid) return unavailable ? -1 : 0;
    pthread_mutex_lock(&cache_mutex);
    unsigned slot = 0;
    for (unsigned i = 1; i < SESSION_CACHE; ++i) if (cache[i].until < cache[slot].until) slot = i;
    snprintf(cache[slot].key, sizeof cache[slot].key, "%s", key);
    snprintf(cache[slot].csrf, sizeof cache[slot].csrf, "%s", csrf);
    cache[slot].until = now + SESSION_CACHE_US;
    pthread_mutex_unlock(&cache_mutex);
    return 1;
}

void jipc_joan_forget(const char *cookie_header)
{
    char cookies[1100];
    if (!jipc_joan_cookies(cookie_header, cookies, sizeof cookies)) return;
    unsigned char digest[20]; char key[41];
    jipc_sha1(cookies, strlen(cookies), digest);
    for (unsigned i = 0; i < 20; ++i) snprintf(key + i * 2, 3, "%02x", digest[i]);
    pthread_mutex_lock(&cache_mutex);
    for (unsigned i = 0; i < SESSION_CACHE; ++i)
        if (!strcmp(cache[i].key, key)) memset(&cache[i], 0, sizeof cache[i]);
    pthread_mutex_unlock(&cache_mutex);
}
