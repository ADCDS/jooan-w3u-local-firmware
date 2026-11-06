/* SPDX-License-Identifier: MIT
 * Camera controls that survive a restart (night mode and infrared, white
 * light, pilot LED, microphone mute, picture per lens, anti-flicker), kept
 * in state_dir/controls.json, and the engine that applies them: automatic
 * day/night from the ISP's exposure value, the night and white-light
 * schedules, and the timed white-light trigger.
 *
 * Lock order: app->control_mutex (taken by jipc_backend_control) before the
 * controls mutex. The engine never calls into the backend while holding its
 * own mutex.
 */
#define _GNU_SOURCE
#include "jipc.h"

#include <cJSON.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NIGHT_DAY, NIGHT_NIGHT, NIGHT_AUTO, NIGHT_SCHEDULE };
static const char *const night_names[] = {"day", "night", "auto", "schedule"};
static const char *const flicker_names[] = {"off", "50", "60"};

typedef struct { int brightness, contrast, saturation, sharpness, hue, denoise; } picture;
typedef struct {
    int night_mode, ir_off, night_ev, day_ev, wait_s, night_from, night_until;
    int white_light, wl_schedule, wl_from, wl_until, trigger_s;
    int pilot_led, mic_mute, anti_flicker, utc_offset_min;
    picture pic[2];
} settings;

typedef struct {
    jipc_app *app;
    pthread_t thread;
    int started;
    atomic_int quit;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    settings s;
    int dirty, applied, apply_picture;
    uint64_t dirty_since;
    int active_night, active_white; /* -1 = not applied yet */
    int ev, ev_valid;
    uint64_t ev_at, cond_since, trigger_until, switched_at, wl_override_until;
    int wl_override;
    char path[600], proc[256], reason[96], error[160];
} controls_state;

static pthread_mutex_t lifetime = PTHREAD_MUTEX_INITIALIZER;
#define EV_READ_US UINT64_C(5000000)
#define SAVE_DELAY_US UINT64_C(3000000)

static void defaults(settings *s)
{
    memset(s, 0, sizeof *s);
    s->night_mode = NIGHT_DAY;
    /* Placeholders until calibrated on the camera: EV rises as the scene
     * darkens; night above night_ev, day again below day_ev. */
    s->night_ev = 30000; s->day_ev = 15000; s->wait_s = 60;
    s->night_from = 18 * 60 + 30; s->night_until = 6 * 60;
    s->wl_from = 19 * 60; s->wl_until = 23 * 60; s->trigger_s = 120;
    for (int i = 0; i < 2; ++i)
        s->pic[i] = (picture){128, 128, 128, 128, 128, 128};
}

static int parse_hhmm(const char *text, int *minutes)
{
    int h, m, n = 0;
    if (!text || sscanf(text, "%2d:%2d%n", &h, &m, &n) != 2 || text[n] || h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    *minutes = h * 60 + m;
    return 0;
}

static void hhmm(int minutes, char out[6])
{
    unsigned m = (unsigned)minutes % 1440u;
    out[0] = (char)('0' + m / 600); out[1] = (char)('0' + m / 60 % 10);
    out[2] = ':'; out[3] = (char)('0' + m % 60 / 10); out[4] = (char)('0' + m % 10); out[5] = 0;
}

/* Inside [from, until), which may wrap past midnight. */
static int in_window(int now, int from, int until)
{
    if (from == until) return 0;
    return from < until ? now >= from && now < until : now >= from || now < until;
}

static int local_minutes(const settings *s)
{
    long long minutes = (long long)time(NULL) / 60 + s->utc_offset_min;
    minutes %= 1440;
    return (int)(minutes < 0 ? minutes + 1440 : minutes);
}

/* Seconds until the next from/until boundary of a window. */
static int next_boundary_s(const settings *s, int from, int until)
{
    time_t now = time(NULL);
    int seconds_today = (int)(((long long)now + s->utc_offset_min * 60LL) % 86400);
    if (seconds_today < 0) seconds_today += 86400;
    int best = 86400;
    for (int i = 0; i < 2; ++i) {
        int edge = (i ? until : from) * 60, wait = edge - seconds_today;
        if (wait <= 0) wait += 86400;
        if (wait < best) best = wait;
    }
    return best;
}

static int get_int(cJSON *object, const char *key, int low, int high, int *out, char *error, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return 0;
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)(int)item->valuedouble ||
        item->valueint < low || item->valueint > high) {
        snprintf(error, capacity, "%s must be a whole number from %d to %d", key, low, high);
        return -1;
    }
    *out = item->valueint;
    return 0;
}

