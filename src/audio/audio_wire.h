#ifndef JOOAN_AUDIO_WIRE_H
#define JOOAN_AUDIO_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define JOOAN_AUDIO_WIRE_HEADER_SIZE 16u
#define JOOAN_AUDIO_WIRE_MAX_PAYLOAD 1024u
#define JOOAN_AUDIO_WIRE_VERSION 1u
/* The browser offers both subprotocols. The server must validate the one-time
 * base64url token following AUTH_PREFIX before selecting WS_PROTOCOL. This
 * keeps credentials out of URLs, logs and binary audio frames. */
#define JOOAN_AUDIO_WS_PROTOCOL "jaud.v1"
#define JOOAN_AUDIO_WS_AUTH_PREFIX "jaud.auth."
#define JOOAN_AUDIO_AUTH_TOKEN_HEX_LENGTH 64u
#define JOOAN_AUDIO_MAX_TALK_MS 60000u
#define JOOAN_AUDIO_DISCONNECT_DEADMAN_MS 2000u

enum jooan_audio_state_code {
    JOOAN_AUDIO_STATE_ACQUIRED = 1,
    JOOAN_AUDIO_STATE_RELEASED = 2,
    JOOAN_AUDIO_STATE_LEASE_EXPIRED = 3
};

enum jooan_audio_wire_type {
    JOOAN_AUDIO_PTT_ACQUIRE = 1,
    JOOAN_AUDIO_PTT_PCMA = 2,
    JOOAN_AUDIO_PTT_RELEASE = 3,
    JOOAN_AUDIO_MIC_PCMA = 0x81,
    JOOAN_AUDIO_STATE = 0x82,
    JOOAN_AUDIO_ERROR = 0xff
};

struct jooan_audio_wire_frame {
    uint8_t type;
    uint8_t flags;
    uint32_t sequence;
    const uint8_t *payload;
    uint16_t payload_length;
};

int jooan_audio_wire_parse(const void *data, size_t length,
                           struct jooan_audio_wire_frame *frame);
int jooan_audio_wire_build(void *output, size_t capacity, uint8_t type,
                           uint8_t flags, uint32_t sequence,
                           const void *payload, uint16_t payload_length,
                           size_t *output_length);

/* WebSockets are ordered: gaps, duplicates and a first sequence other than 1
 * indicate a stale or corrupted talk session and must fail closed. */
int jooan_audio_sequence_accept(uint32_t *last_sequence, uint32_t candidate);

/* Parse the exact two-protocol offer. When expected_token is non-NULL, compare
 * the 64-character lowercase-hex auth value in constant time. The HTTPS daemon
 * owns that expected CSRF value; syntax-only validation in a downstream Unix
 * router is not authentication. */
int jooan_audio_ws_protocol_validate(const char *offered,
                                     const char *expected_token);

#endif
