#define _GNU_SOURCE
#include "jipc.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t jipc_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void jipc_sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
void jipc_log(const char *level, const char *format, ...)
{
    va_list ap; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    flockfile(stderr); fprintf(stderr, "%lld.%03ld [%s] ", (long long)ts.tv_sec, ts.tv_nsec / 1000000L, level);
    va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); funlockfile(stderr);
}
int jipc_write_all(int fd, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    while (bytes) { ssize_t n = write(fd, p, bytes); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n; }
    return 0;
}
int jipc_random(void *data, size_t bytes)
{
    unsigned char *p = data; int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (bytes) { ssize_t n = read(fd, p, bytes); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; } p += n; bytes -= (size_t)n; }
    return close(fd);
}
int jipc_mkdirs(const char *path, unsigned mode)
{
    char buf[1024]; size_t n = strlen(path);
    if (!n || n >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; ++p) if (*p == '/') {
        *p = 0; if (mkdir(buf, (mode_t)mode) && errno != EEXIST) return -1; *p = '/'; }
    if (mkdir(buf, (mode_t)mode) && errno != EEXIST) return -1;
    struct stat st; return stat(buf, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}
int jipc_atomic_file(const char *path, const void *data, size_t size, unsigned mode)
{
    char temporary[1100], parent[1024]; int fd, rc = -1;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary); if (fd < 0) return -1;
    if (!fchmod(fd, (mode_t)mode) && !jipc_write_all(fd, data, size) && !fsync(fd)) rc = 0;
    if (close(fd)) rc = -1;
    if (!rc && rename(temporary, path)) rc = -1;
    if (rc) { unlink(temporary); return -1; }
    if (strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path); char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    rc = fsync(fd); close(fd); return rc;
}
int jipc_equal(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b), length = al > bl ? al : bl; unsigned diff = (unsigned)(al ^ bl);
    for (size_t i = 0; i < length; ++i) diff |= (unsigned)((i < al ? (unsigned char)a[i] : 0) ^ (i < bl ? (unsigned char)b[i] : 0));
    return diff == 0;
}
static cJSON *json_object(const char *json)
{
    if (!json || strlen(json) > JIPC_MAX_JSON || strstr(json, "\\u0000")) return NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, 1);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    for (cJSON *a = root->child; a; a = a->next) for (cJSON *b = a->next; b; b = b->next)
        if (!strcmp(a->string, b->string)) { cJSON_Delete(root); return NULL; }
    return root;
}
int jipc_json_string(const char *json, const char *key, char *out, size_t capacity)
{
    cJSON *root = json_object(json); if (!root) return -1;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key); int rc = 1;
    if (value) { rc = -1; if (cJSON_IsString(value) && strlen(value->valuestring) < capacity) {
        strcpy(out, value->valuestring); rc = 0; } }
    cJSON_Delete(root); return rc;
}
int jipc_json_integer(const char *json, const char *key, int *out)
{
    cJSON *root = json_object(json); if (!root) return -1;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key); int rc = 1;
    if (value) { rc = -1;
        if (cJSON_IsBool(value)) { *out = cJSON_IsTrue(value); rc = 0; }
        else if (cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
                 value->valuedouble >= INT_MIN && value->valuedouble <= INT_MAX &&
                 floor(value->valuedouble) == value->valuedouble) { *out = (int)value->valuedouble; rc = 0; }
    }
    cJSON_Delete(root); return rc;
}
int jipc_json_escape(const char *text, char *out, size_t capacity)
{
    cJSON *value = cJSON_CreateString(text); if (!value) return -1;
    char *encoded = cJSON_PrintUnformatted(value); int rc = -1;
    if (encoded && strlen(encoded) < capacity) { strcpy(out, encoded); rc = (int)strlen(out); }
    cJSON_free(encoded); cJSON_Delete(value); return rc;
}

/* T23 1.3.0 calls these logging FUNC symbols directly. Exported by the daemon
 * for an externally supplied SDK; no vendor logger or allocator is imitated. */
unsigned int IMP_Log_Get_Option(void) { return 0; }
int imp_log_fun(int level, unsigned int option, int output, const char *tag,
                const char *file, int line, const char *function, const char *format, ...)
{
    (void)option; (void)output; (void)file; (void)line; (void)function;
    va_list ap; flockfile(stderr); fprintf(stderr, "[IMP %d %s] ", level, tag ? tag : "");
    va_start(ap, format); int rc = vfprintf(stderr, format ? format : "", ap); va_end(ap);
    funlockfile(stderr); return rc;
}
