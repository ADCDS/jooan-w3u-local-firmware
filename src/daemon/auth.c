#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PBKDF2_ROUNDS 120000u
#define SESSION_SECONDS (30u * 60u)
#define SESSIONS 16
#define BUCKETS 16

typedef struct { int used; char token[65], csrf[65]; time_t expires; int must_change; } Session;
typedef struct { char remote[64]; time_t since; unsigned failures; } Bucket;
static Session sessions[SESSIONS];
static Bucket buckets[BUCKETS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void auth_path(const JoanConfig *cfg, char out[512]) { snprintf(out, 512, "%s/auth.db", cfg->state_dir); }

static int save_record(const JoanConfig *cfg, const char *password, int must_change)
{
    unsigned char salt[16], key[32];
    char hs[33], hk[65], line[192], path[512];
    int n;
    if (joan_random(salt, sizeof(salt))) return -1;
    joan_pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), PBKDF2_ROUNDS, key, sizeof(key));
    joan_hex(salt, sizeof(salt), hs); joan_hex(key, sizeof(key), hk);
    n = snprintf(line, sizeof(line), "v1:%u:%s:%s:%d\n", PBKDF2_ROUNDS, hs, hk, must_change);
    auth_path(cfg, path);
    memset(key, 0, sizeof(key));
    return n > 0 && (size_t)n < sizeof(line) ? joan_write_atomic(path, line, (size_t)n, 0600) : -1;
}

static int verify(const JoanConfig *cfg, const char *password, int *must_change)
{
    char path[512], *p, *save = NULL;
    unsigned char *raw = NULL, salt[16], want[32], got[32];
    size_t n;
    unsigned long rounds;
    int ok = 0;
    auth_path(cfg, path);
    if (joan_read_file(path, &raw, &n, 512)) return 0;
    p = strtok_r((char *)raw, ":\r\n", &save);
    if (!p || strcmp(p, "v1")) goto done;
    p = strtok_r(NULL, ":\r\n", &save); if (!p) goto done; rounds = strtoul(p, NULL, 10);
    p = strtok_r(NULL, ":\r\n", &save); if (!p || joan_unhex(p, salt, 16)) goto done;
    p = strtok_r(NULL, ":\r\n", &save); if (!p || joan_unhex(p, want, 32)) goto done;
    p = strtok_r(NULL, ":\r\n", &save); if (!p || rounds < 50000 || rounds > 1000000) goto done;
    joan_pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), (uint32_t)rounds, got, sizeof(got));
    ok = joan_ct_equal(got, want, sizeof(got));
    if (ok && must_change) *must_change = atoi(p) != 0;
done:
    memset(got, 0, sizeof(got)); memset(want, 0, sizeof(want));
    if (raw) { memset(raw, 0, n); free(raw); }
    return ok;
}

int joan_auth_init(const JoanConfig *cfg)
{
    char path[512];
    FILE *f;
    if (joan_mkdir_p(cfg->state_dir, 0700)) return -1;
    auth_path(cfg, path);
    f = fopen(path, "r");
    if (f) { fclose(f); return 0; }
    /* Deliberately recognizable bootstrap secret; API access remains local TLS,
     * and all privileged endpoints remain locked until it is replaced. */
    return save_record(cfg, "change-me-now", 1); /* hygiene: allow-public-bootstrap */
}

int joan_auth_setup_required(const JoanConfig *cfg)
{
    char path[512]; unsigned char *raw = NULL; size_t n = 0; int required = 1;
    auth_path(cfg, path);
    if (!joan_read_file(path, &raw, &n, 512) && n >= 2) {
        char *last = strrchr((char *)raw, ':');
        if (last) required = atoi(last + 1) != 0;
    }
    if (raw) free(raw);
    return required;
}

static Bucket *bucket_for(const char *remote, time_t now)
{
    unsigned i, free_i = 0;
    for (i = 0; i < BUCKETS; ++i) {
        if (!strcmp(buckets[i].remote, remote)) return &buckets[i];
        if (!buckets[i].remote[0] || buckets[i].since + 300 < now) free_i = i;
    }
    memset(&buckets[free_i], 0, sizeof(buckets[free_i]));
    snprintf(buckets[free_i].remote, sizeof(buckets[free_i].remote), "%s", remote);
    buckets[free_i].since = now;
    return &buckets[free_i];
}

