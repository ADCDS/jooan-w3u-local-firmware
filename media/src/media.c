/* SPDX-License-Identifier: MIT */
#include "jipc.h"
#include "imp_abi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

typedef struct {
    FILE *file;
    unsigned char *nal, *au;
    size_t nal_size, au_size;
    int prefix_pending, nal_pending, have_vcl, keyframe;
} replay_input;
typedef struct {
    jipc_app *app;
    jipc_config config;
    void *imp;
    replay_input replay[JIPC_VIDEO_STREAMS];
    unsigned idr_mask;
    uint64_t idr_at[JIPC_STREAMS];
    atomic_int quit;
    FILE *speaker_capture; /* replay sink: what the speaker would play */
} media_state;

static int fail(char *out, size_t capacity, const char *message)
{
    if (out && capacity) snprintf(out, capacity, "%s", message);
    return -1;
}

/* Finds a three- or four-byte Annex B prefix. The NAL header starts at *payload. */
static size_t annexb_next(const unsigned char *data, size_t size, size_t start,
                          size_t *payload)
{
    for (size_t i = start; i + 3 <= size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { *payload = i + 3; return i; }
        if (i + 4 <= size && !data[i + 2] && data[i + 3] == 1) {
            *payload = i + 4;
            return i;
        }
    }
    *payload = size;
    return size;
}

int jipc_streams_init(jipc_app *app)
{
    memset(app->streams, 0, sizeof app->streams);
    /* Reader deadlines use the monotonic clock: an NTP step of the wall clock
     * must not stall or prematurely expire stream consumers. */
    pthread_condattr_t monotonic;
    if (pthread_condattr_init(&monotonic)) return -1;
    if (pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC)) { pthread_condattr_destroy(&monotonic); return -1; }
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) {
        if (pthread_mutex_init(&app->streams[i].mutex, NULL)) {
            while (i--) {
                pthread_cond_destroy(&app->streams[i].ready);
                pthread_mutex_destroy(&app->streams[i].mutex);
            }
            pthread_condattr_destroy(&monotonic);
            return -1;
        }
        if (pthread_cond_init(&app->streams[i].ready, &monotonic)) {
            pthread_mutex_destroy(&app->streams[i].mutex);
            while (i--) {
                pthread_cond_destroy(&app->streams[i].ready);
                pthread_mutex_destroy(&app->streams[i].mutex);
            }
            pthread_condattr_destroy(&monotonic);
            return -1;
        }
        app->streams[i].capacity = i == JIPC_AUDIO_STREAM ? JIPC_AUDIO_RING : JIPC_VIDEO_RING;
        app->streams[i].width = jipc_video_width(&app->config, i);
        app->streams[i].height = jipc_video_height(&app->config, i);
        app->streams[i].fps = app->config.fps;
    }
    pthread_condattr_destroy(&monotonic);
    return 0;
}

void jipc_streams_destroy(jipc_app *app)
{
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) {
        for (unsigned j = 0; j < JIPC_RING_MAX; ++j)
            free(app->streams[i].slots[j].data);
        pthread_cond_destroy(&app->streams[i].ready);
        pthread_mutex_destroy(&app->streams[i].mutex);
    }
}

/* Say whether an access unit has an IDR, and keep its SPS and PPS for a stream
 * if one is given (with that stream's mutex held). */
static int stream_parameters(jipc_stream *stream, const unsigned char *copy, size_t bytes)
{
    int idr = 0;
    size_t payload, start = annexb_next(copy, bytes, 0, &payload);
    while (start < bytes && payload < bytes) {
        size_t next_payload, next = annexb_next(copy, bytes, payload, &next_payload);
        size_t nal_bytes = next - payload;
        while (nal_bytes && !copy[payload + nal_bytes - 1]) --nal_bytes;
        unsigned type = copy[payload] & 31;
        if (type == 5) idr = 1;
        if (stream && type == 7 && nal_bytes <= sizeof stream->sps) {
            memcpy(stream->sps, copy + payload, nal_bytes);
            stream->sps_size = nal_bytes;
        } else if (stream && type == 8 && nal_bytes <= sizeof stream->pps) {
            memcpy(stream->pps, copy + payload, nal_bytes);
            stream->pps_size = nal_bytes;
        }
        start = next;
        payload = next_payload;
    }
    return idr;
}

/* A privacy hold has begun: nothing already in the rings may reach anyone, later or at all. */
void jipc_streams_forget_video(jipc_app *app)
{
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) {
        if (i == JIPC_AUDIO_STREAM || !app->streams[i].capacity) continue;
        pthread_mutex_lock(&app->streams[i].mutex);
        app->streams[i].floor = app->streams[i].sequence;
        pthread_mutex_unlock(&app->streams[i].mutex);
    }
}

