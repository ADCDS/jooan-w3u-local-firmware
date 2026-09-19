#ifndef JOOAN_AUDIO_GUARD_WIRE_H
#define JOOAN_AUDIO_GUARD_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define JOOAN_AUDIO_GUARD_HEADER_SIZE 24u
/* ACQUIRE is a lease, refreshed by each valid 20 ms PCMA datagram. The guard
 * must force amplifier/playback release when this interval expires, covering
 * daemon crashes where no RELEASE datagram can be emitted. */
#define JOOAN_AUDIO_GUARD_LEASE_MS 500u

struct jooan_audio_guard_frame {
    uint8_t type;
    uint64_t session_id;
    uint32_t sequence;
    const uint8_t *payload;
    uint16_t payload_length;
};

int jooan_audio_guard_datagram_build(void *output, size_t capacity,
                                     uint8_t type, uint64_t session_id,
                                     uint32_t sequence, const void *payload,
                                     uint16_t payload_length,
                                     size_t *output_length);
int jooan_audio_guard_datagram_parse(const void *data, size_t length,
                                     struct jooan_audio_guard_frame *frame);

#endif
