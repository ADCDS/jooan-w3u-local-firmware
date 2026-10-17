/* SPDX-License-Identifier: MIT
 * Offline adapter contract tests. No test calls jipc_imp_open, dlopen, camera
 * devices or ioctls. Including the adapter exposes its private ABI boundaries;
 * SDK calls are replaced by explicit recording functions below.
 */
#define _GNU_SOURCE
#include "../src/imp_backend.c"
#include <sys/mman.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)
static int calls[128], call_count, destruction_failure;
static int release_count, release_result, send_count, send_result, free_audio_slots = 100;
static int stream_overwrite, audio_overwrite;
static int16_t last_audio[320];
static jipc_imp_stream fake_stream;
static jipc_imp_audio_frame fake_audio;
static int audio_available;

static void *low_memory(size_t bytes)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if UINTPTR_MAX > UINT32_MAX && defined(MAP_32BIT)
    flags |= MAP_32BIT;
#endif
    void *address = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    CHECK(address != MAP_FAILED);
    CHECK((uintptr_t)address <= UINT32_MAX);
    return address;
}
static int remember(int event) { CHECK(call_count < 128); calls[call_count++] = event; return 0; }
static int ao_channel_off(int device, int channel) { CHECK(!device && !channel); return remember(10); }
static int ao_off(int device) { CHECK(!device); return remember(11); }
static int ai_channel_off(int device, int channel) { CHECK(device == 1 && !channel); return remember(12); }
static int ai_off(int device) { CHECK(device == 1); return remember(13); }
static int encoder_stop(int channel) { return remember(100 + channel); }
static int source_off(int channel) { return remember(200 + channel); }
static int ordinary_sources;
static int unbind_cells(jipc_imp_cell *source, jipc_imp_cell *destination)
{
    CHECK(source->device == 0 && destination->device == 1 && !source->output && !destination->output);
    int expected_group = ordinary_sources ?
        (source->group == 1 ? 0 : source->group == 2 ? 2 : source->group == 4 ? 1 : 3) :
        (source->group == 0 ? 0 : source->group == 1 ? 2 : source->group == 3 ? 1 : 3);
    CHECK(destination->group == expected_group);
    return remember(300 + source->group);
}
static int encoder_unregister(int channel) { return remember(400 + channel); }
static int encoder_destroy(int channel) { remember(500 + channel); return destruction_failure ? -7 : 0; }
static int group_destroy(int group) { return remember(600 + group); }
static int source_destroy(int channel) { return remember(700 + channel); }
static int system_off(void) { return remember(800); }
static int sensors_off(void) { return remember(801); }
static int sensor_del(jipc_imp_sensor *sensor) { return remember(802 + sensor->sensor_id); }
static int tuning_off(void) { return remember(804); }
static int isp_off(void) { return remember(805); }

static imp_state *cleanup_state(void)
{
    imp_state *state = calloc(1, sizeof *state);
    CHECK(state);
    state->lock_fd = -1;
    state->ao_disable_channel = ao_channel_off; state->ao_disable = ao_off;
    state->ai_disable_channel = ai_channel_off; state->ai_disable = ai_off;
    state->stop = encoder_stop; state->fs_disable = source_off;
    state->unbind = unbind_cells; state->enc_unregister = encoder_unregister;
    state->enc_destroy = encoder_destroy; state->group_destroy = group_destroy;
    state->fs_destroy = source_destroy; state->system_exit = system_off;
    state->disable_sensor = sensors_off; state->del_sensor = sensor_del;
    state->disable_tuning = tuning_off; state->isp_close = isp_off;
    state->sensor[1].sensor_id = 1;
    return state;
}

