/* SPDX-License-Identifier: MIT
 * Runtime adapter for an externally supplied, ABI-compatible T23/Ace libimp.
 * No SDK implementation or vendor binaries are incorporated here.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "imp_abi.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*imp_zero)(void);
typedef int (*imp_one)(int);
typedef int (*imp_two)(int, int);
typedef int (*imp_byte)(unsigned char);
typedef struct {
    void *library;
    int lock_fd, isp, system, tuning, enabled, added[2];
    int fs_created[4], enc_created[4], registered[4], bound[4], fs_enabled[4], receiving[4];
    int groups[4], fds[4], ai_enabled, ai_channel, ao_enabled, ao_channel;
    unsigned sensors, sensor_count, width[2], height[2], sub_width, sub_height, fps, bitrate[4], gop;
    int audio;
    int openimp_fullframe, workspace_created, workspace_enabled;
    unsigned char *frame;
    uint64_t last_video[4], last_audio, next_owner_check;
    int16_t talk_buffer[320];
    size_t talk_count;
    uint64_t talk_since;
    jipc_imp_sensor sensor[2];
    int (*version)(void *);
    imp_zero isp_open, isp_close, enable_sensor, disable_sensor, system_init, system_exit;
    imp_zero enable_tuning, disable_tuning;
    int (*add_sensor)(jipc_imp_sensor *), (*del_sensor)(jipc_imp_sensor *);
    int (*stream_out)(int *);
    int (*set_fps)(unsigned, unsigned), (*set_fps_sec)(unsigned, unsigned);
    int (*fs_create)(int, jipc_imp_fs *), (*fs_set)(int, jipc_imp_fs *);
    imp_one fs_destroy, fs_enable, fs_disable;
    imp_two fs_direct;
    imp_one group_create, group_destroy, enc_destroy, enc_unregister, start, stop, get_fd, idr;
    int (*enc_create)(int, jipc_imp_encoder_attr *);
    imp_two enc_register;
    int (*bind)(jipc_imp_cell *, jipc_imp_cell *), (*unbind)(jipc_imp_cell *, jipc_imp_cell *);
    int (*get_stream)(int, jipc_imp_stream *, int), (*release_stream)(int, jipc_imp_stream *);
    int (*super_frame)(int, jipc_imp_super *);
    imp_one osd_pool, isp_pool, vpubs_pool, vb_pool;
    imp_byte brightness[2], contrast[2], saturation[2], sharpness[2];
    imp_one hflip[2], vflip[2], night[2];
    int (*ai_set_attr)(int, jipc_imp_audio_attr *), (*ao_set_attr)(int, jipc_imp_audio_attr *);
    imp_one ai_enable, ai_disable, ao_enable, ao_disable;
    int (*ai_set_channel)(int, int, jipc_imp_audio_channel *);
    imp_two ai_enable_channel, ai_disable_channel, ao_enable_channel, ao_disable_channel;
    int (*ai_get)(int, int, jipc_imp_audio_frame *, int);
    int (*ai_release)(int, int, jipc_imp_audio_frame *);
    int (*ao_send)(int, int, jipc_imp_audio_frame *, int);
    int (*ao_query)(int, int, int32_t *);
    int (*ai_volume)(int, int, int), (*ao_volume)(int, int, int);
} imp_state;

static const int fs_channels[4] = {0, 1, 3, 4};
static const int enc_channels[4] = {0, 4, 1, 5};
static const int enc_groups[4] = {0, 2, 1, 3};
static int source_channel(const imp_state *s, unsigned int stream)
{
    /* OEM direct_mode=2 uses line rings on FS0/3. Ordinary outputs 1/2
     * and 4/5 carry complete NV12 for OpenIMP's native encoder. */
    static const int ordinary[4] = {1, 2, 4, 5};
    return s->openimp_fullframe ? ordinary[stream] : fs_channels[stream];
}
/* Startup/shutdown are serialized by the application. A failed SDK teardown
 * prevents this process from reusing potentially live SDK global state. */
static int sdk_poisoned;

static int failure(char *out, size_t capacity, const char *format, ...)
{
    if (out && capacity) {
        va_list args;
        va_start(args, format);
        vsnprintf(out, capacity, format, args);
        va_end(args);
    }
    return -1;
}

static int load_symbol(void *library, const char *name, void *target, int required,
                        char *error, size_t capacity)
{
    void *symbol = dlsym(library, name);
    memcpy(target, &symbol, sizeof symbol);
    return required && !symbol ? failure(error, capacity, "libimp lacks required symbol %s", name) : 0;
}

static int media_node(const char *path)
{
    static const char *const names[] = {
        "/dev/tx-isp", "/dev/isp-m", "/dev/framechan", "/dev/misc-ivdc",
        "/dev/vpu", "/dev/jz-vpu", "/dev/ipu", "/dev/dsp", "/dev/audio",
        "/dev/jz-audio", "/dev/jz-dmic", "/dev/ingenic-vpu", "/dev/soc_vpu", "/dev/rmem"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (!strncmp(path, names[i], strlen(names[i]))) return 1;
    return 0;
}

/* Read-only refusal gate: this program never stops or signals the OEM app. */
static int exclusive_owner(char *error, size_t capacity)
{
    DIR *proc = opendir("/proc");
    if (!proc) return failure(error, capacity, "cannot inspect /proc for media owners");
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(proc))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid == (long)getpid()) continue;
        char path[128], name[64] = {0};
        snprintf(path, sizeof path, "/proc/%ld/comm", pid);
        FILE *comm = fopen(path, "r");
        if (comm) {
            if (fgets(name, sizeof name, comm) && !strncmp(name, "jooanipc", 8)) {
                fclose(comm);
                result = failure(error, capacity, "OEM jooanipc is running (pid %ld); media ownership must be arranged externally", pid);
                break;
            }
            fclose(comm);
        }
        snprintf(path, sizeof path, "/proc/%ld/fd", pid);
        DIR *fds = opendir(path);
        if (!fds) {
            if (errno == EACCES || errno == EPERM) {
                result = failure(error, capacity, "cannot verify media ownership for pid %ld; run hardware backend with sufficient permissions", pid);
                break;
            }
            continue;
        }
        struct dirent *fd;
        while ((fd = readdir(fds))) {
            if (fd->d_name[0] == '.') continue;
            char fdpath[384], target[512];
            snprintf(fdpath, sizeof fdpath, "/proc/%ld/fd/%s", pid, fd->d_name);
            ssize_t length = readlink(fdpath, target, sizeof target - 1);
            if (length < 0) continue;
            target[length] = '\0';
            if (media_node(target)) {
                result = failure(error, capacity, "media device %s is held by pid %ld", target, pid);
                break;
            }
        }
        closedir(fds);
        if (result) break;
    }
    closedir(proc);
    return result;
}

