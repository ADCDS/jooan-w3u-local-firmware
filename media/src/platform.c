/* SPDX-License-Identifier: MIT
 * W3-U JA-A12 platform controls. This is original code implementing observed
 * userspace ABIs, not kernel-driver source. See docs/platform.md for evidence.
 */
#define _GNU_SOURCE
#include "jipc.h"
#include "cJSON.h"
#include <math.h>
#include <strings.h>
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

enum { MOTOR_STOP = 1, MOTOR_RESET = 2, MOTOR_POSITION = 4, MOTOR_SPEED = 5,
       MOTOR_MOVE = 3, PAN_MAX = 4080, TILT_MAX = 1230,
       MOTOR_RATE = 450, NUDGE_RATE = 540, PRESET_COUNT = 16 };
/* Boot homing replays the OEM joa_ptz.c motor_step_reset with this board's
 * factory PtzParam: HorizontalMaxStep, VerticalMaxStep, VerticalResetStep and
 * ResetSpeed. See docs/platform.md. */
enum { HOME_PAN_STEPS = 4080, HOME_TILT_STEPS = 1200, HOME_TILT_RESET = 200, HOME_SPEED = 450 };
/* Half the pan/tilt lens's field of view in motor steps, measured on camera1
 * from how far the picture shifts at 1920x1080 in moves that run to completion:
 * 2.37 px per pan step and 2.45 per tilt step, after a dead band of about 25 steps
 * whenever a direction reverses (gear backlash). An ONVIF FOV-relative move of 1
 * brings the frame's edge to its centre, which is how Frigate's autotracker aims. */
#define MANUAL_LEASE_US UINT64_C(1200000)
#define NUDGE_US UINT64_C(350000)
#define PRESET_LIMIT_US UINT64_C(45000000)
#define HOME_STAGE_US UINT64_C(60000000)
#define HOME_SETTLE_US UINT64_C(1000000)
#define POSITION_QUIET_US UINT64_C(5000000)
/* Per boot (tmpfs): a daemon restart or reload must not sweep the head again. */
#ifndef HOMED_MARKER
#define HOMED_MARKER "/tmp/jooanipc-open.homed"
#endif
#ifndef MOTOR_INFO
#define MOTOR_INFO "/proc/jz/motor/motor_info"
#endif

typedef struct { int32_t x, y, moving, speed; } motor_position;
typedef struct { int valid, x, y; char name[65]; } saved_preset;
typedef struct {
    jipc_app *app;
    pthread_mutex_t mutex;
    pthread_t worker;
    int replay, quitting, motor_fd, x, y, known, moving, chunk_active, faulted;
    int vx, vy, goto_active, target_x, target_y, manual_sent, nudge;
    int pan_max, tilt_max, rate;
    uint64_t generation;
    int pan_sign, tilt_sign;
    int night, ir_off, white_light, pilot_led, speaker_enable, speaker_used, gpio_used;
    uint64_t lease_until, goto_until, replay_previous;
    double replay_x, replay_y;
    saved_preset presets[PRESET_COUNT];
    char path[640], last_error[192];
    /* homing: 0 idle, 1 pan sweep into the end stop, 2 driver reset. */
    int homing, homed, saved_valid, saved_x, saved_y;
    /* Only a calibrated position may become the next boot's return target. */
    int position_trusted;
    uint64_t homing_until, homing_settle, position_quiet_until, homed_at;
    /* goto speed (0: the preset speed); a patrol's stops are not saved as the
     * last position, and only someone else's moves pause the patrol. */
    int goto_rate, patrol_move;
    uint64_t manual_at;
    /* Privacy: where the head waits (x -1: stays where it is), and where it goes back to. */
    int privacy, privacy_x, privacy_y, return_valid, return_x, return_y;
    char position_path[640];
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
        /* The infrared LEDs follow night mode unless ir_mode turned them off
         * (a lit street at night: the IR-cut filter opens, the LEDs stay dark). */
        if (!rc && gpio_output(59, night && !state->ir_off)) { rc = -1; saved = errno; }
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
    if (position.x < 0 || position.x > state->pan_max + 100 ||
        position.y < 0 || position.y > state->tilt_max + 100 ||
        position.moving < 0 || position.moving > 1) {
        errno = EPROTO;
        return -1;
    }
    state->x = clamp(position.x, 0, state->pan_max);
    state->y = clamp(position.y, 0, state->tilt_max);
    state->moving = position.moving;
    state->known = 1;
    return 0;
}

