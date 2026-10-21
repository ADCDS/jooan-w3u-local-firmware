#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void nap_ms(long ms) { struct timespec t; t.tv_sec = ms / 1000; t.tv_nsec = (ms % 1000) * 1000000L; while (nanosleep(&t, &t) && errno == EINTR) {} }

/* Helper timeouts must not be measured on the wall clock: time-set moves it,
 * and a forward jump would expire the deadline instantly, killing a helper that
 * is working correctly and reporting the operation as failed. */
static time_t mono_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) return ts.tv_sec;
    return time(NULL);
}

int joan_stage_blob(const JoanConfig *cfg, const char *category,
                    const void *data, size_t len, char id[65], char path[512])
{
    unsigned char hash[32]; char dir[512];
    if (!category || strspn(category, "abcdefghijklmnopqrstuvwxyz-") != strlen(category)) return -1;
    joan_sha256(data, len, hash); joan_hex(hash, sizeof(hash), id);
    if (snprintf(dir, sizeof(dir), "%s/%s", cfg->staging_dir, category) >= (int)sizeof(dir) || joan_mkdir_p(dir, 0700)) return -1;
    if (snprintf(path, 512, "%s/%s.bin", dir, id) >= 512) return -1;
    return joan_write_atomic(path, data, len, 0600);
}

int joan_run_helper(const JoanConfig *cfg, const char *operation,
                    const char *argument_path, const char *id,
                    unsigned char **output, size_t *output_len, unsigned timeout_sec)
{
    int fds[2], status = 0; pid_t pid; unsigned char *buf; size_t used = 0;
    size_t cap = !strcmp(operation, "snapshot") ? 2u * 1024u * 1024u : 65536u;
    time_t deadline = mono_seconds() + timeout_sec;
    if (!cfg->integration_helper[0] || !operation || pipe(fds)) return -1;
    buf = malloc(cap + 1); if (!buf) { close(fds[0]); close(fds[1]); return -1; }
    pid = fork();
    if (pid < 0) { free(buf); close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        (void)setpgid(0,0);
        dup2(fds[1], 1); dup2(fds[1], 2); close(fds[0]); close(fds[1]);
        execl(cfg->integration_helper, cfg->integration_helper, operation,
              argument_path ? argument_path : "-", id ? id : "-", (char *)0);
        _exit(127);
    }
    (void)setpgid(pid,pid);
    close(fds[1]); fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    for (;;) {
        ssize_t n = read(fds[0], buf + used, cap - used);
        if (n > 0) used += (size_t)n;
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        if (mono_seconds() >= deadline || used == cap) {
            /* Helpers may spawn nc or shell pipeline descendants. Kill their
             * private process group before another motor command is issued. */
            kill(-pid, SIGTERM); nap_ms(100);
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            status = -1; break;
        }
        nap_ms(20);
    }
    while (used < cap) { ssize_t n = read(fds[0], buf + used, cap - used); if (n <= 0) break; used += (size_t)n; }
    close(fds[0]); buf[used] = 0;
    if (output) *output = buf; else free(buf);
    if (output_len) *output_len = used;
    return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* The motor path is jooanipc's own ONVIF PTZ service, which the guard keeps
 * on loopback. Calling it here rather than through a helper script costs no
 * fork, so a nudge lasts as long as it was asked to: the helper added a
 * quarter to half a second before each Stop reached the OEM. The OEM runs a
 * ContinuousMove until Stop, ignoring speed and Timeout, so every caller
 * stops explicitly and keeps the deadman armed. direction NULL means Stop;
 * the request is complete before the socket opens. */
int joan_oem_ptz(const JoanConfig *cfg, const char *direction, unsigned speed)
{
    static const char *const velocity[] = { "0.2", "0.4", "0.6", "0.8", "1.0" };
    char body[640], reply[4096]; const char *v; size_t got = 0; ssize_t z;
    int n, head, fd, ok, pan = 0, negative = 0;
    struct sockaddr_in a; struct timeval limit = { 2, 0 };
    if (direction) {
        pan = !strcmp(direction, "left") || !strcmp(direction, "right");
        negative = !strcmp(direction, "left") || !strcmp(direction, "down");
        if (!pan && strcmp(direction, "up") && strcmp(direction, "down")) return -1;
    }
    v = velocity[speed >= 1 && speed <= 5 ? speed - 1 : 2];
    /* The OEM matches these literal prefixes, so they must not change. */
    n = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>");
    if (direction)
        n += snprintf(body + n, sizeof(body) - (size_t)n,
            "<tptz:ContinuousMove><tptz:ProfileToken>profile_0</tptz:ProfileToken><tptz:Velocity>"
            "<tt:PanTilt x=\"%s%s\" y=\"%s%s\"/></tptz:Velocity></tptz:ContinuousMove>",
            pan && negative ? "-" : "", pan ? v : "0.0", !pan && negative ? "-" : "", pan ? "0.0" : v);
    else
        n += snprintf(body + n, sizeof(body) - (size_t)n,
            "<tptz:Stop><tptz:ProfileToken>profile_0</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop>");
    n += snprintf(body + n, sizeof(body) - (size_t)n, "</s:Body></s:Envelope>");
    head = snprintf(reply, sizeof(reply),
        "POST /onvif/Ptz HTTP/1.0\r\nHost: 127.0.0.1:%u\r\nContent-Type: application/soap+xml; charset=utf-8; "
        "action=\"http://www.onvif.org/ver20/ptz/wsdl/%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        cfg->oem_onvif_port, direction ? "ContinuousMove" : "Stop", n, body);
    if (n <= 0 || (size_t)n >= sizeof(body) || head <= 0 || (size_t)head >= sizeof(reply)) return -1;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)cfg->oem_onvif_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) || send(fd, reply, (size_t)head, MSG_NOSIGNAL) != head) { close(fd); return -1; }
    while (got + 1 < sizeof(reply) && (z = recv(fd, reply + got, sizeof(reply) - 1 - got, 0)) > 0) got += (size_t)z;
    close(fd); reply[got] = 0;
    ok = (!strncmp(reply, "HTTP/1.1 200 ", 13) || !strncmp(reply, "HTTP/1.0 200 ", 13)) && !strstr(reply, "Fault>") &&
         strstr(reply, direction ? "<tptz:ContinuousMoveResponse" : "<tptz:StopResponse");
    return ok ? 0 : -1;
}
