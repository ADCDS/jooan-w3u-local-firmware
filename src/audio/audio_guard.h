#ifndef JOOAN_AUDIO_GUARD_H
#define JOOAN_AUDIO_GUARD_H

#include <stddef.h>
#include <stdint.h>

struct jooan_audio_guard_client {
    int fd;
    uint64_t session_id;
    uint32_t last_sequence;
    int acquired;
};

int jooan_audio_guard_open(struct jooan_audio_guard_client *client,
                           const char *socket_path, uint64_t session_id);
int jooan_audio_guard_acquire(struct jooan_audio_guard_client *client,
                              uint32_t sequence);
int jooan_audio_guard_pcma(struct jooan_audio_guard_client *client,
                           uint32_t sequence, const uint8_t *pcma,
                           size_t length);
int jooan_audio_guard_release(struct jooan_audio_guard_client *client,
                              uint32_t sequence);

/* Always attempts RELEASE when the session owns talk. Safe to call repeatedly.
 * A caller must invoke this on WebSocket close, protocol error and timeout. */
void jooan_audio_guard_close(struct jooan_audio_guard_client *client);

#endif
