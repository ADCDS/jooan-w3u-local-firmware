/* SPDX-License-Identifier: MIT
 * Speaker service. One thread owns the audio output and drives the speaker
 * amplifier; talkers (the RTSP backchannel, POST /api/v1/talk) only queue
 * A-law and never wait on the driver, whose writes can take up to 800 ms.
 *
 * A session starts when a talker's first audio arrives: open the output,
 * switch the amplifier on (every session, never cached), let it settle for
 * 200 ms while audio queues up, then play 40 ms frames. It ends when the
 * talker releases (TEARDOWN or disconnect) and its queue drains, or after
 * 500 ms without audio; ending plays 80 ms of silence and mutes again.
 *
 * The siren plays through the same session. It comes ahead of a talker, whose audio
 * is dropped and refused until it ends, and it has its own volume and a deadline.
 */
#include "jipc.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define QUEUE_BYTES 16000u                      /* 2 s of 8 kHz A-law */
#define TRIM_ABOVE 4800u                        /* a backchannel more than 600 ms ahead... */
#define TRIM_TO 2400u                           /* ...is cut back to 300 ms */
#define RELEASE_TAIL 1600u                      /* at most 200 ms after the talker left */
#define DEADMAN_US UINT64_C(500000)
#define SETTLE_MS 200u
#define HALF_DUPLEX_TAIL_US UINT64_C(300000)
#define MUTE_ATTEMPTS 100u
/* File-scope so the offline tests can shorten them. */
static uint64_t speaker_idle_close_us = UINT64_C(10000000);
static uint64_t speaker_retry_unit_us = UINT64_C(1000000);

typedef struct {
    jipc_app *app;
    jipc_speaker_sink sink;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    int quit, full_duplex;
    unsigned char queue[QUEUE_BYTES];
    size_t head, count;
    int leased, session, released;
    uint64_t owner, last_submit_us, session_end_us;
    uint32_t ssrc;
    int open, volume, faulted, mute_pending;
    unsigned mute_attempts, write_errors, backoff_s;
    uint64_t retry_us;
    uint64_t sessions, played_frames, dropped_bytes, refused, faults;
    int siren_on, siren_volume, siren_volume_applied, volume_restore;
    uint64_t siren_until;
    jipc_siren siren;
    char error[160];
} speaker_state;

/* Guards app->speaker_state for every caller outside the speaker thread. */
static pthread_mutex_t lifetime = PTHREAD_MUTEX_INITIALIZER;
static atomic_int gate_released;

static int talk_error(int code, char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return code;
}

int jipc_speaker_gate_open(jipc_app *app)
{
    (void)app;
    if (atomic_load(&gate_released)) return 1;
    /* The platform's boot hook holds the amplifier muted for the first
     * minute and writes this marker when it lets go. An empty value
     * disables the gate (host tests). */
    const char *path = getenv("JIPC_SPEAKER_GATE");
    if (!path) path = "/run/jooan-local/speaker-hook-released";
    struct stat st;
    if (!path[0] || !stat(path, &st)) { atomic_store(&gate_released, 1); return 1; }
    return 0;
}

static void timed_wait(speaker_state *s, unsigned milliseconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&s->wake, &s->mutex, &deadline);
}

static void drop_oldest(speaker_state *s, size_t bytes)
{
    if (bytes > s->count) bytes = s->count;
    s->head = (s->head + bytes) % QUEUE_BYTES;
    s->count -= bytes;
    s->dropped_bytes += bytes;
}

/* Called with the mutex held; returns with it held. Mutes, and closes the
 * output when asked, outside the lock. */
static void stop_output(speaker_state *s, int close_output)
{
    int was_open = s->open;
    if (close_output) s->open = 0;
    pthread_mutex_unlock(&s->mutex);
    if (was_open && !close_output) {
        unsigned char silence[JIPC_SPEAKER_FRAME];
        memset(silence, 0xd5, sizeof silence);
        /* Two silent frames push the last speech out of the driver's partial
         * period without a click; FlushChnBuf could hang (unbounded SYNC). */
        (void)s->sink.write(s->sink.context, silence);
        (void)s->sink.write(s->sink.context, silence);
    }
    int muted = !s->sink.amplifier(s->sink.context, 0);
    if (close_output && was_open) s->sink.close(s->sink.context);
    pthread_mutex_lock(&s->mutex);
    if (!muted) { s->mute_pending = 1; s->mute_attempts = 0; }
}