static int motor_limits(platform_state *state)
{
    FILE *file = fopen(MOTOR_INFO, "r");
    if (!file) return -1;
    char line[160]; int axis = -1, limits[2] = {0};
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "Horizontal motor")) axis = 0;
        else if (strstr(line, "Vertical motor")) axis = 1;
        else if (axis >= 0) { int n; if (sscanf(line, "max steps %d", &n) == 1) limits[axis] = n; }
    }
    int bad = ferror(file); fclose(file);
    if (bad || limits[0] < 100 || limits[0] > 8600 || limits[1] < 100 || limits[1] > 2000) {
        errno = EPROTO; return -1;
    }
    state->pan_max = limits[0]; state->tilt_max = limits[1];
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
    if (motor_limits(state) || ioctl(state->motor_fd, MOTOR_SPEED, &speed) || motor_read(state)) {
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
    state->manual_sent = state->nudge = 0;
    ++state->generation;
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

/* Last position, so a homed boot returns to the view it had. Written once the
 * head has rested at a new place, not per move: /opt is small JFFS2. */
static void position_load(platform_state *state)
{
    FILE *file = fopen(state->position_path, "r");
    int x, y;
    if (!file) return;
    if (fscanf(file, "%d %d", &x, &y) == 2 && x >= 0 && x <= PAN_MAX && y >= 0 && y <= TILT_MAX) {
        state->saved_x = x; state->saved_y = y; state->saved_valid = 1;
    }
    fclose(file);
}
static void position_save(platform_state *state)
{
    char text[32];
    int n = snprintf(text, sizeof(text), "%d %d\n", state->x, state->y);
    if (jipc_atomic_file(state->position_path, text, (size_t)n, 0600)) {
        jipc_log("warning", "platform: cannot save last position: %s", strerror(errno)); return;
    }
    state->saved_x = state->x; state->saved_y = state->y; state->saved_valid = 1;
}
static void position_track(platform_state *state, uint64_t now)
{
    if (state->replay || !state->known || !state->position_trusted || state->faulted ||
        state->homing || state->moving || state->goto_active || state->privacy || state->patrol_move ||
        state->lease_until || (state->saved_valid && state->x == state->saved_x && state->y == state->saved_y)) {
        state->position_quiet_until = 0; return;
    }
    if (!state->position_quiet_until) { state->position_quiet_until = now + POSITION_QUIET_US; return; }
    if (now < state->position_quiet_until) return;
    state->position_quiet_until = 0;
    position_save(state);
}
/* The OEM sequence: declare the head centred, run pan into its end stop,
 * then the driver's own reset rams both axes and returns to the reset point. */
static int motor_home_begin(platform_state *state, uint64_t now)
{
    int32_t speed = HOME_SPEED, sweep[2] = {HOME_PAN_STEPS / 2, 0};
    int32_t reset[4] = {HOME_PAN_STEPS, HOME_TILT_STEPS, HOME_PAN_STEPS / 2, HOME_TILT_RESET};
    if (motor_stop_motion(state) || ioctl(state->motor_fd, MOTOR_SPEED, &speed) ||
        ioctl(state->motor_fd, MOTOR_RESET, reset) || ioctl(state->motor_fd, MOTOR_MOVE, sweep)) return -1;
    state->homing = 1; state->homed = 0; state->moving = 1;
    state->homing_until = now + HOME_STAGE_US; state->homing_settle = now + HOME_SETTLE_US;
    jipc_log("info", "motor homing: pan sweep into the end stop");
    return 0;
}
static void home_tick(platform_state *state, uint64_t now)
{
    if (now >= state->homing_until) {
        state->homing = 0; errno = ETIMEDOUT; worker_error(state, "motor homing"); return;
    }
    if (now < state->homing_settle) return;
    if (motor_read(state)) { state->homing = 0; worker_error(state, "motor homing status"); return; }
    if (state->moving) return;
    /* The pan run ends at the declared maximum. The driver's reset, like the
     * OEM's, is complete once idle: it ends at 2040,1199 on this board, not
     * at the 2040,200 it logs as its reset point (observed on hardware). */
    if (state->homing == 1 && state->x != HOME_PAN_STEPS) return;
    if (state->homing == 1) {
        int32_t reset[4] = {0, 0, 0, 0};
        state->homing = 2; state->moving = 1;
        jipc_log("info", "motor homing: driver reset to the reference point");
        /* The driver's reset does not return until the head has run both axes,
         * 20 s or more. Nobody else may wait that long for the status: everything
         * that would move the head or read the driver checks homing first. */
        pthread_mutex_unlock(&state->mutex);
        int failed = ioctl(state->motor_fd, MOTOR_RESET, reset), saved = errno;
        pthread_mutex_lock(&state->mutex);
        errno = saved;
        if (failed) { state->homing = 0; worker_error(state, "motor reset"); return; }
        state->homing_until = now + HOME_STAGE_US; state->homing_settle = now + HOME_SETTLE_US;
        return;
    }
    if (ioctl(state->motor_fd, MOTOR_STOP, 0) || motor_limits(state) || motor_read(state)) {
        state->homing = 0; worker_error(state, "motor homing completion"); return;
    }
    state->homing = 0; state->homed = 1; state->position_trusted = 1; state->homed_at = now;
    int marker = open(HOMED_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) close(marker);
    jipc_log("info", "motor homing complete at %d,%d (limits %d,%d)", state->x, state->y, state->pan_max, state->tilt_max);
    if (state->privacy) {
        state->target_x = state->privacy_x < 0 ? state->x : clamp(state->privacy_x, 0, state->pan_max);
        state->target_y = clamp(state->privacy_y, 0, state->tilt_max);
        state->goto_active = 1; state->goto_rate = 0; state->goto_until = now + PRESET_LIMIT_US;
    } else if (state->saved_valid && (state->saved_x != state->x || state->saved_y != state->y)) {
        state->target_x = clamp(state->saved_x, 0, state->pan_max);
        state->target_y = clamp(state->saved_y, 0, state->tilt_max);
        state->goto_active = 1; state->goto_rate = 0; state->goto_until = now + PRESET_LIMIT_US;
        jipc_log("info", "motor returning to last position %d,%d", state->target_x, state->target_y);
    }
}

/* One finite displacement stays energized until Stop or completion. Never
 * restart a manual job at each worker tick: natural completion drops the coils.
 * Request3 also restores the driver's axis bookkeeping before a Y move. */
static void native_tick(platform_state *state, uint64_t now)
{
    int32_t move[2] = {0};
    if (state->homing && state->motor_fd >= 0) { home_tick(state, now); return; }
    if (state->motor_fd < 0 || state->faulted ||
        (!state->goto_active && !state->lease_until && !state->chunk_active)) return;
    if (state->goto_active && now >= state->goto_until) {
        errno = ETIMEDOUT; worker_error(state, "motor preset exceeded 45 seconds"); return;
    }
    if (state->lease_until && now >= state->lease_until) {
        if (motor_stop_motion(state)) worker_error(state, "motor deadman stop");
        return;
    }
    if (motor_read(state)) { worker_error(state, "motor status"); return; }
    if (state->chunk_active) {
        if (state->moving) return;
        /* Even natural completion can leave the module's axis-switch flag set. */
        if (ioctl(state->motor_fd, MOTOR_STOP, 0) || motor_read(state)) {
            worker_error(state, "normalize completed motor move"); return;
        }
        state->chunk_active = 0;
    }
    if (state->goto_active) {
        move[0] = state->target_x - state->x;
        if (!move[0]) move[1] = state->target_y - state->y;
        if (!move[0] && !move[1]) { (void)motor_stop_motion(state); return; }
    } else if (state->lease_until && !state->manual_sent) {
        /* Enough steps for the lease plus 100 ms, bounded even if userspace
         * stalls. A nudge is one 350 ms pulse at the old helper's OEM speed. */
        int budget = state->nudge ? (int)((uint64_t)state->rate * (state->lease_until - now + 100000) / 1000000) + 1 :
            (state->vx ? state->pan_max : state->tilt_max);
        if (state->vx) move[0] = clamp(state->x + (state->vx < 0 ? -budget : budget), 0, state->pan_max) - state->x;
        else if (state->vy) move[1] = clamp(state->y + (state->vy < 0 ? -budget : budget), 0, state->tilt_max) - state->y;
        state->manual_sent = 1;
    } else if (state->manual_sent) {
        (void)motor_stop_motion(state); return;
    }
    if (!move[0] && !move[1]) { (void)motor_stop_motion(state); return; }
    int32_t speed = state->goto_active ? (state->goto_rate ? state->goto_rate : MOTOR_RATE) : state->rate;
    if (ioctl(state->motor_fd, MOTOR_SPEED, &speed) || ioctl(state->motor_fd, MOTOR_MOVE, move)) {
        worker_error(state, "motor move"); return;
    }
    state->moving = state->chunk_active = 1;
}

static void *platform_worker(void *argument)
{
    platform_state *state = argument;
    for (;;) {
        pthread_mutex_lock(&state->mutex);
        uint64_t now = jipc_now_us();
        if (state->quitting || state->app->stop) {
            (void)motor_stop_motion(state);
            pthread_mutex_unlock(&state->mutex);
            return NULL;
        }
        if (state->replay) replay_tick(state, now);
        else { native_tick(state, now); position_track(state, now); }
        pthread_mutex_unlock(&state->mutex);
        jipc_sleep_ms(10);
    }
}

static int preset_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > 64) return 0;
    /* Validate printable UTF-8 (and XML 1.0 scalars), not arbitrary bytes. */
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)name[i++]; unsigned cp = c, extra = 0, min = 0;
        if (c < 32 || c == 127) return 0;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; extra = 1; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; extra = 2; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; extra = 3; min = 0x10000; }
        else return 0;
        while (extra--) { if (i == n || ((unsigned char)name[i] & 0xc0) != 0x80) return 0; cp = (cp << 6) | (name[i++] & 63); }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp == 0xfffe || cp == 0xffff) return 0;
    }
    return 1;
}
static int coordinate_valid(platform_state *state, int x, int y)
{
    return state->replay ? x >= -100 && x <= 100 && y >= -100 && y <= 100 :
        x >= 0 && x <= 8600 && y >= 0 && y <= 2000;
}
static int json_integer(cJSON *o, const char *key, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < -10000 ||
        v->valuedouble > 10000 || floor(v->valuedouble) != v->valuedouble) return -1;
    *out = (int)v->valuedouble; return 0;
}
static int unique_keys(cJSON *o)
{
    for (cJSON *a = o->child; a; a = a->next)
        for (cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return 0;
    return 1;
}
static int presets_load(platform_state *state)
{
    char text[8192], backend[16], extra, legacy[640]; struct stat status;
    int old = 0, fd = open(state->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        jipc_config config; jipc_config_copy(state->app, &config);
        snprintf(legacy, sizeof(legacy), "%s/presets.v1", config.state_dir);
        fd = open(legacy, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); old = 1;
    }
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_size >= (off_t)sizeof(text)) { close(fd); return -1; }
    size_t size = 0;
    while (size < (size_t)status.st_size) {
        ssize_t n = read(fd, text + size, (size_t)status.st_size - size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        size += (size_t)n;
    }
    close(fd); text[size] = 0;
    if (memchr(text, 0, size)) return -1;
    if (old) {
        char *save, *line = strtok_r(text, "\n", &save); int slot, x, y;
        if (!line || sscanf(line, "JIPC_PRESETS_1 %15s %c", backend, &extra) != 1 ||
            strcmp(backend, state->replay ? "replay" : "imp")) return -1;
        while ((line = strtok_r(NULL, "\n", &save))) {
            if (sscanf(line, "%d %d %d %c", &slot, &x, &y, &extra) != 3 || slot < 1 || slot > PRESET_COUNT ||
                !coordinate_valid(state, x, y) || state->presets[slot-1].valid) return -1;
            saved_preset *p = &state->presets[slot-1]; *p = (saved_preset){.valid=1,.x=x,.y=y};
            snprintf(p->name, sizeof(p->name), "Preset %d", slot);
        }
        return 0;
    }
    /* Never resurrect v1 when v2 is invalid or intentionally empty. */
    for (size_t i = 0; i < size; ++i) if (text[i] == '\\') {
        if (!strncmp(text + i + 1, "u0000", 5)) return -1;
        ++i; /* An escaped backslash is literal, not a Unicode escape. */
    }
    cJSON *root = cJSON_ParseWithLengthOpts(text, size + 1, NULL, 1), *entries, *v;
    int version, rc = -1;
    if (!cJSON_IsObject(root) || !unique_keys(root) || json_integer(root, "version", &version) || version != 2) goto done;
    v = cJSON_GetObjectItemCaseSensitive(root, "backend");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, state->replay ? "replay" : "imp")) goto done;
    v = cJSON_GetObjectItemCaseSensitive(root, "coordinate_space");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, state->replay ? "normalized" : "motor_steps")) goto done;
    entries = cJSON_GetObjectItemCaseSensitive(root, "presets");
    if (!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) > PRESET_COUNT) goto done;
    cJSON_ArrayForEach(v, entries) {
        int slot, x, y; cJSON *name = cJSON_GetObjectItemCaseSensitive(v, "name");
        if (!cJSON_IsObject(v) || !unique_keys(v) || json_integer(v, "slot", &slot) || slot < 1 || slot > PRESET_COUNT ||
            json_integer(v, "x", &x) || json_integer(v, "y", &y) || !coordinate_valid(state, x, y) ||
            !cJSON_IsString(name) || !preset_name_valid(name->valuestring) || state->presets[slot-1].valid) goto done;
        for (int i = 0; i < PRESET_COUNT; ++i)
            if (state->presets[i].valid && !strcasecmp(state->presets[i].name, name->valuestring)) goto done;
        saved_preset *p = &state->presets[slot-1]; *p = (saved_preset){.valid=1,.x=x,.y=y};
        strcpy(p->name, name->valuestring);
    }
    rc = 0;
