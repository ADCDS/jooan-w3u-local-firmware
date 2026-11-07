#ifndef JIPC_H
#define JIPC_H
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#define JIPC_VERSION "0.1.0"
#define JIPC_VIDEO_STREAMS 4
#define JIPC_AUDIO_STREAM 4
#define JIPC_STREAMS 5
/* Ring depth per stream: video keeps four frames (a slow reader then waits
 * for an IDR); audio keeps 1.28 s of 40 ms frames so jittery readers do not
 * drop sound. */
#define JIPC_RING_MAX 32
#define JIPC_VIDEO_RING 4
#define JIPC_AUDIO_RING 32
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
    jipc_frame slots[JIPC_RING_MAX];
    unsigned capacity;
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
    char ntp_server[64];
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
    int motor_home;
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
    void *ntp_state;
    pthread_t ntp_thread;
    int ntp_started;
    pthread_mutex_t control_mutex;
    atomic_uint credentials_generation; /* counts changes of the stream account */
    atomic_int backend_ready;
    atomic_int control_waiters;
    char backend_error[256];
    uint64_t produced[JIPC_STREAMS];
    int record_active;
    char record_error[256];
    void *speaker_state;
    void *controls_state;
    /* Microphone mute (control mic_mute): the track keeps running, silent. */
    atomic_int mic_muted;
    /* Privacy mode, and "drop every video frame": set from before video starts
     * until the black-out is confirmed, so nothing can leak if it fails. */
    atomic_int privacy_on, privacy_hold;
    atomic_uint privacy_wait_key; /* by source: frames resume at the next keyframe */
    uint64_t video_grace_until; /* jipc_video_healthy */
    /* Level of the microphone audio the NVR receives, over the last full
     * second, in hundredths of a dBFS; audio_level_ms is when it was taken
     * (milliseconds, wrapping; 32 bits because MIPS32 has no 64-bit atomics). */
    atomic_int audio_peak_cdb, audio_rms_cdb;
    atomic_uint audio_level_ms;
    struct { uint64_t start_us, sumsq; uint32_t samples; int32_t peak; } audio_meter;
} jipc_app;
typedef struct { int pan, tilt, moving, night, white_light; double pan_norm, tilt_norm; uint32_t preset_mask; char preset_names[16][65]; char error[192]; } jipc_ptz_status;
typedef struct { int moving, homing, trusted; long manual_age_s; } jipc_ptz_motion;
enum { JIPC_MOVE_ABSOLUTE, JIPC_MOVE_RELATIVE, JIPC_MOVE_RELATIVE_FOV, JIPC_MOVE_STEPS };
/* Half the pan/tilt lens's field of view, in motor steps. */
enum { JIPC_FOV_HALF_PAN = 405, JIPC_FOV_HALF_TILT = 220 };

uint64_t jipc_now_us(void);
/* Failed-login throttle (src/authlimit.c), keyed by peer address. */
#define JIPC_AUTH_FAILURES 10u
#define JIPC_AUTH_WINDOW_US UINT64_C(60000000)
int jipc_auth_blocked(int fd);
void jipc_auth_result(int fd, int ok);
int jipc_auth_blocked_at(const char *peer, uint64_t now);
void jipc_auth_result_at(const char *peer, uint64_t now, int ok);
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
/* The watchdog's question: is every stream getting frames? */
int jipc_video_healthy(jipc_app *app, unsigned streams, uint64_t now);
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
/* Day (0) or night (1) on the ISP and the IR-cut/infrared GPIOs, without
 * recording it as the user's choice (the controls engine's own switch). */
int jipc_backend_night(jipc_app *app, int night, char *error, size_t capacity);
/* Persisted camera controls and their engine (src/controls.c). */
int jipc_controls_start(jipc_app *app);
/* Before video starts: is privacy mode on? Then frames are held until it is applied. */
void jipc_controls_preload(jipc_app *app);
int jipc_controls_home(jipc_app *app, int *x, int *y);
int jipc_controls_set_home(jipc_app *app, int x, int y, char *error, size_t capacity);
void jipc_controls_stop(jipc_app *app);
void jipc_controls_note(jipc_app *app, const char *operation, int value, int value2);
int jipc_controls_patch(jipc_app *app, const char *json, char *error, size_t capacity);
int jipc_controls_trigger(jipc_app *app, int seconds, char *error, size_t capacity);
int jipc_controls_siren(jipc_app *app, int on, int seconds, char *error, size_t capacity);
int jipc_controls_siren_test(jipc_app *app, int sound, char *error, size_t capacity);
int jipc_controls_strobe(jipc_app *app, int seconds, char *error, size_t capacity);
int jipc_controls_json(jipc_app *app, char *out, size_t capacity);
int jipc_controls_picture(jipc_app *app, unsigned lens, int values[4], int *night_mode);
int jipc_backend_control(jipc_app *app, const char *operation, int value,
                         int value2, char *error, size_t capacity);
int jipc_backend_snapshot(jipc_app *app, unsigned channel, jipc_frame *frame,
                          char *error, size_t capacity);
