#define _GNU_SOURCE

#include "guard_sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_length;
};

static const uint32_t round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotate_right(uint32_t value, unsigned int amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t load_be32(const unsigned char *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

static void sha256_transform(struct sha256_context *context,
                             const unsigned char block[64])
{
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t choice, majority, sigma0, sigma1, temporary1, temporary2;
    unsigned int index;

    for (index = 0; index < 16; ++index)
        words[index] = load_be32(block + index * 4U);
    for (; index < 64; ++index) {
        sigma0 = rotate_right(words[index - 15], 7) ^
                 rotate_right(words[index - 15], 18) ^
                 (words[index - 15] >> 3);
        sigma1 = rotate_right(words[index - 2], 17) ^
                 rotate_right(words[index - 2], 19) ^
                 (words[index - 2] >> 10);
        words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    a = context->state[0]; b = context->state[1];
    c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5];
    g = context->state[6]; h = context->state[7];

    for (index = 0; index < 64; ++index) {
        sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        choice = (e & f) ^ ((~e) & g);
        temporary1 = h + sigma1 + choice + round_constants[index] + words[index];
        sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        majority = (a & b) ^ (a & c) ^ (b & c);
        temporary2 = sigma0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }

    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

static void sha256_init(struct sha256_context *context)
{
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memcpy(context->state, initial_state, sizeof(initial_state));
    context->bit_count = 0;
    context->block_length = 0;
}

static void sha256_update(struct sha256_context *context,
                          const unsigned char *input, size_t length)
{
    size_t amount;

    context->bit_count += (uint64_t)length * 8U;
    while (length != 0) {
        amount = sizeof(context->block) - context->block_length;
        if (amount > length)
            amount = length;
        memcpy(context->block + context->block_length, input, amount);
        context->block_length += amount;
        input += amount;
        length -= amount;
        if (context->block_length == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
}

static void sha256_final(struct sha256_context *context, unsigned char digest[32])
{
    unsigned int index;

    context->block[context->block_length++] = 0x80;
    if (context->block_length > 56) {
        memset(context->block + context->block_length, 0,
               sizeof(context->block) - context->block_length);
        sha256_transform(context, context->block);
        context->block_length = 0;
    }
    memset(context->block + context->block_length, 0, 56 - context->block_length);
    for (index = 0; index < 8; ++index)
        context->block[63 - index] =
            (unsigned char)(context->bit_count >> (index * 8U));
    sha256_transform(context, context->block);

    for (index = 0; index < 8; ++index) {
        digest[index * 4] = (unsigned char)(context->state[index] >> 24);
        digest[index * 4 + 1] = (unsigned char)(context->state[index] >> 16);
        digest[index * 4 + 2] = (unsigned char)(context->state[index] >> 8);
        digest[index * 4 + 3] = (unsigned char)context->state[index];
    }
}

static void digest_to_hex(const unsigned char digest[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned int index;

    for (index = 0; index < 32; ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    output[64] = '\0';
}

int jooan_guard_sha256_bytes_hex(const void *data, size_t length,
                                 char output[65])
{
    struct sha256_context context;
    unsigned char digest[32];

    if ((data == NULL && length != 0) || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    sha256_init(&context);
    sha256_update(&context, (const unsigned char *)data, length);
    sha256_final(&context, digest);
    digest_to_hex(digest, output);
    return 0;
}

int jooan_guard_sha256_file_hex(const char *path, char output[65])
{
    struct sha256_context context;
    unsigned char digest[32];
    unsigned char buffer[4096];
    ssize_t amount;
    int descriptor;

    if (path == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }
#ifdef SYS_openat
    descriptor = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
#else
    descriptor = (int)syscall(SYS_open, path, O_RDONLY, 0);
#endif
    if (descriptor < 0)
        return -1;

    sha256_init(&context);
    do {
        amount = (ssize_t)syscall(SYS_read, descriptor, buffer, sizeof(buffer));
        if (amount > 0)
            sha256_update(&context, buffer, (size_t)amount);
    } while (amount > 0 || (amount < 0 && errno == EINTR));
    (void)syscall(SYS_close, descriptor);
    if (amount < 0)
        return -1;

    sha256_final(&context, digest);
    digest_to_hex(digest, output);
    return 0;
}
