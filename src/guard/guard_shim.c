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
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define TALKBACK_PACKET_MAX (JOOAN_AUDIO_GUARD_HEADER_SIZE + 1024U)
#define TALKBACK_POLL_MS 250
#define TALKBACK_LEASE_MS 500U
#define TALKBACK_MAX_MS 60000U
#define GUARD_TRACKED_FDS 1024
#define FD_ROLE_DSP_WRITE 0x01U
#define FD_ROLE_DSP_READ  0x02U
#define FD_ROLE_SPEAKER_DIRECTION 0x04U
#define FD_ROLE_SPEAKER_VALUE     0x08U

#ifdef __UCLIBC__
struct guard_mmsghdr {
    struct msghdr msg_hdr;
    unsigned int msg_len;
};
#else
#define guard_mmsghdr mmsghdr
#endif

static int (*next_connect)(int, const struct sockaddr *, socklen_t);
static int (*next_bind)(int, const struct sockaddr *, socklen_t);
static int (*next_accept)(int, struct sockaddr *, socklen_t *);
static int (*next_accept4)(int, struct sockaddr *, socklen_t *, int);
static ssize_t (*next_sendto)(int, const void *, size_t, int,
                              const struct sockaddr *, socklen_t);
static ssize_t (*next_send)(int, const void *, size_t, int);
static ssize_t (*next_sendmsg)(int, const struct msghdr *, int);
static int (*next_sendmmsg)(int, struct guard_mmsghdr *, unsigned int, int);
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
static ssize_t (*next_writev)(int, const struct iovec *, int);
static ssize_t (*next_sendfile64)(int, int, off64_t *, size_t);
static ssize_t (*next_read)(int, void *, size_t);
static ssize_t (*next_readv)(int, const struct iovec *, int);
static int (*next_ioctl)(int, unsigned long, ...);
static int (*next_getpeername)(int, struct sockaddr *, socklen_t *);
static int (*next_getsockname)(int, struct sockaddr *, socklen_t *);
static int (*next_getsockopt)(int, int, int, void *, socklen_t *);
static int (*next_dup)(int);
static int (*next_dup2)(int, int);
static int (*next_dup3)(int, int, int);
static int (*next_fcntl)(int, int, ...);
static long (*next_syscall)(long, long, long, long, long, long, long);

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
static pthread_mutex_t speaker_lock = PTHREAD_MUTEX_INITIALIZER;
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
static int speaker_enabled;
static unsigned char fd_roles[GUARD_TRACKED_FDS];

static pid_t current_process_id(void)
{
    return (pid_t)syscall(SYS_getpid);
}

static int guard_applies(void)
{
    return guard_active && current_process_id() == guard_owner_pid;
}

static unsigned int fd_role_load(int descriptor)
{
    if (descriptor < 0 || descriptor >= GUARD_TRACKED_FDS)
        return 0;
    return __atomic_load_n(&fd_roles[descriptor], __ATOMIC_ACQUIRE);
}

static int find_role_fd(unsigned int role, int excluded)
{
    int descriptor;

    for (descriptor = 0; descriptor < GUARD_TRACKED_FDS; ++descriptor) {
        if (descriptor != excluded && (fd_role_load(descriptor) & role) != 0)
            return descriptor;
    }
    return -1;
}

static void reset_microphone_state_locked(void)
{
    mic_payload_length = 0;
    mic_has_low_byte = 0;
    __atomic_store_n(&mic_gap, 0, __ATOMIC_RELEASE);
}

static void track_fresh_fd(int descriptor, unsigned int role)
{
    unsigned int previous;

    if (descriptor < 0 || descriptor >= GUARD_TRACKED_FDS)
        return;
    previous = __atomic_exchange_n(&fd_roles[descriptor], (unsigned char)role,
                                   __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&dsp_lock);
    if ((role & FD_ROLE_DSP_WRITE) != 0)
        dsp_descriptor = descriptor;
    else if ((previous & FD_ROLE_DSP_WRITE) != 0 && dsp_descriptor == descriptor)
        dsp_descriptor = find_role_fd(FD_ROLE_DSP_WRITE, descriptor);
    pthread_mutex_unlock(&dsp_lock);

    pthread_mutex_lock(&mic_lock);
    if ((role & FD_ROLE_DSP_READ) != 0) {
        __atomic_store_n(&mic_descriptor, descriptor, __ATOMIC_RELEASE);
        __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
        reset_microphone_state_locked();
    } else if ((previous & FD_ROLE_DSP_READ) != 0 &&
               __atomic_load_n(&mic_descriptor, __ATOMIC_RELAXED) == descriptor) {
        __atomic_store_n(&mic_descriptor,
                         find_role_fd(FD_ROLE_DSP_READ, descriptor),
                         __ATOMIC_RELEASE);
        if (__atomic_load_n(&mic_descriptor, __ATOMIC_RELAXED) < 0) {
            __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
            reset_microphone_state_locked();
        }
    }
    pthread_mutex_unlock(&mic_lock);
}

static unsigned int untrack_fd(int descriptor)
{
    unsigned int roles;
    int replacement;

    if (descriptor < 0 || descriptor >= GUARD_TRACKED_FDS)
        return 0;
    roles = __atomic_exchange_n(&fd_roles[descriptor], 0, __ATOMIC_ACQ_REL);
    if ((roles & FD_ROLE_DSP_WRITE) != 0) {
        pthread_mutex_lock(&dsp_lock);
        if (dsp_descriptor == descriptor)
            dsp_descriptor = find_role_fd(FD_ROLE_DSP_WRITE, descriptor);
        pthread_mutex_unlock(&dsp_lock);
    }
    if ((roles & FD_ROLE_DSP_READ) != 0) {
        pthread_mutex_lock(&mic_lock);
        if (__atomic_load_n(&mic_descriptor, __ATOMIC_RELAXED) == descriptor) {
            replacement = find_role_fd(FD_ROLE_DSP_READ, descriptor);
            __atomic_store_n(&mic_descriptor, replacement, __ATOMIC_RELEASE);
            if (replacement < 0) {
                __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
                reset_microphone_state_locked();
            }
        }
        pthread_mutex_unlock(&mic_lock);
    }
    return roles;
}