done:
    cJSON_Delete(root); return rc;
}

static int presets_save(platform_state *state)
{
    char text[8192], name[400]; jipc_config config; size_t length; int n, first = 1;
    jipc_config_copy(state->app, &config);
    if (jipc_mkdirs(config.state_dir, 0700)) return -1;
    n = snprintf(text, sizeof(text), "{\"version\":2,\"backend\":\"%s\",\"coordinate_space\":\"%s\",\"presets\":[",
        state->replay ? "replay" : "imp", state->replay ? "normalized" : "motor_steps");
    length = (size_t)n;
    for (int i = 0; i < PRESET_COUNT; ++i) {
        saved_preset *p = &state->presets[i]; if (!p->valid) continue;
        if (jipc_json_escape(p->name, name, sizeof(name)) < 0) return -1;
        n = snprintf(text + length, sizeof(text) - length, "%s{\"slot\":%d,\"name\":%s,\"x\":%d,\"y\":%d}", first ? "" : ",", i+1, name, p->x, p->y);
        if (n < 0 || (size_t)n >= sizeof(text) - length - 3) { errno = EOVERFLOW; return -1; }
        length += (size_t)n; first = 0;
    }
    memcpy(text + length, "]}\n", 3); length += 3;
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
    state->pan_sign = state->replay || !(config.flip0 & 1) ? 1 : -1;
    state->tilt_sign = state->replay || !(config.flip0 & 2) ? 1 : -1;
    state->motor_fd = -1;
    state->known = state->replay;
    state->pan_max = PAN_MAX; state->tilt_max = TILT_MAX;
    state->night = state->white_light = state->pilot_led = state->speaker_enable = -1;
    state->replay_previous = jipc_now_us();
    if (snprintf(state->path, sizeof(state->path), "%s/presets.v2.json", config.state_dir)
        >= (int)sizeof(state->path) ||
        snprintf(state->position_path, sizeof(state->position_path), "%s/position", config.state_dir)
        >= (int)sizeof(state->position_path)) {
        free(state); fail(error, capacity, "state path is too long"); return NULL;
    }
    position_load(state);
    /* With motor_home, the position is calibrated once this boot was homed
     * (by this daemon or the OEM bootstrap before a handoff). */
    state->position_trusted = state->replay || !config.motor_home || !access(HOMED_MARKER, F_OK);
    rc = pthread_mutex_init(&state->mutex, NULL);
    if (rc) { free(state); fail(error, capacity, "platform mutex: %s", strerror(rc)); return NULL; }
    if (presets_load(state)) {
        pthread_mutex_destroy(&state->mutex); free(state);
        fail(error, capacity, "invalid preset catalog; restore a valid private catalog"); return NULL;
    }
    rc = pthread_create(&state->worker, NULL, platform_worker, state);
    if (rc) {
        pthread_mutex_destroy(&state->mutex);
        free(state); fail(error, capacity, "platform thread: %s", strerror(rc)); return NULL;
    }
    app->platform_state = state;
    return state;
}

