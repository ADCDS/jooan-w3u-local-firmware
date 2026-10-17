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
int main(void)
{
    CHECK(sizeof(off_t) >= 8);
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
    for (unsigned i = 0; i < 6; ++i) CHECK(!jipc_publish(&app, 0, access_unit, sizeof(access_unit), 2000 + i, 1, JIPC_H264));
    CHECK(jipc_read(&app, 0, &cursor, &frame, 0) == 1); CHECK(frame.sequence == 4); jipc_frame_free(&frame);
    CHECK(jipc_publish(&app, 5, access_unit, sizeof(access_unit), 0, 1, JIPC_H264) < 0);
    CHECK(jipc_publish(&app, 0, access_unit, JIPC_MAX_FRAME + 1u, 0, 1, JIPC_H264) < 0);
    CHECK(!jipc_platform_control(&app, "white_light", 1, 0, error, sizeof(error)));
    jipc_ptz_status status; CHECK(!jipc_platform_status(&app, &status, error, sizeof(error))); CHECK(status.white_light == 1);
    CHECK(jipc_platform_control(&app, "ptz", 101, 0, error, sizeof(error)) < 0);
    CHECK(jipc_platform_status_json(&app, json, sizeof(json)) > 0);
    jipc_platform_stop(&app); jipc_streams_destroy(&app);
    pthread_mutex_destroy(&app.control_mutex); pthread_mutex_destroy(&app.config_mutex);
    unlink(app.config.config_path); rmdir(app.config.state_dir); rmdir(temporary);
    printf("PASS: %u core checks\n", checks); return 0;
}
