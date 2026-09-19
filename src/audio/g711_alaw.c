#include "g711_alaw.h"

static unsigned alaw_segment(unsigned value)
{
    static const unsigned ends[8] = {
        0x1fu, 0x3fu, 0x7fu, 0xffu,
        0x1ffu, 0x3ffu, 0x7ffu, 0xfffu
    };
    unsigned segment;

    for (segment = 0; segment < 8; ++segment) {
        if (value <= ends[segment])
            return segment;
    }
    return 8;
}

uint8_t jooan_alaw_encode_sample(int16_t pcm)
{
    unsigned magnitude;
    unsigned segment;
    unsigned value;
    uint8_t mask;

    if (pcm >= 0) {
        magnitude = (unsigned)pcm;
        mask = 0xd5u;
    } else {
        /* Complement before taking the magnitude, as specified by G.711. */
        magnitude = (unsigned)(-(int)pcm - 1);
        mask = 0x55u;
    }
    if (magnitude > 32635u)
        magnitude = 32635u;
    magnitude >>= 3;
    segment = alaw_segment(magnitude);
    if (segment >= 8)
        return (uint8_t)(0x7fu ^ mask);

    value = segment << 4;
    value |= ((segment < 2 ? magnitude >> 1 : magnitude >> segment) & 0x0fu);
    return (uint8_t)(value ^ mask);
}

int16_t jooan_alaw_decode_sample(uint8_t alaw)
{
    unsigned segment;
    int value;

    alaw ^= 0x55u;
    value = (int)(alaw & 0x0fu) << 4;
    segment = (alaw & 0x70u) >> 4;
    if (segment == 0) {
        value += 8;
    } else {
        value += 0x108;
        if (segment > 1)
            value <<= segment - 1;
    }
    return (int16_t)((alaw & 0x80u) ? value : -value);
}

void jooan_alaw_encode(const int16_t *pcm, uint8_t *alaw, size_t samples)
{
    size_t i;

    if (!pcm || !alaw)
        return;
    for (i = 0; i < samples; ++i)
        alaw[i] = jooan_alaw_encode_sample(pcm[i]);
}

void jooan_alaw_decode(const uint8_t *alaw, int16_t *pcm, size_t samples)
{
    size_t i;

    if (!alaw || !pcm)
        return;
    for (i = 0; i < samples; ++i)
        pcm[i] = jooan_alaw_decode_sample(alaw[i]);
}
