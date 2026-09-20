#define _POSIX_C_SOURCE 200809L

#include "audio_guard.h"
#include "audio_mic_wire.h"
#include "audio_wire.h"
#include "g711_alaw.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WS_SOCKET "/run/jooan-local/audio-ws.sock"
#define DEFAULT_MIC_SOCKET JOOAN_AUDIO_MIC_SOCKET_PATH
#define DEFAULT_GUARD_SOCKET "/tmp/jooan-guard-talkback.sock"
#define DEFAULT_MIC_DRIVER "/dev/dsp"
#define DEFAULT_SPEAKER_DRIVER "/dev/dsp"
#define SPEAKER_VALUE_PATH "/sys/class/gpio/gpio63/value"
#define SPEAKER_ENABLED_LEVEL '0'

#define AMIC_AI_SET_PARAM 0x40085071UL
#define AMIC_AI_ENABLE_STREAM 0x40045060UL
#define AMIC_AI_DISABLE_STREAM 0x40045061UL
#define AMIC_AI_GET_STREAM 0x40145062UL
#define AMIC_AO_SET_PARAM 0x4008506fUL
#define AMIC_AO_ENABLE_STREAM 0x4004505eUL
#define AMIC_AO_DISABLE_STREAM 0x4004505fUL
#define AMIC_AO_PLAY_STREAM 0x40085063UL

#define MAX_CLIENTS 16
#define INPUT_CAPACITY 8192u
#define OUTPUT_CAPACITY 16384u
#define MAX_WS_PAYLOAD (JOOAN_AUDIO_WIRE_HEADER_SIZE + JOOAN_AUDIO_WIRE_MAX_PAYLOAD)
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum client_state {
    CLIENT_HTTP = 0,
    CLIENT_OPEN = 1
};

struct client {
    int fd;
    enum client_state state;
    int allow_talk;
    uint8_t input[INPUT_CAPACITY];
    size_t input_length;
    uint8_t output[OUTPUT_CAPACITY];
    size_t output_offset;
    size_t output_length;
    uint32_t receive_sequence;
    uint32_t transmit_sequence;
    uint64_t session_id;
    uint64_t talk_deadline;
    struct jooan_audio_guard_client guard;
};

struct sha1_context {
    uint32_t state[5];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
};

static volatile sig_atomic_t stopping;
static const char *guard_socket_path;
static uint32_t maximum_talk_ms = JOOAN_AUDIO_MAX_TALK_MS;
static pthread_t microphone_thread;
static int microphone_thread_started;
static int speaker_output_descriptor = -1;
static int speaker_output_direct;
static int speaker_talk_logged;

struct microphone_driver_parameter {
    uint32_t sample_rate;
    uint16_t sample_bits;
    uint16_t channels;
};

struct microphone_driver_stream {
    uintptr_t pcm;
    uint32_t byte_length;
    uintptr_t reference_pcm;
    uint32_t reference_samples;
    uint32_t timeout_ms;
};

struct speaker_driver_stream {
    uintptr_t pcm;
    uint32_t byte_length;
    uint32_t reserved[3];
};

struct microphone_capture_config {
    char device[128];
    struct sockaddr_un destination;
    socklen_t destination_length;
};

static struct microphone_capture_config microphone_capture;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void store_be16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void store_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void microphone_capture_emit(int sender, uint32_t *sequence,
                                    const uint8_t *pcma)
{
    uint8_t datagram[JOOAN_AUDIO_MIC_DATAGRAM_SIZE];

    *sequence = *sequence == UINT32_MAX ? 1U : *sequence + 1U;
    memcpy(datagram, JOOAN_AUDIO_MIC_MAGIC, 4);
    datagram[4] = JOOAN_AUDIO_MIC_VERSION;
    datagram[5] = JOOAN_AUDIO_MIC_HEADER_SIZE;
    store_be16(datagram + 6, JOOAN_AUDIO_MIC_PAYLOAD_SIZE);
    store_be32(datagram + 8, *sequence);
    memcpy(datagram + JOOAN_AUDIO_MIC_HEADER_SIZE, pcma,
           JOOAN_AUDIO_MIC_PAYLOAD_SIZE);
    (void)sendto(sender, datagram, sizeof(datagram),
                 MSG_DONTWAIT | MSG_NOSIGNAL,
                 (const struct sockaddr *)&microphone_capture.destination,
                 microphone_capture.destination_length);
}