static void end_session(speaker_state *s, int close_output)
{
    s->session = s->leased = s->released = 0;
    s->head = s->count = 0;
    s->siren_on = 0;
    s->session_end_us = jipc_now_us();
    stop_output(s, close_output);
    /* The next talker gets the speaker's own volume back. */
    if (s->volume_restore) {
        int open = s->open, volume = s->volume;
        s->volume_restore = 0;
        if (open) {
            pthread_mutex_unlock(&s->mutex);
            (void)s->sink.volume(s->sink.context, volume);
            pthread_mutex_lock(&s->mutex);
        }
    }
}

/* A DAC gain of 60 is unity, and a sound's full level is just below clipping there, so
 * 100% is the loudest it plays clean. A step of the gain is 0.5 dB. */
static int siren_gain(int percent) { return 60 * percent / 100; }

static void fault(speaker_state *s, const char *message)
{
    s->faulted = 1;
    ++s->faults;
    s->backoff_s = s->backoff_s ? (s->backoff_s >= 30 ? 60 : s->backoff_s * 2) : 5;
    s->retry_us = jipc_now_us() + (uint64_t)s->backoff_s * speaker_retry_unit_us;
    s->write_errors = 0;
    snprintf(s->error, sizeof s->error, "%s", message);
    jipc_log("error", "speaker: %s; retrying in %u s", message, s->backoff_s);
    end_session(s, 1);
}

