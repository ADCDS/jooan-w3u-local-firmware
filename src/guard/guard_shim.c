#define _GNU_SOURCE

#include "guard.h"
#include "guard_sha256.h"
#include "../audio/audio_guard_wire.h"
#include "../audio/audio_mic_wire.h"
#include "../audio/audio_wire.h"
#include "../audio/g711_alaw.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define TALKBACK_PACKET_MAX (JOOAN_AUDIO_GUARD_HEADER_SIZE + 1024U)
#define TALKBACK_POLL_MS 250
#define TALKBACK_LEASE_MS 500U

static int (*next_connect)(int, const struct sockaddr *, socklen_t);
static ssize_t (*next_sendto)(int, const void *, size_t, int,
                              const struct sockaddr *, socklen_t);
static ssize_t (*next_send)(int, const void *, size_t, int);
static ssize_t (*next_sendmsg)(int, const struct msghdr *, int);
#ifndef __UCLIBC__
static int (*next_sendmmsg)(int, struct mmsghdr *, unsigned int, int);
#endif
static int (*next_getaddrinfo)(const char *, const char *,
                              const struct addrinfo *, struct addrinfo **);
static struct hostent *(*next_gethostbyname)(const char *);
static struct hostent *(*next_gethostbyname2)(const char *, int);
static int (*next_gethostbyname_r)(const char *, struct hostent *, char *,
                                   size_t, struct hostent **, int *);
static int (*next_gethostbyname2_r)(const char *, int, struct hostent *, char *,
                                    size_t, struct hostent **, int *);
static int (*next_open)(const char *, int, ...);
static int (*next_open64)(const char *, int, ...);
static int (*next_close)(int);
static ssize_t (*next_write)(int, const void *, size_t);
static ssize_t (*next_read)(int, void *, size_t);
static ssize_t (*next_readv)(int, const struct iovec *, int);
static int (*next_ioctl)(int, unsigned long, ...);
static int (*next_getpeername)(int, struct sockaddr *, socklen_t *);
static int (*next_getsockname)(int, struct sockaddr *, socklen_t *);

static int guard_active;
static pid_t guard_owner_pid = -1;
static int dsp_descriptor = -1;
static int mic_descriptor = -1;
static int talkback_descriptor = -1;
static int mic_socket_descriptor = -1;
static int talkback_thread_started;
static int talkback_stop;
static pthread_t talkback_thread;
static pthread_mutex_t dsp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mic_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mic_read_lock = PTHREAD_MUTEX_INITIALIZER;
static char talkback_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct sockaddr_un mic_socket_address;
static socklen_t mic_socket_address_length;
static uint8_t mic_payload[JOOAN_AUDIO_MIC_PAYLOAD_SIZE];
static size_t mic_payload_length;
static uint8_t mic_low_byte;
static int mic_has_low_byte;
static int mic_gap;
static uint32_t mic_sequence;
static uint32_t mic_generation;

static pid_t current_process_id(void)
{
    return (pid_t)syscall(SYS_getpid);
}

static int guard_applies(void)
{
    return guard_active && current_process_id() == guard_owner_pid;
}

static void resolve_symbols(void)
{
#define RESOLVE(name) *(void **)(&next_##name) = dlsym(RTLD_NEXT, #name)
    RESOLVE(connect);
    RESOLVE(sendto);
    RESOLVE(send);
    RESOLVE(sendmsg);
#ifndef __UCLIBC__
    RESOLVE(sendmmsg);
#endif
    RESOLVE(getaddrinfo);
    RESOLVE(gethostbyname);
    RESOLVE(gethostbyname2);
    RESOLVE(gethostbyname_r);
    RESOLVE(gethostbyname2_r);
    RESOLVE(open);
    RESOLVE(open64);
    RESOLVE(close);
    RESOLVE(write);
    RESOLVE(read);
    RESOLVE(readv);
    RESOLVE(ioctl);
    RESOLVE(getpeername);
    RESOLVE(getsockname);
#undef RESOLVE
}

static int executable_path(char output[PATH_MAX])
{
    ssize_t length = readlink("/proc/self/exe", output, PATH_MAX - 1);
    if (length < 0)
        return -1;
    output[length] = '\0';
    return 0;
}

