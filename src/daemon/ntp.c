#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The board has no RTC: boot restores the coarse last-time, which lags real
 * time by the downtime. WSSE clients such as Frigate measure the camera's
 * offset once, so every reboot used to break them until they restarted. An
 * SNTP server on the camera's own network, normally its gateway, keeps the
 * clock on UTC. Steps go through the privileged time-set helper, which also
 * persists last-time, so the next boot starts close to real time. */
#define NTP_UNIX_EPOCH 2208988800ULL
#define NTP_TIMEOUT_MS 2000u
#define NTP_RETRY_MS 30000u
#define NTP_POLL_MS 3600000u
/* time-set works in whole seconds; a smaller threshold would chase rounding. */
#define NTP_STEP_US 2000000LL
/* Reject replies claiming a time before this release could have been built. */
#define NTP_MIN_UNIX 1767225600LL /* 2026-01-01T00:00:00Z */

static struct {
    const JoanConfig *cfg;
    pthread_t thread;
    int started, wake[2];
    volatile int quitting;
    pthread_mutex_t lock;
    char server[64], resolved[64], error[128];
    unsigned attempts;
    int synchronized, ever;
    uint64_t last_sync_ms;
    int64_t last_offset_us;
} N = { .wake = { -1, -1 }, .lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t mono_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
static uint64_t mono_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

int joan_ntp_parse_server(const char *text, uint32_t *address, uint16_t *port)
{
    char host[INET_ADDRSTRLEN]; unsigned long number = 123; struct in_addr parsed;
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
    if (inet_pton(AF_INET, host, &parsed) != 1 || parsed.s_addr == htonl(INADDR_ANY)) return -1;
    *address = ntohl(parsed.s_addr); *port = (uint16_t)number;
    return 0;
}
static uint64_t read64(const unsigned char *p)
{ uint64_t value = 0; unsigned i; for (i = 0; i < 8; ++i) value = value << 8 | p[i]; return value; }
/* NTP era 0 ends in 2036; a clear top bit means era 1 (valid 1968..2104). */
static int64_t unix_us(uint64_t stamp)
{
    uint64_t seconds = stamp >> 32, fraction = stamp & 0xffffffffu;
    if (!(seconds & 0x80000000u)) seconds += 1ULL << 32;
    return (int64_t)(seconds - NTP_UNIX_EPOCH) * 1000000 + (int64_t)((fraction * 1000000u + (1u << 31)) >> 32);
}
int joan_ntp_offset(const unsigned char *reply, size_t size, const unsigned char origin[8],
                    int64_t sent_us, uint64_t round_trip_us, int64_t *offset_us)
{
    unsigned leap, version, mode, stratum; uint64_t received, transmitted; int64_t t2, t3, t4;
    if (size < 48) return -1;
    leap = reply[0] >> 6; version = (reply[0] >> 3) & 7; mode = reply[0] & 7; stratum = reply[1];
    /* Leap 3 and stratum 0/16 are unsynchronized servers or kiss-o'-death. */
    if (leap == 3 || version < 3 || version > 4 || mode != 4 || stratum < 1 || stratum > 15) return -1;
    /* The random origin cookie rejects off-path and stale replies. */
    if (memcmp(reply + 24, origin, 8)) return -1;
    received = read64(reply + 32); transmitted = read64(reply + 40);
    if (!received || !transmitted) return -1;
    t2 = unix_us(received); t3 = unix_us(transmitted); t4 = sent_us + (int64_t)round_trip_us;
    if (t2 < NTP_MIN_UNIX * 1000000 || t3 < t2 || (uint64_t)(t3 - t2) > round_trip_us) return -1;
    *offset_us = ((t2 - sent_us) + (t3 - t4)) / 2;
    return 0;
}

static void load_setting(void)
{
    char path[512]; unsigned char *raw = NULL; size_t n = 0;
    N.server[0] = 0;
    snprintf(path, sizeof(path), "%s/ntp-server", N.cfg->state_dir);
    if (joan_read_file(path, &raw, &n, 128)) return;
    while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) n--;
    if (n < sizeof(N.server)) { memcpy(N.server, raw, n); N.server[n] = 0; }
    free(raw);
}
/* "gateway" follows the supervisor's current default route. */
static int resolve(const char *server, struct sockaddr_in *out, char *name, size_t cap)
{
    char text[64]; uint32_t address; uint16_t port;
    if (!strcmp(server, "gateway")) {
        char path[512]; unsigned char *raw = NULL; size_t n = 0, i = 0;
        snprintf(path, sizeof(path), "%s/default-gateway", N.cfg->run_dir);
        if (joan_read_file(path, &raw, &n, 128) || !n) { free(raw); return -1; }
        while (i < n && i + 1 < sizeof(text) && raw[i] != ' ' && raw[i] != '\n') { text[i] = (char)raw[i]; i++; }
        text[i] = 0; free(raw);
    } else snprintf(text, sizeof(text), "%s", server);
    if (joan_ntp_parse_server(text, &address, &port)) return -1;
    memset(out, 0, sizeof(*out)); out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(address); out->sin_port = htons(port);
    snprintf(name, cap, "%s", text);
    return 0;
}
static int exchange(const struct sockaddr_in *server, int64_t *offset_us, char *error, size_t cap)
{
    unsigned char origin[8], request[48], reply[68]; struct timespec wall;
    int fd = socket(AF_INET, SOCK_DGRAM, 0), rc = -1, invalid = 0;
    int64_t sent_us; uint64_t sent, deadline;
    if (fd < 0) { snprintf(error, cap, "socket: %s", strerror(errno)); return -1; }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(request, 0, sizeof(request));
    if (connect(fd, (const struct sockaddr *)server, sizeof(*server))) { snprintf(error, cap, "connect: %s", strerror(errno)); goto done; }
    if (joan_random(origin, sizeof(origin))) { snprintf(error, cap, "random origin unavailable"); goto done; }
    request[0] = 0x23; /* leap 0, version 4, client mode */
    memcpy(request + 40, origin, sizeof(origin));
    if (clock_gettime(CLOCK_REALTIME, &wall)) { snprintf(error, cap, "clock unavailable"); goto done; }
    sent_us = (int64_t)wall.tv_sec * 1000000 + wall.tv_nsec / 1000;
    sent = mono_us(); deadline = sent + NTP_TIMEOUT_MS * 1000u;
    if (send(fd, request, sizeof(request), 0) != (ssize_t)sizeof(request)) { snprintf(error, cap, "send: %s", strerror(errno)); goto done; }
    for (int watch_wake = 1;;) {
        struct pollfd p[2]; int ready; ssize_t n; uint64_t now = mono_us();
        if (now >= deadline) { snprintf(error, cap, invalid ? "no valid reply" : "no reply"); break; }
        p[0].fd = fd; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = watch_wake ? N.wake[0] : -1; p[1].events = POLLIN; p[1].revents = 0;
        ready = poll(p, 2, (int)((deadline - now + 999) / 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { snprintf(error, cap, "poll: %s", strerror(errno)); break; }
        if (N.quitting) { snprintf(error, cap, "stopping"); break; }
        /* A configuration change is left in the pipe for idle() to act on. */
        if (p[1].revents) watch_wake = 0;
        if (!(p[0].revents & POLLIN)) continue;
        n = recv(fd, reply, sizeof(reply), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            snprintf(error, cap, "receive: %s", strerror(errno)); break;
        }
        if (!joan_ntp_offset(reply, (size_t)n, origin, sent_us, mono_us() - sent, offset_us)) { rc = 0; break; }
        invalid = 1; /* ignore it and keep waiting for the real reply */
    }
done:
    close(fd); return rc;
}
static int step_clock(int64_t offset_us, char *error, size_t cap)
{
    struct timespec now; char epoch[24];
    if (clock_gettime(CLOCK_REALTIME, &now)) { snprintf(error, cap, "clock unavailable"); return -1; }
    snprintf(epoch, sizeof(epoch), "%lld",
             (long long)(((int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000 + offset_us + 500000) / 1000000));
    if (joan_run_helper(N.cfg, "time-set", NULL, epoch, NULL, NULL, 10)) { snprintf(error, cap, "time-set helper failed"); return -1; }
    return 0;
}
/* Sleep until the deadline, a configuration change or shutdown. */
static void idle(unsigned ms)
{
    uint64_t until = mono_ms() + ms; unsigned char drain[16];
    while (!N.quitting) {
        struct pollfd p; uint64_t now = mono_ms(), left;
        if (now >= until) return;
        left = until - now; if (left > 60000u) left = 60000u;
        p.fd = N.wake[0]; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, (int)left) > 0) { while (read(N.wake[0], drain, sizeof(drain)) > 0) {} return; }
    }
}
static void *ntp_main(void *unused)
{
    int failing = 0; (void)unused;
    while (!N.quitting) {
        char server[64], name[64] = "", error[128] = ""; struct sockaddr_in address;
        int64_t offset = 0; int ok, stepped = 0;
        pthread_mutex_lock(&N.lock); snprintf(server, sizeof(server), "%s", N.server); pthread_mutex_unlock(&N.lock);
        if (!server[0]) { failing = 0; idle(NTP_POLL_MS); continue; }
        if (resolve(server, &address, name, sizeof(name))) { ok = 0; snprintf(error, sizeof(error), "no address for %s", server); }
        else ok = !exchange(&address, &offset, error, sizeof(error));
        if (N.quitting) break;
        if (ok && (offset >= NTP_STEP_US || offset <= -NTP_STEP_US)) {
            if (step_clock(offset, error, sizeof(error))) ok = 0; else stepped = 1;
        }
        pthread_mutex_lock(&N.lock);
        if (!strcmp(server, N.server)) { /* a concurrent change restarts from scratch */
            N.attempts++; N.synchronized = ok; snprintf(N.resolved, sizeof(N.resolved), "%s", name);
            if (ok) { N.ever = 1; N.last_sync_ms = mono_ms(); N.last_offset_us = offset; N.error[0] = 0; }
            else snprintf(N.error, sizeof(N.error), "%s", error);
        }
        pthread_mutex_unlock(&N.lock);
        if (stepped) fprintf(stderr, "clock stepped %+.3f s from NTP server %s\n", (double)offset / 1e6, name);
        else if (ok && failing) fprintf(stderr, "NTP server %s reachable again (offset %+.3f s)\n", name, (double)offset / 1e6);
        else if (!ok && !failing) fprintf(stderr, "NTP %s: %s; retrying every %u s\n", server, error, NTP_RETRY_MS / 1000);
        failing = !ok;
        idle(ok ? NTP_POLL_MS : NTP_RETRY_MS);
    }
    return NULL;
}

int joan_ntp_start(const JoanConfig *cfg)
{
    int i;
    if (N.started) return -1;
    N.cfg = cfg; N.quitting = 0;
    load_setting();
    if (pipe(N.wake)) return -1;
    for (i = 0; i < 2; ++i) {
        int flags = fcntl(N.wake[i], F_GETFL, 0);
        if (flags < 0 || fcntl(N.wake[i], F_SETFL, flags | O_NONBLOCK) || fcntl(N.wake[i], F_SETFD, FD_CLOEXEC)) {
            close(N.wake[0]); close(N.wake[1]); N.wake[0] = N.wake[1] = -1; return -1;
        }
    }
    if (pthread_create(&N.thread, NULL, ntp_main, NULL)) { close(N.wake[0]); close(N.wake[1]); N.wake[0] = N.wake[1] = -1; return -1; }
    N.started = 1;
    if (N.server[0]) fprintf(stderr, "NTP client for %s\n", N.server);
    return 0;
}
void joan_ntp_stop(void)
{
    unsigned char byte = 1;
    if (!N.started) return;
    N.quitting = 1;
    (void)write(N.wake[1], &byte, 1);
    pthread_join(N.thread, NULL);
    close(N.wake[0]); close(N.wake[1]); N.wake[0] = N.wake[1] = -1; N.started = 0;
}
int joan_ntp_configure(const char *server)
{
    char path[512], line[72]; uint32_t address; uint16_t port; unsigned char byte = 1; int n;
    if (!N.cfg || strlen(server) >= sizeof(N.server)) return -1;
    if (server[0] && strcmp(server, "gateway") && joan_ntp_parse_server(server, &address, &port)) return -1;
    snprintf(path, sizeof(path), "%s/ntp-server", N.cfg->state_dir);
    if (server[0]) {
        n = snprintf(line, sizeof(line), "%s\n", server);
        if (joan_write_atomic(path, line, (size_t)n, 0600)) return -1;
    } else if (unlink(path) && errno != ENOENT) return -1;
    pthread_mutex_lock(&N.lock);
    snprintf(N.server, sizeof(N.server), "%s", server);
    N.attempts = 0; N.synchronized = 0; N.ever = 0; N.resolved[0] = 0; N.error[0] = 0;
    pthread_mutex_unlock(&N.lock);
    if (N.started) (void)write(N.wake[1], &byte, 1);
    return 0;
}
void joan_ntp_status(char *out, size_t cap)
{
    pthread_mutex_lock(&N.lock);
    if (!N.server[0]) snprintf(out, cap, "{\"server\":\"\",\"synchronized\":false}");
    else if (N.ever)
        snprintf(out, cap, "{\"server\":\"%s\",\"resolved\":\"%s\",\"synchronized\":%s,\"attempts\":%u,"
                 "\"last_offset_ms\":%.3f,\"last_sync_age_seconds\":%llu,\"error\":\"%s\"}",
                 N.server, N.resolved, N.synchronized ? "true" : "false", N.attempts,
                 (double)N.last_offset_us / 1000.0, (unsigned long long)((mono_ms() - N.last_sync_ms) / 1000u), N.error);
    else
        snprintf(out, cap, "{\"server\":\"%s\",\"resolved\":\"%s\",\"synchronized\":false,\"attempts\":%u,\"error\":\"%s\"}",
                 N.server, N.resolved, N.attempts, N.error);
    pthread_mutex_unlock(&N.lock);
}