/* Caller holds mic_read_lock, dsp_lock and mic_lock in that order. */
static void assign_duplicated_fd_locked(int destination, unsigned int roles,
                                        unsigned int previous_roles)
{
    int replacement;

    if (destination < 0 || destination >= GUARD_TRACKED_FDS)
        return;
    __atomic_store_n(&fd_roles[destination], (unsigned char)roles,
                     __ATOMIC_RELEASE);
    if ((roles & FD_ROLE_DSP_WRITE) != 0)
        dsp_descriptor = destination;
    else if ((previous_roles & FD_ROLE_DSP_WRITE) != 0 &&
             dsp_descriptor == destination)
        dsp_descriptor = find_role_fd(FD_ROLE_DSP_WRITE, destination);

    if ((roles & FD_ROLE_DSP_READ) != 0) {
        __atomic_store_n(&mic_descriptor, destination, __ATOMIC_RELEASE);
    } else if ((previous_roles & FD_ROLE_DSP_READ) != 0 &&
               __atomic_load_n(&mic_descriptor,
                               __ATOMIC_RELAXED) == destination) {
        replacement = find_role_fd(FD_ROLE_DSP_READ, destination);
        __atomic_store_n(&mic_descriptor, replacement, __ATOMIC_RELEASE);
        if (replacement < 0) {
            __atomic_add_fetch(&mic_generation, 1U, __ATOMIC_RELEASE);
            reset_microphone_state_locked();
        }
    }
}

static void resolve_symbols(void)
{
#define RESOLVE(name) *(void **)(&next_##name) = dlsym(RTLD_NEXT, #name)
    RESOLVE(connect);
    RESOLVE(bind);
    RESOLVE(accept);
    RESOLVE(accept4);
    RESOLVE(sendto);
    RESOLVE(send);
    RESOLVE(sendmsg);
    RESOLVE(sendmmsg);
    RESOLVE(getaddrinfo);
    RESOLVE(gethostbyname);
    RESOLVE(gethostbyname2);
    RESOLVE(gethostbyname_r);
    RESOLVE(gethostbyname2_r);
    RESOLVE(open);
    RESOLVE(open64);
    RESOLVE(close);
    RESOLVE(write);
    RESOLVE(writev);
    RESOLVE(sendfile64);
    RESOLVE(read);
    RESOLVE(readv);
    RESOLVE(ioctl);
    RESOLVE(getpeername);
    RESOLVE(getsockname);
    RESOLVE(getsockopt);
    RESOLVE(dup);
    RESOLVE(dup2);
    RESOLVE(dup3);
    RESOLVE(fcntl);
    RESOLVE(syscall);
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

static int executable_is_named_jooanipc(void)
{
    char path[PATH_MAX];
    const char *name;

    if (executable_path(path) != 0)
        return 0;
    name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    return strcmp(name, "jooanipc") == 0;
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

static const char *configured_speaker_direction_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_SPEAKER_DIRECTION_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_SPEAKER_DIRECTION_PATH;
}

static const char *configured_speaker_value_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_SPEAKER_VALUE_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_SPEAKER_VALUE_PATH;
}

static const char *configured_speaker_ready_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_SPEAKER_READY_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_SPEAKER_READY_PATH;
}

static const char *configured_speaker_released_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_SPEAKER_RELEASED_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_SPEAKER_RELEASED_PATH;
}

static int speaker_write_path(const char *path, const char *value,
                              size_t length)
{
    int descriptor;
    ssize_t amount;
    int saved_errno;

    if (next_open == NULL || next_write == NULL || next_close == NULL)
        return -1;
    descriptor = next_open(path, O_WRONLY);
    if (descriptor < 0)
        return -1;
    amount = next_write(descriptor, value, length);
    saved_errno = errno;
    (void)next_close(descriptor);
    errno = saved_errno;
    return amount == (ssize_t)length ? 0 : -1;
}

static int speaker_read_level(char *level)
{
    int descriptor;
    ssize_t amount;
    int saved_errno;

    if (level == NULL || next_open == NULL || next_read == NULL ||
        next_close == NULL)
        return -1;
    descriptor = next_open(configured_speaker_value_path(), O_RDONLY);
    if (descriptor < 0)
        return -1;
    amount = next_read(descriptor, level, 1);
    saved_errno = errno;
    (void)next_close(descriptor);
    errno = saved_errno;
    return amount == 1 ? 0 : -1;
}

