/* SPDX-License-Identifier: MIT
 * W3-U JA-A12 platform controls. This is original code implementing observed
 * userspace ABIs, not kernel-driver source. See docs/platform.md for evidence.
 */
#define _GNU_SOURCE
#include "jipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { MOTOR_STOP = 1, MOTOR_POSITION = 4, MOTOR_SPEED = 5,
       MOTOR_MOVE_NONBLOCK = 8, PAN_MAX = 8600, TILT_MAX = 2000,
       MOTOR_RATE = 450, CHUNK_STEPS = 90, PRESET_COUNT = 16 };
#define MANUAL_LEASE_US UINT64_C(1200000)
#define CHUNK_LIMIT_US UINT64_C(1000000)
#define PRESET_LIMIT_US UINT64_C(45000000)

typedef struct { int32_t x, y, moving, speed; } motor_position;
typedef struct { int valid, x, y; } saved_preset;
typedef struct {
    jipc_app *app;
    pthread_mutex_t mutex;
    pthread_t worker;
    int replay, quitting, motor_fd, x, y, known, moving, chunk_active, faulted;
    int vx, vy, axis, goto_active, target_x, target_y;
    int night, white_light, pilot_led, speaker_enable, speaker_used, gpio_used;
    uint64_t lease_until, chunk_until, goto_until, replay_previous, next_chunk_us;
    double replay_x, replay_y;
    saved_preset presets[PRESET_COUNT];
    char path[640], last_error[192];
} platform_state;

/* Protects app->platform_state lifetime. HTTP/ONVIF controls and shutdown may
 * arrive concurrently; the worker only takes its private mutex. */
static pthread_mutex_t platform_lifetime = PTHREAD_MUTEX_INITIALIZER;

static int fail(char *error, size_t capacity, const char *format, ...)
{
    va_list ap;
    if (error && capacity) {
        va_start(ap, format);
        vsnprintf(error, capacity, format, ap);
        va_end(ap);
    }
    return -1;
}

static int clamp(int value, int minimum, int maximum)
{
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

static int write_text(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    int rc, saved;
    if (fd < 0) return -1;
    rc = jipc_write_all(fd, value, strlen(value));
    saved = errno;
    if (close(fd) && !rc) { rc = -1; saved = errno; }
    errno = saved;
    return rc;
}

/* Use only the verified outputs. In particular GPIO57 is a button input in
 * this factory profile, despite its presence in an unused indicator section. */
static int gpio_output(int pin, int high)
{
    char path[96], number[16];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK)) {
        snprintf(number, sizeof(number), "%d", pin);
        if (write_text("/sys/class/gpio/export", number) && errno != EBUSY)
            return -1;
    }
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/active_low", pin);
    if (write_text(path, "0")) return -1;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    /* Sysfs high/low atomically configures output direction and initial level. */
    return write_text(path, high ? "high" : "low");
}

static int gpio_night(platform_state *state, int night)
{
    int rc = 0, saved = 0;
    if (!state->replay) {
        state->gpio_used = 1;
        /* IrcutModel=1: night pulses GPIO49, day pulses GPIO50. Both sensor
         * descriptions refer to the same pair, so pulse it exactly once. */
        if (gpio_output(49, night) || gpio_output(50, !night)) {
            rc = -1; saved = errno;
        } else {
            jipc_sleep_ms(150);
        }
        if (gpio_output(49, 0) && !rc) { rc = -1; saved = errno; }
        if (gpio_output(50, 0) && !rc) { rc = -1; saved = errno; }
        if (!rc && gpio_output(59, night)) { rc = -1; saved = errno; }
    }
    if (!rc) state->night = night;
    errno = saved;
    return rc;
}

static int motor_read(platform_state *state)
{
    motor_position position = {0};
    if (state->replay) return 0;
    if (state->motor_fd < 0 || ioctl(state->motor_fd, MOTOR_POSITION, &position))
        return -1;
    if (position.x < 0 || position.x > PAN_MAX + 100 ||
        position.y < 0 || position.y > TILT_MAX + 100 ||
        position.moving < 0 || position.moving > 1) {
        errno = EPROTO;
        return -1;
    }
    state->x = clamp(position.x, 0, PAN_MAX);
    state->y = clamp(position.y, 0, TILT_MAX);
    state->moving = position.moving;
    state->known = 1;
    return 0;
}

