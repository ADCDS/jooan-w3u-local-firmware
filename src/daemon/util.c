#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int joan_mkdir_p(const char *path, mode_t mode)
{
    char copy[512];
    char *p;
    size_t n;
    if (!path || (n = strlen(path)) == 0 || n >= sizeof(copy)) return -1;
    memcpy(copy, path, n + 1);
    for (p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, mode) && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(copy, mode) == 0 || errno == EEXIST ? 0 : -1;
}

int joan_write_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
    char tmp[600];
    const unsigned char *p = data;
    size_t left = len;
    int fd;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return -1;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        p += n; left -= (size_t)n;
    }
    if (fsync(fd) || close(fd) || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
fail:
    close(fd); unlink(tmp); return -1;
}

int joan_read_file(const char *path, unsigned char **out, size_t *len, size_t max)
{
    struct stat st;
    unsigned char *buf;
    size_t off = 0;
    int fd;
    if (!out || !len || stat(path, &st) || st.st_size < 0 || (uint64_t)st.st_size > max) return -1;
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) { free(buf); return -1; }
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + off, (size_t)st.st_size - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); free(buf); return -1; }
        off += (size_t)n;
    }
    close(fd); buf[off] = 0; *out = buf; *len = off; return 0;
}

int joan_random(void *out, size_t len)
{
    unsigned char *p = out;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        p += n; len -= (size_t)n;
    }
    close(fd); return 0;
}

void joan_hex(const unsigned char *in, size_t len, char *out)
{
    static const char h[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; ++i) { out[i * 2] = h[in[i] >> 4]; out[i * 2 + 1] = h[in[i] & 15]; }
    out[len * 2] = 0;
}

int joan_unhex(const char *in, unsigned char *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        int a = in[i*2], b = in[i*2+1];
        a = a >= '0' && a <= '9' ? a-'0' : a >= 'a' && a <= 'f' ? a-'a'+10 : a >= 'A' && a <= 'F' ? a-'A'+10 : -1;
        b = b >= '0' && b <= '9' ? b-'0' : b >= 'a' && b <= 'f' ? b-'a'+10 : b >= 'A' && b <= 'F' ? b-'A'+10 : -1;
        if (a < 0 || b < 0) return -1;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return in[len * 2] == 0 ? 0 : -1;
}

int joan_ct_equal(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    unsigned char d = 0;
    while (n--) d |= *x++ ^ *y++;
    return d == 0;
}
