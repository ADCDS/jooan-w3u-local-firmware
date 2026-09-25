#ifndef JOAN_DAEMON_H
#define JOAN_DAEMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JOAN_VERSION "0.2.9"
#define JOAN_MAX_BODY (16u * 1024u)
#define JOAN_MAX_UPDATE_BODY 2097344u
#define JOAN_MAX_HEADERS 16384u
#define JOAN_TOKEN_BYTES 32u
#define JOAN_TOKEN_HEX (JOAN_TOKEN_BYTES * 2u)

typedef struct {
    char state_dir[256];
    char staging_dir[256];
    char release_sequence_path[256];
    char web_dir[256];
    char bind_addr[64];
    unsigned port;
    unsigned redirect_port;
    int plain_http;
    char public_host[128];
    char integration_helper[256];
    char audio_socket[256];
    char sd_dir[256];
    unsigned mqtt_port;
    unsigned rtsp_port;
    unsigned rtsp_proxy_port;
    int mdns_enabled;
    unsigned mdns_port;
} JoanConfig;

typedef struct {
    int fd;
    char method[12];
    char path[512];
    char query[512];
    char remote[64];
    char cookie[1024];
    char csrf[128];
    char content_type[128];
    char upgrade[64];
    char ws_key[128];
    char ws_protocol[128];
    char origin[256];
    char host[256];
    char transfer_encoding[64];
    char range[64];
    size_t content_length;
    unsigned char *body;
} JoanRequest;

typedef struct {
    int authenticated;
    int must_change;
    char token[JOAN_TOKEN_HEX + 1];
    char csrf[JOAN_TOKEN_HEX + 1];
} JoanAuthz;

int joan_mkdir_p(const char *path, mode_t mode);
int joan_write_atomic(const char *path, const void *data, size_t len, mode_t mode);
int joan_read_file(const char *path, unsigned char **out, size_t *len, size_t max);
int joan_random(void *out, size_t len);
void joan_hex(const unsigned char *in, size_t len, char *out);
int joan_unhex(const char *in, unsigned char *out, size_t len);
int joan_ct_equal(const void *a, const void *b, size_t n);
void joan_sha256(const void *data, size_t len, unsigned char out[32]);
void joan_pbkdf2_sha256(const void *password, size_t password_len,
                        const unsigned char *salt, size_t salt_len,
                        uint32_t iterations, unsigned char *out, size_t out_len);

int joan_auth_init(const JoanConfig *cfg);
int joan_auth_setup_required(const JoanConfig *cfg);
int joan_auth_ssh_synchronized(const JoanConfig *cfg);
int joan_auth_rtsp_synchronized(const JoanConfig *cfg);
int joan_auth_login(const JoanConfig *cfg, const char *remote,
                    const char *user, const char *password, JoanAuthz *out);
int joan_auth_request(const JoanRequest *req, int require_csrf, JoanAuthz *out);
int joan_auth_change_password(const JoanConfig *cfg, const JoanAuthz *auth,
                              const char *old_password, const char *new_password);
void joan_auth_logout(const JoanAuthz *auth);
void joan_auth_shift(time_t delta);

int joan_run_helper(const JoanConfig *cfg, const char *operation,
                    const char *argument_path, const char *id,
                    unsigned char **output, size_t *output_len, unsigned timeout_sec);
int joan_stage_blob(const JoanConfig *cfg, const char *category,
                    const void *data, size_t len, char id[65], char path[512]);

int joan_mqtt_bridge_start(const JoanConfig *cfg);
void joan_mqtt_bridge_stop(void);
int joan_mqtt_bridge_publish(const char *topic, const void *payload, size_t len);
int joan_mqtt_request(unsigned command, const char *payload, char operation[65]);
int joan_mqtt_operation(const char *operation, char *response, size_t capacity);
const char *joan_mqtt_bridge_status(void);

int joan_mdns_start(const JoanConfig *cfg);
void joan_mdns_stop(void);
int joan_mdns_get_hostname(const JoanConfig *cfg, char out[64]);
int joan_mdns_get_configured_hostname(const JoanConfig *cfg, char out[64]);
int joan_mdns_set_hostname(const JoanConfig *cfg, const char *hostname);
const char *joan_mdns_status(void);

int joan_tls_ensure_identity(const JoanConfig *cfg);
int joan_tls_enroll_identity(const JoanConfig *cfg, const char *pem, size_t len,
                             char *why, size_t why_len);
int joan_tls_clear_identity(const JoanConfig *cfg);
int joan_fmp4_start(const JoanConfig *cfg);
int joan_fmp4_init_segment(const char *stream, unsigned char **data, size_t *len,
                           unsigned timeout_ms);
int joan_fmp4_fragment(const char *stream, uint32_t after,
                       unsigned char **data, size_t *len, uint32_t *first,
                       uint32_t *sequence, unsigned timeout_ms);
const char *joan_fmp4_status(const char *stream);
int joan_rtsp_proxy_start(const JoanConfig *cfg);
void joan_rtsp_proxy_stop(void);
typedef struct JoanRecMp4 JoanRecMp4;
typedef int (*JoanRecSink)(void *ctx, const void *data, size_t len);
JoanRecMp4 *joan_recmp4_open(const char *path);
long joan_recmp4_length(const JoanRecMp4 *m);
int joan_recmp4_write(JoanRecMp4 *m, JoanRecSink sink, void *ctx, long from, long to);
void joan_recmp4_close(JoanRecMp4 *m);

int joan_server_run(const JoanConfig *cfg);
void joan_server_stop(void);

#endif