static int motor_open_device(platform_state *state)
{
    struct stat status;
    int32_t speed = MOTOR_RATE;
    if (state->replay || state->motor_fd >= 0) return 0;
    state->motor_fd = open("/dev/motor", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state->motor_fd < 0) return -1;
    if (fstat(state->motor_fd, &status) || !S_ISCHR(status.st_mode)) {
        close(state->motor_fd);
        state->motor_fd = -1;
        errno = ENODEV;
        return -1;
    }
    if (ioctl(state->motor_fd, MOTOR_SPEED, &speed) || motor_read(state)) {
        int saved = errno;
        close(state->motor_fd);
        state->motor_fd = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

static int motor_stop_motion(platform_state *state)
{
    int rc = 0;
    state->vx = state->vy = state->goto_active = state->chunk_active = 0;
    state->lease_until = 0;
    state->next_chunk_us = 0;
    if (!state->replay && state->motor_fd >= 0) {
        rc = ioctl(state->motor_fd, MOTOR_STOP, 0);
        if (!rc) rc = motor_read(state);
    }
    if (!rc) state->moving = 0;
    return rc;
}

static void worker_error(platform_state *state, const char *operation)
{
    int saved = errno;
    (void)motor_stop_motion(state);
    state->faulted = 1;
    snprintf(state->last_error, sizeof(state->last_error), "%s: %s",
             operation, strerror(saved));
    jipc_log("error", "platform: %s", state->last_error);
}

static void replay_tick(platform_state *state, uint64_t now)
{
    double dt = (double)(now - state->replay_previous) / 1000000.0;
    state->replay_previous = now;
    if (dt > 0.1) dt = 0.1;
    if (state->goto_active) {
        double dx = state->target_x - state->replay_x;
        double dy = state->target_y - state->replay_y;
        double amount = 70.0 * dt;
        state->replay_x += dx > amount ? amount : dx < -amount ? -amount : dx;
        state->replay_y += dy > amount ? amount : dy < -amount ? -amount : dy;
        state->x = (int)state->replay_x;
        state->y = (int)state->replay_y;
        if (state->replay_x == state->target_x && state->replay_y == state->target_y)
            state->goto_active = 0;
    } else if (state->lease_until && now < state->lease_until) {
        state->replay_x += state->vx * dt * 0.7;
        state->replay_y += state->vy * dt * 0.7;
        if (state->replay_x < -100) state->replay_x = -100;
        if (state->replay_x > 100) state->replay_x = 100;
        if (state->replay_y < -100) state->replay_y = -100;
        if (state->replay_y > 100) state->replay_y = 100;
        state->x = (int)state->replay_x;
        state->y = (int)state->replay_y;
    } else {
        state->vx = state->vy = 0;
        state->lease_until = 0;
    }
    state->moving = state->goto_active || state->vx || state->vy;
}

static void native_tick(platform_state *state, uint64_t now)
{
    int delta_x = 0, delta_y = 0, amount;
    int32_t move[2];
    if (state->motor_fd < 0 || state->faulted) return;
    if ((state->lease_until && now >= state->lease_until) ||
        (state->goto_active && now >= state->goto_until)) {
        if (motor_stop_motion(state)) worker_error(state, "motor deadman stop");
        return;
    }
    if (motor_read(state)) { worker_error(state, "motor status"); return; }
    if (state->chunk_active && state->moving) {
        if (now >= state->chunk_until) {
            errno = ETIMEDOUT;
            worker_error(state, "motor chunk exceeded one second");
        }
        return;
    }
    state->chunk_active = 0;
    if (state->goto_active) {
        delta_x = state->target_x - state->x;
        delta_y = state->target_y - state->y;
        if (!delta_x && !delta_y) {
            (void)motor_stop_motion(state);
            return;
        }
        delta_x = clamp(delta_x, -CHUNK_STEPS, CHUNK_STEPS);
        delta_y = clamp(delta_y, -CHUNK_STEPS, CHUNK_STEPS);
    } else if (state->lease_until) {
        if (now < state->next_chunk_us) return;
        delta_x = state->vx * CHUNK_STEPS / 100;
        delta_y = state->vy * CHUNK_STEPS / 100;
        if (state->vx && !delta_x) delta_x = state->vx < 0 ? -1 : 1;
        if (state->vy && !delta_y) delta_y = state->vy < 0 ? -1 : 1;
        delta_x = clamp(state->x + delta_x, 0, PAN_MAX) - state->x;
        delta_y = clamp(state->y + delta_y, 0, TILT_MAX) - state->y;
    }
    /* Both axes share coils through GPIO17. Never submit simultaneous axes:
     * the driver's command8 may replace the first axis with the second. */
    state->axis = !state->axis;
    if (delta_x && (!delta_y || state->axis)) delta_y = 0;
    else if (delta_y) delta_x = 0;
    amount = delta_x ? delta_x : delta_y;
    if (!amount) { state->moving = 0; return; }
    move[0] = delta_x;
    move[1] = delta_y;
    if (ioctl(state->motor_fd, MOTOR_MOVE_NONBLOCK, move)) {
        worker_error(state, "motor move");
        return;
    }
    state->moving = state->chunk_active = 1;
    state->chunk_until = now + CHUNK_LIMIT_US;
    if (!state->goto_active) state->next_chunk_us = now + UINT64_C(200000);
}

static void *platform_worker(void *argument)
{
    platform_state *state = argument;
    for (;;) {
        uint64_t now = jipc_now_us();
        pthread_mutex_lock(&state->mutex);
        if (state->quitting || state->app->stop) {
            (void)motor_stop_motion(state);
            pthread_mutex_unlock(&state->mutex);
            return NULL;
        }
        if (state->replay) replay_tick(state, now);
        else native_tick(state, now);
        pthread_mutex_unlock(&state->mutex);
        jipc_sleep_ms(25);
    }
}

static void presets_load(platform_state *state)
{
    char text[2048], backend[16], extra;
    struct stat status;
    int fd = open(state->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK), slot, x, y;
    ssize_t bytes;
    char *line, *save;
    if (fd < 0) return;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode)) { close(fd); return; }
    bytes = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (bytes <= 0 || bytes == (ssize_t)sizeof(text) - 1) return;
    text[bytes] = 0;
    line = strtok_r(text, "\n", &save);
    if (!line || sscanf(line, "JIPC_PRESETS_1 %15s %c", backend, &extra) != 1 ||
        strcmp(backend, state->replay ? "replay" : "imp")) return;
    while ((line = strtok_r(NULL, "\n", &save))) {
        if (sscanf(line, "%d %d %d %c", &slot, &x, &y, &extra) != 3 ||
            slot < 1 || slot > PRESET_COUNT ||
            (state->replay && (x < -100 || x > 100 || y < -100 || y > 100)) ||
            (!state->replay && (x < 0 || x > PAN_MAX || y < 0 || y > TILT_MAX)))
            continue;
        state->presets[slot - 1] = (saved_preset){1, x, y};
    }
}