static void test_cleanup(void)
{
    /* Recovered OEM shutdown dependencies, including sensor-ID order and the
     * private FS {0,1,3,4} / encoder {0,4,1,5} mapping. */
    static const int expected[] = {
        10,11,12,13,105,101,104,100,204,203,201,200,
        304,405,505,303,401,501,301,404,504,300,400,500,
        603,602,601,600,704,703,701,700,800,801,803,802,804,805
    };
    imp_state *state = cleanup_state();
    state->ao_channel = state->ao_enabled = state->ai_channel = state->ai_enabled = 1;
    state->system = state->enabled = state->tuning = state->isp = 1;
    state->added[0] = state->added[1] = 1;
    for (unsigned i = 0; i < 4; ++i)
        state->receiving[i] = state->fs_enabled[i] = state->bound[i] = state->registered[i] =
        state->enc_created[i] = state->groups[i] = state->fs_created[i] = 1;
    call_count = 0;
    jipc_imp_close(state);
    CHECK(call_count == (int)(sizeof expected / sizeof expected[0]));
    CHECK(!memcmp(calls, expected, sizeof expected));
    CHECK(!sdk_poisoned);

    /* Native ordinary outputs must stop before their global FS0 owner. */
    static const int ordinary[] = {
        105,101,104,100,205,204,202,201,200,
        305,405,505,304,401,501,302,404,504,301,400,500,
        603,602,601,600,705,704,702,701,700,800,801,803,802,804,805
    };
    state = cleanup_state(); ordinary_sources = 1;
    state->openimp_fullframe = state->workspace_created = state->workspace_enabled = 1;
    state->system = state->enabled = state->tuning = state->isp = 1;
    state->added[0] = state->added[1] = 1;
    for (unsigned i = 0; i < 4; ++i)
        state->receiving[i] = state->fs_enabled[i] = state->bound[i] = state->registered[i] =
        state->enc_created[i] = state->groups[i] = state->fs_created[i] = 1;
    call_count = 0; jipc_imp_close(state);
    CHECK(call_count == (int)(sizeof ordinary / sizeof ordinary[0]));
    CHECK(!memcmp(calls, ordinary, sizeof ordinary)); CHECK(!sdk_poisoned);
    ordinary_sources = 0;

    state = cleanup_state(); state->openimp_fullframe = state->workspace_created = state->isp = 1;
    call_count = 0; jipc_imp_close(state);
    CHECK(call_count == 2 && calls[0] == 700 && calls[1] == 805);

    /* A failure before registration must not invoke receive-stop, unbind or
     * unregister against resources never acquired. */
    state = cleanup_state();
    state->fs_created[0] = state->groups[0] = state->enc_created[0] = state->isp = 1;
    call_count = 0;
    destruction_failure = 1;
    jipc_imp_close(state);
    static const int partial[] = {500,600,700,805};
    CHECK(call_count == 4 && !memcmp(calls, partial, sizeof partial));
    CHECK(sdk_poisoned);
    imp_state poisoned = {0};
    char error[256] = {0};
    CHECK(jipc_imp_poll(NULL, &poisoned, 0, error, sizeof error) == -1);
    CHECK(strstr(error, "reset the camera"));
    sdk_poisoned = destruction_failure = 0;
}

static int get_stream(int channel, jipc_imp_stream *stream, int block)
{
    CHECK(channel == 4 && block == 0);
    *stream = fake_stream;
    if (stream_overwrite) ((unsigned char *)stream)[sizeof *stream] = 0;
    return 0;
}
static int release_stream(int channel, jipc_imp_stream *stream)
{
    CHECK(channel == 4 && stream->packs == fake_stream.packs);
    ++release_count;
    return release_result;
}

static void test_packs(void)
{
    unsigned char *memory = low_memory(4096);
    jipc_imp_pack *packs = (jipc_imp_pack *)memory;
    unsigned char *payload = memory + 256;
    static const unsigned char expected[] = {0,0,0,1,0x65,0x88,0,0,1,0x61,0x80};
    memcpy(payload, expected, sizeof expected);
    packs[0] = (jipc_imp_pack){.address = (uint32_t)(uintptr_t)payload, .length = 6};
    packs[1] = (jipc_imp_pack){.address = (uint32_t)(uintptr_t)(payload + 6), .length = sizeof expected - 6};
    fake_stream = (jipc_imp_stream){(uint32_t)(uintptr_t)packs, 2, 17, 0};
    imp_state state = {.frame = malloc(JIPC_MAX_FRAME), .get_stream = get_stream, .release_stream = release_stream};
    CHECK(state.frame);
    char error[256] = {0};
    jipc_frame frame = {0};
    release_count = release_result = 0;
    CHECK(!copy_stream(&state, 4, &frame, error, sizeof error));
    CHECK(release_count == 1 && frame.size == sizeof expected);
    CHECK(!memcmp(frame.data, expected, sizeof expected));
    packs[1].length = JIPC_MAX_FRAME + 1;
    CHECK(copy_stream(&state, 4, &frame, error, sizeof error) == -1);
    CHECK(release_count == 2); /* Even malformed SDK frames must be released. */
    packs[1].length = sizeof expected - 6;
    release_result = -1;
    CHECK(copy_stream(&state, 4, &frame, error, sizeof error) == -1);
    CHECK(release_count == 3 && strstr(error, "ReleaseStream"));
    release_result = 0;
    fake_stream.pack_count = 0;
    CHECK(copy_stream(&state, 4, &frame, error, sizeof error) == -1);
    CHECK(release_count == 4);
    fake_stream.pack_count = 2;
    stream_overwrite = 1;
    CHECK(copy_stream(&state, 4, &frame, error, sizeof error) == -1);
    CHECK(release_count == 5 && sdk_poisoned && strstr(error, "record ABI"));
    stream_overwrite = sdk_poisoned = 0;
    free(state.frame);
    CHECK(!munmap(memory, 4096));
}

