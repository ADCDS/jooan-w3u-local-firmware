#ifndef JOOAN_G711_ALAW_H
#define JOOAN_G711_ALAW_H

#include <stddef.h>
#include <stdint.h>

#define JOOAN_AUDIO_SAMPLE_RATE 16000u
#define JOOAN_AUDIO_PACKET_MS 20u
#define JOOAN_AUDIO_SAMPLES_PER_PACKET 320u

uint8_t jooan_alaw_encode_sample(int16_t pcm);
int16_t jooan_alaw_decode_sample(uint8_t alaw);

void jooan_alaw_encode(const int16_t *pcm, uint8_t *alaw, size_t samples);
void jooan_alaw_decode(const uint8_t *alaw, int16_t *pcm, size_t samples);

#endif