static int validate_elf(const char *path, char *error, size_t capacity)
{
    unsigned char header[52];
    FILE *file = fopen(path, "rb");
    if (!file) return failure(error, capacity, "cannot open libimp: %s", strerror(errno));
    size_t bytes = fread(header, 1, sizeof header, file);
    fclose(file);
    if (bytes != sizeof header || memcmp(header, "\177ELF", 4) ||
        header[4] != 1 || header[5] != 1 || header[18] != 8 || header[19] != 0 ||
        header[16] != 3 || header[17] != 0)
        return failure(error, capacity, "libimp must be an ELF32 little-endian MIPS shared library");
    uint32_t flags = (uint32_t)header[36] | (uint32_t)header[37] << 8 |
                     (uint32_t)header[38] << 16 | (uint32_t)header[39] << 24;
    if ((flags & 0x20) || ((flags & 0xf000) && (flags & 0xf000) != 0x1000))
        return failure(error, capacity, "libimp requires the MIPS o32 ABI");
    return 0;
}

#define REQUIRE(field, name) do { if (load_symbol(s->library, name, &s->field, 1, error, capacity)) return -1; } while (0)
#define OPTIONAL(field, name) do { load_symbol(s->library, name, &s->field, 0, error, capacity); } while (0)
static int load_sdk(imp_state *s, char *error, size_t capacity)
{
    REQUIRE(version, "IMP_System_GetVersion");
    REQUIRE(isp_open, "IMP_ISP_Open"); REQUIRE(isp_close, "IMP_ISP_Close");
    REQUIRE(add_sensor, "IMP_ISP_AddSensor"); REQUIRE(del_sensor, "IMP_ISP_DelSensor");
    REQUIRE(enable_sensor, "IMP_ISP_EnableSensor"); REQUIRE(disable_sensor, "IMP_ISP_DisableSensor");
    REQUIRE(system_init, "IMP_System_Init"); REQUIRE(system_exit, "IMP_System_Exit");
    REQUIRE(enable_tuning, "IMP_ISP_EnableTuning"); REQUIRE(disable_tuning, "IMP_ISP_DisableTuning");
    REQUIRE(fs_create, "IMP_FrameSource_CreateChn"); REQUIRE(fs_set, "IMP_FrameSource_SetChnAttr");
    REQUIRE(fs_destroy, "IMP_FrameSource_DestroyChn"); REQUIRE(fs_enable, "IMP_FrameSource_EnableChn");
    REQUIRE(fs_disable, "IMP_FrameSource_DisableChn"); REQUIRE(fs_direct, "IMP_FrameSource_SetDirectModeAttr");
    REQUIRE(group_create, "IMP_Encoder_CreateGroup"); REQUIRE(group_destroy, "IMP_Encoder_DestroyGroup");
    REQUIRE(enc_create, "IMP_Encoder_CreateChn"); REQUIRE(enc_destroy, "IMP_Encoder_DestroyChn");
    REQUIRE(enc_register, "IMP_Encoder_RegisterChn"); REQUIRE(enc_unregister, "IMP_Encoder_UnRegisterChn");
    REQUIRE(bind, "IMP_System_Bind"); REQUIRE(unbind, "IMP_System_UnBind");
    REQUIRE(start, "IMP_Encoder_StartRecvPic"); REQUIRE(stop, "IMP_Encoder_StopRecvPic");
    REQUIRE(get_fd, "IMP_Encoder_GetFd"); REQUIRE(get_stream, "IMP_Encoder_GetStream");
    REQUIRE(release_stream, "IMP_Encoder_ReleaseStream"); REQUIRE(idr, "IMP_Encoder_RequestIDR");
    REQUIRE(super_frame, "IMP_Encoder_SetSuperFrameCfg");
    OPTIONAL(stream_out, "IMP_ISP_SetStreamOut"); OPTIONAL(set_fps, "IMP_ISP_Tuning_SetSensorFPS");
    OPTIONAL(set_fps_sec, "IMP_ISP_Tuning_SetSensorFPS_Sec");
    OPTIONAL(osd_pool, "IMP_OSD_SetPoolSize"); OPTIONAL(isp_pool, "IMP_OSD_SetPoolSize_ISP");
    OPTIONAL(vpubs_pool, "IMP_Set_Vpubs_Size"); OPTIONAL(vb_pool, "IMP_Set_Vbmpool1_Size");
    const char *const suffix[2] = {"", "_Sec"};
    for (unsigned i = 0; i < 2; ++i) {
        char symbol[96];
#define TUNING(field, symbolname) do { snprintf(symbol, sizeof symbol, "IMP_ISP_Tuning_" symbolname "%s", suffix[i]); OPTIONAL(field[i], symbol); } while (0)
        TUNING(brightness, "SetBrightness"); TUNING(contrast, "SetContrast");
        TUNING(saturation, "SetSaturation"); TUNING(sharpness, "SetSharpness");
        TUNING(hflip, "SetISPHflip"); TUNING(vflip, "SetISPVflip"); TUNING(night, "SetISPRunningMode");
#undef TUNING
    }
    OPTIONAL(ai_set_attr, "IMP_AI_SetPubAttr"); OPTIONAL(ai_enable, "IMP_AI_Enable");
    OPTIONAL(ai_disable, "IMP_AI_Disable"); OPTIONAL(ai_set_channel, "IMP_AI_SetChnParam");
    OPTIONAL(ai_enable_channel, "IMP_AI_EnableChn"); OPTIONAL(ai_disable_channel, "IMP_AI_DisableChn");
    OPTIONAL(ai_get, "IMP_AI_GetFrame"); OPTIONAL(ai_release, "IMP_AI_ReleaseFrame");
    OPTIONAL(ai_volume, "IMP_AI_SetVol"); OPTIONAL(ao_volume, "IMP_AO_SetVol");
    OPTIONAL(ao_set_attr, "IMP_AO_SetPubAttr"); OPTIONAL(ao_enable, "IMP_AO_Enable");
    OPTIONAL(ao_disable, "IMP_AO_Disable"); OPTIONAL(ao_enable_channel, "IMP_AO_EnableChn");
    OPTIONAL(ao_disable_channel, "IMP_AO_DisableChn"); OPTIONAL(ao_send, "IMP_AO_SendFrame");
    OPTIONAL(ao_query, "IMP_AO_QueryChnStat");
    return 0;
}
#undef REQUIRE
#undef OPTIONAL

