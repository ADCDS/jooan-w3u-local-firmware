#define _GNU_SOURCE

#include "guard.h"
#include "guard_sha256.h"
#include "../audio/audio_guard_wire.h"
#include "../audio/audio_mic_wire.h"
#include "../audio/audio_wire.h"
#include "../audio/g711_alaw.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <unistd.h>

struct accept_probe {
    int listener;
    int accepted;
};

static void *accept_probe_thread(void *opaque)
{
    struct accept_probe *probe = opaque;

    probe->accepted = accept(probe->listener, NULL, NULL);
    return NULL;
}

static int policy_probe(void)
{
    struct addrinfo *result = NULL;
    struct sockaddr_in blocked;
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);
    int listener = -1;
    int client = -1;
    int datagram = -1;
    int mqtt_listener = -1;
    int mqtt_client = -1;
    int mqtt_accepted = -1;
    int connectivity_client = -1;
    int api_listener = -1;
    int api_client = -1;
    int api_accepted = -1;
    int rtsp_listener = -1;
    int rtsp_first = -1;
    int rtsp_second = -1;
    int local_pair[2] = { -1, -1 };
    struct iovec vector;
    struct msghdr message;
    char mqtt_port[16];
    char rtsp_port[16];
    char rtsp_digest[65];
    const char *rtsp_password = getenv("JOOAN_GUARD_TEST_RTSP_PASSWORD_PATH");
    const char *rtsp_sync = getenv("JOOAN_GUARD_TEST_RTSP_SYNC_PATH");
    struct accept_probe accept_probe;
    pthread_t accept_thread;
    int rc = 1;

    if (getaddrinfo(JOOAN_GUARD_API_HOST, "443", NULL, &result) != 0 ||
        result == NULL || !jooan_guard_sockaddr_is_loopback(
            result->ai_addr, result->ai_addrlen))
        goto done;
    freeaddrinfo(result);
    result = NULL;
    if (getaddrinfo("blocked.invalid", "443", NULL, &result) != EAI_NONAME)
        goto done;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || client < 0)
        goto done;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&local, &local_length) != 0 ||
        local.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        connect(client, (struct sockaddr *)&local, sizeof(local)) != 0)
        goto done;

    mqtt_listener = socket(AF_INET, SOCK_STREAM, 0);
    mqtt_client = socket(AF_INET, SOCK_STREAM, 0);
    if (mqtt_listener < 0 || mqtt_client < 0)
        goto done;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(0x7f000002UL);
    local.sin_port = 0;
    local_length = sizeof(local);
    if (syscall(SYS_bind,mqtt_listener,(struct sockaddr *)&local,sizeof(local)) != 0 ||
        listen(mqtt_listener, 1) != 0 ||
        getsockname(mqtt_listener, (struct sockaddr *)&local,
                    &local_length) != 0 ||
        snprintf(mqtt_port, sizeof(mqtt_port), "%u",
                 (unsigned)ntohs(local.sin_port)) <= 0 ||
        setenv("JOOAN_GUARD_TEST_MQTT_PORT", mqtt_port, 1) != 0)
        goto done;
    local.sin_addr.s_addr = htonl(0x7f000002UL);
    local.sin_port = htons(443);
    if (connect(mqtt_client, (struct sockaddr *)&local, sizeof(local)) != 0)
        goto done;
    mqtt_accepted = accept(mqtt_listener, NULL, NULL);
    if (mqtt_accepted < 0)
        goto done;

    api_listener = socket(AF_INET, SOCK_STREAM, 0);
    api_client = socket(AF_INET, SOCK_STREAM, 0);
    if (api_listener < 0 || api_client < 0)
        goto done;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    local_length = sizeof(local);
    if (bind(api_listener, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        listen(api_listener, 1) != 0 ||
        getsockname(api_listener, (struct sockaddr *)&local, &local_length) != 0)
        goto done;
    local.sin_addr.s_addr = htonl(0x7f000003UL);
    if (connect(api_client, (struct sockaddr *)&local, sizeof(local)) != 0)
        goto done;
    api_accepted = accept(api_listener, NULL, NULL);
    if (api_accepted < 0)
        goto done;

    if (rtsp_password == NULL || rtsp_sync == NULL)
        goto done;
    rtsp_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (rtsp_listener < 0)
        goto done;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    local_length = sizeof(local);
    if (bind(rtsp_listener, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        listen(rtsp_listener, 2) != 0 ||
        getsockname(rtsp_listener, (struct sockaddr *)&local,
                    &local_length) != 0 ||
        snprintf(rtsp_port, sizeof(rtsp_port), "%u",
                 (unsigned)ntohs(local.sin_port)) <= 0 ||
        setenv("JOOAN_GUARD_TEST_RTSP_PORT", rtsp_port, 1) != 0 ||
        setenv("JOOAN_GUARD_TEST_RTSP_UPSTREAM_PORT", rtsp_port, 1) != 0)
        goto done;
    {
        static const char password[] = "temporary-password\n";
        int password_fd = open(rtsp_password, O_WRONLY | O_CREAT | O_TRUNC,
                               0600);
        if (password_fd < 0 ||
            write(password_fd, password, sizeof(password) - 1) !=
                (ssize_t)(sizeof(password) - 1) || close(password_fd) != 0 ||
            jooan_guard_sha256_file_hex(rtsp_password, rtsp_digest) != 0)
            goto done;
    }
    (void)unlink(rtsp_sync);
    memset(&accept_probe, 0, sizeof(accept_probe));
    accept_probe.listener = rtsp_listener;
    accept_probe.accepted = -1;
    if (pthread_create(&accept_thread, NULL, accept_probe_thread,
                       &accept_probe) != 0)
        goto done;
    rtsp_first = socket(AF_INET, SOCK_STREAM, 0);
    if (rtsp_first < 0 ||
        connect(rtsp_first, (struct sockaddr *)&local, sizeof(local)) != 0)
        goto rtsp_join_fail;
    {
        struct timeval timeout = { 1, 0 };
        unsigned char byte;
        (void)setsockopt(rtsp_first, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
        if (recv(rtsp_first, &byte, 1, 0) != 0)
            goto rtsp_join_fail;
    }
    {
        int marker_fd = open(rtsp_sync, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (marker_fd < 0 || write(marker_fd, rtsp_digest, 64) != 64 ||
            close(marker_fd) != 0)
            goto rtsp_join_fail;
    }
    rtsp_second = socket(AF_INET, SOCK_STREAM, 0);
    if (rtsp_second < 0 ||
        connect(rtsp_second, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        pthread_join(accept_thread, NULL) != 0 || accept_probe.accepted < 0)
        goto done;
    close(accept_probe.accepted);
    accept_probe.accepted = -1;
    goto rtsp_gate_done;
rtsp_join_fail:
    (void)shutdown(rtsp_listener, SHUT_RDWR);
    (void)pthread_join(accept_thread, NULL);
    goto done;
rtsp_gate_done:

    memset(&blocked, 0, sizeof(blocked));
    blocked.sin_family = AF_INET;
    blocked.sin_port = htons(443);
    inet_pton(AF_INET, "192.0.2.1", &blocked.sin_addr);
    connectivity_client=socket(AF_INET,SOCK_DGRAM,0);
    local_length=sizeof(local);memset(&local,0,sizeof(local));
    if(connectivity_client<0||
       connect(connectivity_client,(struct sockaddr*)&blocked,sizeof(blocked))!=0||
       getpeername(connectivity_client,(struct sockaddr*)&local,&local_length)!=0||
       local.sin_family!=AF_INET||ntohl(local.sin_addr.s_addr)!=0x7f000004UL||
       ntohs(local.sin_port)!=JOOAN_GUARD_CONNECTIVITY_PORT)
        goto done;
    errno = 0;
    if (connect(client, (struct sockaddr *)&blocked, sizeof(blocked)) != -1)
        goto done;

    datagram = socket(AF_INET, SOCK_DGRAM, 0);
    errno = 0;
    if (sendto(datagram, "x", 1, 0, (struct sockaddr *)&blocked,
               sizeof(blocked)) != -1 || errno != EACCES)
        goto done;

    memset(&message, 0, sizeof(message));
    vector.iov_base = (void *)"x";
    vector.iov_len = 1;
    message.msg_name = &blocked;
    message.msg_namelen = sizeof(blocked);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    errno = 0;
    if (sendmsg(datagram, &message, 0) != -1 || errno != EACCES)
        goto done;

#ifndef __UCLIBC__
    {
        struct mmsghdr batch;
        memset(&batch, 0, sizeof(batch));
        batch.msg_hdr = message;
        errno = 0;
        if (sendmmsg(datagram, &batch, 1, 0) != -1 || errno != EACCES)
            goto done;
    }
#endif

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, local_pair) != 0 ||
        send(local_pair[0], "x", 1, 0) != 1)
        goto done;
    rc = 0;

done:
    if (result != NULL)
        freeaddrinfo(result);
    if (datagram >= 0)
        close(datagram);
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
    if (local_pair[0] >= 0)
        close(local_pair[0]);
    if (local_pair[1] >= 0)
        close(local_pair[1]);
    if (mqtt_accepted >= 0)
        close(mqtt_accepted);
    if (mqtt_client >= 0)
        close(mqtt_client);
    if (mqtt_listener >= 0)
        close(mqtt_listener);
    if (connectivity_client >= 0)
        close(connectivity_client);
    if (api_accepted >= 0)
        close(api_accepted);
    if (api_client >= 0)
        close(api_client);
    if (api_listener >= 0)
        close(api_listener);
    if (rtsp_first >= 0)
        close(rtsp_first);
    if (rtsp_second >= 0)
        close(rtsp_second);
    if (rtsp_listener >= 0)
        close(rtsp_listener);
    return rc;
}

static int vfork_close_probe(int descriptor)
{
    pid_t child = vfork();
    int status;

    if (child < 0)
        return -1;
    if (child == 0) {
        close(descriptor);
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

struct dsp_writev_race {
    int descriptor;
    ssize_t result;
};

static int wait_file_contents(const char *path, const char *expected,
                              size_t expected_length)
{
    char value[16];
    ssize_t amount;
    int descriptor;
    int attempt;

    if (expected_length > sizeof(value))
        return -1;
    for (attempt = 0; attempt < 100; ++attempt) {
        descriptor = open(path, O_RDONLY);
        if (descriptor >= 0) {
            amount = read(descriptor, value, sizeof(value));
            (void)close(descriptor);
            if (amount == (ssize_t)expected_length &&
                memcmp(value, expected, expected_length) == 0)
                return 0;
        }
        usleep(10000);
    }
    return -1;
}

static int write_file_contents(const char *path, const char *value,
                               size_t length)
{
    int descriptor = open(path, O_WRONLY);
    ssize_t amount;

    if (descriptor < 0)
        return -1;
    amount = write(descriptor, value, length);
    if (close(descriptor) != 0)
        return -1;
    return amount == (ssize_t)length ? 0 : -1;
}

static void *dsp_writev_thread(void *opaque)
{
    static const unsigned char first = 0xa5;
    static const unsigned char second = 0x5a;
    struct dsp_writev_race *race = opaque;
    struct iovec vectors[2];

    vectors[0].iov_base = (void *)&first;
    vectors[0].iov_len = 1;
    vectors[1].iov_base = (void *)&second;
    vectors[1].iov_len = 1;
    race->result = writev(race->descriptor, vectors, 2);
    return NULL;
}

static int talkback_probe(void)
{
    static const unsigned char encoded_prefix[] = { 0xd5, 0x55, 0x80, 0x00 };
    static const unsigned char expected[] = {
        0x08, 0x00, 0xf8, 0xff, 0x80, 0x15, 0x80, 0xea
    };
    unsigned char datagram[JOOAN_AUDIO_GUARD_HEADER_SIZE +
                           JOOAN_AUDIO_SAMPLES_PER_PACKET];
    unsigned char encoded[JOOAN_AUDIO_SAMPLES_PER_PACKET];
    size_t datagram_length;
    const char *dsp_path = getenv("JOOAN_GUARD_TEST_DSP_PATH");
    const char *socket_path = getenv("JOOAN_GUARD_TEST_TALKBACK_PATH");
    const char *speaker_direction =
        getenv("JOOAN_GUARD_TEST_SPEAKER_DIRECTION_PATH");
    const char *speaker_value = getenv("JOOAN_GUARD_TEST_SPEAKER_VALUE_PATH");
    struct sockaddr_un address;
    struct stat status;
    unsigned char actual[sizeof(expected)];
    int dsp = -1;
    int sender = -1;
    int reader = -1;
    int attempt;
    int rc = 1;
    int alias;
    struct dsp_writev_race vector_race;
    pthread_t vector_thread;

    if (dsp_path == NULL || socket_path == NULL || speaker_direction == NULL ||
        speaker_value == NULL)
        return 1;
    if (wait_file_contents(speaker_direction, "out", 3) != 0 ||
        wait_file_contents(speaker_value, "1", 1) != 0 ||
        write_file_contents(speaker_direction, "in", 2) != 0 ||
        wait_file_contents(speaker_direction, "out", 3) != 0 ||
        write_file_contents(speaker_value, "0", 1) != 0 ||
        wait_file_contents(speaker_value, "1", 1) != 0)
        return 1;
    dsp = open(dsp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (dsp < 0 || sender < 0)
        goto done;
    alias = dup(dsp);
    if (alias < 0 || close(dsp) != 0)
        goto done;
    dsp = alias;
    alias = fcntl(dsp, F_DUPFD, 100);
    if (alias < 0 || close(dsp) != 0)
        goto done;
    dsp = alias;
    alias = dup2(dsp, 500);
    if (alias != 500 || close(dsp) != 0)
        goto done;
    dsp = alias;
    if (fcntl(dsp, F_GETFD) < 0)
        goto done;
    if (vfork_close_probe(dsp) != 0)
        goto done;
    errno = 0;
    if (ioctl(dsp, JOOAN_GUARD_AO_PLAY_IOCTL, NULL) != 0)
        goto done;
    errno = 0;
    if (ioctl(dsp, 0x40045060UL) != -1 || errno != ENOTTY)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path))
        goto done;
    strcpy(address.sun_path, socket_path);
    memset(encoded, 0xd5, sizeof(encoded));
    memcpy(encoded, encoded_prefix, sizeof(encoded_prefix));

    /* Keep an OEM vector write in the DSP critical section while talkback
     * arrives. The OEM write reports success but only decoded PTT PCM reaches
     * the output file. */
    memset(&vector_race, 0, sizeof(vector_race));
    vector_race.descriptor = dsp;
    if (setenv("JOOAN_GUARD_TEST_DSP_WRITEV_PAUSE", "1", 1) != 0 ||
        pthread_create(&vector_thread, NULL, dsp_writev_thread,
                       &vector_race) != 0)
        goto done;
    usleep(20000);
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_ACQUIRE,
            UINT64_C(0x2122232425262728), 1, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length ||
        jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_PCMA,
            UINT64_C(0x2122232425262728), 2, encoded, sizeof(encoded),
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length) {
        (void)pthread_join(vector_thread, NULL);
        goto done;
    }
    if (wait_file_contents(speaker_value, "0", 1) != 0 ||
        write_file_contents(speaker_value, "1", 1) != 0 ||
        wait_file_contents(speaker_value, "0", 1) != 0) {
        (void)pthread_join(vector_thread, NULL);
        goto done;
    }
    if (pthread_join(vector_thread, NULL) != 0 || vector_race.result != 2 ||
        unsetenv("JOOAN_GUARD_TEST_DSP_WRITEV_PAUSE") != 0)
        goto done;
    for (attempt = 0; attempt < 100; ++attempt) {
        if (stat(dsp_path, &status) == 0 &&
            status.st_size == (off_t)(sizeof(encoded) * 2U))
            break;
        usleep(10000);
    }
    if (attempt == 100)
        goto done;
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_RELEASE,
            UINT64_C(0x2122232425262728), 3, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length ||
        wait_file_contents(speaker_value, "1", 1) != 0 ||
        ftruncate(dsp, 0) != 0 ||
        lseek(dsp, 0, SEEK_SET) != 0)
        goto done;

    /* A stale owner must expire before it can deliver audio. */
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_ACQUIRE,
            UINT64_C(0x1112131415161718), 1, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    usleep(650000);
    if (wait_file_contents(speaker_value, "1", 1) != 0)
        goto done;
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_PCMA,
            UINT64_C(0x1112131415161718), 2, encoded, sizeof(encoded),
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    if (wait_file_contents(speaker_value, "1", 1) != 0)
        goto done;
    usleep(100000);
    if (stat(dsp_path, &status) != 0 || status.st_size != 0)
        goto done;

    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_ACQUIRE,
            UINT64_C(0x0102030405060708), 1, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    if (wait_file_contents(speaker_value, "1", 1) != 0)
        goto done;
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_PCMA,
            UINT64_C(0x0102030405060708), 2, encoded, sizeof(encoded),
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    if (wait_file_contents(speaker_value, "0", 1) != 0)
        goto done;
    for (attempt = 0; attempt < 100; ++attempt) {
        if (stat(dsp_path, &status) == 0 &&
            status.st_size == (off_t)(sizeof(encoded) * 2U))
            break;
        usleep(10000);
    }
    if (attempt == 100)
        goto done;
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_RELEASE,
            UINT64_C(0x0102030405060708), 3, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    if (wait_file_contents(speaker_value, "1", 1) != 0)
        goto done;
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_PCMA,
            UINT64_C(0x0102030405060708), 4, encoded, sizeof(encoded),
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length)
        goto done;
    usleep(100000);
    if (stat(dsp_path, &status) != 0 ||
        status.st_size != (off_t)(sizeof(encoded) * 2U))
        goto done;

    /* Direct-driver talkback uses the guard only as an amplifier deadman.
     * A valid lease enables the active-low amplifier without injecting DSP
     * data, and RELEASE restores mute. */
    if (jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_ACQUIRE,
            UINT64_C(0x3132333435363738), 1, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length ||
        jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_GUARD_SPEAKER_LEASE,
            UINT64_C(0x3132333435363738), 2, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length ||
        wait_file_contents(speaker_value, "0", 1) != 0 ||
        stat(dsp_path, &status) != 0 ||
        status.st_size != (off_t)(sizeof(encoded) * 2U) ||
        jooan_audio_guard_datagram_build(
            datagram, sizeof(datagram), JOOAN_AUDIO_PTT_RELEASE,
            UINT64_C(0x3132333435363738), 3, NULL, 0,
            &datagram_length) != 0 ||
        sendto(sender, datagram, datagram_length, 0,
               (struct sockaddr *)&address, sizeof(address)) !=
        (ssize_t)datagram_length ||
        wait_file_contents(speaker_value, "1", 1) != 0)
        goto done;
    close(dsp);
    dsp = -1;
    reader = open(dsp_path, O_RDONLY);
    if (reader < 0 || read(reader, actual, sizeof(actual)) !=
                      (ssize_t)sizeof(actual) ||
        memcmp(actual, expected, sizeof(expected)) != 0)
        goto done;

    rc = 0;

done:
    if (reader >= 0)
        close(reader);
    if (sender >= 0)
        close(sender);
    if (dsp >= 0)
        close(dsp);
    return rc;
}