static int get_bool(cJSON *object, const char *key, int *out, char *error, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return 0;
    if (!cJSON_IsBool(item)) { snprintf(error, capacity, "%s must be true or false", key); return -1; }
    *out = cJSON_IsTrue(item);
    return 0;
}

static int get_time(cJSON *object, const char *key, int *out, char *error, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return 0;
    if (!cJSON_IsString(item) || parse_hhmm(item->valuestring, out)) {
        snprintf(error, capacity, "%s must be a time like 18:30", key);
        return -1;
    }
    return 0;
}

static int get_name(cJSON *object, const char *key, const char *const *names, int count, int *out,
                    char *error, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return 0;
    if (cJSON_IsString(item))
        for (int i = 0; i < count; ++i)
            if (!strcmp(item->valuestring, names[i])) { *out = i; return 0; }
    snprintf(error, capacity, "%s has an unknown value", key);
    return -1;
}

/* Applies a JSON patch onto a copy of the settings; nothing changes on error. */
static int patch(settings *s, cJSON *root, int *picture_changed, char *error, size_t capacity)
{
    settings next = *s;
    static const char *const ir_names[] = {"night", "off"};
    cJSON *night = cJSON_GetObjectItemCaseSensitive(root, "night");
    if (night) {
        if (!cJSON_IsObject(night)) return snprintf(error, capacity, "night must be an object"), -1;
        if (get_name(night, "mode", night_names, 4, &next.night_mode, error, capacity) ||
            get_name(night, "ir", ir_names, 2, &next.ir_off, error, capacity) ||
            get_int(night, "night_ev", 1, 10000000, &next.night_ev, error, capacity) ||
            get_int(night, "day_ev", 1, 10000000, &next.day_ev, error, capacity) ||
            get_int(night, "wait_s", 1, 3600, &next.wait_s, error, capacity) ||
            get_time(night, "from", &next.night_from, error, capacity) ||
            get_time(night, "until", &next.night_until, error, capacity)) return -1;
        if (next.day_ev >= next.night_ev)
            return snprintf(error, capacity, "the day level must be below the night level"), -1;
    }
    cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "white_light");
    if (light) {
        if (!cJSON_IsObject(light)) return snprintf(error, capacity, "white_light must be an object"), -1;
        if (get_bool(light, "on", &next.white_light, error, capacity) ||
            get_int(light, "trigger_s", 5, 3600, &next.trigger_s, error, capacity)) return -1;
        cJSON *schedule = cJSON_GetObjectItemCaseSensitive(light, "schedule");
        if (schedule && (!cJSON_IsObject(schedule) ||
                         get_bool(schedule, "enabled", &next.wl_schedule, error, capacity) ||
                         get_time(schedule, "from", &next.wl_from, error, capacity) ||
                         get_time(schedule, "until", &next.wl_until, error, capacity))) {
            if (!error[0]) snprintf(error, capacity, "schedule must be an object");
            return -1;
        }
    }
    if (get_bool(root, "pilot_led", &next.pilot_led, error, capacity) ||
        get_bool(root, "mic_mute", &next.mic_mute, error, capacity) ||
        get_name(root, "anti_flicker", flicker_names, 3, &next.anti_flicker, error, capacity) ||
        get_int(root, "utc_offset_min", -720, 840, &next.utc_offset_min, error, capacity)) return -1;
    cJSON *pictures = cJSON_GetObjectItemCaseSensitive(root, "picture");
    if (pictures) {
        if (!cJSON_IsArray(pictures) || cJSON_GetArraySize(pictures) > 2)
            return snprintf(error, capacity, "picture must be a list of at most two lenses"), -1;
        for (int i = 0; i < cJSON_GetArraySize(pictures); ++i) {
            cJSON *p = cJSON_GetArrayItem(pictures, i);
            if (cJSON_IsNull(p)) continue;
            if (!cJSON_IsObject(p)) return snprintf(error, capacity, "picture entries must be objects"), -1;
            picture *q = &next.pic[i];
            if (get_int(p, "brightness", 0, 255, &q->brightness, error, capacity) ||
                get_int(p, "contrast", 0, 255, &q->contrast, error, capacity) ||
                get_int(p, "saturation", 0, 255, &q->saturation, error, capacity) ||
                get_int(p, "sharpness", 0, 255, &q->sharpness, error, capacity) ||
                get_int(p, "hue", 0, 255, &q->hue, error, capacity) ||
                get_int(p, "denoise", 0, 255, &q->denoise, error, capacity)) return -1;
        }
    }
    if (picture_changed)
        *picture_changed = memcmp(next.pic, s->pic, sizeof next.pic) || next.anti_flicker != s->anti_flicker;
    *s = next;
    return 0;
}