/* Speaker input from HTTP; owner identifies the talker (see jipc_talk_submit). */
int jipc_backend_talk(jipc_app *app, uint64_t owner, const void *pcma, size_t size,
                      char *error, size_t capacity);
/* JSON object describing microphone state ("{...}"), or -1. */
int jipc_backend_audio_json(jipc_app *app, char *out, size_t capacity);

/* G.711 A-law and an audio clock that stamps frames by sample count
 * (src/audio.c). The clock anchors at the first frame's capture time and
 * re-anchors, reporting a discontinuity, when it drifts more than 150 ms. */
unsigned char jipc_alaw_encode(int sample);
int jipc_alaw_decode(unsigned char code);
/* Published microphone audio: talk or a mute replaces it with silence. */
int jipc_mic_silenced(jipc_app *app);
void jipc_audio_meter_add(jipc_app *app, const unsigned char *pcma, size_t bytes, uint64_t now_us);
int jipc_audio_level_json(jipc_app *app, char *out, size_t capacity);
typedef struct { uint64_t anchor_us, samples; unsigned rate; int started; } jipc_audio_clock;
void jipc_audio_clock_init(jipc_audio_clock *clock, unsigned rate);
int jipc_audio_clock_ahead(const jipc_audio_clock *clock, uint64_t now_us, unsigned samples);
uint64_t jipc_audio_clock_stamp(jipc_audio_clock *clock, uint64_t now_us,
                                unsigned samples, int *discontinuity);

/* Speaker service (src/speaker.c): the only caller of the audio output and
 * the only automatic driver of the speaker amplifier. Producers never block. */
#define JIPC_SPEAKER_FRAME 320u          /* samples (and A-law bytes) per write: 40 ms */
typedef struct {
    void *context;
    int (*open)(void *context, int volume, char *error, size_t capacity);
    /* Plays one JIPC_SPEAKER_FRAME of A-law; may block up to ~800 ms. */
    int (*write)(void *context, const unsigned char *pcma);
    void (*close)(void *context);
    int (*volume)(void *context, int volume);
    int (*amplifier)(void *context, int on);
} jipc_speaker_sink;
enum { JIPC_TALK_OK = 0, JIPC_TALK_INVALID = -1, JIPC_TALK_BUSY = -2, JIPC_TALK_UNAVAILABLE = -3 };
int jipc_speaker_start(jipc_app *app, const jipc_speaker_sink *sink);
void jipc_speaker_stop(jipc_app *app);
int jipc_speaker_available(jipc_app *app);
/* RTSP submissions (http=0) drop the oldest audio when the queue is full;
 * HTTP submissions (http=1) are refused with JIPC_TALK_BUSY instead. */
int jipc_talk_submit(jipc_app *app, uint64_t owner, uint32_t ssrc, int http,
                     const unsigned char *pcma, size_t size, char *error, size_t capacity);
void jipc_talk_release(jipc_app *app, uint64_t owner);
/* Nonzero while a talk session plays and for 300 ms after (half-duplex). */
int jipc_talk_active(jipc_app *app);
int jipc_speaker_set_volume(jipc_app *app, int volume, char *error, size_t capacity);
/* The siren (src/siren.c, src/speaker.c): sounds through the speaker service, ahead of any talker. */
enum { JIPC_SIREN_SOUNDS = 4, JIPC_SIREN_MAX_SECONDS = 300, JIPC_SIREN_TEST_SECONDS = 3 };
typedef struct { unsigned sound; uint32_t t; uint16_t phase; } jipc_siren;
const char *jipc_siren_name(unsigned sound);
int jipc_siren_sound(const char *name);
void jipc_siren_init(jipc_siren *siren, unsigned sound);
void jipc_siren_fill(jipc_siren *siren, unsigned char *pcma, size_t samples);
int jipc_siren_start(jipc_app *app, unsigned sound, unsigned seconds, int percent, char *error, size_t capacity);
void jipc_siren_stop(jipc_app *app);
int jipc_siren_active(jipc_app *app);
void jipc_speaker_watch(jipc_app *app);
int jipc_speaker_gate_open(jipc_app *app);
int jipc_speaker_status_json(jipc_app *app, char *out, size_t capacity);
int jipc_platform_control(jipc_app *app, const char *operation, int value,
                          int value2, char *error, size_t capacity);
