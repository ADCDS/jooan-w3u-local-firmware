/* SPDX-License-Identifier: MIT
 * G.711 A-law (from its piecewise quantizer definition) and a sample-count
 * audio clock for RTP timestamps.
 */
#include "jipc.h"

#include <math.h>
#include <stdio.h>

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

int jipc_mic_silenced(jipc_app *app)
{
    return jipc_talk_active(app) || atomic_load(&app->mic_muted) || atomic_load(&app->privacy_on);
}

static int centi_db(double amplitude)
{
    if (amplitude < 1) return -9600;
    double db = 20.0 * log10(amplitude / 32768.0);
    return (int)lround(db * 100.0);
}

/* Called by the audio publisher with the stream lock held. */
void jipc_audio_meter_add(jipc_app *app, const unsigned char *pcma, size_t bytes, uint64_t now_us)
{
    if (!app->audio_meter.start_us) app->audio_meter.start_us = now_us;
    for (size_t i = 0; i < bytes; ++i) {
        int sample = jipc_alaw_decode(pcma[i]);
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > app->audio_meter.peak) app->audio_meter.peak = magnitude;
        app->audio_meter.sumsq += (uint64_t)((int64_t)sample * sample);
    }
    app->audio_meter.samples += (uint32_t)bytes;
    if (now_us - app->audio_meter.start_us < UINT64_C(1000000) || !app->audio_meter.samples) return;
    atomic_store(&app->audio_peak_cdb, centi_db(app->audio_meter.peak));
    atomic_store(&app->audio_rms_cdb, centi_db(sqrt((double)app->audio_meter.sumsq / app->audio_meter.samples)));
    atomic_store(&app->audio_level_ms, (unsigned)(now_us / 1000u) | 1u); /* 0 = never */
    app->audio_meter.start_us = now_us;
    app->audio_meter.sumsq = 0;
    app->audio_meter.samples = 0;
    app->audio_meter.peak = 0;
}

int jipc_audio_level_json(jipc_app *app, char *out, size_t capacity)
{
    unsigned at = atomic_load(&app->audio_level_ms);
    if (!at || (unsigned)(jipc_now_us() / 1000u) - at > 3000u)
        return snprintf(out, capacity, "null") >= (int)capacity ? -1 : 0;
    int peak = atomic_load(&app->audio_peak_cdb), rms = atomic_load(&app->audio_rms_cdb);
    int n = snprintf(out, capacity, "{\"peak_dbfs\":%.1f,\"rms_dbfs\":%.1f}", peak / 100.0, rms / 100.0);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
