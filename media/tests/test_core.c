#define _GNU_SOURCE
#include "jipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(expression) do { ++checks; if (!(expression)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); } } while (0)
static void ntp_stamp(unsigned char *p, int64_t unix_us)
{
    uint64_t seconds = ((uint64_t)(unix_us / 1000000) + 2208988800ULL) & 0xffffffffu;
    uint64_t value = seconds << 32 | (((uint64_t)(unix_us % 1000000) << 32) / 1000000);
    for (int i = 7; i >= 0; --i) { p[i] = (unsigned char)value; value >>= 8; }
}
static void ntp_reply(unsigned char reply[48], const unsigned char origin[8], int64_t received, int64_t transmitted)
{
    memset(reply, 0, 48); reply[0] = 0x24; reply[1] = 2; /* leap 0, version 4, server; stratum 2 */
    memcpy(reply + 24, origin, 8); ntp_stamp(reply + 32, received); ntp_stamp(reply + 40, transmitted);
}
static void ntp_checks(jipc_app *app)
{
    uint32_t address = 0; uint16_t port = 0; char error[256];
    CHECK(!jipc_ntp_server("192.0.2.1", &address, &port) && address == 0xc0000201u && port == 123);
    CHECK(!jipc_ntp_server("127.0.0.1:12345", &address, &port) && address == 0x7f000001u && port == 12345);
    const char *bad_servers[] = { "", "0.0.0.0", "gateway.lan", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:65536",
                                  "1.2.3.4:+5", "1.2.3.4:5x", "1.2.3.4 ", "::1" };
    for (size_t i = 0; i < sizeof(bad_servers) / sizeof(bad_servers[0]); ++i)
        CHECK(jipc_ntp_server(bad_servers[i], &address, &port) < 0);

    /* Local-only setting: validated, persisted, never accepted from the API. */
    strcpy(app->config.ntp_server, "gateway.lan"); CHECK(jipc_config_validate(&app->config, error, sizeof(error)) < 0);
    strcpy(app->config.ntp_server, "192.0.2.1"); CHECK(!jipc_config_validate(&app->config, error, sizeof(error)));
    CHECK(!jipc_config_save(&app->config));
    jipc_config loaded; jipc_config_defaults(&loaded);
    CHECK(!jipc_config_load(&loaded, app->config.config_path, error, sizeof(error)) && !strcmp(loaded.ntp_server, "192.0.2.1"));
    CHECK(jipc_config_patch(app, "{\"ntp_server\":\"10.0.0.1\"}", error, sizeof(error)) < 0);
    CHECK(!strcmp(app->config.ntp_server, "192.0.2.1"));

    const unsigned char origin[8] = {1, 2, 3, 4, 5, 6, 7, 8}, other[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    const int64_t real = 1792588800LL * 1000000; /* 2026-10-21T00:00:00Z */
    unsigned char reply[48]; int64_t offset = 0;
    /* Camera restored a saved clock nine hours behind; 30 ms round trip. */
    int64_t sent = real - 9LL * 3600 * 1000000;
    ntp_reply(reply, origin, real + 10000, real + 11000);
    CHECK(!jipc_ntp_offset(reply, 48, origin, sent, 30000, &offset) && offset == 9LL * 3600 * 1000000 - 4500);
    /* Camera ahead of the server. */
    ntp_reply(reply, origin, real, real);
    CHECK(!jipc_ntp_offset(reply, 48, origin, real + 120000000, 0, &offset) && offset == -120000000);
    /* NTP era 1 (after 2036-02-07) decodes from the wrapped seconds field. */
    const int64_t era1 = 2208988800LL * 1000000; /* 2040-01-01T00:00:00Z */
    ntp_reply(reply, origin, era1 + 5000000, era1 + 5000000);
    CHECK(!jipc_ntp_offset(reply, 48, origin, era1, 0, &offset) && offset == 5000000);

    ntp_reply(reply, origin, real, real);
    CHECK(jipc_ntp_offset(reply, 47, origin, real, 1000, &offset) < 0);
    CHECK(jipc_ntp_offset(reply, 48, other, real, 1000, &offset) < 0);
    reply[0] = 0x23; CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);  /* client mode */
    reply[0] = 0xe4; CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);  /* leap 3 */
    reply[0] = 0x14; CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);  /* version 2 */
    reply[0] = 0x1c; CHECK(!jipc_ntp_offset(reply, 48, origin, real, 1000, &offset)); /* version 3 */
    reply[0] = 0x24; reply[1] = 0; CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);
    reply[1] = 16; CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);
    reply[1] = 2; memset(reply + 40, 0, 8); CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);
    ntp_reply(reply, origin, real + 2000, real + 1000);   /* transmitted before received */
    CHECK(jipc_ntp_offset(reply, 48, origin, real, 5000, &offset) < 0);
    ntp_reply(reply, origin, real, real + 6000);          /* server time exceeds round trip */
    CHECK(jipc_ntp_offset(reply, 48, origin, real, 5000, &offset) < 0);
    ntp_reply(reply, origin, 1735689600LL * 1000000, 1735689600LL * 1000000); /* 2025 */
    CHECK(jipc_ntp_offset(reply, 48, origin, real, 1000, &offset) < 0);

    app->config.ntp_server[0] = 0; CHECK(!jipc_config_save(&app->config));
}
static void audio_checks(void)
{
    /* Sample-count clock: 40 ms frames step exactly despite dequeue jitter,
     * and a long stall re-anchors with a discontinuity. */
    jipc_audio_clock clock; jipc_audio_clock_init(&clock, 8000);
    int jump = 0;
    uint64_t now = 10000000, pts = jipc_audio_clock_stamp(&clock, now, 320, &jump);
    CHECK(jump && pts == now - 40000);
    const int jitter[] = {20000, -15000, 9000, -20000, 0};
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t next = jipc_audio_clock_stamp(&clock, now + 40000 * (i + 1) + (uint64_t)(int64_t)jitter[i], 320, &jump);
        CHECK(!jump && next == pts + 40000 * (i + 1));
    }
    uint64_t later = jipc_audio_clock_stamp(&clock, now + 40000 * 6 + 300000, 320, &jump);
    CHECK(jump && later == now + 40000 * 6 + 300000 - 40000);
    /* A burst of buffered frames is "ahead" (the caller drops it); stamping
     * one anyway never moves time back or repeats it. */
    uint64_t t0 = now + 40000 * 6 + 300000, last = later;
    CHECK(!jipc_audio_clock_ahead(&clock, t0 + 40000, 320));
    unsigned accepted = 0, dropped = 0;
    for (unsigned i = 0; i < 20; ++i) {
        if (jipc_audio_clock_ahead(&clock, t0 + 40000 + i * 1000, 320)) { ++dropped; continue; }
        uint64_t next = jipc_audio_clock_stamp(&clock, t0 + 40000 + i * 1000, 320, &jump);
        CHECK(!jump && next == last + 40000);
        last = next; ++accepted;
    }
    CHECK(accepted == 5 && dropped == 15); /* stays within 150 ms of now */
    uint64_t forced = jipc_audio_clock_stamp(&clock, t0, 320, &jump);
    CHECK(!jump && forced == last + 40000);
    /* Ring depth per stream; a slow audio reader resumes
     * at the oldest retained frame, not the newest. */
    jipc_app *app = calloc(1, sizeof *app); CHECK(app);
    jipc_config_defaults(&app->config);
    CHECK(!jipc_streams_init(app));
    CHECK(app->streams[0].capacity == JIPC_VIDEO_RING && app->streams[JIPC_AUDIO_STREAM].capacity == JIPC_AUDIO_RING);
    unsigned char sample[320]; memset(sample, 0xd5, sizeof sample);
    for (unsigned i = 0; i < 40; ++i)
        CHECK(!jipc_publish(app, JIPC_AUDIO_STREAM, sample, sizeof sample, 1000 + i, 1, JIPC_PCMA));
    jipc_frame frame = {0}; uint64_t cursor = 1;
    CHECK(jipc_read(app, JIPC_AUDIO_STREAM, &cursor, &frame, 0) == 1);
    CHECK(frame.sequence == 40 - JIPC_AUDIO_RING + 1 && frame.pts_us == 1000 + 40 - JIPC_AUDIO_RING);
    jipc_frame_free(&frame);
    /* Privacy: what was in a video ring before a hold is not read by anyone afterwards. */
    for (unsigned i = 0; i < 3; ++i) CHECK(!jipc_publish(app, 0, sample, sizeof sample, 2000 + i, 1, JIPC_H264));
    jipc_streams_forget_video(app);
    cursor = 0;
    CHECK(jipc_read(app, 0, &cursor, &frame, 0) == 0);      /* someone joining */
    cursor = 1;
    CHECK(jipc_read(app, 0, &cursor, &frame, 0) == 0);      /* someone behind */
    CHECK(!jipc_publish(app, 0, sample, sizeof sample, 3000, 1, JIPC_H264));
    CHECK(jipc_read(app, 0, &cursor, &frame, 0) == 1 && frame.pts_us == 3000);
    jipc_frame_free(&frame);
    jipc_streams_destroy(app); free(app);
}