/* No work is undone unless its corresponding acquisition succeeded. */
static void cleanup_result(const char *operation, int result)
{
    if (result) {
        sdk_poisoned = 1;
        jipc_log("error", "libimp cleanup %s returned %d; verify driver detachment or reset the camera before reusing reserved memory", operation, result);
    }
}

void jipc_imp_close(void *state)
{
    imp_state *s = state;
    if (!s) return;
    if (s->ao_channel) cleanup_result("AO_DisableChn", s->ao_disable_channel(0, 0));
    if (s->ao_enabled) cleanup_result("AO_Disable", s->ao_disable(0));
    if (s->ai_channel) cleanup_result("AI_DisableChn", s->ai_disable_channel(1, 0));
    if (s->ai_enabled) cleanup_result("AI_Disable", s->ai_disable(1));
    for (int i = 3; i >= 0; --i)
        if (s->receiving[i]) cleanup_result("Encoder_StopRecvPic", s->stop(enc_channels[i]));
    for (int i = 3; i >= 0; --i)
        if (s->fs_enabled[i]) cleanup_result("FrameSource_DisableChn", s->fs_disable(source_channel(s, i)));
    if (s->workspace_enabled) cleanup_result("FrameSource_DisableWorkspace", s->fs_disable(0));
    for (int i = 3; i >= 0; --i) {
        jipc_imp_cell source = {0, source_channel(s, i), 0}, destination = {1, enc_groups[i], 0};
        if (s->bound[i]) cleanup_result("System_UnBind", s->unbind(&source, &destination));
        if (s->registered[i]) cleanup_result("Encoder_UnRegisterChn", s->enc_unregister(enc_channels[i]));
        if (s->enc_created[i]) cleanup_result("Encoder_DestroyChn", s->enc_destroy(enc_channels[i]));
    }
    for (int i = 3; i >= 0; --i)
        if (s->groups[i]) cleanup_result("Encoder_DestroyGroup", s->group_destroy(i));
    for (int i = 3; i >= 0; --i)
        if (s->fs_created[i]) cleanup_result("FrameSource_DestroyChn", s->fs_destroy(source_channel(s, i)));
    if (s->workspace_created) cleanup_result("FrameSource_DestroyWorkspace", s->fs_destroy(0));
    /* OEM shutdown order differs from generic SDK examples. */
    if (s->system) cleanup_result("System_Exit", s->system_exit());
    if (s->enabled) cleanup_result("ISP_DisableSensor", s->disable_sensor());
    for (int i = 1; i >= 0; --i)
        if (s->added[i]) cleanup_result("ISP_DelSensor", s->del_sensor(&s->sensor[i]));
    if (s->tuning) cleanup_result("ISP_DisableTuning", s->disable_tuning());
    if (s->isp) cleanup_result("ISP_Close", s->isp_close());
    /* Failed teardown can leave SDK threads alive. Preserve their code mapping
     * until process exit instead of unloading it underneath those threads. */
    if (s->library && !sdk_poisoned) dlclose(s->library);
    if (s->lock_fd >= 0) close(s->lock_fd);
    free(s->frame);
    free(s);
}

static int sdk_result(int rc, const char *operation, char *error, size_t capacity)
{
    return rc ? failure(error, capacity, "%s failed (%d)", operation, rc) : 0;
}

static int validate_provider(const char *version, const jipc_config *config,
                             char *error, size_t capacity)
{
    if (!strcmp(version, "libimp.so T23 openimp")) {
        const char *abi = getenv("OPENIMP_T23_DRIVER_ABI");
        const char *encoder = getenv("OPENIMP_T23_ENCODER");
        const char *fallback = getenv("OPENIMP_T23_NATIVE_FALLBACK");
        if (!abi || strcmp(abi, "oem80") || !encoder || strcmp(encoder, "native") ||
            !fallback || strcmp(fallback, "0"))
            return failure(error, capacity, "OpenIMP requires OPENIMP_T23_DRIVER_ABI=oem80, OPENIMP_T23_ENCODER=native and OPENIMP_T23_NATIVE_FALLBACK=0");
        if (config->audio)
            return failure(error, capacity, "OpenIMP bring-up requires audio disabled");
        if (config->fps != 15)
            return failure(error, capacity, "OpenIMP OEM sensor activation is qualified at 15 fps");
        if (config->width > 1280 || config->height > 720 ||
            (config->sensors == 2 && (jipc_video_width(config,0) != jipc_video_width(config,2) ||
             jipc_video_height(config,0) != jipc_video_height(config,2))))
            return failure(error, capacity, "OpenIMP dual-sensor capture requires matching main geometry up to 1280x720; asymmetric DMA layouts are not qualified");
        return 0;
    }
    if (!strstr(version, "1.3.0") && !strstr(version, "1.0.1"))
        return failure(error, capacity, "unrecognized libimp version: %.100s", version);
    return 0;
}

