/* SPDX-License-Identifier: MIT
 * Camera controls that survive a restart (night mode and infrared, white
 * light, pilot LED, microphone mute, picture per lens, anti-flicker), kept
 * in state_dir/controls.json, and the engine that applies them: automatic
 * day/night from the ISP's exposure value, the night and white-light
 * schedules, the timed white-light trigger, the patrol and privacy mode.
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { NIGHT_DAY, NIGHT_NIGHT, NIGHT_AUTO, NIGHT_SCHEDULE };
static const char *const night_names[] = {"day", "night", "auto", "schedule"};
static const char *const flicker_names[] = {"off", "50", "60"};

/* No hue: the camera's ISP driver rejects V4L2 hue (BCSH) controls. */
typedef struct { int brightness, contrast, saturation, sharpness, denoise; } picture;
typedef struct { int slot, dwell_s; } patrol_stop;
enum { PATROL_STOPS = 16 };
enum { PATROL_GO, PATROL_MOVING, PATROL_DWELL };
typedef struct {
    int night_mode, ir_off, night_ev, day_ev, wait_s, night_from, night_until;
    int white_light, wl_schedule, wl_from, wl_until, trigger_s;
    int pilot_led, mic_mute, anti_flicker, utc_offset_min;
    int siren_sound, siren_volume, siren_seconds, siren_strobe;
    int overlay_on, overlay_corner, overlay_format;
    char overlay_name[JIPC_OVERLAY_NAME + 1];
    picture pic[2];
    int patrol_on, patrol_pause_s, patrol_count;
    patrol_stop stops[PATROL_STOPS];
    int privacy_on, privacy_schedule, privacy_from, privacy_until, privacy_slot;
    int privacy_held_until; /* "on" by hand under a schedule: a wall-clock time, so it survives a restart */
    int home_set, home_x, home_y;
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
    uint64_t ev_at, cond_since, trigger_until, strobe_until, switched_at, wl_override_until, auto_hold_us;
    uint64_t auto_settle_us, auto_settle_at;
    int auto_settling, auto_start, reapply_night, reapply_white, hold_factor;
    uint64_t privacy_since, own_light_at, retry_night_at, retry_white_at, save_failed_at, started_at, strobe_since, patrol_move_at;
    int clock_ok, schedules_ready, strobe_with_siren, engine_known, night_ok, network_time;
    const char *error_what; /* which work c->error is about: only that work clears it */
    uint64_t retry_picture_at;
    pthread_t engine_self;
    char clock_marker[256], ntp_config[256];
    uint64_t clock_wait_us;
    int wl_override;
    int zone_valid, zone_min;
    uint64_t zone_at;
    /* Privacy: what is applied, a manual change until the schedule's next edge,
     * and when the black-out was confirmed (frames are held until a moment later). */
    int active_privacy, privacy_override;
    uint64_t privacy_override_until, covered_at;
    /* Patrol: where it is and what it is doing, for the status too. */
    int patrol_index, patrol_phase, patrol_skipped;
    int overlay_shown;
    long long overlay_second;
    uint64_t patrol_until;
    const char *patrol_state;
    int patrol_left_s;
    char path[600], proc[256], oem_config[256], reason[96], error[160];
} controls_state;

static pthread_mutex_t lifetime = PTHREAD_MUTEX_INITIALIZER;
#define EV_READ_US UINT64_C(5000000)
#define SAVE_DELAY_US UINT64_C(3000000)
#define AUTO_HOLD_S 300 /* at least this long between automatic switches */
#define AUTO_SETTLE_S 10 /* the exposure right after video starts is not yet the scene's */
/* A switch to day that reverses soon after: the camera's own infrared is lighting something
 * near, and day is then near black. Stay in night longer each time, up to an hour. The hold
 * before going back to night stays short, because that is the state worth leaving. */
#define AUTO_HOLD_MAX_FACTOR 12
/* How long schedules wait for something to confirm the clock at a start. Privacy stays on
 * meanwhile, and the rest waits. With network time off nothing is coming, so briefly. With it
 * on, long enough for a router back from the same power cut to get its own time first. */
#define CLOCK_WAIT_US UINT64_C(120000000)
#define CLOCK_WAIT_NTP_US UINT64_C(1800000000)
#define RETRY_US UINT64_C(30000000)
/* From the black-out to letting frames through: what was in the pipe, and the switch to day. */
#define PRIVACY_SETTLE_US UINT64_C(2000000)

