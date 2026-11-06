#define _GNU_SOURCE
#include "jipc.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fail(char *error, size_t capacity, const char *message)
{ snprintf(error, capacity, "%s", message); return -1; }
static int printable(const char *text, int quoted)
{
    for (; *text; ++text) if ((unsigned char)*text < 32 || (unsigned char)*text == 127 ||
            (quoted && (*text == '"' || *text == '\\'))) return 0;
    return 1;
}
unsigned jipc_video_owner(const jipc_config *c, unsigned ch)
{ return c->main_only && ch < JIPC_VIDEO_STREAMS ? ch & ~1u : ch; }
unsigned jipc_video_width(const jipc_config *c, unsigned ch)
{ ch = jipc_video_owner(c,ch); return ch % 2 ? c->sub_width : ch == 2 && c->width1 ? c->width1 : c->width; }
unsigned jipc_video_height(const jipc_config *c, unsigned ch)
{ ch = jipc_video_owner(c,ch); return ch % 2 ? c->sub_height : ch == 2 && c->height1 ? c->height1 : c->height; }
unsigned jipc_video_bitrate(const jipc_config *c, unsigned ch)
{
    ch = jipc_video_owner(c,ch);
    unsigned primary = ch >= 2 && c->bitrate1 ? c->bitrate1 : c->bitrate;
    return ch % 2 ? (c->sub_bitrate ? c->sub_bitrate : primary > 256 ? primary / 2 : primary) : primary;
}
void jipc_config_defaults(jipc_config *c)
{
    memset(c, 0, sizeof(*c)); strcpy(c->bind, "127.0.0.1"); strcpy(c->username, "admin");
    strcpy(c->realm, "jooanipc-open"); strcpy(c->backend, "replay");
    strcpy(c->state_dir, "./state"); strcpy(c->record_dir, "./recordings");
    c->http_port = 8080; c->rtsp_port = 8554; c->max_clients = 4;
    c->fps = 15; c->bitrate = 1024; c->width = 2304; c->height = 1296;
    c->sub_width = 640; c->sub_height = 360; c->sensors = 2;
    c->record_seconds = 300; c->record_max_mb = 512; c->audio = 1;
    c->discovery = 1; c->discovery_port = 3702;
}
typedef struct { const char *name; size_t offset, capacity; int numeric; } setting;
#define STRING_SETTING(name, field) { name, offsetof(jipc_config, field), sizeof(((jipc_config *)0)->field), 0 }
#define NUMBER_SETTING(name, field) { name, offsetof(jipc_config, field), sizeof(unsigned), 1 }
#define BOOL_SETTING(name, field) { name, offsetof(jipc_config, field), sizeof(int), 2 }
static const setting settings[] = {
    STRING_SETTING("bind", bind), STRING_SETTING("username", username), STRING_SETTING("password", password),
    STRING_SETTING("realm", realm), STRING_SETTING("backend", backend), STRING_SETTING("libimp", libimp),
    STRING_SETTING("input0", replay[0]), STRING_SETTING("input1", replay[1]),
    STRING_SETTING("input2", replay[2]), STRING_SETTING("input3", replay[3]),
    STRING_SETTING("state_dir", state_dir), STRING_SETTING("record_dir", record_dir),
    STRING_SETTING("tls_cert", tls_cert), STRING_SETTING("tls_key", tls_key),
    STRING_SETTING("ntp_server", ntp_server),
    NUMBER_SETTING("http_port", http_port), NUMBER_SETTING("rtsp_port", rtsp_port),
    NUMBER_SETTING("max_clients", max_clients), NUMBER_SETTING("fps", fps), NUMBER_SETTING("bitrate", bitrate),
    NUMBER_SETTING("width", width), NUMBER_SETTING("height", height), NUMBER_SETTING("sub_width", sub_width),
    NUMBER_SETTING("width1", width1), NUMBER_SETTING("height1", height1),
    NUMBER_SETTING("bitrate1", bitrate1), NUMBER_SETTING("sub_bitrate", sub_bitrate),
    NUMBER_SETTING("gop_frames", gop_frames),
    NUMBER_SETTING("sub_height", sub_height), NUMBER_SETTING("sensors", sensors),
    NUMBER_SETTING("flip0", flip0), NUMBER_SETTING("flip1", flip1),
    NUMBER_SETTING("record_seconds", record_seconds), NUMBER_SETTING("record_max_mb", record_max_mb),
    BOOL_SETTING("audio", audio), BOOL_SETTING("recording", recording), BOOL_SETTING("allow_plain_http", allow_plain_http),
    BOOL_SETTING("discovery", discovery), NUMBER_SETTING("discovery_port", discovery_port),
    BOOL_SETTING("watchdog", watchdog), BOOL_SETTING("onvif_nudge", onvif_nudge),
    BOOL_SETTING("main_only", main_only), BOOL_SETTING("motor_home", motor_home),
};
static const setting *find_setting(const char *name)
{
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i)
        if (!strcmp(name, settings[i].name)) return &settings[i];
    return NULL;
}
static char *trim(char *s)
{ while (isspace((unsigned char)*s)) ++s; size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0; return s; }
int jipc_config_load(jipc_config *c, const char *path, char *error, size_t capacity)
{
    FILE *file = fopen(path, "r"); char line[2048]; unsigned number = 0;
    if (!file) return fail(error, capacity, "cannot open configuration file");
    if (strlen(path) >= sizeof(c->config_path)) { fclose(file); return fail(error, capacity, "configuration path too long"); }
    strcpy(c->config_path, path);
    unsigned char seen[sizeof(settings) / sizeof(settings[0])] = {0};
    while (fgets(line, sizeof(line), file)) {
        ++number; if (!strchr(line, '\n') && !feof(file)) { fclose(file); return fail(error, capacity, "configuration line too long"); }
        char *key = trim(line); if (!*key || *key == '#' || *key == ';') continue;
        char *equals = strchr(key, '='); if (!equals) goto invalid;
        *equals++ = 0; key = trim(key); char *value = trim(equals);
        const setting *s = find_setting(key); if (!s || seen[s - settings]) goto invalid;
        seen[s - settings] = 1; unsigned char *dest = (unsigned char *)c + s->offset;
        if (!s->numeric) { if (strlen(value) >= s->capacity) goto invalid; strcpy((char *)dest, value); }
        else { char *end; errno = 0; unsigned long n = strtoul(value, &end, 10);
            if (!*value || *value == '-' || errno || *end || n > UINT_MAX || (s->numeric == 2 && n > 1)) goto invalid;
            if (s->numeric == 2) *(int *)dest = (int)n; else *(unsigned *)dest = (unsigned)n;
        }
        continue;
invalid:
        fclose(file); snprintf(error, capacity, "invalid or duplicate setting at line %u", number); return -1;
    }
    int bad = ferror(file); fclose(file); if (bad) return fail(error, capacity, "configuration read failed");
    /* Validate after command-line overrides, so --input can complete a template. */
    return 0;
}
int jipc_native_video_validate(const jipc_config *c, char *error, size_t capacity)
{
    if (c->main_only) {
        if (c->sensors != 2 || c->width > 1920 || c->height > 1080 ||
            jipc_video_width(c,2) > 1024 || jipc_video_height(c,2) > 576)
            return fail(error, capacity, "main_only requires two sensors, primary up to 1920x1080 and fixed view up to 1024x576");
    } else if (c->width > 1280 || c->height > 720 || (c->sensors == 2 &&
        (jipc_video_width(c,0) != jipc_video_width(c,2) || jipc_video_height(c,0) != jipc_video_height(c,2)))) {
        return fail(error, capacity, "independent native streams require matching main dimensions up to 1280x720");
    }
    return 0;
}
int jipc_config_validate(const jipc_config *c, char *error, size_t capacity)
{
    /* Unquoted INI values must survive save/load without trimming or injecting
     * settings. This also applies to strings supplied by the API and CLI. */
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        const setting *s = &settings[i];
        if (s->numeric) continue;
        const char *value = (const char *)c + s->offset; size_t length = strlen(value);
        if (!printable(value, 0) || (length && (isspace((unsigned char)value[0]) ||
                isspace((unsigned char)value[length - 1]))))
            return fail(error, capacity, "string settings require printable values without leading or trailing whitespace");
    }
    if (strcmp(c->backend, "replay") && strcmp(c->backend, "imp")) return fail(error, capacity, "backend must be replay or imp");
    if (!c->username[0] || strchr(c->username, ':') || !printable(c->username, 1) ||
        !c->password[0] || !printable(c->password, 0)) return fail(error, capacity, "set a nonempty printable username/password in the private configuration");
    if (!c->realm[0] || !printable(c->realm, 1)) return fail(error, capacity, "invalid authentication realm");
    struct in_addr bind_address;
    if (inet_pton(AF_INET, c->bind, &bind_address) != 1 || !c->http_port || c->http_port > 65535 ||
        !c->rtsp_port || c->rtsp_port > 65535 || c->http_port == c->rtsp_port)
        return fail(error, capacity, "bind must be an IPv4 address with distinct valid server ports");
    if ((c->tls_cert[0] != 0) != (c->tls_key[0] != 0)) return fail(error, capacity, "TLS requires both certificate and private key");
    uint32_t ntp_address; uint16_t ntp_port;
    if (c->ntp_server[0] && jipc_ntp_server(c->ntp_server, &ntp_address, &ntp_port))
        return fail(error, capacity, "ntp_server must be an IPv4 address with an optional :port");
    if (!c->tls_cert[0] && !c->allow_plain_http && (ntohl(bind_address.s_addr) >> 24) != 127)
        return fail(error, capacity, "LAN HTTP requires TLS or explicit allow_plain_http=1");
    if (c->sensors < 1 || c->sensors > 2 || c->max_clients < 1 || c->max_clients > 8 || c->fps < 1 || c->fps > 30 || c->bitrate < 64 || c->bitrate > 8192)
        return fail(error, capacity, "invalid sensors, clients, frame rate or bitrate");
    if (c->flip0 > 3 || c->flip1 > 3) return fail(error, capacity, "sensor flips require bitmasks 0..3");
    if (c->main_only && c->sensors != 2) return fail(error, capacity, "main_only requires two sensors");
    if (c->width < 160 || c->width > 4096 || c->height < 120 || c->height > 2160 || c->width % 2 || c->height % 2 ||
        c->sub_width < 160 || c->sub_width > c->width || c->sub_height < 120 || c->sub_height > c->height || c->sub_width % 2 || c->sub_height % 2)
        return fail(error, capacity, "invalid even video dimensions");
    if (!!c->width1 != !!c->height1 ||
        (c->width1 && (c->width1 < c->sub_width || c->width1 > 4096 || c->width1 % 2 ||
         c->height1 < c->sub_height || c->height1 > 2160 || c->height1 % 2)) ||
        (c->bitrate1 && (c->bitrate1 < 64 || c->bitrate1 > 8192)) ||
        (c->sub_bitrate && (c->sub_bitrate < 64 || c->sub_bitrate > 8192)) || c->gop_frames > 300)
        return fail(error, capacity, "invalid secondary sensor geometry, bitrate or GOP");
    const char *native = getenv("OPENIMP_T23_ENCODER"), *abi = getenv("OPENIMP_T23_DRIVER_ABI");
    if (!strcmp(c->backend,"imp") && native && !strcmp(native,"native") && abi && !strcmp(abi,"oem80") &&
        jipc_native_video_validate(c,error,capacity)) return -1;
    if (c->record_seconds < 1 || c->record_seconds > 3600 || c->record_max_mb < 1 || c->record_max_mb > 4096)
        return fail(error, capacity, "invalid recording retention or segment duration");
    if (!c->discovery_port || c->discovery_port > 65535) return fail(error, capacity, "invalid discovery port");
    if (!c->state_dir[0] || !c->record_dir[0] || !printable(c->state_dir, 0) || !printable(c->record_dir, 0))
        return fail(error, capacity, "invalid state/recording directory");
    if (!strcmp(c->backend, "replay") && !c->replay[0][0]) return fail(error, capacity, "replay backend requires input0 Annex-B H.264 file");
    if (!strcmp(c->backend, "imp") && !c->libimp[0]) return fail(error, capacity, "imp backend requires an explicit compatible external libimp path");
    if (c->watchdog && strcmp(c->backend,"imp")) return fail(error,capacity,"hardware watchdog requires the IMP backend");
    return 0;
}
int jipc_config_save(const jipc_config *c)
{
    if (!c->config_path[0]) return -1;
    char output[8192]; size_t used = 0;
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        const setting *s = &settings[i]; const unsigned char *p = (const unsigned char *)c + s->offset; int n;
        if (s->numeric == 1) n = snprintf(output + used, sizeof(output) - used, "%s=%u\n", s->name, *(const unsigned *)p);
        else if (s->numeric == 2) n = snprintf(output + used, sizeof(output) - used, "%s=%d\n", s->name, *(const int *)p);
        else n = snprintf(output + used, sizeof(output) - used, "%s=%s\n", s->name, (const char *)p);
        if (n < 0 || (size_t)n >= sizeof(output) - used) return -1;
        used += (size_t)n;
    }
    return jipc_atomic_file(c->config_path, output, used, 0600);
}
void jipc_config_copy(jipc_app *app, jipc_config *out)
{ pthread_mutex_lock(&app->config_mutex); *out = app->config; pthread_mutex_unlock(&app->config_mutex); }
int jipc_config_json(jipc_app *app, char *out, size_t capacity)
{
    jipc_config c; jipc_config_copy(app, &c); cJSON *o = cJSON_CreateObject(); if (!o) return -1;
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        const setting *s = &settings[i]; const unsigned char *p = (const unsigned char *)&c + s->offset;
        if (!strcmp(s->name, "password")) { cJSON_AddBoolToObject(o, "password_set", c.password[0] != 0); continue; }
        if (s->numeric == 1) cJSON_AddNumberToObject(o, s->name, *(const unsigned *)p);
        else if (s->numeric == 2) cJSON_AddBoolToObject(o, s->name, *(const int *)p);
        else cJSON_AddStringToObject(o, s->name, (const char *)p);
    }
    char *text = cJSON_PrintUnformatted(o); int rc = -1;
    if (text && strlen(text) < capacity) { strcpy(out, text); rc = (int)strlen(out); }
    cJSON_free(text); cJSON_Delete(o); return rc;
}
int jipc_config_patch(jipc_app *app, const char *json, char *error, size_t capacity)
{
    if (!json || strlen(json) > JIPC_MAX_JSON || strstr(json, "\\u0000")) return fail(error, capacity, "invalid JSON configuration patch");
    cJSON *o = cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, 1);
    if (!cJSON_IsObject(o) || !o->child) { cJSON_Delete(o); return fail(error, capacity, "patch must be a nonempty JSON object"); }
    pthread_mutex_lock(&app->config_mutex); jipc_config next = app->config; int rc = -1, restart = 0;
    for (cJSON *v = o->child; v; v = v->next) {
        for (cJSON *w = v->next; w; w = w->next) if (!strcmp(v->string, w->string)) goto invalid;
        const setting *s = find_setting(v->string);
        /* Filesystem roots, listener endpoints, input/plugin paths and the
         * system clock source are local-administrator settings. */
        if (!s || !strcmp(s->name, "bind") || strstr(s->name, "port") || !strcmp(s->name, "backend") ||
            !strcmp(s->name, "realm") || strstr(s->name, "dir") || !strcmp(s->name, "libimp") ||
            !strcmp(s->name, "ntp_server") || !strcmp(s->name, "motor_home") ||
            !strncmp(s->name, "input", 5) || !strncmp(s->name, "tls_", 4) || !strcmp(s->name, "allow_plain_http") || !strcmp(s->name,"watchdog")) goto invalid;
        unsigned char *dest = (unsigned char *)&next + s->offset;
        if (!s->numeric) { if (!cJSON_IsString(v) || strlen(v->valuestring) >= s->capacity) goto invalid; strcpy((char *)dest, v->valuestring); }
        else if (s->numeric == 2) { if (!cJSON_IsBool(v)) goto invalid; *(int *)dest = cJSON_IsTrue(v); }
        else { if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 || v->valuedouble > UINT_MAX || floor(v->valuedouble) != v->valuedouble) goto invalid;
            *(unsigned *)dest = (unsigned)v->valuedouble; }
        if (strcmp(s->name, "username") && strcmp(s->name, "password") && strcmp(s->name, "recording") && strcmp(s->name, "record_seconds") && strcmp(s->name, "record_max_mb")) restart = 1;
    }
    if (jipc_config_validate(&next, error, capacity)) goto done;
    if (jipc_config_save(&next)) { fail(error, capacity, "configuration persistence failed"); goto done; }
    app->config = next; app->record_active = next.recording; if (restart) app->restart = 1; rc = 0; goto done;
invalid:
    fail(error, capacity, "unknown, duplicate, invalid or local-only configuration setting");
done:
    pthread_mutex_unlock(&app->config_mutex); cJSON_Delete(o); return rc;
}