static int speaker_set_enabled(int enabled)
{
    const char level = enabled ? JOOAN_GUARD_SPEAKER_ENABLED_LEVEL :
                                 JOOAN_GUARD_SPEAKER_MUTED_LEVEL;
    const char muted = JOOAN_GUARD_SPEAKER_MUTED_LEVEL;
    int direction_result;
    int value_result;
    int attempt;
    char observed = '\0';

    pthread_mutex_lock(&speaker_lock);
    for (attempt = 0; attempt < 3; ++attempt) {
        direction_result = speaker_write_path(
            configured_speaker_direction_path(), "out", 3);
        value_result = speaker_write_path(configured_speaker_value_path(),
                                          &level, 1);
        if (direction_result == 0 && value_result == 0 &&
            speaker_read_level(&observed) == 0 && observed == level) {
            __atomic_store_n(&speaker_enabled, enabled != 0,
                             __ATOMIC_RELEASE);
            pthread_mutex_unlock(&speaker_lock);
            return 0;
        }
        usleep(1000);
    }
    __atomic_store_n(&speaker_enabled, 0, __ATOMIC_RELEASE);
    for (attempt = 0; attempt < 3; ++attempt) {
        (void)speaker_write_path(configured_speaker_direction_path(),
                                 "out", 3);
        (void)speaker_write_path(configured_speaker_value_path(), &muted, 1);
        if (speaker_read_level(&observed) == 0 && observed == muted)
            break;
        usleep(1000);
    }
    pthread_mutex_unlock(&speaker_lock);
    return -1;
}

static int speaker_publish_ready(void)
{
    static const char ready[] = "ready\n";
    const char *path = configured_speaker_ready_path();
    int descriptor;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    ssize_t amount;
    int saved_errno;

    if (next_open == NULL || next_write == NULL || next_close == NULL)
        return -1;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor = next_open(path, flags, 0600);
    if (descriptor < 0)
        return -1;
    amount = next_write(descriptor, ready, sizeof(ready) - 1U);
    saved_errno = errno;
    (void)next_close(descriptor);
    errno = saved_errno;
    return amount == (ssize_t)(sizeof(ready) - 1U) ? 0 : -1;
}

static int speaker_wait_hook_release(void)
{
    const char *path = configured_speaker_released_path();
    struct stat status;
    int attempt;

    for (attempt = 0; attempt < 200; ++attempt) {
        if (lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == geteuid())
            return 0;
        usleep(10000);
    }
    return -1;
}

static ssize_t speaker_confined_write(int descriptor, unsigned int role,
                                      size_t reported_length)
{
    char level;
    const char *value;
    size_t value_length = 1;
    ssize_t amount;

    if (reported_length > (size_t)SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&speaker_lock);
    level = __atomic_load_n(&speaker_enabled, __ATOMIC_ACQUIRE) ?
            JOOAN_GUARD_SPEAKER_ENABLED_LEVEL :
            JOOAN_GUARD_SPEAKER_MUTED_LEVEL;
    value = &level;
    if ((role & FD_ROLE_SPEAKER_DIRECTION) != 0) {
        value = "out";
        value_length = 3;
    }
    amount = next_write(descriptor, value, value_length);
    if (amount != (ssize_t)value_length) {
        pthread_mutex_unlock(&speaker_lock);
        return amount < 0 ? amount : (errno = EIO, -1);
    }
    pthread_mutex_unlock(&speaker_lock);
    return (ssize_t)reported_length;
}

#ifdef GUARD_TESTING
static const char *configured_mic_socket_path(void)
{
    const char *path = getenv("JOOAN_GUARD_TEST_MIC_SOCKET_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
    return JOOAN_AUDIO_MIC_SOCKET_PATH;
}
#endif

static uint16_t configured_local_mqtt_port(void)
{
#ifdef GUARD_TESTING
    const char *value = getenv("JOOAN_GUARD_TEST_MQTT_PORT");
    char *end = NULL;
    unsigned long port;

    if (value != NULL && value[0] != '\0') {
        port = strtoul(value, &end, 10);
        if (end != value && *end == '\0' && port > 0 && port <= 65535UL)
            return (uint16_t)port;
    }
#endif
    return JOOAN_GUARD_LOCAL_MQTT_PORT;
}

static const char *configured_rtsp_password_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_RTSP_PASSWORD_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_RTSP_PASSWORD_PATH;
}

static const char *configured_rtsp_sync_path(void)
{
#ifdef GUARD_TESTING
    const char *path = getenv("JOOAN_GUARD_TEST_RTSP_SYNC_PATH");
    if (path != NULL && path[0] != '\0')
        return path;
#endif
    return JOOAN_GUARD_RTSP_SYNC_PATH;
}

static uint16_t configured_rtsp_public_port(void)
{
#ifdef GUARD_TESTING
    const char *value = getenv("JOOAN_GUARD_TEST_RTSP_PORT");
    char *end = NULL;
    unsigned long port;

    if (value != NULL && value[0] != '\0') {
        port = strtoul(value, &end, 10);
        if (end != value && *end == '\0' && port > 0 && port <= 65535UL)
            return (uint16_t)port;
    }
#endif
    return JOOAN_GUARD_RTSP_PUBLIC_PORT;
}

static uint16_t configured_rtsp_upstream_port(void)
{
#ifdef GUARD_TESTING
    const char *value = getenv("JOOAN_GUARD_TEST_RTSP_UPSTREAM_PORT");
    char *end = NULL;
    unsigned long port;

    if (value != NULL && value[0] != '\0') {
        port = strtoul(value, &end, 10);
        if (end != value && *end == '\0' && port > 0 && port <= 65535UL)
            return (uint16_t)port;
    }
#endif
    return JOOAN_GUARD_RTSP_UPSTREAM_PORT;
}