int jipc_publish(jipc_app *app, unsigned channel, const void *data, size_t bytes,
                 uint64_t pts_us, int keyframe, enum jipc_codec codec)
{
    if (!app || channel >= JIPC_STREAMS || !data || !bytes || bytes > JIPC_MAX_FRAME ||
        (codec != JIPC_H264 && codec != JIPC_PCMA && codec != JIPC_JPEG))
        return -1;
    unsigned char *copy = malloc(bytes);
    if (!copy) return -1;
    memcpy(copy, data, bytes);
    jipc_stream *stream = &app->streams[channel];
    if (!stream->capacity) { free(copy); return -1; }
    pthread_mutex_lock(&stream->mutex);
    if (channel == JIPC_AUDIO_STREAM && codec == JIPC_PCMA) jipc_audio_meter_add(app, copy, bytes, jipc_now_us());
    if (codec == JIPC_H264 && stream_parameters(stream, copy, bytes)) keyframe = 1;
    uint64_t sequence = ++stream->sequence;
    jipc_frame *frame = &stream->slots[(sequence - 1) % stream->capacity];
    stream->held -= frame->size;
    free(frame->data);
    *frame = (jipc_frame){copy, bytes, pts_us, sequence, !!keyframe, codec};
    stream->held += bytes;
    if (channel != JIPC_AUDIO_STREAM) {
        uint64_t oldest = sequence > stream->capacity ? sequence - stream->capacity + 1 : 1;
        if (stream->kept < oldest) stream->kept = oldest;
        while (stream->held > JIPC_VIDEO_RING_BYTES && stream->kept < sequence) {
            jipc_frame *drop = &stream->slots[(stream->kept++ - 1) % stream->capacity];
            stream->held -= drop->size;
            free(drop->data);
            drop->data = NULL;
            drop->size = 0;
        }
    }
    app->produced[channel] = sequence;
    pthread_cond_broadcast(&stream->ready);
    pthread_mutex_unlock(&stream->mutex);
    return 0;
}

/* Every stream has a recent frame. Frames held for privacy stop on purpose, and
 * after a hold a stream only resumes at its next keyframe, so the check waits a
 * little longer then; a held start has no frames at all until that point. An
 * encoder that never resumes still fails here, once the grace has passed.
 *
 * A stamp later than `now` is the freshest frame there is: `now` was read before
 * the locks, and this thread can wait on one while a reader copies a large frame
 * and the encoder thread stamps the next. Counting that as stale stopped the
 * service on camera1 when privacy ended, with every stream running. */
int jipc_video_stale(jipc_app *app, unsigned streams, uint64_t now, uint64_t *age_us)
{
    if (atomic_load(&app->privacy_hold)) { app->video_grace_until = now + UINT64_C(15000000); return -1; }
    int stale = -1;
    for (unsigned i = 0; i < streams; ++i) {
        pthread_mutex_lock(&app->streams[i].mutex);
        uint64_t sequence = app->streams[i].sequence;
        uint64_t stamp = sequence ? app->streams[i].slots[(sequence - 1) % app->streams[i].capacity].pts_us : 0;
        pthread_mutex_unlock(&app->streams[i].mutex);
        if ((!stamp || (now > stamp && now - stamp > UINT64_C(15000000))) && stale < 0) {
            stale = (int)i;
            if (age_us) *age_us = stamp ? now - stamp : 0;
        }
    }
    return now < app->video_grace_until ? -1 : stale;
}

int jipc_video_healthy(jipc_app *app, unsigned streams, uint64_t now)
{
    return jipc_video_stale(app, streams, now, NULL) < 0;
}

int jipc_publish_video(jipc_app *app, unsigned source, int main_only,
                       const void *data, size_t bytes, uint64_t pts_us, int keyframe)
{
    if (source >= JIPC_VIDEO_STREAMS || (main_only && (source & 1))) return -1;
    /* Held for privacy: no picture reaches anyone, but the parameter sets do. They
     * show nothing, and RTSP and the startup check need them. Afterwards a stream
     * restarts at a keyframe: P-frames would decode onto the last picture before
     * the hold. */
    unsigned bit = 1u << source;
    if (atomic_load(&app->privacy_hold)) {
        atomic_fetch_or(&app->privacy_wait_key, bit);
        for (unsigned ch = source; ch <= source + (main_only ? 1u : 0u); ++ch) {
            jipc_stream *stream = &app->streams[ch];
            if (!stream->capacity) continue;
            pthread_mutex_lock(&stream->mutex);
            (void)stream_parameters(stream, data, bytes);
            pthread_mutex_unlock(&stream->mutex);
        }
        return 0;
    }
    if (atomic_load(&app->privacy_wait_key) & bit) {
        if (!stream_parameters(NULL, data, bytes)) return 0;
        atomic_fetch_and(&app->privacy_wait_key, ~bit);
    }
    if (jipc_publish(app,source,data,bytes,pts_us,keyframe,JIPC_H264)) return -1;
    return main_only ? jipc_publish(app,source+1,data,bytes,pts_us,keyframe,JIPC_H264) : 0;
}

