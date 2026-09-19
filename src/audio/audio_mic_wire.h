#ifndef JOOAN_AUDIO_MIC_WIRE_H
#define JOOAN_AUDIO_MIC_WIRE_H

/* Local guard -> audio-router microphone datagram.
 *
 * All multibyte fields are big-endian:
 *   0..3   "JAGM"
 *   4      version (1)
 *   5      header size (12)
 *   6..7   payload size (320)
 *   8..11  nonzero sequence
 *   12..   320 G.711 A-law samples (16 kHz mono, exactly 20 ms)
 */
#define JOOAN_AUDIO_MIC_MAGIC "JAGM"
#define JOOAN_AUDIO_MIC_VERSION 1U
#define JOOAN_AUDIO_MIC_HEADER_SIZE 12U
#define JOOAN_AUDIO_MIC_PAYLOAD_SIZE 320U
#define JOOAN_AUDIO_MIC_DATAGRAM_SIZE \
    (JOOAN_AUDIO_MIC_HEADER_SIZE + JOOAN_AUDIO_MIC_PAYLOAD_SIZE)
#define JOOAN_AUDIO_MIC_SOCKET_PATH "/run/jooan-local/mic.sock"

#endif
