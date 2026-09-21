#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <pthread.h>
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PBKDF2_ROUNDS 120000u
#define SESSION_SECONDS (30u * 60u)
#define SESSIONS 16
#define BUCKETS 16
#define DEFAULT_PASSWORD "admin"

typedef struct { int used; char token[65], csrf[65]; time_t expires; int must_change; } Session;
typedef struct { char remote[64]; time_t since; unsigned failures; } Bucket;
static Session sessions[SESSIONS];
static Bucket buckets[BUCKETS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void auth_path(const JoanConfig *cfg, char out[512]) { snprintf(out, 512, "%s/auth.db", cfg->state_dir); }

static int ssh_password_path(const JoanConfig *cfg, char out[512])
{
    char dir[512];
    if (snprintf(dir, sizeof(dir), "%s/ssh", cfg->state_dir) >= (int)sizeof(dir) ||
        joan_mkdir_p(dir, 0700)) return -1;
    return snprintf(out, 512, "%s/passwd", dir) < 512 ? 0 : -1;
}

static int build_records(const char *password, int warning,
                         char auth[192], size_t *auth_len,
                         char ssh[384], size_t *ssh_len,
                         char rtsp[320],size_t *rtsp_len)
{
    unsigned char salt[16], key[32];
    char hs[33], hk[65], crypt_salt[40];
    static const char alphabet[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char *hash;
    int n;
    if (joan_random(salt, sizeof(salt))) return -1;
    joan_pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), PBKDF2_ROUNDS, key, sizeof(key));
    joan_hex(salt, sizeof(salt), hs); joan_hex(key, sizeof(key), hk);
    n = snprintf(auth, 192, "v1:%u:%s:%s:%d\n", PBKDF2_ROUNDS, hs, hk, warning);
    if (n <= 0 || n >= 192) return -1;
    *auth_len=(size_t)n;
    /* This camera's uClibc 0.9.33.2 libcrypt implements modular `$1$` and
     * legacy DES, but not `$5$`/`$6$`. Use its strongest compatible format;
     * the HTTPS account itself remains PBKDF2-SHA256 above. */
    strcpy(crypt_salt,"$1$");
    for(n=0;n<8;n++)crypt_salt[3+n]=alphabet[salt[n]&63u];
    crypt_salt[11]='$';crypt_salt[12]=0;
    hash=crypt(password,crypt_salt);
    if(!hash||strchr(hash,'\n')||strchr(hash,':'))return-1;
    n=snprintf(ssh,384,"admin:%s\n",hash);
    memset(key, 0, sizeof(key));
    if(n<=0||n>=384)return-1;
    *ssh_len=(size_t)n;
    n=snprintf(rtsp,320,"%s\n",password);if(n<=0||n>=320)return-1;*rtsp_len=(size_t)n;
    return 0;
}