static char *settings_json(const settings *s, int with_version)
{
    char a[6], b[6], c[6], d[6];
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (with_version) cJSON_AddNumberToObject(root, "v", 1);
    cJSON *night = cJSON_AddObjectToObject(root, "night");
    hhmm(s->night_from, a); hhmm(s->night_until, b); hhmm(s->wl_from, c); hhmm(s->wl_until, d);
    if (night) {
        cJSON_AddStringToObject(night, "mode", night_names[s->night_mode]);
        cJSON_AddStringToObject(night, "ir", s->ir_off ? "off" : "night");
        cJSON_AddNumberToObject(night, "night_ev", s->night_ev);
        cJSON_AddNumberToObject(night, "day_ev", s->day_ev);
        cJSON_AddNumberToObject(night, "wait_s", s->wait_s);
        cJSON_AddStringToObject(night, "from", a);
        cJSON_AddStringToObject(night, "until", b);
    }
    cJSON *light = cJSON_AddObjectToObject(root, "white_light");
    if (light) {
        cJSON_AddBoolToObject(light, "on", s->white_light);
        cJSON_AddNumberToObject(light, "trigger_s", s->trigger_s);
        cJSON *schedule = cJSON_AddObjectToObject(light, "schedule");
        if (schedule) {
            cJSON_AddBoolToObject(schedule, "enabled", s->wl_schedule);
            cJSON_AddStringToObject(schedule, "from", c);
            cJSON_AddStringToObject(schedule, "until", d);
        }
    }
    cJSON_AddBoolToObject(root, "pilot_led", s->pilot_led);
    cJSON_AddBoolToObject(root, "mic_mute", s->mic_mute);
    cJSON_AddStringToObject(root, "anti_flicker", flicker_names[s->anti_flicker]);
    cJSON_AddNumberToObject(root, "utc_offset_min", s->utc_offset_min);
    cJSON *pictures = cJSON_AddArrayToObject(root, "picture");
    for (int i = 0; pictures && i < 2; ++i) {
        cJSON *p = cJSON_CreateObject();
        if (!p) break;
        cJSON_AddNumberToObject(p, "brightness", s->pic[i].brightness);
        cJSON_AddNumberToObject(p, "contrast", s->pic[i].contrast);
        cJSON_AddNumberToObject(p, "saturation", s->pic[i].saturation);
        cJSON_AddNumberToObject(p, "sharpness", s->pic[i].sharpness);
        cJSON_AddNumberToObject(p, "hue", s->pic[i].hue);
        cJSON_AddNumberToObject(p, "denoise", s->pic[i].denoise);
        cJSON_AddItemToArray(pictures, p);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static void load(controls_state *c)
{
    defaults(&c->s);
    FILE *file = fopen(c->path, "re");
    if (!file) return;
    char text[4096];
    size_t n = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[n] = 0;
    cJSON *root = cJSON_Parse(text);
    char error[160] = {0};
    if (!root || patch(&c->s, root, NULL, error, sizeof error))
        jipc_log("error", "controls: ignoring %s: %s", c->path, error[0] ? error : "not valid JSON");
    cJSON_Delete(root);
}

static void save(controls_state *c)
{
    char *text = settings_json(&c->s, 1);
    if (!text) return;
    size_t n = strlen(text);
    text[n] = '\n'; /* cJSON leaves room for the terminator only; write without it */
    if (jipc_atomic_file(c->path, text, n + 1, 0600))
        snprintf(c->error, sizeof c->error, "cannot save controls: %s", strerror(errno));
    cJSON_free(text);
}

/* /proc/jz/isp/isp-m0 has one "ISP INFO <n>" section per lens; lens 0 (the
 * pan/tilt lens) decides, as both lenses share the IR-cut filter and LEDs. */
static int read_ev(const char *path, int *ev)
{
    FILE *file = fopen(path, "re");
    if (!file) return -1;
    char line[256];
    int section = -1, found = -1;
    while (fgets(line, sizeof line, file)) {
        const char *info = strstr(line, "ISP INFO ");
        if (info) { section = atoi(info + 9); continue; }
        if (section == 0 && !strncmp(line, "ISP EV value:", 13)) {
            char *end;
            long value = strtol(line + 13, &end, 10);
            if (end != line + 13 && value >= 0 && value <= 100000000) { *ev = (int)value; found = 0; }
            break;
        }
    }
    fclose(file);
    return found;
}

static void set_error(controls_state *c, const char *what, const char *detail)
{
    pthread_mutex_lock(&c->mutex);
    snprintf(c->error, sizeof c->error, "%s: %s", what, detail && *detail ? detail : "refused");
    pthread_mutex_unlock(&c->mutex);
}

static void apply_picture(controls_state *c, const settings *s)
{
    static const char *const names[] = {"brightness", "contrast", "saturation", "sharpness", "hue", "denoise"};
    char error[160];
    for (unsigned lens = 0; lens < c->app->config.sensors && lens < 2; ++lens) {
        const int values[] = {s->pic[lens].brightness, s->pic[lens].contrast, s->pic[lens].saturation,
                              s->pic[lens].sharpness, s->pic[lens].hue, s->pic[lens].denoise};
        for (unsigned i = 0; i < 6; ++i) {
            error[0] = 0;
            if (jipc_backend_control(c->app, names[i], values[i], (int)lens, error, sizeof error) < 0)
                set_error(c, names[i], error);
        }
    }
    error[0] = 0;
    if (jipc_backend_control(c->app, "anti_flicker", s->anti_flicker, 0, error, sizeof error) < 0)
        set_error(c, "anti_flicker", error);
}

static void *engine(void *opaque)
{
    controls_state *c = opaque;
    jipc_app *app = c->app;
    while (!atomic_load(&c->quit) && !app->stop) {
        uint64_t now = jipc_now_us();
        pthread_mutex_lock(&c->mutex);
        if (!app->backend_ready) {
            pthread_mutex_unlock(&c->mutex);
            jipc_sleep_ms(200);
            continue;
        }
        int first = !c->applied, do_picture = first || c->apply_picture;
        c->applied = 1; c->apply_picture = 0;
        settings s = c->s;
        /* Exposure: only read while automatic mode needs it. */
        if (s.night_mode == NIGHT_AUTO && c->proc[0] && (!c->ev_at || now - c->ev_at >= EV_READ_US)) {
            int ev;
            c->ev_at = now;
            pthread_mutex_unlock(&c->mutex);
            int ok = !read_ev(c->proc, &ev);
            pthread_mutex_lock(&c->mutex);
            c->ev_valid = ok;
            if (ok) c->ev = ev;
            s = c->s;
        }
        int night = c->active_night;
        switch (s.night_mode) {
        case NIGHT_DAY: night = 0; snprintf(c->reason, sizeof c->reason, "day (chosen)"); break;
        case NIGHT_NIGHT: night = 1; snprintf(c->reason, sizeof c->reason, "night (chosen)"); break;
        case NIGHT_SCHEDULE:
            night = in_window(local_minutes(&s), s.night_from, s.night_until);
            snprintf(c->reason, sizeof c->reason, "schedule");
            break;
        default:
            if (!c->ev_valid) {
                if (night < 0) night = 0;
                snprintf(c->reason, sizeof c->reason, "no exposure reading; holding");
            } else if (night < 0) {
                night = c->ev >= (s.night_ev + s.day_ev) / 2;
                snprintf(c->reason, sizeof c->reason, "auto: started %s", night ? "dark" : "light");
            } else {
                int want = night ? c->ev <= s.day_ev : c->ev >= s.night_ev;
                if (!want) c->cond_since = 0;
                else if (!c->cond_since) c->cond_since = now;
                if (want && now - c->cond_since >= (uint64_t)s.wait_s * 1000000u) {
                    night = !night;
                    c->cond_since = 0;
                    snprintf(c->reason, sizeof c->reason, "auto: level %d %s %d for %d s", c->ev,
                             night ? ">=" : "<=", night ? s.night_ev : s.day_ev, s.wait_s);
                }
            }
        }
        /* White light: a trigger, then a manual change until the next
         * schedule boundary, then the schedule, then the manual state. */
        int white;
        if (c->trigger_until && now < c->trigger_until) white = 1;
        else {
            c->trigger_until = 0;
            if (s.wl_schedule && c->wl_override_until && now < c->wl_override_until) white = c->wl_override;
            else if (s.wl_schedule) { c->wl_override_until = 0; white = in_window(local_minutes(&s), s.wl_from, s.wl_until); }
            else white = s.white_light;
        }
        int change_night = night != c->active_night, change_white = white != c->active_white;
        int save_now = c->dirty && now - c->dirty_since >= SAVE_DELAY_US;
        if (save_now) c->dirty = 0;
        if (change_night) { c->active_night = night; c->switched_at = now; }
        if (change_white) c->active_white = white;
        if (save_now) save(c);
        pthread_mutex_unlock(&c->mutex);

        char error[160];
        if (first) {
            atomic_store(&app->mic_muted, s.mic_mute);
            error[0] = 0;
            if (jipc_platform_control(app, "pilot_led", s.pilot_led, 0, error, sizeof error) < 0)
                set_error(c, "pilot_led", error);
        }
        if (first || change_night) {
            error[0] = 0;
            if (jipc_platform_control(app, "ir_mode", s.ir_off, 0, error, sizeof error) < 0 ||
                jipc_backend_night(app, night, error, sizeof error) < 0) set_error(c, "night", error);
            else jipc_log("info", "controls: %s (%s)", night ? "night" : "day", c->reason);
        }
        if (first || change_white) {
            error[0] = 0;
            if (jipc_platform_control(app, "white_light", white, 0, error, sizeof error) < 0)
                set_error(c, "white_light", error);
        }
        if (do_picture) apply_picture(c, &s);

        pthread_mutex_lock(&c->mutex);
        if (!atomic_load(&c->quit) && !c->apply_picture) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            pthread_cond_timedwait(&c->wake, &c->mutex, &until);
        }
        pthread_mutex_unlock(&c->mutex);
    }
    /* A pending change is not lost at shutdown. */
    pthread_mutex_lock(&c->mutex);
    if (c->dirty) { c->dirty = 0; save(c); }
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

int jipc_controls_start(jipc_app *app)
{
    controls_state *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->app = app;
    c->active_night = c->active_white = -1;
    pthread_mutex_init(&c->mutex, NULL);
    pthread_cond_init(&c->wake, NULL);
    jipc_config config;
    jipc_config_copy(app, &config);
    snprintf(c->path, sizeof c->path, "%s/controls.json", config.state_dir);
    /* JIPC_ISP_PROC: a test hook in the environment, never a camera.ini key. */
    const char *proc = getenv("JIPC_ISP_PROC");
    snprintf(c->proc, sizeof c->proc, "%s", proc ? proc : strcmp(config.backend, "imp") ? "" : "/proc/jz/isp/isp-m0");
    load(c);
    pthread_mutex_lock(&lifetime);
    app->controls_state = c;
    pthread_mutex_unlock(&lifetime);
    if (pthread_create(&c->thread, NULL, engine, c)) {
        jipc_log("error", "controls: cannot start the engine thread");
        return 0;
    }
    c->started = 1;
    return 0;
}

void jipc_controls_stop(jipc_app *app)
{
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    app->controls_state = NULL;
    pthread_mutex_unlock(&lifetime);
    if (!c) return;
    atomic_store(&c->quit, 1);
    pthread_mutex_lock(&c->mutex);
    pthread_cond_broadcast(&c->wake);
    pthread_mutex_unlock(&c->mutex);
    if (c->started) pthread_join(c->thread, NULL);
    pthread_cond_destroy(&c->wake);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}

/* Called with app->control_mutex held by jipc_backend_control after a
 * successful manual control, so it survives a restart. */
void jipc_controls_note(jipc_app *app, const char *operation, int value, int value2)
{
    controls_state *c = app->controls_state;
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    settings before = c->s;
    picture *p = value2 >= 0 && value2 < 2 ? &c->s.pic[value2] : NULL;
    if ((!strcmp(operation, "night") || !strcmp(operation, "ircut")) && (value == 0 || value == 1)) {
        c->s.night_mode = value ? NIGHT_NIGHT : NIGHT_DAY;
        c->active_night = value;
    } else if (!strcmp(operation, "white_light")) {
        if (c->s.wl_schedule) {
            c->wl_override = value;
            c->wl_override_until = jipc_now_us() +
                (uint64_t)next_boundary_s(&c->s, c->s.wl_from, c->s.wl_until) * 1000000u;
        } else c->s.white_light = value;
        c->trigger_until = 0;
        c->active_white = value;
    } else if (!strcmp(operation, "pilot_led")) c->s.pilot_led = value;
    else if (!strcmp(operation, "mic_mute")) c->s.mic_mute = value;
    else if (!strcmp(operation, "anti_flicker")) c->s.anti_flicker = value;
    else if (p && !strcmp(operation, "brightness")) p->brightness = value;
    else if (p && !strcmp(operation, "contrast")) p->contrast = value;
    else if (p && !strcmp(operation, "saturation")) p->saturation = value;
    else if (p && !strcmp(operation, "sharpness")) p->sharpness = value;
    else if (p && !strcmp(operation, "hue")) p->hue = value;
    else if (p && !strcmp(operation, "denoise")) p->denoise = value;
    if (memcmp(&before, &c->s, sizeof before) && !c->dirty) { c->dirty = 1; c->dirty_since = jipc_now_us(); }
    pthread_mutex_unlock(&c->mutex);
}

int jipc_controls_patch(jipc_app *app, const char *json, char *error, size_t capacity)
{
    if (error && capacity) error[0] = 0;
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); snprintf(error, capacity, "controls are not running"); return -1; }
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root); pthread_mutex_unlock(&lifetime);
        snprintf(error, capacity, "the request must be a JSON object");
        return -1;
    }
    pthread_mutex_lock(&c->mutex);
    settings before = c->s;
    int picture_changed = 0;
    int rc = patch(&c->s, root, &picture_changed, error, capacity);
    if (!rc) {
        cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "white_light");
        if (light && cJSON_GetObjectItemCaseSensitive(light, "on")) c->wl_override_until = 0;
        if (c->s.night_mode != before.night_mode || c->s.night_ev != before.night_ev ||
            c->s.day_ev != before.day_ev) c->cond_since = 0;
        if (c->s.ir_off != before.ir_off) c->active_night = -1; /* re-apply with the new IR mode */
        if (picture_changed) c->apply_picture = 1;
        if (c->s.mic_mute != before.mic_mute) atomic_store(&app->mic_muted, c->s.mic_mute);
        if (memcmp(&before, &c->s, sizeof before)) { c->dirty = 1; c->dirty_since = jipc_now_us(); }
        pthread_cond_broadcast(&c->wake);
    }
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    cJSON_Delete(root);
    return rc;
}

