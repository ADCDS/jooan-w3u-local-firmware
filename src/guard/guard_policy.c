#include "guard.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>

static int hostname_equal(const char *left, const char *right)
{
    size_t left_length;
    size_t right_length;
    size_t index;

    if (left == NULL || right == NULL)
        return 0;

    left_length = strlen(left);
    while (left_length > 0 && left[left_length - 1] == '.')
        --left_length;
    right_length = strlen(right);
    if (left_length != right_length)
        return 0;

    for (index = 0; index < left_length; ++index) {
        if (tolower((unsigned char)left[index]) !=
            tolower((unsigned char)right[index]))
            return 0;
    }
    return 1;
}

int jooan_guard_hostname_is_approved(const char *name)
{
    return hostname_equal(name, JOOAN_GUARD_API_HOST) ||
           hostname_equal(name, JOOAN_GUARD_MQTT_HOST);
}

int jooan_guard_hostname_is_mqtt(const char *name)
{
    return hostname_equal(name, JOOAN_GUARD_MQTT_HOST);
}

int jooan_guard_hostname_is_api(const char *name)
{
    return hostname_equal(name, JOOAN_GUARD_API_HOST);
}

int jooan_guard_hostname_is_loopback(const char *name)
{
    struct in_addr address4;
    struct in6_addr address6;

    if (hostname_equal(name, "localhost"))
        return 1;
    if (name == NULL)
        return 0;
    if (inet_pton(AF_INET, name, &address4) == 1)
        return (ntohl(address4.s_addr) >> 24) == 127;
    if (inet_pton(AF_INET6, name, &address6) == 1)
        return IN6_IS_ADDR_LOOPBACK(&address6);
    return 0;
}

int jooan_guard_sockaddr_is_loopback(const struct sockaddr *address,
                                     socklen_t address_length)
{
    const struct sockaddr_in *address4;
    const struct sockaddr_in6 *address6;

    if (address == NULL ||
        address_length < (socklen_t)sizeof(address->sa_family))
        return 0;
    if (address->sa_family == AF_UNIX)
        return 1;
#ifdef AF_NETLINK
    if (address->sa_family == AF_NETLINK)
        return 1;
#endif
    if (address->sa_family == AF_INET) {
        if (address_length < (socklen_t)sizeof(*address4))
            return 0;
        address4 = (const struct sockaddr_in *)address;
        return (ntohl(address4->sin_addr.s_addr) >> 24) == 127;
    }
    if (address->sa_family == AF_INET6) {
        if (address_length < (socklen_t)sizeof(*address6))
            return 0;
        address6 = (const struct sockaddr_in6 *)address;
        return IN6_IS_ADDR_LOOPBACK(&address6->sin6_addr);
    }
    return 0;
}

int16_t jooan_guard_alaw_to_pcm16(uint8_t value)
{
    int magnitude;
    int segment;

    value ^= 0x55;
    magnitude = (value & 0x0f) << 4;
    segment = (value & 0x70) >> 4;
    if (segment == 0) {
        magnitude += 8;
    } else if (segment == 1) {
        magnitude += 0x108;
    } else {
        magnitude += 0x108;
        magnitude <<= segment - 1;
    }
    return (int16_t)((value & 0x80) ? magnitude : -magnitude);
}
