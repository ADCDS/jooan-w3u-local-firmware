/* Strict, shell-safe inspection of a JOOAN IronMan package. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

#ifndef JOOAN_MODEL_TOKEN
#define JOOAN_MODEL_TOKEN "A12"
#endif

#define HEADER_SIZE 96u
#define TRAILER_SIZE 96u
#define PAYLOAD_LIMIT 0x200001u
#define PACKAGE_MAX (PAYLOAD_LIMIT + HEADER_SIZE + TRAILER_SIZE)

static int decimal_field(const unsigned char field[8], size_t *value)
{
    size_t i = 0, result = 0;
    if (field[0] < '0' || field[0] > '9') return -1;
    while (i < 8 && field[i]) {
        if (field[i] < '0' || field[i] > '9') return -1;
        result = result * 10u + (size_t)(field[i] - '0');
        ++i;
    }
    while (i < 8) if (field[i++]) return -1;
    *value = result;
    return 0;
}

static void qa_decode(const unsigned char raw[TRAILER_SIZE],
                      unsigned char decoded[TRAILER_SIZE])
{
    unsigned char buffer[TRAILER_SIZE], temporary;
    size_t stride, offset;
    memcpy(buffer, raw, sizeof(buffer));
    stride = ((buffer[TRAILER_SIZE - 1] >> 2) & 15u) + 1u;
    offset = ((TRAILER_SIZE - 3u) / stride) * stride;
    while (offset >= stride) {
        temporary = buffer[offset];
        buffer[offset] = buffer[offset + 1u];
        buffer[offset + 1u] = temporary;
        offset -= stride;
    }
    decoded[0] = (unsigned char)((buffer[0] >> 2) |
                                  (buffer[TRAILER_SIZE - 1] << 6));
    for (offset = 1; offset < TRAILER_SIZE; ++offset)
        decoded[offset] = (unsigned char)((buffer[offset] >> 2) |
                                           (buffer[offset - 1] << 6));
}

static int lower_hex_equal(const unsigned char recorded[32],
                           const unsigned char digest[16])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 16; ++i) {
        if (recorded[i * 2] != (unsigned char)hex[digest[i] >> 4] ||
            recorded[i * 2 + 1] != (unsigned char)hex[digest[i] & 15]) return 0;
    }
    return 1;
}

static void print_hex(const unsigned char *data, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < length; ++i) {
        putchar(hex[data[i] >> 4]);
        putchar(hex[data[i] & 15]);
    }
}

int main(int argc, char **argv)
{
    FILE *stream = NULL;
    unsigned char header[HEADER_SIZE], raw_trailer[TRAILER_SIZE], trailer[TRAILER_SIZE];
    unsigned char block[4096], body_md5[16], payload_md5[16], package_sha256[32];
    mbedtls_md5_context body_context, payload_context;
    mbedtls_sha256_context package_context;
    size_t package_size, body_size, payload_size, remaining, count;
    long file_size;
    char version[49], expected_model[64];
    int result = 1, contexts_ready = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PACKAGE\n", argv[0]);
        return 2;
    }
    stream = fopen(argv[1], "rb");
    if (!stream || fseek(stream, 0, SEEK_END) || (file_size = ftell(stream)) < 0) {
        fprintf(stderr, "invalid IronMan package size or input\n");
        goto done;
    }
    package_size = (size_t)file_size;
    if (package_size < HEADER_SIZE + TRAILER_SIZE || package_size > PACKAGE_MAX ||
        fseek(stream, 0, SEEK_SET) || fread(header, 1, sizeof(header), stream) != sizeof(header)) {
        fprintf(stderr, "invalid IronMan package size or input\n");
        goto done;
    }
    if (memcmp(header, "jooan\0\0\0", 8) ||
        decimal_field(header + 8, &body_size) || body_size + HEADER_SIZE != package_size) {
        fprintf(stderr, "invalid IronMan header\n");
        goto done;
    }
    memcpy(version, header + 16, 48);
    version[48] = '\0';
    if (!memchr(version, '\0', 48) ||
        snprintf(expected_model, sizeof(expected_model), ";ProductName=%s", JOOAN_MODEL_TOKEN) <= 0 ||
        strncmp(version, "ver=", 4) || strchr(version, ';') != strstr(version, expected_model) ||
        !strstr(version, expected_model) || strcmp(strstr(version, expected_model), expected_model)) {
        fprintf(stderr, "IronMan model/version field rejected\n");
        goto done;
    }
    if (fseek(stream, (long)(package_size - TRAILER_SIZE), SEEK_SET) ||
        fread(raw_trailer, 1, sizeof(raw_trailer), stream) != sizeof(raw_trailer)) goto done;
    qa_decode(raw_trailer, trailer);
    if (memcmp(trailer, "toolv\0\0\0", 8) ||
        decimal_field(trailer + 8, &payload_size) || payload_size >= PAYLOAD_LIMIT ||
        payload_size + HEADER_SIZE + TRAILER_SIZE != package_size) {
        fprintf(stderr, "IronMan trailer rejected\n");
        goto done;
    }

    mbedtls_md5_init(&body_context);
    mbedtls_md5_init(&payload_context);
    mbedtls_sha256_init(&package_context);
    contexts_ready = 1;
    if (mbedtls_md5_starts_ret(&body_context) ||
        mbedtls_md5_starts_ret(&payload_context) ||
        mbedtls_sha256_starts_ret(&package_context, 0) || fseek(stream, 0, SEEK_SET)) goto done;
    remaining = package_size;
    while (remaining) {
        count = remaining < sizeof(block) ? remaining : sizeof(block);
        if (fread(block, 1, count, stream) != count ||
            mbedtls_sha256_update_ret(&package_context, block, count)) goto done;
        remaining -= count;
    }
    if (mbedtls_sha256_finish_ret(&package_context, package_sha256) ||
        fseek(stream, HEADER_SIZE, SEEK_SET)) goto done;
    remaining = body_size;
    while (remaining) {
        count = remaining < sizeof(block) ? remaining : sizeof(block);
        if (fread(block, 1, count, stream) != count ||
            mbedtls_md5_update_ret(&body_context, block, count)) goto done;
        remaining -= count;
    }
    if (mbedtls_md5_finish_ret(&body_context, body_md5) ||
        !lower_hex_equal(header + 64, body_md5) || fseek(stream, HEADER_SIZE, SEEK_SET)) {
        fprintf(stderr, "IronMan body MD5 rejected\n");
        goto done;
    }
    remaining = payload_size;
    while (remaining) {
        count = remaining < sizeof(block) ? remaining : sizeof(block);
        if (fread(block, 1, count, stream) != count ||
            mbedtls_md5_update_ret(&payload_context, block, count)) goto done;
        remaining -= count;
    }
    if (mbedtls_md5_finish_ret(&payload_context, payload_md5) ||
        !lower_hex_equal(trailer + 64, payload_md5)) {
        fprintf(stderr, "IronMan payload MD5 rejected\n");
        goto done;
    }
    printf("payload_offset=%u\n", HEADER_SIZE);
    printf("payload_length=%lu\n", (unsigned long)payload_size);
    printf("package_length=%lu\n", (unsigned long)package_size);
    printf("model_token=%s\n", JOOAN_MODEL_TOKEN);
    printf("package_sha256=");
    print_hex(package_sha256, sizeof(package_sha256));
    putchar('\n');
    result = 0;
done:
    if (contexts_ready) {
        mbedtls_sha256_free(&package_context);
        mbedtls_md5_free(&payload_context);
        mbedtls_md5_free(&body_context);
    }
    if (stream) fclose(stream);
    return result;
}