static void microphone_retry_pause(void)
{
    struct timespec delay = { 1, 0 };

    while (!stopping && nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void *microphone_capture_main(void *unused)
{
    struct microphone_driver_parameter parameter = {
        JOOAN_AUDIO_SAMPLE_RATE, 16U, 1U
    };
    int16_t pcm[JOOAN_AUDIO_SAMPLES_PER_PACKET * 2U];
    uint8_t pcma[JOOAN_AUDIO_MIC_PAYLOAD_SIZE];
    size_t pcma_length = 0;
    uint32_t sequence = 0;
    int logged_failure = 0;

    (void)unused;
    while (!stopping) {
        int descriptor = open(microphone_capture.device, O_RDONLY);
        int sender = -1;
        int enabled = 0;

        if (descriptor >= 0) {
            int descriptor_flags = fcntl(descriptor, F_GETFD, 0);
            if (descriptor_flags >= 0)
                (void)fcntl(descriptor, F_SETFD,
                            descriptor_flags | FD_CLOEXEC);
        }
        if (descriptor >= 0 &&
            ioctl(descriptor, AMIC_AI_SET_PARAM, &parameter) == 0 &&
            ioctl(descriptor, AMIC_AI_ENABLE_STREAM, 1) == 0) {
            sender = socket(AF_UNIX, SOCK_DGRAM, 0);
            enabled = sender >= 0;
        }
        if (!enabled) {
            if (!logged_failure) {
                perror("microphone driver capture");
                logged_failure = 1;
            }
            if (sender >= 0)
                close(sender);
            if (descriptor >= 0)
                close(descriptor);
            microphone_retry_pause();
            continue;
        }
        if (logged_failure)
            fprintf(stderr, "microphone driver capture recovered\n");
        else
            fprintf(stderr, "microphone driver capture ready\n");
        logged_failure = 0;
        while (!stopping) {
            struct microphone_driver_stream stream;
            size_t samples;
            size_t index;

            memset(&stream, 0, sizeof(stream));
            stream.pcm = (uintptr_t)pcm;
            stream.byte_length = sizeof(pcm);
            stream.timeout_ms = 1000U;
            if (ioctl(descriptor, AMIC_AI_GET_STREAM, &stream) != 0 ||
                stream.byte_length == 0 ||
                stream.byte_length > sizeof(pcm) ||
                (stream.byte_length & 1U) != 0)
                break;
            samples = stream.byte_length / 2U;
            for (index = 0; index < samples; ++index) {
                pcma[pcma_length++] = jooan_alaw_encode_sample(pcm[index]);
                if (pcma_length == sizeof(pcma)) {
                    microphone_capture_emit(sender, &sequence, pcma);
                    pcma_length = 0;
                }
            }
        }
        (void)ioctl(descriptor, AMIC_AI_DISABLE_STREAM, 1);
        close(sender);
        close(descriptor);
        pcma_length = 0;
        if (!stopping)
            microphone_retry_pause();
    }
    return NULL;
}

static int microphone_capture_start(const char *device,
                                    const char *socket_path)
{
    size_t device_length = strlen(device);
    size_t path_length = strlen(socket_path);

    if (!strcmp(device, "off"))
        return 0;
    if (device_length == 0 ||
        device_length >= sizeof(microphone_capture.device) ||
        path_length == 0 ||
        path_length >= sizeof(microphone_capture.destination.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    memset(&microphone_capture, 0, sizeof(microphone_capture));
    memcpy(microphone_capture.device, device, device_length + 1U);
    microphone_capture.destination.sun_family = AF_UNIX;
    memcpy(microphone_capture.destination.sun_path, socket_path,
           path_length + 1U);
    microphone_capture.destination_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1U);
    if (pthread_create(&microphone_thread, NULL,
                       microphone_capture_main, NULL) != 0)
        return -1;
    microphone_thread_started = 1;
    return 0;
}

static int speaker_output_start(const char *device)
{
    struct microphone_driver_parameter parameter = {
        JOOAN_AUDIO_SAMPLE_RATE, 16U, 1U
    };
    int descriptor_flags;

    if (!strcmp(device, "off"))
        return 0;
    speaker_output_descriptor = open(device, O_WRONLY);
    if (speaker_output_descriptor < 0)
        return -1;
    descriptor_flags = fcntl(speaker_output_descriptor, F_GETFD, 0);
    if (descriptor_flags >= 0)
        (void)fcntl(speaker_output_descriptor, F_SETFD,
                    descriptor_flags | FD_CLOEXEC);
    if (ioctl(speaker_output_descriptor, AMIC_AO_SET_PARAM, &parameter) != 0 ||
        ioctl(speaker_output_descriptor, AMIC_AO_ENABLE_STREAM, 1) != 0) {
        int saved_errno = errno;
        close(speaker_output_descriptor);
        speaker_output_descriptor = -1;
        errno = saved_errno;
        return -1;
    }
    speaker_output_direct = 1;
    fprintf(stderr, "speaker driver output ready\n");
    return 0;
}

static int speaker_output_submit(const uint8_t *pcma, size_t length)
{
    int16_t pcm[JOOAN_AUDIO_SAMPLES_PER_PACKET];
    struct speaker_driver_stream stream;

    if (!speaker_output_direct || speaker_output_descriptor < 0 ||
        pcma == NULL || length != JOOAN_AUDIO_SAMPLES_PER_PACKET) {
        errno = EINVAL;
        return -1;
    }
    jooan_alaw_decode(pcma, pcm, JOOAN_AUDIO_SAMPLES_PER_PACKET);
    memset(&stream, 0, sizeof(stream));
    stream.pcm = (uintptr_t)pcm;
    stream.byte_length = sizeof(pcm);
    if (ioctl(speaker_output_descriptor, AMIC_AO_PLAY_STREAM, &stream) != 0 ||
        stream.byte_length != sizeof(pcm)) {
        if (stream.byte_length != sizeof(pcm))
            errno = EIO;
        return -1;
    }
    return 0;
}

static int speaker_amplifier_confirm_enabled(void)
{
    struct timespec delay = { 0, 4000000L };
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        char level = '\0';
        int descriptor = open(SPEAKER_VALUE_PATH, O_RDONLY);
        ssize_t amount = -1;

        if (descriptor >= 0) {
            amount = read(descriptor, &level, 1);
            close(descriptor);
        }
        if (amount == 1 && level == SPEAKER_ENABLED_LEVEL)
            return 0;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
        delay.tv_sec = 0;
        delay.tv_nsec = 4000000L;
    }
    errno = EIO;
    return -1;
}

static void speaker_output_stop(void)
{
    if (speaker_output_descriptor >= 0) {
        (void)ioctl(speaker_output_descriptor, AMIC_AO_DISABLE_STREAM, 1);
        close(speaker_output_descriptor);
    }
    speaker_output_descriptor = -1;
    speaker_output_direct = 0;
}

static uint32_t rotate_left(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32u - bits));
}