/* The white light for `seconds` (0: the configured trigger_s), then back to
 * the schedule or the manual state: for Home Assistant on an NVR event. */
int jipc_controls_trigger(jipc_app *app, int seconds, char *error, size_t capacity)
{
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); snprintf(error, capacity, "controls are not running"); return -1; }
    if (seconds && (seconds < 1 || seconds > 3600)) {
        pthread_mutex_unlock(&lifetime);
        snprintf(error, capacity, "the light can stay on for 1 to 3600 seconds");
        return -1;
    }
    pthread_mutex_lock(&c->mutex);
    c->trigger_until = jipc_now_us() + (uint64_t)(seconds ? seconds : c->s.trigger_s) * 1000000u;
    pthread_cond_broadcast(&c->wake);
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    return 0;
}

/* Brightness, saturation, contrast, sharpness (0-255) of a lens, and the
 * night mode (0 day, 1 night, 2 auto, 3 schedule), for ONVIF imaging. */
int jipc_controls_picture(jipc_app *app, unsigned lens, int values[4], int *night_mode)
{
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c || lens > 1) { pthread_mutex_unlock(&lifetime); return -1; }
    pthread_mutex_lock(&c->mutex);
    const picture *p = &c->s.pic[lens];
    values[0] = p->brightness; values[1] = p->saturation; values[2] = p->contrast; values[3] = p->sharpness;
    *night_mode = c->s.night_mode;
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    return 0;
}

