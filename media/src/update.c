/* SPDX-License-Identifier: MIT
 * A media update from the browser. A package holds an app partition image built from
 * this camera's own, the scripts that flash it, and a manifest signed with the
 * platform's release key (tools/make-media-update.py). It is written to RAM as it
 * arrives, checked by the platform's own verifier, and flashed only when asked.
 * Nothing in a package runs before its signature has been checked.
 *
 * The package: a 96-byte header "JIPCMEDIA2 <manifest> <signature> <upgrade script>
 * <apply script> <boot script> <image>" padded with spaces and ending in a newline, then
 * those parts. The boot script is the native-boot.sh this release goes with.
 */
#define _GNU_SOURCE
#include "jipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define HEADER 96u
#define IMAGE_BYTES 3211264u
#define MANIFEST_MAX 8192u
#define SIGNATURE_MAX 256u
#define SCRIPT_MAX 32768u
#define STAGED_US (UINT64_C(15) * 60 * 1000000) /* an abandoned upload does not stay in RAM */

static const char *const parts[] = {"release.manifest", "release.manifest.sig", "upgrade-appfs.sh",
                                    "apply-media-update.sh", "native-boot.sh", "appfs.img"};
enum { MANIFEST, SIGNATURE, UPGRADE, APPLY, BOOT, IMAGE, PARTS };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int receiving;
static pid_t applying;
static uint64_t staged_at;
static char release[32], failed[160];

/* Test hooks in the environment, never camera.ini keys. */
static const char *directory(void)
{
    const char *dir = getenv("JIPC_UPDATE_DIR");
    return dir && *dir ? dir : "/tmp/jipc-media-update";
}

static const char *verifier(void)
{
    const char *path = getenv("JIPC_UPDATE_VERIFIER");
    return path && *path ? path : "/run/jooan-local/controller/shared/jooan-auth-verify";
}

static void part_path(char *out, size_t capacity, const char *name)
{
    snprintf(out, capacity, "%s/%s", directory(), name);
}

static void log_path(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s.log", directory());
}

static void wipe(void)
{
    char path[PATH_MAX];
    for (unsigned i = 0; i < PARTS; ++i) { part_path(path, sizeof path, parts[i]); unlink(path); }
    part_path(path, sizeof path, "check.out"); unlink(path);
    rmdir(directory());
    release[0] = 0;
    staged_at = 0;
}

/* Runs a program with a clean environment and only the output given. fork() could fail
 * here: the daemon's address space is larger than the camera's memory. */
static pid_t spawn(const char *const argv[], const char *output)
{
    static char *const environment[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL};
    pid_t pid = vfork();
    if (pid) return pid;
    setsid();
    int in = open("/dev/null", O_RDONLY), out = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (in < 0 || out < 0 || dup2(in, 0) < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0) _exit(127);
    for (int fd = 3; fd < 256; ++fd) close(fd);
    execve(argv[0], (char *const *)argv, environment);
    _exit(127);
}