int jipc_platform_nudge(jipc_app *app, int x, int y, char *error, size_t capacity);
int jipc_platform_preset_create(jipc_app *app, int *slot, const char *name, char *error, size_t capacity);
int jipc_platform_preset_rename(jipc_app *app, int slot, const char *name, char *error, size_t capacity);
int jipc_platform_preset_set(jipc_app *app, int slot, const char *name, char *error, size_t capacity);
/* Boot homing when motor_home=1 (no OEM bootstrap); once per boot. */
int jipc_platform_start(jipc_app *app);
int jipc_platform_move(jipc_app *app, int kind, double x, double y, double speed, char *error, size_t capacity);
/* The timestamp burned into the picture (src/overlay.c). */
enum { JIPC_OVERLAY_YMD, JIPC_OVERLAY_DMY, JIPC_OVERLAY_MDY };
enum { JIPC_OVERLAY_TOP_LEFT, JIPC_OVERLAY_TOP_RIGHT, JIPC_OVERLAY_BOTTOM_LEFT, JIPC_OVERLAY_BOTTOM_RIGHT };
/* 19 for the date and time, a space, 16 for a name; at scale 3, 880 by 30 by 4 bytes. */
enum { JIPC_OVERLAY_NAME = 16, JIPC_OVERLAY_CHARS = 36, JIPC_OVERLAY_BYTES = 880 * 30 * 4 };
size_t jipc_overlay_text(char *out, size_t capacity, long long utc, int offset_minutes, int format, const char *name);
int jipc_overlay_render(const char *text, unsigned scale, unsigned char *pixels, size_t capacity, unsigned *width, unsigned *height);
int jipc_backend_overlay(jipc_app *app, unsigned lens, const char *text, int corner, char *error, size_t capacity);
/* Aiming from the fixed lens's picture (src/aim.c). u and v run from 0 to 1 across it. */
typedef struct { double x[6], y[6]; } jipc_aim;
void jipc_aim_steps(const jipc_aim *aim, double u, double v, double *x, double *y);
int jipc_aim_where(const jipc_aim *aim, double x, double y, double *u, double *v);
int jipc_aim_json(jipc_app *app, char *out, size_t capacity);
int jipc_aim_post(jipc_app *app, const char *json, char *error, size_t capacity); /* returns an HTTP status */
int jipc_platform_steps(jipc_app *app, int *x, int *y);
int jipc_platform_patrol_goto(jipc_app *app, int slot, char *error, size_t capacity);
int jipc_platform_motion(jipc_app *app, jipc_ptz_motion *motion);
int jipc_platform_privacy(jipc_app *app, int on, int slot, char *error, size_t capacity);
void jipc_platform_stop(jipc_app *app);
int jipc_platform_status(jipc_app *app, jipc_ptz_status *status, char *error, size_t capacity);
int jipc_platform_status_json(jipc_app *app, char *out, size_t capacity);

int jipc_rtsp_start(jipc_app *app);
void jipc_rtsp_stop(jipc_app *app);
/* Platform daemon (joan-daemon) on loopback, for the HTTPS UI's single
 * sign-in and its /api/joan/ relay. */
#define JIPC_JOAN_REPLY_LIMIT 65536
typedef struct {
    char *raw, *body;
    size_t length;
    int status, cookies;
    char type[128], set_cookie[2][512];
} jipc_joan_reply;
size_t jipc_joan_cookies(const char *header, char *out, size_t capacity);
int jipc_joan_request(const char *method, const char *path, const char *type,
                      const char *cookies, const char *csrf, const char *body,
                      size_t length, jipc_joan_reply *reply);
/* Upload: send `length` bytes of `body`, then pull the rest up to `total`. */
typedef ssize_t (*jipc_joan_pull)(void *context, void *data, size_t capacity);
/* A media update from the browser (src/update.c). The three that answer give an HTTP status. */
int jipc_update_receive(const char *first, size_t first_size, size_t total, jipc_joan_pull pull, void *context,
                        char *out, size_t out_capacity, char *error, size_t capacity);
int jipc_update_apply(const char *release, char *error, size_t capacity);
void jipc_update_discard(void);
void jipc_update_tick(void);
int jipc_update_json(char *out, size_t capacity);
int jipc_joan_stream(const char *method, const char *path, const char *type,
                     const char *cookies, const char *csrf, const char *body,
                     size_t length, size_t total, jipc_joan_pull pull, void *context,
                     jipc_joan_reply *reply);
#define JIPC_JOAN_UPLOAD_LIMIT 2097344u /* the platform's own update body limit */
#define JIPC_MEDIA_UPDATE_LIMIT 3400000u /* an app partition, its scripts and its manifest */
void jipc_joan_reply_free(jipc_joan_reply *reply);
int jipc_joan_session(const char *cookie_header, char *csrf, size_t capacity);
void jipc_joan_forget(const char *cookie_header);
int jipc_http_start(jipc_app *app);
void jipc_http_stop(jipc_app *app);
int jipc_discovery_start(jipc_app *app);
void jipc_discovery_stop(jipc_app *app);
/* SNTP client for a board without an RTC; ntp_server is "IPv4[:port]". */
int jipc_ntp_server(const char *text, uint32_t *address, uint16_t *port);
int jipc_ntp_offset(const unsigned char *reply, size_t size, const unsigned char origin[8],
                    int64_t sent_us, uint64_t round_trip_us, int64_t *offset_us);
int jipc_ntp_start(jipc_app *app);
void jipc_ntp_stop(jipc_app *app);
int jipc_ntp_status_json(jipc_app *app, char *out, size_t capacity);
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
