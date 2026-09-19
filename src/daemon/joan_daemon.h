#ifndef JOAN_DAEMON_H
#define JOAN_DAEMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JOAN_VERSION "0.1.0"
#define JOAN_MAX_BODY (8u * 1024u * 1024u)
#define JOAN_MAX_HEADERS 16384u
#define JOAN_TOKEN_BYTES 32u
#define JOAN_TOKEN_HEX (JOAN_TOKEN_BYTES * 2u)

typedef struct {
    char state_dir[256];
    char staging_dir[256];
    char web_dir[256];
    char bind_addr[64];
    unsigned port;
    unsigned redirect_port;
    int plain_http;
    char public_host[128];
    char integration_helper[256];
    char audio_socket[256];
    unsigned mqtt_port;
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
int joan_auth_login(const JoanConfig *cfg, const char *remote,
                    const char *user, const char *password, JoanAuthz *out);
int joan_auth_request(const JoanRequest *req, int require_csrf, JoanAuthz *out);
int joan_auth_change_password(const JoanConfig *cfg, const JoanAuthz *auth,
                              const char *old_password, const char *new_password);
void joan_auth_logout(const JoanAuthz *auth);

int joan_run_helper(const JoanConfig *cfg, const char *operation,
                    const char *argument_path, const char *id,
                    unsigned char **output, size_t *output_len, unsigned timeout_sec);
int joan_stage_blob(const JoanConfig *cfg, const char *category,
                    const void *data, size_t len, char id[65], char path[512]);

int joan_mqtt_bridge_start(const JoanConfig *cfg);
void joan_mqtt_bridge_stop(void);
int joan_mqtt_bridge_publish(const char *topic, const void *payload, size_t len);
const char *joan_mqtt_bridge_status(void);

int joan_tls_ensure_identity(const JoanConfig *cfg);
int joan_server_run(const JoanConfig *cfg);

#endif
