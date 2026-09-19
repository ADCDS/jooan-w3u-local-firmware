#include "audio_guard.h"
#include "audio_guard_wire.h"
#include "audio_wire.h"
#include "g711_alaw.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct vector {
    int16_t pcm;
    uint8_t alaw;
    int16_t decoded;
};

static void test_alaw(void)
{
    static const struct vector vectors[] = {
        { 0,       0xd5, 8 },
        { -1,      0x55, -8 },
        { 16,      0xd4, 24 },
        { -31,     0x54, -24 },
        { 1000,    0xfa, 1008 },
        { -1000,   0x7a, -1008 },
        { 4096,    0x85, 4224 },
        { -4096,   0x1a, -4032 },
        { 32767,   0xaa, 32256 },
        { -32768,  0x2a, -32256 }
    };
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        assert(jooan_alaw_encode_sample(vectors[i].pcm) == vectors[i].alaw);
        assert(jooan_alaw_decode_sample(vectors[i].alaw) == vectors[i].decoded);
    }
}

static void test_wire(void)
{
    uint8_t buffer[64];
    uint8_t payload[] = { 0xd5, 0x55, 0xfa };
    struct jooan_audio_wire_frame frame;
    size_t length = 0;
    uint32_t sequence = 0;

    assert(jooan_audio_wire_build(buffer, sizeof(buffer),
           JOOAN_AUDIO_PTT_PCMA, 0, 1, payload, sizeof(payload), &length) == 0);
    assert(length == JOOAN_AUDIO_WIRE_HEADER_SIZE + sizeof(payload));
    assert(jooan_audio_wire_parse(buffer, length, &frame) == 0);
    assert(frame.type == JOOAN_AUDIO_PTT_PCMA && frame.sequence == 1);
    assert(frame.payload_length == sizeof(payload));
    assert(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    assert(jooan_audio_sequence_accept(&sequence, 1) == 0);
    assert(jooan_audio_sequence_accept(&sequence, 2) == 0);
    errno = 0;
    assert(jooan_audio_sequence_accept(&sequence, 2) == -1 && errno == EPROTO);
    buffer[0] = 'X';
    assert(jooan_audio_wire_parse(buffer, length, &frame) == -1);
}

static void test_guard_wire(void)
{
    uint8_t buffer[JOOAN_AUDIO_GUARD_HEADER_SIZE +
                   JOOAN_AUDIO_SAMPLES_PER_PACKET];
    uint8_t pcma[JOOAN_AUDIO_SAMPLES_PER_PACKET];
    struct jooan_audio_guard_frame frame;
    size_t length = 0;

    memset(pcma, 0xd5, sizeof(pcma));
    assert(jooan_audio_guard_datagram_build(
        buffer, sizeof(buffer), JOOAN_AUDIO_PTT_PCMA,
        UINT64_C(0x1020304050607080), 9, pcma, sizeof(pcma), &length) == 0);
    assert(jooan_audio_guard_datagram_parse(buffer, length, &frame) == 0);
    assert(frame.type == JOOAN_AUDIO_PTT_PCMA);
    assert(frame.session_id == UINT64_C(0x1020304050607080));
    assert(frame.sequence == 9 && frame.payload_length == sizeof(pcma));
    assert(memcmp(frame.payload, pcma, sizeof(pcma)) == 0);
    buffer[22] = 1;
    assert(jooan_audio_guard_datagram_parse(buffer, length, &frame) == -1);
}

static void test_guard_disconnect_release(void)
{
    struct sockaddr_un address;
    struct jooan_audio_guard_client client;
    struct jooan_audio_guard_frame frame;
    uint8_t datagram[JOOAN_AUDIO_GUARD_HEADER_SIZE +
                     JOOAN_AUDIO_SAMPLES_PER_PACKET];
    uint8_t pcma[JOOAN_AUDIO_SAMPLES_PER_PACKET];
    char path[sizeof(address.sun_path)];
    ssize_t length;
    int receiver;
    unsigned i;

    assert(snprintf(path, sizeof(path), "/tmp/jooan-audio-test-%ld.sock",
                    (long)getpid()) > 0);
    unlink(path);
    receiver = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(receiver >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    assert(bind(receiver, (struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1) == 0);

    memset(pcma, 0xd5, sizeof(pcma));
    assert(jooan_audio_guard_open(&client, path, UINT64_C(0x1234)) == 0);
    assert(jooan_audio_guard_acquire(&client, 1) == 0);
    assert(jooan_audio_guard_pcma(&client, 2, pcma, sizeof(pcma)) == 0);
    jooan_audio_guard_close(&client); /* Must emit sequence 3 RELEASE. */

    for (i = 0; i < 3; ++i) {
        length = recv(receiver, datagram, sizeof(datagram), 0);
        assert(length > 0);
        assert(jooan_audio_guard_datagram_parse(
                   datagram, (size_t)length, &frame) == 0);
        assert(frame.session_id == UINT64_C(0x1234));
        assert(frame.sequence == i + 1);
        assert(frame.type == (i == 0 ? JOOAN_AUDIO_PTT_ACQUIRE :
                              i == 1 ? JOOAN_AUDIO_PTT_PCMA :
                                       JOOAN_AUDIO_PTT_RELEASE));
    }
    close(receiver);
    unlink(path);
}

int main(void)
{
    test_alaw();
    test_wire();
    test_guard_wire();
    test_guard_disconnect_release();
    puts("audio tests: ok");
    return 0;
}
