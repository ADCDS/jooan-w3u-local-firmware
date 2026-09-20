/* Verify the signed release manifest with the public key compiled at build. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#ifndef JOOAN_RELEASE_PUBLIC_KEY_HEX
#error "JOOAN_RELEASE_PUBLIC_KEY_HEX must be supplied by the target generator"
#endif

#define MANIFEST_MAX (256u * 1024u)
#define SIGNATURE_MAX 80u

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_public_key(unsigned char out[65])
{
    const char *hex = JOOAN_RELEASE_PUBLIC_KEY_HEX;
    size_t i;
    for (i = 0; i < 65; ++i) {
        int high = hex_nibble((unsigned char)hex[i * 2]);
        int low = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (high < 0 || low < 0) return -1;
        out[i] = (unsigned char)((high << 4) | low);
    }
    return hex[130] == '\0' && out[0] == 4 ? 0 : -1;
}

static int read_file(const char *path, size_t limit,
                     unsigned char **output, size_t *length)
{
    FILE *stream;
    long size;
    unsigned char *data;
    stream = fopen(path, "rb");
    if (!stream) return -1;
    if (fseek(stream, 0, SEEK_END) || (size = ftell(stream)) < 0 ||
        (unsigned long)size > limit || fseek(stream, 0, SEEK_SET)) {
        fclose(stream);
        return -1;
    }
    data = malloc((size_t)size + 1u);
    if (!data || (size && fread(data, 1, (size_t)size, stream) != (size_t)size)) {
        free(data);
        fclose(stream);
        return -1;
    }
    data[size] = '\0';
    fclose(stream);
    *output = data;
    *length = (size_t)size;
    return 0;
}

static int sha256_file(const char *path, unsigned char output[32])
{
    FILE *stream = fopen(path, "rb");
    unsigned char block[4096];
    size_t count;
    mbedtls_sha256_context context;
    int result = -1;
    if (!stream) return -1;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0)) goto done;
    while ((count = fread(block, 1, sizeof(block), stream)) != 0)
        if (mbedtls_sha256_update_ret(&context, block, count)) goto done;
    if (ferror(stream) || mbedtls_sha256_finish_ret(&context, output)) goto done;
    result = 0;
done:
    mbedtls_sha256_free(&context);
    fclose(stream);
    return result;
}

static int safe_relative(const char *path)
{
    const char *cursor = path;
    if (!*path || *path == '/') return 0;
    while (*cursor) {
        if ((cursor == path || cursor[-1] == '/') && cursor[0] == '.' &&
            cursor[1] == '.' && (cursor[2] == '/' || cursor[2] == '\0')) return 0;
        ++cursor;
    }
    return 1;
}

static int verify_tree(unsigned char *manifest, size_t manifest_length,
                       const char *root)
{
    char *line, *save = NULL, path[1024];
    int in_files = 0, ended = 0, count = 0;
    struct stat status;
    unsigned char actual[32];
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (manifest_length == MANIFEST_MAX) return -1;
    manifest[manifest_length] = '\0';
    for (line = strtok_r((char *)manifest, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(line, "files-begin")) {
            if (in_files || ended) return -1;
            in_files = 1;
            continue;
        }
        if (!strcmp(line, "files-end")) {
            if (!in_files || ended) return -1;
            in_files = 0;
            ended = 1;
            continue;
        }
        if (!in_files) continue;
        if (strlen(line) < 67 || line[64] != ' ' || line[65] != ' ' ||
            !safe_relative(line + 66) ||
            snprintf(path, sizeof(path), "%s/%s", root, line + 66) >= (int)sizeof(path) ||
            lstat(path, &status) || !S_ISREG(status.st_mode) ||
            sha256_file(path, actual)) return -1;
        for (i = 0; i < 32; ++i)
            if (line[i * 2] != hex[actual[i] >> 4] ||
                line[i * 2 + 1] != hex[actual[i] & 15]) return -1;
        ++count;
    }
    return ended && !in_files && count > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    unsigned char public_key[65], hash[32];
    unsigned char *manifest = NULL, *signature = NULL;
    size_t manifest_length = 0, signature_length = 0;
    mbedtls_ecdsa_context context;
    int result = 2;
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s MANIFEST SIGNATURE [SIGNED-ROOT]\n", argv[0]);
        return 2;
    }
    if (decode_public_key(public_key) ||
        read_file(argv[1], MANIFEST_MAX, &manifest, &manifest_length) ||
        read_file(argv[2], SIGNATURE_MAX, &signature, &signature_length)) {
        fprintf(stderr, "release signature input error: %s\n", strerror(errno));
        goto done;
    }
    mbedtls_ecdsa_init(&context);
    if (mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1) ||
        mbedtls_ecp_point_read_binary(&context.grp, &context.Q,
                                      public_key, sizeof(public_key)) ||
        mbedtls_sha256_ret(manifest, manifest_length, hash, 0)) {
        fprintf(stderr, "release verifier initialization failed\n");
        goto crypto_done;
    }
    result = mbedtls_ecdsa_read_signature(&context, hash, sizeof(hash),
                                          signature, signature_length) ? 1 : 0;
    if (result) fprintf(stderr, "release manifest signature rejected\n");
    if (!result && argc == 4 && verify_tree(manifest, manifest_length, argv[3])) {
        fprintf(stderr, "signed release file inventory rejected\n");
        result = 1;
    }
crypto_done:
    mbedtls_ecdsa_free(&context);
done:
    free(signature);
    free(manifest);
    return result;
}
