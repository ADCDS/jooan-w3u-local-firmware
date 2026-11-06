/* SPDX-License-Identifier: MIT
 * The siren's sounds, as 8 kHz A-law for the speaker service. No samples are stored:
 * each sound is a tone with a frequency and a level that depend on the time.
 */
#include "jipc.h"
#include <stdint.h>
#include <string.h>

#define RATE 8000u
#define FULL 26000     /* near full scale, leaving the DAC's unity gain unclipped */
#define SOFT 11000
#define RAMP (RATE / 200u) /* 5 ms, so that a tone starts and stops without a click */

static const char *const names[JIPC_SIREN_SOUNDS] = {"alarm", "two_tone", "pulse", "chime"};

const char *jipc_siren_name(unsigned sound)
{
    return sound < JIPC_SIREN_SOUNDS ? names[sound] : NULL;
}

int jipc_siren_sound(const char *name)
{
    for (unsigned i = 0; name && i < JIPC_SIREN_SOUNDS; ++i)
        if (!strcmp(name, names[i])) return (int)i;
    return -1;
}

void jipc_siren_init(jipc_siren *g, unsigned sound)
{
    memset(g, 0, sizeof *g);
    g->sound = sound < JIPC_SIREN_SOUNDS ? sound : 0;
}

/* A sine from two parabolas: its harmonics are 40 dB down, and it needs no table. */
static int wave(uint16_t phase)
{
    int half = phase >= 32768u, x = (int)(phase & 32767u);
    int y = (int)(((int64_t)x * (32768 - x)) >> 13);
    return half ? -y : y;
}

static unsigned ramp(unsigned t, unsigned length)
{
    unsigned edge = t < length - t ? t : length - t;
    return edge >= RAMP ? RAMP : edge;
}

/* The tone at sample t of the sound's cycle. */
static void tone(unsigned sound, uint32_t t, unsigned *hz, int *level)
{
    switch (sound) {
    case 1: { /* Two-tone: half a second each, high and low */
        unsigned at = t % RATE, high = at < RATE / 2, within = at % (RATE / 2);
        *hz = high ? 1100 : 800;
        *level = (int)(FULL * ramp(within, RATE / 2) / RAMP);
        break;
    }
    case 2: { /* Pulse: short beeps, 150 ms on and 150 ms off */
        unsigned length = RATE * 150 / 1000, at = t % (2 * length);
        *hz = 1500;
        *level = at < length ? (int)(FULL * ramp(at, length) / RAMP) : 0;
        break;
    }
    case 3: { /* Chime: two soft notes that die away, then a rest */
        unsigned first = RATE * 6 / 10, second = RATE * 8 / 10, at = t % (RATE * 24 / 10);
        unsigned length = at < first ? first : second, within = at < first ? at : at - first;
        *hz = at < first ? 880 : 660;
        if (at >= first + second) { *level = 0; break; }
        /* falling as a square, from the note's start to its end */
        uint32_t left = length - within;
        int rise = within < RAMP ? (int)within : (int)RAMP;
        *level = (int)((uint64_t)SOFT * left * left / ((uint64_t)length * length) * (unsigned)rise / RAMP);
        break;
    }
    default: { /* Alarm: rises and falls between 500 and 1500 Hz, every 2 s */
        unsigned period = 2 * RATE, at = t % period, up = at < period / 2 ? at : period - at;
        *hz = 500 + (unsigned)((uint64_t)1000 * up / (period / 2));
        *level = FULL;
        break;
    }
    }
}

void jipc_siren_fill(jipc_siren *g, unsigned char *pcma, size_t samples)
{
    for (size_t i = 0; i < samples; ++i) {
        unsigned hz;
        int level;
        tone(g->sound, g->t, &hz, &level);
        /* the first samples of the very first cycle fade in, whatever the sound */
        if (g->t < RAMP) level = level * (int)g->t / (int)RAMP;
        g->phase = (uint16_t)(g->phase + (uint32_t)hz * 65536u / RATE);
        pcma[i] = jipc_alaw_encode((int)(((int64_t)wave(g->phase) * level) >> 15));
        ++g->t;
    }
}
