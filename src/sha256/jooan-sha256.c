/* Small tmpfs-only SHA-256 utility backed by the exact-gated OEM mbedcrypto. */
#include <stdio.h>
#include <stdlib.h>

#include <mbedtls/sha256.h>

static int hash_file(const char *path, unsigned char digest[32])
{
    FILE *stream = fopen(path, "rb");
    unsigned char block[16384];
    size_t count;
    mbedtls_sha256_context context;
    int result = -1;
    if (!stream) return -1;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0)) goto done;
    while ((count = fread(block, 1, sizeof(block), stream)) != 0)
        if (mbedtls_sha256_update_ret(&context, block, count)) goto done;
    if (ferror(stream) || mbedtls_sha256_finish_ret(&context, digest)) goto done;
    result = 0;
done:
    mbedtls_sha256_free(&context);
    fclose(stream);
    return result;
}

int main(int argc, char **argv)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    int argument, status = 0;
    size_t offset;
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [...]\n", argv[0]);
        return 2;
    }
    for (argument = 1; argument < argc; ++argument) {
        if (hash_file(argv[argument], digest)) {
            fprintf(stderr, "%s: cannot hash %s\n", argv[0], argv[argument]);
            status = 1;
            continue;
        }
        for (offset = 0; offset < sizeof(digest); ++offset) {
            putchar(hex[digest[offset] >> 4]);
            putchar(hex[digest[offset] & 15]);
        }
        printf("  %s\n", argv[argument]);
    }
    return status;
}