static int presets_save(platform_state *state)
{
    char text[2048];
    jipc_config config;
    size_t length;
    int index, n;
    jipc_config_copy(state->app, &config);
    if (jipc_mkdirs(config.state_dir, 0700)) return -1;
    n = snprintf(text, sizeof(text), "JIPC_PRESETS_1 %s\n", state->replay ? "replay" : "imp");
    length = (size_t)n;
    for (index = 0; index < PRESET_COUNT; ++index) {
        saved_preset *preset = &state->presets[index];
        if (!preset->valid) continue;
        n = snprintf(text + length, sizeof(text) - length, "%d %d %d\n",
                     index + 1, preset->x, preset->y);
        if (n < 0 || (size_t)n >= sizeof(text) - length) { errno = EOVERFLOW; return -1; }
        length += (size_t)n;
    }
    return jipc_atomic_file(state->path, text, length, 0600);
}

/* Called with platform_lifetime held. Creation touches no target device. */
static platform_state *platform_get(jipc_app *app, char *error, size_t capacity)
{
    platform_state *state = app->platform_state;
    jipc_config config;
    int rc;
    if (state) return state;
    if (app->stop) { fail(error, capacity, "application is stopping"); return NULL; }
    jipc_config_copy(app, &config);
    if (strcmp(config.backend, "replay") && strcmp(config.backend, "imp")) {
        fail(error, capacity, "platform requires backend replay or imp"); return NULL;
    }
    state = calloc(1, sizeof(*state));
    if (!state) { fail(error, capacity, "platform allocation failed"); return NULL; }
    state->app = app;
    state->replay = !strcmp(config.backend, "replay");
    state->motor_fd = -1;
    state->known = state->replay;
    state->night = state->white_light = state->pilot_led = state->speaker_enable = -1;
    state->replay_previous = jipc_now_us();
    if (snprintf(state->path, sizeof(state->path), "%s/presets.v1", config.state_dir)
        >= (int)sizeof(state->path)) {
        free(state); fail(error, capacity, "preset path is too long"); return NULL;
    }
    rc = pthread_mutex_init(&state->mutex, NULL);
    if (rc) { free(state); fail(error, capacity, "platform mutex: %s", strerror(rc)); return NULL; }
    presets_load(state);
    rc = pthread_create(&state->worker, NULL, platform_worker, state);
    if (rc) {
        pthread_mutex_destroy(&state->mutex);
        free(state); fail(error, capacity, "platform thread: %s", strerror(rc)); return NULL;
    }
    app->platform_state = state;
    return state;
}