static int run(const char *const argv[], const char *output)
{
    pid_t pid = spawn(argv, output);
    if (pid < 0) return -1;
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int verify(void)
{
    char manifest[PATH_MAX], signature[PATH_MAX], out[PATH_MAX];
    part_path(manifest, sizeof manifest, parts[MANIFEST]);
    part_path(signature, sizeof signature, parts[SIGNATURE]);
    part_path(out, sizeof out, "check.out");
    const char *const argv[] = {verifier(), manifest, signature, directory(), NULL};
    return run(argv, out);
}

static int read_text(const char *path, char *out, size_t capacity)
{
    FILE *file = fopen(path, "re");
    if (!file) return -1;
    size_t n = fread(out, 1, capacity - 1, file);
    fclose(file);
    out[n] = 0;
    return 0;
}

/* A line of the manifest, before its list of files. */
static int field(const char *manifest, const char *key, char *out, size_t capacity)
{
    size_t length = strlen(key);
    for (const char *line = manifest; line && *line;) {
        const char *end = strchr(line, '\n');
        if (!strncmp(line, "files-begin", 11)) return -1;
        if (end && !strncmp(line, key, length) && line[length] == '=') {
            size_t n = (size_t)(end - line) - length - 1;
            if (n >= capacity) return -1;
            memcpy(out, line + length + 1, n);
            out[n] = 0;
            return 0;
        }
        line = end ? end + 1 : NULL;
    }
    return -1;
}

static int plain_release(const char *text)
{
    size_t n = strlen(text);
    if (n != 17 || text[8] != '-') return 0;
    for (size_t i = 0; i < n; ++i) if (i != 8 && !strchr("0123456789abcdef", text[i])) return 0;
    return 1;
}

static int write_part(const char *name, size_t size, const unsigned char **first, size_t *first_size,
                      jipc_joan_pull pull, void *context, char *error, size_t capacity)
{
    char path[PATH_MAX], chunk[4096];
    part_path(path, sizeof path, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { snprintf(error, capacity, "Cannot keep the package: %s", strerror(errno)); return 500; }
    int status = 0;
    while (size && !status) {
        const void *data;
        size_t n;
        if (*first_size) {
            n = *first_size < size ? *first_size : size;
            data = *first;
            *first += n;
            *first_size -= n;
        } else {
            ssize_t got = pull(context, chunk, size < sizeof chunk ? size : sizeof chunk);
            if (got <= 0) { snprintf(error, capacity, "The upload stopped before the whole package arrived"); status = 400; break; }
            n = (size_t)got;
            data = chunk;
        }
        for (const char *p = data; n && !status;) {
            ssize_t put = write(fd, p, n);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { snprintf(error, capacity, "Cannot keep the package: %s", strerror(errno)); status = 507; break; }
            p += put; n -= (size_t)put; size -= (size_t)put;
        }
    }
    close(fd);
    return status;
}

static int receive_package(const char *first, size_t first_size, size_t total, jipc_joan_pull pull, void *context,
                           char *error, size_t capacity)
{
    const unsigned char *rest = (const unsigned char *)first;
    char header[HEADER + 1];
    size_t have = 0;
    if (total < HEADER) { snprintf(error, capacity, "This is not a media package"); return 400; }
    while (have < HEADER) {
        if (first_size) {
            size_t n = first_size < HEADER - have ? first_size : HEADER - have;
            memcpy(header + have, rest, n);
            rest += n; first_size -= n; have += n;
        } else {
            ssize_t got = pull(context, header + have, HEADER - have);
            if (got <= 0) { snprintf(error, capacity, "The upload stopped before the whole package arrived"); return 400; }
            have += (size_t)got;
        }
    }
    header[HEADER] = 0;
    unsigned sizes[PARTS];
    if (sscanf(header, "JIPCMEDIA2 %u %u %u %u %u %u", &sizes[MANIFEST], &sizes[SIGNATURE], &sizes[UPGRADE], &sizes[APPLY], &sizes[BOOT], &sizes[IMAGE]) != 6 ||
        header[HEADER - 1] != '\n') {
        snprintf(error, capacity, "This is not a media package");
        return 400;
    }
    if (!sizes[MANIFEST] || sizes[MANIFEST] > MANIFEST_MAX || !sizes[SIGNATURE] || sizes[SIGNATURE] > SIGNATURE_MAX ||
        !sizes[UPGRADE] || sizes[UPGRADE] > SCRIPT_MAX || !sizes[APPLY] || sizes[APPLY] > SCRIPT_MAX ||
        !sizes[BOOT] || sizes[BOOT] > SCRIPT_MAX ||
        sizes[IMAGE] != IMAGE_BYTES ||
        total != (size_t)HEADER + sizes[MANIFEST] + sizes[SIGNATURE] + sizes[UPGRADE] + sizes[APPLY] + sizes[BOOT] + sizes[IMAGE]) {
        snprintf(error, capacity, "This is not a media package for this camera");
        return 400;
    }
    if (mkdir(directory(), 0700) && errno != EEXIST) { snprintf(error, capacity, "Cannot keep the package: %s", strerror(errno)); return 500; }
    for (unsigned i = 0; i < PARTS; ++i) {
        int status = write_part(parts[i], sizes[i], &rest, &first_size, pull, context, error, capacity);
        if (status) return status;
    }
    /* The signature first: nothing else in the package is trusted until then. */
    if (verify()) { snprintf(error, capacity, "The signature does not match: this package was not signed for this camera"); return 400; }
    char manifest[MANIFEST_MAX + 1], path[PATH_MAX], value[64];
    part_path(path, sizeof path, parts[MANIFEST]);
    if (read_text(path, manifest, sizeof manifest) ||
        field(manifest, "artifact_kind", value, sizeof value) || strcmp(value, "media") ||
        field(manifest, "target_id", value, sizeof value) || strcmp(value, "jooan-ja-a12-t23n-dual-cv2005-skw6316") ||
        field(manifest, "device_model", value, sizeof value) || strcmp(value, "JA-A12") ||
        field(manifest, "release_version", value, sizeof value) || !plain_release(value)) {
        snprintf(error, capacity, "This signed package is not a media release for this camera");
        return 400;
    }
    for (unsigned i = UPGRADE; i < PARTS; ++i) {
        char listed[64];
        snprintf(listed, sizeof listed, "  %s\n", parts[i]);
        if (!strstr(manifest, listed)) { snprintf(error, capacity, "This signed package does not cover all its files"); return 400; }
    }
    /* Its own script knows what it needs: the app partition it was built from, for one. */
    char script[PATH_MAX], out[PATH_MAX];
    part_path(script, sizeof script, parts[APPLY]);
    part_path(out, sizeof out, "check.out");
    const char *const argv[] = {"/bin/sh", script, "check", NULL};
    if (run(argv, out)) {
        char why[128] = "";
        read_text(out, why, sizeof why);
        why[strcspn(why, "\n")] = 0;
        snprintf(error, capacity, "%s", why[0] ? why : "The package cannot be installed on this camera");
        return 409;
    }
    if (snprintf(release, sizeof release, "%s", value) >= (int)sizeof release) {
        snprintf(error, capacity, "The package names no valid release");
        return 409;
    }
    staged_at = jipc_now_us();
    return 200;
}

int jipc_update_receive(const char *first, size_t first_size, size_t total, jipc_joan_pull pull, void *context,
                        char *out, size_t out_capacity, char *error, size_t capacity)
{
    pthread_mutex_lock(&lock);
    if (receiving || applying) {
        pthread_mutex_unlock(&lock);
        snprintf(error, capacity, receiving ? "Another upload is in progress" : "An update is being installed");
        return 409;
    }
    receiving = 1;
    wipe();
    failed[0] = 0;
    pthread_mutex_unlock(&lock);
    /* Long: the upload itself and hashing 3 MB, which the lock must not be held over. */
    int status = receive_package(first, first_size, total, pull, context, error, capacity);
    pthread_mutex_lock(&lock);
    receiving = 0;
    if (status != 200) wipe();
    else snprintf(out, out_capacity, "{\"release\":\"%s\"}\n", release);
    pthread_mutex_unlock(&lock);
    return status;
}

int jipc_update_apply(const char *wanted, char *error, size_t capacity)
{
    pthread_mutex_lock(&lock);
    int status = 200;
    if (applying) { snprintf(error, capacity, "An update is being installed"); status = 409; }
    else if (receiving) { snprintf(error, capacity, "Another upload is in progress"); status = 409; }
    else if (!release[0] || strcmp(wanted, release)) { snprintf(error, capacity, "That package is no longer here: upload it again"); status = 409; }
    else if (verify()) { wipe(); snprintf(error, capacity, "The package changed since it was checked: upload it again"); status = 400; }
    else {
        char script[PATH_MAX], log[PATH_MAX];
        part_path(script, sizeof script, parts[APPLY]);
        log_path(log, sizeof log);
        const char *const argv[] = {"/bin/sh", script, "apply", NULL};
        failed[0] = 0;
        applying = spawn(argv, log);
        if (applying < 0) { applying = 0; snprintf(error, capacity, "Cannot start the update: %s", strerror(errno)); status = 500; }
    }
    pthread_mutex_unlock(&lock);
    return status;
}

void jipc_update_discard(void)
{
    pthread_mutex_lock(&lock);
    if (!receiving && !applying) { wipe(); failed[0] = 0; }
    pthread_mutex_unlock(&lock);
}

/* The script's outcome, if it ended without the camera restarting. */
static void reap(void)
{
    int status;
    if (!applying || waitpid(applying, &status, WNOHANG) != applying) return;
    applying = 0;
    if (WIFEXITED(status) && !WEXITSTATUS(status)) return;
    char log[PATH_MAX], text[4096] = "";
    log_path(log, sizeof log);
    read_text(log, text, sizeof text);
    const char *why = NULL;
    for (const char *p = strstr(text, "ABORT"); p; p = strstr(p + 1, "ABORT")) why = p;
    snprintf(failed, sizeof failed, "%.150s", why ? why : "the update script failed");
    failed[strcspn(failed, "\n")] = 0;
}

void jipc_update_tick(void)
{
    pthread_mutex_lock(&lock);
    reap();
    const char *hold = getenv("JIPC_UPDATE_STAGED_S"); /* a test hook, like the two above */
    uint64_t limit = hold && *hold ? (uint64_t)atoi(hold) * 1000000u : STAGED_US;
    if (release[0] && !applying && !receiving && jipc_now_us() - staged_at > limit) wipe();
    pthread_mutex_unlock(&lock);
}

int jipc_update_json(char *out, size_t capacity)
{
    pthread_mutex_lock(&lock);
    reap();
    char why[340] = "\"\"";
    jipc_json_escape(failed, why, sizeof why);
    int n = snprintf(out, capacity, "{\"state\":\"%s\",\"release\":%s%s%s,\"error\":%s}\n",
                     receiving ? "receiving" : applying ? "installing" : release[0] ? "staged" : "idle",
                     release[0] ? "\"" : "", release[0] ? release : "null", release[0] ? "\"" : "", why);
    pthread_mutex_unlock(&lock);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
