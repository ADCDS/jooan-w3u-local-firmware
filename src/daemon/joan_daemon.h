#ifndef JOAN_DAEMON_H
#define JOAN_DAEMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JOAN_VERSION "0.2.22"
#define JOAN_MAX_BODY (16u * 1024u)
#define JOAN_MAX_UPDATE_BODY 2097344u
#define JOAN_MAX_HEADERS 16384u
#define JOAN_TOKEN_BYTES 32u
#define JOAN_TOKEN_HEX (JOAN_TOKEN_BYTES * 2u)

typedef struct {
    char state_dir[256];
    char staging_dir[256];
    char run_dir[256];
    char release_sequence_path[256];
    char web_dir[256];
    char bind_addr[64];
    unsigned port;
    int plain_mqtt;
    char integration_helper[256];
    unsigned mqtt_port;
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
    char origin[256];
    char host[256];
    char transfer_encoding[64];
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
/* -1: missing/expired cookie; -2: valid cookie but missing/wrong CSRF. */
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
int joan_mqtt_request(unsigned command, const char *payload, char operation[65]);
int joan_mqtt_operation(const char *operation, char *response, size_t capacity);
void joan_mqtt_abandon(const char *operation);
const char *joan_mqtt_bridge_status(void);

int joan_mdns_start(const JoanConfig *cfg);
void joan_mdns_stop(void);
int joan_mdns_get_hostname(const JoanConfig *cfg, char out[64]);
int joan_mdns_get_configured_hostname(const JoanConfig *cfg, char out[64]);
int joan_mdns_set_hostname(const JoanConfig *cfg, const char *hostname);
const char *joan_mdns_status(void);

int joan_tls_ensure_identity(const JoanConfig *cfg);

/* SNTP client for the RTC-less board. The configured server is "gateway"
 * (the current default route), an IPv4 address with optional :port, or
 * absent (off). Clock steps go through the privileged time-set helper,
 * which also persists last-time for the next boot. */
int joan_ntp_start(const JoanConfig *cfg);
void joan_ntp_stop(void);
int joan_ntp_configure(const char *server);
void joan_ntp_status(char *out, size_t cap);
/* Pure helpers, exposed for host tests. */
int joan_ntp_parse_server(const char *text, uint32_t *address, uint16_t *port);
int joan_ntp_offset(const unsigned char *reply, size_t size, const unsigned char origin[8],
                    int64_t sent_us, uint64_t round_trip_us, int64_t *offset_us);

int joan_server_run(const JoanConfig *cfg);
void joan_server_stop(void);

#endif
