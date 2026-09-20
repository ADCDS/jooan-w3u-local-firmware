#define _GNU_SOURCE

#include "audio_guard.h"
#include "audio_guard_wire.h"
#include "audio_wire.h"
#include "g711_alaw.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void test_alaw_boundaries(void)
{
    static const struct {
        int16_t pcm;
        uint8_t encoded;
        int16_t decoded;
    } vectors[] = {
        { 0, 0xd5, 8 }, { -1, 0x55, -8 },
        { 1000, 0xfa, 1008 }, { -1000, 0x7a, -1008 },
        { 32767, 0xaa, 32256 }, { -32768, 0x2a, -32256 }
    };
    size_t i;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        assert(jooan_alaw_encode_sample(vectors[i].pcm) == vectors[i].encoded);
        assert(jooan_alaw_decode_sample(vectors[i].encoded) == vectors[i].decoded);
    }
}

static void test_wire_rejects_malformed_frames(void)
{
    uint8_t storage[JOOAN_AUDIO_WIRE_HEADER_SIZE + 4];
    const uint8_t payload[4] = { 0xd5, 0x55, 0xfa, 0x7a };
    struct jooan_audio_wire_frame frame;
    size_t length = 0;

    assert(jooan_audio_wire_build(storage, sizeof(storage),
           JOOAN_AUDIO_PTT_PCMA, 0, 1, payload, sizeof(payload), &length) == 0);
    assert(length == sizeof(storage));
    assert(jooan_audio_wire_parse(storage, length, &frame) == 0);
    assert(frame.sequence == 1 && frame.payload_length == sizeof(payload));

    errno = 0;
    assert(jooan_audio_wire_parse(storage, JOOAN_AUDIO_WIRE_HEADER_SIZE - 1,
                                  &frame) == -1 && errno == EINVAL);
    storage[0] = 'X';
    errno = 0;
    assert(jooan_audio_wire_parse(storage, length, &frame) == -1 && errno == EPROTO);
    storage[0] = 'J';
    storage[4] = 2;
    assert(jooan_audio_wire_parse(storage, length, &frame) == -1);
    storage[4] = JOOAN_AUDIO_WIRE_VERSION;
    storage[5] = 0x44;
    assert(jooan_audio_wire_parse(storage, length, &frame) == -1);
    storage[5] = JOOAN_AUDIO_PTT_ACQUIRE;
    assert(jooan_audio_wire_parse(storage, length, &frame) == -1);

    errno = 0;
    assert(jooan_audio_wire_build(storage, sizeof(storage),
           JOOAN_AUDIO_PTT_PCMA, 0, 0, payload, sizeof(payload), &length) == -1 &&
           errno == EINVAL);
}

static void test_sequence_is_contiguous_and_wraps(void)
{
    uint32_t last = 0;
    assert(jooan_audio_sequence_accept(&last, 1) == 0);
    assert(jooan_audio_sequence_accept(&last, 2) == 0);
    errno = 0;
    assert(jooan_audio_sequence_accept(&last, 2) == -1 && errno == EPROTO);
    assert(last == 2);
    last = UINT32_MAX;
    assert(jooan_audio_sequence_accept(&last, 1) == 0);
}

static ssize_t receive_frame(int fd, uint8_t *buffer, size_t size)
{
    ssize_t got = recv(fd, buffer, size, 0);
    assert(got >= 24);
    assert(memcmp(buffer, "JAGD", 4) == 0);
    assert(buffer[4] == 1 && be16(buffer + 6) == 0 && be16(buffer + 22) == 0);
    assert((size_t)got == 24u + be16(buffer + 20));
    return got;
}

static void test_guard_session_and_fail_safe_close(void)
{
    char directory[] = "/tmp/jooan-audio-contract.XXXXXX";
    char socket_path[108];
    struct sockaddr_un address;
    struct jooan_audio_guard_client client;
    uint8_t frame[24 + JOOAN_AUDIO_SAMPLES_PER_PACKET];
    uint8_t packet[JOOAN_AUDIO_SAMPLES_PER_PACKET];
    int server;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(socket_path, sizeof(socket_path), "%s/guard.sock", directory) > 0);
    server = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(server >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    assert(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);

    assert(jooan_audio_guard_open(&client, socket_path,
                                  UINT64_C(0x0102030405060708)) == 0);
    assert(jooan_audio_guard_acquire(&client, 1) == 0);
    (void)receive_frame(server, frame, sizeof(frame));
    assert(frame[5] == JOOAN_AUDIO_PTT_ACQUIRE);
    assert(be64(frame + 8) == UINT64_C(0x0102030405060708));
    assert(be32(frame + 16) == 1 && be16(frame + 20) == 0);

    memset(packet, 0xd5, sizeof(packet));
    assert(jooan_audio_guard_pcma(&client, 2, packet, sizeof(packet)) == 0);
    (void)receive_frame(server, frame, sizeof(frame));
    assert(frame[5] == JOOAN_AUDIO_PTT_PCMA);
    assert(be32(frame + 16) == 2);
    assert(be16(frame + 20) == JOOAN_AUDIO_SAMPLES_PER_PACKET);

    assert(jooan_audio_guard_speaker_lease(&client, 3) == 0);
    (void)receive_frame(server, frame, sizeof(frame));
    assert(frame[5] == JOOAN_AUDIO_GUARD_SPEAKER_LEASE);
    assert(be32(frame + 16) == 3 && be16(frame + 20) == 0);

    errno = 0;
    assert(jooan_audio_guard_pcma(&client, 4, packet, sizeof(packet) - 1) == -1 &&
           errno == EINVAL);
    jooan_audio_guard_close(&client);
    (void)receive_frame(server, frame, sizeof(frame));
    assert(frame[5] == JOOAN_AUDIO_PTT_RELEASE);
    assert(be32(frame + 16) == 4 && be16(frame + 20) == 0);
    assert(client.fd == -1 && client.acquired == 0);
    jooan_audio_guard_close(&client);

    close(server);
    unlink(socket_path);
    assert(rmdir(directory) == 0);
}

int main(void)
{
    test_alaw_boundaries();
    test_wire_rejects_malformed_frames();
    test_sequence_is_contiguous_and_wraps();
    test_guard_session_and_fail_safe_close();
    puts("audio-contract: PASS");
    return 0;
}