static int platform_control(jipc_app *app, const char *operation, int value,
                          int value2, const char *name, uint64_t *generation, int *allocated_slot, char *error, size_t capacity)
{
    platform_state *state;
    int rc = 0, motion, preset_op;
    saved_preset previous;
    if (error && capacity) error[0] = 0;
    if (!app || !operation) return fail(error, capacity, "invalid platform request");
    motion = !strcmp(operation, "ptz") || !strcmp(operation, "ptz_nudge");
    preset_op = !strcmp(operation, "preset_set") || !strcmp(operation, "preset_goto") ||
                !strcmp(operation, "preset_remove") || !strcmp(operation, "preset_rename") ||
                !strcmp(operation, "preset_patrol");
    int recalibrate = !strcmp(operation, "ptz_recalibrate");
    if (!motion && !preset_op && !recalibrate && strcmp(operation, "ptz_stop") &&
        strcmp(operation, "night") && strcmp(operation, "ircut") &&
        strcmp(operation, "white_light") && strcmp(operation, "pilot_led") &&
        strcmp(operation, "speaker_enable") && strcmp(operation, "ir_mode"))
        return fail(error, capacity, "unsupported platform operation: %s", operation);
    if (motion && (value < -100 || value > 100 || value2 < -100 || value2 > 100))
        return fail(error, capacity, "PTZ velocity must be between -100 and 100");
    if (preset_op && (value < 1 || value > PRESET_COUNT) &&
        !(allocated_slot && !strcmp(operation,"preset_set") && value == 0))
        return fail(error, capacity, "preset slot must be between 1 and 16");
    if ((!strcmp(operation, "night") || !strcmp(operation, "ircut")) && value == 2)
        return fail(error, capacity, "automatic day/night requires an unverified sensor exposure ABI; select day=0 or night=1");
    if (!motion && !preset_op && strcmp(operation, "ptz_stop") && (value < 0 || value > 1))
        return fail(error, capacity, "control value must be 0 or 1");
    if (name && !preset_name_valid(name)) return fail(error, capacity, "preset name requires 1..64 printable UTF-8 bytes");
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, capacity);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    if (app->stop || state->quitting) { rc = fail(error, capacity, "application is stopping"); goto done; }
    if (allocated_slot && !strcmp(operation,"preset_set") && value == 0) {
        for (int i = 0; i < PRESET_COUNT; ++i) if (!state->presets[i].valid) { value = i+1; break; }
        if (!value) { rc = fail(error, capacity, "all preset slots are occupied"); goto done; }
    }
    if ((motion || recalibrate || (preset_op && strcmp(operation, "preset_remove") && strcmp(operation, "preset_rename"))) &&
        motor_open_device(state)) {
        rc = fail(error, capacity, "motor device: %s", strerror(errno)); goto done;
    }
    if (state->homing && (motion || recalibrate || (preset_op && strcmp(operation, "preset_rename")) || !strcmp(operation, "ptz_stop"))) {
        rc = fail(error, capacity, "pan/tilt homing in progress"); goto done;
    }
    if (state->privacy && (motion || recalibrate || !strcmp(operation, "preset_goto") ||
                           !strcmp(operation, "preset_patrol") || !strcmp(operation, "preset_set"))) {
        rc = fail(error, capacity, "privacy mode is on"); goto done;
    }
    if (motion) {
        int vx=value*state->pan_sign, vy=value2*state->tilt_sign;
        int nudge = !strcmp(operation, "ptz_nudge");
        if (abs(vx) >= abs(vy)) vy = 0; else vx = 0;
        /* Coalesce a concurrent same-direction nudge without extending it. */
        int same = state->nudge == nudge && state->vx == vx && state->vy == vy && state->lease_until;
        if (same && !nudge) state->lease_until = jipc_now_us() + MANUAL_LEASE_US;
        if (!same) {
            if (motor_stop_motion(state)) { rc = fail(error, capacity, "stop previous motion: %s", strerror(errno)); goto done; }
            state->vx = vx; state->vy = vy; state->nudge = nudge;
            state->rate = nudge ? NUDGE_RATE : clamp((abs(vx) + abs(vy)) * 9, 100, 900);
            state->lease_until = (vx || vy) ? jipc_now_us() + (nudge ? NUDGE_US : MANUAL_LEASE_US) : 0;
        }
        state->last_error[0] = 0; state->faulted = 0;
        state->manual_at = jipc_now_us(); state->patrol_move = 0;
        if (generation) *generation = state->generation;
    } else if (!strcmp(operation, "ptz_stop")) {
        /* In privacy mode the only motion is the head on its way to the pose. */
        if (!state->privacy) {
            if (motor_stop_motion(state)) rc = fail(error, capacity, "motor stop: %s", strerror(errno));
            state->manual_at = jipc_now_us(); state->patrol_move = 0; /* someone chose to stop it: steering */
        }
    } else if (!strcmp(operation, "preset_set")) {
        if (motor_stop_motion(state) || motor_read(state)) { rc = fail(error, capacity, "motor position: %s", strerror(errno)); goto done; }
        previous = state->presets[value - 1];
        char effective[65];
        if (name) strcpy(effective, name);
        else if (previous.valid) strcpy(effective, previous.name);
        else snprintf(effective, sizeof(effective), "Preset %d", value);
        for (int i = 0; i < PRESET_COUNT; ++i)
            if (i != value - 1 && state->presets[i].valid && !strcasecmp(effective, state->presets[i].name)) {
                rc = fail(error, capacity, "preset name already exists"); goto done;
            }
        saved_preset *preset = &state->presets[value - 1];
        preset->valid = 1; preset->x = state->x; preset->y = state->y;
        strcpy(preset->name, effective);
        if (presets_save(state)) {
            state->presets[value - 1] = previous;
            rc = fail(error, capacity, "save preset: %s", strerror(errno));
        }
    } else if (!strcmp(operation, "preset_rename")) {
        /* The name only; the saved position stays where it is. */
        if (!name) { rc = fail(error, capacity, "preset_rename requires a name"); goto done; }
        if (!state->presets[value - 1].valid) { rc = fail(error, capacity, "preset slot is empty"); goto done; }
        for (int i = 0; i < PRESET_COUNT; ++i)
            if (i != value - 1 && state->presets[i].valid && !strcasecmp(name, state->presets[i].name)) {
                rc = fail(error, capacity, "preset name already exists"); goto done;
            }
        /* This writes flash with the lock held, and only a manual move's lease stops it. */
        if (state->lease_until) { rc = fail(error, capacity, "the head is being steered: try again"); goto done; }
        previous = state->presets[value - 1];
        strcpy(state->presets[value - 1].name, name);
        if (presets_save(state)) {
            state->presets[value - 1] = previous;
            rc = fail(error, capacity, "rename preset: %s", strerror(errno));
        }
    } else if (!strcmp(operation, "preset_remove")) {
        /* Persistence cannot hold a live manual deadline behind flash I/O. In privacy
         * mode there is none, and the head must get to its pose. */
        if (!state->privacy && motor_stop_motion(state)) { rc = fail(error, capacity, "motor stop failed"); goto done; }
        previous = state->presets[value - 1];
        state->presets[value - 1].valid = 0;
        if (presets_save(state)) {
            state->presets[value - 1] = previous;
            rc = fail(error, capacity, "remove preset: %s", strerror(errno));
        }
    } else if (recalibrate) {
        /* The same run as at boot, then back to where the head is now, saved in
         * case the camera restarts first. */
        if (state->replay) { state->homed = 1; state->homed_at = jipc_now_us(); goto done; }
        if (!motor_read(state) && state->position_trusted && (!state->saved_valid || state->saved_x != state->x || state->saved_y != state->y))
            position_save(state);
        state->last_error[0] = 0; state->faulted = 0; state->patrol_move = 0; state->manual_at = jipc_now_us();
        /* These steps stop meaning anything from here until the run ends. A restart meanwhile
         * homes again instead of trusting them. */
        (void)unlink(HOMED_MARKER); state->position_trusted = 0;
        if (motor_home_begin(state, jipc_now_us())) { int saved = errno; worker_error(state, "motor homing start"); rc = fail(error, capacity, "motor homing: %s", strerror(saved)); }
    } else if (!strcmp(operation, "preset_goto") || !strcmp(operation, "preset_patrol")) {
        saved_preset *preset = &state->presets[value - 1];
        if (!preset->valid) { rc = fail(error, capacity, "preset slot is empty"); goto done; }
        if (!state->replay && (preset->x > state->pan_max || preset->y > state->tilt_max)) {
            rc = fail(error, capacity, "preset exceeds calibrated motor travel"); goto done;
        }
        if (motor_stop_motion(state)) { rc = fail(error, capacity, "motor stop: %s", strerror(errno)); goto done; }
        state->target_x = preset->x;
        state->target_y = preset->y;
        state->goto_until = jipc_now_us() + PRESET_LIMIT_US;
        state->goto_active = 1; state->goto_rate = 0;
        state->patrol_move = !strcmp(operation, "preset_patrol");
        if (!state->patrol_move) state->manual_at = jipc_now_us();
        state->last_error[0] = 0;
        state->faulted = 0;
    } else if (!strcmp(operation, "ir_mode")) {
        state->ir_off = value;
        if (!state->replay && state->night == 1 && gpio_output(59, !value)) /* -1: nothing applied yet */
            rc = fail(error, capacity, "infrared GPIO: %s", strerror(errno));
    } else if (!strcmp(operation, "night") || !strcmp(operation, "ircut")) {
        pthread_mutex_unlock(&state->mutex);
        if (gpio_night(state, value)) rc = fail(error, capacity, "IR-cut/infrared GPIO: %s", strerror(errno));
        pthread_mutex_lock(&state->mutex);
    } else {
        int pin = !strcmp(operation, "white_light") ? 60 :
                  !strcmp(operation, "speaker_enable") ? 63 : 58;
        if (!state->replay) {
            if (pin == 63) state->speaker_used = 1;
            else state->gpio_used = 1;
        }
        /* The JA-A12 speaker amplifier is active-low (factory SpeakLevelEx=0,
         * and the platform's boot hook mutes the OEM siren by holding it high):
         * speaker_enable=1 drives GPIO63 low. */
        int level = pin == 63 ? !value : value;
        if (!state->replay && gpio_output(pin, level))
            rc = fail(error, capacity, "GPIO%d: %s", pin, strerror(errno));
        else if (pin == 60) state->white_light = value;
        else if (pin == 63) state->speaker_enable = value;
        else state->pilot_led = value;
    }
