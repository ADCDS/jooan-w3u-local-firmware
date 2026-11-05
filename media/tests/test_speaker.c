/* SPDX-License-Identifier: MIT
 * Speaker service contract with a fake sink: session start and settle, the
 * deadman mute, one talker, queue policies, release, the boot gate, faults,
 * idle close and half-duplex. No audio device is touched.
 */
#define _GNU_SOURCE
#include "../src/speaker.c"
#include <fcntl.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char played[64000];
static size_t played_size;
static int opens, closes, amp_on, amp_off, amp_state, write_result, volume_set = -999;

static int fake_open(void *c, int volume, char *e, size_t n)
{ (void)c; (void)e; (void)n; pthread_mutex_lock(&fake_lock); ++opens; volume_set = volume; pthread_mutex_unlock(&fake_lock); return 0; }
static int fake_write(void *c, const unsigned char *pcma)
{
    (void)c;
    jipc_sleep_ms(5);
    pthread_mutex_lock(&fake_lock);
    int rc = write_result;
    if (!rc && played_size + JIPC_SPEAKER_FRAME <= sizeof played) {
        memcpy(played + played_size, pcma, JIPC_SPEAKER_FRAME);
        played_size += JIPC_SPEAKER_FRAME;
    }
    pthread_mutex_unlock(&fake_lock);
    return rc;
}
static void fake_close(void *c) { (void)c; pthread_mutex_lock(&fake_lock); ++closes; pthread_mutex_unlock(&fake_lock); }
static int fake_volume(void *c, int v) { (void)c; pthread_mutex_lock(&fake_lock); volume_set = v; pthread_mutex_unlock(&fake_lock); return 0; }
static int fake_amplifier(void *c, int on)
{
    (void)c;
    pthread_mutex_lock(&fake_lock);
    if (on) ++amp_on; else ++amp_off;
    amp_state = on;
    pthread_mutex_unlock(&fake_lock);
    return 0;
}
static int read_int(int *value) { pthread_mutex_lock(&fake_lock); int v = *value; pthread_mutex_unlock(&fake_lock); return v; }
static void reset_fake(void)
{
    pthread_mutex_lock(&fake_lock);
    played_size = 0; opens = closes = amp_on = amp_off = amp_state = write_result = 0;
    pthread_mutex_unlock(&fake_lock);
}
static int wait_until(int (*predicate)(void), unsigned milliseconds)
{
    for (unsigned waited = 0; waited < milliseconds; waited += 5) {
        if (predicate()) return 1;
        jipc_sleep_ms(5);
    }
    return predicate();
}
static jipc_app *current;
static int session_idle(void)
{
    speaker_state *s = current->speaker_state;
    pthread_mutex_lock(&s->mutex); int idle = !s->session && !s->count; pthread_mutex_unlock(&s->mutex);
    return idle;
}
static int session_running(void)
{
    speaker_state *s = current->speaker_state;
    pthread_mutex_lock(&s->mutex); int running = s->session; pthread_mutex_unlock(&s->mutex);
    return running;
}
static int muted(void) { return read_int(&amp_off) > 0 && !read_int(&amp_state); }
static int faulted(void)
{
    speaker_state *s = current->speaker_state;
    pthread_mutex_lock(&s->mutex); int f = s->faulted; pthread_mutex_unlock(&s->mutex);
    return f;
}
static int closed(void) { return read_int(&closes) > 0; }

/* The submitted bytes must appear in order, contiguously, in what played. */
static int played_contains(const unsigned char *data, size_t size)
{
    pthread_mutex_lock(&fake_lock);
    int found = played_size >= size && memmem(played, played_size, data, size) != NULL;
    pthread_mutex_unlock(&fake_lock);
    return found;
}