static void sha1_transform(struct sha1_context *context, const uint8_t *block)
{
    uint32_t words[80];
    uint32_t a, b, c, d, e, f, k, temporary;
    unsigned i;

    for (i = 0; i < 16; ++i) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   block[i * 4 + 3];
    }
    for (i = 16; i < 80; ++i)
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^
                               words[i - 14] ^ words[i - 16], 1);
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    for (i = 0; i < 80; ++i) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = UINT32_C(0x5a827999);
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = UINT32_C(0x6ed9eba1);
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = UINT32_C(0x8f1bbcdc);
        } else {
            f = b ^ c ^ d;
            k = UINT32_C(0xca62c1d6);
        }
        temporary = rotate_left(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temporary;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

static void sha1_init(struct sha1_context *context)
{
    memset(context, 0, sizeof(*context));
    context->state[0] = UINT32_C(0x67452301);
    context->state[1] = UINT32_C(0xefcdab89);
    context->state[2] = UINT32_C(0x98badcfe);
    context->state[3] = UINT32_C(0x10325476);
    context->state[4] = UINT32_C(0xc3d2e1f0);
}

static void sha1_update(struct sha1_context *context, const void *data,
                        size_t length)
{
    const uint8_t *bytes = data;

    context->bytes += length;
    while (length) {
        size_t amount = sizeof(context->block) - context->used;
        if (amount > length)
            amount = length;
        memcpy(context->block + context->used, bytes, amount);
        context->used += amount;
        bytes += amount;
        length -= amount;
        if (context->used == sizeof(context->block)) {
            sha1_transform(context, context->block);
            context->used = 0;
        }
    }
}

static void sha1_final(struct sha1_context *context, uint8_t digest[20])
{
    uint64_t bits = context->bytes * 8u;
    unsigned i;

    context->block[context->used++] = 0x80;
    if (context->used > 56) {
        memset(context->block + context->used, 0,
               sizeof(context->block) - context->used);
        sha1_transform(context, context->block);
        context->used = 0;
    }
    memset(context->block + context->used, 0, 56 - context->used);
    for (i = 0; i < 8; ++i)
        context->block[63 - i] = (uint8_t)(bits >> (i * 8));
    sha1_transform(context, context->block);
    for (i = 0; i < 5; ++i) {
        digest[i * 4] = (uint8_t)(context->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(context->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(context->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)context->state[i];
    }
}

static int base64_encode(const uint8_t *input, size_t length,
                         char *output, size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((length + 2) / 3) * 4 + 1;
    size_t in = 0, out = 0;

    if (!input || !output || capacity < needed)
        return -1;
    while (in < length) {
        uint32_t value = (uint32_t)input[in++] << 16;
        unsigned remaining = (unsigned)(length - (in - 1));
        if (in < length)
            value |= (uint32_t)input[in++] << 8;
        if (in < length)
            value |= input[in++];
        output[out++] = alphabet[(value >> 18) & 63u];
        output[out++] = alphabet[(value >> 12) & 63u];
        output[out++] = remaining > 1 ? alphabet[(value >> 6) & 63u] : '=';
        output[out++] = remaining > 2 ? alphabet[value & 63u] : '=';
    }
    output[out] = '\0';
    return 0;
}

static int websocket_accept(const char *key, char output[29])
{
    struct sha1_context context;
    uint8_t digest[20];

    sha1_init(&context);
    sha1_update(&context, key, strlen(key));
    sha1_update(&context, WS_GUID, strlen(WS_GUID));
    sha1_final(&context, digest);
    return base64_encode(digest, sizeof(digest), output, 29);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int descriptor_flags;

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;
    descriptor_flags = fcntl(fd, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0)
        return -1;
    return 0;
}

static int ensure_parent_directory(const char *path)
{
    char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char *slash;

    if (!path || strlen(path) >= sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(directory, path);
    slash = strrchr(directory, '/');
    if (!slash || slash == directory)
        return 0;
    *slash = '\0';
    if (mkdir(directory, 0750) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int bind_unix_socket(const char *path, int type)
{
    struct sockaddr_un address;
    size_t path_length = strlen(path);
    int fd;

    if (path_length == 0 || path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_parent_directory(path) != 0)
        return -1;
    fd = socket(AF_UNIX, type, 0);
    if (fd < 0)
        return -1;
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_length + 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address,
             offsetof(struct sockaddr_un, sun_path) + path_length + 1) != 0 ||
        chmod(path, 0660) != 0) {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

static uint64_t new_session_id(void)
{
    uint64_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        ssize_t amount = read(fd, &value, sizeof(value));
        close(fd);
        if (amount == (ssize_t)sizeof(value) && value != 0)
            return value;
    }
    value = ((uint64_t)(unsigned long)time(NULL) << 32) ^
            ((uint64_t)(unsigned long)getpid() << 16) ^
            (uint64_t)(uintptr_t)&value;
    return value ? value : 1;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return 0;
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000) +
           (uint64_t)timestamp.tv_nsec / UINT64_C(1000000);
}

static void client_init(struct client *client)
{
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->guard.fd = -1;
}

static void client_close(struct client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    jooan_audio_guard_close(&client->guard);
    client_init(client);
}

static int client_queue(struct client *client, const void *data, size_t length)
{
    if (client->output_offset &&
        client->output_offset + client->output_length + length >
            sizeof(client->output)) {
        memmove(client->output, client->output + client->output_offset,
                client->output_length);
        client->output_offset = 0;
    }
    if (length > sizeof(client->output) - client->output_offset -
                 client->output_length) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(client->output + client->output_offset + client->output_length,
           data, length);
    client->output_length += length;
    return 0;
}

static int client_flush(struct client *client)
{
    while (client->output_length) {
        ssize_t amount = send(client->fd,
                              client->output + client->output_offset,
                              client->output_length, 0);
        if (amount > 0) {
            client->output_offset += (size_t)amount;
            client->output_length -= (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    client->output_offset = 0;
    return 0;
}

static int queue_ws_frame(struct client *client, uint8_t opcode,
                          const void *payload, size_t length)
{
    uint8_t header[4];
    size_t header_length;

    if (length > 65535u)
        return -1;
    header[0] = (uint8_t)(0x80u | opcode);
    if (length <= 125u) {
        header[1] = (uint8_t)length;
        header_length = 2;
    } else {
        header[1] = 126;
        header[2] = (uint8_t)(length >> 8);
        header[3] = (uint8_t)length;
        header_length = 4;
    }
    if (client_queue(client, header, header_length) != 0 ||
        (length && client_queue(client, payload, length) != 0))
        return -1;
    return 0;
}

static int queue_audio_state(struct client *client, uint8_t state)
{
    uint8_t frame[JOOAN_AUDIO_WIRE_HEADER_SIZE + 1];
    size_t frame_length;
    uint32_t sequence = client->transmit_sequence == UINT32_MAX ? 1u :
                        client->transmit_sequence + 1u;

    if (jooan_audio_wire_build(frame, sizeof(frame), JOOAN_AUDIO_STATE, 0,
                               sequence, &state, 1, &frame_length) != 0 ||
        queue_ws_frame(client, 0x2u, frame, frame_length) != 0)
        return -1;
    client->transmit_sequence = sequence;
    return 0;
}

static int client_release_guard(struct client *client, uint8_t state)
{
    int result = 0;

    if (client->guard.acquired &&
        jooan_audio_guard_release(&client->guard, 0) != 0)
        result = -1;
    jooan_audio_guard_close(&client->guard);
    client->talk_deadline = 0;
    if (queue_audio_state(client, state) != 0)
        result = -1;
    return result;
}

static int valid_websocket_key(const char *key)
{
    size_t i;

    if (!key || strlen(key) != 24 || key[22] != '=' || key[23] != '=')
        return 0;
    for (i = 0; i < 22; ++i) {
        if (!(isalnum((unsigned char)key[i]) || key[i] == '+' || key[i] == '/'))
            return 0;
    }
    return 1;
}

static int token_list_contains(const char *value, const char *wanted)
{
    const char *cursor = value;
    size_t wanted_length = strlen(wanted);

    while (cursor && *cursor) {
        const char *end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
            ++cursor;
        end = strchr(cursor, ',');
        if (!end)
            end = cursor + strlen(cursor);
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t'))
            --end;
        if ((size_t)(end - cursor) == wanted_length &&
            !strncasecmp(cursor, wanted, wanted_length))
            return 1;
        cursor = *end ? end + 1 : NULL;
    }
    return 0;
}

static char *trim(char *value)
{
    char *end;

    while (*value == ' ' || *value == '\t')
        ++value;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return value;
}

static int client_handshake(struct client *client)
{
    char request[INPUT_CAPACITY + 1];
    char key[128] = "";
    char protocols[1024] = "";
    char upgrade[64] = "";
    char connection[128] = "";
    char version[32] = "";
    char method[8], path[128], http_version[16];
    char accept[29];
    char response[512];
    char *header_end;
    char *line;
    char *save = NULL;
    size_t header_length;
    int response_length;
    unsigned key_count = 0, protocol_count = 0, upgrade_count = 0;
    unsigned connection_count = 0, version_count = 0;

    if (client->input_length >= sizeof(request))
        return -1;
    memcpy(request, client->input, client->input_length);
    request[client->input_length] = '\0';
    header_end = strstr(request, "\r\n\r\n");
    if (!header_end)
        return 0;
    header_length = (size_t)(header_end - request) + 4;
    line = strtok_r(request, "\r\n", &save);
    if (!line || sscanf(line, "%7s %127s %15s", method, path, http_version) != 3 ||
        strcmp(method, "GET") || strcmp(http_version, "HTTP/1.1") ||
        (strcmp(path, "/ws/audio/mic") && strcmp(path, "/ws/audio/talk")))
        return -1;
    /* Both public routes are duplex. Keeping the aliases equivalent lets the
     * bundled listen UI add repeated push-to-talk without a second socket. */
    client->allow_talk = 1;
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        char *colon = strchr(line, ':');
        char *value;
        if (!colon)
            return -1;
        *colon = '\0';
        value = trim(colon + 1);
        if (!strcasecmp(line, "Upgrade")) {
            if (++upgrade_count != 1 || strlen(value) >= sizeof(upgrade))
                return -1;
            snprintf(upgrade, sizeof(upgrade), "%s", value);
        } else if (!strcasecmp(line, "Connection")) {
            if (++connection_count != 1 || strlen(value) >= sizeof(connection))
                return -1;
            snprintf(connection, sizeof(connection), "%s", value);
        } else if (!strcasecmp(line, "Sec-WebSocket-Version")) {
            if (++version_count != 1 || strlen(value) >= sizeof(version))
                return -1;
            snprintf(version, sizeof(version), "%s", value);
        } else if (!strcasecmp(line, "Sec-WebSocket-Key")) {
            if (++key_count != 1 || strlen(value) >= sizeof(key))
                return -1;
            snprintf(key, sizeof(key), "%s", value);
        } else if (!strcasecmp(line, "Sec-WebSocket-Protocol")) {
            if (++protocol_count != 1 || strlen(value) >= sizeof(protocols))
                return -1;
            snprintf(protocols, sizeof(protocols), "%s", value);
        }
    }
    if (upgrade_count != 1 || connection_count != 1 || version_count != 1 ||
        key_count != 1 || protocol_count != 1 ||
        strcasecmp(upgrade, "websocket") ||
        !token_list_contains(connection, "Upgrade") || strcmp(version, "13") ||
        !valid_websocket_key(key) ||
        jooan_audio_ws_protocol_validate(protocols, NULL) != 0 ||
        websocket_accept(key, accept) != 0)
        return -1;
    response_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Sec-WebSocket-Protocol: %s\r\n\r\n",
        accept, JOOAN_AUDIO_WS_PROTOCOL);
    if (response_length <= 0 || (size_t)response_length >= sizeof(response) ||
        client_queue(client, response, (size_t)response_length) != 0)
        return -1;
    memmove(client->input, client->input + header_length,
            client->input_length - header_length);
    client->input_length -= header_length;
    client->state = CLIENT_OPEN;
    return 1;
}

static int client_audio_frame(struct client *client,
                              const uint8_t *payload, size_t length)
{
    struct jooan_audio_wire_frame frame;
    uint32_t accepted = client->receive_sequence;

    if (!client->allow_talk ||
        jooan_audio_wire_parse(payload, length, &frame) != 0 ||
        jooan_audio_sequence_accept(&accepted, frame.sequence) != 0)
        return -1;
    if (frame.type == JOOAN_AUDIO_PTT_ACQUIRE) {
        if (client->guard.acquired)
            return -1;
        if (client->guard.fd >= 0)
            jooan_audio_guard_close(&client->guard);
        client->session_id = new_session_id();
        if (jooan_audio_guard_open(&client->guard, guard_socket_path,
                                   client->session_id) != 0) {
            perror("audio guard connect");
            return -1;
        }
        /* JAUD is monotonic for the WebSocket lifetime. Each press is a new
         * leased JAGD session and therefore begins its own sequence at one. */
        if (jooan_audio_guard_acquire(&client->guard, 1) != 0) {
            perror("audio guard acquire");
            return -1;
        }
        client->talk_deadline = monotonic_milliseconds() +
                                maximum_talk_ms;
        speaker_talk_logged = 0;
        if (queue_audio_state(client, JOOAN_AUDIO_STATE_ACQUIRED) != 0)
            return -1;
    } else if (frame.type == JOOAN_AUDIO_PTT_PCMA) {
        uint32_t guard_sequence;
        if (!client->guard.acquired)
            return 0;
        guard_sequence = client->guard.last_sequence == UINT32_MAX ? 1u :
                         client->guard.last_sequence + 1u;
        if (speaker_output_direct) {
            if (speaker_output_submit(frame.payload, frame.payload_length) != 0) {
                perror("audio speaker submit");
                return -1;
            }
            if (jooan_audio_guard_speaker_lease(&client->guard,
                                                guard_sequence) != 0) {
                perror("audio speaker lease");
                return -1;
            }
            if (!speaker_talk_logged) {
                if (speaker_amplifier_confirm_enabled() != 0) {
                    perror("audio speaker amplifier");
                    return -1;
                }
                fprintf(stderr, "authenticated speaker PCM started\n");
                speaker_talk_logged = 1;
            }
        } else if (jooan_audio_guard_pcma(&client->guard, guard_sequence,
                                          frame.payload,
                                          frame.payload_length) != 0) {
            return -1;
        }
    } else if (frame.type == JOOAN_AUDIO_PTT_RELEASE) {
        if (client_release_guard(client, JOOAN_AUDIO_STATE_RELEASED) != 0)
            return -1;
    } else {
        return -1;
    }
    client->receive_sequence = accepted;
    return 0;
}

static int client_websocket_frames(struct client *client)
{
    for (;;) {
        uint8_t first, second, opcode, mask[4];
        uint8_t payload[MAX_WS_PAYLOAD];
        size_t header_length, payload_length, total, i;

        if (client->input_length < 2)
            return 0;
        first = client->input[0];
        second = client->input[1];
        opcode = first & 0x0fu;
        if ((first & 0x80u) == 0 || (first & 0x70u) != 0 ||
            (second & 0x80u) == 0)
            return -1;
        payload_length = second & 0x7fu;
        header_length = 2;
        if (payload_length == 126) {
            if (client->input_length < 4)
                return 0;
            payload_length = ((size_t)client->input[2] << 8) | client->input[3];
            header_length = 4;
        } else if (payload_length == 127) {
            return -1;
        }
        if ((opcode & 0x08u) && payload_length > 125u)
            return -1;
        if (payload_length > sizeof(payload))
            return -1;
        total = header_length + 4 + payload_length;
        if (client->input_length < total)
            return 0;
        memcpy(mask, client->input + header_length, sizeof(mask));
        for (i = 0; i < payload_length; ++i)
            payload[i] = client->input[header_length + 4 + i] ^ mask[i & 3u];
        memmove(client->input, client->input + total,
                client->input_length - total);
        client->input_length -= total;

        if (opcode == 0x2) {
            if (client_audio_frame(client, payload, payload_length) != 0)
                return -1;
        } else if (opcode == 0x8) {
            return -1;
        } else if (opcode == 0x9) {
            if (queue_ws_frame(client, 0xau, payload, payload_length) != 0)
                return -1;
        } else if (opcode != 0xau) {
            return -1;
        }
    }
}

static int client_read(struct client *client)
{
    for (;;) {
        ssize_t amount;

        if (client->input_length == sizeof(client->input)) {
            errno = EMSGSIZE;
            return -1;
        }
        amount = recv(client->fd, client->input + client->input_length,
                      sizeof(client->input) - client->input_length, 0);
        if (amount > 0) {
            client->input_length += (size_t)amount;
            continue;
        }
        if (amount == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        return -1;
    }
    if (client->state == CLIENT_HTTP) {
        int result = client_handshake(client);
        if (result < 0)
            return -1;
        if (result == 0)
            return 0;
    }
    return client_websocket_frames(client);
}

static int broadcast_microphone(struct client *clients,
                                const uint8_t *pcma, size_t length)
{
    unsigned i;

    if (length == 0 || length > JOOAN_AUDIO_WIRE_MAX_PAYLOAD)
        return -1;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        uint8_t frame[JOOAN_AUDIO_WIRE_HEADER_SIZE +
                      JOOAN_AUDIO_WIRE_MAX_PAYLOAD];
        size_t frame_length;
        uint32_t sequence;

        if (clients[i].fd < 0 || clients[i].state != CLIENT_OPEN)
            continue;
        sequence = clients[i].transmit_sequence == UINT32_MAX ? 1u :
                   clients[i].transmit_sequence + 1u;
        if (jooan_audio_wire_build(frame, sizeof(frame),
                                   JOOAN_AUDIO_MIC_PCMA, 0, sequence,
                                   pcma, (uint16_t)length,
                                   &frame_length) != 0 ||
            queue_ws_frame(&clients[i], 0x2u, frame, frame_length) != 0) {
            client_close(&clients[i]);
            continue;
        }
        clients[i].transmit_sequence = sequence;
    }
    return 0;
}

static void expire_talk_leases(struct client *clients)
{
    uint64_t now = monotonic_milliseconds();
    unsigned i;

    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd < 0 || !clients[i].guard.acquired ||
            clients[i].talk_deadline == 0 || now < clients[i].talk_deadline)
            continue;
        if (client_release_guard(&clients[i],
                                 JOOAN_AUDIO_STATE_LEASE_EXPIRED) != 0)
            client_close(&clients[i]);
    }
}

static int add_client(struct client *clients, int listener)
{
    int fd;
    unsigned i;

    fd = accept(listener, NULL, NULL);
    if (fd < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            clients[i].session_id = new_session_id();
            return 0;
        }
    }
    close(fd);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--ws-socket PATH] [--mic-socket PATH] "
            "[--mic-driver PATH|off] [--speaker-driver PATH|off] "
            "[--guard-socket PATH] "
            "[--max-talk-ms 1..60000]\n", program);
}