int jipc_platform_control(jipc_app *app, const char *operation, int value,
                          int value2, char *error, size_t capacity)
{
    platform_state *state;
    int rc = 0, motion, preset_op;
    saved_preset previous;
    if (error && capacity) error[0] = 0;
    if (!app || !operation) return fail(error, capacity, "invalid platform request");
    motion = !strcmp(operation, "ptz");
    preset_op = !strcmp(operation, "preset_set") || !strcmp(operation, "preset_goto") ||
                !strcmp(operation, "preset_remove");
    if (!motion && !preset_op && strcmp(operation, "ptz_stop") &&
        strcmp(operation, "night") && strcmp(operation, "ircut") &&
        strcmp(operation, "white_light") && strcmp(operation, "pilot_led") &&
        strcmp(operation, "speaker_enable"))
        return fail(error, capacity, "unsupported platform operation: %s", operation);
    if (motion && (value < -100 || value > 100 || value2 < -100 || value2 > 100))
        return fail(error, capacity, "PTZ velocity must be between -100 and 100");
    if (preset_op && (value < 1 || value > PRESET_COUNT))
        return fail(error, capacity, "preset slot must be between 1 and 16");
    if ((!strcmp(operation, "night") || !strcmp(operation, "ircut")) && value == 2)
        return fail(error, capacity, "automatic day/night requires an unverified sensor exposure ABI; select day=0 or night=1");
    if (!motion && !preset_op && strcmp(operation, "ptz_stop") && (value < 0 || value > 1))
        return fail(error, capacity, "control value must be 0 or 1");
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, capacity);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    if (app->stop || state->quitting) { rc = fail(error, capacity, "application is stopping"); goto done; }
    if ((motion || (preset_op && strcmp(operation, "preset_remove"))) && motor_open_device(state)) {
        rc = fail(error, capacity, "motor device: %s", strerror(errno)); goto done;
    }
    if (motion) {
        if ((state->goto_active || state->vx != value || state->vy != value2 || (!value && !value2)) &&
            motor_stop_motion(state)) { rc = fail(error, capacity, "stop previous motion: %s", strerror(errno)); goto done; }
        state->vx = value;
        state->vy = value2;
        state->lease_until = (value || value2) ? jipc_now_us() + MANUAL_LEASE_US : 0;
        state->last_error[0] = 0;
        state->faulted = 0;
    } else if (!strcmp(operation, "ptz_stop")) {
        if (motor_stop_motion(state)) rc = fail(error, capacity, "motor stop: %s", strerror(errno));
    } else if (!strcmp(operation, "preset_set")) {
        if (motor_stop_motion(state) || motor_read(state)) { rc = fail(error, capacity, "motor position: %s", strerror(errno)); goto done; }
        previous = state->presets[value - 1];
        state->presets[value - 1] = (saved_preset){1, state->x, state->y};
        if (presets_save(state)) {
            state->presets[value - 1] = previous;
            rc = fail(error, capacity, "save preset: %s", strerror(errno));
        }
    } else if (!strcmp(operation, "preset_remove")) {
        previous = state->presets[value - 1];
        state->presets[value - 1].valid = 0;
        if (presets_save(state)) {
            state->presets[value - 1] = previous;
            rc = fail(error, capacity, "remove preset: %s", strerror(errno));
        }
    } else if (!strcmp(operation, "preset_goto")) {
        saved_preset *preset = &state->presets[value - 1];
        if (!preset->valid) { rc = fail(error, capacity, "preset slot is empty"); goto done; }
        if (motor_stop_motion(state)) { rc = fail(error, capacity, "motor stop: %s", strerror(errno)); goto done; }
        state->target_x = preset->x;
        state->target_y = preset->y;
        state->goto_until = jipc_now_us() + PRESET_LIMIT_US;
        state->goto_active = 1;
        state->last_error[0] = 0;
        state->faulted = 0;
    } else if (!strcmp(operation, "night") || !strcmp(operation, "ircut")) {
        if (gpio_night(state, value)) rc = fail(error, capacity, "IR-cut/infrared GPIO: %s", strerror(errno));
    } else {
        int pin = !strcmp(operation, "white_light") ? 60 :
                  !strcmp(operation, "speaker_enable") ? 63 : 58;
        if (!state->replay) {
            if (pin == 63) state->speaker_used = 1;
            else state->gpio_used = 1;
        }
        if (!state->replay && gpio_output(pin, value))
            rc = fail(error, capacity, "GPIO%d: %s", pin, strerror(errno));
        else if (pin == 60) state->white_light = value;
        else if (pin == 63) state->speaker_enable = value;
        else state->pilot_led = value;
    }
