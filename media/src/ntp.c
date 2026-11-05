#define _GNU_SOURCE
#include "jipc.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The board has no RTC: the retained controller restores a coarse saved time
 * at boot, which can be hours behind. WSSE clients such as Frigate measure the
 * camera offset once, so a clock that changes across camera reboots rejects
 * their later requests. A local SNTP server keeps the clock on real UTC. */
#define NTP_UNIX_EPOCH 2208988800ULL
#define NTP_TIMEOUT_MS 2000u
#define NTP_RETRY_MS 30000u
#define NTP_POLL_MS 3600000u
#define NTP_STEP_US 1000000LL
/* Reject replies claiming a time before this release could have been built. */
#define NTP_MIN_UNIX 1767225600LL /* 2026-01-01T00:00:00Z */

typedef struct {
    jipc_app *app;
    struct sockaddr_in server;
    char name[64];
    int wake[2];
    atomic_int quitting;
    pthread_mutex_t mutex;
    int mutex_ready;
    unsigned attempts;
    int synchronized, ever;
    uint64_t last_sync_us;
    int64_t last_offset_us;
    char error[128];
} ntp_state;

int jipc_ntp_server(const char *text, uint32_t *address, uint16_t *port)
{
    char host[INET_ADDRSTRLEN]; unsigned long number = 123;
    const char *colon = strchr(text, ':');
    size_t length = colon ? (size_t)(colon - text) : strlen(text);
    if (!length || length >= sizeof(host)) return -1;
    memcpy(host, text, length); host[length] = 0;
    if (colon) {
        char *end; errno = 0;
        if (colon[1] < '0' || colon[1] > '9') return -1;
        number = strtoul(colon + 1, &end, 10);
        if (errno || *end || !number || number > 65535) return -1;
    }
    struct in_addr parsed;
    if (inet_pton(AF_INET, host, &parsed) != 1 || parsed.s_addr == htonl(INADDR_ANY)) return -1;
    *address = ntohl(parsed.s_addr); *port = (uint16_t)number;
    return 0;
}
static uint64_t read64(const unsigned char *p)
{ uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) value = value << 8 | p[i]; return value; }
/* NTP era 0 ends in 2036; a clear top bit means era 1 (valid 1968..2104). */
static int64_t unix_us(uint64_t stamp)
{
    uint64_t seconds = stamp >> 32, fraction = stamp & 0xffffffffu;
    if (!(seconds & 0x80000000u)) seconds += 1ULL << 32;
    return (int64_t)(seconds - NTP_UNIX_EPOCH) * 1000000 + (int64_t)((fraction * 1000000u + (1u << 31)) >> 32);
}
int jipc_ntp_offset(const unsigned char *reply, size_t size, const unsigned char origin[8],
                    int64_t sent_us, uint64_t round_trip_us, int64_t *offset_us)
{
    if (size < 48) return -1;
    unsigned leap = reply[0] >> 6, version = (reply[0] >> 3) & 7, mode = reply[0] & 7, stratum = reply[1];
    /* Leap 3 and stratum 0/16 are unsynchronized servers or kiss-o'-death. */
    if (leap == 3 || version < 3 || version > 4 || mode != 4 || stratum < 1 || stratum > 15) return -1;
    /* The random origin cookie rejects off-path and stale replies. */
    if (memcmp(reply + 24, origin, 8)) return -1;
    uint64_t received = read64(reply + 32), transmitted = read64(reply + 40);
    if (!received || !transmitted) return -1;
    int64_t t2 = unix_us(received), t3 = unix_us(transmitted), t4 = sent_us + (int64_t)round_trip_us;
    if (t2 < NTP_MIN_UNIX * 1000000 || t3 < t2 || (uint64_t)(t3 - t2) > round_trip_us) return -1;
    *offset_us = ((t2 - sent_us) + (t3 - t4)) / 2;
    return 0;
}
/* Wait on the wake pipe; returns nonzero once the service is stopping. */
static int idle(ntp_state *s, unsigned ms)
{
    uint64_t until = jipc_now_us() + (uint64_t)ms * 1000u;
    while (!atomic_load(&s->quitting) && !s->app->stop) {
        uint64_t now = jipc_now_us(); if (now >= until) return 0;
        struct pollfd p = { s->wake[0], POLLIN, 0 };
        uint64_t left = (until - now + 999) / 1000; if (left > 1000) left = 1000;
        if (poll(&p, 1, (int)left) > 0) return 1;
    }
    return 1;
}
static int exchange(ntp_state *s, int64_t *offset_us, char *error, size_t capacity)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = -1, invalid = 0;
    if (fd < 0) { snprintf(error, capacity, "socket: %s", strerror(errno)); return -1; }
    unsigned char origin[8], request[48] = {0}, reply[68];
    struct timespec wall;
    if (connect(fd, (struct sockaddr *)&s->server, sizeof(s->server))) { snprintf(error, capacity, "connect: %s", strerror(errno)); goto done; }
    if (jipc_random(origin, sizeof(origin))) { snprintf(error, capacity, "random origin unavailable"); goto done; }
    request[0] = 0x23; /* leap 0, version 4, client mode */
    memcpy(request + 40, origin, sizeof(origin));
    if (clock_gettime(CLOCK_REALTIME, &wall)) { snprintf(error, capacity, "clock unavailable"); goto done; }
    int64_t sent_us = (int64_t)wall.tv_sec * 1000000 + wall.tv_nsec / 1000;
    uint64_t sent = jipc_now_us(), deadline = sent + NTP_TIMEOUT_MS * 1000u;
    if (send(fd, request, sizeof(request), 0) != (ssize_t)sizeof(request)) { snprintf(error, capacity, "send: %s", strerror(errno)); goto done; }
    for (;;) {
        uint64_t now = jipc_now_us();
        if (now >= deadline) { snprintf(error, capacity, invalid ? "no valid reply" : "no reply"); break; }
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { s->wake[0], POLLIN, 0 } };
        int ready = poll(p, 2, (int)((deadline - now + 999) / 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { snprintf(error, capacity, "poll: %s", strerror(errno)); break; }
        if (p[1].revents) { snprintf(error, capacity, "stopping"); break; }
        if (!ready) continue;
        ssize_t n = recv(fd, reply, sizeof(reply), 0);
        uint64_t round_trip = jipc_now_us() - sent;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            snprintf(error, capacity, "receive: %s", strerror(errno)); break;
        }
        if (!jipc_ntp_offset(reply, (size_t)n, origin, sent_us, round_trip, offset_us)) { rc = 0; break; }
        invalid = 1; /* ignore it and keep waiting for the real reply */
    }
