#define _POSIX_C_SOURCE 200112L

#include <netdb.h>

int main(void)
{
    struct addrinfo hints;
    struct addrinfo *result = 0;
    int rc;

    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    hints.ai_protocol = 0;
    hints.ai_addrlen = 0;
    hints.ai_addr = 0;
    hints.ai_canonname = 0;
    hints.ai_next = 0;
    rc = getaddrinfo("192.0.2.1", "443", &hints, &result);
    if (result != 0)
        freeaddrinfo(result);
    return rc == 0 ? 0 : 1;
}