static void *speaker_main(void *opaque)
{
    speaker_state *s = opaque;
    unsigned char frame[JIPC_SPEAKER_FRAME];
    pthread_mutex_lock(&s->mutex);
    while (!s->quit) {
        uint64_t now = jipc_now_us();
        if (!s->session) {
            if (s->mute_pending && s->mute_attempts < MUTE_ATTEMPTS) {
                ++s->mute_attempts;
                pthread_mutex_unlock(&s->mutex);
                int rc = s->sink.amplifier(s->sink.context, 0);
                pthread_mutex_lock(&s->mutex);
                if (!rc) s->mute_pending = 0;
            }
            if (s->open && now - s->session_end_us > speaker_idle_close_us) {
                s->open = 0;
                pthread_mutex_unlock(&s->mutex);
                s->sink.close(s->sink.context);
                pthread_mutex_lock(&s->mutex);
                continue;
            }
            if (s->faulted && now >= s->retry_us) s->faulted = 0;
            if (s->count && (s->faulted || !jipc_speaker_gate_open(s->app))) {
                s->head = s->count = 0;
                s->leased = s->released = 0;
            }
            if (s->siren_on && (s->faulted || !jipc_speaker_gate_open(s->app))) s->siren_on = 0;
            if (!s->count && !s->siren_on) { timed_wait(s, 100); continue; }
            /* A talker's first audio, or the siren: start a session. */
            s->session = 1;
            ++s->sessions;
            int volume = s->siren_on ? siren_gain(s->siren_volume) : s->volume, need_open = !s->open;
            if (s->siren_on && need_open) s->siren_volume_applied = s->siren_volume;
            pthread_mutex_unlock(&s->mutex);
            char error[160] = {0};
            int rc = need_open ? s->sink.open(s->sink.context, volume, error, sizeof error) : 0;
            int opened = !rc;
            if (!rc) rc = s->sink.amplifier(s->sink.context, 1);
            pthread_mutex_lock(&s->mutex);
            if (opened) s->open = 1;
            if (rc) { fault(s, error[0] ? error : "speaker output or amplifier failed"); continue; }
            /* Amplifier settle time, which doubles as the jitter prebuffer. */
            pthread_mutex_unlock(&s->mutex);
            jipc_sleep_ms(SETTLE_MS);
            pthread_mutex_lock(&s->mutex);
            continue;
        }
        if (s->siren_on && now >= s->siren_until) { s->siren_on = 0; s->released = 1; }
        int finished = !s->siren_on && (s->released ? !s->count : !s->count && now - s->last_submit_us > DEADMAN_US);
        if (finished) { end_session(s, 0); continue; }
        if (s->siren_on) {
            if (s->siren_volume_applied != s->siren_volume) {
                int gain = siren_gain(s->siren_volume);
                s->siren_volume_applied = s->siren_volume;
                pthread_mutex_unlock(&s->mutex);
                (void)s->sink.volume(s->sink.context, gain);
                pthread_mutex_lock(&s->mutex);
            }
            s->volume_restore = 1;
            jipc_siren_fill(&s->siren, frame, sizeof frame);
        } else if (!s->count) {
            /* Underrun: give the talker 40 ms, then keep the driver fed. */
            timed_wait(s, 40);
            if (s->count || s->quit) continue;
            memset(frame, 0xd5, sizeof frame);
        } else {
            size_t take = s->count < sizeof frame ? s->count : sizeof frame;
            for (size_t i = 0; i < take; ++i) frame[i] = s->queue[(s->head + i) % QUEUE_BYTES];
            memset(frame + take, 0xd5, sizeof frame - take);
            s->head = (s->head + take) % QUEUE_BYTES;
            s->count -= take;
        }
        pthread_mutex_unlock(&s->mutex);
        int rc = s->sink.write(s->sink.context, frame);
        pthread_mutex_lock(&s->mutex);
        if (rc) {
            if (++s->write_errors >= 3) fault(s, "audio output refused three frames");
        } else {
            s->write_errors = 0;
            s->backoff_s = 0;
            ++s->played_frames;
        }
    }
    int active = s->session || s->mute_pending, open = s->open;
    s->session = s->open = 0;
    pthread_mutex_unlock(&s->mutex);
    /* At process shutdown the platform may already refuse; platform stop
     * mutes the amplifier too. */
    if (active) (void)s->sink.amplifier(s->sink.context, 0);
    if (open) s->sink.close(s->sink.context);
    return NULL;
}

int jipc_speaker_start(jipc_app *app, const jipc_speaker_sink *sink)
{
    if (!app || !sink || !sink->open || !sink->write || !sink->close || !sink->volume || !sink->amplifier)
        return -1;
    speaker_state *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->app = app;
    s->sink = *sink;
    s->volume = 60; /* unity gain in the library's -30..120 scale */
    const char *duplex = getenv("JIPC_TALK_FULL_DUPLEX");
    s->full_duplex = duplex && !strcmp(duplex, "1");
    /* After a crash the amplifier may have been left on: mute once the boot
     * hook has let go of it. */
    s->mute_pending = jipc_speaker_gate_open(app);
    pthread_condattr_t monotonic;
    if (pthread_condattr_init(&monotonic)) { free(s); return -1; }
    pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
    if (pthread_mutex_init(&s->mutex, NULL)) { pthread_condattr_destroy(&monotonic); free(s); return -1; }
    if (pthread_cond_init(&s->wake, &monotonic)) {
        pthread_condattr_destroy(&monotonic); pthread_mutex_destroy(&s->mutex); free(s); return -1;
    }
    pthread_condattr_destroy(&monotonic);
    pthread_attr_t attributes;
    int created = -1;
    if (!pthread_attr_init(&attributes)) {
        if (!pthread_attr_setstacksize(&attributes, 64u * 1024u))
            created = pthread_create(&s->thread, &attributes, speaker_main, s);
        pthread_attr_destroy(&attributes);
    }
    if (created) {
        pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->mutex); free(s);
        return -1;
    }
    pthread_mutex_lock(&lifetime);
    app->speaker_state = s;
    pthread_mutex_unlock(&lifetime);
    return 0;
}