int jipc_read(jipc_app *app, unsigned channel, uint64_t *cursor,
              jipc_frame *out, unsigned timeout_ms)
{
    if (!app || channel >= JIPC_STREAMS || !cursor || !out) return -1;
    memset(out, 0, sizeof *out);
    jipc_stream *stream = &app->streams[channel];
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&stream->mutex);
    /* Held for privacy: what is still in the ring is the picture from before, and someone
     * joining now gets the newest frame, which may be a whole picture. */
#define PRIVACY_HELD (channel != JIPC_AUDIO_STREAM && (atomic_load(&app->privacy_hold) || atomic_load(&app->privacy_wait_key)))
    while ((stream->sequence <= *cursor || stream->sequence <= stream->floor || PRIVACY_HELD) && !app->stop) {
        if (!timeout_ms) { pthread_mutex_unlock(&stream->mutex); return 0; }
        int rc = pthread_cond_timedwait(&stream->ready, &stream->mutex, &deadline);
        if (rc) { pthread_mutex_unlock(&stream->mutex); return rc == ETIMEDOUT ? 0 : -1; }
    }
    if (!stream->sequence || stream->sequence <= *cursor || stream->sequence <= stream->floor || PRIVACY_HELD) {
        pthread_mutex_unlock(&stream->mutex);
        return 0;
    }
#undef PRIVACY_HELD
    uint64_t oldest = stream->sequence > stream->capacity ?
                      stream->sequence - stream->capacity + 1 : 1;
    if (oldest < stream->kept) oldest = stream->kept;
    uint64_t wanted = *cursor ? *cursor + 1 : stream->sequence;
    if (wanted < oldest) wanted = oldest;
    if (wanted <= stream->floor) wanted = stream->floor + 1;
    jipc_frame *source = &stream->slots[(wanted - 1) % stream->capacity];
    unsigned char *copy = malloc(source->size);
    if (!copy) { pthread_mutex_unlock(&stream->mutex); return -1; }
    memcpy(copy, source->data, source->size);
    *out = *source;
    out->data = copy;
    *cursor = wanted;
    pthread_mutex_unlock(&stream->mutex);
    return 1;
}

void jipc_frame_free(jipc_frame *frame)
{
    if (frame) { free(frame->data); memset(frame, 0, sizeof *frame); }
}

int jipc_codec_parameters(jipc_app *app, unsigned channel,
                          unsigned char *sps, size_t *sps_size,
                          unsigned char *pps, size_t *pps_size)
{
    if (!app || channel >= JIPC_VIDEO_STREAMS || !sps_size || !pps_size) return -1;
    jipc_stream *stream = &app->streams[channel];
    pthread_mutex_lock(&stream->mutex);
    int rc = !stream->sps_size || !stream->pps_size ||
             *sps_size < stream->sps_size || *pps_size < stream->pps_size || !sps || !pps;
    if (!rc) {
        memcpy(sps, stream->sps, stream->sps_size);
        memcpy(pps, stream->pps, stream->pps_size);
    }
    *sps_size = stream->sps_size;
    *pps_size = stream->pps_size;
    pthread_mutex_unlock(&stream->mutex);
    return rc ? -1 : 0;
}

/* FILE buffering keeps this streaming parser bounded to two frame buffers per
 * input; a long recording never has to fit in camera RAM. Prefixes are normalized
 * to four bytes. Trailing zeros are Annex B padding rather than NAL content. */
static int replay_nal(replay_input *input)
{
    unsigned zeros = 0;
    int c;
    if (!input->prefix_pending) {
        while ((c = fgetc(input->file)) != EOF) {
            if (c == 1 && zeros >= 2) break;
            zeros = c == 0 ? zeros + (zeros < 4) : 0;
        }
        if (c == EOF) return ferror(input->file) ? -1 : 0;
    }
    input->prefix_pending = 0;
    input->nal_size = 4;
    memcpy(input->nal, "\0\0\0\1", 4);
    zeros = 0;
    while ((c = fgetc(input->file)) != EOF) {
        if (c == 1 && zeros >= 2) { input->prefix_pending = 1; break; }
        if (c == 0) {
            if (++zeros > JIPC_MAX_FRAME) return -1;
            continue;
        }
        if (zeros + 1 > JIPC_MAX_FRAME - input->nal_size) return -1;
        memset(input->nal + input->nal_size, 0, zeros);
        input->nal_size += zeros;
        zeros = 0;
        input->nal[input->nal_size++] = (unsigned char)c;
    }
    if (ferror(input->file)) return -1;
    return input->nal_size > 4 ? 1 : 0;
}