int joan_auth_login(const JoanConfig *cfg, const char *remote,
                    const char *user, const char *password, JoanAuthz *out)
{
    Bucket *b; Session *s = NULL; unsigned i; time_t now = time(NULL); int must = 0;
    unsigned char random[32];
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&lock);
    b = bucket_for(remote, now);
    if (b->since + 60 < now) { b->since = now; b->failures = 0; }
    if (b->failures >= 5) { pthread_mutex_unlock(&lock); return -2; }
    pthread_mutex_unlock(&lock);
    if (strcmp(user, "admin") || !verify(cfg, password, &must)) {
        pthread_mutex_lock(&lock); b = bucket_for(remote, now); b->failures++; pthread_mutex_unlock(&lock);
        return -1;
    }
    if (joan_random(random, sizeof(random))) return -1;
    pthread_mutex_lock(&lock);
    for (i = 0; i < SESSIONS; ++i) if (!sessions[i].used || sessions[i].expires < now) { s = &sessions[i]; break; }
    if (!s) s = &sessions[0];
    memset(s, 0, sizeof(*s)); s->used = 1; s->expires = now + SESSION_SECONDS; s->must_change = must;
    joan_hex(random, sizeof(random), s->token);
    if (joan_random(random, sizeof(random))) { memset(s, 0, sizeof(*s)); pthread_mutex_unlock(&lock); return -1; }
    joan_hex(random, sizeof(random), s->csrf);
    out->authenticated = 1; out->must_change = must;
    memcpy(out->token, s->token, sizeof(out->token)); memcpy(out->csrf, s->csrf, sizeof(out->csrf));
    b = bucket_for(remote, now); b->failures = 0;
    pthread_mutex_unlock(&lock); memset(random, 0, sizeof(random)); return 0;
}

static int cookie_token(const char *cookie, char out[65])
{
    const char *p = strstr(cookie ? cookie : "", "joan_session="); size_t n;
    if (!p) return -1;
    p += 13;
    n = strcspn(p, "; \r\n");
    if (n != 64) return -1;
    memcpy(out, p, n); out[n] = 0; return 0;
}

int joan_auth_request(const JoanRequest *req, int require_csrf, JoanAuthz *out)
{
    char token[65]; unsigned i; time_t now = time(NULL);
    memset(out, 0, sizeof(*out)); if (cookie_token(req->cookie, token)) return -1;
    pthread_mutex_lock(&lock);
    for (i = 0; i < SESSIONS; ++i) if (sessions[i].used && sessions[i].expires >= now && joan_ct_equal(token, sessions[i].token, 64)) {
        if (require_csrf && (!req->csrf[0] || !joan_ct_equal(req->csrf, sessions[i].csrf, 64))) break;
        out->authenticated = 1; out->must_change = sessions[i].must_change;
        memcpy(out->token, sessions[i].token, sizeof(out->token)); memcpy(out->csrf, sessions[i].csrf, sizeof(out->csrf));
        pthread_mutex_unlock(&lock); return 0;
    }
    pthread_mutex_unlock(&lock); return -1;
}

int joan_auth_change_password(const JoanConfig *cfg, const JoanAuthz *auth,
                              const char *old_password, const char *new_password)
{
    unsigned i; int ignored;
    if (!auth->authenticated || !verify(cfg, old_password, &ignored) || strlen(new_password) < 12 ||
        !strcmp(new_password, "change-me-now") || /* hygiene: allow-public-bootstrap */
        save_record(cfg, new_password, 0)) return -1;
    pthread_mutex_lock(&lock);
    for (i = 0; i < SESSIONS; ++i) memset(&sessions[i], 0, sizeof(sessions[i]));
    pthread_mutex_unlock(&lock); return 0;
}

void joan_auth_logout(const JoanAuthz *auth)
{
    unsigned i; pthread_mutex_lock(&lock);
    for (i = 0; i < SESSIONS; ++i) if (sessions[i].used && joan_ct_equal(auth->token, sessions[i].token, 64)) memset(&sessions[i], 0, sizeof(sessions[i]));
    pthread_mutex_unlock(&lock);
}