#define STEP(call, name) do { jipc_log("info", "libimp begin %s", name); if (sdk_result((call), name, error, capacity)) goto failed; } while (0)
int jipc_imp_open(jipc_app *app, void **out, char *error, size_t capacity)
{
    *out = NULL;
#if !defined(__mips__) || UINTPTR_MAX != UINT32_MAX || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    return failure(error, capacity, "IMP hardware backend requires native little-endian MIPS32/o32; use replay on this host");
#endif
    jipc_config config;
    jipc_config_copy(app, &config);
    if (sdk_poisoned)
        return failure(error, capacity, "a prior IMP teardown failed; verify driver detachment or reset the camera before hardware initialization");
    if (config.sensors < 1 || config.sensors > 2 || !config.libimp[0])
        return failure(error, capacity, "IMP requires one or two sensors and an explicit libimp path");
    if (config.width > 2304 || config.height > 1296 || config.width1 > 2304 || config.height1 > 1296 || config.sub_width > config.width ||
        config.sub_height > config.height || config.fps > 30)
        return failure(error, capacity, "video geometry/fps exceeds the recovered CV2005 profile");
    if (config.width % 16 || config.sub_width % 16 || config.height % 8 || config.sub_height % 8 || config.width1 % 16 || config.height1 % 8)
        return failure(error, capacity, "IMP crop/scaler profile requires widths divisible by 16 and heights divisible by 8");
    if (validate_elf(config.libimp, error, capacity) || exclusive_owner(error, capacity)) return -1;
    imp_state *s = calloc(1, sizeof *s);
    if (!s) return failure(error, capacity, "cannot allocate IMP backend");
    s->lock_fd = -1;
    s->sensors = config.sensors;
    for (unsigned i = 0; i < 2; ++i) { s->width[i] = jipc_video_width(&config, i*2); s->height[i] = jipc_video_height(&config, i*2); }
    s->sub_width = config.sub_width; s->sub_height = config.sub_height;
    s->fps = config.fps; s->audio = config.audio;
    s->gop = config.gop_frames ? config.gop_frames : config.fps * 2;
    for (unsigned i = 0; i < 4; ++i) s->bitrate[i] = jipc_video_bitrate(&config, i);
    s->frame = malloc(JIPC_MAX_FRAME);
    if (!s->frame) { failure(error, capacity, "cannot allocate encoder copy buffer"); goto failed; }
    char lock_path[640];
    snprintf(lock_path, sizeof lock_path, "%s/media.lock", config.state_dir);
    s->lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->lock_fd < 0 || flock(s->lock_fd, LOCK_EX | LOCK_NB)) {
        failure(error, capacity, "cannot acquire media ownership lock: %s", strerror(errno));
        goto failed;
    }
    char digest[65];
    if (jipc_sha256_file(config.libimp, digest)) {
        failure(error, capacity, "cannot hash external libimp");
        goto failed;
    }
    const char *expected = getenv("JIPC_LIBIMP_SHA256");
    if (expected && (strlen(expected) != 64 || !jipc_equal(expected, digest))) {
        failure(error, capacity, "external libimp SHA-256 does not match JIPC_LIBIMP_SHA256");
        goto failed;
    }
    jipc_log("info", "external libimp SHA-256 %s", digest);
    s->library = dlopen(config.libimp, RTLD_NOW | RTLD_LOCAL);
    if (!s->library) { failure(error, capacity, "dlopen libimp: %s", dlerror()); goto failed; }
    if (load_sdk(s, error, capacity)) goto failed;
    char version[128] = {0};
    STEP(s->version(version), "IMP_System_GetVersion");
    version[sizeof version - 1] = '\0';
    if (validate_provider(version, &config, error, capacity)) goto failed;
    /* This OEM module runs both AE sensor slots in direct_mode=2, even when
     * the consumer publishes only sensor 0. Leaving slot 1 unregistered lets
     * its firmware worker dereference a NULL sensor attribute. */
    s->openimp_fullframe = !strcmp(version, "libimp.so T23 openimp");
    s->sensor_count = s->openimp_fullframe ? 2 : s->sensors;
    jipc_log("info", "libimp version %s; required private ABI: FS84/encoder164/stream16/pack32", version);
    /* These are recovered allocator setters, not replacements for unknown
     * driver initialization. Absent optional setters retain the SDK defaults. */
    if (s->osd_pool) STEP(s->osd_pool(0x10000), "IMP_OSD_SetPoolSize");
    if (s->isp_pool) STEP(s->isp_pool(0x50000), "IMP_OSD_SetPoolSize_ISP");
    if (s->vpubs_pool) STEP(s->vpubs_pool(0x80000), "IMP_Set_Vpubs_Size");
    if (s->vb_pool) STEP(s->vb_pool(0xac800), "IMP_Set_Vbmpool1_Size");
    if (exclusive_owner(error, capacity)) goto failed;
    STEP(s->isp_open(), "IMP_ISP_Open"); s->isp = 1;
    for (unsigned i = 0; i < s->sensor_count; ++i) {
        jipc_imp_sensor *sensor = &s->sensor[i];
        snprintf(sensor->name, sizeof sensor->name, "%s", i ? "cv2005s1" : "cv2005");
        snprintf(sensor->i2c_name, sizeof sensor->i2c_name, "%s", sensor->name);
        sensor->sensor_id = (uint16_t)i;
        sensor->bus_type = 1;
        sensor->i2c_address = i ? 0x36 : 0x35;
        sensor->i2c_adapter = 0;
        jipc_sleep_ms(1000);
        STEP(s->add_sensor(sensor), "IMP_ISP_AddSensor"); s->added[i] = 1;
    }
    STEP(s->enable_sensor(), "IMP_ISP_EnableSensor"); s->enabled = 1;
    if (s->sensor_count == 1) {
        if (!s->stream_out) { failure(error, capacity, "single sensor mode requires IMP_ISP_SetStreamOut"); goto failed; }
        int stream_out = 1;
        STEP(s->stream_out(&stream_out), "IMP_ISP_SetStreamOut");
    }
    STEP(s->system_init(), "IMP_System_Init"); s->system = 1;
    STEP(s->enable_tuning(), "IMP_ISP_EnableTuning"); s->tuning = 1;
    if (s->set_fps) STEP(s->set_fps(s->fps, 1), "IMP_ISP_Tuning_SetSensorFPS");
    if (s->sensor_count == 2 && s->set_fps_sec) STEP(s->set_fps_sec(s->fps, 1), "IMP_ISP_Tuning_SetSensorFPS_Sec");
    for (unsigned sensor = 0; sensor < s->sensors; ++sensor) {
        unsigned flip = sensor ? config.flip1 : config.flip0;
        if (flip && (!s->hflip[sensor] || !s->vflip[sensor])) { failure(error, capacity, "SDK lacks configured sensor flip controls"); goto failed; }
        if (s->hflip[sensor]) STEP(s->hflip[sensor](flip & 1), "IMP_ISP_Tuning_SetISPHflip");
        if (s->vflip[sensor]) STEP(s->vflip[sensor]((flip >> 1) & 1), "IMP_ISP_Tuning_SetISPVflip");
    }
    if (s->openimp_fullframe) {
        jipc_imp_fs holder = {.width = 640, .height = 360, .pixel_format = 10,
            .scaler_enable = 1, .scaler_width = 640, .scaler_height = 360,
            .fps_num = s->fps, .fps_den = 1, .buffer_count = 1};
        STEP(s->fs_create(0, &holder), "IMP_FrameSource_CreateWorkspace"); s->workspace_created = 1;
        STEP(s->fs_set(0, &holder), "IMP_FrameSource_SetWorkspace");
    }
    for (unsigned i = 0; i < s->sensors * 2; ++i) {
        unsigned width = i % 2 ? s->sub_width : s->width[i/2];
        unsigned height = i % 2 ? s->sub_height : s->height[i/2];
        jipc_imp_fs fs = {
            .width = width, .height = height, .pixel_format = 10,
            .crop_enable = !s->openimp_fullframe, .crop_width = width, .crop_height = height,
            .scaler_enable = 1, .scaler_width = width, .scaler_height = height,
            .fps_num = s->fps, .fps_den = 1, .buffer_count = 1, .channel_type = 0
        };
        STEP(s->fs_create(source_channel(s, i), &fs), "IMP_FrameSource_CreateChn"); s->fs_created[i] = 1;
        STEP(s->fs_set(source_channel(s, i), &fs), "IMP_FrameSource_SetChnAttr");
    }
    for (unsigned i = 0; i < 4; ++i) { STEP(s->group_create(i), "IMP_Encoder_CreateGroup"); s->groups[i] = 1; }
    for (unsigned i = 0; i < s->sensors * 2; ++i) {
        jipc_imp_encoder_attr encoder = {{0}};
        uint32_t *w = encoder.words;
        w[0] = 1; /* Recovered payload enum: H.264. */
        w[3] = i % 2 ? s->sub_width : s->width[i/2];
        w[4] = i % 2 ? s->sub_height : s->height[i/2];
        w[12] = s->fps; w[13] = 1; w[14] = s->gop;
        /* OEM rate-control mode 1 branch, offsets +0x3c..+0x54. */
        w[15] = 1; w[16] = 45; w[17] = 15;
        w[18] = s->bitrate[i];
        w[20] = 3; w[21] = 15;
        w[40] = !s->openimp_fullframe && !(i % 2) && s->sensors == 2; /* bEnableIvdc, byte +0xa0. */
        STEP(s->enc_create(enc_channels[i], &encoder), "IMP_Encoder_CreateChn"); s->enc_created[i] = 1;
        STEP(s->enc_register(enc_groups[i], enc_channels[i]), "IMP_Encoder_RegisterChn"); s->registered[i] = 1;
        jipc_imp_cell source = {0, source_channel(s, i), 0}, destination = {1, enc_groups[i], 0};
        STEP(s->bind(&source, &destination), "IMP_System_Bind"); s->bound[i] = 1;
    }
    if (s->openimp_fullframe) {
        STEP(s->fs_enable(0), "IMP_FrameSource_EnableWorkspace"); s->workspace_enabled = 1;
    }
    for (unsigned i = 0; i < s->sensors * 2; ++i) {
        if (!s->openimp_fullframe && !(i % 2) && s->sensors == 2)
            STEP(s->fs_direct(source_channel(s, i), 540), "IMP_FrameSource_SetDirectModeAttr");
        STEP(s->fs_enable(source_channel(s, i)), "IMP_FrameSource_EnableChn"); s->fs_enabled[i] = 1;
    }
    /* Finish configuring the shared ISP outputs before any GetFd pump can
     * allocate/run the VPU concurrently with a later channel's STREAMON. */
    for (unsigned i = 0; i < s->sensors * 2; ++i) {
        unsigned max_bytes = i % 2 ? 0x10000 : 0x40000;
        jipc_imp_super super = {2, max_bytes * 8, max_bytes * 80 / 14, 0, 2};
        STEP(s->super_frame(enc_channels[i], &super), "IMP_Encoder_SetSuperFrameCfg");
        STEP(s->start(enc_channels[i]), "IMP_Encoder_StartRecvPic"); s->receiving[i] = 1;
        s->fds[i] = s->get_fd(enc_channels[i]);
        if (s->fds[i] < 0) { failure(error, capacity, "IMP_Encoder_GetFd(%d) failed", enc_channels[i]); goto failed; }
        s->last_video[i] = jipc_now_us();
    }
    if (s->audio) {
        if (!s->ai_set_attr || !s->ai_enable || !s->ai_disable || !s->ai_set_channel ||
            !s->ai_enable_channel || !s->ai_disable_channel || !s->ai_get || !s->ai_release) {
            failure(error, capacity, "audio enabled but required IMP_AI symbols are missing"); goto failed;
        }
        jipc_imp_audio_attr audio = {16000, 16, 1, 40, 640, 1};
        jipc_imp_audio_channel channel = {20, 0};
        STEP(s->ai_set_attr(1, &audio), "IMP_AI_SetPubAttr");
        STEP(s->ai_enable(1), "IMP_AI_Enable"); s->ai_enabled = 1;
        STEP(s->ai_set_channel(1, 0, &channel), "IMP_AI_SetChnParam");
        STEP(s->ai_enable_channel(1, 0), "IMP_AI_EnableChn"); s->ai_channel = 1;
    }
    s->last_audio = s->next_owner_check = jipc_now_us();
    /* A successfully created graph is insufficient evidence of usable media.
     * Require every configured stream (and microphone, when enabled) to yield
     * data before reporting readiness to HTTP/RTSP. */
    uint64_t ready_deadline = jipc_now_us() + 5000000;
    for (;;) {
        if (app->stop || jipc_imp_poll(app, s, 20, error, capacity)) goto failed;
        int ready = 1;
        for (unsigned i = 0; i < s->sensors * 2; ++i) {
            pthread_mutex_lock(&app->streams[i].mutex);
            ready &= app->streams[i].sequence != 0 && app->streams[i].sps_size != 0 && app->streams[i].pps_size != 0;
            pthread_mutex_unlock(&app->streams[i].mutex);
        }
        if (s->audio) {
            pthread_mutex_lock(&app->streams[JIPC_AUDIO_STREAM].mutex);
            ready &= app->streams[JIPC_AUDIO_STREAM].sequence != 0;
            pthread_mutex_unlock(&app->streams[JIPC_AUDIO_STREAM].mutex);
        }
        if (ready) break;
        if (jipc_now_us() >= ready_deadline) {
            failure(error, capacity, "IMP graph did not deliver all configured H.264 parameter sets/video/audio within 5 seconds");
            goto failed;
        }
    }
    *out = s;
    return 0;