/* first_mb_in_slice is the first unsigned Exp-Golomb value of the RBSP. Its
 * value is zero exactly when the first payload bit is one, so no full SPS/PPS
 * parser is needed for ordinary progressive Annex B slice grouping. */
static int replay_access_unit(replay_input *input)
{
    input->au_size = 0;
    input->have_vcl = input->keyframe = 0;
    for (;;) {
        int rc = input->nal_pending ? 1 : replay_nal(input);
        input->nal_pending = 0;
        if (rc <= 0) return rc < 0 ? -1 : input->have_vcl;
        unsigned type = input->nal[4] & 31;
        int vcl = type >= 1 && type <= 5;
        int first_slice = vcl && input->nal_size > 5 && (input->nal[5] & 128);
        int boundary = input->have_vcl && (first_slice || type == 9 || type == 7 || type == 8 || type == 6);
        if (boundary) { input->nal_pending = 1; return 1; }
        if (input->nal_size > JIPC_MAX_FRAME - input->au_size) return -1;
        memcpy(input->au + input->au_size, input->nal, input->nal_size);
        input->au_size += input->nal_size;
        input->have_vcl |= vcl;
        input->keyframe |= type == 5;
    }
}

static void replay_rewind(replay_input *input)
{
    rewind(input->file);
    input->prefix_pending = input->nal_pending = 0;
}

static int replay_next(replay_input *input, int request_idr)
{
    if (request_idr) replay_rewind(input);
    int rewound = 0;
    for (unsigned count = 0; count < 100000; ++count) {
        int rc = replay_access_unit(input);
        if (rc < 0) return -1;
        if (!rc) {
            if (rewound) return -1;
            replay_rewind(input);
            rewound = 1;
            continue;
        }
        if (!request_idr || input->keyframe) return 0;
    }
    return -1;
}

static void media_release(media_state *state)
{
    if (!state) return;
    if (state->imp) jipc_imp_close(state->imp);
    if (state->speaker_capture) fclose(state->speaker_capture);
    for (unsigned i = 0; i < JIPC_VIDEO_STREAMS; ++i) {
        if (state->replay[i].file) fclose(state->replay[i].file);
        free(state->replay[i].nal);
        free(state->replay[i].au);
    }
    free(state);
}

static void backend_failed(media_state *state, const char *error)
{
    jipc_app *app = state->app;
    app->backend_ready = 0;
    pthread_mutex_lock(&app->config_mutex);
    snprintf(app->backend_error, sizeof app->backend_error, "%s", error);
    pthread_mutex_unlock(&app->config_mutex);
    jipc_log("error", "media: %s", error);
}