int main(int argc, char **argv)
{
    const char *ws_path = getenv("JOAN_AUDIO_WS_SOCKET");
    const char *mic_path = getenv("JOAN_AUDIO_MIC_SOCKET");
    const char *mic_driver = getenv("JOAN_AUDIO_MIC_DRIVER");
    const char *speaker_driver = getenv("JOAN_AUDIO_SPEAKER_DRIVER");
    const char *guard_path = getenv("JOAN_GUARD_TALKBACK_SOCKET");
    struct client clients[MAX_CLIENTS];
    struct sigaction action;
    int listener = -1, microphone = -1;
    uint32_t microphone_sequence = 0;
    unsigned i;
    int argument;

    if (!ws_path || !*ws_path)
        ws_path = DEFAULT_WS_SOCKET;
    if (!mic_path || !*mic_path)
        mic_path = DEFAULT_MIC_SOCKET;
    if (!mic_driver || !*mic_driver)
        mic_driver = DEFAULT_MIC_DRIVER;
    if (!speaker_driver || !*speaker_driver)
        speaker_driver = DEFAULT_SPEAKER_DRIVER;
    if (!guard_path || !*guard_path)
        guard_path = DEFAULT_GUARD_SOCKET;
    for (argument = 1; argument < argc; argument += 2) {
        if (argument + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (!strcmp(argv[argument], "--ws-socket"))
            ws_path = argv[argument + 1];
        else if (!strcmp(argv[argument], "--mic-socket"))
            mic_path = argv[argument + 1];
        else if (!strcmp(argv[argument], "--mic-driver"))
            mic_driver = argv[argument + 1];
        else if (!strcmp(argv[argument], "--speaker-driver"))
            speaker_driver = argv[argument + 1];
        else if (!strcmp(argv[argument], "--guard-socket"))
            guard_path = argv[argument + 1];
        else if (!strcmp(argv[argument], "--max-talk-ms")) {
            char *end = NULL;
            unsigned long value = strtoul(argv[argument + 1], &end, 10);
            if (!end || *end || value == 0 || value > JOOAN_AUDIO_MAX_TALK_MS) {
                usage(argv[0]);
                return 2;
            }
            maximum_talk_ms = (uint32_t)value;
        }
        else {
            usage(argv[0]);
            return 2;
        }
    }
    guard_socket_path = guard_path;
    for (i = 0; i < MAX_CLIENTS; ++i)
        client_init(&clients[i]);
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    umask(0007);

    listener = bind_unix_socket(ws_path, SOCK_STREAM);
    if (listener < 0 || listen(listener, 8) != 0) {
        perror("audio websocket socket");
        goto failed;
    }
    microphone = bind_unix_socket(mic_path, SOCK_DGRAM);
    if (microphone < 0) {
        perror("audio microphone socket");
        goto failed;
    }
    if (speaker_output_start(speaker_driver) != 0) {
        perror("audio speaker output");
        goto failed;
    }
    if (microphone_capture_start(mic_driver, mic_path) != 0) {
        perror("audio microphone capture");
        goto failed;
    }

    while (!stopping) {
        struct pollfd descriptors[2 + MAX_CLIENTS];
        int ready;

        expire_talk_leases(clients);
        descriptors[0].fd = listener;
        descriptors[0].events = POLLIN;
        descriptors[1].fd = microphone;
        descriptors[1].events = POLLIN;
        for (i = 0; i < MAX_CLIENTS; ++i) {
            descriptors[2 + i].fd = clients[i].fd;
            descriptors[2 + i].events = POLLIN;
            if (clients[i].output_length)
                descriptors[2 + i].events |= POLLOUT;
            descriptors[2 + i].revents = 0;
        }
        ready = poll(descriptors, 2 + MAX_CLIENTS, 250);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("audio poll");
            break;
        }
        if (descriptors[0].revents & POLLIN)
            (void)add_client(clients, listener);
        if (descriptors[1].revents & POLLIN) {
            uint8_t datagram[JOOAN_AUDIO_MIC_DATAGRAM_SIZE];
            ssize_t amount;
            while ((amount = recv(microphone, datagram,
                                  sizeof(datagram), 0)) > 0) {
                uint32_t sequence;
                if (amount != (ssize_t)sizeof(datagram) ||
                    memcmp(datagram, JOOAN_AUDIO_MIC_MAGIC, 4) != 0 ||
                    datagram[4] != JOOAN_AUDIO_MIC_VERSION ||
                    datagram[5] != JOOAN_AUDIO_MIC_HEADER_SIZE ||
                    read_be16(datagram + 6) != JOOAN_AUDIO_MIC_PAYLOAD_SIZE)
                    continue;
                sequence = read_be32(datagram + 8);
                /* Datagram gaps are expected when the guard has no listener
                 * or a nonblocking send drops a packet. Reject only duplicates
                 * and backwards traffic; sequence 1 marks restart/wrap. */
                if (sequence == 0 ||
                    (microphone_sequence != 0 && sequence != 1 &&
                     (int32_t)(sequence - microphone_sequence) <= 0))
                    continue;
                microphone_sequence = sequence;
                (void)broadcast_microphone(
                    clients, datagram + JOOAN_AUDIO_MIC_HEADER_SIZE,
                    JOOAN_AUDIO_MIC_PAYLOAD_SIZE);
            }
        }
        for (i = 0; i < MAX_CLIENTS; ++i) {
            short events = descriptors[2 + i].revents;
            if (clients[i].fd < 0 || !events)
                continue;
            if ((events & (POLLERR | POLLHUP | POLLNVAL)) ||
                ((events & POLLIN) && client_read(&clients[i]) != 0) ||
                (clients[i].fd >= 0 && (events & POLLOUT) &&
                 client_flush(&clients[i]) != 0))
                client_close(&clients[i]);
        }
    }

    stopping = 1;
    for (i = 0; i < MAX_CLIENTS; ++i)
        client_close(&clients[i]);
    if (microphone_thread_started)
        (void)pthread_join(microphone_thread, NULL);
    speaker_output_stop();
    close(microphone);
    close(listener);
    unlink(mic_path);
    unlink(ws_path);
    return 0;

failed:
    stopping = 1;
    for (i = 0; i < MAX_CLIENTS; ++i)
        client_close(&clients[i]);
    if (microphone_thread_started)
        (void)pthread_join(microphone_thread, NULL);
    speaker_output_stop();
    if (microphone >= 0)
        close(microphone);
    if (listener >= 0)
        close(listener);
    unlink(mic_path);
    unlink(ws_path);
    return 1;
}