failed:
    jipc_imp_close(s);
    return -1;
}
#undef STEP

static int copy_stream(imp_state *s, int channel, jipc_frame *frame,
                        char *error, size_t capacity)
{
    struct {
        jipc_imp_stream stream;
        unsigned char guard[256 - sizeof(jipc_imp_stream)];
    } output;
    memset(&output.stream, 0, sizeof output.stream);
    memset(output.guard, 0xa5, sizeof output.guard);
    int rc = s->get_stream(channel, &output.stream, 0);
    if (rc) return failure(error, capacity, "IMP_Encoder_GetStream(%d) failed (%d)", channel, rc);
    int abi_mismatch = 0;
    for (size_t i = 0; i < sizeof output.guard; ++i)
        abi_mismatch |= output.guard[i] != 0xa5;
    if (abi_mismatch) {
        /* SDK release receives exactly the same original record, including any
         * larger-layout fields it wrote into the bounded guard area. */
        s->release_stream(channel, &output.stream);
        sdk_poisoned = 1;
        return failure(error, capacity, "libimp encoder stream record ABI exceeds the recovered 16-byte layout");
    }
    jipc_imp_stream *stream = &output.stream;
    size_t bytes = 0;
    int valid = stream->packs && stream->pack_count && stream->pack_count <= 4096;
    jipc_imp_pack *packs = (jipc_imp_pack *)(uintptr_t)stream->packs;
    if (valid) {
        for (uint32_t i = 0; i < stream->pack_count; ++i) {
            if (!packs[i].address || packs[i].length > JIPC_MAX_FRAME - bytes) { valid = 0; break; }
            memcpy(s->frame + bytes, (const void *)(uintptr_t)packs[i].address, packs[i].length);
            bytes += packs[i].length;
        }
    }
    /* Release even when validation or allocation fails. SDK owns pack memory. */
    int release = s->release_stream(channel, stream);
    if (!valid || !bytes) return failure(error, capacity, "invalid/oversized IMP encoder frame on channel %d", channel);
    if (release) return failure(error, capacity, "IMP_Encoder_ReleaseStream(%d) failed (%d)", channel, release);
    frame->data = s->frame;
    frame->size = bytes;
    frame->pts_us = jipc_now_us();
    return 0;
}