static int audio_attributes(int device, jipc_imp_audio_attr *attribute)
{
    CHECK(device == 0 && attribute->sample_rate == 8000 && attribute->bit_width == 16);
    CHECK(attribute->samples == 320 && attribute->sound_mode == 1 && attribute->channels == 1);
    return 0;
}
static int audio_device(int device) { CHECK(!device); return 0; }
static int audio_channel(int device, int channel) { CHECK(!device && !channel); return 0; }
static int audio_send(int device, int channel, jipc_imp_audio_frame *frame, int block)
{
    CHECK(!device && !channel && block == 1 && frame->bytes == 640);
    CHECK(frame->bit_width == 16 && frame->sound_mode == 1);
    ++send_count;
    memcpy(last_audio, (void *)(uintptr_t)frame->address, sizeof last_audio);
    return send_result;
}
static int audio_queue(int device, int channel, int32_t *queue)
{
    CHECK(!device && !channel);
    queue[0] = 100; queue[1] = free_audio_slots; queue[2] = 100 - free_audio_slots;
    return 0;
}

static void test_speaker(void)
{
    CHECK(pcm_alaw(0) == 0xd5 && pcm_alaw(-1) == 0x55);
    CHECK(pcm_alaw(32767) == 0xaa && pcm_alaw(-32768) == 0x2a);
    CHECK(alaw_pcm(0xd5) == 8 && alaw_pcm(0x55) == -8);
    imp_state *state = low_memory(sizeof *state);
    state->audio = 1;
    state->ao_set_attr = audio_attributes; state->ao_enable = state->ao_disable = audio_device;
    state->ao_enable_channel = state->ao_disable_channel = audio_channel;
    state->ao_send = audio_send; state->ao_query = audio_queue;
    unsigned char pcma[320]; memset(pcma, 0xd5, sizeof pcma);
    char error[256] = {0};
    send_count = send_result = 0;
    CHECK(!jipc_imp_talk(state, pcma, 160, error, sizeof error));
    CHECK(!send_count && state->talk_count == 160);
    CHECK(!jipc_imp_talk(state, pcma, 160, error, sizeof error));
    CHECK(send_count == 1 && state->talk_count == 0);
    for (unsigned i = 0; i < 320; ++i) CHECK(last_audio[i] == 8);
    CHECK(!jipc_imp_talk(state, pcma, 160, error, sizeof error));
    state->talk_since = jipc_now_us() - 60000;
    state->next_owner_check = jipc_now_us(); /* Prevent unrelated /proc scans. */
    CHECK(!jipc_imp_poll(NULL, state, 0, error, sizeof error));
    CHECK(send_count == 2 && !state->talk_count);
    for (unsigned i = 0; i < 160; ++i) CHECK(last_audio[i] == 8);
    for (unsigned i = 160; i < 320; ++i) CHECK(last_audio[i] == 0);
    free_audio_slots = 0;
    CHECK(jipc_imp_talk(state, pcma, sizeof pcma, error, sizeof error) == -1);
    CHECK(!state->talk_count && send_count == 2);
    free_audio_slots = 100;
    send_result = -1;
    CHECK(jipc_imp_talk(state, pcma, sizeof pcma, error, sizeof error) == -1);
    CHECK(state->talk_count == 320);
    send_result = 0;
    state->talk_since = jipc_now_us() - 60000;
    CHECK(!jipc_imp_poll(NULL, state, 0, error, sizeof error));
    CHECK(!state->talk_count && send_count == 4);
    CHECK(!munmap(state, sizeof *state));
}