static int executable_is_supported(void)
{
    char path[PATH_MAX];
    char digest[65];

    if (executable_path(path) != 0)
        return 0;
#ifdef GUARD_TESTING
    {
        const char *test_path = getenv("JOOAN_GUARD_TEST_EXE");
        if (test_path != NULL && strcmp(test_path, path) == 0)
            return 1;
    }
#endif
    return jooan_guard_sha256_file_hex(path, digest) == 0 &&
           strcmp(digest, JOOAN_GUARD_SUPPORTED_JOOANIPC_SHA256) == 0;
}

static const char *configured_dsp_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_DSP_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_DSP_PATH;
}

static const char *configured_talkback_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_TALKBACK_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_TALKBACK_PATH;
}

static const char *configured_mic_socket_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_MIC_SOCKET_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_AUDIO_MIC_SOCKET_PATH;
}

static void disable_dsp_if_current(int descriptor)
{
    if (dsp_descriptor == descriptor)
        dsp_descriptor = -1;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &timestamp) != 0)
        return 0;
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000) +
           (uint64_t)timestamp.tv_nsec / UINT64_C(1000000);
}

static int talkback_should_stop(void)
{
    int stop;

    pthread_mutex_lock(&dsp_lock);
    stop = talkback_stop;
    pthread_mutex_unlock(&dsp_lock);
    return stop;
}

static int write_pcm_locked(int descriptor, const unsigned char *data,
                            size_t length)
{
    size_t offset = 0;
    ssize_t amount;

    if (next_write == NULL)
        return -1;
    while (offset < length) {
        amount = next_write(descriptor, data + offset, length - offset);
        if (amount > 0) {
            offset += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        disable_dsp_if_current(descriptor);
        return -1;
    }
    return 0;
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

static void microphone_emit_locked(void)
{
    uint8_t datagram[JOOAN_AUDIO_MIC_DATAGRAM_SIZE];
    int saved_errno = errno;

    if (mic_socket_descriptor < 0 || next_sendto == NULL)
        return;
    ++mic_sequence;
    if (mic_sequence == 0)
        mic_sequence = 1;
    memcpy(datagram, JOOAN_AUDIO_MIC_MAGIC, 4);
    datagram[4] = JOOAN_AUDIO_MIC_VERSION;
    datagram[5] = JOOAN_AUDIO_MIC_HEADER_SIZE;
    store_be16(datagram + 6, JOOAN_AUDIO_MIC_PAYLOAD_SIZE);
    store_be32(datagram + 8, mic_sequence);
    memcpy(datagram + JOOAN_AUDIO_MIC_HEADER_SIZE, mic_payload,
           sizeof(mic_payload));
    /* The socket and per-call flag are both nonblocking. No listener, a full
     * queue, or any other delivery failure drops this packet without changing
     * the OEM read result. */
    (void)next_sendto(mic_socket_descriptor, datagram, sizeof(datagram),
                      MSG_DONTWAIT | MSG_NOSIGNAL,
                      (const struct sockaddr *)&mic_socket_address,
                      mic_socket_address_length);
    errno = saved_errno;
}

static void microphone_feed_locked(const uint8_t *input, size_t length)
{
    int16_t sample;
    size_t index = 0;

    if (__atomic_exchange_n(&mic_gap, 0, __ATOMIC_ACQ_REL)) {
        mic_payload_length = 0;
        mic_has_low_byte = 0;
    }
    if (mic_has_low_byte && length != 0) {
        sample = (int16_t)((uint16_t)mic_low_byte |
                           ((uint16_t)input[index++] << 8));
        mic_payload[mic_payload_length++] = jooan_alaw_encode_sample(sample);
        mic_has_low_byte = 0;
        if (mic_payload_length == sizeof(mic_payload)) {
            microphone_emit_locked();
            mic_payload_length = 0;
        }
    }
    while (index + 1 < length) {
        sample = (int16_t)((uint16_t)input[index] |
                           ((uint16_t)input[index + 1] << 8));
        index += 2;
        mic_payload[mic_payload_length++] = jooan_alaw_encode_sample(sample);
        if (mic_payload_length == sizeof(mic_payload)) {
            microphone_emit_locked();
            mic_payload_length = 0;
        }
        if (__atomic_load_n(&mic_gap, __ATOMIC_ACQUIRE)) {
            mic_payload_length = 0;
            mic_has_low_byte = 0;
            return;
        }
    }
    if (index < length) {
        mic_low_byte = input[index];
        mic_has_low_byte = 1;
    }
}

static int microphone_try_begin(int descriptor, uint32_t generation)
{
    if (!guard_applies() ||
        descriptor != __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE) ||
        generation != __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE))
        return 0;
    if (pthread_mutex_trylock(&mic_lock) != 0) {
        __atomic_store_n(&mic_gap, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if (descriptor != __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE) ||
        generation != __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&mic_lock);
        return 0;
    }
    return 1;
}