static void *media_main(void *opaque)
{
    media_state *state = opaque;
    jipc_app *app = state->app;
    uint64_t next_video = jipc_now_us(), next_audio = next_video;
    const uint64_t interval = 1000000u / state->config.fps;
    /* Replay audio: 20 ms of A-law silence, or a quiet 500 Hz tone with
     * JIPC_REPLAY_TONE=1 so tests can tell sound from the half-duplex mute. */
    unsigned char silence[160], tone[160], *replay_audio = silence;
    memset(silence, 0xd5, sizeof silence);
    for (unsigned i = 0; i < sizeof tone; ++i) {
        static const int wave[16] = {0, 1531, 2828, 3695, 4000, 3695, 2828, 1531,
                                     0, -1531, -2828, -3695, -4000, -3695, -2828, -1531};
        tone[i] = jipc_alaw_encode(wave[i % 16]);
    }
    const char *tone_env = getenv("JIPC_REPLAY_TONE");
    if (tone_env && !strcmp(tone_env, "1")) replay_audio = tone;
    jipc_audio_clock clock;
    jipc_audio_clock_init(&clock, 8000);
    while (!app->stop && !atomic_load(&state->quit)) {
        char error[256] = {0};
        if (state->imp) {
            if (jipc_imp_wait(state->imp, 10) < 0) {
                backend_failed(state, "encoder descriptor wait failed"); break;
            }
            /* Controls already waiting take precedence over another drain.
             * At most one available frame per channel is drained per pass. */
            uint64_t priority_until = jipc_now_us() + 5000;
            while (atomic_load(&app->control_waiters) && !app->stop && !state->quit && jipc_now_us() < priority_until)
                jipc_sleep_ms(1);
            pthread_mutex_lock(&app->control_mutex);
            int rc = jipc_imp_poll(app, state->imp, 0, error, sizeof error);
            if (rc < 0) backend_failed(state, error);
            pthread_mutex_unlock(&app->control_mutex);
            if (rc < 0) break;
            continue;
        }
        uint64_t now = jipc_now_us();
        if (now >= next_video) {
            pthread_mutex_lock(&app->control_mutex);
            unsigned idr = state->idr_mask;
            state->idr_mask = 0;
            pthread_mutex_unlock(&app->control_mutex);
            for (unsigned i = 0; i < state->config.sensors * 2; i += state->config.main_only ? 2 : 1) {
                replay_input *input = &state->replay[i];
                if (replay_next(input, !!(idr & (1u << i))) ||
                    jipc_publish_video(app, i, state->config.main_only, input->au, input->au_size, next_video,
                                       input->keyframe)) {
                    snprintf(error, sizeof error, "replay stream %u: malformed/oversized Annex B frame or allocation failure", i);
                    pthread_mutex_lock(&app->control_mutex);
                    backend_failed(state, error);
                    pthread_mutex_unlock(&app->control_mutex);
                    atomic_store(&state->quit, 1);
                    break;
                }
            }
            next_video += interval;
            if (now > next_video + interval * 4) next_video = now + interval;
        }
        if (state->config.audio && now >= next_audio) {
            const unsigned char *payload = jipc_mic_silenced(app) ? silence : replay_audio;
            if (jipc_publish(app, JIPC_AUDIO_STREAM, payload, sizeof silence,
                             jipc_audio_clock_stamp(&clock, next_audio + 20000, sizeof silence, NULL), 1, JIPC_PCMA)) {
                pthread_mutex_lock(&app->control_mutex);
                backend_failed(state, "cannot publish replay audio");
                pthread_mutex_unlock(&app->control_mutex);
                break;
            }
            next_audio += 20000;
            if (now > next_audio + 80000) next_audio = now + 20000;
        }
        jipc_sleep_ms(2);
    }
    pthread_mutex_lock(&app->control_mutex);
    app->backend_ready = 0;
    pthread_mutex_unlock(&app->control_mutex);
    return NULL;
}

/* Speaker sinks, used only by the speaker thread (src/speaker.c). The
 * hardware sink drives the IMP audio output; the replay sink paces in real
 * time and appends what would be played to JIPC_SPEAKER_CAPTURE. */
static int sink_amplifier(void *context, int on)
{
    media_state *state = context;
    char error[160];
    return jipc_platform_control(state->app, "speaker_enable", on, 0, error, sizeof error);
}
static int imp_sink_open(void *context, int volume, char *error, size_t capacity)
{
    media_state *state = context;
    return jipc_imp_ao_open(state->imp, volume, error, capacity);
}
static int imp_sink_write(void *context, const unsigned char *pcma)
{
    media_state *state = context;
    return jipc_imp_ao_write(state->imp, pcma);
}
static void imp_sink_close(void *context)
{
    media_state *state = context;
    jipc_imp_ao_close(state->imp);
}
static int imp_sink_volume(void *context, int volume)
{
    media_state *state = context;
    return jipc_imp_ao_volume(state->imp, volume);
}
static int replay_sink_open(void *context, int volume, char *error, size_t capacity)
{
    (void)context; (void)error; (void)capacity;
    return volume < -30 || volume > 120 ? -1 : 0;
}
static int replay_sink_write(void *context, const unsigned char *pcma)
{
    media_state *state = context;
    if (state->speaker_capture &&
        fwrite(pcma, 1, JIPC_SPEAKER_FRAME, state->speaker_capture) == JIPC_SPEAKER_FRAME)
        fflush(state->speaker_capture);
    jipc_sleep_ms(40);
    return 0;
}
static void replay_sink_close(void *context) { (void)context; }
static int replay_sink_volume(void *context, int volume)
{
    (void)context;
    return volume < -30 || volume > 120 ? -1 : 0;
}

static void speaker_begin(jipc_app *app, media_state *state)
{
    jipc_speaker_sink sink = {.context = state, .amplifier = sink_amplifier};
    if (state->imp) {
        sink.open = imp_sink_open; sink.write = imp_sink_write;
        sink.close = imp_sink_close; sink.volume = imp_sink_volume;
    } else {
        const char *capture = getenv("JIPC_SPEAKER_CAPTURE");
        if (capture && capture[0]) state->speaker_capture = fopen(capture, "ab");
        sink.open = replay_sink_open; sink.write = replay_sink_write;
        sink.close = replay_sink_close; sink.volume = replay_sink_volume;
    }
    /* Talk is optional: video never depends on the speaker. */
    if (jipc_speaker_start(app, &sink))
        jipc_log("error", "speaker service unavailable; two-way talk is disabled");
}