done:
    if (!rc && allocated_slot) *allocated_slot = value;
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

int jipc_platform_control(jipc_app *app, const char *operation, int value,
                          int value2, char *error, size_t capacity)
{ return platform_control(app, operation, value, value2, NULL, NULL, NULL, error, capacity); }

int jipc_platform_preset_create(jipc_app *app, int *slot, const char *name, char *error, size_t capacity)
{
    if (!slot) return fail(error, capacity, "missing preset slot result");
    return platform_control(app, "preset_set", 0, 0, name, NULL, slot, error, capacity);
}

int jipc_platform_preset_set(jipc_app *app, int slot, const char *name, char *error, size_t capacity)
{ return platform_control(app, "preset_set", slot, 0, name, NULL, NULL, error, capacity); }

int jipc_platform_preset_rename(jipc_app *app, int slot, const char *name, char *error, size_t capacity)
{ return platform_control(app, "preset_rename", slot, 0, name, NULL, NULL, error, capacity); }

int jipc_platform_nudge(jipc_app *app, int x, int y, char *error, size_t capacity)
{
    uint64_t generation = 0;
    if (platform_control(app, "ptz_nudge", x, y, NULL, &generation, NULL, error, capacity)) return -1;
    /* Do not keep either lifetime or state lock while waiting. Stop, another
     * direction, shutdown and the deadline worker can all preempt this call. */
    for (;;) {
        pthread_mutex_lock(&platform_lifetime);
        platform_state *state = app->platform_state;
        if (!state || app->stop) { pthread_mutex_unlock(&platform_lifetime); return 0; }
        pthread_mutex_lock(&state->mutex);
        int done = generation != state->generation || !state->lease_until;
        int faulted = state->faulted;
        if (faulted) fail(error, capacity, "%s", state->last_error);
        pthread_mutex_unlock(&state->mutex);
        pthread_mutex_unlock(&platform_lifetime);
        if (done || faulted) return faulted ? -1 : 0;
        jipc_sleep_ms(5);
    }
}