int jipc_controls_json(jipc_app *app, char *out, size_t capacity)
{
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); return -1; }
    pthread_mutex_lock(&c->mutex);
    char *base = settings_json(&c->s, 0);
    uint64_t now = jipc_now_us();
    cJSON *root = base ? cJSON_Parse(base) : NULL;
    cJSON_free(base);
    if (root) {
        cJSON *live = cJSON_AddObjectToObject(root, "live");
        if (live) {
            cJSON_AddStringToObject(live, "night", c->active_night < 0 ? "unknown" : c->active_night ? "night" : "day");
            cJSON_AddStringToObject(live, "reason", c->reason);
            if (c->ev_valid) cJSON_AddNumberToObject(live, "level", c->ev);
            else cJSON_AddNullToObject(live, "level");
            cJSON_AddNumberToObject(live, "since_s", c->switched_at ? (double)((now - c->switched_at) / 1000000u) : 0);
            cJSON_AddNumberToObject(live, "pending_s",
                c->cond_since ? (double)((now - c->cond_since) / 1000000u) : 0);
            cJSON_AddBoolToObject(live, "white_light", c->active_white == 1);
            cJSON_AddNumberToObject(live, "trigger_left_s",
                c->trigger_until && now < c->trigger_until ? (double)((c->trigger_until - now + 999999u) / 1000000u) : 0);
            cJSON_AddNumberToObject(live, "local_minutes", local_minutes(&c->s));
            cJSON_AddStringToObject(live, "error", c->error);
        }
    }
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    char *text = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    int rc = -1;
    if (text && strlen(text) < capacity) { strcpy(out, text); rc = 0; }
    cJSON_free(text);
    return rc;
}