/* G.711 A-law is implemented directly from its piecewise quantizer definition. */
static unsigned char pcm_alaw(int sample)
{
    unsigned mask = sample >= 0 ? 0xd5 : 0x55;
    unsigned magnitude = sample >= 0 ? (unsigned)sample : (unsigned)(-sample - 1);
    if (magnitude > 32767) magnitude = 32767;
    unsigned segment = 0;
    for (unsigned threshold = 256; segment < 7 && magnitude >= threshold; threshold <<= 1) ++segment;
    unsigned quantized = segment ? magnitude >> (segment + 3) : magnitude >> 4;
    return (unsigned char)(((segment << 4) | (quantized & 15)) ^ mask);
}

static int alaw_pcm(unsigned char sample)
{
    unsigned value = sample ^ 0x55;
    unsigned segment = (value >> 4) & 7;
    int magnitude = (int)((value & 15) << 4) + 8;
    if (segment) magnitude += 256;
    if (segment > 1) magnitude <<= segment - 1;
    return value & 128 ? magnitude : -magnitude;
}

static int speaker_flush(imp_state *s)
{
    if (!s->talk_count) return 0;
    memset(s->talk_buffer + s->talk_count, 0,
           (320 - s->talk_count) * sizeof s->talk_buffer[0]);
    jipc_imp_audio_frame frame = {.bit_width = 16, .sound_mode = 1,
        .address = (uint32_t)(uintptr_t)s->talk_buffer, .bytes = sizeof s->talk_buffer};
    int rc = s->ao_send(0, 0, &frame, 1);
    if (!rc) s->talk_count = 0;
    return rc;
}

static int audio_output_open(imp_state *s, char *error, size_t capacity)
{
    if (!s->audio) return failure(error, capacity, "audio is disabled");
    if (!s->ao_set_attr || !s->ao_enable || !s->ao_disable || !s->ao_enable_channel ||
        !s->ao_disable_channel || !s->ao_send)
        return failure(error, capacity, "SDK lacks required audio output symbols");
    if (!s->ao_enabled) {
        jipc_imp_audio_attr attribute = {8000, 16, 1, 20, 320, 1};
        if (sdk_result(s->ao_set_attr(0, &attribute), "IMP_AO_SetPubAttr", error, capacity)) return -1;
        if (sdk_result(s->ao_enable(0), "IMP_AO_Enable", error, capacity)) return -1;
        s->ao_enabled = 1;
    }
    if (!s->ao_channel) {
        if (sdk_result(s->ao_enable_channel(0, 0), "IMP_AO_EnableChn", error, capacity)) return -1;
        s->ao_channel = 1;
    }
    return 0;
}

/* Descriptor identities are immutable until media shutdown joins its thread.
 * This OS wait makes no SDK calls and must run outside control_mutex. */
