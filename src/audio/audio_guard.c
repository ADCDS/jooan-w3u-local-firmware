#define _GNU_SOURCE
#include "audio_guard.h"

#include "audio_guard_wire.h"
#include "audio_wire.h"
#include "g711_alaw.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int send_guard(struct jooan_audio_guard_client *client, uint8_t type,
                      uint32_t sequence, const uint8_t *payload, size_t length)
{
    uint8_t datagram[JOOAN_AUDIO_GUARD_HEADER_SIZE +
                     JOOAN_AUDIO_SAMPLES_PER_PACKET];
    size_t datagram_length;
    ssize_t sent;

    if (!client || client->fd < 0 || sequence == 0 ||
        length > JOOAN_AUDIO_WIRE_MAX_PAYLOAD || (length && !payload)) {
        errno = EINVAL;
        return -1;
    }
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), type, client->session_id, sequence,
            payload, (uint16_t)length, &datagram_length) != 0)
        return -1;
    sent = send(client->fd, datagram, datagram_length, MSG_NOSIGNAL);
    if (sent != (ssize_t)datagram_length) {
        if (sent >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

int jooan_audio_guard_open(struct jooan_audio_guard_client *client,
                           const char *socket_path, uint64_t session_id)
{
    struct sockaddr_un address;
    size_t path_length;
    int fd;

    if (!client || !socket_path || session_id == 0) {
        errno = EINVAL;
        return -1;
    }
    path_length = strlen(socket_path);
    if (path_length == 0 || path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1);

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + path_length + 1) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    client->fd = fd;
    client->session_id = session_id;
    return 0;
}

int jooan_audio_guard_acquire(struct jooan_audio_guard_client *client,
                              uint32_t sequence)
{
    uint32_t accepted;

    if (!client || client->acquired) {
        errno = EALREADY;
        return -1;
    }
    accepted = client->last_sequence;
    if (jooan_audio_sequence_accept(&accepted, sequence) != 0 ||
        send_guard(client, JOOAN_AUDIO_PTT_ACQUIRE, sequence, NULL, 0) != 0)
        return -1;
    client->last_sequence = accepted;
    client->acquired = 1;
    return 0;
}

int jooan_audio_guard_pcma(struct jooan_audio_guard_client *client,
                           uint32_t sequence, const uint8_t *pcma,
                           size_t length)
{
    uint32_t accepted;

    if (!client || !client->acquired) {
        errno = EPERM;
        return -1;
    }
    if (!pcma || length != JOOAN_AUDIO_SAMPLES_PER_PACKET) {
        errno = EINVAL;
        return -1;
    }
    accepted = client->last_sequence;
    if (jooan_audio_sequence_accept(&accepted, sequence) != 0 ||
        send_guard(client, JOOAN_AUDIO_PTT_PCMA, sequence, pcma, length) != 0)
        return -1;
    client->last_sequence = accepted;
    return 0;
}

int jooan_audio_guard_speaker_lease(struct jooan_audio_guard_client *client,
                                    uint32_t sequence)
{
    uint32_t accepted;

    if (!client || !client->acquired) {
        errno = EPERM;
        return -1;
    }
    accepted = client->last_sequence;
    if (jooan_audio_sequence_accept(&accepted, sequence) != 0 ||
        send_guard(client, JOOAN_AUDIO_GUARD_SPEAKER_LEASE,
                   sequence, NULL, 0) != 0)
        return -1;
    client->last_sequence = accepted;
    return 0;
}

int jooan_audio_guard_release(struct jooan_audio_guard_client *client,
                              uint32_t sequence)
{
    uint32_t accepted;
    int result;

    if (!client) {
        errno = EINVAL;
        return -1;
    }
    if (!client->acquired)
        return 0;
    if (sequence == 0)
        sequence = client->last_sequence == UINT32_MAX ? 1u :
                   client->last_sequence + 1u;
    accepted = client->last_sequence;
    if (jooan_audio_sequence_accept(&accepted, sequence) != 0)
        return -1;
    result = send_guard(client, JOOAN_AUDIO_PTT_RELEASE, sequence, NULL, 0);
    /* Fail safe locally even if the receiver vanished. The guard must also
     * expire ownership by session heartbeat/lease. */
    client->acquired = 0;
    client->last_sequence = accepted;
    return result;
}

void jooan_audio_guard_close(struct jooan_audio_guard_client *client)
{
    if (!client)
        return;
    if (client->fd >= 0) {
        (void)jooan_audio_guard_release(client, 0);
        close(client->fd);
    }
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}