int jipc_backend_start(jipc_app *app)
{
    if (app->media_started || app->backend_state) return -1;
    media_state *state = calloc(1, sizeof *state);
    if (!state) return fail(app->backend_error, sizeof app->backend_error, "cannot allocate media backend");
    state->app = app;
    atomic_init(&state->quit, 0);
    jipc_config_copy(app, &state->config);
    app->backend_error[0] = '\0';
    if (!state->config.fps || state->config.fps > 120 ||
        state->config.sensors < 1 || state->config.sensors > 2) {
        media_release(state);
        return fail(app->backend_error, sizeof app->backend_error, "invalid media frame rate or sensor count");
    }
    if (!strcmp(state->config.backend, "imp")) {
        if (jipc_imp_open(app, &state->imp, app->backend_error, sizeof app->backend_error)) {
            media_release(state);
            return -1;
        }
    } else if (!strcmp(state->config.backend, "replay")) {
        for (unsigned i = 0; i < state->config.sensors * 2; i += state->config.main_only ? 2 : 1) {
            replay_input *input = &state->replay[i];
            const char *path = state->config.replay[i][0] ? state->config.replay[i] : state->config.replay[0];
            input->file = fopen(path, "rb");
            input->nal = malloc(JIPC_MAX_FRAME);
            input->au = malloc(JIPC_MAX_FRAME);
            if (!input->file || !input->nal || !input->au || replay_next(input, 1)) {
                snprintf(app->backend_error, sizeof app->backend_error,
                         "replay stream %u: cannot read H.264 Annex B file containing an IDR", i);
                media_release(state);
                return -1;
            }
            /* Publish before service startup so DESCRIBE has SPS/PPS immediately. */
            if (jipc_publish_video(app, i, state->config.main_only, input->au, input->au_size, jipc_now_us(), 1)) {
                media_release(state);
                return fail(app->backend_error, sizeof app->backend_error, "cannot publish initial replay frame");
            }
            pthread_mutex_lock(&app->streams[i].mutex);
            int parameters_present = app->streams[i].sps_size && app->streams[i].pps_size;
            pthread_mutex_unlock(&app->streams[i].mutex);
            if (!parameters_present) {
                media_release(state);
                return fail(app->backend_error, sizeof app->backend_error,
                            "replay input must include SPS and PPS before its first IDR");
            }
        }
    } else {
        media_release(state);
        return fail(app->backend_error, sizeof app->backend_error, "unknown media backend");
    }
    app->backend_state = state;
    app->backend_ready = 1;
    int rc = pthread_create(&app->media_thread, NULL, media_main, state);
    if (rc) {
        app->backend_state = NULL;
        app->backend_ready = 0;
        media_release(state);
        return fail(app->backend_error, sizeof app->backend_error, "cannot start media thread");
    }
    app->media_started = 1;
    if (state->config.audio) speaker_begin(app, state);
    return 0;
}

void jipc_backend_stop(jipc_app *app)
{
    /* The speaker uses the backend's audio output: stop it first. */
    jipc_speaker_stop(app);
    pthread_mutex_lock(&app->control_mutex);
    media_state *state = app->backend_state;
    if (state) atomic_store(&state->quit, 1);
    app->backend_ready = 0;
    pthread_mutex_unlock(&app->control_mutex);
    if (app->media_started) { pthread_join(app->media_thread, NULL); app->media_started = 0; }
    pthread_mutex_lock(&app->control_mutex);
    app->backend_state = NULL;
    media_release(state);
    pthread_mutex_unlock(&app->control_mutex);
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) {
        pthread_mutex_lock(&app->streams[i].mutex);
        pthread_cond_broadcast(&app->streams[i].ready);
        pthread_mutex_unlock(&app->streams[i].mutex);
    }
}

static void control_lock(jipc_app *app)
{
    atomic_fetch_add(&app->control_waiters, 1);
    pthread_mutex_lock(&app->control_mutex);
    atomic_fetch_sub(&app->control_waiters, 1);
}

/* Called with the control lock held. */
static int night_apply(jipc_app *app, media_state *state, int night, char *error, size_t capacity)
{
    if (state->imp) {
        int rc = jipc_imp_control(state->imp, "isp_night", night, 0, error, capacity);
        if (rc) return rc;
    }
    return jipc_platform_control(app, "night", night, 0, error, capacity);
}

int jipc_backend_night(jipc_app *app, int night, char *error, size_t capacity)
{
    control_lock(app);
    media_state *state = app->backend_state;
    int rc = !state || !app->backend_ready ? fail(error, capacity, "media backend is not ready")
                                           : night_apply(app, state, night ? 1 : 0, error, capacity);
    pthread_mutex_unlock(&app->control_mutex);
    return rc;
}