int jipc_imp_wait(void *state, unsigned timeout_ms)
{
    imp_state *s = state;
    struct pollfd fds[4];
    for (unsigned i = 0; i < s->sensors * 2; ++i) fds[i] = (struct pollfd){s->fds[i], POLLIN, 0};
    int rc = poll(fds, s->sensors * 2, (int)timeout_ms);
    return rc < 0 && errno != EINTR ? -1 : 0;
}

int jipc_imp_poll(jipc_app *app, void *state, unsigned timeout_ms,
                  char *error, size_t capacity)
{
    imp_state *s = state;
    if (sdk_poisoned) return failure(error, capacity, "IMP SDK cleanup failed; verify driver detachment or reset the camera before reusing hardware");
    struct pollfd fds[4];
    unsigned count = s->sensors * 2;
    for (unsigned i = 0; i < count; ++i) fds[i] = (struct pollfd){s->fds[i], POLLIN, 0};
    int rc = poll(fds, count, (int)timeout_ms);
    if (rc < 0 && errno != EINTR) return failure(error, capacity, "encoder poll: %s", strerror(errno));
    uint64_t now = jipc_now_us();
    for (unsigned i = 0; i < count; ++i) {
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            return failure(error, capacity, "encoder fd failed for stream %u", i);
        if (fds[i].revents & POLLIN) {
            jipc_frame frame = {0};
            if (copy_stream(s, enc_channels[i], &frame, error, capacity)) return -1;
            if (jipc_publish(app, i, frame.data, frame.size, frame.pts_us, 0, JIPC_H264))
                return failure(error, capacity, "cannot publish hardware video stream %u", i);
            s->last_video[i] = now;
        }
        if (now - s->last_video[i] > 15000000)
            return failure(error, capacity, "no encoded video on stream %u for 15 seconds", i);
    }
    if (s->ai_channel) {
        struct {
            jipc_imp_audio_frame frame;
            unsigned char guard[256 - sizeof(jipc_imp_audio_frame)];
        } output;
        memset(&output.frame, 0, sizeof output.frame);
        memset(output.guard, 0xa5, sizeof output.guard);
        /* Audio nonblocking enum is 1; the video API uses a different boolean. */
        if (!s->ai_get(1, 0, &output.frame, 1)) {
            int abi_mismatch = 0;
            for (size_t i = 0; i < sizeof output.guard; ++i)
                abi_mismatch |= output.guard[i] != 0xa5;
            if (abi_mismatch) {
                s->ai_release(1, 0, &output.frame);
                sdk_poisoned = 1;
                return failure(error, capacity, "libimp audio frame record ABI exceeds the recovered 32-byte layout");
            }
            jipc_imp_audio_frame *frame = &output.frame;
            unsigned char encoded[4096];
            size_t samples = frame->bytes / 4; /* average pairs: 16 kHz -> 8 kHz */
            int valid = frame->address && frame->bit_width == 16 && frame->sound_mode == 1 &&
                        !(frame->bytes % 4) && samples && samples <= sizeof encoded;
            if (valid) {
                const int16_t *pcm = (const int16_t *)(uintptr_t)frame->address;
                for (size_t i = 0; i < samples; ++i)
                    encoded[i] = pcm_alaw(((int)pcm[i * 2] + pcm[i * 2 + 1]) / 2);
            }
            int released = s->ai_release(1, 0, frame);
            if (!valid || released) return failure(error, capacity, "invalid microphone frame or IMP_AI_ReleaseFrame failure");
            if (jipc_publish(app, JIPC_AUDIO_STREAM, encoded, samples, now, 1, JIPC_PCMA))
                return failure(error, capacity, "cannot publish microphone audio");
            s->last_audio = now;
        }
        if (now - s->last_audio > 15000000)
            return failure(error, capacity, "no microphone audio for 15 seconds");
    }
    /* Preserve 20 ms HTTP chunks across requests instead of padding each chunk
     * into a 40 ms SDK frame. Flush a final partial chunk after a brief idle. */
    if (s->talk_count && now - s->talk_since >= 50000 && speaker_flush(s) &&
        now - s->talk_since >= 5000000)
        return failure(error, capacity, "speaker queue has refused pending audio for 5 seconds");
    if (now - s->next_owner_check >= 5000000) {
        s->next_owner_check = now;
        if (exclusive_owner(error, capacity)) return -1;
    }
    return 0;
}

int jipc_imp_control(void *state, const char *operation, int value, int sensor,
                     char *error, size_t capacity)
{
    imp_state *s = state;
    if (!strcmp(operation, "idr")) {
        if (value < 0 || (unsigned)value >= s->sensors * 2)
            return failure(error, capacity, "invalid stream channel");
        return sdk_result(s->idr(enc_channels[value]), "IMP_Encoder_RequestIDR", error, capacity);
    }
    if (!strcmp(operation, "isp_night")) {
        if (value != 0 && value != 1) return failure(error, capacity, "night mode requires 0 (day) or 1 (night); automatic exposure mode is unsupported");
        for (unsigned i = 0; i < s->sensors; ++i) {
            if (!s->night[i]) return failure(error, capacity, "SDK lacks sensor %u running-mode control", i);
            if (sdk_result(s->night[i](value), "IMP_ISP_Tuning_SetISPRunningMode", error, capacity)) return -1;
        }
        return 0;
    }
    if (!strcmp(operation, "volume")) {
        if (value < -30 || value > 120 || (sensor != 0 && sensor != 1))
            return failure(error, capacity, "volume requires -30..120 and target 0 (microphone) or 1 (speaker)");
        if (!sensor && s->ai_volume && s->ai_channel)
            return sdk_result(s->ai_volume(1, 0, value), "IMP_AI_SetVol", error, capacity);
        if (sensor && s->ao_volume) {
            if (audio_output_open(s, error, capacity)) return -1;
            return sdk_result(s->ao_volume(0, 0, value), "IMP_AO_SetVol", error, capacity);
        }
        return failure(error, capacity, "requested audio channel is not enabled or SDK lacks volume control");
    }
    if (sensor < 0 || (unsigned)sensor >= s->sensors) return failure(error, capacity, "invalid sensor index");
    imp_byte setter = NULL;
    if (!strcmp(operation, "brightness")) setter = s->brightness[sensor];
    else if (!strcmp(operation, "contrast")) setter = s->contrast[sensor];
    else if (!strcmp(operation, "saturation")) setter = s->saturation[sensor];
    else if (!strcmp(operation, "sharpness")) setter = s->sharpness[sensor];
    else if (!strcmp(operation, "hflip") || !strcmp(operation, "vflip")) {
        if (value != 0 && value != 1) return failure(error, capacity, "flip value must be 0 or 1");
        imp_one flip = !strcmp(operation, "hflip") ? s->hflip[sensor] : s->vflip[sensor];
        return flip ? sdk_result(flip(value), operation, error, capacity) : failure(error, capacity, "SDK lacks requested flip control");
    } else if (!strcmp(operation, "flip")) {
        if (value < 0 || value > 3 || !s->hflip[sensor] || !s->vflip[sensor])
            return failure(error, capacity, "flip requires bitmask 0..3 and both SDK controls");
        if (sdk_result(s->hflip[sensor](value & 1), "horizontal flip", error, capacity)) return -1;
        return sdk_result(s->vflip[sensor]((value >> 1) & 1), "vertical flip", error, capacity);
    } else return failure(error, capacity, "unsupported ISP control");
    if (!setter) return failure(error, capacity, "SDK lacks requested image control");
    if (value < 0 || value > 255) return failure(error, capacity, "image control must be in 0..255");
    return sdk_result(setter((unsigned char)value), operation, error, capacity);
}

