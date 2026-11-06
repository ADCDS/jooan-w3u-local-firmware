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
static atomic_int audio_mode; /* 1: the fake microphone delivers frames */
static int16_t last_audio[320];
static atomic_int ai_gain_set = -1, ai_volume_set = -1, ao_gain_set = -1;
static int fake_ai_gain(int d, int c, int v) { CHECK(d == 1 && !c); atomic_store(&ai_gain_set, v); return 0; }
static int fake_ai_volume(int d, int c, int v) { CHECK(d == 1 && !c); atomic_store(&ai_volume_set, v); return 0; }
static int fake_ao_gain(int d, int c, int v) { CHECK(!d && !c); atomic_store(&ao_gain_set, v); return 0; }
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
    int expected_group = ordinary_sources == 2 ? (source->group == 1 ? 0 : 1) : ordinary_sources ?
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
    CHECK(!device && !channel && block == 0 && frame->bytes == 640);
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
    CHECK(jipc_alaw_encode(0) == 0xd5 && jipc_alaw_encode(-1) == 0x55);
    CHECK(jipc_alaw_encode(32767) == 0xaa && jipc_alaw_encode(-32768) == 0x2a);
    CHECK(jipc_alaw_decode(0xd5) == 8 && jipc_alaw_decode(0x55) == -8);
    for (unsigned code = 0; code < 256; ++code)
        CHECK(jipc_alaw_encode(jipc_alaw_decode((unsigned char)code)) == code);
    imp_state *state = low_memory(sizeof *state);
    state->audio = 1;
    state->ao_set_attr = audio_attributes; state->ao_enable = state->ao_disable = audio_device;
    state->ao_enable_channel = state->ao_disable_channel = audio_channel;
    state->ao_send = audio_send; state->ao_query = audio_queue;
    state->ao_gain = fake_ao_gain; atomic_store(&state->speaker_gain, 25);
    unsigned char pcma[JIPC_SPEAKER_FRAME]; memset(pcma, 0xd5, sizeof pcma);
    char error[256] = {0};
    send_count = send_result = 0;
    CHECK(jipc_imp_ao_write(state, pcma) == -1 && !send_count); /* not open */
    CHECK(!jipc_imp_ao_open(state, 60, error, sizeof error));
    CHECK(state->ao_enabled && state->ao_channel);
    CHECK(atomic_load(&ao_gain_set) == 25); /* the stock AoGain, set once open */
    /* A gain change waits for the next open: the speaker thread owns the fd. */
    CHECK(!jipc_imp_control(state, "gain", 12, 1, error, sizeof error) && atomic_load(&ao_gain_set) == 25);
    CHECK(jipc_imp_control(state, "gain", 32, 1, error, sizeof error) == -1);
    CHECK(!jipc_imp_ao_write(state, pcma) && send_count == 1);
    for (unsigned i = 0; i < 320; ++i) CHECK(last_audio[i] == 8);
    send_result = -1;
    CHECK(jipc_imp_ao_write(state, pcma) == -1 && send_count == 2);
    send_result = 0;
    CHECK(jipc_imp_ao_volume(state, 200) == -1 && !jipc_imp_ao_volume(state, 60));
    jipc_imp_ao_close(state);
    CHECK(!state->ao_enabled && !state->ao_channel && !sdk_poisoned);
    CHECK(!jipc_imp_ao_open(state, 60, error, sizeof error) && atomic_load(&ao_gain_set) == 12);
    jipc_imp_ao_close(state);
    state->audio = 0;
    CHECK(jipc_imp_ao_open(state, 60, error, sizeof error) == -1);
    CHECK(!munmap(state, sizeof *state));
}