int jipc_backend_control(jipc_app *app, const char *operation, int value,
                         int value2, char *error, size_t capacity)
{
    if (!operation) return fail(error, capacity, "missing control operation");
    /* The siren never waits on the encoder either. */
    if (!strcmp(operation, "siren")) return jipc_controls_siren(app, value, value2, error, capacity);
    if (!strcmp(operation, "siren_test")) return jipc_controls_siren_test(app, value, error, capacity);
    if (!strcmp(operation, "strobe")) return jipc_controls_strobe(app, value, error, capacity);
    /* Motors and their deadlines never wait on the encoder/JPEG SDK gate. */
    if (!strcmp(operation, "ptz") || !strcmp(operation, "ptz_stop") || !strcmp(operation, "ptz_recalibrate") ||
        !strncmp(operation, "preset_", 7))
        return jipc_platform_control(app, operation, value, value2, error, capacity);
    control_lock(app);
    media_state *state = app->backend_state;
    int rc;
    if (!state || !app->backend_ready) rc = fail(error, capacity, "media backend is not ready");
    else if (!strcmp(operation, "idr")) {
        if (value < 0 || (unsigned)value >= state->config.sensors * 2)
            rc = fail(error, capacity, "invalid stream channel");
        else {
            /* At most one forced keyframe a second per encoder: each costs the shared bitrate, and a
             * viewer that keeps falling behind must not make it worse for everyone. */
            unsigned owner = jipc_video_owner(&state->config, (unsigned)value);
            uint64_t idr_now = jipc_now_us();
            if (state->idr_at[owner] && idr_now - state->idr_at[owner] < UINT64_C(1000000)) rc = 0;
            else {
                state->idr_at[owner] = idr_now;
                if (state->imp) rc = jipc_imp_control(state->imp, operation, value, value2, error, capacity);
                else { state->idr_mask |= 1u << owner; rc = 0; }
            }
        }
    } else if (!strcmp(operation, "night") && (value == 2 || value == 3)) {
        rc = jipc_controls_patch(app, value == 2 ? "{\"night\":{\"mode\":\"auto\"}}" : "{\"night\":{\"mode\":\"schedule\"}}",
                                 error, capacity);
    } else if (atomic_load(&app->privacy_on) && (!strcmp(operation, "night") || !strcmp(operation, "ircut") || !strcmp(operation, "isp_night"))) {
        /* Night mode undoes the black-out. Kept for afterwards, like the picture settings. */
        rc = value != 0 && value != 1 ? fail(error, capacity, "this takes 0 (day) or 1 (night)") : 0;
    } else if (!strcmp(operation, "night")) {
        rc = value != 0 && value != 1 ? fail(error, capacity, "night requires 0 (day), 1 (night), 2 (auto) or 3 (schedule)")
                                      : night_apply(app, state, value, error, capacity);
    } else if (!strcmp(operation, "ir_mode")) {
        rc = value != 0 && value != 1 ? fail(error, capacity, "ir_mode requires 0 (infrared at night) or 1 (always off)")
                                      : jipc_controls_patch(app, value ? "{\"night\":{\"ir\":\"off\"}}" : "{\"night\":{\"ir\":\"night\"}}",
                                                            error, capacity);
    } else if (!strcmp(operation, "privacy")) {
        rc = value != 0 && value != 1 ? fail(error, capacity, "privacy takes value 1 (on) or 0 (off)")
                                      : jipc_controls_patch(app, value ? "{\"privacy\":{\"on\":true}}" : "{\"privacy\":{\"on\":false}}",
                                                            error, capacity);
    } else if (!strcmp(operation, "white_light_trigger")) {
        rc = jipc_controls_trigger(app, value, error, capacity);
    } else if (!strcmp(operation, "speaker_enable")) {
        if (value && !jipc_speaker_gate_open(app))
            rc = fail(error, capacity, "the speaker stays muted until the boot hook releases it");
        else rc = jipc_platform_control(app, operation, value, value2, error, capacity);
    } else if (!strcmp(operation, "mic_mute")) {
        if (!state->config.audio) rc = fail(error, capacity, "audio is off");
        else if (value != 0 && value != 1) rc = fail(error, capacity, "mic_mute requires 0 or 1");
        else { atomic_store(&app->mic_muted, value || atomic_load(&app->privacy_on)); rc = 0; }
    } else if (!strcmp(operation, "volume") && value2 == 1) {
        rc = jipc_speaker_set_volume(app, value, error, capacity);
    } else if (state->imp && (!strcmp(operation, "flip") || !strcmp(operation, "hflip") || !strcmp(operation, "vflip"))) {
        if (value2 < 0 || (unsigned)value2 >= state->config.sensors || value < 0 ||
            value > (!strcmp(operation,"flip") ? 3 : 1)) {
            rc = fail(error,capacity,"invalid sensor or flip bitmask");
        } else {
            /* Apply orientation and PTZ signs together on ordered restart.
             * Direct SDK writes would leave platform direction state stale. */
            jipc_config config; jipc_config_copy(app,&config);
            unsigned flip=value2?config.flip1:config.flip0;
            if (!strcmp(operation,"flip")) flip=(unsigned)value;
            else {
                unsigned bit=!strcmp(operation,"hflip")?1u:2u;
                flip=(flip&~bit)|(value?bit:0u);
            }
            char patch[32];snprintf(patch,sizeof(patch),"{\"flip%d\":%u}",value2,flip);
            rc=jipc_config_patch(app,patch,error,capacity);
        }
    } else if (!strcmp(operation, "privacy_cover")) {
        /* Black on a lens: measured on camera1 in daylight, every pixel of both
         * lenses reads Y=16 U=V=128 with these three at 0. */
        rc = 0;
        static const char *const cover[] = {"brightness", "contrast", "saturation"};
        for (unsigned i = 0; state->imp && !rc && i < 3; ++i)
            rc = jipc_imp_control(state->imp, cover[i], 0, value, error, capacity);
    } else if (atomic_load(&app->privacy_on) && (!strcmp(operation, "brightness") || !strcmp(operation, "contrast") ||
               !strcmp(operation, "saturation"))) {
        /* kept for afterwards; the lens stays black */
        rc = value < 0 || value > 255 ? fail(error, capacity, "brightness, contrast and saturation must be 0 to 255") : 0;
    } else if (state->imp && (!strcmp(operation, "brightness") || !strcmp(operation, "contrast") ||
               !strcmp(operation, "saturation") || !strcmp(operation, "sharpness") ||
               !strcmp(operation, "isp_night") || !strcmp(operation, "volume") ||
               !strcmp(operation, "gain") || !strcmp(operation, "denoise") ||
               !strcmp(operation, "anti_flicker"))) {
        rc = jipc_imp_control(state->imp, operation, value, value2, error, capacity);
    } else rc = jipc_platform_control(app, operation, value, value2, error, capacity);
    /* Manual changes survive a restart. */
    if (!rc) jipc_controls_note(app, operation, value, value2);
    pthread_mutex_unlock(&app->control_mutex);
    return rc;
}