static void disable_dsp_if_current(int descriptor)
{
    if (descriptor >= 0 && descriptor < GUARD_TRACKED_FDS)
        (void)__atomic_fetch_and(&fd_roles[descriptor],
                                 (unsigned char)~FD_ROLE_DSP_WRITE,
                                 __ATOMIC_ACQ_REL);
    if (dsp_descriptor == descriptor)
        dsp_descriptor = find_role_fd(FD_ROLE_DSP_WRITE, descriptor);
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

static int submit_pcm_locked(int descriptor, const unsigned char *data,
                             size_t length)
{
    struct jooan_guard_ao_play_request request;
    size_t offset = 0;
    size_t chunk;
    uint32_t consumed;
    int result;

    if (next_ioctl == NULL)
        return -1;
    while (offset < length) {
        chunk = length - offset;
        if (chunk > UINT32_MAX)
            chunk = UINT32_MAX;
        memset(&request, 0, sizeof(request));
        request.pcm = (uintptr_t)(data + offset);
        request.byte_length = (uint32_t)chunk;
#ifdef GUARD_TESTING
        if (getenv("JOOAN_GUARD_TEST_FAKE_AO_IOCTL") != NULL) {
            ssize_t amount = next_write(descriptor, data + offset, chunk);
            result = amount == (ssize_t)chunk ? 0 : -1;
            request.byte_length = amount > 0 ? (uint32_t)amount : 0;
        } else
#endif
            result = next_ioctl(descriptor, JOOAN_GUARD_AO_PLAY_IOCTL,
                                &request);
        if (result < 0 && errno == EINTR)
            continue;
        consumed = request.byte_length;
        if (result != 0 || consumed == 0 || consumed > chunk) {
            disable_dsp_if_current(descriptor);
            return -1;
        }
        offset += consumed;
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
    if (!guard_applies() || (fd_role_load(descriptor) & FD_ROLE_DSP_READ) == 0 ||
        generation != __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE))
        return 0;
    if (pthread_mutex_trylock(&mic_lock) != 0) {
        __atomic_store_n(&mic_gap, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if ((fd_role_load(descriptor) & FD_ROLE_DSP_READ) == 0 ||
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
        (fd_role_load(descriptor) & FD_ROLE_DSP_READ) == 0)
        return 0;
    observed = __atomic_load_n(&mic_generation, __ATOMIC_ACQUIRE);
    if (pthread_mutex_trylock(&mic_read_lock) != 0) {
        __atomic_store_n(&mic_gap, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if ((fd_role_load(descriptor) & FD_ROLE_DSP_READ) == 0 ||
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

#ifdef GUARD_TESTING
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
#endif

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
    uint64_t session_started = 0;
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
        if (active_session != 0 &&
            (now >= lease_deadline ||
             (now >= session_started &&
              now - session_started >= TALKBACK_MAX_MS))) {
            active_session = 0;
            session_started = 0;
            last_sequence = 0;
            lease_deadline = 0;
            (void)speaker_set_enabled(0);
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
        if (active_session != 0 &&
            (now >= lease_deadline ||
             (now >= session_started &&
              now - session_started >= TALKBACK_MAX_MS))) {
            active_session = 0;
            session_started = 0;
            last_sequence = 0;
            lease_deadline = 0;
            (void)speaker_set_enabled(0);
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
            session_started = 0;
            last_sequence = 0;
            lease_deadline = 0;
            (void)speaker_set_enabled(0);
            continue;
        }
        if (frame.type == JOOAN_AUDIO_PTT_ACQUIRE) {
            if (frame.sequence == 1) {
                if (active_session != 0 &&
                    active_session != frame.session_id)
                    (void)speaker_set_enabled(0);
                active_session = frame.session_id;
                session_started = now;
                last_sequence = frame.sequence;
                lease_deadline = now + TALKBACK_LEASE_MS;
            }
            continue;
        }
        if (active_session == 0 || frame.session_id != active_session ||
            last_sequence == UINT32_MAX || frame.sequence != last_sequence + 1U) {
            active_session = 0;
            session_started = 0;
            last_sequence = 0;
            lease_deadline = 0;
            (void)speaker_set_enabled(0);
            continue;
        }
        last_sequence = frame.sequence;
        if (frame.type == JOOAN_AUDIO_PTT_RELEASE) {
            active_session = 0;
            session_started = 0;
            last_sequence = 0;
            lease_deadline = 0;
            (void)speaker_set_enabled(0);
            continue;
        }
        if (frame.type == JOOAN_AUDIO_GUARD_SPEAKER_LEASE) {
            if (__atomic_load_n(&speaker_enabled, __ATOMIC_ACQUIRE) ||
                speaker_set_enabled(1) == 0) {
                lease_deadline = now + TALKBACK_LEASE_MS;
            } else {
                active_session = 0;
                session_started = 0;
                last_sequence = 0;
                lease_deadline = 0;
                (void)speaker_set_enabled(0);
            }
            continue;
        }
        if (frame.type != JOOAN_AUDIO_PTT_PCMA)
            continue;
        for (index = 0; index < frame.payload_length; ++index) {
            sample = jooan_guard_alaw_to_pcm16(frame.payload[index]);
            pcm[index * 2] = (unsigned char)((uint16_t)sample & 0xffU);
            pcm[index * 2 + 1] = (unsigned char)((uint16_t)sample >> 8);
        }

        pthread_mutex_lock(&dsp_lock);
        descriptor = dsp_descriptor;
        if (descriptor >= 0 &&
            submit_pcm_locked(descriptor, pcm,
                              (size_t)frame.payload_length * 2U) == 0 &&
            (__atomic_load_n(&speaker_enabled, __ATOMIC_ACQUIRE) ||
             speaker_set_enabled(1) == 0)) {
            lease_deadline = now + TALKBACK_LEASE_MS;
        } else {
            (void)speaker_set_enabled(0);
        }
        pthread_mutex_unlock(&dsp_lock);
    }
    (void)speaker_set_enabled(0);
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
    if (!guard_active && executable_is_named_jooanipc())
        _exit(126);
    if (guard_active && next_close != NULL && next_write != NULL &&
        next_ioctl != NULL) {
        guard_owner_pid = current_process_id();
        if (speaker_set_enabled(0) == 0 && speaker_publish_ready() == 0 &&
            speaker_wait_hook_release() == 0)
            start_talkback();
#ifdef GUARD_TESTING
        if (next_read != NULL && next_readv != NULL && next_sendto != NULL)
            start_microphone_tap();
#endif
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
    (void)speaker_set_enabled(0);
    (void)unlink(configured_speaker_ready_path());
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

int bind(int descriptor, const struct sockaddr *address, socklen_t length)
{
    struct sockaddr_in address4;
    struct sockaddr_in6 address6;
    socklen_t option_length;
    int socket_type;
    uint16_t port,upstream_port;

    if (next_bind == NULL)
        resolve_symbols();
    if (next_bind == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!guard_applies() || address == NULL)
        return next_bind(descriptor, address, length);
    if (length < (socklen_t)sizeof(address->sa_family))
        return deny_network();
    if (address->sa_family == AF_INET) {
        if (length < (socklen_t)sizeof(address4))
            return deny_network();
        memcpy(&address4, address, sizeof(address4));
        port = (uint16_t)((uint16_t)
            ((const unsigned char *)&address4.sin_port)[0] << 8);
        port = (uint16_t)(port |
            ((const unsigned char *)&address4.sin_port)[1]);
        option_length = sizeof(socket_type);
        if (next_getsockopt != NULL &&
            next_getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type,
                            &option_length) == 0 &&
            port == configured_rtsp_public_port() && socket_type == SOCK_STREAM) {
            address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            upstream_port=configured_rtsp_upstream_port();
            ((unsigned char *)&address4.sin_port)[0]=(unsigned char)(upstream_port>>8);
            ((unsigned char *)&address4.sin_port)[1]=(unsigned char)(upstream_port&0xff);
            return next_bind(descriptor, (const struct sockaddr *)&address4,
                             sizeof(address4));
        }
        address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return next_bind(descriptor, (const struct sockaddr *)&address4,
                         sizeof(address4));
    }
    if (address->sa_family == AF_INET6) {
        if (length < (socklen_t)sizeof(address6))
            return deny_network();
        memcpy(&address6, address, sizeof(address6));
        port = (uint16_t)((uint16_t)
            ((const unsigned char *)&address6.sin6_port)[0] << 8);
        port = (uint16_t)(port |
            ((const unsigned char *)&address6.sin6_port)[1]);
        option_length = sizeof(socket_type);
        if (next_getsockopt != NULL &&
            next_getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type,
                            &option_length) == 0 &&
            port == configured_rtsp_public_port() && socket_type == SOCK_STREAM) {
            address6.sin6_addr = in6addr_loopback;
            upstream_port=configured_rtsp_upstream_port();
            ((unsigned char *)&address6.sin6_port)[0]=(unsigned char)(upstream_port>>8);
            ((unsigned char *)&address6.sin6_port)[1]=(unsigned char)(upstream_port&0xff);
            return next_bind(descriptor, (const struct sockaddr *)&address6,
                             sizeof(address6));
        }
        address6.sin6_addr = in6addr_loopback;
        return next_bind(descriptor, (const struct sockaddr *)&address6,
                         sizeof(address6));
    }
    if (address->sa_family != AF_UNIX
#ifdef AF_NETLINK
        && address->sa_family != AF_NETLINK
#endif
#ifdef AF_ALG
        && address->sa_family != AF_ALG
#endif
#ifdef AF_BLUETOOTH
        && address->sa_family != AF_BLUETOOTH
#endif
       )
        return deny_network();
    return next_bind(descriptor, address, length);
}

static int rtsp_credentials_synchronized(void)
{
    char digest[65];
    char marker[66];
    ssize_t length;
    int descriptor;

    if (jooan_guard_sha256_file_hex(configured_rtsp_password_path(), digest))
        return 0;
    if (next_open == NULL || next_read == NULL || next_close == NULL)
        return 0;
    descriptor = next_open(configured_rtsp_sync_path(), O_RDONLY);
    if (descriptor < 0)
        return 0;
    length = next_read(descriptor, marker, sizeof(marker));
    (void)next_close(descriptor);
    if (length != 64 && length != 65)
        return 0;
    if (length == 65 && marker[64] != '\n')
        return 0;
    return memcmp(marker, digest, 64) == 0;
}

static int accepted_rtsp_must_close(int listener,
                                    const struct sockaddr *peer,
                                    socklen_t peer_length)
{
    struct sockaddr_storage local;
    socklen_t local_length = sizeof(local);
    uint16_t port;

    if (!guard_applies() || next_getsockname == NULL ||
        next_getsockname(listener, (struct sockaddr *)&local,
                         &local_length) != 0)
        return 0;
    if (local.ss_family == AF_INET &&
        local_length >= (socklen_t)sizeof(struct sockaddr_in)) {
        port = (uint16_t)((uint16_t)((const unsigned char *)&
            ((const struct sockaddr_in *)&local)->sin_port)[0] << 8);
        port = (uint16_t)(port | ((const unsigned char *)&
            ((const struct sockaddr_in *)&local)->sin_port)[1]);
    } else if (local.ss_family == AF_INET6 &&
               local_length >= (socklen_t)sizeof(struct sockaddr_in6)) {
        port = (uint16_t)((uint16_t)((const unsigned char *)&
            ((const struct sockaddr_in6 *)&local)->sin6_port)[0] << 8);
        port = (uint16_t)(port | ((const unsigned char *)&
            ((const struct sockaddr_in6 *)&local)->sin6_port)[1]);
    } else {
        return 0;
    }
    if (port != configured_rtsp_upstream_port())
        return 0;
#ifdef GUARD_TESTING
    if (getenv("JOOAN_GUARD_TEST_RTSP_GATE_ALL") == NULL &&
        jooan_guard_sockaddr_is_loopback(peer, peer_length))
        return 0;
#else
    if (jooan_guard_sockaddr_is_loopback(peer, peer_length))
        return 0;
#endif
    return !rtsp_credentials_synchronized();
}

int accept(int descriptor, struct sockaddr *address, socklen_t *address_length)
{
    struct sockaddr_storage peer;
    struct sockaddr *target;
    socklen_t peer_length;
    socklen_t *target_length;
    int accepted;

    if (next_accept == NULL)
        resolve_symbols();
    if (next_accept == NULL) {
        errno = ENOSYS;
        return -1;
    }
    for (;;) {
        peer_length = sizeof(peer);
        target = address != NULL && address_length != NULL ?
                 address : (struct sockaddr *)&peer;
        target_length = address != NULL && address_length != NULL ?
                        address_length : &peer_length;
        accepted = next_accept(descriptor, target, target_length);
        if (accepted < 0 || !accepted_rtsp_must_close(
                descriptor, target, *target_length))
            return accepted;
        if (next_close != NULL)
            (void)next_close(accepted);
    }
}

int accept4(int descriptor, struct sockaddr *address,
            socklen_t *address_length, int flags)
{
    struct sockaddr_storage peer;
    struct sockaddr *target;
    socklen_t peer_length;
    socklen_t *target_length;
    int accepted;

    if (next_accept4 == NULL)
        resolve_symbols();
    if (next_accept4 == NULL) {
        errno = ENOSYS;
        return -1;
    }
    for (;;) {
        peer_length = sizeof(peer);
        target = address != NULL && address_length != NULL ?
                 address : (struct sockaddr *)&peer;
        target_length = address != NULL && address_length != NULL ?
                        address_length : &peer_length;
        accepted = next_accept4(descriptor, target, target_length, flags);
        if (accepted < 0 || !accepted_rtsp_must_close(
                descriptor, target, *target_length))
            return accepted;
        if (next_close != NULL)
            (void)next_close(accepted);
    }
}

static int connected_socket_is_allowed(int descriptor);

int connect(int descriptor, const struct sockaddr *address, socklen_t length)
{
    struct sockaddr_in redirected;
    uint16_t connectivity_port;

    if (next_connect == NULL)
        resolve_symbols();
    if (next_connect == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (guard_applies() && address != NULL &&
        length >= (socklen_t)sizeof(redirected) &&
        address->sa_family == AF_INET &&
        !jooan_guard_sockaddr_is_loopback(address,length) &&
        ((const unsigned char *)&((const struct sockaddr_in *)address)->sin_port)[0] == 1 &&
        ((const unsigned char *)&((const struct sockaddr_in *)address)->sin_port)[1] == 0xbb) {
        memcpy(&redirected,address,sizeof(redirected));
        inet_pton(AF_INET,JOOAN_GUARD_CONNECTIVITY_LOOPBACK4,
                  &redirected.sin_addr);
        connectivity_port=JOOAN_GUARD_CONNECTIVITY_PORT;
        ((unsigned char *)&redirected.sin_port)[0]=(unsigned char)(connectivity_port>>8);
        ((unsigned char *)&redirected.sin_port)[1]=(unsigned char)(connectivity_port&0xff);
        return next_connect(descriptor,(const struct sockaddr *)&redirected,
                            sizeof(redirected));
    }
    if (guard_applies() && !jooan_guard_sockaddr_is_loopback(address, length))
        return deny_network();
    if (guard_applies() && address != NULL &&
        length >= (socklen_t)sizeof(redirected) &&
        address->sa_family == AF_INET) {
        memcpy(&redirected, address, sizeof(redirected));
        if (ntohl(redirected.sin_addr.s_addr) == 0x7f000002UL) {
            uint16_t mqtt_port = configured_local_mqtt_port();
            ((unsigned char *)&redirected.sin_port)[0] =
                (unsigned char)(mqtt_port >> 8);
            ((unsigned char *)&redirected.sin_port)[1] =
                (unsigned char)(mqtt_port & 0xff);
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
    return jooan_guard_listener_reply_port_allowed(port);
}

static int descriptor_has_disallowed_peer(int descriptor)
{
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);

    if (next_getpeername == NULL ||
        next_getpeername(descriptor, (struct sockaddr *)&peer,
                         &peer_length) != 0)
        return 0;
    return !connected_socket_is_allowed(descriptor);
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

int guard_export_sendmmsg(int descriptor, struct guard_mmsghdr *messages,
                          unsigned int count, int flags) __asm__("sendmmsg");

int guard_export_sendmmsg(int descriptor, struct guard_mmsghdr *messages,
                          unsigned int count, int flags)
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
    if (next_sendmmsg != NULL)
        return next_sendmmsg(descriptor, messages, count, flags);
#ifdef SYS_sendmmsg
    return (int)syscall(SYS_sendmmsg, descriptor, messages, count, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}

long guard_export_syscall(long number, long argument1, long argument2,
                          long argument3, long argument4, long argument5,
                          long argument6) __asm__("syscall");

long guard_export_syscall(long number, long argument1, long argument2,
                          long argument3, long argument4, long argument5,
                          long argument6)
{
    unsigned int index;
    struct guard_mmsghdr *messages;

    if (next_syscall == NULL)
        resolve_symbols();
    if (next_syscall == NULL) {
        errno = ENOSYS;
        return -1;
    }
#ifdef SYS_sendmmsg
    if (number == SYS_sendmmsg && guard_applies()) {
        messages = (struct guard_mmsghdr *)(uintptr_t)argument2;
        if (messages == NULL || argument3 < 0 || argument3 > 1024)
            return deny_network();
        for (index = 0; index < (unsigned long)argument3; ++index) {
            if (!message_destination_is_loopback((int)argument1,
                                                 &messages[index].msg_hdr))
                return deny_network();
        }
    }
#endif
    return next_syscall(number, argument1, argument2, argument3, argument4,
                        argument5, argument6);
}

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
        if ((flags & O_ACCMODE) == O_WRONLY)
            track_fresh_fd(descriptor, FD_ROLE_DSP_WRITE);
#ifdef GUARD_TESTING
        else if ((flags & O_ACCMODE) == O_RDONLY)
            track_fresh_fd(descriptor, FD_ROLE_DSP_READ);
#endif
    } else if (guard_applies() && descriptor >= 0 &&
               (flags & O_ACCMODE) != O_RDONLY &&
               strcmp(path, configured_speaker_direction_path()) == 0) {
        track_fresh_fd(descriptor, FD_ROLE_SPEAKER_DIRECTION);
    } else if (guard_applies() && descriptor >= 0 &&
               (flags & O_ACCMODE) != O_RDONLY &&
               strcmp(path, configured_speaker_value_path()) == 0) {
        track_fresh_fd(descriptor, FD_ROLE_SPEAKER_VALUE);
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

int dup(int descriptor)
{
    unsigned int roles;
    int result;

    if (next_dup == NULL)
        resolve_symbols();
    if (next_dup == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!guard_applies())
        return next_dup(descriptor);
    pthread_mutex_lock(&mic_read_lock);
    pthread_mutex_lock(&dsp_lock);
    pthread_mutex_lock(&mic_lock);
    roles = fd_role_load(descriptor);
    result = next_dup(descriptor);
    if (result >= 0)
        assign_duplicated_fd_locked(result, roles, fd_role_load(result));
    pthread_mutex_unlock(&mic_lock);
    pthread_mutex_unlock(&dsp_lock);
    pthread_mutex_unlock(&mic_read_lock);
    return result;
}

static int duplicate_to_tracked_fd(int descriptor, int destination,
                                   int flags, int use_dup3)
{
    unsigned int source_roles;
    unsigned int previous_roles;
    int result;

    if (descriptor == destination && !use_dup3)
        return next_dup2(descriptor, destination);
    pthread_mutex_lock(&mic_read_lock);
    pthread_mutex_lock(&dsp_lock);
    pthread_mutex_lock(&mic_lock);
    source_roles = fd_role_load(descriptor);
    previous_roles = fd_role_load(destination);
    if (use_dup3) {
        if (next_dup3 != NULL)
            result = next_dup3(descriptor, destination, flags);
#ifdef SYS_dup3
        else if (next_syscall != NULL)
            result = (int)next_syscall(SYS_dup3, descriptor, destination,
                                       flags, 0, 0, 0);
#endif
        else {
            errno = ENOSYS;
            result = -1;
        }
    } else {
        result = next_dup2(descriptor, destination);
    }
    if (result >= 0)
        assign_duplicated_fd_locked(destination, source_roles, previous_roles);
    pthread_mutex_unlock(&mic_lock);
    pthread_mutex_unlock(&dsp_lock);
    pthread_mutex_unlock(&mic_read_lock);
    return result;
}

int dup2(int descriptor, int destination)
{
    int result;

    if (next_dup2 == NULL)
        resolve_symbols();
    if (next_dup2 == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (guard_applies())
        return duplicate_to_tracked_fd(descriptor, destination, 0, 0);
    result = next_dup2(descriptor, destination);
    return result;
}

int guard_export_dup3(int descriptor, int destination, int flags)
    __asm__("dup3");

int guard_export_dup3(int descriptor, int destination, int flags)
{
    int result;

    if (next_dup3 == NULL)
        resolve_symbols();
    if (guard_applies())
        return duplicate_to_tracked_fd(descriptor, destination, flags, 1);
    if (next_dup3 != NULL)
        result = next_dup3(descriptor, destination, flags);
#ifdef SYS_dup3
    else if (next_syscall != NULL)
        result = (int)next_syscall(SYS_dup3, descriptor, destination,
                                   flags, 0, 0, 0);
#endif
    else {
        errno = ENOSYS;
        result = -1;
    }
    return result;
}

static int fcntl_command_has_argument(int command)
{
    switch (command) {
    case F_GETFD:
    case F_GETFL:
#ifdef F_GETOWN
    case F_GETOWN:
#endif
#ifdef F_GETSIG
    case F_GETSIG:
#endif
#ifdef F_GETLEASE
    case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
        return 0;
    default:
        return 1;
    }
}

int fcntl(int descriptor, int command, ...)
{
    va_list arguments;
    long argument = 0;
    unsigned int roles;
    int duplicate_command;
    int result;

    if (next_fcntl == NULL)
        resolve_symbols();
    if (next_fcntl == NULL) {
        errno = ENOSYS;
        return -1;
    }
    duplicate_command = command == F_DUPFD;
#ifdef F_DUPFD_CLOEXEC
    duplicate_command = duplicate_command || command == F_DUPFD_CLOEXEC;
#endif
    if (fcntl_command_has_argument(command)) {
        va_start(arguments, command);
        argument = va_arg(arguments, long);
        va_end(arguments);
    }
    if (guard_applies() && duplicate_command) {
        pthread_mutex_lock(&mic_read_lock);
        pthread_mutex_lock(&dsp_lock);
        pthread_mutex_lock(&mic_lock);
        roles = fd_role_load(descriptor);
        result = next_fcntl(descriptor, command, argument);
        if (result >= 0)
            assign_duplicated_fd_locked(result, roles, fd_role_load(result));
        pthread_mutex_unlock(&mic_lock);
        pthread_mutex_unlock(&dsp_lock);
        pthread_mutex_unlock(&mic_read_lock);
        return result;
    }
    if (fcntl_command_has_argument(command))
        result = next_fcntl(descriptor, command, argument);
    else
        result = next_fcntl(descriptor, command);
    return result;
}

ssize_t write(int descriptor, const void *buffer, size_t length)
{
    unsigned int roles;
    ssize_t result;

    if (next_write == NULL)
        resolve_symbols();
    if (next_write == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!guard_applies())
        return next_write(descriptor, buffer, length);

    roles = fd_role_load(descriptor);
    if ((roles & (FD_ROLE_SPEAKER_DIRECTION | FD_ROLE_SPEAKER_VALUE)) != 0)
        return speaker_confined_write(descriptor, roles, length);

    if ((roles & FD_ROLE_DSP_WRITE) == 0 &&
        descriptor_has_disallowed_peer(descriptor))
        return (ssize_t)deny_network();

    pthread_mutex_lock(&dsp_lock);
    if ((fd_role_load(descriptor) & FD_ROLE_DSP_WRITE) != 0) {
        /* OEM speaker playback includes the motion-detection siren.  Pretend
         * it was consumed; authenticated talkback uses next_write directly
         * from talkback_main and is the only allowed playback producer. */
        if (length > (size_t)SSIZE_MAX) {
            errno = EINVAL;
            result = -1;
        } else {
            result = (ssize_t)length;
        }
    } else {
        pthread_mutex_unlock(&dsp_lock);
        return next_write(descriptor, buffer, length);
    }
    pthread_mutex_unlock(&dsp_lock);
    return result;
}

ssize_t writev(int descriptor, const struct iovec *vectors, int vector_count)
{
    unsigned int roles;
    size_t length = 0;
    int index;
    ssize_t result;

    if (next_writev == NULL)
        resolve_symbols();
    if (next_writev == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!guard_applies())
        return next_writev(descriptor, vectors, vector_count);
    roles = fd_role_load(descriptor);
    if ((roles & (FD_ROLE_SPEAKER_DIRECTION | FD_ROLE_SPEAKER_VALUE)) != 0) {
        if (vector_count < 0 || (vector_count != 0 && vectors == NULL)) {
            errno = EINVAL;
            return -1;
        }
        for (index = 0; index < vector_count; ++index) {
            if (vectors[index].iov_len > (size_t)SSIZE_MAX - length) {
                errno = EINVAL;
                return -1;
            }
            length += vectors[index].iov_len;
        }
        return speaker_confined_write(descriptor, roles, length);
    }
    if ((roles & FD_ROLE_DSP_WRITE) == 0 &&
        descriptor_has_disallowed_peer(descriptor))
        return (ssize_t)deny_network();

    pthread_mutex_lock(&dsp_lock);
    if ((fd_role_load(descriptor) & FD_ROLE_DSP_WRITE) != 0) {
#ifdef GUARD_TESTING
        if (getenv("JOOAN_GUARD_TEST_DSP_WRITEV_PAUSE") != NULL)
            usleep(100000);
#endif
        if (vector_count < 0 || (vector_count != 0 && vectors == NULL)) {
            errno = EINVAL;
            result = -1;
        } else {
            length = 0;
            for (index = 0; index < vector_count; ++index) {
                if (vectors[index].iov_len > (size_t)SSIZE_MAX - length) {
                    errno = EINVAL;
                    result = -1;
                    break;
                }
                length += vectors[index].iov_len;
            }
            if (index == vector_count)
                result = (ssize_t)length;
        }
    } else {
        pthread_mutex_unlock(&dsp_lock);
        return next_writev(descriptor, vectors, vector_count);
    }
    pthread_mutex_unlock(&dsp_lock);
    return result;
}

ssize_t guard_export_sendfile64(int output, int input, off64_t *offset,
                                size_t count) __asm__("sendfile64");

ssize_t guard_export_sendfile64(int output, int input, off64_t *offset,
                                size_t count)
{
    if (next_sendfile64 == NULL)
        resolve_symbols();
    if (next_sendfile64 == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (guard_applies() && descriptor_has_disallowed_peer(output))
        return (ssize_t)deny_network();
    return next_sendfile64(output, input, offset, count);
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
#ifdef GUARD_TESTING
        if (getenv("JOOAN_GUARD_TEST_MIC_PAUSE") != NULL)
            usleep(100000);
#endif
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
int guard_export_ioctl(int descriptor, unsigned long request, void *argument)
    __asm__("ioctl");

int guard_export_ioctl(int descriptor, unsigned long request, void *argument)
{
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
    if (!guard_applies())
        return next_ioctl(descriptor, request, argument);

    if ((fd_role_load(descriptor) & FD_ROLE_DSP_READ) != 0 &&
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
    if ((fd_role_load(descriptor) & FD_ROLE_DSP_WRITE) != 0) {
        /* Exact OEM AO payload submission. Dropping it prevents the motion
         * alarm from being queued while muted and leaking into a later PTT
         * window. Configuration ioctls still reach the driver. */
        if (request == JOOAN_GUARD_AO_PLAY_IOCTL)
            result = 0;
        else
            result = next_ioctl(descriptor, request, argument);
    }
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
        (void)untrack_fd(descriptor);
    }
    if (next_close == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return next_close(descriptor);
}