static int audio_get(int device, int channel, jipc_imp_audio_frame *frame, int block)
{
    CHECK(device == 1 && !channel && block == 1);
    if (!atomic_load(&audio_mode)) return -1;
    jipc_sleep_ms(5);
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
static int mic_attributes(int device, jipc_imp_audio_attr *attribute)
{
    CHECK(device == 1 && attribute->sample_rate == 16000 && attribute->bit_width == 16);
    CHECK(attribute->samples == 640 && attribute->frame_count == 12 && attribute->channels == 1);
    return 0;
}
static int mic_device(int device) { CHECK(device == 1); return 0; }
static int mic_parameters(int device, int channel, jipc_imp_audio_channel *parameters)
{
    /* The 12-byte T23 layout: depth, AEC channel select, reserved. */
    CHECK(device == 1 && !channel && parameters->depth == 12 &&
          !parameters->aec_channel && !parameters->reserved);
    return 0;
}
static int mic_channel(int device, int channel) { CHECK(device == 1 && !channel); return 0; }
static int mic_poll(int device, int channel, unsigned timeout_ms)
{
    CHECK(device == 1 && !channel && timeout_ms == 100);
    if (atomic_load(&audio_mode)) return 0;
    jipc_sleep_ms(2);
    return -1;
}
static int wait_for(atomic_uint *counter, unsigned minimum, unsigned milliseconds)
{
    for (unsigned waited = 0; waited < milliseconds; waited += 5) {
        if (atomic_load(counter) >= minimum) return 1;
        jipc_sleep_ms(5);
    }
    return atomic_load(counter) >= minimum;
}

static void test_microphone(void)
{
    int16_t *pcm = low_memory(4096); /* zeros encode to A-law 0xd5 */
    fake_audio = (jipc_imp_audio_frame){.bit_width = 16, .sound_mode = 1,
        .address = (uint32_t)(uintptr_t)pcm, .bytes = 1280};
    jipc_app *app = calloc(1, sizeof *app); CHECK(app);
    app->config.fps = 15;
    CHECK(!jipc_streams_init(app));
    imp_state *s = low_memory(sizeof *s);
    s->app = app; s->audio = 1;
    CHECK(!pthread_mutex_init(&s->mic_mutex, NULL)); s->mic_mutex_ready = 1;
    s->ai_set_attr = mic_attributes; s->ai_enable = s->ai_disable = mic_device;
    s->ai_set_channel = mic_parameters; s->ai_enable_channel = s->ai_disable_channel = mic_channel;
    s->ai_get = audio_get; s->ai_release = audio_release; s->ai_poll = mic_poll;
    s->ai_gain = fake_ai_gain; s->ai_volume = fake_ai_volume;
    atomic_store(&s->mic_gain, 22); atomic_store(&s->mic_volume, 80);
    mic_stall_us = 300000; mic_retry_unit_us = 1000;
    release_count = 0;
    atomic_store(&audio_mode, 1);
    mic_start(s);
    CHECK(wait_for(&s->mic_frames, 3, 2000));
    CHECK(atomic_load(&s->mic_state) == MIC_RUNNING);
    /* Opened at the stock levels (AiGain 22, AiVolume 80); a change is applied
     * by the microphone thread itself, never by the caller. */
    CHECK(atomic_load(&ai_gain_set) == 22 && atomic_load(&ai_volume_set) == 80);
    char control_error[160];
    CHECK(!jipc_imp_control(s, "gain", 26, 0, control_error, sizeof control_error));
    CHECK(!jipc_imp_control(s, "volume", 70, 0, control_error, sizeof control_error));
    for (unsigned waited = 0; waited < 2000 && atomic_load(&ai_volume_set) != 70; waited += 5) jipc_sleep_ms(5);
    CHECK(atomic_load(&ai_gain_set) == 26 && atomic_load(&ai_volume_set) == 70);
    CHECK(jipc_imp_control(s, "gain", -1, 0, control_error, sizeof control_error) == -1);
    jipc_frame first = {0}, second = {0};
    uint64_t cursor = app->streams[JIPC_AUDIO_STREAM].sequence - 2;
    CHECK(jipc_read(app, JIPC_AUDIO_STREAM, &cursor, &first, 0) == 1);
    CHECK(jipc_read(app, JIPC_AUDIO_STREAM, &cursor, &second, 0) == 1);
    CHECK(first.codec == JIPC_PCMA && first.size == 320);
    for (size_t i = 0; i < first.size; ++i) CHECK(first.data[i] == 0xd5);
    CHECK(second.pts_us - first.pts_us == 40000); /* stamped by sample count */
    jipc_frame_free(&first); jipc_frame_free(&second);
    /* The microphone stops: silence keeps time moving, then capture restarts. */
    atomic_store(&audio_mode, 0);
    CHECK(wait_for(&s->mic_fill, 1, 2000));
    CHECK(wait_for(&s->mic_restarts, 1, 3000));
    /* A frame record larger than the ABI disables only the microphone. */
    audio_overwrite = 1;
    atomic_store(&audio_mode, 1);
    for (unsigned waited = 0; waited < 3000 && atomic_load(&s->mic_state) != MIC_FAILED; waited += 5) jipc_sleep_ms(5);
    CHECK(atomic_load(&s->mic_state) == MIC_FAILED && !sdk_poisoned);
    char json[512];
    CHECK(!jipc_imp_audio_json(s, json, sizeof json) && strstr(json, "\"state\":\"failed\""));
    mic_stop(s);
    audio_overwrite = 0;
    atomic_store(&audio_mode, 0);
    CHECK(!s->ai_channel && !s->ai_enabled);
    pthread_mutex_destroy(&s->mic_mutex);
    jipc_streams_destroy(app); free(app);
    CHECK(!munmap(s, sizeof *s));
    CHECK(!munmap(pcm, 4096));
}


static unsigned graph_enabled;
static int graph_fail_enable;
static int graph_create_source(int ch, jipc_imp_fs *attr)
{
    CHECK(ch==0||ch==1||ch==5);
    CHECK(attr->width==(ch==1?1920u:640u) && attr->height==(ch==1?1080u:360u));
    CHECK(attr->buffer_count==1 && attr->scaler_enable && !attr->crop_enable && attr->pixel_format==10);
    return remember(1000+ch);
}
static int graph_set_source(int ch,jipc_imp_fs *attr)
{ CHECK(attr->width==(ch==1?1920u:640u));return remember(1010+ch); }
static int graph_group(int group){ CHECK(group==0||group==1);return remember(1020+group); }
static int graph_encoder(int ch,jipc_imp_encoder_attr *attr)
{
    CHECK(ch==0||ch==1);
    CHECK(attr->words[3]==(ch?640u:1920u) && attr->words[4]==(ch?360u:1080u));
    CHECK(attr->words[18]==(ch?768u:3072u)&&attr->words[14]==15&&!attr->words[40]);
    return remember(1030+ch);
}
static int graph_register(int group,int ch){CHECK(group==ch && ch<2);return remember(1040+ch);}
static int graph_bind(jipc_imp_cell *source,jipc_imp_cell *dest)
{
    CHECK(source->device==0&&dest->device==1);
    CHECK((source->group==1&&dest->group==0)||(source->group==5&&dest->group==1));
    return remember(1050+source->group);
}
static int graph_enable(int ch)
{
    remember(1060+ch);
    if(graph_fail_enable&&ch==5)return -7;
    graph_enabled|=1u<<ch;return 0;
}
static int graph_super(int ch,jipc_imp_super *attr)
{ CHECK(ch<2&&attr->i_bits==0x40000*8);return remember(1070+ch); }
static int graph_start(int ch){CHECK(graph_enabled==((1u<<0)|(1u<<1)|(1u<<5)));return remember(1080+ch);}
static int graph_fd(int ch){CHECK(graph_enabled==35);remember(1090+ch);return 50+ch;}
/* The frame source reaches each encoder through an OSD group, and comes apart in
 * the reverse order; a region is made once and stays where it is told. */
static int ov_binds[8][4],ov_bind_count,ov_unbinds,ov_stopped,ov_destroyed,ov_rgn_set,ov_rgn_shown,ov_rgn_made,ov_rgn_gone,ov_last_rect[4],ov_last_fmt;
static int ov_bind(jipc_imp_cell *a,jipc_imp_cell *b){int *r=ov_binds[ov_bind_count++];r[0]=a->device;r[1]=a->group;r[2]=b->device;r[3]=b->group;return 0;}
static int ov_unbind(jipc_imp_cell *a,jipc_imp_cell *b){int *r=ov_binds[--ov_bind_count];CHECK(r[0]==a->device&&r[1]==a->group&&r[2]==b->device&&r[3]==b->group);++ov_unbinds;return 0;}
static int ov_group(int g){CHECK(g==0||g==1);return 0;}
static int ov_stop(int g){(void)g;++ov_stopped;return 0;}
static int ov_group_destroy(int g){(void)g;++ov_destroyed;return 0;}
static int ov_create(void *a){CHECK(!a);return ov_rgn_made++;}
static int ov_destroy(int h){(void)h;++ov_rgn_gone;return 0;}
static int ov_register(int h,int g,void *a){(void)h;(void)g;CHECK(a);return 0;}
static int ov_unregister(int h,int g){(void)h;(void)g;return 0;}
static int ov_set(int h,void *a){(void)h;int32_t *w=a;CHECK(w[0]==7);for(int i=0;i<4;++i)ov_last_rect[i]=w[1+i];ov_last_fmt=w[7];++ov_rgn_set;return 0;}
static int ov_show(int h,int g,int on){(void)h;(void)g;ov_rgn_shown=on;return 0;}
static void test_overlay_graph(void)
{
    imp_state *s=cleanup_state();ordinary_sources=2;
    s->sensors=2;s->main_only=s->openimp_fullframe=1;s->width[0]=1920;s->height[0]=1080;
    s->width[1]=640;s->height[1]=360;s->fps=s->gop=15;s->bitrate[0]=3072;s->bitrate[2]=768;
    s->fs_create=graph_create_source;s->fs_set=graph_set_source;s->group_create=graph_group;
    s->enc_create=graph_encoder;s->enc_register=graph_register;s->fs_enable=graph_enable;s->super_frame=graph_super;s->start=graph_start;s->get_fd=graph_fd;
    s->bind=ov_bind;s->unbind=ov_unbind;
    s->osd_group_create=ov_group;s->osd_group_destroy=ov_group_destroy;s->osd_group_start=ov_group;s->osd_group_stop=ov_stop;
    s->osd_rgn_create=ov_create;s->osd_rgn_destroy=ov_destroy;s->osd_rgn_register=ov_register;s->osd_rgn_unregister=ov_unregister;s->osd_rgn_set=ov_set;s->osd_rgn_show=ov_show;
    graph_enabled=0;graph_fail_enable=0;call_count=0;char error[256];
    CHECK(!video_graph_open(s,error,sizeof(error)));
    /* lens 0: FS 1 -> OSD 0 -> encoder group 0; lens 1: FS 5 -> OSD 1 -> encoder group 1 */
    CHECK(ov_bind_count==4);
    CHECK(ov_binds[0][0]==0&&ov_binds[0][1]==source_channel(s,0)&&ov_binds[0][2]==4&&ov_binds[0][3]==0);
    CHECK(ov_binds[1][0]==4&&ov_binds[1][1]==0&&ov_binds[1][2]==1&&ov_binds[1][3]==enc_groups[0]);
    CHECK(ov_binds[2][0]==0&&ov_binds[2][1]==source_channel(s,2)&&ov_binds[2][2]==4&&ov_binds[2][3]==1);
    CHECK(ov_binds[3][0]==4&&ov_binds[3][1]==1&&ov_binds[3][2]==1&&ov_binds[3][3]==enc_groups[2]);
    CHECK(!s->bound[0]&&!s->bound[2]);
    /* No text yet: no region, and hiding it is harmless. */
    CHECK(!jipc_imp_overlay(s,0,NULL,0,error,sizeof(error))&&!ov_rgn_made);
    /* First text: one region, in the corner asked for, shown. */
    CHECK(!jipc_imp_overlay(s,0,"2026-11-06 14:42:08",JIPC_OVERLAY_TOP_LEFT,error,sizeof(error)));
    CHECK(ov_rgn_made==1&&ov_rgn_set==1&&ov_rgn_shown==1&&ov_last_fmt==17);
    CHECK(ov_last_rect[0]==24&&ov_last_rect[1]==24&&!((ov_last_rect[2]-ov_last_rect[0]+1)%16));
    /* The next second: the same region. */
    CHECK(!jipc_imp_overlay(s,0,"2026-11-06 14:42:09",JIPC_OVERLAY_BOTTOM_RIGHT,error,sizeof(error)));
    CHECK(ov_rgn_made==1&&ov_rgn_set==2);
    CHECK(ov_last_rect[2]==1920-24-1&&ov_last_rect[3]==1080-24-1);
    /* The small lens has its own region, at a smaller scale and margin, inside its frame. */
    CHECK(!jipc_imp_overlay(s,1,"2026-11-06 14:42:09 0123456789abcdef",JIPC_OVERLAY_TOP_RIGHT,error,sizeof(error)));
    CHECK(ov_rgn_made==2&&ov_last_rect[2]==640-16-1&&ov_last_rect[1]==16&&ov_last_rect[0]>=16);
    /* What does not fit is refused, and a lens that is not there is ignored. */
    CHECK(jipc_imp_overlay(s,1,"2026-11-06 14:42:09 0123456789abcdefgh",0,error,sizeof(error))<0);
    CHECK(!jipc_imp_overlay(s,2,"x",0,error,sizeof(error))&&ov_rgn_made==2);
    /* Hidden, then all of it comes apart. */
    CHECK(!jipc_imp_overlay(s,0,NULL,0,error,sizeof(error))&&!ov_rgn_shown);
    CHECK(!jipc_imp_overlay(s,0,"2026-11-06 14:42:10",0,error,sizeof(error))&&ov_rgn_shown==1);
    jipc_imp_close(s);
    CHECK(!ov_bind_count&&ov_unbinds==4&&ov_stopped==2&&ov_destroyed==2&&ov_rgn_gone==2);
}
static void test_main_only_graph(void)
{
    for(int partial=0;partial<2;++partial){
        imp_state *s=cleanup_state();ordinary_sources=2;
        s->sensors=2;s->main_only=s->openimp_fullframe=1;s->width[0]=1920;s->height[0]=1080;
        s->width[1]=640;s->height[1]=360;s->fps=s->gop=15;s->bitrate[0]=3072;s->bitrate[2]=768;
        s->fs_create=graph_create_source;s->fs_set=graph_set_source;s->group_create=graph_group;
        s->enc_create=graph_encoder;s->enc_register=graph_register;s->bind=graph_bind;
        s->fs_enable=graph_enable;s->super_frame=graph_super;s->start=graph_start;s->get_fd=graph_fd;
        graph_enabled=0;graph_fail_enable=partial;call_count=0;char error[256];
        CHECK(video_graph_open(s,error,sizeof(error))==(partial?-1:0));
        CHECK(!s->fs_created[1]&&!s->fs_created[3]&&!s->enc_created[1]&&!s->enc_created[3]);
        CHECK(!s->groups[2]&&!s->groups[3]);
        if(!partial){CHECK(s->fds[0]==50&&s->fds[2]==51);CHECK(s->receiving[0]&&s->receiving[2]);}
        else CHECK(!s->receiving[0]&&!s->receiving[2]);
        call_count=0;jipc_imp_close(s);
        const int complete[]={101,100,205,201,200,305,401,501,301,400,500,601,600,705,701,700};
        const int failed[]={201,200,305,401,501,301,400,500,601,600,705,701,700};
        const int *want=partial?failed:complete;size_t bytes=partial?sizeof(failed):sizeof(complete);
        CHECK(call_count==(int)(bytes/sizeof(int))&&!memcmp(calls,want,bytes));
        CHECK(!sdk_poisoned);
    }
    ordinary_sources=0;graph_fail_enable=0;
}

static jipc_imp_pack *pair_packs;
static int pair_pipe[2][2],pair_get[2],pair_release[2],pair_idr;
static int pair_get_stream(int ch,jipc_imp_stream *stream,int block)
{
    CHECK(ch==0||ch==1);CHECK(block==0);char byte;CHECK(read(pair_pipe[ch][0],&byte,1)==1);
    ++pair_get[ch];*stream=(jipc_imp_stream){(uint32_t)(uintptr_t)&pair_packs[ch],1,1,0};return 0;
}
static int pair_release_stream(int ch,jipc_imp_stream *stream)
{ CHECK(ch==0||ch==1);CHECK(stream->packs==(uint32_t)(uintptr_t)&pair_packs[ch]);++pair_release[ch];return 0; }
static int pair_request_idr(int ch){pair_idr=ch;return 0;}
/* A still in main_only: encoder 2 or 3 on that lens's only group, at its main size,
 * alongside H.264, and every resource given back. */
static int still_created,still_registered,still_group,still_started,still_stopped,still_unregistered,still_destroyed;
static unsigned still_width,still_height;
static int still_pipe[2];
static jipc_imp_pack *still_pack;
static int still_create(int ch,jipc_imp_encoder_attr *a){still_created=ch;still_width=a->words[3];still_height=a->words[4];CHECK(a->words[0]==0);return 0;}
static int still_register(int group,int ch){still_group=group;still_registered=ch;return 0;}
static int still_start(int ch){still_started=ch;CHECK(write(still_pipe[1],"x",1)==1);return 0;}
static int still_stop(int ch){still_stopped=ch;return 0;}
static int still_unregister(int ch){still_unregistered=ch;return 0;}
static int still_destroy(int ch){still_destroyed=ch;return 0;}
static int still_get_fd(int ch){(void)ch;return still_pipe[0];}
static int still_get_stream(int ch,jipc_imp_stream *stream,int block)
{ (void)ch;CHECK(block==0);char byte;CHECK(read(still_pipe[0],&byte,1)==1);*stream=(jipc_imp_stream){(uint32_t)(uintptr_t)still_pack,1,1,0};return 0; }
static int still_release_stream(int ch,jipc_imp_stream *stream){(void)ch;(void)stream;return 0;}
/* The picture only arrives once something else got the control lock. */
static pthread_mutex_t still_control=PTHREAD_MUTEX_INITIALIZER;
static int still_start_quiet(int ch){still_started=ch;return 0;}
static void *still_needs_control(void *arg)
{ (void)arg;pthread_mutex_lock(&still_control);CHECK(write(still_pipe[1],"x",1)==1);pthread_mutex_unlock(&still_control);return NULL; }
static void test_main_only_still(void)
{
    static const unsigned char jpeg[]={0xff,0xd8,0xff,0xe0,0,0,0xff,0xd9};
    unsigned char *memory=low_memory(4096);still_pack=(jipc_imp_pack*)memory;
    memcpy(memory+128,jpeg,sizeof(jpeg));*still_pack=(jipc_imp_pack){.address=(uint32_t)(uintptr_t)(memory+128),.length=sizeof(jpeg)};
    CHECK(!pipe(still_pipe));
    imp_state s={.sensors=2,.main_only=1,.enc_create=still_create,.enc_register=still_register,.start=still_start,.stop=still_stop,
                 .enc_unregister=still_unregister,.enc_destroy=still_destroy,.get_fd=still_get_fd,.get_stream=still_get_stream,.release_stream=still_release_stream};
    s.width[0]=1920;s.height[0]=1080;s.width[1]=640;s.height[1]=360;s.frame=malloc(JIPC_MAX_FRAME);CHECK(s.frame);
    char error[256];
    for(unsigned ch=0;ch<4;++ch){
        unsigned lens=ch/2;jipc_frame frame={0};
        CHECK(!jipc_imp_snapshot(&s,ch,NULL,&frame,error,sizeof(error)));
        CHECK(frame.codec==JIPC_JPEG&&frame.size==sizeof(jpeg)&&frame.data&&!memcmp(frame.data,jpeg,sizeof(jpeg)));
        CHECK(still_created==(int)(2+lens)&&still_registered==(int)(2+lens)&&still_group==(int)lens);
        CHECK(still_width==(lens?640u:1920u)&&still_height==(lens?360u:1080u));
        CHECK(still_started==(int)(2+lens)&&still_stopped==(int)(2+lens)&&still_unregistered==(int)(2+lens)&&still_destroyed==(int)(2+lens));
        jipc_frame_free(&frame);
    }
    jipc_frame none={0};CHECK(jipc_imp_snapshot(&s,4,NULL,&none,error,sizeof(error))<0&&!none.data);
    /* Video keeps draining during the wait, and the caller gets its lock back. */
    s.start=still_start_quiet;
    pthread_t helper;jipc_frame frame={0};
    pthread_mutex_lock(&still_control);
    CHECK(!pthread_create(&helper,NULL,still_needs_control,NULL));
    CHECK(!jipc_imp_snapshot(&s,0,&still_control,&frame,error,sizeof(error)));
    CHECK(pthread_mutex_trylock(&still_control)==EBUSY);
    pthread_mutex_unlock(&still_control);
    CHECK(!pthread_join(helper,NULL));
    jipc_frame_free(&frame);
    free(s.frame);close(still_pipe[0]);close(still_pipe[1]);
}
static void test_main_only_delivery(void)
{
    unsigned char *memory=low_memory(4096);pair_packs=(jipc_imp_pack*)memory;
    /* Distinct opaque Annex-B markers test routing/cache ownership; the
     * protocol integration uses genuine independently encoded fixtures. */
    const unsigned char data[]={0,0,0,1,0x67,0x42,0,0x1e,0x89,0,0,0,1,0x68,0xce,6,0xe2,0,0,0,1,0x65,0x80,0x7f};
    for(int i=0;i<2;++i){
        unsigned char *body=memory+128+i*128;memcpy(body,data,sizeof(data));body[8]+=(unsigned char)i;
        pair_packs[i]=(jipc_imp_pack){.address=(uint32_t)(uintptr_t)body,.length=sizeof(data)};
        CHECK(!pipe(pair_pipe[i]));pair_get[i]=pair_release[i]=0;
    }
    jipc_app app={0};jipc_config_defaults(&app.config);app.config.main_only=1;app.config.width=1920;app.config.height=1080;
    app.config.width1=640;app.config.height1=360;CHECK(!jipc_streams_init(&app));
    imp_state s={.sensors=2,.main_only=1,.get_stream=pair_get_stream,.release_stream=pair_release_stream,.idr=pair_request_idr};
    s.frame=malloc(JIPC_MAX_FRAME);CHECK(s.frame);s.fds[0]=pair_pipe[0][0];s.fds[2]=pair_pipe[1][0];
    s.last_video[0]=s.last_video[2]=s.next_owner_check=jipc_now_us();char error[256];
    struct pollfd fds[4];unsigned slots[4];CHECK(video_pollfds(&s,fds,slots)==2);
    CHECK(slots[0]==0&&slots[1]==2&&fds[0].fd==s.fds[0]&&fds[1].fd==s.fds[2]);
    CHECK(write(pair_pipe[0][1],"x",1)==1);CHECK(!jipc_imp_wait(&s,0));
    CHECK(!jipc_imp_poll(&app,&s,0,error,sizeof(error)));
    CHECK(pair_get[0]==1&&pair_release[0]==1&&!pair_get[1]);
    CHECK(app.produced[0]==1&&app.produced[1]==1&&!app.produced[2]&&!app.produced[3]);
    CHECK(write(pair_pipe[1][1],"x",1)==1);CHECK(!jipc_imp_poll(&app,&s,0,error,sizeof(error)));
    CHECK(pair_get[0]==1&&pair_get[1]==1&&pair_release[1]==1);
    for(unsigned owner=0;owner<4;owner+=2){
        uint64_t a=0,b=0;jipc_frame one={0},two={0};
        CHECK(jipc_read(&app,owner,&a,&one,0)==1&&jipc_read(&app,owner+1,&b,&two,0)==1);
        CHECK(one.pts_us==two.pts_us&&one.keyframe&&two.keyframe&&one.size==two.size&&!memcmp(one.data,two.data,one.size));
        CHECK(one.data!=two.data&&one.data[8]==(unsigned char)(data[8]+owner/2));
        one.data[8]=0;jipc_frame_free(&one);CHECK(two.data[8]!=(unsigned char)0);jipc_frame_free(&two);
        CHECK(app.streams[owner].sps_size==app.streams[owner+1].sps_size);
        CHECK(!memcmp(app.streams[owner].sps,app.streams[owner+1].sps,app.streams[owner].sps_size));
    }
    for(int ch=0;ch<4;++ch){CHECK(!jipc_imp_control(&s,"idr",ch,0,error,sizeof(error)));CHECK(pair_idr==ch/2);}
    CHECK(jipc_imp_control(&s,"idr",4,0,error,sizeof(error))<0);
    s.last_video[2]=jipc_now_us()-16000000;CHECK(write(pair_pipe[0][1],"x",1)==1);
    CHECK(jipc_imp_poll(&app,&s,0,error,sizeof(error))<0&&strstr(error,"stream 2"));
    CHECK(app.produced[0]==2&&app.produced[1]==2&&app.produced[2]==1&&app.produced[3]==1);
    for(int i=0;i<2;++i){close(pair_pipe[i][0]);close(pair_pipe[i][1]);}
    free(s.frame);jipc_streams_destroy(&app);CHECK(!munmap(memory,4096));
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
    config.audio = 1; /* qualified: microphone and speaker over OpenIMP */
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    config.audio = 0; config.sensors = 2;
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    config.width=1280;config.height=720;config.width1=640;config.height1=360;
    CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error) == -1);
    config.width1=1280;config.height1=720;
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    config.main_only=1;config.width=1920;config.height=1080;config.width1=640;config.height1=360;
    CHECK(!validate_provider("libimp.so T23 openimp", &config, error, sizeof error));
    CHECK(validate_provider("IMP-1.3.0", &config, error, sizeof error)<0);
    config.width1=1280;CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error)<0);
    config.width1=640;config.width=2304;CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error)<0);
    config.width=1920;config.sensors=1;CHECK(validate_provider("libimp.so T23 openimp", &config, error, sizeof error)<0);
    config.sensors=2;
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
    test_provider(); test_cleanup(); test_packs(); test_speaker(); test_microphone(); test_main_only_graph(); test_overlay_graph(); test_main_only_delivery(); test_main_only_still();
    puts("IMP offline adapter contracts passed");
    return 0;
}
