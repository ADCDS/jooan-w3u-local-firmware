#include "guard.h"
#include "guard_sha256.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void test_hostnames(void)
{
    assert(jooan_guard_hostname_is_approved(JOOAN_GUARD_API_HOST));
    assert(jooan_guard_hostname_is_approved("USE1MQTT01.JOOANIOT.COM."));
    assert(!jooan_guard_hostname_is_approved("evil.example"));
    assert(jooan_guard_hostname_is_loopback("localhost"));
    assert(jooan_guard_hostname_is_loopback("127.99.1.2"));
    assert(jooan_guard_hostname_is_loopback("::1"));
    assert(!jooan_guard_hostname_is_loopback("192.0.2.1"));
}

static void test_addresses(void)
{
    struct sockaddr_in address4;
    struct sockaddr_in6 address6;
    struct sockaddr local_family;

    memset(&address4, 0, sizeof(address4));
    address4.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &address4.sin_addr) == 1);
    assert(!jooan_guard_sockaddr_is_loopback((struct sockaddr *)&address4, 0));
    assert(!jooan_guard_sockaddr_is_loopback((struct sockaddr *)&address4, 1));
    assert(jooan_guard_sockaddr_is_loopback((struct sockaddr *)&address4,
                                             sizeof(address4)));
    assert(inet_pton(AF_INET, "192.0.2.1", &address4.sin_addr) == 1);
    assert(!jooan_guard_sockaddr_is_loopback((struct sockaddr *)&address4,
                                              sizeof(address4)));

    memset(&address6, 0, sizeof(address6));
    address6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::1", &address6.sin6_addr) == 1);
    assert(jooan_guard_sockaddr_is_loopback((struct sockaddr *)&address6,
                                             sizeof(address6)));
#ifdef AF_ALG
    memset(&local_family, 0, sizeof(local_family));
    local_family.sa_family = AF_ALG;
    assert(jooan_guard_sockaddr_is_loopback(&local_family,
                                             sizeof(local_family)));
#endif
}

static void test_alaw(void)
{
    assert(jooan_guard_alaw_to_pcm16(0xd5) == 8);
    assert(jooan_guard_alaw_to_pcm16(0x55) == -8);
    assert(jooan_guard_alaw_to_pcm16(0x80) == 5504);
    assert(jooan_guard_alaw_to_pcm16(0x00) == -5504);
}

static void test_reply_ports(void)
{
    assert(jooan_guard_listener_reply_port_allowed(554));
    assert(jooan_guard_listener_reply_port_allowed(8899));
    assert(jooan_guard_listener_reply_port_allowed(9898));
    assert(jooan_guard_listener_reply_port_allowed(24569));
    assert(!jooan_guard_listener_reply_port_allowed(443));
    assert(!jooan_guard_listener_reply_port_allowed(1883));
    assert(jooan_guard_listener_bind_external_allowed(554, SOCK_STREAM));
    assert(!jooan_guard_listener_bind_external_allowed(554, SOCK_DGRAM));
    assert(!jooan_guard_listener_bind_external_allowed(8899, SOCK_STREAM));
}

static void test_sha256(void)
{
    char digest[65];

    assert(jooan_guard_sha256_bytes_hex("abc", 3, digest) == 0);
    assert(strcmp(digest,
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
}

int main(void)
{
    test_hostnames();
    test_addresses();
    test_alaw();
    test_reply_ports();
    test_sha256();
    puts("guard unit tests: ok");
    return 0;
}
