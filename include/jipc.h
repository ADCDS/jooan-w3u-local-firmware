#ifndef JIPC_H
#define JIPC_H
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#define JIPC_VERSION "0.1.0"
#define JIPC_VIDEO_STREAMS 4
#define JIPC_AUDIO_STREAM 4
#define JIPC_STREAMS 5
#define JIPC_RING_SLOTS 4
#define JIPC_MAX_FRAME (1024u * 1024u)
#define JIPC_MAX_JSON 16384
enum jipc_codec { JIPC_H264 = 1, JIPC_PCMA = 2, JIPC_JPEG = 3 };
typedef struct {
    unsigned char *data;
    size_t size;
    uint64_t pts_us, sequence;
    int keyframe;
    enum jipc_codec codec;
} jipc_frame;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    jipc_frame slots[JIPC_RING_SLOTS];
    uint64_t sequence;
    unsigned char sps[512], pps[512];
    size_t sps_size, pps_size;
    unsigned width, height, fps;
} jipc_stream;
typedef struct {
    char bind[64], username[33], password[129], realm[64];
    char backend[16], replay[JIPC_VIDEO_STREAMS][512], libimp[512];
    char state_dir[512], record_dir[512], config_path[512];
    char tls_cert[512], tls_key[512];
    unsigned http_port, rtsp_port, max_clients, fps, bitrate;
    unsigned width, height, sub_width, sub_height;
    unsigned width1, height1, bitrate1, sub_bitrate, gop_frames;
    unsigned sensors, record_seconds, record_max_mb;
    unsigned flip0, flip1;
    int audio, recording, allow_plain_http;
    int discovery;
    int watchdog;
    int onvif_nudge;
    int main_only;
    unsigned discovery_port;
} jipc_config;
typedef struct jipc_app {
    jipc_config config;
    pthread_mutex_t config_mutex;
    jipc_stream streams[JIPC_STREAMS];
    atomic_int stop;
    atomic_int restart;
    uint64_t started_us;
    pthread_t media_thread, http_thread, rtsp_thread, record_thread;
    int media_started, http_started, rtsp_started, record_started;
    void *backend_state;
    void *http_state, *rtsp_state, *platform_state;
    void *record_state;
    void *discovery_state;
    pthread_t discovery_thread;
    int discovery_started;
    pthread_mutex_t control_mutex;
    atomic_int backend_ready;
    atomic_int control_waiters;
    char backend_error[256];
    uint64_t produced[JIPC_STREAMS];
    int record_active;
    char record_error[256];
} jipc_app;
typedef struct { int pan, tilt, moving, night, white_light; uint32_t preset_mask; char preset_names[16][65]; char error[192]; } jipc_ptz_status;

uint64_t jipc_now_us(void);
void jipc_sleep_ms(unsigned milliseconds);
void jipc_log(const char *level, const char *format, ...);
int jipc_random(void *data, size_t bytes);
int jipc_write_all(int fd, const void *data, size_t bytes);
int jipc_mkdirs(const char *path, unsigned mode);
int jipc_atomic_file(const char *path, const void *data, size_t size, unsigned mode);
int jipc_base64(const void *data, size_t size, char *out, size_t capacity);
int jipc_base64_decode(const char *data, unsigned char *out, size_t capacity);
void jipc_md5(const void *data, size_t size, char hex[33]);
void jipc_sha1(const void *data, size_t size, unsigned char out[20]);
int jipc_sha256_file(const char *path, char hex[65]);
int jipc_equal(const char *left, const char *right);
int jipc_json_string(const char *json, const char *key, char *out, size_t capacity);
int jipc_json_integer(const char *json, const char *key, int *out);
int jipc_json_escape(const char *text, char *out, size_t capacity);

unsigned jipc_video_owner(const jipc_config *c, unsigned channel);
unsigned jipc_video_width(const jipc_config *c, unsigned channel);
unsigned jipc_video_height(const jipc_config *c, unsigned channel);
unsigned jipc_video_bitrate(const jipc_config *c, unsigned channel);
void jipc_config_defaults(jipc_config *config);
int jipc_config_load(jipc_config *config, const char *path, char *error, size_t capacity);
int jipc_config_save(const jipc_config *config);
int jipc_config_validate(const jipc_config *config, char *error, size_t capacity);
int jipc_native_video_validate(const jipc_config *config, char *error, size_t capacity);
void jipc_config_copy(jipc_app *app, jipc_config *out);
int jipc_config_json(jipc_app *app, char *out, size_t capacity);
int jipc_config_patch(jipc_app *app, const char *json, char *error, size_t capacity);

int jipc_streams_init(jipc_app *app);
void jipc_streams_destroy(jipc_app *app);
int jipc_publish(jipc_app *app, unsigned channel, const void *data, size_t bytes,
                 uint64_t pts_us, int keyframe, enum jipc_codec codec);
/* Publish a physical video source, optionally also to its adjacent alias.
 * main_only accepts only even source slots; each logical ring owns its copy. */
int jipc_publish_video(jipc_app *app, unsigned source, int main_only,
                       const void *data, size_t bytes, uint64_t pts_us, int keyframe);
int jipc_read(jipc_app *app, unsigned channel, uint64_t *cursor,
              jipc_frame *frame, unsigned timeout_ms);
void jipc_frame_free(jipc_frame *frame);
int jipc_codec_parameters(jipc_app *app, unsigned channel,
                          unsigned char *sps, size_t *sps_size,
                          unsigned char *pps, size_t *pps_size);

/* SDK backend is loaded at runtime; no vendor library is shipped or linked. */
int jipc_backend_start(jipc_app *app);
void jipc_backend_stop(jipc_app *app);
int jipc_backend_control(jipc_app *app, const char *operation, int value,
                         int value2, char *error, size_t capacity);
int jipc_backend_snapshot(jipc_app *app, unsigned channel, jipc_frame *frame,
                          char *error, size_t capacity);
int jipc_backend_talk(jipc_app *app, const void *pcma, size_t size,
                      char *error, size_t capacity);
int jipc_platform_control(jipc_app *app, const char *operation, int value,
                          int value2, char *error, size_t capacity);
int jipc_platform_nudge(jipc_app *app, int x, int y, char *error, size_t capacity);
int jipc_platform_preset_create(jipc_app *app, int *slot, const char *name, char *error, size_t capacity);
int jipc_platform_preset_set(jipc_app *app, int slot, const char *name, char *error, size_t capacity);
void jipc_platform_stop(jipc_app *app);
int jipc_platform_status(jipc_app *app, jipc_ptz_status *status, char *error, size_t capacity);
int jipc_platform_status_json(jipc_app *app, char *out, size_t capacity);

int jipc_rtsp_start(jipc_app *app);
void jipc_rtsp_stop(jipc_app *app);
int jipc_http_start(jipc_app *app);
void jipc_http_stop(jipc_app *app);
int jipc_discovery_start(jipc_app *app);
void jipc_discovery_stop(jipc_app *app);
int jipc_record_start(jipc_app *app);
void jipc_record_stop(jipc_app *app);
int jipc_record_set(jipc_app *app, int enabled);
int jipc_status_json(jipc_app *app, char *out, size_t capacity);

/* ONVIF handler: returns HTTP status and fills XML. HTTP auth runs first. */
int jipc_onvif(jipc_app *app, const char *path, const char *body,
               const char *host, char *out, size_t capacity);
int jipc_onvif_authorize(jipc_app *app, const char *body);
int jipc_onvif_public_time(const char *body);
#endif
