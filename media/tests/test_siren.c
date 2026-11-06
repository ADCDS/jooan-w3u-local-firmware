/* SPDX-License-Identifier: MIT
 * The siren's four sounds, measured from what they produce. */
#include "jipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 8000u

static int *render(unsigned sound, unsigned seconds)
{
    size_t n = RATE * (size_t)seconds;
    unsigned char *code = malloc(n);
    int *out = malloc(n * sizeof *out);
    jipc_siren g;
    assert(code && out);
    jipc_siren_init(&g, sound);
    /* in frames, as the speaker service asks for them */
    for (size_t at = 0; at < n; at += JIPC_SPEAKER_FRAME)
        jipc_siren_fill(&g, code + at, n - at < JIPC_SPEAKER_FRAME ? n - at : JIPC_SPEAKER_FRAME);
    for (size_t i = 0; i < n; ++i) out[i] = jipc_alaw_decode(code[i]);
    free(code);
    return out;
}

/* Hz between two times, from upward zero crossings. */
static unsigned hz(const int *s, double from, double to)
{
    size_t a = (size_t)(from * RATE), b = (size_t)(to * RATE);
    unsigned crossings = 0;
    for (size_t i = a + 1; i < b; ++i) if (s[i - 1] < 0 && s[i] >= 0) ++crossings;
    return (unsigned)((double)crossings / (to - from) + 0.5);
}

static int peak(const int *s, double from, double to)
{
    int top = 0;
    for (size_t i = (size_t)(from * RATE); i < (size_t)(to * RATE); ++i) top = abs(s[i]) > top ? abs(s[i]) : top;
    return top;
}

#define NEAR(value, want, within) ((int)(value) >= (int)(want) - (int)(within) && (int)(value) <= (int)(want) + (int)(within))

static void names(void)
{
    for (unsigned i = 0; i < JIPC_SIREN_SOUNDS; ++i) assert(jipc_siren_sound(jipc_siren_name(i)) == (int)i);
    assert(jipc_siren_sound("bell") < 0 && jipc_siren_sound(NULL) < 0 && !jipc_siren_name(JIPC_SIREN_SOUNDS));
}

static void alarm(void)
{
    int *s = render(0, 4);
    /* It rises for a second and falls for a second, between 500 and 1500 Hz. */
    assert(NEAR(hz(s, 0.00, 0.10), 550, 40));
    assert(NEAR(hz(s, 0.45, 0.55), 1000, 40));
    assert(NEAR(hz(s, 0.90, 1.00), 1450, 40));
    assert(NEAR(hz(s, 1.45, 1.55), 1000, 40));
    assert(NEAR(hz(s, 2.45, 2.55), 1000, 40));
    /* Loud, and never clipped. */
    assert(NEAR(peak(s, 0.5, 3.5), 26000, 1500) && peak(s, 0, 4) < 32000);
    free(s);
}

static void two_tone(void)
{
    int *s = render(1, 3);
    assert(NEAR(hz(s, 0.05, 0.45), 1100, 15));
    assert(NEAR(hz(s, 0.55, 0.95), 800, 15));
    assert(NEAR(hz(s, 1.05, 1.45), 1100, 15));
    assert(NEAR(peak(s, 0.1, 0.4), 26000, 1500));
    /* Each half fades at its edges, so the change does not click. */
    assert(abs(s[RATE / 2 - 1]) < 2000);
    free(s);
}

static void pulse(void)
{
    int *s = render(2, 2);
    /* 150 ms on, 150 ms off. */
    assert(NEAR(hz(s, 0.02, 0.13), 1500, 40) && NEAR(peak(s, 0.02, 0.13), 26000, 1500));
    assert(peak(s, 0.16, 0.29) < 16);
    assert(NEAR(peak(s, 0.32, 0.43), 26000, 1500));
    assert(peak(s, 0.46, 0.59) < 16);
    free(s);
}

static void chime(void)
{
    int *s = render(3, 5);
    /* Two notes, the second lower; soft, dying away, and a rest. */
    assert(NEAR(hz(s, 0.05, 0.25), 880, 30));
    assert(NEAR(hz(s, 0.65, 0.95), 660, 30));
    assert(peak(s, 0.0, 2.4) <= 12000);
    assert(peak(s, 0.02, 0.1) > 2 * peak(s, 0.4, 0.5));
    assert(peak(s, 1.45, 2.35) < 16);
    /* Then it starts again. */
    assert(NEAR(hz(s, 2.45, 2.65), 880, 30));
    free(s);
}

static void continuous(void)
{
    /* However it is cut into frames, the sound is the same. */
    jipc_siren a, b;
    unsigned char one[3200], two[3200];
    jipc_siren_init(&a, 0);
    jipc_siren_init(&b, 0);
    jipc_siren_fill(&a, one, sizeof one);
    for (size_t at = 0; at < sizeof two; at += 7)
        jipc_siren_fill(&b, two + at, sizeof two - at < 7 ? sizeof two - at : 7);
    assert(!memcmp(one, two, sizeof one));
    /* It starts from nothing, and an unknown sound is the alarm. */
    assert(abs(jipc_alaw_decode(one[0])) < 64);
    jipc_siren_init(&b, 99);
    assert(b.sound == 0);
}

int main(void)
{
    names();
    alarm();
    two_tone();
    pulse();
    chime();
    continuous();
    puts("siren: PASS");
    return 0;
}
