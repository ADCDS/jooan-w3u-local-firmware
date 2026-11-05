#define _GNU_SOURCE
#include "jipc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { int fd; char path[1024]; uint64_t first, last, frames, bytes, opened; } segment;
typedef struct { jipc_app *app; atomic_int quit; uint64_t cursor[JIPC_STREAMS]; segment files[JIPC_STREAMS]; } recorder;
static void finish(segment *s)
{
    if (s->fd < 0) return;
    if (fsync(s->fd)) jipc_log("error", "recording fsync failed: %s", strerror(errno));
    close(s->fd); s->fd = -1;
    char path[1100], json[512]; snprintf(path, sizeof(path), "%s.json", s->path);
    int n = snprintf(json, sizeof(json), "{\"frames\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"first_pts_us\":%" PRIu64 ",\"last_pts_us\":%" PRIu64 "}\n", s->frames, s->bytes, s->first, s->last);
    if (n > 0 && (size_t)n < sizeof(json) && jipc_atomic_file(path, json, (size_t)n, 0600))
        jipc_log("error", "recording metadata persistence failed");
}
static int owned_name(const char *name)
{
    if (strncmp(name, "stream-", 7)) return 0;
    for (const char *p = name; *p; ++p) if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.", *p)) return 0;
    return strstr(name, ".h264") || strstr(name, ".alaw");
}
static int retention(recorder *r, const jipc_config *cfg)
{
    const uint64_t limit = (uint64_t)cfg->record_max_mb * 1024u * 1024u;
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        DIR *dir = opendir(cfg->record_dir); if (!dir) return -1;
        uint64_t total = 0; time_t oldest = 0; char victim[256] = {0}; struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!owned_name(entry->d_name)) continue;
            struct stat st; if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
            total += (uint64_t)st.st_size; int active = 0;
            for (unsigned i = 0; i < JIPC_STREAMS; ++i) if (r->files[i].fd >= 0) {
                const char *base = strrchr(r->files[i].path, '/'); base = base ? base + 1 : r->files[i].path;
                if (!strcmp(base, entry->d_name)) active = 1;
            }
            if (!active && (!victim[0] || st.st_mtime < oldest)) {
                snprintf(victim, sizeof(victim), "%s", entry->d_name); oldest = st.st_mtime;
            }
        }
        if (total <= limit) { closedir(dir); return 0; }
        if (!victim[0]) {
            /* A small quota must rotate active segments, not permanently stop. */
            int closed = 0;
            for (unsigned i = 0; i < JIPC_STREAMS; ++i) if (r->files[i].fd >= 0) { finish(&r->files[i]); closed = 1; }
            closedir(dir);
            if (closed) continue;
            errno = ENOSPC; return -1;
        }
        int rc = unlinkat(dirfd(dir), victim, 0); closedir(dir); if (rc) return -1;
    }
    errno = ENOSPC; return -1;
}
static int begin(segment *s, const jipc_config *c, unsigned channel, const jipc_frame *frame)
{
    struct timespec now; struct tm tm; char stamp[32]; clock_gettime(CLOCK_REALTIME, &now); gmtime_r(&now.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);
    uint64_t mono = jipc_now_us();
    int n = snprintf(s->path, sizeof(s->path), "%s/stream-%u-%s-%" PRIu64 ".%s", c->record_dir, channel, stamp, mono, channel == JIPC_AUDIO_STREAM ? "alaw" : "h264");
    if (n < 0 || (size_t)n >= sizeof(s->path)) return -1;
    s->fd = open(s->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->fd < 0) return -1;
    s->first = s->last = frame->pts_us; s->frames = s->bytes = 0; s->opened = mono; return 0;
}
static void record_error(recorder *r, const char *message)
{
    int saved_errno = errno; char detail[sizeof(r->app->record_error)];
    snprintf(detail, sizeof(detail), "%s: %s", message, strerror(saved_errno));
    pthread_mutex_lock(&r->app->config_mutex); r->app->record_active = 0;
    snprintf(r->app->record_error, sizeof(r->app->record_error), "%s", detail);
    pthread_mutex_unlock(&r->app->config_mutex); jipc_log("error", "%s", detail);
}
static void *record_main(void *opaque)
{
    recorder *r = opaque; jipc_app *app = r->app; uint64_t checked = 0;
    while (!app->stop && !atomic_load(&r->quit)) {
        jipc_config cfg; jipc_config_copy(app, &cfg);
        pthread_mutex_lock(&app->config_mutex); int enabled = app->record_active; pthread_mutex_unlock(&app->config_mutex);
        if (!enabled) {
            for (unsigned i = 0; i < JIPC_STREAMS; ++i) { finish(&r->files[i]); r->cursor[i] = 0; }
            jipc_sleep_ms(40); continue;
        }
        if (jipc_now_us() - checked > 1000000u) {
            checked = jipc_now_us(); if (retention(r, &cfg)) { record_error(r, "recording retention failed"); continue; }
        }
        for (unsigned channel = 0; channel < JIPC_STREAMS; ++channel) {
            if ((channel < JIPC_AUDIO_STREAM && (channel >= cfg.sensors * 2 || jipc_video_owner(&cfg,channel) != channel)) || (channel == JIPC_AUDIO_STREAM && !cfg.audio)) continue;
            uint64_t previous = r->cursor[channel];
            jipc_frame frame; int rc = jipc_read(app, channel, &r->cursor[channel], &frame, 0); if (rc <= 0) continue;
            segment *s = &r->files[channel];
            if (previous && frame.sequence != previous + 1) {
                jipc_log("warn", "recording stream %u lost %" PRIu64 " frames; starting a new segment", channel, frame.sequence - previous - 1);
                finish(s);
                if (channel != JIPC_AUDIO_STREAM && !frame.keyframe) {
                    char ignored[128]; (void)jipc_backend_control(app, "idr", (int)channel, 0, ignored, sizeof(ignored));
                    jipc_frame_free(&frame); continue;
                }
            }
            if (s->fd >= 0 && jipc_now_us() - s->opened >= (uint64_t)cfg.record_seconds * 1000000u) {
                if (channel == JIPC_AUDIO_STREAM || frame.keyframe) finish(s);
                else { char ignored[128]; (void)jipc_backend_control(app, "idr", (int)channel, 0, ignored, sizeof(ignored)); }
            }
            if (s->fd < 0 && channel != JIPC_AUDIO_STREAM && !frame.keyframe) { jipc_frame_free(&frame); continue; }
            if (s->fd < 0) {
                if (begin(s, &cfg, channel, &frame)) { record_error(r, "cannot create recording"); jipc_frame_free(&frame); break; }
                if (channel != JIPC_AUDIO_STREAM) {
                    unsigned char sps[512], pps[512]; size_t a = sizeof(sps), b = sizeof(pps); static const unsigned char prefix[] = {0, 0, 0, 1};
                    if (jipc_codec_parameters(app, channel, sps, &a, pps, &b) ||
                        jipc_write_all(s->fd, prefix, sizeof(prefix)) || jipc_write_all(s->fd, sps, a) ||
                        jipc_write_all(s->fd, prefix, sizeof(prefix)) || jipc_write_all(s->fd, pps, b)) {
                        record_error(r, "cannot write codec headers"); jipc_frame_free(&frame); break;
                    }
                    s->bytes += a + b + 8;
                }
            }
            if (jipc_write_all(s->fd, frame.data, frame.size)) { record_error(r, "cannot write recording"); jipc_frame_free(&frame); break; }
            s->last = frame.pts_us; s->bytes += frame.size; ++s->frames; jipc_frame_free(&frame);
        }
        jipc_sleep_ms(5);
    }
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) finish(&r->files[i]);
    return NULL;
}
int jipc_record_set(jipc_app *app, int enabled)
{
    if (enabled != 0 && enabled != 1) return -1;
    jipc_config cfg; jipc_config_copy(app, &cfg);
    if (enabled) {
        char path[1100]; if (jipc_mkdirs(cfg.record_dir, 0700)) return -1;
        if (snprintf(path, sizeof(path), "%s/.write-test-XXXXXX", cfg.record_dir) >= (int)sizeof(path)) return -1;
        int fd = mkstemp(path); if (fd < 0) return -1; close(fd); unlink(path);
    }
    pthread_mutex_lock(&app->config_mutex); cfg = app->config; cfg.recording = enabled;
    if (jipc_config_save(&cfg)) { pthread_mutex_unlock(&app->config_mutex); return -1; }
    app->config.recording = enabled; app->record_active = enabled; app->record_error[0] = 0;
    pthread_mutex_unlock(&app->config_mutex);
    if (enabled) for (unsigned i = 0; i < cfg.sensors * 2; i += cfg.main_only ? 2 : 1) { char ignored[128]; (void)jipc_backend_control(app, "idr", (int)i, 0, ignored, sizeof(ignored)); }
    return 0;
}
int jipc_record_start(jipc_app *app)
{
    recorder *r = calloc(1, sizeof(*r)); if (!r) return -1;
    r->app = app; atomic_init(&r->quit, 0); for (unsigned i = 0; i < JIPC_STREAMS; ++i) r->files[i].fd = -1;
    if (jipc_mkdirs(app->config.record_dir, 0700)) { free(r); return -1; }
    app->record_active = app->config.recording; app->record_state = r;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 128 * 1024);
    int rc = pthread_create(&app->record_thread, &attr, record_main, r); pthread_attr_destroy(&attr);
    if (rc) { free(r); app->record_state = NULL; return -1; }
    app->record_started = 1; return 0;
}
void jipc_record_stop(jipc_app *app)
{
    recorder *r = app->record_state; if (!r) return;
    atomic_store(&r->quit, 1); if (app->record_started) pthread_join(app->record_thread, NULL);
    app->record_started = 0; app->record_state = NULL; free(r);
}
