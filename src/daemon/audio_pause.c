#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef JOAN_AUDIO_TRIAL_SECONDS
#define JOAN_AUDIO_TRIAL_SECONDS 60
#endif
#ifndef JOAN_AUDIO_ACK_SECONDS
#define JOAN_AUDIO_ACK_SECONDS 15
#endif

/* Persist only operator changes and the first-minute recovery marker. The
 * renewable lease and the guard's acknowledgement live exclusively in tmpfs. */
static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int running, stopping, enabled, armed;
    uint64_t since;
    char config[512], pending[512], lease[512], effective[512];
    char fault[40];
} A={.lock=PTHREAD_MUTEX_INITIALIZER};

static uint64_t mono(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec;
}
static int remove_file(const char *p){return !unlink(p)||errno==ENOENT?0:-1;}
static int active_locked(uint64_t now)
{
    unsigned char *raw=NULL;size_t n=0;unsigned long long stamp;long pid;int used=0,ok=0;
    if(!joan_read_file(A.effective,&raw,&n,80)&&
       sscanf((char*)raw,"v1 %ld %llu%n",&pid,&stamp,&used)==2&&pid>1&&
       ((size_t)used==n||((size_t)used+1==n&&raw[used]=='\n'))&&
       stamp<=now&&now-stamp<=2)ok=1;
    free(raw);return ok;
}
static void recover_locked(const char *reason)
{
    (void)remove_file(A.lease);
    A.enabled=0;A.armed=0;
    snprintf(A.fault,sizeof(A.fault),"%s",reason);
    /* If this write fails the pending marker remains: next startup also
     * refuses to re-pause. Never renew a lease after a recovery failure. */
    if(!joan_write_atomic(A.config,"0\n",2,0600))
        (void)remove_file(A.pending);
}
static void *loop(void *unused)
{
    (void)unused;
    for(;;){struct timespec delay={1,0};uint64_t now;int health;char lease[64];int n;
        pthread_mutex_lock(&A.lock);
        if(A.stopping){(void)remove_file(A.lease);pthread_mutex_unlock(&A.lock);break;}
        now=mono();health=joan_rtsp_proxy_audio_health();
        if(A.enabled&&now){
            if(!A.armed&&health==1){
                if(joan_write_atomic(A.pending,"1\n",2,0600))recover_locked("storage_unavailable");
                else{A.armed=1;A.since=now;}
            }
            if(A.armed){
                if(health!=1)recover_locked("video_stalled");
                else if(now-A.since>JOAN_AUDIO_ACK_SECONDS&&!active_locked(now))recover_locked("callback_unavailable");
                else{
                    n=snprintf(lease,sizeof(lease),"v1 %llu\n",(unsigned long long)(now+8));
                    if(joan_write_atomic(A.lease,lease,(size_t)n,0600))recover_locked("lease_unavailable");
                    else if(now-A.since>=JOAN_AUDIO_TRIAL_SECONDS&&(remove_file(A.pending)))recover_locked("storage_unavailable");
                }
            }
        }
        pthread_mutex_unlock(&A.lock);
        nanosleep(&delay,NULL);
    }
    return NULL;
}
int joan_audio_pause_start(const JoanConfig *cfg)
{
    unsigned char *raw=NULL;size_t n=0;
    if(snprintf(A.config,sizeof(A.config),"%s/audio-pause",cfg->state_dir)>=(int)sizeof(A.config)||
       snprintf(A.pending,sizeof(A.pending),"%s/audio-pause.pending",cfg->state_dir)>=(int)sizeof(A.pending)||
       snprintf(A.lease,sizeof(A.lease),"%s/audio-pause.lease",cfg->run_dir)>=(int)sizeof(A.lease)||
       snprintf(A.effective,sizeof(A.effective),"%s/audio-pause.active",cfg->run_dir)>=(int)sizeof(A.effective)||
       joan_mkdir_p(cfg->run_dir,0700))return -1;
    (void)remove_file(A.lease);(void)remove_file(A.effective);
    A.enabled=!joan_read_file(A.config,&raw,&n,8)&&n==2&&!memcmp(raw,"1\n",2);
    free(raw);A.armed=A.stopping=0;A.fault[0]=0;
    if(access(A.pending,F_OK)==0)recover_locked("previous_start_interrupted");
    if(pthread_create(&A.thread,NULL,loop,NULL))return -1;
    A.running=1;return 0;
}
void joan_audio_pause_stop(void)
{
    if(!A.running)return;
    pthread_mutex_lock(&A.lock);A.stopping=1;pthread_mutex_unlock(&A.lock);
    pthread_join(A.thread,NULL);A.running=0;
    /* A clean restart is safe; an abrupt death leaves the trial marker. */
    (void)remove_file(A.pending);
}
int joan_audio_pause_set(int enabled)
{
    int rc;
    if(!A.running)return -1;
    pthread_mutex_lock(&A.lock);
    rc=joan_write_atomic(A.config,enabled?"1\n":"0\n",2,0600);
    if(!rc){
        (void)remove_file(A.lease);
        if(remove_file(A.pending))rc=-1;
        A.enabled=rc?0:enabled;A.armed=0;A.fault[0]=0;
    }
    pthread_mutex_unlock(&A.lock);return rc;
}
void joan_audio_pause_status(char *out,size_t cap)
{
    int active;const char *state;
    pthread_mutex_lock(&A.lock);
    active=A.enabled&&active_locked(mono());
    state=A.fault[0]?"recovered":!A.enabled?"running":active?"paused":A.armed?"starting":"waiting_for_video";
    snprintf(out,cap,"{\"enabled\":%s,\"active\":%s,\"state\":\"%s\",\"recovery_reason\":\"%s\"}",
             A.enabled?"true":"false",active?"true":"false",state,A.fault);
    pthread_mutex_unlock(&A.lock);
}