/* Exact moves. x and y are the ONVIF values: position, or a share of the travel,
 * or of the field of view, with +x to the right and +y up, or motor steps. */
int jipc_platform_move(jipc_app *app, int kind, double x, double y, double speed, char *error, size_t capacity)
{
    platform_state *state;
    int rc = 0;
    if (error && capacity) error[0] = 0;
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, capacity);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    if (app->stop || state->quitting) { rc = fail(error, capacity, "application is stopping"); goto done; }
    if (motor_open_device(state)) { rc = fail(error, capacity, "motor device: %s", strerror(errno)); goto done; }
    if (state->homing) { rc = fail(error, capacity, "pan/tilt homing in progress"); goto done; }
    if (state->privacy) { rc = fail(error, capacity, "privacy mode is on"); goto done; }
    if (motor_stop_motion(state)) { rc = fail(error, capacity, "motor stop: %s", strerror(errno)); goto done; }
    double tx, ty;
    int low = state->replay ? -100 : 0;
    int high_x = state->replay ? 100 : state->pan_max, high_y = state->replay ? 100 : state->tilt_max;
    if (kind == JIPC_MOVE_STEPS) { tx = x; ty = y; }
    else if (state->replay) {
        double scale = kind == JIPC_MOVE_RELATIVE_FOV ? 50 : 100;
        tx = (kind == JIPC_MOVE_ABSOLUTE ? 0 : state->x) + x * scale;
        ty = (kind == JIPC_MOVE_ABSOLUTE ? 0 : state->y) + y * scale;
    } else if (kind == JIPC_MOVE_ABSOLUTE) {
        tx = (x * state->pan_sign + 1) / 2 * state->pan_max;
        ty = (y * state->tilt_sign + 1) / 2 * state->tilt_max;
    } else {
        double half_x = kind == JIPC_MOVE_RELATIVE_FOV ? JIPC_FOV_HALF_PAN : state->pan_max / 2.0;
        double half_y = kind == JIPC_MOVE_RELATIVE_FOV ? JIPC_FOV_HALF_TILT : state->tilt_max / 2.0;
        tx = state->x + x * state->pan_sign * half_x;
        ty = state->y + y * state->tilt_sign * half_y;
    }
    state->target_x = clamp((int)lround(tx), low, high_x);
    state->target_y = clamp((int)lround(ty), low, high_y);
    state->goto_rate = speed > 0 ? clamp((int)lround(speed * 900), 100, 900) : 0;
    int rate = state->goto_rate ? state->goto_rate : MOTOR_RATE;
    uint64_t distance = (uint64_t)(abs(state->target_x - state->x) + abs(state->target_y - state->y));
    state->goto_until = jipc_now_us() + distance * 1000000u / (unsigned)rate + UINT64_C(10000000);
    state->goto_active = 1; state->patrol_move = 0; state->manual_at = jipc_now_us();
    state->last_error[0] = 0; state->faulted = 0;
