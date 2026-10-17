#define _GNU_SOURCE
#include "jipc.h"
#include "cJSON.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted, reload_requested;
static void signal_handler(int number)
{ if (number == SIGHUP) reload_requested = 1; else interrupted = 1; }
int jipc_status_json(jipc_app *app, char *out, size_t capacity)
{
    jipc_config cfg; jipc_config_copy(app, &cfg); cJSON *o = cJSON_CreateObject(); if (!o) return -1;
    cJSON_AddStringToObject(o, "version", JIPC_VERSION); cJSON_AddStringToObject(o, "backend", cfg.backend);
    cJSON_AddStringToObject(o, "hardware_profile", "JA-A12-T23N-CV2005-CV2005S1");
    cJSON_AddBoolToObject(o, "hardware_validated", 0);
    cJSON_AddNumberToObject(o, "uptime_seconds", (double)(jipc_now_us() - app->started_us) / 1000000.0);
    pthread_mutex_lock(&app->control_mutex);
    cJSON_AddBoolToObject(o, "ready", app->backend_ready && !app->stop);
    cJSON_AddStringToObject(o, "backend_error", app->backend_error);
    pthread_mutex_unlock(&app->control_mutex);
    pthread_mutex_lock(&app->config_mutex);
    cJSON_AddBoolToObject(o, "recording", app->record_active);
    cJSON_AddStringToObject(o, "record_error", app->record_error);
    cJSON_AddBoolToObject(o, "restart_pending", app->restart);
    pthread_mutex_unlock(&app->config_mutex);
    cJSON *streams = cJSON_AddArrayToObject(o, "streams");
    for (unsigned i = 0; i < JIPC_STREAMS; ++i) {
        if ((i < JIPC_AUDIO_STREAM && i >= cfg.sensors * 2) || (i == JIPC_AUDIO_STREAM && !cfg.audio)) continue;
        cJSON *s = cJSON_CreateObject(); pthread_mutex_lock(&app->streams[i].mutex);
        cJSON_AddNumberToObject(s, "channel", i); cJSON_AddNumberToObject(s, "frames", (double)app->produced[i]);
        cJSON_AddNumberToObject(s, "width", app->streams[i].width); cJSON_AddNumberToObject(s, "height", app->streams[i].height);
        cJSON_AddBoolToObject(s, "codec_ready", i == JIPC_AUDIO_STREAM || (app->streams[i].sps_size && app->streams[i].pps_size));
        pthread_mutex_unlock(&app->streams[i].mutex); cJSON_AddItemToArray(streams, s);
    }
    char status[2048]; if (jipc_platform_status_json(app, status, sizeof(status)) >= 0) {
        cJSON *platform = cJSON_Parse(status); if (platform) cJSON_AddItemToObject(o, "platform", platform);
    }
    char *text = cJSON_PrintUnformatted(o); int rc = -1;
    if (text && strlen(text) < capacity) { strcpy(out, text); rc = (int)strlen(text); }
    cJSON_free(text); cJSON_Delete(o); return rc;
}
static void usage(FILE *out)
{
    fprintf(out, "jooanipc-open %s\nUsage: jooanipc-open --config FILE [options]\n"
        "  --init FILE          create private configuration with random password\n"
        "  --input FILE         replay Annex-B H.264 input0 (other inputs default to it)\n"
        "  --backend replay|imp select file replay or external IMP hardware backend\n"
        "  --libimp FILE        compatible external T23/Ace library, never bundled\n"
        "  --bind ADDRESS       listener IPv4 address (default127.0.0.1)\n"
        "  --http-port N --rtsp-port N --record-dir DIR\n"
        "  --allow-plain-http   explicitly permit plaintext HTTP outside loopback\n"
        "  --check-config       validate without starting devices or listeners\n"
        "  --duration N         stop after N seconds (integration tests)\n"
        "  --version --help\n"
        "SIGHUP reloads configuration by stopping and restarting owned services.\n", JIPC_VERSION);
}
static int number(const char *text, unsigned *out)
{
    char *end; errno = 0; unsigned long n = strtoul(text, &end, 10);
    if (!*text || *text == '-' || errno || *end || n > 86400) return -1;
    *out = (unsigned)n; return 0;
}
static int assign(char *out, size_t capacity, const char *value)
{ if (strlen(value) >= capacity) return -1; strcpy(out, value); return 0; }
static int apply_overrides(jipc_config *config, const char *input, const char *backend,
                           const char *library, const char *bind, const char *record_dir,
                           unsigned http, unsigned rtsp, int plain)
{
    if ((input && assign(config->replay[0], sizeof(config->replay[0]), input)) ||
        (backend && assign(config->backend, sizeof(config->backend), backend)) ||
        (library && assign(config->libimp, sizeof(config->libimp), library)) ||
        (bind && assign(config->bind, sizeof(config->bind), bind)) ||
        (record_dir && assign(config->record_dir, sizeof(config->record_dir), record_dir))) return -1;
    if (http) config->http_port = http;
    if (rtsp) config->rtsp_port = rtsp;
    if (plain) config->allow_plain_http = 1;
    return 0;
}
int main(int argc, char **argv)
{
    const char *config_path = NULL, *initial = NULL, *input = NULL, *backend = NULL, *library = NULL, *bind = NULL, *record_dir = NULL;
    unsigned http = 0, rtsp = 0, duration = 0; int plain = 0, check = 0;
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'}, {"init", required_argument, NULL, 'i'},
        {"input", required_argument, NULL, 'f'}, {"backend", required_argument, NULL, 'b'},
        {"libimp", required_argument, NULL, 'l'}, {"bind", required_argument, NULL, 'a'},
        {"http-port", required_argument, NULL, 'p'}, {"rtsp-port", required_argument, NULL, 'r'},
        {"record-dir", required_argument, NULL, 'd'}, {"duration", required_argument, NULL, 't'},
        {"allow-plain-http", no_argument, NULL, 'P'}, {"check-config", no_argument, NULL, 'C'},
        {"version", no_argument, NULL, 'v'}, {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int opt; while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) switch (opt) {
        case 'c': config_path = optarg; break; case 'i': initial = optarg; break; case 'f': input = optarg; break;
        case 'b': backend = optarg; break; case 'l': library = optarg; break; case 'a': bind = optarg; break;
        case 'd': record_dir = optarg; break; case 'P': plain = 1; break; case 'C': check = 1; break;
        case 'p': if (number(optarg, &http)) goto bad; break; case 'r': if (number(optarg, &rtsp)) goto bad; break;
        case 't': if (number(optarg, &duration)) goto bad; break;
        case 'v': puts(JIPC_VERSION); return 0; case 'h': usage(stdout); return 0; default: goto bad;
    }
    if (optind != argc || (!config_path && !initial)) goto bad;
    jipc_config config; jipc_config_defaults(&config); char error[256] = {0};
    if (config_path && jipc_config_load(&config, config_path, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 2; }
    if (apply_overrides(&config, input, backend, library, bind, record_dir, http, rtsp, plain)) goto bad;
    if (initial) {
        struct stat st; if (!lstat(initial, &st) || errno != ENOENT) { fputs("Refusing to replace existing configuration\n", stderr); return 2; }
        unsigned char secret[16]; if (jipc_random(secret, sizeof(secret))) { perror("random"); return 1; }
        for (size_t i = 0; i < sizeof(secret); ++i) snprintf(config.password + i * 2, 3, "%02x", secret[i]);
        if (assign(config.config_path, sizeof(config.config_path), initial) || jipc_config_save(&config)) { perror("configuration creation"); return 1; }
        printf("Created %s (mode0600)\nUsername: %s\nPassword: %s\n", initial, config.username, config.password); return 0;
    }
    if (jipc_config_validate(&config, error, sizeof(error))) { fprintf(stderr, "%s\n", error); return 2; }
    if (check) { puts("Configuration valid; devices were not opened"); return 0; }
    struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = signal_handler; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL); sigaction(SIGHUP, &action, NULL);
    signal(SIGPIPE, SIG_IGN); uint64_t deadline = duration ? jipc_now_us() + (uint64_t)duration * 1000000u : 0; int result = 0;
    do {
        reload_requested = 0; jipc_app *app = calloc(1, sizeof(*app)); if (!app) return 1;
        app->config = config; app->started_us = jipc_now_us();
        atomic_init(&app->stop, 0); atomic_init(&app->restart, 0);
        pthread_mutex_init(&app->config_mutex, NULL); pthread_mutex_init(&app->control_mutex, NULL);
        if (jipc_mkdirs(config.state_dir, 0700) || jipc_streams_init(app)) { free(app); return 1; }
        if (jipc_backend_start(app) || jipc_record_start(app) || jipc_rtsp_start(app) || jipc_http_start(app) || jipc_discovery_start(app)) {
            jipc_log("error", "startup failed: %s", app->backend_error[0] ? app->backend_error : "see previous service diagnostics"); result = 1;
        } else {
            jipc_log("info", "ready: backend=%s, %u video streams", config.backend, config.sensors * 2);
            while (!interrupted && !reload_requested && !app->restart && (!deadline || jipc_now_us() < deadline)) {
                pthread_mutex_lock(&app->control_mutex); int ready = app->backend_ready; pthread_mutex_unlock(&app->control_mutex);
                if (!ready) { jipc_log("error", "media backend stopped; shutting down services"); result = 1; break; }
                jipc_sleep_ms(100);
            }
        }
        int restart = !result && !interrupted && (!deadline || jipc_now_us() < deadline) && (reload_requested || app->restart);
        app->stop = 1; jipc_discovery_stop(app); jipc_http_stop(app); jipc_rtsp_stop(app); jipc_record_stop(app);
        jipc_backend_stop(app); jipc_platform_stop(app); jipc_streams_destroy(app);
        pthread_mutex_destroy(&app->control_mutex); pthread_mutex_destroy(&app->config_mutex); free(app);
        if (!restart) break;
        jipc_config_defaults(&config);
        if (jipc_config_load(&config, config_path, error, sizeof(error)) ||
            apply_overrides(&config, input, backend, library, bind, record_dir, http, rtsp, plain) ||
            jipc_config_validate(&config, error, sizeof(error))) { jipc_log("error", "reload: %s", error); result = 1; break; }
    } while (!interrupted);
    return result;
bad:
    usage(stderr); return 2;
}