static int microphone_capture_try_begin(int descriptor, uint32_t *generation)
{
    uint32_t observed;

    if (!guard_applies() || generation == NULL ||
        descriptor != __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE))
        return 0;
    observed = __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE);
    if (pthread_mutex_trylock(&mic_read_lock) != 0) {
        __atomic_store_n(&mic_gap, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if (descriptor != __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE) ||
        observed != __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&mic_read_lock);
        return 0;
    }
    *generation = observed;
    return 1;
}

static void microphone_tap_buffer(int descriptor, uint32_t generation,
                                  const void *buffer, size_t length)
{
    if (length == 0 || !microphone_try_begin(descriptor, generation))
        return;
    microphone_feed_locked((const uint8_t *)buffer, length);
    pthread_mutex_unlock(&mic_lock);
}

static void microphone_tap_iov(int descriptor, uint32_t generation,
                               const struct iovec *vectors, int vector_count,
                               size_t length)
{
    size_t remaining = length;
    size_t amount;
    int index;

    if (length == 0 || vectors == NULL || vector_count <= 0 ||
        !microphone_try_begin(descriptor, generation))
        return;
    for (index = 0; index < vector_count && remaining != 0; ++index) {
        amount = vectors[index].iov_len;
        if (amount > remaining)
            amount = remaining;
        microphone_feed_locked((const uint8_t *)vectors[index].iov_base,
                               amount);
        remaining -= amount;
    }
    pthread_mutex_unlock(&mic_lock);
}

static void start_microphone_tap(void)
{
    const char *path = configured_mic_socket_path();
    size_t path_length = strlen(path);
    int flags;

    if (path_length == 0 || path_length >= sizeof(mic_socket_address.sun_path))
        return;
    mic_socket_descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (mic_socket_descriptor < 0)
        return;
    flags = fcntl(mic_socket_descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(mic_socket_descriptor, F_SETFL,
                           flags | O_NONBLOCK) != 0) {
        next_close(mic_socket_descriptor);
        mic_socket_descriptor = -1;
        return;
    }
    (void)fcntl(mic_socket_descriptor, F_SETFD, FD_CLOEXEC);
    memset(&mic_socket_address, 0, sizeof(mic_socket_address));
    mic_socket_address.sun_family = AF_UNIX;
    memcpy(mic_socket_address.sun_path, path, path_length + 1);
    mic_socket_address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1);
}