static void defaults(settings *s)
{
    memset(s, 0, sizeof *s);
    s->night_mode = NIGHT_DAY;
    s->siren_volume = 100; s->siren_seconds = 30;
    /* EV rises as the scene darkens: night above night_ev, day again below
     * day_ev. Measured at dawn on camera1: with the IR-cut filter open the
     * sky's infrared makes the level read about 3.6x lower than in day mode
     * at the same light, so day_ev must stay under night_ev / 3.6 or the
     * switch back to day immediately reads dark again and flaps. */
    s->night_ev = 30000; s->day_ev = 5000; s->wait_s = 60;
    s->night_from = 18 * 60 + 30; s->night_until = 6 * 60;
    s->wl_from = 19 * 60; s->wl_until = 23 * 60; s->trigger_s = 120;
    s->patrol_pause_s = 120;
    s->privacy_from = 22 * 60; s->privacy_until = 6 * 60;
    for (int i = 0; i < 2; ++i)
        s->pic[i] = (picture){128, 128, 128, 128, 128};
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

/* The camera's own time zone: the platform's PUT /api/v1/timezone stores it
 * as "GMT-03:00" in the vendor config (/IpcParam/TimeZomeCfg/GmtTz). */
static int camera_zone(const char *path, int *minutes)
{
    FILE *file = fopen(path, "re");
    if (!file) return -1;
    char text[256];
    int found = -1;
    while (fgets(text, sizeof text, file)) {
        const char *at = strstr(text, "\"GmtTz\"");
        if (!at || !(at = strstr(at + 7, "GMT"))) continue;
        char sign; int h, m;
        if (sscanf(at, "GMT%c%2d:%2d", &sign, &h, &m) == 3 && (sign == '+' || sign == '-') && h <= 14 && m <= 59) {
            *minutes = (sign == '-' ? -1 : 1) * (h * 60 + m);
            found = 0;
        }
        break;
    }
    fclose(file);
    return found;
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
    cJSON *patrol = cJSON_GetObjectItemCaseSensitive(root, "patrol");
    if (patrol) {
        if (!cJSON_IsObject(patrol)) return snprintf(error, capacity, "patrol must be an object"), -1;
        if (get_bool(patrol, "enabled", &next.patrol_on, error, capacity) ||
            get_int(patrol, "pause_s", 0, 3600, &next.patrol_pause_s, error, capacity)) return -1;
        cJSON *stops = cJSON_GetObjectItemCaseSensitive(patrol, "stops");
        if (stops) {
            if (!cJSON_IsArray(stops) || cJSON_GetArraySize(stops) > PATROL_STOPS)
                return snprintf(error, capacity, "stops must be a list of at most %d", PATROL_STOPS), -1;
            memset(next.stops, 0, sizeof next.stops);
            next.patrol_count = 0;
            cJSON *stop;
            cJSON_ArrayForEach(stop, stops) {
                patrol_stop *q = &next.stops[next.patrol_count];
                q->dwell_s = 10;
                if (!cJSON_IsObject(stop) || !cJSON_GetObjectItemCaseSensitive(stop, "slot"))
                    return snprintf(error, capacity, "each stop needs a slot"), -1;
                if (get_int(stop, "slot", 1, 16, &q->slot, error, capacity) ||
                    get_int(stop, "dwell_s", 3, 3600, &q->dwell_s, error, capacity)) return -1;
                next.patrol_count++;
            }
        }
    }
    cJSON *siren = cJSON_GetObjectItemCaseSensitive(root, "siren");
    if (siren) {
        if (!cJSON_IsObject(siren)) return snprintf(error, capacity, "siren must be an object"), -1;
        cJSON *sound = cJSON_GetObjectItemCaseSensitive(siren, "sound");
        if (sound) {
            int found = cJSON_IsString(sound) ? jipc_siren_sound(sound->valuestring) : -1;
            if (found < 0) return snprintf(error, capacity, "sound must be alarm, two_tone, pulse or chime"), -1;
            next.siren_sound = found;
        }
        if (get_int(siren, "volume", 1, 100, &next.siren_volume, error, capacity) ||
            get_int(siren, "seconds", 1, JIPC_SIREN_MAX_SECONDS, &next.siren_seconds, error, capacity) ||
            get_bool(siren, "strobe", &next.siren_strobe, error, capacity)) return -1;
    }
    cJSON *overlay = cJSON_GetObjectItemCaseSensitive(root, "overlay");
    if (overlay) {
        static const char *const corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
        static const char *const formats[] = {"ymd", "dmy", "mdy"};
        if (!cJSON_IsObject(overlay)) return snprintf(error, capacity, "overlay must be an object"), -1;
        if (get_bool(overlay, "enabled", &next.overlay_on, error, capacity)) return -1;
        cJSON *item = cJSON_GetObjectItemCaseSensitive(overlay, "position");
        if (item) {
            int found = -1;
            for (int i = 0; cJSON_IsString(item) && i < 4; ++i) if (!strcmp(item->valuestring, corners[i])) found = i;
            if (found < 0) return snprintf(error, capacity, "position must be top-left, top-right, bottom-left or bottom-right"), -1;
            next.overlay_corner = found;
        }
        item = cJSON_GetObjectItemCaseSensitive(overlay, "format");
        if (item) {
            int found = -1;
            for (int i = 0; cJSON_IsString(item) && i < 3; ++i) if (!strcmp(item->valuestring, formats[i])) found = i;
            if (found < 0) return snprintf(error, capacity, "format must be ymd, dmy or mdy"), -1;
            next.overlay_format = found;
        }
        item = cJSON_GetObjectItemCaseSensitive(overlay, "name");
        if (item) {
            int fits = cJSON_IsString(item) && strlen(item->valuestring) <= JIPC_OVERLAY_NAME;
            for (const char *p = fits ? item->valuestring : ""; *p; ++p) if (*p < 32 || *p > 126) fits = 0;
            if (!fits) return snprintf(error, capacity, "name must be plain text of at most %d characters", JIPC_OVERLAY_NAME), -1;
            snprintf(next.overlay_name, sizeof next.overlay_name, "%s", item->valuestring);
        }
    }
    cJSON *privacy = cJSON_GetObjectItemCaseSensitive(root, "privacy");
    if (privacy) {
        if (!cJSON_IsObject(privacy)) return snprintf(error, capacity, "privacy must be an object"), -1;
        if (get_bool(privacy, "on", &next.privacy_on, error, capacity) ||
            get_int(privacy, "slot", 0, 16, &next.privacy_slot, error, capacity) ||
            get_int(privacy, "held_until", 0, 2147483647, &next.privacy_held_until, error, capacity)) return -1;
        cJSON *schedule = cJSON_GetObjectItemCaseSensitive(privacy, "schedule");
        if (schedule && (!cJSON_IsObject(schedule) ||
                         get_bool(schedule, "enabled", &next.privacy_schedule, error, capacity) ||
                         get_time(schedule, "from", &next.privacy_from, error, capacity) ||
                         get_time(schedule, "until", &next.privacy_until, error, capacity))) {
            if (!error[0]) snprintf(error, capacity, "schedule must be an object");
            return -1;
        }
    }
    cJSON *home = cJSON_GetObjectItemCaseSensitive(root, "home");
    if (home) {
        if (!cJSON_IsObject(home) || !cJSON_GetObjectItemCaseSensitive(home, "x") || !cJSON_GetObjectItemCaseSensitive(home, "y") ||
            get_int(home, "x", -100, 8600, &next.home_x, error, capacity) ||
            get_int(home, "y", -100, 2000, &next.home_y, error, capacity)) {
            if (!error[0]) snprintf(error, capacity, "home needs x and y");
            return -1;
        }
        next.home_set = 1;
    }
    if (get_bool(root, "pilot_led", &next.pilot_led, error, capacity) ||
        get_bool(root, "mic_mute", &next.mic_mute, error, capacity) ||
        get_name(root, "anti_flicker", flicker_names, 3, &next.anti_flicker, error, capacity) ||
        get_int(root, "utc_offset_min", -840, 840, &next.utc_offset_min, error, capacity)) return -1;
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
        cJSON_AddNumberToObject(p, "denoise", s->pic[i].denoise);
        cJSON_AddItemToArray(pictures, p);
    }
    cJSON *patrol = cJSON_AddObjectToObject(root, "patrol");
    if (patrol) {
        cJSON_AddBoolToObject(patrol, "enabled", s->patrol_on);
        cJSON_AddNumberToObject(patrol, "pause_s", s->patrol_pause_s);
        cJSON *stops = cJSON_AddArrayToObject(patrol, "stops");
        for (int i = 0; stops && i < s->patrol_count; ++i) {
            cJSON *stop = cJSON_CreateObject();
            if (!stop) break;
            cJSON_AddNumberToObject(stop, "slot", s->stops[i].slot);
            cJSON_AddNumberToObject(stop, "dwell_s", s->stops[i].dwell_s);
            cJSON_AddItemToArray(stops, stop);
        }
    }
    cJSON *siren = cJSON_AddObjectToObject(root, "siren");
    if (siren) {
        cJSON_AddStringToObject(siren, "sound", jipc_siren_name((unsigned)s->siren_sound));
        cJSON_AddNumberToObject(siren, "volume", s->siren_volume);
        cJSON_AddNumberToObject(siren, "seconds", s->siren_seconds);
        cJSON_AddBoolToObject(siren, "strobe", s->siren_strobe);
    }
    cJSON *overlay = cJSON_AddObjectToObject(root, "overlay");
    if (overlay) {
        static const char *const corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
        static const char *const formats[] = {"ymd", "dmy", "mdy"};
        cJSON_AddBoolToObject(overlay, "enabled", s->overlay_on);
        cJSON_AddStringToObject(overlay, "position", corners[s->overlay_corner]);
        cJSON_AddStringToObject(overlay, "format", formats[s->overlay_format]);
        cJSON_AddStringToObject(overlay, "name", s->overlay_name);
    }
    cJSON *privacy = cJSON_AddObjectToObject(root, "privacy");
    if (privacy) {
        hhmm(s->privacy_from, a); hhmm(s->privacy_until, b);
        cJSON_AddBoolToObject(privacy, "on", s->privacy_on);
        cJSON_AddNumberToObject(privacy, "slot", s->privacy_slot);
        if (s->privacy_held_until) cJSON_AddNumberToObject(privacy, "held_until", s->privacy_held_until);
        cJSON *schedule = cJSON_AddObjectToObject(privacy, "schedule");
        if (schedule) {
            cJSON_AddBoolToObject(schedule, "enabled", s->privacy_schedule);
            cJSON_AddStringToObject(schedule, "from", a);
            cJSON_AddStringToObject(schedule, "until", b);
        }
    }
    if (s->home_set) {
        cJSON *home = cJSON_AddObjectToObject(root, "home");
        if (home) { cJSON_AddNumberToObject(home, "x", s->home_x); cJSON_AddNumberToObject(home, "y", s->home_y); }
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
    if (!root || !cJSON_IsObject(root)) {
        jipc_log("error", "controls: ignoring %s: not valid JSON", c->path);
        cJSON_Delete(root);
        return;
    }
    /* Section by section: one value out of range must not cost the rest, least of all privacy. */
    for (cJSON *item = root->child; item; item = item->next) {
        char error[160] = {0};
        settings trial = c->s;
        cJSON *one = cJSON_CreateObject();
        if (one && item->string && cJSON_AddItemReferenceToObject(one, item->string, item) &&
            !patch(&trial, one, NULL, error, sizeof error)) c->s = trial;
        else jipc_log("error", "controls: ignoring %s in %s: %s", item->string ? item->string : "?", c->path, error);
        cJSON_Delete(one);
    }
    cJSON_Delete(root);
}

/* Takes the text; nonzero if it could not be written. */
static int save_text(const char *path, char *text)
{
    if (!text) return -1;
    size_t n = strlen(text);
    text[n] = '\n'; /* cJSON leaves room for the terminator only; write without it */
    int rc = jipc_atomic_file(path, text, n + 1, 0600);
    cJSON_free(text);
    return rc;
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
    c->error_what = what;
    pthread_mutex_unlock(&c->mutex);
}

static void clear_error(controls_state *c, const char *what)
{
    pthread_mutex_lock(&c->mutex);
    if (c->error_what && !strcmp(c->error_what, what)) { c->error[0] = 0; c->error_what = NULL; }
    pthread_mutex_unlock(&c->mutex);
}

/* Black on every lens; nonzero if any of them refused. */
static int apply_cover(controls_state *c)
{
    char error[160];
    int failed = 0;
    for (unsigned lens = 0; lens < c->app->config.sensors && lens < 2; ++lens) {
        error[0] = 0;
        if (jipc_backend_control(c->app, "privacy_cover", (int)lens, (int)lens, error, sizeof error) < 0) {
            set_error(c, "privacy", error);
            failed = 1;
        }
    }
    if (!failed) clear_error(c, "privacy");
    return failed;
}

/* Nonzero if anything refused. */
static int apply_picture(controls_state *c, const settings *s, int covered)
{
    static const char *const names[] = {"brightness", "contrast", "saturation", "sharpness", "denoise"};
    char error[160];
    int failed = 0;
    for (unsigned lens = 0; lens < c->app->config.sensors && lens < 2; ++lens) {
        const int values[] = {s->pic[lens].brightness, s->pic[lens].contrast, s->pic[lens].saturation,
                              s->pic[lens].sharpness, s->pic[lens].denoise};
        for (unsigned i = covered ? 3 : 0; i < 5; ++i) {
            error[0] = 0;
            if (jipc_backend_control(c->app, names[i], values[i], (int)lens, error, sizeof error) < 0) {
                set_error(c, names[i], error);
                failed = 1;
            } else clear_error(c, names[i]);
        }
    }
    error[0] = 0;
    if (jipc_backend_control(c->app, "anti_flicker", s->anti_flicker, 0, error, sizeof error) < 0) {
        set_error(c, "anti_flicker", error);
        failed = 1;
    } else clear_error(c, "anti_flicker");
    return failed;
}

/* Privacy mode: a manual change until the schedule's next edge, then the
 * schedule (or being on all the time). */
static int privacy_wanted(const controls_state *c, const settings *s, uint64_t now)
{
    if (c->privacy_override_until && now < c->privacy_override_until) return c->privacy_override;
    if (s->privacy_schedule) {
        if (s->privacy_held_until && (long long)time(NULL) < s->privacy_held_until) return 1;
        /* A clock nothing has confirmed may be hours behind after a power cut. Until then, or
         * until the wait runs out with network time off, a camera with a privacy window stays private. */
        if (!c->schedules_ready) return 1;
        return in_window(local_minutes(s), s->privacy_from, s->privacy_until);
    }
    return s->privacy_on;
}

/* One tick of the patrol: go to a stop, wait for the head, stay, next. Someone
 * else moving the head pauses it; it picks up at the same stop. Called without
 * the mutex, as it talks to the motor. */
static void patrol_tick(controls_state *c, const settings *s, int privacy, uint64_t now)
{
    const char *state = "off";
    int left = 0;
    jipc_ptz_motion motion;
    pthread_mutex_lock(&c->mutex);
    int index = c->patrol_index, phase = c->patrol_phase, skipped = c->patrol_skipped;
    uint64_t until = c->patrol_until, moved = c->patrol_move_at;
    pthread_mutex_unlock(&c->mutex);
    if (!s->patrol_on || !s->patrol_count) { index = 0; phase = PATROL_GO; skipped = 0; }
    else if (privacy) { state = "privacy"; phase = PATROL_GO; }
    else if (jipc_platform_motion(c->app, &motion)) state = "unavailable";
    else if (motion.homing || !motion.trusted) { state = "homing"; phase = PATROL_GO; } /* never drive on steps it doubts */
    else if (s->patrol_pause_s && motion.manual_age_s >= 0 && motion.manual_age_s < s->patrol_pause_s) {
        state = "paused"; left = s->patrol_pause_s - (int)motion.manual_age_s; phase = PATROL_GO;
    } else {
        if (index >= s->patrol_count) index = 0;
        if (phase == PATROL_DWELL && now >= until) { index = (index + 1) % s->patrol_count; phase = PATROL_GO; }
        if (phase == PATROL_MOVING && !motion.moving) {
            /* Stopped by someone on the way, with no pause set: it is not there, so go again. */
            if (motion.manual_age_s >= 0 && (uint64_t)motion.manual_age_s * 1000000u < now - moved) phase = PATROL_GO;
            else { phase = PATROL_DWELL; until = now + (uint64_t)s->stops[index].dwell_s * 1000000u; }
        }
        if (phase == PATROL_GO) {
            char error[160] = {0};
            if (jipc_platform_patrol_goto(c->app, s->stops[index].slot, error, sizeof error)) {
                /* A deleted preset: skip it, unless that was every stop. */
                index = (index + 1) % s->patrol_count;
                if (++skipped >= s->patrol_count) { state = "no_stops"; skipped = s->patrol_count; }
                else state = "moving";
            } else { skipped = 0; phase = PATROL_MOVING; state = "moving"; moved = now; }
        } else if (phase == PATROL_MOVING) state = "moving";
        else { state = "dwell"; left = (int)((until - now + 999999u) / 1000000u); }
    }
    pthread_mutex_lock(&c->mutex);
    c->patrol_index = index; c->patrol_phase = phase; c->patrol_skipped = skipped; c->patrol_until = until;
    c->patrol_state = state; c->patrol_left_s = left; c->patrol_move_at = moved;
    pthread_mutex_unlock(&c->mutex);
}

/* The timestamp, redrawn on each new second. Nothing until the clock has been set:
 * a wrong date in a recording is worse than none. */
static void overlay_tick(controls_state *c, const settings *s, int clock_ok)
{
    char text[64], error[160] = {0};
    long long now = (long long)time(NULL);
    /* Not on the time a boot restores, which lags by the time without power. */
    int show = s->overlay_on && clock_ok;
    if (!show && !c->overlay_shown) return;
    if (show && now == c->overlay_second) return;
    c->overlay_second = now;
    c->overlay_shown = show;
    if (show) jipc_overlay_text(text, sizeof text, now, s->utc_offset_min, s->overlay_format, s->overlay_name);
    for (unsigned lens = 0; lens < 2; ++lens)
        if (jipc_backend_overlay(c->app, lens, show ? text : NULL, s->overlay_corner, error, sizeof error) < 0) {
            set_error(c, "overlay", error);
            return;
        }
    clear_error(c, "overlay");
}

/* A file with something in it: network time is configured if its file names a server. */
static int has_content(const char *path)
{
    struct stat file;
    return !stat(path, &file) && file.st_size > 0;
}

/* Frames are let through: ask for keyframes now, instead of waiting out a whole GOP of nothing,
 * which at a long GOP would look like a dead stream to the health check. */
static void release_video(jipc_app *app)
{
    char error[64];
    for (unsigned lens = 0; lens < app->config.sensors && lens < 2; ++lens)
        (void)jipc_backend_control(app, "idr", (int)(lens * 2), 0, error, sizeof error);
}

static void *engine(void *opaque)
{
    controls_state *c = opaque;
    jipc_app *app = c->app;
    pthread_mutex_lock(&c->mutex);
    c->engine_self = pthread_self();
    c->engine_known = 1;
    pthread_mutex_unlock(&c->mutex);
    while (!atomic_load(&c->quit) && !app->stop) {
        uint64_t now = jipc_now_us();
        int siren_active = jipc_siren_active(app);
        jipc_speaker_watch(app);
        pthread_mutex_lock(&c->mutex);
        if (!app->backend_ready) {
            pthread_mutex_unlock(&c->mutex);
            jipc_sleep_ms(200);
            continue;
        }
        int first = !c->applied, do_picture = first || c->apply_picture;
        c->applied = 1; c->apply_picture = 0;
        if (c->retry_picture_at && now >= c->retry_picture_at) { c->retry_picture_at = 0; do_picture = 1; }
        c->clock_ok = !access(c->clock_marker, F_OK) || (atomic_load(&app->clock_confirmed) && !getenv("JIPC_CLOCK_IGNORE_SNTP"));
        c->network_time = has_content(c->ntp_config);
        c->schedules_ready = c->clock_ok || now - c->started_at >=
            (c->clock_wait_us ? c->clock_wait_us : c->network_time ? CLOCK_WAIT_NTP_US : CLOCK_WAIT_US);
        if (c->retry_night_at && now >= c->retry_night_at) { c->retry_night_at = 0; c->reapply_night = 1; }
        if (c->retry_white_at && now >= c->retry_white_at) { c->retry_white_at = 0; c->reapply_white = 1; }
        /* A strobe that came with the siren goes with it, however the siren ended. */
        if (c->strobe_with_siren && c->strobe_until && !siren_active && now >= c->strobe_since + UINT64_C(1000000))
            c->strobe_until = 0;
        if (c->oem_config[0] && (!c->zone_at || now - c->zone_at >= UINT64_C(60000000))) {
            int minutes;
            c->zone_at = now;
            c->zone_valid = !camera_zone(c->oem_config, &minutes);
            if (c->zone_valid) c->zone_min = minutes;
        }
        if (c->zone_valid) c->s.utc_offset_min = c->zone_min; /* the camera's zone wins */
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
        int night = c->active_night, started_day = 0;
        int privacy = privacy_wanted(c, &s, now);
        int change_privacy = privacy != c->active_privacy;
        if (change_privacy) c->active_privacy = privacy;
        /* Coming out of privacy is a start: the hold and the wait are for a switch, and this
         * was none, so a dark scene does not sit in near-black video for minutes. Privacy ran
         * in day mode with the infrared off, which is what a start settles for, so its time
         * counts: day mode in the dark makes frames up to 40 times their usual size (measured
         * on camera1), and video should not spend longer there than the reading needs. */
        if (change_privacy && privacy) c->privacy_since = now;
        if (change_privacy && !privacy) {
            c->auto_start = 1; c->cond_since = 0;
            c->auto_settling = c->privacy_since != 0; /* the first tick is no privacy ending */
            c->auto_settle_at = c->privacy_since + c->auto_settle_us;
        }
        /* Going in: day mode again whatever this engine believes, as something outside it may
         * have changed the ISP's mode. And if the output says the black-out is not working. */
        if (change_privacy && privacy) c->reapply_night = 1;
        if (atomic_exchange(&app->privacy_leak, 0)) {
            c->reapply_night = 1;
            snprintf(c->error, sizeof c->error, "privacy: the picture was still there; video is held");
            c->error_what = "privacy leak";
        }
        if (s.night_mode != NIGHT_AUTO) c->auto_start = 0;
        /* The camera's own light is not daylight: decide without it. */
        if (c->strobe_until || c->trigger_until) c->own_light_at = now;
        uint64_t hold = c->auto_hold_us * (uint64_t)(night == 1 && c->hold_factor ? c->hold_factor : 1);
        /* The black-out only works in the ISP's day mode. In night mode it ignores the picture
         * settings that make it: measured on camera1, the whole street shows. So privacy runs in
         * day mode with the infrared off, whatever the chosen mode says, which comes back after. */
        if (privacy) {
            night = 0;
            snprintf(c->reason, sizeof c->reason, "privacy mode: day mode, which the black-out needs");
        } else switch (s.night_mode) {
        case NIGHT_DAY: night = 0; snprintf(c->reason, sizeof c->reason, "day (chosen)"); break;
        case NIGHT_NIGHT: night = 1; snprintf(c->reason, sizeof c->reason, "night (chosen)"); break;
        case NIGHT_SCHEDULE:
            if (!c->schedules_ready) {
                if (night < 0) night = 0;
                snprintf(c->reason, sizeof c->reason, "schedule: waiting for the clock");
                break;
            }
            night = in_window(local_minutes(&s), s.night_from, s.night_until);
            snprintf(c->reason, sizeof c->reason, c->clock_ok ? "schedule" : "schedule, on a clock nothing has confirmed");
            break;
        default:
            if (!c->ev_valid) {
                if (night < 0) night = 0;
                started_day = c->auto_start; /* no reading yet: nothing started, so no hold */
                snprintf(c->reason, sizeof c->reason, "no exposure reading; holding");
            } else if (c->auto_start) {
                /* A start: day, until the exposure has settled, which reads for the filter
                 * in that position, so the usual rule applies. A first reading was wrong in
                 * both directions. Staying in day starts no hold: dusk can still come. */
                if (!c->auto_settling) { c->auto_settling = 1; c->auto_settle_at = now + c->auto_settle_us; }
                night = 0;
                if (now < c->auto_settle_at) snprintf(c->reason, sizeof c->reason, "auto: waiting for the exposure to settle");
                else {
                    c->auto_settling = 0;
                    c->auto_start = 0;
                    night = c->ev >= s.night_ev;
                    started_day = !night;
                    snprintf(c->reason, sizeof c->reason, "auto: started %s, level %d", night ? "dark" : "light", c->ev);
                }
            } else if (c->own_light_at && now - c->own_light_at < (uint64_t)s.wait_s * 1000000u) {
                c->cond_since = 0;
                snprintf(c->reason, sizeof c->reason, "auto: waiting, the camera's own light is on");
            } else {
                int want = night ? c->ev <= s.day_ev : c->ev >= s.night_ev;
                if (!want) c->cond_since = 0;
                else if (!c->cond_since) c->cond_since = now;
                if (night) snprintf(c->reason, sizeof c->reason, "auto: night; day at level %d or below", s.day_ev);
                else snprintf(c->reason, sizeof c->reason, "auto: day; night at level %d or above", s.night_ev);
                /* The hold stops flapping when a switch changes the reading itself. */
                if (want && now - c->cond_since >= (uint64_t)s.wait_s * 1000000u &&
                    (!c->switched_at || now - c->switched_at >= hold)) {
                    /* Back to day soon after going to night means the switch itself changed the
                     * reading: wait longer each time, until a switch lasts. */
                    if (night) {
                        int factor = c->hold_factor ? c->hold_factor : 1;
                        factor = c->switched_at && now - c->switched_at < hold * 4 ? factor * 2 : 1;
                        c->hold_factor = factor > AUTO_HOLD_MAX_FACTOR ? AUTO_HOLD_MAX_FACTOR : factor;
                    }
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
        if (c->strobe_until && now < c->strobe_until) white = (int)(now / UINT64_C(500000) & 1);
        else if (c->strobe_until = 0, c->trigger_until && now < c->trigger_until) white = 1;
        else {
            c->trigger_until = 0;
            if (s.wl_schedule && c->wl_override_until && now < c->wl_override_until) white = c->wl_override;
            else if (s.wl_schedule) { c->wl_override_until = 0; white = c->schedules_ready && in_window(local_minutes(&s), s.wl_from, s.wl_until); }
            else white = s.white_light;
        }
        int switched = night != c->active_night, change_night = switched || c->reapply_night;
        int change_white = white != c->active_white || c->reapply_white;
        c->reapply_night = c->reapply_white = 0;
        int save_now = c->dirty && now - c->dirty_since >= SAVE_DELAY_US &&
                       (!c->save_failed_at || now - c->save_failed_at >= RETRY_US);
        char *to_save = save_now ? settings_json(&c->s, 1) : NULL;
        int to_day = switched && !night;
        if (to_save) c->dirty = 0;
        if (switched) { c->active_night = night; c->switched_at = now; }
        if (started_day) c->switched_at = 0;
        /* The white light going off: what the last switch saw is gone, so no hold over it. */
        if (change_white && !white && c->active_white == 1 && !switched) c->switched_at = 0;
        if (change_white) c->active_white = white;
        int clock_ok = c->clock_ok;
        pthread_mutex_unlock(&c->mutex);
        /* A flash write can take a while, and everything that touches a control waits for this lock. */
        if (to_save) {
            if (save_text(c->path, to_save)) {
                int saved = errno;
                pthread_mutex_lock(&c->mutex);
                c->dirty = 1; c->save_failed_at = now;
                snprintf(c->error, sizeof c->error, "cannot save controls: %s", strerror(saved));
                c->error_what = "save";
                pthread_mutex_unlock(&c->mutex);
            } else { c->save_failed_at = 0; clear_error(c, "save"); }
        }

        char error[160];
        /* Privacy on: hold the frames, go to day mode, black out, wait for what was in the
         * pipe, and only then let frames through. If day mode or the black-out fails the
         * frames stay held, so a failure pauses video instead of showing it. Off: the picture
         * comes back first, then the head. */
        if (first || change_privacy) {
            if (privacy) {
                atomic_store(&app->privacy_hold, 1);
                c->covered_at = 0;
                jipc_streams_forget_video(app);
                jipc_backend_qp_floor(app, 1);
            } else if (!first) jipc_backend_qp_floor(app, 0);
            atomic_store(&app->privacy_on, privacy);
            atomic_store(&app->mic_muted, s.mic_mute || privacy);
            if (!privacy) do_picture = 1;
            error[0] = 0;
            if (jipc_platform_privacy(app, privacy, s.privacy_slot, error, sizeof error) < 0) set_error(c, "privacy head", error);
            else clear_error(c, "privacy head");
            jipc_log("info", "controls: privacy %s", privacy ? "on" : "off");
        }
        /* Off: the picture first, in day mode, then whatever mode was chosen. */
        if (do_picture && !privacy) {
            do_picture = 0;
            if (apply_picture(c, &s, 0)) {
                pthread_mutex_lock(&c->mutex);
                c->retry_picture_at = now + RETRY_US;
                pthread_mutex_unlock(&c->mutex);
            }
        }
        if (first) {
            error[0] = 0;
            if (jipc_platform_control(app, "pilot_led", s.pilot_led, 0, error, sizeof error) < 0)
                set_error(c, "pilot_led", error);
        }
        if (first || change_night) {
            error[0] = 0;
            if (jipc_platform_control(app, "ir_mode", s.ir_off, 0, error, sizeof error) < 0 ||
                jipc_backend_night(app, night, error, sizeof error) < 0) {
                set_error(c, "night", error);
                c->night_ok = 0;
                pthread_mutex_lock(&c->mutex);
                c->retry_night_at = now + RETRY_US;
                pthread_mutex_unlock(&c->mutex);
            } else {
                jipc_log("info", "controls: %s (%s)", night ? "night" : "day", c->reason);
                c->night_ok = 1;
                clear_error(c, "night");
                /* The mode has changed: whatever was blacked out before is to be done again. */
                if (privacy) {
                    atomic_store(&app->privacy_hold, 1);
                    c->covered_at = 0;
                    jipc_streams_forget_video(app);
                }
            }
        }
        /* After the mode: a keyframe of the picture that stays, not of the one before the switch. */
        if ((first || change_privacy) && !privacy) release_video(app);
        if (privacy && !c->covered_at) { if (c->night_ok && !apply_cover(c)) c->covered_at = now; }
        else if (privacy && atomic_load(&app->privacy_hold) && now - c->covered_at >= PRIVACY_SETTLE_US) {
            atomic_store(&app->privacy_hold, 0);
            release_video(app);
        }
        if (first || change_white) {
            error[0] = 0;
            if (jipc_platform_control(app, "white_light", white, 0, error, sizeof error) < 0) {
                set_error(c, "white_light", error);
                pthread_mutex_lock(&c->mutex);
                c->retry_white_at = now + RETRY_US;
                pthread_mutex_unlock(&c->mutex);
            } else clear_error(c, "white_light");
        }
        /* The ISP ignores picture settings in night mode: going to day, they are written again. */
        if (to_day && !privacy) do_picture = 1;
        if (do_picture && apply_picture(c, &s, privacy)) {
            pthread_mutex_lock(&c->mutex);
            c->retry_picture_at = now + RETRY_US;
            pthread_mutex_unlock(&c->mutex);
        }
        if (!privacy) atomic_store(&app->privacy_hold, 0);
        patrol_tick(c, &s, privacy, now);
        overlay_tick(c, &s, clock_ok);
        jipc_update_tick();

        pthread_mutex_lock(&c->mutex);
        if (!atomic_load(&c->quit) && !c->apply_picture) {
            /* On the monotonic clock: a wait on wall-clock time would sleep through a backward
             * step of however long, with every schedule, timer and save stopped behind it. */
            struct timespec until, wall;
            clock_gettime(CLOCK_MONOTONIC, &until);
            long add = 1000000000;
            if (c->strobe_until) add = 250000000; /* a flash is half a second */
            else if (s.overlay_on && !clock_gettime(CLOCK_REALTIME, &wall)) add = 1000000000 - wall.tv_nsec; /* the timestamp changes on the second */
            until.tv_nsec += add;
            if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec += 1; }
            pthread_cond_timedwait(&c->wake, &c->mutex, &until);
        }
        pthread_mutex_unlock(&c->mutex);
    }
    /* A pending change is not lost at shutdown. */
    pthread_mutex_lock(&c->mutex);
    if (c->dirty) { c->dirty = 0; (void)save_text(c->path, settings_json(&c->s, 1)); }
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

/* JIPC_CLOCK_MARKER: test hook. The platform makes this file once network time or a
 * person has confirmed the clock; a boot's restored time has not. */
static const char *clock_marker(void)
{
    const char *marker = getenv("JIPC_CLOCK_MARKER");
    return marker ? marker : "/run/jooan-local/clock-set";
}

/* Before video starts, so a camera in privacy mode never shows a frame at boot:
 * the engine lets frames through once it has blacked out the lenses. */
void jipc_controls_preload(jipc_app *app)
{
    controls_state *c = calloc(1, sizeof *c);
    if (!c) return;
    jipc_config config;
    jipc_config_copy(app, &config);
    snprintf(c->path, sizeof c->path, "%s/controls.json", config.state_dir);
    load(c);
    int minutes;
    const char *oem = getenv("JIPC_OEM_CONFIG");
    if (!camera_zone(oem ? oem : "/opt/conf/config.json", &minutes)) c->s.utc_offset_min = minutes;
    c->schedules_ready = !access(clock_marker(), F_OK) || atomic_load(&app->clock_confirmed);
    int privacy = privacy_wanted(c, &c->s, 0);
    /* The microphone too: RTSP is listening before the engine's first tick. */
    atomic_store(&app->mic_muted, c->s.mic_mute || privacy);
    if (privacy) {
        atomic_store(&app->privacy_on, 1);
        atomic_store(&app->privacy_hold, 1);
        jipc_log("info", "controls: privacy mode at start; holding video until it is applied");
    }
    free(c);
}

int jipc_controls_start(jipc_app *app)
{
    controls_state *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->app = app;
    c->active_night = c->active_white = c->active_privacy = -1;
    c->patrol_state = "off";
    c->auto_start = 1;
    c->started_at = jipc_now_us();
    snprintf(c->clock_marker, sizeof c->clock_marker, "%s", clock_marker());
    /* JIPC_CLOCK_WAIT_S and JIPC_NTP_CONFIG: test hooks. */
    const char *clock_wait = getenv("JIPC_CLOCK_WAIT_S");
    c->clock_wait_us = clock_wait && *clock_wait ? (uint64_t)atoi(clock_wait) * UINT64_C(1000000) : 0;
    const char *ntp_config = getenv("JIPC_NTP_CONFIG");
    snprintf(c->ntp_config, sizeof c->ntp_config, "%s", ntp_config ? ntp_config : "/opt/custom/jooan-local/config/ntp-server");
    pthread_mutex_init(&c->mutex, NULL);
    pthread_condattr_t monotonic;
    pthread_condattr_init(&monotonic);
    pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
    pthread_cond_init(&c->wake, &monotonic);
    pthread_condattr_destroy(&monotonic);
    jipc_config config;
    jipc_config_copy(app, &config);
    snprintf(c->path, sizeof c->path, "%s/controls.json", config.state_dir);
    /* JIPC_ISP_PROC: a test hook in the environment, never a camera.ini key. */
    /* JIPC_AUTO_HOLD_S: test hook for the hold between automatic switches. */
    const char *hold = getenv("JIPC_AUTO_HOLD_S");
    c->auto_hold_us = (uint64_t)(hold && *hold ? atoi(hold) : AUTO_HOLD_S) * 1000000u;
    /* JIPC_AUTO_SETTLE_S: test hook for how long a start waits for the exposure. */
    const char *settle = getenv("JIPC_AUTO_SETTLE_S");
    c->auto_settle_us = (uint64_t)(settle && *settle ? atoi(settle) : AUTO_SETTLE_S) * 1000000u;
    /* JIPC_OEM_CONFIG: test hook for the vendor config holding the zone. */
    const char *oem = getenv("JIPC_OEM_CONFIG");
    snprintf(c->oem_config, sizeof c->oem_config, "%s", oem ? oem : "/opt/conf/config.json");
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
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); return; }
    pthread_mutex_lock(&c->mutex);
    /* The engine re-applying what is saved is not a change, and its copy may be a moment old. */
    if (c->engine_known && pthread_equal(c->engine_self, pthread_self())) {
        pthread_mutex_unlock(&c->mutex);
        pthread_mutex_unlock(&lifetime);
        return;
    }
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
        c->trigger_until = c->strobe_until = 0;
        c->active_white = value;
        c->reapply_white = 1; /* the engine may have been about to write the opposite: its tick settles it */
    } else if (!strcmp(operation, "pilot_led")) c->s.pilot_led = value;
    else if (!strcmp(operation, "mic_mute")) c->s.mic_mute = value;
    else if (!strcmp(operation, "anti_flicker")) c->s.anti_flicker = value;
    else if (p && !strcmp(operation, "brightness")) p->brightness = value;
    else if (p && !strcmp(operation, "contrast")) p->contrast = value;
    else if (p && !strcmp(operation, "saturation")) p->saturation = value;
    else if (p && !strcmp(operation, "sharpness")) p->sharpness = value;
    else if (p && !strcmp(operation, "denoise")) p->denoise = value;
    if (memcmp(&before, &c->s, sizeof before) && !c->dirty) { c->dirty = 1; c->dirty_since = jipc_now_us(); }
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
}

/* The siren with the saved sound, volume and, unless given, length; its flashing light with it. */
int jipc_controls_siren(jipc_app *app, int on, int seconds, char *error, size_t capacity)
{
    if (error && capacity) error[0] = 0;
    if (on != 0 && on != 1) { snprintf(error, capacity, "siren takes value 1 (on) or 0 (off)"); return -1; }
    if (seconds < 0 || seconds > JIPC_SIREN_MAX_SECONDS) { snprintf(error, capacity, "the siren can sound for 1 to %d seconds", JIPC_SIREN_MAX_SECONDS); return -1; }
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); snprintf(error, capacity, "controls are not running"); return -1; }
    int rc = 0;
    pthread_mutex_lock(&c->mutex);
    settings s = c->s;
    pthread_mutex_unlock(&c->mutex);
    if (!on) jipc_siren_stop(app);
    else rc = jipc_siren_start(app, (unsigned)s.siren_sound, (unsigned)(seconds ? seconds : s.siren_seconds), s.siren_volume, error, capacity);
    if (!rc) {
        pthread_mutex_lock(&c->mutex);
        if (!on) c->strobe_until = 0;
        else if (s.siren_strobe) {
            c->strobe_since = jipc_now_us();
            c->strobe_until = c->strobe_since + (uint64_t)(seconds ? seconds : s.siren_seconds) * UINT64_C(1000000);
            c->strobe_with_siren = 1;
        }
        pthread_cond_signal(&c->wake);
        pthread_mutex_unlock(&c->mutex);
    }
    pthread_mutex_unlock(&lifetime);
    return rc;
}

