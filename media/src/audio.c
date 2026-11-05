/* SPDX-License-Identifier: MIT
 * G.711 A-law (from its piecewise quantizer definition) and a sample-count
 * audio clock for RTP timestamps.
 */
#include "jipc.h"

unsigned char jipc_alaw_encode(int sample)
{
    unsigned mask = sample >= 0 ? 0xd5 : 0x55;
    unsigned magnitude = sample >= 0 ? (unsigned)sample : (unsigned)(-sample - 1);
    if (magnitude > 32767) magnitude = 32767;
    unsigned segment = 0;
    for (unsigned threshold = 256; segment < 7 && magnitude >= threshold; threshold <<= 1) ++segment;
    unsigned quantized = segment ? magnitude >> (segment + 3) : magnitude >> 4;
    return (unsigned char)(((segment << 4) | (quantized & 15)) ^ mask);
}

int jipc_alaw_decode(unsigned char code)
{
    unsigned value = code ^ 0x55;
    unsigned segment = (value >> 4) & 7;
    int magnitude = (int)((value & 15) << 4) + 8;
    if (segment) magnitude += 256;
    if (segment > 1) magnitude <<= segment - 1;
    return value & 128 ? magnitude : -magnitude;
}

void jipc_audio_clock_init(jipc_audio_clock *clock, unsigned rate)
{
    clock->anchor_us = clock->samples = 0;
    clock->rate = rate ? rate : 8000;
    clock->started = 0;
}

static uint64_t samples_us(uint64_t samples, unsigned rate)
{
    return samples / rate * 1000000u + samples % rate * 1000000u / rate;
}

/* Ahead means stamping this frame would put the timeline more than 150 ms
 * past now: a burst of buffered frames after a stall, or a capture clock
 * faster than ours. The caller drops the frame instead of repeating time. */
int jipc_audio_clock_ahead(const jipc_audio_clock *clock, uint64_t now_us, unsigned samples)
{
    if (!clock->started) return 0;
    uint64_t end = clock->anchor_us + samples_us(clock->samples + samples, clock->rate);
    return end > now_us + UINT64_C(150000);
}

uint64_t jipc_audio_clock_stamp(jipc_audio_clock *clock, uint64_t now_us,
                                unsigned samples, int *discontinuity)
{
    uint64_t duration = samples_us(samples, clock->rate);
    int restart = !clock->started;
    if (clock->started) {
        /* The frame just captured should end about now. Only a late frame
         * (a stall) moves the anchor, and only forward, so timestamps never
         * repeat or go back. */
        uint64_t end = clock->anchor_us + samples_us(clock->samples + samples, clock->rate);
        restart = now_us > end + UINT64_C(150000);
    }
    if (restart) {
        clock->anchor_us = now_us > duration ? now_us - duration : 0;
        clock->samples = 0;
        clock->started = 1;
    }
    if (discontinuity) *discontinuity = restart;
    uint64_t pts = clock->anchor_us + samples_us(clock->samples, clock->rate);
    clock->samples += samples;
    return pts;
}
