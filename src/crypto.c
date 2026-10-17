#include "jipc.h"
#include <mbedtls/base64.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

static void hex(const unsigned char *data, size_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) { out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15]; }
    out[size * 2] = 0;
}
int jipc_base64(const void *data, size_t size, char *out, size_t capacity)
{
    size_t written = 0; if (mbedtls_base64_encode((unsigned char *)out, capacity, &written, data, size)) return -1;
    return (int)written;
}
int jipc_base64_decode(const char *data, unsigned char *out, size_t capacity)
{
    size_t written = 0; if (mbedtls_base64_decode(out, capacity, &written, (const unsigned char *)data, strlen(data))) return -1;
    return (int)written;
}
void jipc_md5(const void *data, size_t size, char out[33])
{
    unsigned char digest[16]; if (mbedtls_md5(data, size, digest)) memset(digest, 0, sizeof(digest)); hex(digest, sizeof(digest), out);
}
void jipc_sha1(const void *data, size_t size, unsigned char out[20])
{
    if (mbedtls_sha1(data, size, out)) memset(out, 0, 20);
}
int jipc_sha256_file(const char *path, char out[65])
{
    unsigned char buffer[16384], digest[32]; size_t n; int rc = -1;
    FILE *file = fopen(path, "rb"); if (!file) return -1;
    mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0)) goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), file))) if (mbedtls_sha256_update(&ctx, buffer, n)) goto done;
    if (ferror(file) || mbedtls_sha256_finish(&ctx, digest)) goto done;
    hex(digest, sizeof(digest), out); rc = 0;
done:
    mbedtls_sha256_free(&ctx); fclose(file); return rc;
}