int jipc_imp_snapshot(void *state, unsigned channel, jipc_frame *frame,
                      char *error, size_t capacity)
{
    imp_state *s = state;
    if (channel >= s->sensors * 2) return failure(error, capacity, "invalid snapshot channel");
    /* The recovered JPEG path is attached to each sensor's substream group.
     * It shares that group's input with H.264, using encoder 2 or 3. */
    unsigned sensor = channel / 2;
    int encoder_channel = 2 + (int)sensor, group = 2 + (int)sensor;
    jipc_imp_encoder_attr attribute = {{0}};
    attribute.words[3] = s->sub_width;
    attribute.words[4] = s->sub_height; /* payload enum zero is JPEG. */
    int created = 0, registered = 0, receiving = 0, result = -1;
    if (sdk_result(s->enc_create(encoder_channel, &attribute), "JPEG IMP_Encoder_CreateChn", error, capacity)) goto done;
    created = 1;
    if (sdk_result(s->enc_register(group, encoder_channel), "JPEG IMP_Encoder_RegisterChn", error, capacity)) goto done;
    registered = 1;
    if (sdk_result(s->start(encoder_channel), "JPEG IMP_Encoder_StartRecvPic", error, capacity)) goto done;
    receiving = 1;
    int fd = s->get_fd(encoder_channel);
    if (fd < 0) { failure(error, capacity, "JPEG encoder fd is unavailable"); goto done; }
    struct pollfd pollfd = {fd, POLLIN, 0};
    int rc;
    do { rc = poll(&pollfd, 1, 2000); } while (rc < 0 && errno == EINTR);
    if (rc <= 0 || !(pollfd.revents & POLLIN)) { failure(error, capacity, "JPEG encoder timed out or failed"); goto done; }
    jipc_frame image = {0};
    if (copy_stream(s, encoder_channel, &image, error, capacity)) goto done;
    if (image.size < 4 || image.data[0] != 0xff || image.data[1] != 0xd8 ||
        image.data[image.size - 2] != 0xff || image.data[image.size - 1] != 0xd9) {
        failure(error, capacity, "encoder returned an invalid JPEG image"); goto done;
    }
    frame->data = malloc(image.size);
    if (!frame->data) { failure(error, capacity, "cannot allocate JPEG response"); goto done; }
    memcpy(frame->data, image.data, image.size);
    frame->size = image.size; frame->pts_us = image.pts_us;
    frame->codec = JIPC_JPEG; frame->keyframe = 1;
    result = 0;
done:
    if (receiving) {
        int rc = s->stop(encoder_channel);
        cleanup_result("JPEG encoder stop", rc);
        if (rc) result = failure(error, capacity, "JPEG encoder stop failed");
    }
    if (registered) {
        int rc = s->enc_unregister(encoder_channel);
        cleanup_result("JPEG encoder unregister", rc);
        if (rc) result = failure(error, capacity, "JPEG encoder unregister failed");
    }
    if (created) {
        int rc = s->enc_destroy(encoder_channel);
        cleanup_result("JPEG encoder destroy", rc);
        if (rc) result = failure(error, capacity, "JPEG encoder destroy failed");
    }
    if (result) jipc_frame_free(frame);
    return result;
}

int jipc_imp_talk(void *state, const void *data, size_t bytes,
                  char *error, size_t capacity)
{
    imp_state *s = state;
    if (!s->audio) return failure(error, capacity, "audio is disabled");
    if (!data || !bytes || bytes > 8000) return failure(error, capacity, "speaker input requires 1..8000 PCMA bytes (8 kHz mono)");
    if (audio_output_open(s, error, capacity)) return -1;
    if (s->ao_query) {
        int32_t queue[3] = {0};
        if (sdk_result(s->ao_query(0, 0, queue), "IMP_AO_QueryChnStat", error, capacity)) return -1;
        size_t required = (s->talk_count + bytes) / 320;
        if (queue[1] < 0 || required > (size_t)queue[1])
            return failure(error, capacity, "speaker queue lacks room for this PCMA chunk; send shorter chunks");
    }
    const unsigned char *alaw = data;
    s->talk_since = jipc_now_us();
    for (size_t offset = 0; offset < bytes; ++offset) {
        if (s->talk_count == 320 && speaker_flush(s))
            return failure(error, capacity, "IMP_AO_SendFrame refused audio; speaker queue may be full");
        s->talk_buffer[s->talk_count++] = (int16_t)alaw_pcm(alaw[offset]);
        if (s->talk_count == 320 && speaker_flush(s))
            return failure(error, capacity, "IMP_AO_SendFrame refused audio; speaker queue may be full");
    }
    return 0;
}