void jipc_speaker_stop(jipc_app *app)
{
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    if (app) app->speaker_state = NULL;
    pthread_mutex_unlock(&lifetime);
    if (!s) return;
    pthread_mutex_lock(&s->mutex);
    s->quit = 1;
    pthread_cond_broadcast(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

int jipc_speaker_available(jipc_app *app)
{
    pthread_mutex_lock(&lifetime);
    int available = app && app->speaker_state;
    pthread_mutex_unlock(&lifetime);
    return available;
}

int jipc_talk_submit(jipc_app *app, uint64_t owner, uint32_t ssrc, int http,
                     const unsigned char *pcma, size_t size, char *error, size_t capacity)
{
    if (!pcma || !size || size > 8000)
        return talk_error(JIPC_TALK_INVALID, error, capacity, "speaker input requires 1..8000 bytes of 8 kHz A-law");
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    if (!s) {
        pthread_mutex_unlock(&lifetime);
        return talk_error(JIPC_TALK_UNAVAILABLE, error, capacity, "the speaker is unavailable (audio is disabled)");
    }
    if (!jipc_speaker_gate_open(app)) {
        pthread_mutex_unlock(&lifetime);
        return talk_error(JIPC_TALK_UNAVAILABLE, error, capacity, "the speaker stays muted until the boot hook releases it");
    }
    pthread_mutex_lock(&s->mutex);
    int result = JIPC_TALK_OK;
    uint64_t now = jipc_now_us();
    if (s->faulted && now < s->retry_us) {
        result = talk_error(JIPC_TALK_UNAVAILABLE, error, capacity, "the speaker output failed; retrying shortly");
    } else if (s->siren_on) {
        ++s->refused;
        result = talk_error(JIPC_TALK_BUSY, error, capacity, "the siren is sounding");
    } else if (s->leased && (s->owner != owner || s->ssrc != ssrc)) {
        ++s->refused;
        result = talk_error(JIPC_TALK_BUSY, error, capacity, "another talker is using the speaker");
    } else if (http && s->count + size > QUEUE_BYTES) {
        result = talk_error(JIPC_TALK_BUSY, error, capacity, "the speaker queue is full; send shorter chunks");
    } else {
        if (!s->leased) { s->leased = 1; s->owner = owner; s->ssrc = ssrc; }
        s->released = 0;
        if (s->count + size > QUEUE_BYTES) drop_oldest(s, s->count + size - QUEUE_BYTES);
        for (size_t i = 0; i < size; ++i) s->queue[(s->head + s->count + i) % QUEUE_BYTES] = pcma[i];
        s->count += size;
        /* A backchannel that ran ahead (network burst) only adds latency. */
        if (!http && s->count > TRIM_ABOVE) drop_oldest(s, s->count - TRIM_TO);
        s->last_submit_us = now;
        pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_unlock(&lifetime);
    return result;
}

void jipc_talk_release(jipc_app *app, uint64_t owner)
{
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    if (s) {
        pthread_mutex_lock(&s->mutex);
        if (s->leased && s->owner == owner) {
            s->released = 1;
            if (s->count > RELEASE_TAIL) s->count = RELEASE_TAIL;
            pthread_cond_signal(&s->wake);
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&lifetime);
}

int jipc_talk_active(jipc_app *app)
{
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    int active = 0;
    if (s) {
        pthread_mutex_lock(&s->mutex);
        uint64_t now = jipc_now_us();
        active = !s->full_duplex && (s->session ||
                 (s->session_end_us && now - s->session_end_us < HALF_DUPLEX_TAIL_US));
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&lifetime);
    return active;
}

int jipc_siren_start(jipc_app *app, unsigned sound, unsigned seconds, int percent, char *error, size_t capacity)
{
    if (sound >= JIPC_SIREN_SOUNDS || !seconds || seconds > JIPC_SIREN_MAX_SECONDS || percent < 1 || percent > 100)
        return talk_error(-1, error, capacity, "the siren takes one of its sounds, 1 to 300 seconds and 1 to 100%");
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    int rc = 0;
    if (!s) rc = talk_error(-1, error, capacity, "the speaker is unavailable (audio is disabled)");
    else if (!jipc_speaker_gate_open(app))
        rc = talk_error(-1, error, capacity, "the speaker stays muted until the boot hook releases it");
    else {
        pthread_mutex_lock(&s->mutex);
        if (s->faulted && jipc_now_us() < s->retry_us)
            rc = talk_error(-1, error, capacity, "the speaker output failed; retrying shortly");
        else {
            /* Whoever was talking stops here. */
            s->head = s->count = 0;
            s->leased = s->released = 0;
            s->siren_on = 1;
            s->siren_volume = percent;
            s->siren_volume_applied = 0; /* no percentage: whatever the output has now is not it */
            s->siren_until = jipc_now_us() + (uint64_t)seconds * UINT64_C(1000000);
            jipc_siren_init(&s->siren, sound);
            pthread_cond_signal(&s->wake);
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&lifetime);
    return rc;
}

void jipc_siren_stop(jipc_app *app)
{
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    if (s) {
        pthread_mutex_lock(&s->mutex);
        if (s->siren_on) { s->siren_on = 0; s->released = 1; pthread_cond_signal(&s->wake); }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&lifetime);
}

int jipc_speaker_set_volume(jipc_app *app, int volume, char *error, size_t capacity)
{
    if (volume < -30 || volume > 120)
        return talk_error(-1, error, capacity, "speaker volume must be -30..120 (60 is unity gain)");
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    int rc = 0;
    if (!s) rc = talk_error(-1, error, capacity, "the speaker is unavailable (audio is disabled)");
    else {
        pthread_mutex_lock(&s->mutex);
        s->volume = volume;
        int open = s->open && !s->siren_on; /* kept for after the siren */
        pthread_mutex_unlock(&s->mutex);
        /* The library's volume is a plain store; safe beside a blocked write. */
        if (open && s->sink.volume(s->sink.context, volume))
            rc = talk_error(-1, error, capacity, "the audio output refused the volume");
    }
    pthread_mutex_unlock(&lifetime);
    return rc;
}

int jipc_speaker_status_json(jipc_app *app, char *out, size_t capacity)
{
    pthread_mutex_lock(&lifetime);
    speaker_state *s = app ? app->speaker_state : NULL;
    int n;
    if (!s) n = snprintf(out, capacity, "{\"state\":\"off\",\"gate_released\":%s}",
                         jipc_speaker_gate_open(app) ? "true" : "false");
    else {
        pthread_mutex_lock(&s->mutex);
        char escaped[340] = "\"\"";
        jipc_json_escape(s->error, escaped, sizeof escaped);
        const char *state = s->faulted ? "faulted" : s->siren_on ? "siren" : s->session ? "talking" : "idle";
        char siren[64] = "null";
        if (s->siren_on) {
            uint64_t now = jipc_now_us();
            snprintf(siren, sizeof siren, "{\"sound\":\"%s\",\"left_s\":%u}", jipc_siren_name(s->siren.sound),
                     now < s->siren_until ? (unsigned)((s->siren_until - now + 999999u) / 1000000u) : 0u);
        }
        n = snprintf(out, capacity,
                     "{\"state\":\"%s\",\"siren\":%s,\"gate_released\":%s,\"volume\":%d,\"half_duplex\":%s,"
                     "\"sessions\":%llu,\"played_ms\":%llu,\"dropped_ms\":%llu,\"refused\":%llu,"
                     "\"faults\":%llu,\"queued_ms\":%u,\"error\":%s}",
                     state, siren, jipc_speaker_gate_open(app) ? "true" : "false", s->volume,
                     s->full_duplex ? "false" : "true", (unsigned long long)s->sessions,
                     (unsigned long long)(s->played_frames * 40u),
                     (unsigned long long)(s->dropped_bytes / 8u), (unsigned long long)s->refused,
                     (unsigned long long)s->faults, (unsigned)(s->count / 8u), escaped);
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&lifetime);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
