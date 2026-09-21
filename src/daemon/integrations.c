#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
        dup2(fds[1], 1); dup2(fds[1], 2); close(fds[0]); close(fds[1]);
        execl(cfg->integration_helper, cfg->integration_helper, operation,
              argument_path ? argument_path : "-", id ? id : "-", (char *)0);
        _exit(127);
    }
    close(fds[1]); fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    for (;;) {
        ssize_t n = read(fds[0], buf + used, cap - used);
        if (n > 0) used += (size_t)n;
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        if (mono_seconds() >= deadline || used == cap) { kill(pid, SIGTERM); nap_ms(100); if (waitpid(pid, &status, WNOHANG) != pid) { kill(pid, SIGKILL); waitpid(pid, &status, 0); } status = -1; break; }
        nap_ms(20);
    }
    while (used < cap) { ssize_t n = read(fds[0], buf + used, cap - used); if (n <= 0) break; used += (size_t)n; }
    close(fds[0]); buf[used] = 0;
    if (output) *output = buf; else free(buf);
    if (output_len) *output_len = used;
    return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