int jipc_backend_overlay(jipc_app *app, unsigned lens, const char *text, int corner, char *error, size_t capacity)
{
    control_lock(app);
    media_state *state = app->backend_state;
    int rc = state && app->backend_ready && state->imp ? jipc_imp_overlay(state->imp, lens, text, corner, error, capacity) : 0;
    pthread_mutex_unlock(&app->control_mutex);
    return rc;
}

int jipc_backend_snapshot(jipc_app *app, unsigned channel, jipc_frame *frame,
                          char *error, size_t capacity)
{
    if (!frame) return fail(error, capacity, "missing snapshot result");
    memset(frame, 0, sizeof *frame);
    if (atomic_load(&app->privacy_on)) return fail(error, capacity, "privacy mode is on");
    /* One at a time: the control lock is let go during the wait, and a lens has
     * one JPEG encoder, which exists only for its still. */
    static pthread_mutex_t one_still = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&one_still);
    control_lock(app);
    media_state *state = app->backend_state;
    int rc = !state || !app->backend_ready ? fail(error, capacity, "media backend is not ready") :
             state->imp ? jipc_imp_snapshot(state->imp, channel, &app->control_mutex, frame, error, capacity) :
             fail(error, capacity, "JPEG snapshots are unavailable from an H.264 replay input");
    pthread_mutex_unlock(&app->control_mutex);
    if (!rc && atomic_load(&app->privacy_on)) { /* it came on during the wait */
        jipc_frame_free(frame);
        rc = fail(error, capacity, "privacy mode is on");
    }
    pthread_mutex_unlock(&one_still);
    return rc;
}

int jipc_backend_talk(jipc_app *app, uint64_t owner, const void *data, size_t bytes,
                      char *error, size_t capacity)
{
    /* Queued for the speaker thread; never blocks on the audio output. */
    return jipc_talk_submit(app, owner, 0, 1, data, bytes, error, capacity);
}

int jipc_backend_audio_json(jipc_app *app, char *out, size_t capacity)
{
    control_lock(app);
    media_state *state = app->backend_state;
    int rc;
    if (!state || !state->config.audio) rc = snprintf(out, capacity, "{\"state\":\"off\"}");
    else if (state->imp) rc = jipc_imp_audio_json(state->imp, out, capacity) ? -1 : 0;
    else rc = snprintf(out, capacity, "{\"state\":\"replay\"}");
    pthread_mutex_unlock(&app->control_mutex);
    return rc < 0 || (size_t)rc >= capacity ? -1 : 0;
}