static int save_record(const JoanConfig *cfg, const char *password, int warning)
{
    char auth[192],ssh[384],rtsp[320],auth_file[512],ssh_file[512],rtsp_file[512];
    unsigned char *old_auth=NULL,*old_ssh=NULL,*old_rtsp=NULL;size_t an,sn,rn,old_an=0,old_sn=0,old_rn=0;
    int had_auth,had_ssh,had_rtsp;
    if(build_records(password,warning,auth,&an,ssh,&sn,rtsp,&rn))return-1;
    auth_path(cfg,auth_file);if(ssh_password_path(cfg,ssh_file))return-1;
    if(snprintf(rtsp_file,sizeof(rtsp_file),"%s/rtsp.password",cfg->state_dir)>=(int)sizeof(rtsp_file))return-1;
    had_auth=!joan_read_file(auth_file,&old_auth,&old_an,512);
    had_ssh=!joan_read_file(ssh_file,&old_ssh,&old_sn,1024);
    had_rtsp=!joan_read_file(rtsp_file,&old_rtsp,&old_rn,512);
    if(joan_write_atomic(ssh_file,ssh,sn,0600))goto fail;
    if(joan_write_atomic(rtsp_file,rtsp,rn,0600)){if(had_ssh)joan_write_atomic(ssh_file,old_ssh,old_sn,0600);else unlink(ssh_file);goto fail;}
    if(joan_write_atomic(auth_file,auth,an,0600)){
        if(had_ssh)joan_write_atomic(ssh_file,old_ssh,old_sn,0600);else unlink(ssh_file);
        if(had_rtsp)joan_write_atomic(rtsp_file,old_rtsp,old_rn,0600);else unlink(rtsp_file);
        goto fail;
    }
    {char marker[512];snprintf(marker,sizeof(marker),"%s/rtsp.synced",cfg->state_dir);unlink(marker);}
    memset(auth,0,sizeof(auth));memset(ssh,0,sizeof(ssh));memset(rtsp,0,sizeof(rtsp));
    if(old_auth){memset(old_auth,0,old_an);free(old_auth);}if(old_ssh){memset(old_ssh,0,old_sn);free(old_ssh);}if(old_rtsp){memset(old_rtsp,0,old_rn);free(old_rtsp);}
    return 0;
fail:
    if(!had_auth)unlink(auth_file);
    memset(auth,0,sizeof(auth));memset(ssh,0,sizeof(ssh));memset(rtsp,0,sizeof(rtsp));
    if(old_auth){memset(old_auth,0,old_an);free(old_auth);}if(old_ssh){memset(old_ssh,0,old_sn);free(old_ssh);}if(old_rtsp){memset(old_rtsp,0,old_rn);free(old_rtsp);}
    return -1;
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
    char path[512],ssh_path[512];
    FILE *f;
    if (joan_mkdir_p(cfg->state_dir, 0700)) return -1;
    auth_path(cfg, path);
    f = fopen(path, "r");
    if (f) {
        fclose(f);
        if(ssh_password_path(cfg,ssh_path))return-1;
        f=fopen(ssh_path,"r");if(f){char rtsp_path[512];fclose(f);snprintf(rtsp_path,sizeof(rtsp_path),"%s/rtsp.password",cfg->state_dir);f=fopen(rtsp_path,"r");if(f){fclose(f);return 0;}}
        /* A pre-sync bootstrap database can be repaired because its public
         * initial secret is known. Never guess or reset a customized secret. */
        if(verify(cfg,DEFAULT_PASSWORD,NULL))return save_record(cfg,DEFAULT_PASSWORD,1);
        /* Upgraded 0.1 databases contain no recoverable plaintext. Preserve
         * the web credential and mark SSH unsynchronized until rotation. */
        return 0;
    }
    /* Deliberately recognizable bootstrap secret; API access remains local TLS,
     * and all privileged endpoints remain locked until it is replaced. */
    return save_record(cfg, DEFAULT_PASSWORD, 1); /* hygiene: allow-public-bootstrap */
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

int joan_auth_ssh_synchronized(const JoanConfig *cfg)
{
    char path[512];FILE*f;if(ssh_password_path(cfg,path))return 0;f=fopen(path,"r");if(!f)return 0;{char line[512];int ok=fgets(line,sizeof(line),f)!=NULL&&!strncmp(line,"admin:$",7)&&strchr(line,'\n')!=NULL;fclose(f);return ok;}
}

int joan_auth_rtsp_synchronized(const JoanConfig *cfg)
{
    char path[512],marker_path[512],want[65];unsigned char*secret=NULL,*marker=NULL,hash[32];size_t n=0,m=0;snprintf(path,sizeof(path),"%s/rtsp.password",cfg->state_dir);snprintf(marker_path,sizeof(marker_path),"%s/rtsp.synced",cfg->state_dir);if(joan_read_file(path,&secret,&n,512)||joan_read_file(marker_path,&marker,&m,128)){free(secret);free(marker);return 0;}joan_sha256(secret,n,hash);joan_hex(hash,32,want);{int ok=m>=64&&!memcmp(marker,want,64);memset(secret,0,n);free(secret);free(marker);return ok;}
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
    unsigned i; int ignored;size_t password_len=new_password?strlen(new_password):0;
    /* No minimum. The operator owns this device and its threat model -- a
       camera on an isolated VLAN with a memorable password is a choice the
       UI should allow, not overrule. Control characters and the 128-byte
       ceiling still go, because those break the SSH and RTSP records the
       password is synchronized into. */
    if(!password_len||password_len>128)return-1;
    for(i=0;i<password_len;i++)if((unsigned char)new_password[i]<0x20||(unsigned char)new_password[i]==0x7f)return-1;
    if (!auth->authenticated || !verify(cfg, old_password, &ignored) ||
        save_record(cfg, new_password, !strcmp(new_password,DEFAULT_PASSWORD))) return -1;
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

/* Re-issue this session's lifetime against the current clock. Setting the
   system time forward would otherwise leave every session's absolute expiry
   in the past, logging the caller out on their next request. */
/* Setting the camera clock moves wall time, and session expiries are absolute
   wall-clock stamps, so a forward jump would strand every live session. Shift
   all sessions by the same delta the clock moved: each keeps exactly the life
   it had left, and the 30-minute absolute cap is preserved (a no-op time-set
   shifts by ~0, so this cannot be replayed to renew a session indefinitely). */
void joan_auth_shift(time_t delta)
{
    unsigned i; pthread_mutex_lock(&lock);
    for (i = 0; i < SESSIONS; ++i) if (sessions[i].used) sessions[i].expires += delta;
    pthread_mutex_unlock(&lock);
}