done:
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

int jipc_platform_steps(jipc_app *app, int *x, int *y)
{
    pthread_mutex_lock(&platform_lifetime);
    platform_state *state = platform_get(app, NULL, 0);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    int rc = state->homing || motor_open_device(state) || motor_read(state) ? -1 : 0;
    *x = state->x; *y = state->y;
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return rc;
}

int jipc_platform_patrol_goto(jipc_app *app, int slot, char *error, size_t capacity)
{ return platform_control(app, "preset_patrol", slot, 0, NULL, NULL, NULL, error, capacity); }

int jipc_platform_motion(jipc_app *app, jipc_ptz_motion *motion)
{
    pthread_mutex_lock(&platform_lifetime);
    platform_state *state = platform_get(app, NULL, 0);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    motion->moving = state->moving || state->goto_active || state->vx || state->vy;
    motion->homing = state->homing;
    uint64_t now = jipc_now_us();
    motion->trusted = state->position_trusted;
    motion->manual_age_s = state->manual_at ? (long)((now - state->manual_at) / 1000000u) : -1;
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_unlock(&platform_lifetime);
    return 0;
}

/* Privacy: the head waits at a preset, or looking down at the end of its tilt,
 * and comes back afterwards. Moves from anyone else are refused meanwhile. */
int jipc_platform_privacy(jipc_app *app, int on, int slot, char *error, size_t capacity)
{
    platform_state *state;
    int rc = 0;
    if (error && capacity) error[0] = 0;
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, capacity);
    if (!state) { pthread_mutex_unlock(&platform_lifetime); return -1; }
    pthread_mutex_lock(&state->mutex);
    int was = state->privacy;
    if (on) {
        saved_preset *preset = slot >= 1 && slot <= PRESET_COUNT && state->presets[slot - 1].valid ? &state->presets[slot - 1] : NULL;
        state->privacy_x = preset ? preset->x : -1;
        /* Down is the far end of the tilt on an upside-down mount, and step 0 on an upright one. */
        state->privacy_y = preset ? preset->y : state->replay ? 100 : state->tilt_sign < 0 ? state->tilt_max : 0;
    }
    state->privacy = on;
    if (state->homing) goto done; /* homing's end goes to the right place */
    /* Where the head really belongs, read before the stop forgets it: where it was going,
     * if it was on its way there, is the view, not the point it had got to. */
    int going = state->goto_active && !state->patrol_move, going_x = state->target_x, going_y = state->target_y;
    if (motor_open_device(state) || motor_stop_motion(state)) { rc = fail(error, capacity, "motor: %s", strerror(errno)); goto done; }
    if (on) {
        int pose_x = state->privacy_x < 0 ? state->x : state->privacy_x;
        /* Already at the pose with nothing here remembering why: a daemon restart during
         * privacy. The way back is then the saved view, not the pose. */
        int at_pose = abs(state->x - pose_x) <= 2 && abs(state->y - state->privacy_y) <= 2;
        if (!was && going) { state->return_x = going_x; state->return_y = going_y; state->return_valid = 1; }
        else if (!was && at_pose && state->saved_valid) { state->return_x = state->saved_x; state->return_y = state->saved_y; state->return_valid = 1; }
        else if (!was && state->known && state->position_trusted) { state->return_x = state->x; state->return_y = state->y; state->return_valid = 1; }
        state->target_x = state->privacy_x < 0 ? state->x : state->privacy_x;
        state->target_y = state->privacy_y;
    } else if (was && state->return_valid) {
        state->target_x = state->return_x; state->target_y = state->return_y; state->return_valid = 0;
    } else if (was && state->saved_valid) {
        state->target_x = state->saved_x; state->target_y = state->saved_y;
    } else goto done;
    state->goto_rate = 0; state->patrol_move = 0; state->goto_until = jipc_now_us() + PRESET_LIMIT_US; state->goto_active = 1;
    state->last_error[0] = 0; state->faulted = 0;
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
    /* During homing the driver may be busy; the last known position will do. */
    if (!state->homing && (motor_open_device(state) || motor_read(state))) {
        fail(error, capacity, "motor status: %s", strerror(errno));
        goto done;
    }
    status->pan = state->replay ? state->x : state->pan_sign*(state->x * 200 / state->pan_max - 100);
    status->tilt = state->replay ? state->y : state->tilt_sign*(state->y * 200 / state->tilt_max - 100);
    status->pan_norm = state->replay ? state->x / 100.0 : state->pan_sign * (2.0 * state->x / state->pan_max - 1);
    status->tilt_norm = state->replay ? state->y / 100.0 : state->tilt_sign * (2.0 * state->y / state->tilt_max - 1);
    status->moving = state->moving || state->goto_active || state->vx || state->vy;
    status->night = state->night;
    status->white_light = state->white_light;
    for (index = 0; index < PRESET_COUNT; ++index)
        if (state->presets[index].valid) { status->preset_mask |= UINT32_C(1) << index;
            strcpy(status->preset_names[index], state->presets[index].name); }
    strcpy(status->error, state->last_error);
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
    x = state->replay ? state->x : state->pan_sign*(state->x * 200 / state->pan_max - 100);
    y = state->replay ? state->y : state->tilt_sign*(state->y * 200 / state->tilt_max - 100);
    if (jipc_json_escape(state->last_error, escaped, sizeof(escaped)) < 0)
        strcpy(escaped, "\"error encoding platform status\"");
    /* Unknown for a homing this daemon did not run (an earlier start this boot). */
    char homed_ago[24] = "null";
    if (state->homed_at) snprintf(homed_ago, sizeof homed_ago, "%llu", (unsigned long long)((jipc_now_us() - state->homed_at) / 1000000u));
    n = snprintf(out, capacity,
                 "{\"backend\":\"%s\",\"position_known\":%s,\"position_x\":%d,\"position_y\":%d,"
                 "\"moving\":%s,\"preset_motion\":%s,\"homing\":%s,\"homed\":%s,\"night\":%d,\"white_light\":%d,"
                 "\"privacy\":%s,\"homed_ago_s\":%s,"
                 "\"pilot_led\":%d,\"speaker_enable\":%d,\"manual_timeout_ms\":1200,\"last_error\":%s,\"presets\":[",
                 state->replay ? "replay" : "imp", state->known ? "true" : "false", x, y,
                 (state->moving || state->goto_active || state->vx || state->vy) ? "true" : "false",
                 state->goto_active ? "true" : "false", state->homing ? "true" : "false",
                 state->homed ? "true" : "false", state->night, state->white_light,
                 state->privacy ? "true" : "false", homed_ago,
                 state->pilot_led, state->speaker_enable, escaped);
    if (n < 0 || (size_t)n >= capacity) goto done;
    used = (size_t)n;
    for (index = 0; index < PRESET_COUNT; ++index) {
        saved_preset *preset = &state->presets[index];
        if (!preset->valid) continue;
        x = state->replay ? preset->x : state->pan_sign*(preset->x * 200 / state->pan_max - 100);
        y = state->replay ? preset->y : state->tilt_sign*(preset->y * 200 / state->tilt_max - 100);
        char name[400]; if (jipc_json_escape(preset->name, name, sizeof(name)) < 0) goto done;
        n = snprintf(out + used, capacity - used, "%s{\"slot\":%d,\"name\":%s,\"x\":%d,\"y\":%d}",
                     first ? "" : ",", index + 1, name, x, y);
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

/* Boot homing (motor_home=1) replaces the OEM bootstrap's calibration. It runs
 * once per boot, asynchronously in the worker; PTZ is refused until it ends. */
int jipc_platform_start(jipc_app *app)
{
    jipc_config config;
    platform_state *state;
    char error[192];
    int rc = 0;
    jipc_config_copy(app, &config);
    if (!config.motor_home) return 0;
    if (strcmp(config.backend, "imp")) { jipc_log("info", "motor homing skipped: %s backend", config.backend); return 0; }
    if (!access(HOMED_MARKER, F_OK)) { jipc_log("info", "motor already homed this boot"); return 0; }
    pthread_mutex_lock(&platform_lifetime);
    state = platform_get(app, error, sizeof(error));
    if (!state) { pthread_mutex_unlock(&platform_lifetime); jipc_log("error", "motor homing: %s", error); return -1; }
    pthread_mutex_lock(&state->mutex);
    if (motor_open_device(state) || motor_home_begin(state, jipc_now_us())) {
        rc = -1; worker_error(state, "motor homing start");
    }
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
    if (!state->replay && state->speaker_used) (void)gpio_output(63, 1); /* active-low: mute */
    if (state->motor_fd >= 0) close(state->motor_fd);
    pthread_mutex_destroy(&state->mutex);
    app->platform_state = NULL;
    free(state);
    pthread_mutex_unlock(&platform_lifetime);
}
