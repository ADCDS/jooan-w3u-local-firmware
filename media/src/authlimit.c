// SPDX-License-Identifier: MIT
/* Failed-login throttle shared by HTTP Basic, ONVIF WS-Security and RTSP
 * Digest. A peer address that fails JIPC_AUTH_FAILURES times within
 * JIPC_AUTH_WINDOW_US of its first failure is refused, without checking its
 * credentials, until that window ends. Requests that carry no credentials
 * never count: every HTTP and RTSP client starts without them. */
#include "jipc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define SLOTS 32
typedef struct { char peer[INET6_ADDRSTRLEN]; uint64_t since; unsigned failures; } slot;
static slot slots[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static slot *find(const char *peer, uint64_t now, int create)
{
    slot *victim = NULL;
    for (unsigned i = 0; i < SLOTS; ++i) {
        slot *s = &slots[i];
        if (s->peer[0] && now - s->since >= JIPC_AUTH_WINDOW_US) memset(s, 0, sizeof(*s));
        if (s->peer[0] && !strcmp(s->peer, peer)) return s;
        if (!victim || !s->peer[0] || (victim->peer[0] && s->since < victim->since)) victim = s;
    }
    if (!create) return NULL;
    memset(victim, 0, sizeof(*victim));
    snprintf(victim->peer, sizeof(victim->peer), "%s", peer);
    victim->since = now;
    return victim;
}

int jipc_auth_blocked_at(const char *peer, uint64_t now)
{
    pthread_mutex_lock(&lock);
    slot *s = find(peer, now, 0);
    int blocked = s && s->failures >= JIPC_AUTH_FAILURES;
    pthread_mutex_unlock(&lock);
    return blocked;
}

void jipc_auth_result_at(const char *peer, uint64_t now, int ok)
{
    pthread_mutex_lock(&lock);
    slot *s = find(peer, now, !ok);
    if (s && !ok && s->failures < JIPC_AUTH_FAILURES) s->failures++;
    else if (s && ok && s->failures < JIPC_AUTH_FAILURES) memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&lock);
}

static int peer_of(int fd, char *out, size_t capacity)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getpeername(fd, (struct sockaddr *)&address, &length)) return -1;
    if (address.ss_family == AF_INET)
        return inet_ntop(AF_INET, &((struct sockaddr_in *)&address)->sin_addr, out, capacity) ? 0 : -1;
    if (address.ss_family == AF_INET6)
        return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&address)->sin6_addr, out, capacity) ? 0 : -1;
    return -1;
}

int jipc_auth_blocked(int fd)
{
    char peer[INET6_ADDRSTRLEN];
    return !peer_of(fd, peer, sizeof(peer)) && jipc_auth_blocked_at(peer, jipc_now_us());
}

void jipc_auth_result(int fd, int ok)
{
    char peer[INET6_ADDRSTRLEN];
    if (!peer_of(fd, peer, sizeof(peer))) jipc_auth_result_at(peer, jipc_now_us(), ok);
}
