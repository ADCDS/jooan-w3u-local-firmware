#ifndef JOOAN_GUARD_SHA256_H
#define JOOAN_GUARD_SHA256_H

#include <stddef.h>

int jooan_guard_sha256_file_hex(const char *path, char output[65]);
int jooan_guard_sha256_bytes_hex(const void *data, size_t length,
                                 char output[65]);

#endif