static void *talkback_main(void *unused)
{
    unsigned char datagram[TALKBACK_PACKET_MAX];
    unsigned char pcm[JOOAN_AUDIO_WIRE_MAX_PAYLOAD * 2U];
    struct jooan_audio_guard_frame frame;
    struct pollfd poll_descriptor;
    ssize_t amount;
    size_t index;
    int descriptor;
    int16_t sample;
    uint64_t active_session = 0;
    uint64_t lease_deadline = 0;
    uint64_t now;
    uint64_t remaining;
    uint32_t last_sequence = 0;
    int poll_timeout;

    (void)unused;
    poll_descriptor.fd = talkback_descriptor;
    poll_descriptor.events = POLLIN;
    while (!talkback_should_stop()) {
        now = monotonic_milliseconds();
        if (active_session != 0 && now >= lease_deadline) {
            active_session = 0;
            last_sequence = 0;
            lease_deadline = 0;
        }
        poll_timeout = TALKBACK_POLL_MS;
        if (active_session != 0) {
            remaining = lease_deadline - now;
            if (remaining < (uint64_t)poll_timeout)
                poll_timeout = remaining == 0 ? 1 : (int)remaining;
        }
        poll_descriptor.revents = 0;
        amount = poll(&poll_descriptor, 1, poll_timeout);
        now = monotonic_milliseconds();
        if (active_session != 0 && now >= lease_deadline) {
            active_session = 0;
            last_sequence = 0;
            lease_deadline = 0;
        }
        if (amount == 0)
            continue;
        if (amount < 0)
            continue;
        if ((poll_descriptor.revents & POLLIN) == 0)
            continue;
        amount = recv(talkback_descriptor, datagram, sizeof(datagram), 0);
        if (amount <= 0)
            continue;

        if (jooan_audio_guard_datagram_parse(datagram, (size_t)amount,
                                              &frame) != 0) {
            active_session = 0;
            last_sequence = 0;
            lease_deadline = 0;
            continue;
        }
        if (frame.type == JOOAN_AUDIO_PTT_ACQUIRE) {
            if (active_session == 0 && frame.sequence == 1) {
                active_session = frame.session_id;
                last_sequence = frame.sequence;
                lease_deadline = now + TALKBACK_LEASE_MS;
            }
            continue;
        }
        if (active_session == 0 || frame.session_id != active_session ||
            last_sequence == UINT32_MAX || frame.sequence != last_sequence + 1U) {
            active_session = 0;
            last_sequence = 0;
            lease_deadline = 0;
            continue;
        }
        last_sequence = frame.sequence;
        if (frame.type == JOOAN_AUDIO_PTT_RELEASE) {
            active_session = 0;
            last_sequence = 0;
            lease_deadline = 0;
            continue;
        }
        if (frame.type != JOOAN_AUDIO_PTT_PCMA)
            continue;
        lease_deadline = now + TALKBACK_LEASE_MS;

        for (index = 0; index < frame.payload_length; ++index) {
            sample = jooan_guard_alaw_to_pcm16(frame.payload[index]);
            pcm[index * 2] = (unsigned char)((uint16_t)sample & 0xffU);
            pcm[index * 2 + 1] = (unsigned char)((uint16_t)sample >> 8);
        }

        pthread_mutex_lock(&dsp_lock);
        descriptor = dsp_descriptor;
        if (descriptor >= 0)
            (void)write_pcm_locked(descriptor, pcm,
                                   (size_t)frame.payload_length * 2U);
        pthread_mutex_unlock(&dsp_lock);
    }
    return NULL;
}

static void start_talkback(void)
{
    struct sockaddr_un address;
    struct stat status;
    const char *path = configured_talkback_path();
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(address.sun_path))
        return;
    if (lstat(path, &status) == 0) {
        if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid())
            return;
        if (unlink(path) != 0)
            return;
    } else if (errno != ENOENT) {
        return;
    }

    talkback_descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (talkback_descriptor < 0)
        return;
    (void)fcntl(talkback_descriptor, F_SETFD, FD_CLOEXEC);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1);
    if (bind(talkback_descriptor, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1)) != 0) {
        next_close(talkback_descriptor);
        talkback_descriptor = -1;
        return;
    }
    if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
        next_close(talkback_descriptor);
        talkback_descriptor = -1;
        unlink(path);
        return;
    }
    memcpy(talkback_path, path, length + 1);
    if (pthread_create(&talkback_thread, NULL, talkback_main, NULL) != 0) {
        next_close(talkback_descriptor);
        talkback_descriptor = -1;
        unlink(talkback_path);
        talkback_path[0] = '\0';
        return;
    }
    talkback_thread_started = 1;
}

__attribute__((constructor)) static void guard_initialize(void)
{
    resolve_symbols();
    guard_active = executable_is_supported();
    if (guard_active && next_close != NULL && next_write != NULL) {
        guard_owner_pid = current_process_id();
        start_talkback();
        if (next_read != NULL && next_readv != NULL && next_sendto != NULL)
            start_microphone_tap();
    }
}

