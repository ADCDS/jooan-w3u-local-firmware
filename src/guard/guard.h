#ifndef JOOAN_GUARD_H
#define JOOAN_GUARD_H

#include <stdint.h>
#include <sys/socket.h>

#define JOOAN_GUARD_SUPPORTED_JOOANIPC_SHA256 \
    "edd1afa9f89f74f60d23fc56a1347f9b406400d38a23aa46ebcc45983fd09355"

#define JOOAN_GUARD_API_HOST  "use1api.jooaniot.com"
#define JOOAN_GUARD_MQTT_HOST "use1mqtt01.jooaniot.com"
#define JOOAN_GUARD_LOOPBACK4 "127.0.0.1"
#define JOOAN_GUARD_MQTT_LOOPBACK4 "127.0.0.2"
#define JOOAN_GUARD_API_LOOPBACK4 "127.0.0.3"
#define JOOAN_GUARD_LOOPBACK6 "::1"
#define JOOAN_GUARD_LOCAL_MQTT_PORT 1883
#define JOOAN_GUARD_DSP_PATH "/dev/dsp"
#define JOOAN_GUARD_TALKBACK_PATH "/tmp/jooan-guard-talkback.sock"
#define JOOAN_GUARD_MIC_STREAM_IOCTL 0x40145062UL

/* Exact OEM AMIC_AI_GET_STREAM userspace request. On the supported 32-bit
 * target this is five 32-bit words. Pointer-sized fields keep host-only tests
 * valid without changing the target layout. */
struct jooan_guard_mic_stream_request {
    uintptr_t pcm;
    uint32_t byte_length;
    uintptr_t reference_pcm;
    uint32_t reference_samples;
    uint32_t timeout;
};
#if UINTPTR_MAX == UINT32_MAX
typedef char jooan_guard_mic_stream_request_must_be_20_bytes[
    sizeof(struct jooan_guard_mic_stream_request) == 20 ? 1 : -1];
#endif

int jooan_guard_hostname_is_approved(const char *name);
int jooan_guard_hostname_is_mqtt(const char *name);
int jooan_guard_hostname_is_api(const char *name);
int jooan_guard_hostname_is_loopback(const char *name);
int jooan_guard_sockaddr_is_loopback(const struct sockaddr *address,
                                     socklen_t address_length);
int16_t jooan_guard_alaw_to_pcm16(uint8_t value);

#endif
