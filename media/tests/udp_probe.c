/* SPDX-License-Identifier: MIT
 * Bounded UDP exchange for protocol probes over an SSH stdin/stdout pipe.
 * Does not initialize or access camera devices. Exit 2 means receive timeout.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    char *end; errno = 0; unsigned long port = strtoul(argv[2], &end, 10);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    if (errno || !*argv[2] || *end || !port || port > 65535 ||
        inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) return 1;
    address.sin_port = htons((unsigned short)port);
    unsigned char buffer[8192]; size_t size = 0;
    while (size < sizeof(buffer)) {
        ssize_t n = read(STDIN_FILENO, buffer + size, sizeof(buffer) - size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 1;
        if (!n) break;
        size += (size_t)n;
    }
    if (!size) return 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return 1;
    struct timeval timeout = {1, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        send(fd, buffer, size, 0) != (ssize_t)size) { close(fd); return 1; }
    ssize_t received;
    do { received = recv(fd, buffer, sizeof(buffer), 0); } while (received < 0 && errno == EINTR);
    int saved_errno = errno; close(fd);
    if (received < 0) return saved_errno == EAGAIN || saved_errno == EWOULDBLOCK ? 2 : 1;
    size = 0;
    while (size < (size_t)received) {
        ssize_t n = write(STDOUT_FILENO, buffer + size, (size_t)received - size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 1;
        size += (size_t)n;
    }
    return 0;
}