__attribute__((destructor)) static void guard_shutdown(void)
{
    if (!guard_applies())
        return;
    pthread_mutex_lock(&dsp_lock);
    talkback_stop = 1;
    pthread_mutex_unlock(&dsp_lock);
    if (talkback_thread_started)
        (void)pthread_join(talkback_thread, NULL);
    if (talkback_descriptor >= 0 && next_close != NULL)
        next_close(talkback_descriptor);
    if (mic_socket_descriptor >= 0 && next_close != NULL)
        next_close(mic_socket_descriptor);
    if (talkback_path[0] != '\0')
        unlink(talkback_path);
}

static int deny_network(void)
{
    errno = EACCES;
    return -1;
}

static int connected_socket_is_allowed(int descriptor);

int connect(int descriptor, const struct sockaddr *address, socklen_t length)
{
    struct sockaddr_in redirected;

    if (next_connect == NULL)
        resolve_symbols();
    if (guard_applies() && !jooan_guard_sockaddr_is_loopback(address, length))
        return deny_network();
    if (next_connect == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (guard_applies() && address != NULL &&
        length >= (socklen_t)sizeof(redirected) &&
        address->sa_family == AF_INET) {
        memcpy(&redirected, address, sizeof(redirected));
        if (ntohl(redirected.sin_addr.s_addr) == 0x7f000002UL) {
            redirected.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ((unsigned char *)&redirected.sin_port)[0] =
                (unsigned char)(JOOAN_GUARD_LOCAL_MQTT_PORT >> 8);
            ((unsigned char *)&redirected.sin_port)[1] =
                (unsigned char)(JOOAN_GUARD_LOCAL_MQTT_PORT & 0xff);
            return next_connect(descriptor, (const struct sockaddr *)&redirected,
                                sizeof(redirected));
        }
        if (ntohl(redirected.sin_addr.s_addr) == 0x7f000003UL) {
            redirected.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return next_connect(descriptor, (const struct sockaddr *)&redirected,
                                sizeof(redirected));
        }
    }
    return next_connect(descriptor, address, length);
}

ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length)
{
    if (next_sendto == NULL)
        resolve_symbols();
    if (guard_applies()) {
        if (address != NULL &&
            !jooan_guard_sockaddr_is_loopback(address, address_length))
            return (ssize_t)deny_network();
        if (address == NULL && !connected_socket_is_allowed(descriptor))
            return (ssize_t)deny_network();
    }
    if (next_sendto == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_sendto(descriptor, buffer, length, flags, address,
                       address_length);
}

static int connected_socket_is_allowed(int descriptor)
{
    struct sockaddr_storage peer;
    struct sockaddr_storage local;
    socklen_t peer_length = sizeof(peer);
    socklen_t local_length = sizeof(local);
    uint16_t port;

    if (next_getpeername == NULL ||
        next_getpeername(descriptor,(struct sockaddr *)&peer,&peer_length) != 0)
        return 0;
    if (jooan_guard_sockaddr_is_loopback((struct sockaddr *)&peer,peer_length))
        return 1;
    if (next_getsockname == NULL ||
        next_getsockname(descriptor,(struct sockaddr *)&local,&local_length) != 0 ||
        local.ss_family != AF_INET || local_length < sizeof(struct sockaddr_in))
        return 0;
    port=(uint16_t)(((const unsigned char *)&
        ((struct sockaddr_in *)&local)->sin_port)[0] << 8);
    port=(uint16_t)(port | ((const unsigned char *)&
        ((struct sockaddr_in *)&local)->sin_port)[1]);
    return port==554U || port==8899U || port==9898U || port==24569U;
}

ssize_t send(int descriptor, const void *buffer, size_t length, int flags)
{
    if (next_send == NULL)
        resolve_symbols();
    if (guard_applies() && !connected_socket_is_allowed(descriptor))
        return (ssize_t)deny_network();
    if (next_send == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_send(descriptor, buffer, length, flags);
}

static int message_destination_is_loopback(int descriptor,
                                           const struct msghdr *message)
{
    if (message == NULL)
        return 0;
    if (message->msg_name != NULL)
        return jooan_guard_sockaddr_is_loopback(
            (const struct sockaddr *)message->msg_name,
            (socklen_t)message->msg_namelen);
    return connected_socket_is_allowed(descriptor);
}

ssize_t sendmsg(int descriptor, const struct msghdr *message, int flags)
{
    if (next_sendmsg == NULL)
        resolve_symbols();
    if (guard_applies() && !message_destination_is_loopback(descriptor, message))
        return (ssize_t)deny_network();
    if (next_sendmsg == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_sendmsg(descriptor, message, flags);
}

#ifndef __UCLIBC__
int sendmmsg(int descriptor, struct mmsghdr *messages, unsigned int count,
             int flags)
{
    unsigned int index;

    if (next_sendmmsg == NULL)
        resolve_symbols();
    if (guard_applies()) {
        if (messages == NULL)
            return deny_network();
        for (index = 0; index < count; ++index) {
            if (!message_destination_is_loopback(descriptor,
                                                 &messages[index].msg_hdr))
                return deny_network();
        }
    }
    if (next_sendmmsg == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_sendmmsg(descriptor, messages, count, flags);
}
#endif

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result)
{
    const char *redirect;

    if (next_getaddrinfo == NULL)
        resolve_symbols();
    if (next_getaddrinfo == NULL)
        return EAI_SYSTEM;
    if (!guard_applies() || node == NULL)
        return next_getaddrinfo(node, service, hints, result);
    if (jooan_guard_hostname_is_approved(node)) {
        if (hints != NULL && hints->ai_family == AF_INET6)
            redirect = JOOAN_GUARD_LOOPBACK6;
        else if (jooan_guard_hostname_is_mqtt(node))
            redirect = JOOAN_GUARD_MQTT_LOOPBACK4;
        else
            redirect = JOOAN_GUARD_API_LOOPBACK4;
        return next_getaddrinfo(redirect, service, hints, result);
    }
    if (jooan_guard_hostname_is_loopback(node))
        return next_getaddrinfo(node, service, hints, result);
    return EAI_NONAME;
}

static struct hostent *guard_host_lookup(const char *name, int family)
{
    const char *redirect;

    if (!jooan_guard_hostname_is_approved(name) &&
        !jooan_guard_hostname_is_loopback(name)) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    if (family == AF_INET6)
        redirect = JOOAN_GUARD_LOOPBACK6;
    else if (jooan_guard_hostname_is_mqtt(name))
        redirect = JOOAN_GUARD_MQTT_LOOPBACK4;
    else if (jooan_guard_hostname_is_api(name))
        redirect = JOOAN_GUARD_API_LOOPBACK4;
    else
        redirect = JOOAN_GUARD_LOOPBACK4;
    if (family == AF_INET6 && next_gethostbyname2 != NULL)
        return next_gethostbyname2(redirect, family);
    return next_gethostbyname(redirect);
}

struct hostent *gethostbyname(const char *name)
{
    if (next_gethostbyname == NULL)
        resolve_symbols();
    if (next_gethostbyname == NULL) {
        h_errno = NO_RECOVERY;
        return NULL;
    }
    if (!guard_applies())
        return next_gethostbyname(name);
    return guard_host_lookup(name, AF_INET);
}

struct hostent *gethostbyname2(const char *name, int family)
{
    if (next_gethostbyname2 == NULL || next_gethostbyname == NULL)
        resolve_symbols();
    if (next_gethostbyname == NULL) {
        h_errno = NO_RECOVERY;
        return NULL;
    }
    if (!guard_applies())
        return next_gethostbyname2 != NULL ?
               next_gethostbyname2(name, family) : next_gethostbyname(name);
    return guard_host_lookup(name, family);
}

int gethostbyname_r(const char *name, struct hostent *entry, char *buffer,
                    size_t buffer_length, struct hostent **result,
                    int *host_error)
{
    const char *redirect = name;

    if (next_gethostbyname_r == NULL)
        resolve_symbols();
    if (next_gethostbyname_r == NULL)
        return ENOSYS;
    if (guard_applies()) {
        if (jooan_guard_hostname_is_mqtt(name))
            redirect = JOOAN_GUARD_MQTT_LOOPBACK4;
        else if (jooan_guard_hostname_is_api(name))
            redirect = JOOAN_GUARD_API_LOOPBACK4;
        else if (!jooan_guard_hostname_is_loopback(name)) {
            *result = NULL;
            if (host_error != NULL)
                *host_error = HOST_NOT_FOUND;
            return 0;
        }
    }
    return next_gethostbyname_r(redirect, entry, buffer, buffer_length,
                                result, host_error);
}

int gethostbyname2_r(const char *name, int family, struct hostent *entry,
                     char *buffer, size_t buffer_length,
                     struct hostent **result, int *host_error)
{
    const char *redirect = name;

    if (next_gethostbyname2_r == NULL)
        resolve_symbols();
    if (next_gethostbyname2_r == NULL)
        return ENOSYS;
    if (guard_applies()) {
        if (jooan_guard_hostname_is_approved(name)) {
            if (family == AF_INET6)
                redirect = JOOAN_GUARD_LOOPBACK6;
            else if (jooan_guard_hostname_is_mqtt(name))
                redirect = JOOAN_GUARD_MQTT_LOOPBACK4;
            else
                redirect = JOOAN_GUARD_API_LOOPBACK4;
        }
        else if (!jooan_guard_hostname_is_loopback(name)) {
            *result = NULL;
            if (host_error != NULL)
                *host_error = HOST_NOT_FOUND;
            return 0;
        }
    }
    return next_gethostbyname2_r(redirect, family, entry, buffer,
                                 buffer_length, result, host_error);
}

static int guarded_open(const char *path, int flags, mode_t mode, int use_mode,
                        int use_open64)
{
    int descriptor;

    if (next_open == NULL)
        resolve_symbols();
    if (use_open64 && next_open64 != NULL)
        descriptor = use_mode ? next_open64(path, flags, mode) :
                                next_open64(path, flags);
    else if (next_open != NULL)
        descriptor = use_mode ? next_open(path, flags, mode) :
                                next_open(path, flags);
    else {
        errno = ENOSYS;
        return -1;
    }
    if (guard_applies() && descriptor >= 0 &&
        strcmp(path, configured_dsp_path()) == 0) {
        if ((flags & O_ACCMODE) == O_WRONLY) {
            pthread_mutex_lock(&dsp_lock);
            dsp_descriptor = descriptor;
            pthread_mutex_unlock(&dsp_lock);
        } else if ((flags & O_ACCMODE) == O_RDONLY) {
            pthread_mutex_lock(&mic_lock);
            __atomic_store_n(&mic_descriptor, descriptor, __ATOMIC_RELEASE);
            __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
            mic_payload_length = 0;
            mic_has_low_byte = 0;
            __atomic_store_n(&mic_gap, 0, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&mic_lock);
        }
    }
    return descriptor;
}

/* Some 32-bit libc headers redirect the C identifier `open` to the assembler
 * symbol `open64` when their toolchain defaults to 64-bit file offsets. Give
 * each exported interposer an explicit assembler name so both ABI entry
 * points remain present regardless of those header redirects. */
int guard_export_open(const char *path, int flags, ...) __asm__("open");
int guard_export_open64(const char *path, int flags, ...) __asm__("open64");

int guard_export_open(const char *path, int flags, ...)
{
    va_list arguments;
    mode_t mode = 0;
    int use_mode = (flags & O_CREAT) != 0;

    if (use_mode) {
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return guarded_open(path, flags, mode, use_mode, 0);
}

int guard_export_open64(const char *path, int flags, ...)
{
    va_list arguments;
    mode_t mode = 0;
    int use_mode = (flags & O_CREAT) != 0;

    if (use_mode) {
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return guarded_open(path, flags, mode, use_mode, 1);
}

ssize_t write(int descriptor, const void *buffer, size_t length)
{
    ssize_t result;

    if (next_write == NULL)
        resolve_symbols();
    if (next_write == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!guard_applies())
        return next_write(descriptor, buffer, length);

    pthread_mutex_lock(&dsp_lock);
    if (descriptor == dsp_descriptor) {
        result = next_write(descriptor, buffer, length);
        if (result < 0)
            disable_dsp_if_current(descriptor);
    } else {
        pthread_mutex_unlock(&dsp_lock);
        return next_write(descriptor, buffer, length);
    }
    pthread_mutex_unlock(&dsp_lock);
    return result;
}

ssize_t read(int descriptor, void *buffer, size_t length)
{
    ssize_t result;
    uint32_t generation = 0;
    int capture;
    int saved_errno;

    if (next_read == NULL)
        resolve_symbols();
    if (next_read == NULL) {
        errno = ENOSYS;
        return -1;
    }
    capture = microphone_capture_try_begin(descriptor, &generation);
    result = next_read(descriptor, buffer, length);
    saved_errno = errno;
    if (capture) {
        if (result > 0)
            microphone_tap_buffer(descriptor, generation, buffer,
                                  (size_t)result);
        pthread_mutex_unlock(&mic_read_lock);
    }
    errno = saved_errno;
    return result;
}

ssize_t readv(int descriptor, const struct iovec *vectors, int vector_count)
{
    ssize_t result;
    uint32_t generation = 0;
    int capture;
    int saved_errno;

    if (next_readv == NULL)
        resolve_symbols();
    if (next_readv == NULL) {
        errno = ENOSYS;
        return -1;
    }
    capture = microphone_capture_try_begin(descriptor, &generation);
    result = next_readv(descriptor, vectors, vector_count);
    saved_errno = errno;
    if (capture) {
        if (result > 0)
            microphone_tap_iov(descriptor, generation, vectors, vector_count,
                               (size_t)result);
        pthread_mutex_unlock(&mic_read_lock);
    }
    errno = saved_errno;
    return result;
}

/* Modern 32-bit glibc headers may redirect the C identifier to
 * __ioctl_time64. The camera ABI and uClibc binary import the literal `ioctl`
 * symbol, so pin the exported assembler name just as the open wrappers do. */
int guard_export_ioctl(int descriptor, unsigned long request, ...)
    __asm__("ioctl");

int guard_export_ioctl(int descriptor, unsigned long request, ...)
{
    va_list arguments;
    void *argument;
    int result;
    int saved_errno;
    int capture;
    uint32_t generation = 0;

    if (next_ioctl == NULL)
        resolve_symbols();
    if (next_ioctl == NULL) {
        errno = ENOSYS;
        return -1;
    }
    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);

    if (!guard_applies())
        return next_ioctl(descriptor, request, argument);

    if (descriptor ==
            __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE) &&
        request == JOOAN_GUARD_MIC_STREAM_IOCTL) {
        const struct jooan_guard_mic_stream_request *stream = argument;
        capture = microphone_capture_try_begin(descriptor, &generation);
#ifdef GUARD_TESTING
        if (getenv("JOOAN_GUARD_TEST_FAKE_MIC_IOCTL") != NULL)
            result = 0;
        else
#endif
            result = next_ioctl(descriptor, request, argument);
        saved_errno = errno;
        if (capture && result == 0 && stream != NULL && stream->pcm != 0 &&
            stream->byte_length <= 65536U)
            microphone_tap_buffer(descriptor, generation,
                                  (const void *)stream->pcm,
                                  stream->byte_length);
        if (capture)
            pthread_mutex_unlock(&mic_read_lock);
        errno = saved_errno;
        return result;
    }
    pthread_mutex_lock(&dsp_lock);
    if (descriptor == dsp_descriptor)
        result = next_ioctl(descriptor, request, argument);
    else {
        pthread_mutex_unlock(&dsp_lock);
        return next_ioctl(descriptor, request, argument);
    }
    pthread_mutex_unlock(&dsp_lock);
    return result;
}

int close(int descriptor)
{
    if (next_close == NULL)
        resolve_symbols();
    if (guard_applies()) {
        pthread_mutex_lock(&dsp_lock);
        disable_dsp_if_current(descriptor);
        pthread_mutex_unlock(&dsp_lock);

        if (descriptor ==
            __atomic_load_n(&mic_descriptor, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&mic_lock);
            if (descriptor ==
                __atomic_load_n(&mic_descriptor, __ATOMIC_RELAXED)) {
                __atomic_store_n(&mic_descriptor, -1, __ATOMIC_RELEASE);
                __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
                mic_payload_length = 0;
                mic_has_low_byte = 0;
                __atomic_store_n(&mic_gap, 0, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&mic_lock);
        }
    }
    if (next_close == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_close(descriptor);
}
