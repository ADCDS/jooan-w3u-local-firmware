#include "audio_wire.h"

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

static int valid_type(uint8_t type)
{
    switch (type) {
    case JOOAN_AUDIO_PTT_ACQUIRE:
    case JOOAN_AUDIO_PTT_PCMA:
    case JOOAN_AUDIO_PTT_RELEASE:
    case JOOAN_AUDIO_MIC_PCMA:
    case JOOAN_AUDIO_STATE:
    case JOOAN_AUDIO_ERROR:
        return 1;
    default:
        return 0;
    }
}

int jooan_audio_wire_parse(const void *data, size_t length,
                           struct jooan_audio_wire_frame *frame)
{
    const uint8_t *p = data;
    uint16_t payload_length;

    if (!p || !frame || length < JOOAN_AUDIO_WIRE_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }
    if (memcmp(p, "JAUD", 4) != 0 || p[4] != JOOAN_AUDIO_WIRE_VERSION ||
        p[7] != JOOAN_AUDIO_WIRE_HEADER_SIZE || !valid_type(p[5]) ||
        read_be32(p + 8) == 0 ||
        read_be16(p + 14) != 0) {
        errno = EPROTO;
        return -1;
    }
    payload_length = read_be16(p + 12);
    if (payload_length > JOOAN_AUDIO_WIRE_MAX_PAYLOAD ||
        length != JOOAN_AUDIO_WIRE_HEADER_SIZE + payload_length) {
        errno = EMSGSIZE;
        return -1;
    }
    if ((p[5] == JOOAN_AUDIO_PTT_ACQUIRE ||
         p[5] == JOOAN_AUDIO_PTT_RELEASE) && payload_length != 0) {
        errno = EPROTO;
        return -1;
    }
    if ((p[5] == JOOAN_AUDIO_PTT_PCMA || p[5] == JOOAN_AUDIO_MIC_PCMA) &&
        payload_length == 0) {
        errno = EPROTO;
        return -1;
    }

    frame->type = p[5];
    frame->flags = p[6];
    frame->sequence = read_be32(p + 8);
    frame->payload = p + JOOAN_AUDIO_WIRE_HEADER_SIZE;
    frame->payload_length = payload_length;
    return 0;
}

int jooan_audio_wire_build(void *output, size_t capacity, uint8_t type,
                           uint8_t flags, uint32_t sequence,
                           const void *payload, uint16_t payload_length,
                           size_t *output_length)
{
    uint8_t *p = output;
    struct jooan_audio_wire_frame ignored;
    size_t length = JOOAN_AUDIO_WIRE_HEADER_SIZE + payload_length;

    if (!p || sequence == 0 || payload_length > JOOAN_AUDIO_WIRE_MAX_PAYLOAD ||
        length > capacity || (payload_length && !payload)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(p, "JAUD", 4);
    p[4] = JOOAN_AUDIO_WIRE_VERSION;
    p[5] = type;
    p[6] = flags;
    p[7] = JOOAN_AUDIO_WIRE_HEADER_SIZE;
    write_be32(p + 8, sequence);
    write_be16(p + 12, payload_length);
    write_be16(p + 14, 0);
    if (payload_length)
        memcpy(p + JOOAN_AUDIO_WIRE_HEADER_SIZE, payload, payload_length);
    if (jooan_audio_wire_parse(p, length, &ignored) != 0)
        return -1;
    if (output_length)
        *output_length = length;
    return 0;
}

int jooan_audio_sequence_accept(uint32_t *last_sequence, uint32_t candidate)
{
    uint32_t expected;

    if (!last_sequence || candidate == 0) {
        errno = EINVAL;
        return -1;
    }
    expected = *last_sequence == UINT32_MAX ? 1u : *last_sequence + 1u;
    if (candidate != expected) {
        errno = EPROTO;
        return -1;
    }
    *last_sequence = candidate;
    return 0;
}