done:
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

int jipc_platform_status(jipc_app *app, jipc_ptz_status *status, char *error, size_t capacity)
{
    platform_state *state;
    int rc = -1, index;
    if (!app || !status) return fail(error, capacity, "invalid platform status request");
    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, capacity);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    if (motor_open_device(state) || motor_read(state)) {
        fail(error, capacity, "motor status: %s", strerror(errno));
        goto done;
    }
    status->pan = state->replay ? state->x : state->x * 200 / PAN_MAX - 100;
    status->tilt = state->replay ? state->y : state->y * 200 / TILT_MAX - 100;
    status->moving = state->moving || state->goto_active || state->vx || state->vy;
    status->night = state->night;
    status->white_light = state->white_light;
    for (index = 0; index < PRESET_COUNT; ++index)
        if (state->presets[index].valid) status->preset_mask |= UINT32_C(1) << index;
    rc = 0;
done:
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

int jipc_platform_status_json(jipc_app *app, char *out, size_t capacity)
{
    platform_state *state;
    char error[64], escaped[400];
    size_t used;
    int n, index, first = 1, x, y, rc = -1;
    if (!app || !out || !capacity) return -1;
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, sizeof(error));
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    x = state->replay ? state->x : state->x * 200 / PAN_MAX - 100;
    y = state->replay ? state->y : state->y * 200 / TILT_MAX - 100;
    if (jipc_json_escape(state->last_error, escaped, sizeof(escaped)) < 0)
        strcpy(escaped, "\"error encoding platform status\"");
    n = snprintf(out, capacity,
                 "{\"backend\":\"%s\",\"position_known\":%s,\"position_x\":%d,\"position_y\":%d,"
                 "\"moving\":%s,\"preset_motion\":%s,\"night\":%d,\"white_light\":%d,"
                 "\"pilot_led\":%d,\"speaker_enable\":%d,\"manual_timeout_ms\":1200,\"last_error\":%s,\"presets\":[",
                 state->replay ? "replay" : "imp", state->known ? "true" : "false", x, y,
                 (state->moving || state->goto_active || state->vx || state->vy) ? "true" : "false",
                 state->goto_active ? "true" : "false", state->night, state->white_light,
                 state->pilot_led, state->speaker_enable, escaped);
    if (n < 0 || (size_t)n >= capacity) goto done;
    used = (size_t)n;
    for (index = 0; index < PRESET_COUNT; ++index) {
        saved_preset *preset = &state->presets[index];
        if (!preset->valid) continue;
        x = state->replay ? preset->x : preset->x * 200 / PAN_MAX - 100;
        y = state->replay ? preset->y : preset->y * 200 / TILT_MAX - 100;
        n = snprintf(out + used, capacity - used, "%s{\"slot\":%d,\"x\":%d,\"y\":%d}",
                     first ? "" : ",", index + 1, x, y);
        if (n < 0 || (size_t)n >= capacity - used) goto done;
        used += (size_t)n;
        first = 0;
    }
    if (capacity - used < 3) goto done;
    memcpy(out + used, "]}", 3);
    rc = (int)(used + 2);
done:
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

void jipc_platform_stop(jipc_app *app)
{
    platform_state *state;
    if (!app) return;
    pthread_mutex_lock(&platform_lifetime);
    state = app->platform_state;
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return; }
    pthread_mutex_lock(&state->mutex);
    state->quitting = 1;
    (void)motor_stop_motion(state);
    pthread_mutex_unlock(&state->mutex);
    pthread_join(state->worker, NULL);
    if (!state->replay && state->gpio_used) {
        (void)gpio_output(49, 0);
        (void)gpio_output(50, 0);
        (void)gpio_output(59, 0);
        (void)gpio_output(60, 0);
        (void)gpio_output(58, 0);
    }
    if (!state->replay && state->speaker_used) (void)gpio_output(63, 0);
    if (state->motor_fd >= 0) close(state->motor_fd);
    pthread_mutex_destroy(&state->mutex);
    app->platform_state = NULL;
    free(state);
    pthread_mutex_unlock(&platform_lifetime);
}