done:
    close(fd); return rc;
}
static int step_clock(int64_t offset_us, char *error, size_t capacity)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) { snprintf(error, capacity, "clock unavailable"); return -1; }
    int64_t target = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000 + offset_us;
    struct timespec next = { (time_t)(target / 1000000), (long)(target % 1000000) * 1000 };
    if (clock_settime(CLOCK_REALTIME, &next)) { snprintf(error, capacity, "clock step: %s", strerror(errno)); return -1; }
    return 0;
}
static void *ntp_main(void *opaque)
{
    ntp_state *s = opaque; int failing = 0;
    while (!atomic_load(&s->quitting) && !s->app->stop) {
        int64_t offset = 0; char error[128] = {0};
        int ok = !exchange(s, &offset, error, sizeof(error)), stepped = 0;
        if (atomic_load(&s->quitting) || s->app->stop) break;
        if (ok && (offset >= NTP_STEP_US || offset <= -NTP_STEP_US)) {
            if (step_clock(offset, error, sizeof(error))) ok = 0; else stepped = 1;
        }
        pthread_mutex_lock(&s->mutex);
        ++s->attempts; s->synchronized = ok;
        if (ok) { s->ever = 1; s->last_sync_us = jipc_now_us(); s->last_offset_us = offset; s->error[0] = 0; }
        else snprintf(s->error, sizeof(s->error), "%s", error);
        pthread_mutex_unlock(&s->mutex);
        if (stepped) jipc_log("info", "clock stepped %+.3f s from NTP server %s", offset / 1e6, s->name);
        else if (ok && failing) jipc_log("info", "NTP server %s reachable again (offset %+.3f s)", s->name, offset / 1e6);
        else if (!ok && !failing) jipc_log("warning", "NTP server %s: %s; retrying every %u s", s->name, error, NTP_RETRY_MS / 1000);
        failing = !ok;
        if (idle(s, ok ? NTP_POLL_MS : NTP_RETRY_MS)) break;
    }
    return NULL;
}
static void ntp_release(ntp_state *s)
{
    if (s->wake[0] >= 0) close(s->wake[0]);
    if (s->wake[1] >= 0) close(s->wake[1]);
    if (s->mutex_ready) pthread_mutex_destroy(&s->mutex);
    free(s);
}
int jipc_ntp_start(jipc_app *app)
{
    if (app->ntp_started || app->ntp_state) return -1;
    jipc_config config; jipc_config_copy(app, &config);
    if (!config.ntp_server[0]) return 0;
    uint32_t address; uint16_t port;
    if (jipc_ntp_server(config.ntp_server, &address, &port)) return -1;
    ntp_state *s = calloc(1, sizeof(*s)); if (!s) return -1;
    s->app = app; s->wake[0] = s->wake[1] = -1; atomic_init(&s->quitting, 0);
    snprintf(s->name, sizeof(s->name), "%s", config.ntp_server);
    s->server.sin_family = AF_INET; s->server.sin_addr.s_addr = htonl(address); s->server.sin_port = htons(port);
    if (pthread_mutex_init(&s->mutex, NULL)) { ntp_release(s); return -1; }
    s->mutex_ready = 1;
    if (pipe(s->wake)) { ntp_release(s); return -1; }
    for (unsigned i = 0; i < 2; ++i) {
        int flags = fcntl(s->wake[i], F_GETFL, 0);
        if (flags < 0 || fcntl(s->wake[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->wake[i], F_SETFD, FD_CLOEXEC) < 0) { ntp_release(s); return -1; }
    }
    if (pthread_create(&app->ntp_thread, NULL, ntp_main, s)) { ntp_release(s); return -1; }
    app->ntp_state = s; app->ntp_started = 1;
    jipc_log("info", "NTP client for %s", s->name);
    return 0;
}
void jipc_ntp_stop(jipc_app *app)
{
    ntp_state *s = app->ntp_state; if (!s) return;
    atomic_store(&s->quitting, 1);
    unsigned char byte = 1; ssize_t ignored = write(s->wake[1], &byte, 1); (void)ignored;
    if (app->ntp_started) pthread_join(app->ntp_thread, NULL);
    app->ntp_state = NULL; app->ntp_started = 0;
    ntp_release(s);
}
int jipc_ntp_status_json(jipc_app *app, char *out, size_t capacity)
{
    ntp_state *s = app->ntp_state; cJSON *o = cJSON_CreateObject(); if (!o) return -1;
    if (!s) cJSON_AddStringToObject(o, "source", "none");
    else {
        pthread_mutex_lock(&s->mutex);
        cJSON_AddStringToObject(o, "source", "ntp");
        cJSON_AddStringToObject(o, "server", s->name);
        cJSON_AddBoolToObject(o, "synchronized", s->synchronized);
        cJSON_AddNumberToObject(o, "attempts", s->attempts);
        if (s->ever) {
            cJSON_AddNumberToObject(o, "last_offset_ms", (double)s->last_offset_us / 1000.0);
            cJSON_AddNumberToObject(o, "last_sync_age_seconds", (double)((jipc_now_us() - s->last_sync_us) / 1000000u));
        }
        cJSON_AddStringToObject(o, "error", s->error);
        pthread_mutex_unlock(&s->mutex);
    }
    char *text = cJSON_PrintUnformatted(o); int rc = -1;
    if (text && strlen(text) < capacity) { strcpy(out, text); rc = (int)strlen(text); }
    cJSON_free(text); cJSON_Delete(o); return rc;
}
