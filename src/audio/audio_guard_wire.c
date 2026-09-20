#include "audio_guard_wire.h"

#include "audio_wire.h"
#include "g711_alaw.h"

#include <errno.h>
#include <string.h>

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void write_be64(uint8_t *p, uint64_t value)
{
    write_be32(p, (uint32_t)(value >> 32));
    write_be32(p + 4, (uint32_t)value);
}

static int valid_guard_payload(uint8_t type, uint16_t length)
{
    if (type == JOOAN_AUDIO_PTT_ACQUIRE || type == JOOAN_AUDIO_PTT_RELEASE)
        return length == 0;
    if (type == JOOAN_AUDIO_GUARD_SPEAKER_LEASE)
        return length == 0;
    if (type == JOOAN_AUDIO_PTT_PCMA)
        return length == JOOAN_AUDIO_SAMPLES_PER_PACKET;
    return 0;
}

int jooan_audio_guard_datagram_build(void *output, size_t capacity,
                                     uint8_t type, uint64_t session_id,
                                     uint32_t sequence, const void *payload,
                                     uint16_t payload_length,
                                     size_t *output_length)
{
    uint8_t *p = output;
    size_t length = JOOAN_AUDIO_GUARD_HEADER_SIZE + payload_length;

    if (!p || session_id == 0 || sequence == 0 || length > capacity ||
        !valid_guard_payload(type, payload_length) ||
        (payload_length && !payload)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(p, "JAGD", 4);
    p[4] = 1;
    p[5] = type;
    write_be16(p + 6, 0);
    write_be64(p + 8, session_id);
    write_be32(p + 16, sequence);
    write_be16(p + 20, payload_length);
    write_be16(p + 22, 0);
    if (payload_length)
        memcpy(p + JOOAN_AUDIO_GUARD_HEADER_SIZE, payload, payload_length);
    if (output_length)
        *output_length = length;
    return 0;
}

int jooan_audio_guard_datagram_parse(const void *data, size_t length,
                                     struct jooan_audio_guard_frame *frame)
{
    const uint8_t *p = data;
    uint16_t payload_length;

    if (!p || !frame || length < JOOAN_AUDIO_GUARD_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }
    payload_length = read_be16(p + 20);
    if (memcmp(p, "JAGD", 4) != 0 || p[4] != 1 || read_be16(p + 6) != 0 ||
        read_be16(p + 22) != 0 ||
        length != JOOAN_AUDIO_GUARD_HEADER_SIZE + payload_length ||
        !valid_guard_payload(p[5], payload_length) ||
        read_be64(p + 8) == 0 || read_be32(p + 16) == 0) {
        errno = EPROTO;
        return -1;
    }
    frame->type = p[5];
    frame->session_id = read_be64(p + 8);
    frame->sequence = read_be32(p + 16);
    frame->payload = p + JOOAN_AUDIO_GUARD_HEADER_SIZE;
    frame->payload_length = payload_length;
    return 0;
}