int main(void)
{
    jipc_speaker_sink sink = {.context = NULL, .open = fake_open, .write = fake_write,
        .close = fake_close, .volume = fake_volume, .amplifier = fake_amplifier};
    jipc_app *app = calloc(1, sizeof *app); CHECK(app);
    current = app;
    char error[256];
    unsigned char speech[1600];
    for (size_t i = 0; i < sizeof speech; ++i) speech[i] = (unsigned char)(i * 7 + 1) | 0x80;

    /* Boot gate: talk is refused until the hook's marker exists. */
    char marker[] = "/tmp/jipc-speaker-gate-XXXXXX";
    int fd = mkstemp(marker); CHECK(fd >= 0); close(fd); unlink(marker);
    setenv("JIPC_SPEAKER_GATE", marker, 1);
    CHECK(!jipc_speaker_available(app));
    CHECK(jipc_talk_submit(app, 1, 5, 0, speech, 320, error, sizeof error) == JIPC_TALK_UNAVAILABLE);
    CHECK(!jipc_speaker_start(app, &sink) && jipc_speaker_available(app));
    CHECK(jipc_talk_submit(app, 1, 5, 0, speech, 320, error, sizeof error) == JIPC_TALK_UNAVAILABLE);
    CHECK(strstr(error, "boot hook"));
    fd = open(marker, O_CREAT | O_WRONLY, 0600); CHECK(fd >= 0); close(fd);
    CHECK(jipc_speaker_gate_open(app));
    unlink(marker);
    CHECK(jipc_speaker_gate_open(app)); /* cached once released */

    /* A session: open, amplifier on, settle, play in order, deadman mute. */
    reset_fake();
    CHECK(jipc_talk_submit(app, 1, 5, 0, speech, sizeof speech, error, sizeof error) == JIPC_TALK_OK);
    CHECK(wait_until(session_running, 1000));
    CHECK(jipc_talk_active(app));
    /* Another talker, or another SSRC on the same connection, is refused. */
    CHECK(jipc_talk_submit(app, 2, 5, 0, speech, 320, error, sizeof error) == JIPC_TALK_BUSY);
    CHECK(jipc_talk_submit(app, 1, 6, 0, speech, 320, error, sizeof error) == JIPC_TALK_BUSY);
    CHECK(wait_until(session_idle, 3000));
    CHECK(wait_until(muted, 1000));
    CHECK(read_int(&opens) == 1 && read_int(&amp_on) == 1);
    CHECK(played_contains(speech, sizeof speech));
    CHECK(jipc_talk_active(app)); /* half-duplex tail */
    jipc_sleep_ms(350);
    CHECK(!jipc_talk_active(app));
    char json[1024];
    CHECK(!jipc_speaker_status_json(app, json, sizeof json));
    CHECK(strstr(json, "\"state\":\"idle\"") && strstr(json, "\"sessions\":1") && strstr(json, "\"refused\":2"));

    /* Release (TEARDOWN): the session ends promptly, at most 200 ms of tail. */
    reset_fake();
    CHECK(jipc_talk_submit(app, 3, 9, 0, speech, sizeof speech, error, sizeof error) == JIPC_TALK_OK);
    CHECK(wait_until(session_running, 1000));
    uint64_t released_at = jipc_now_us();
    jipc_talk_release(app, 3);
    CHECK(wait_until(session_idle, 1500));
    CHECK(jipc_now_us() - released_at < UINT64_C(1200000));
    CHECK(wait_until(muted, 1000));
    CHECK(read_int(&opens) == 0); /* still open from the first session */

    /* HTTP talkers are told when the 2 s queue is full; RTSP drops oldest. */
    speaker_state *s = app->speaker_state;
    reset_fake();
    static unsigned char big[8000];
    memset(big, 0x9a, sizeof big);
    uint64_t http_owner = UINT64_C(1) << 32 | 0x0a000002u;
    CHECK(jipc_talk_submit(app, http_owner, 0, 1, big, sizeof big, error, sizeof error) == JIPC_TALK_OK);
    CHECK(jipc_talk_submit(app, http_owner, 0, 1, big, sizeof big, error, sizeof error) == JIPC_TALK_OK ||
          strstr(error, "queue is full"));
    int busy = 0;
    for (int i = 0; i < 3; ++i)
        busy |= jipc_talk_submit(app, http_owner, 0, 1, big, sizeof big, error, sizeof error) == JIPC_TALK_BUSY;
    CHECK(busy && strstr(error, "queue is full"));
    CHECK(jipc_talk_submit(app, 1, 1, 0, big, 100, error, sizeof error) == JIPC_TALK_BUSY);
    CHECK(jipc_talk_submit(app, http_owner, 0, 1, NULL, 10, error, sizeof error) == JIPC_TALK_INVALID);
    CHECK(jipc_talk_submit(app, http_owner, 0, 1, big, 8001, error, sizeof error) == JIPC_TALK_INVALID);
    CHECK(wait_until(session_idle, 6000));
    CHECK(wait_until(muted, 1000));
    reset_fake();
    for (int i = 0; i < 4; ++i)
        CHECK(jipc_talk_submit(app, 4, 4, 0, big, 2000, error, sizeof error) == JIPC_TALK_OK);
    pthread_mutex_lock(&s->mutex);
    CHECK(s->count <= TRIM_ABOVE && s->dropped_bytes > 0); /* ran-ahead audio trimmed */
    pthread_mutex_unlock(&s->mutex);
    CHECK(wait_until(session_idle, 3000));

    /* Volume: validated, applied while open, used at the next open. */
    CHECK(jipc_speaker_set_volume(app, 121, error, sizeof error) < 0);
    CHECK(!jipc_speaker_set_volume(app, 70, error, sizeof error) && read_int(&volume_set) == 70);

    /* Faults: three refused frames close the output, mute, and back off. */
    speaker_retry_unit_us = 40000; /* 5 "seconds" = 200 ms */
    reset_fake();
    pthread_mutex_lock(&fake_lock); write_result = -1; pthread_mutex_unlock(&fake_lock);
    CHECK(jipc_talk_submit(app, 5, 5, 0, speech, sizeof speech, error, sizeof error) == JIPC_TALK_OK);
    CHECK(wait_until(faulted, 2000));
    CHECK(wait_until(closed, 1000) && wait_until(muted, 1000));
    CHECK(jipc_talk_submit(app, 5, 5, 0, speech, 320, error, sizeof error) == JIPC_TALK_UNAVAILABLE);
    pthread_mutex_lock(&fake_lock); write_result = 0; pthread_mutex_unlock(&fake_lock);
    jipc_sleep_ms(260);
    CHECK(jipc_talk_submit(app, 5, 5, 0, speech, sizeof speech, error, sizeof error) == JIPC_TALK_OK);
    CHECK(wait_until(session_running, 1000) || played_contains(speech, 320));
    CHECK(wait_until(session_idle, 3000));
    CHECK(read_int(&opens) == 1); /* reopened after the fault */

    /* Idle: the output closes when nobody has talked for a while. */
    speaker_idle_close_us = 100000;
    reset_fake();
    CHECK(wait_until(closed, 2000));

    CHECK(!jipc_speaker_status_json(app, json, sizeof json) && strstr(json, "\"faults\":1"));
    jipc_speaker_stop(app);
    CHECK(!jipc_speaker_available(app));
    CHECK(jipc_talk_submit(app, 1, 1, 0, speech, 10, error, sizeof error) == JIPC_TALK_UNAVAILABLE);
    jipc_talk_release(app, 1);
    CHECK(!jipc_talk_active(app));
    free(app);
    printf("speaker service: %u checks PASS\n", checks);
    return 0;
}