int main(void)
{
    CHECK(sizeof(off_t) >= 8);
    audio_checks();
    char hex[65], encoded[256]; unsigned char decoded[256], sha[20];
    jipc_md5("abc", 3, hex); CHECK(!strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"));
    jipc_sha1("abc", 3, sha); CHECK(!memcmp(sha, (unsigned char[]){0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d}, 20));
    CHECK(jipc_base64("camera", 6, encoded, sizeof(encoded)) == 8); CHECK(!strcmp(encoded, "Y2FtZXJh"));
    CHECK(jipc_base64_decode(encoded, decoded, sizeof(decoded)) == 6); CHECK(!memcmp(decoded, "camera", 6));
    CHECK(jipc_equal("same", "same")); CHECK(!jipc_equal("different", "differentX"));
    int value = 0; CHECK(!jipc_json_integer("{\"value\":17}", "value", &value)); CHECK(value == 17);
    CHECK(jipc_json_integer("{\"value\":1.5}", "value", &value) < 0);
    CHECK(jipc_json_integer("{\"value\":1,\"value\":2}", "value", &value) < 0);
    CHECK(jipc_json_integer("{} garbage", "value", &value) < 0);
    CHECK(jipc_json_string("{\"operation\":\"ptz\\u0000stop\"}", "operation", encoded, sizeof(encoded)) < 0);
    CHECK(jipc_json_string("{\"name\":\"camera\"}", "name", encoded, sizeof(encoded)) == 0);
    CHECK(!strcmp(encoded, "camera")); CHECK(jipc_json_string("{}", "name", encoded, sizeof(encoded)) == 1);
    char temporary[] = "/tmp/jipc-core-XXXXXX"; CHECK(mkdtemp(temporary) != NULL);
    jipc_app app; memset(&app, 0, sizeof(app)); jipc_config_defaults(&app.config);
    pthread_mutex_init(&app.config_mutex, NULL); pthread_mutex_init(&app.control_mutex, NULL);
    strcpy(app.config.password, "test-credential"); strcpy(app.config.replay[0], "/tmp/fixture.h264");
    app.config.flip0=3;app.config.flip1=1;
    app.config.width1=640;app.config.height1=360;app.config.bitrate1=768;
    app.config.sub_bitrate=256;app.config.gop_frames=15;
    CHECK(jipc_video_width(&app.config,0)==2304 && jipc_video_width(&app.config,2)==640);
    CHECK(jipc_video_height(&app.config,2)==360 && jipc_video_bitrate(&app.config,2)==768);
    CHECK(jipc_video_bitrate(&app.config,1)==256 && jipc_video_bitrate(&app.config,3)==256);
    snprintf(app.config.state_dir, sizeof(app.config.state_dir), "%s/state", temporary);
    snprintf(app.config.record_dir, sizeof(app.config.record_dir), "%s/recordings", temporary);
    snprintf(app.config.config_path, sizeof(app.config.config_path), "%s/settings.ini", temporary);
    char error[256]; CHECK(jipc_config_validate(&app.config, error, sizeof(error)) == 0);
    CHECK(jipc_config_save(&app.config) == 0); struct stat st; CHECK(!stat(app.config.config_path, &st)); CHECK((st.st_mode & 0777) == 0600);
    jipc_config loaded; jipc_config_defaults(&loaded); CHECK(!jipc_config_load(&loaded, app.config.config_path, error, sizeof(error)));
    CHECK(!strcmp(loaded.password, app.config.password)); CHECK(loaded.fps == 15);
    CHECK(loaded.flip0==3&&loaded.flip1==1);
    CHECK(loaded.width1==640&&loaded.height1==360&&loaded.bitrate1==768&&loaded.gop_frames==15);
    CHECK(jipc_config_json(&app, encoded, sizeof(encoded)) < 0);
    char json[8192]; CHECK(jipc_config_json(&app, json, sizeof(json)) > 0); CHECK(!strstr(json, "test-credential"));
    app.restart=0;
    CHECK(!jipc_config_patch(&app,"{\"main_only\":true,\"width\":1920,\"height\":1080}",error,sizeof(error)));
    CHECK(app.restart&&app.config.main_only);
    CHECK(jipc_video_width(&app.config,1)==1920&&jipc_video_height(&app.config,1)==1080);
    CHECK(jipc_video_width(&app.config,3)==640&&jipc_video_height(&app.config,3)==360);
    CHECK(jipc_video_bitrate(&app.config,1)==app.config.bitrate&&jipc_video_bitrate(&app.config,3)==768);
    CHECK(jipc_video_owner(&app.config,1)==0&&jipc_video_owner(&app.config,3)==2&&jipc_video_owner(&app.config,4)==4);
    jipc_config_defaults(&loaded);CHECK(!jipc_config_load(&loaded,app.config.config_path,error,sizeof(error))&&loaded.main_only);
    CHECK(jipc_config_patch(&app,"{\"sensors\":1}",error,sizeof(error))<0);
    CHECK(!jipc_config_patch(&app,"{\"main_only\":false}",error,sizeof(error)));
    CHECK(jipc_video_width(&app.config,1)==app.config.sub_width);
    CHECK(jipc_config_patch(&app, "{\"fps\":12,\"bitrate\":512}", error, sizeof(error)) == 0); CHECK(app.restart == 1);
    CHECK(jipc_config_patch(&app, "{\"fps\":0}", error, sizeof(error)) < 0); CHECK(app.config.fps == 12);
    CHECK(jipc_config_patch(&app, "{\"flip0\":4}", error, sizeof(error)) < 0); CHECK(app.config.flip0==3);
    CHECK(jipc_config_patch(&app, "{\"unknown\":1}", error, sizeof(error)) < 0);
    CHECK(jipc_config_patch(&app, "{\"password\":\"x\",\"password\":\"y\"}", error, sizeof(error)) < 0);
    CHECK(jipc_config_patch(&app, "{\"password\":\" secret \"}", error, sizeof(error)) < 0);
    CHECK(jipc_config_patch(&app, "{\"username\":\" admin\"}", error, sizeof(error)) < 0);
    CHECK(jipc_config_patch(&app, "{\"password\":\"bad\\nsetting=1\"}", error, sizeof(error)) < 0);
    CHECK(jipc_config_patch(&app, "{\"password\":\"secret with spaces\",\"username\":\"local user\"}", error, sizeof(error)) == 0);
    jipc_config_defaults(&loaded); CHECK(!jipc_config_load(&loaded, app.config.config_path, error, sizeof(error)));
    CHECK(!strcmp(loaded.password, app.config.password) && !strcmp(loaded.username, app.config.username));
    jipc_config invalid = app.config; strcpy(invalid.replay[0], "file\npassword=bad");
    CHECK(jipc_config_validate(&invalid, error, sizeof(error)) < 0);
    invalid = app.config; strcpy(invalid.bind, "::1");
    CHECK(jipc_config_validate(&invalid, error, sizeof(error)) < 0);
    /* Recreating the service state on reload must not forget accepted WSSE
     * tokens that remain valid in the five-minute timestamp window. */
    time_t now = time(NULL); struct tm utc; char created[32], nonce_text[64], digest_text[64], soap[1536];
    unsigned char nonce[16], digest[20], input[256];
    CHECK(gmtime_r(&now, &utc) != NULL);
    CHECK(strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0);
    CHECK(!jipc_random(nonce, sizeof(nonce)));
    size_t created_size = strlen(created), password_size = strlen(app.config.password);
    memcpy(input, nonce, sizeof(nonce)); memcpy(input + sizeof(nonce), created, created_size);
    memcpy(input + sizeof(nonce) + created_size, app.config.password, password_size);
    jipc_sha1(input, sizeof(nonce) + created_size + password_size, digest);
    CHECK(jipc_base64(nonce, sizeof(nonce), nonce_text, sizeof(nonce_text)) > 0);
    CHECK(jipc_base64(digest, sizeof(digest), digest_text, sizeof(digest_text)) > 0);
    snprintf(soap, sizeof(soap), "<Envelope><Header><Security><UsernameToken><Username>%s</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</Password>"
        "<Nonce>%s</Nonce><Created>%s</Created></UsernameToken></Security></Header>"
        "<Body><GetProfiles/></Body></Envelope>", app.config.username, digest_text, nonce_text, created);
    CHECK(jipc_onvif_authorize(&app, soap) == 1);
    jipc_app *reloaded = calloc(1, sizeof(*reloaded)); CHECK(reloaded != NULL);
    reloaded->config = app.config; pthread_mutex_init(&reloaded->config_mutex, NULL);
    CHECK(jipc_onvif_authorize(reloaded, soap) == 0);
    pthread_mutex_destroy(&reloaded->config_mutex); free(reloaded);
    CHECK(jipc_streams_init(&app) == 0);
    static const unsigned char access_unit[] = {0,0,0,1,0x67,0x42,0,0x1e,0x89,0,0,0,1,0x68,0xce,6,0xe2,0,0,0,1,0x65,0x80,0x7f};
    CHECK(!jipc_publish(&app, 0, access_unit, sizeof(access_unit), 1000, 1, JIPC_H264));
    unsigned char sps[512], pps[512]; size_t ss = sizeof(sps), ps = sizeof(pps);
    CHECK(!jipc_codec_parameters(&app, 0, sps, &ss, pps, &ps)); CHECK(ss == 5 && ps == 4);
    uint64_t cursor = 0; jipc_frame frame; CHECK(jipc_read(&app, 0, &cursor, &frame, 0) == 1);
    CHECK(frame.sequence == 1 && frame.keyframe && frame.size == sizeof(access_unit)); jipc_frame_free(&frame);
    CHECK(jipc_read(&app, 0, &cursor, &frame, 0) == 0);
    for (unsigned i = 0; i < JIPC_VIDEO_RING + 2; ++i) CHECK(!jipc_publish(&app, 0, access_unit, sizeof(access_unit), 2000 + i, 1, JIPC_H264));
    CHECK(jipc_read(&app, 0, &cursor, &frame, 0) == 1); CHECK(frame.sequence == 4); jipc_frame_free(&frame);
    CHECK(jipc_publish(&app, 5, access_unit, sizeof(access_unit), 0, 1, JIPC_H264) < 0);
    CHECK(jipc_publish(&app, 0, access_unit, JIPC_MAX_FRAME + 1u, 0, 1, JIPC_H264) < 0);
    CHECK(!jipc_platform_control(&app, "white_light", 1, 0, error, sizeof(error)));
    jipc_ptz_status status; CHECK(!jipc_platform_status(&app, &status, error, sizeof(error))); CHECK(status.white_light == 1);
    CHECK(jipc_platform_control(&app, "ptz", 101, 0, error, sizeof(error)) < 0);
    CHECK(jipc_platform_status_json(&app, json, sizeof(json)) > 0);
    ntp_checks(&app);
    jipc_platform_stop(&app); jipc_streams_destroy(&app);
    pthread_mutex_destroy(&app.control_mutex); pthread_mutex_destroy(&app.config_mutex);
    unlink(app.config.config_path); rmdir(app.config.state_dir); rmdir(temporary);
    printf("PASS: %u core checks\n", checks); return 0;
}