static int audio_get(int device, int channel, jipc_imp_audio_frame *frame, int block)
{
    CHECK(device == 1 && !channel && block == 1);
    if (!audio_available) return -1;
    audio_available = 0;
    *frame = fake_audio;
    if (audio_overwrite) ((unsigned char *)frame)[sizeof *frame] = 0;
    return 0;
}
static int audio_release(int device, int channel, jipc_imp_audio_frame *frame)
{
    CHECK(device == 1 && !channel && frame->address == fake_audio.address);
    ++release_count;
    return 0;
}

static void test_microphone(void)
{
    int16_t *pcm = low_memory(4096);
    fake_audio = (jipc_imp_audio_frame){.bit_width = 16, .sound_mode = 1,
        .address = (uint32_t)(uintptr_t)pcm, .bytes = 1280};
    jipc_app *app = calloc(1, sizeof *app); CHECK(app);
    app->config.fps = 15;
    CHECK(!jipc_streams_init(app));
    imp_state state = {.ai_channel = 1, .ai_get = audio_get, .ai_release = audio_release};
    state.last_audio = state.next_owner_check = jipc_now_us();
    char error[256] = {0};
    audio_available = 1; release_count = 0;
    CHECK(!jipc_imp_poll(app, &state, 0, error, sizeof error));
    CHECK(release_count == 1);
    jipc_frame frame = {0}; uint64_t cursor = 0;
    CHECK(jipc_read(app, JIPC_AUDIO_STREAM, &cursor, &frame, 0) == 1);
    CHECK(frame.codec == JIPC_PCMA && frame.size == 320);
    for (size_t i = 0; i < frame.size; ++i) CHECK(frame.data[i] == 0xd5);
    jipc_frame_free(&frame);
    audio_available = 1; fake_audio.bytes = 1282;
    CHECK(jipc_imp_poll(app, &state, 0, error, sizeof error) == -1);
    CHECK(release_count == 2);
    audio_available = audio_overwrite = 1;
    fake_audio.bytes = 1280;
    CHECK(jipc_imp_poll(app, &state, 0, error, sizeof error) == -1);
    CHECK(release_count == 3 && sdk_poisoned && strstr(error, "record ABI"));
    audio_overwrite = sdk_poisoned = 0;
    state.last_audio = jipc_now_us() - 16000000;
    CHECK(jipc_imp_poll(app, &state, 0, error, sizeof error) == -1);
    CHECK(strstr(error, "microphone"));
    jipc_streams_destroy(app); free(app);
    CHECK(!munmap(pcm, 4096));
}

static void test_provider(void)
{
    jipc_config config = {0};
    char error[512];
    config.sensors = 1;
    config.fps = 15;
    CHECK(!validate_provider("IMP-1.3.0", &config, error, sizeof error));
    CHECK(validate_provider("unknown", &config, error, sizeof error) == -1);
    unsetenv("OPENIMP_T23_DRIVER_ABI");
    unsetenv("OPENIMP_T23_ENCODER");
    unsetenv("OPENIMP_T23_NATIVE_FALLBACK");
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    setenv("OPENIMP_T23_DRIVER_ABI", "sdk88", 1);
    setenv("OPENIMP_T23_ENCODER", "native", 1);
    setenv("OPENIMP_T23_NATIVE_FALLBACK", "0", 1);
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    setenv("OPENIMP_T23_DRIVER_ABI", "oem80", 1);
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    setenv("OPENIMP_T23_NATIVE_FALLBACK", "1", 1);
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    setenv("OPENIMP_T23_NATIVE_FALLBACK", "0", 1);
    config.audio = 1;
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    config.audio = 0; config.sensors = 2;
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    config.width=1280;config.height=720;config.width1=640;config.height1=360;
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    config.width1=1280;config.height1=720;
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    config.fps = 10;
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    unsetenv("OPENIMP_T23_DRIVER_ABI");
    unsetenv("OPENIMP_T23_ENCODER");
    unsetenv("OPENIMP_T23_NATIVE_FALLBACK");
    CHECK(media_node("/dev/rmem") && media_node("/dev/soc_vpu"));
    CHECK(!media_node("/dev/watchdog"));
}

int main(void)
{
    test_provider(); test_cleanup(); test_packs(); test_speaker(); test_microphone();
    puts("IMP offline adapter contracts passed");
    return 0;
}