/* A short sound at the saved volume, to choose one by. */
int jipc_controls_siren_test(jipc_app *app, int sound, char *error, size_t capacity)
{
    if (error && capacity) error[0] = 0;
    if (sound < 0 || sound >= JIPC_SIREN_SOUNDS) { snprintf(error, capacity, "siren_test takes a sound from 0 to %d", JIPC_SIREN_SOUNDS - 1); return -1; }
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); snprintf(error, capacity, "controls are not running"); return -1; }
    pthread_mutex_lock(&c->mutex);
    int volume = c->s.siren_volume;
    if (c->strobe_with_siren) c->strobe_until = 0; /* this replaces the siren the flashing came with */
    pthread_mutex_unlock(&c->mutex);
    int rc = jipc_siren_start(app, (unsigned)sound, JIPC_SIREN_TEST_SECONDS, volume, error, capacity);
    pthread_mutex_unlock(&lifetime);
    return rc;
}

int jipc_controls_strobe(jipc_app *app, int seconds, char *error, size_t capacity)
{
    if (error && capacity) error[0] = 0;
    if (seconds < 0 || seconds > JIPC_SIREN_MAX_SECONDS) { snprintf(error, capacity, "strobe takes 1 to %d seconds, or 0 to stop", JIPC_SIREN_MAX_SECONDS); return -1; }
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); snprintf(error, capacity, "controls are not running"); return -1; }
    pthread_mutex_lock(&c->mutex);
    c->strobe_since = jipc_now_us();
    c->strobe_until = seconds ? c->strobe_since + (uint64_t)seconds * UINT64_C(1000000) : 0;
    c->strobe_with_siren = 0; /* asked for by itself: it lasts its own time */
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    return 0;
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
        /* Choosing on or off by hand ends whatever else is driving the light, as the control does. */
        if (light && (cJSON_GetObjectItemCaseSensitive(light, "on") || cJSON_GetObjectItemCaseSensitive(light, "schedule"))) {
            if (cJSON_GetObjectItemCaseSensitive(light, "on")) c->wl_override_until = 0;
            c->strobe_until = c->trigger_until = 0;
        }
        if (c->s.night_mode != before.night_mode || c->s.night_ev != before.night_ev ||
            c->s.day_ev != before.day_ev) { c->cond_since = 0; c->hold_factor = 0; }
        if (c->s.ir_off != before.ir_off) { c->reapply_night = 1; c->hold_factor = 0; } /* the same mode, with the new IR setting */
        if (picture_changed) c->apply_picture = 1;
        if (c->s.mic_mute != before.mic_mute) atomic_store(&app->mic_muted, c->s.mic_mute || atomic_load(&app->privacy_on));
        /* With a schedule, on/off holds until the schedule's next edge. */
        cJSON *privacy = cJSON_GetObjectItemCaseSensitive(root, "privacy");
        cJSON *privacy_on = privacy ? cJSON_GetObjectItemCaseSensitive(privacy, "on") : NULL;
        if (privacy_on && c->s.privacy_schedule) {
            int edge = next_boundary_s(&c->s, c->s.privacy_from, c->s.privacy_until);
            c->privacy_override = c->s.privacy_on;
            c->privacy_override_until = jipc_now_us() + (uint64_t)edge * 1000000u;
            /* "On" has to outlast a restart too: the owner was told it lasts until the next change. */
            c->s.privacy_held_until = c->s.privacy_on ? (int)((long long)time(NULL) + edge) : 0;
            c->s.privacy_on = before.privacy_on;
        } else if (c->s.privacy_schedule && !before.privacy_schedule && before.privacy_on) {
            /* A schedule turned on during manual privacy: stays private until its next edge, not off at once. */
            int edge = next_boundary_s(&c->s, c->s.privacy_from, c->s.privacy_until);
            c->privacy_override = 1;
            c->privacy_override_until = jipc_now_us() + (uint64_t)edge * 1000000u;
            c->s.privacy_held_until = (int)((long long)time(NULL) + edge);
            c->s.privacy_on = 0; /* or turning the schedule off later would black out with no warning */
        } else if (!c->s.privacy_schedule && before.privacy_schedule && !privacy_on && before.privacy_held_until &&
                   (long long)time(NULL) < before.privacy_held_until) {
            /* A schedule turned off while "on" by hand holds: it stays on, by hand. */
            c->s.privacy_on = 1;
            c->s.privacy_held_until = 0;
            c->privacy_override_until = 0;
        } else if (privacy_on || c->s.privacy_schedule != before.privacy_schedule) {
            c->privacy_override_until = 0;
            c->s.privacy_held_until = 0;
        }
        cJSON *patrol = cJSON_GetObjectItemCaseSensitive(root, "patrol");
        if (patrol && cJSON_GetObjectItemCaseSensitive(patrol, "stops")) { c->patrol_index = 0; c->patrol_phase = PATROL_GO; c->patrol_skipped = 0; }
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