static uint16_t load_be16(const unsigned char *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t load_be32(const unsigned char *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static int receive_mic_frame(int socket_descriptor, uint32_t sequence,
                             const int16_t *expected)
{
    unsigned char datagram[JOOAN_AUDIO_MIC_DATAGRAM_SIZE];
    struct pollfd wait_descriptor;
    ssize_t amount;
    size_t index;

    wait_descriptor.fd = socket_descriptor;
    wait_descriptor.events = POLLIN;
    wait_descriptor.revents = 0;
    if (poll(&wait_descriptor, 1, 1000) != 1 ||
        (wait_descriptor.revents & POLLIN) == 0)
        return -1;
    amount = recv(socket_descriptor, datagram, sizeof(datagram), 0);
    if (amount != (ssize_t)sizeof(datagram) ||
        memcmp(datagram, JOOAN_AUDIO_MIC_MAGIC, 4) != 0 ||
        datagram[4] != JOOAN_AUDIO_MIC_VERSION ||
        datagram[5] != JOOAN_AUDIO_MIC_HEADER_SIZE ||
        load_be16(datagram + 6) != JOOAN_AUDIO_MIC_PAYLOAD_SIZE ||
        load_be32(datagram + 8) != sequence)
        return -1;
    for (index = 0; index < JOOAN_AUDIO_MIC_PAYLOAD_SIZE; ++index) {
        if (datagram[JOOAN_AUDIO_MIC_HEADER_SIZE + index] !=
            jooan_alaw_encode_sample(expected[index]))
            return -1;
    }
    return 0;
}

struct microphone_race {
    int descriptor;
    unsigned char buffer[JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U];
    ssize_t result;
};

static void *microphone_race_read(void *opaque)
{
    struct microphone_race *race = opaque;

    race->result = read(race->descriptor, race->buffer, sizeof(race->buffer));
    return NULL;
}

static int microphone_probe(void)
{
    const char *dsp_path = getenv("JOOAN_GUARD_TEST_DSP_PATH");
    const char *socket_path = getenv("JOOAN_GUARD_TEST_MIC_SOCKET_PATH");
    unsigned char pcm_bytes[JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 4U];
    unsigned char first[JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U - 1U];
    unsigned char second[1];
    unsigned char vector_first[200];
    unsigned char vector_second[JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U -
                                sizeof(vector_first)];
    int16_t samples[JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U];
    struct sockaddr_un address;
    struct iovec vectors[2];
    struct jooan_guard_mic_stream_request stream_request;
    struct microphone_race race;
    struct pollfd no_frame;
    pthread_t race_thread;
    size_t index;
    size_t path_length;
    ssize_t amount;
    int listener = -1;
    int writer = -1;
    int reader = -1;
    int rc = 1;
    int alias;

    if (dsp_path == NULL || socket_path == NULL)
        return 1;
    path_length = strlen(socket_path);
    if (path_length == 0 || path_length >= sizeof(address.sun_path))
        return 1;
    for (index = 0; index < sizeof(samples) / sizeof(samples[0]); ++index) {
        samples[index] = (int16_t)((int)index * 97 - 30000);
        pcm_bytes[index * 2] = (unsigned char)((uint16_t)samples[index] & 0xffU);
        pcm_bytes[index * 2 + 1] =
            (unsigned char)((uint16_t)samples[index] >> 8);
    }

    listener = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (listener < 0)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1);
    (void)unlink(socket_path);
    if (bind(listener, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         path_length + 1)) != 0)
        goto done;

    writer = open(dsp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (writer < 0 || pwrite(writer, pcm_bytes, sizeof(pcm_bytes), 0) !=
                      (ssize_t)sizeof(pcm_bytes))
        goto done;
    close(writer);
    writer = -1;

    reader = open(dsp_path, O_RDONLY);
    if (reader < 0 || vfork_close_probe(reader) != 0)
        goto done;
    alias = dup2(reader, 501);
    if (alias != 501 || close(reader) != 0)
        goto done;
    reader = alias;
    if (read(reader, first, sizeof(first)) != (ssize_t)sizeof(first) ||
        read(reader, second, sizeof(second)) != (ssize_t)sizeof(second) ||
        receive_mic_frame(listener, 1, samples) != 0)
        goto done;

    vectors[0].iov_base = vector_first;
    vectors[0].iov_len = sizeof(vector_first);
    vectors[1].iov_base = vector_second;
    vectors[1].iov_len = sizeof(vector_second);
    amount = readv(reader, vectors, 2);
    if (amount != (ssize_t)(sizeof(vector_first) + sizeof(vector_second)) ||
        receive_mic_frame(listener, 2,
                          samples + JOOAN_AUDIO_MIC_PAYLOAD_SIZE) != 0)
        goto done;

    memset(&stream_request, 0, sizeof(stream_request));
    stream_request.pcm = (uintptr_t)pcm_bytes;
    stream_request.byte_length = JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U;
    if (ioctl(reader, JOOAN_GUARD_MIC_STREAM_IOCTL, &stream_request) != 0 ||
        receive_mic_frame(listener, 3, samples) != 0)
        goto done;

    /* Force close/reopen after the OEM read returns but before the tap phase.
     * Generation validation must discard the stale buffer even when the new
     * open reuses the same integer descriptor. */
    if (lseek(reader, 0, SEEK_SET) != 0 ||
        setenv("JOOAN_GUARD_TEST_MIC_PAUSE", "1", 1) != 0)
        goto done;
    memset(&race, 0, sizeof(race));
    race.descriptor = reader;
    if (pthread_create(&race_thread, NULL, microphone_race_read, &race) != 0)
        goto done;
    usleep(20000);
    if (close(reader) != 0) {
        (void)pthread_join(race_thread, NULL);
        reader = -1;
        goto done;
    }
    reader = open(dsp_path, O_RDONLY);
    if (pthread_join(race_thread, NULL) != 0 ||
        race.result != (ssize_t)sizeof(race.buffer) || reader < 0 ||
        unsetenv("JOOAN_GUARD_TEST_MIC_PAUSE") != 0)
        goto done;
    no_frame.fd = listener;
    no_frame.events = POLLIN;
    no_frame.revents = 0;
    if (poll(&no_frame, 1, 150) != 0)
        goto done;

    /* Saturate the Unix datagram queue. Every OEM read must still complete;
     * the guard drops excess frames through MSG_DONTWAIT/EAGAIN. */
    for (index = 0; index < 128; ++index) {
        if (lseek(reader, 0, SEEK_SET) != 0 ||
            read(reader, pcm_bytes, JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U) !=
                (ssize_t)(JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U))
            goto done;
    }

    /* With the listener gone, capture remains successful and the datagram is
     * simply dropped by the nonblocking sender. */
    close(listener);
    listener = -1;
    errno = EDOM;
    if (unlink(socket_path) != 0 || lseek(reader, 0, SEEK_SET) != 0 ||
        read(reader, pcm_bytes, JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U) !=
            (ssize_t)(JOOAN_AUDIO_MIC_PAYLOAD_SIZE * 2U) || errno != EDOM)
        goto done;
    if (ioctl(reader, JOOAN_GUARD_MIC_STREAM_IOCTL, &stream_request) != 0)
        goto done;
    rc = 0;

done:
    if (reader >= 0)
        close(reader);
    if (writer >= 0)
        close(writer);
    if (listener >= 0)
        close(listener);
    (void)unlink(socket_path == NULL ? "" : socket_path);
    return rc;
}

static int child_probe(const char *child)
{
    pid_t process = fork();
    int status;

    if (process < 0)
        return 1;
    if (process == 0) {
        execl(child, child, (char *)NULL);
        _exit(127);
    }
    if (waitpid(process, &status, 0) != process)
        return 1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "policy") == 0)
        return policy_probe();
    if (argc == 2 && strcmp(argv[1], "talkback") == 0)
        return talkback_probe();
    if (argc == 2 && strcmp(argv[1], "microphone") == 0)
        return microphone_probe();
    if (argc == 3 && strcmp(argv[1], "child") == 0)
        return child_probe(argv[2]);
    fprintf(stderr,
            "usage: %s policy|talkback|microphone|child CHILD\n", argv[0]);
    return 2;
}