/* The saved home position, in motor steps: nonzero when there is none. */
int jipc_controls_home(jipc_app *app, int *x, int *y)
{
    pthread_mutex_lock(&lifetime);
    controls_state *c = app->controls_state;
    if (!c) { pthread_mutex_unlock(&lifetime); return -1; }
    pthread_mutex_lock(&c->mutex);
    int rc = c->s.home_set ? 0 : -1;
    *x = c->s.home_x; *y = c->s.home_y;
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_unlock(&lifetime);
    return rc;
}

int jipc_controls_set_home(jipc_app *app, int x, int y, char *error, size_t capacity)
{
    char json[64];
    snprintf(json, sizeof json, "{\"home\":{\"x\":%d,\"y\":%d}}", x, y);
    return jipc_controls_patch(app, json, error, capacity);
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
    uint64_t live_hold = c->auto_hold_us * (uint64_t)(c->active_night == 1 && c->hold_factor ? c->hold_factor : 1);
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
            cJSON_AddNumberToObject(live, "hold_left_s", c->s.night_mode == NIGHT_AUTO && c->switched_at &&
                now - c->switched_at < live_hold
                ? (double)((live_hold - (now - c->switched_at)) / 1000000u) : 0);
            cJSON_AddBoolToObject(live, "white_light", c->active_white == 1);
            cJSON_AddNumberToObject(live, "trigger_left_s",
                c->trigger_until && now < c->trigger_until ? (double)((c->trigger_until - now + 999999u) / 1000000u) : 0);
            cJSON_AddNumberToObject(live, "strobe_left_s",
                c->strobe_until && now < c->strobe_until ? (double)((c->strobe_until - now + 999999u) / 1000000u) : 0);
            cJSON_AddBoolToObject(live, "clock_ok", c->clock_ok);
            cJSON_AddBoolToObject(live, "network_time", c->network_time);
            cJSON_AddNumberToObject(live, "local_minutes", local_minutes(&c->s));
            cJSON_AddStringToObject(live, "zone", c->zone_valid ? "camera" : "browser");
            cJSON_AddBoolToObject(live, "privacy", atomic_load(&c->app->privacy_on)); /* true from before video starts */
            cJSON *patrol = cJSON_AddObjectToObject(live, "patrol");
            if (patrol) {
                cJSON_AddStringToObject(patrol, "state", c->patrol_state ? c->patrol_state : "off");
                int moving = c->s.patrol_on && c->patrol_index < c->s.patrol_count;
                cJSON_AddNumberToObject(patrol, "slot", moving ? c->s.stops[c->patrol_index].slot : 0);
                cJSON_AddNumberToObject(patrol, "left_s", c->patrol_left_s);
            }
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
