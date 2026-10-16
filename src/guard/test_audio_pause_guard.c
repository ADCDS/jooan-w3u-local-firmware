#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int (*audio_sleep)(unsigned);
static int (*audio_condition)(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);
static pthread_mutex_t queue_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond=PTHREAD_COND_INITIALIZER;
static int condition_result;
static int done;
static double mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int lease(const char *text)
{
    FILE *f=fopen(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"),"w");
    if(!f)return -1;
    fputs(text,f);return fclose(f);
}
static void *callback(void *unused)
{
    (void)unused;audio_sleep(10000);__atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;
}
static int pause_then_resume(int expire)
{
    char raw[64];pthread_t thread;double start,deadline;const char *active=getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE");
    /* The off-path probe cache is one second; use the real, unrelated sleep. */
    usleep(1100000);
    snprintf(raw,sizeof(raw),"v1 %llu\n",(unsigned long long)mono()+2);
    if(lease(raw))return 1;
    __atomic_store_n(&done,0,__ATOMIC_RELEASE);
    if(pthread_create(&thread,NULL,callback,NULL))return 1;
    deadline=mono()+1;
    while(access(active,F_OK)&&mono()<deadline)usleep(10000);
    if(access(active,F_OK)||__atomic_load_n(&done,__ATOMIC_ACQUIRE))return 1;
    /* Other callers at the same sleep duration must never be paused. */
    start=mono();usleep(10000);if(mono()-start>.3)return 1;
    if(!expire)unlink(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"));
    deadline=mono()+3;
    while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)&&mono()<deadline)usleep(10000);
    if(!__atomic_load_n(&done,__ATOMIC_ACQUIRE))return 1;
    pthread_join(thread,NULL);
    return access(active,F_OK)==0;
}
static void *condition_callback(void *unused)
{
    struct timespec deadline;
    (void)unused;
    if(prctl(PR_SET_NAME,"AE_AudioFrameCB",0,0,0))return NULL;
    clock_gettime(CLOCK_REALTIME,&deadline); /* Past deadline reproduces OEM spin. */
    deadline.tv_sec--;
    pthread_mutex_lock(&queue_mutex);
    condition_result=audio_condition(&queue_cond,&queue_mutex,&deadline);
    /* The caller must again own its mutex on return. */
    if(pthread_mutex_unlock(&queue_mutex))condition_result=-1;
    __atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;
}
static int condition_probe(int expire)
{
    pthread_t thread;char raw[64];double deadline;int i,locked=0;
    const char *active=getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE");
    snprintf(raw,sizeof(raw),"v1 %llu\n",(unsigned long long)mono()+2);
    if(lease(raw))return 1;
    __atomic_store_n(&done,0,__ATOMIC_RELEASE);
    if(pthread_create(&thread,NULL,condition_callback,NULL))return 1;
    deadline=mono()+1;
    while(access(active,F_OK)&&mono()<deadline)usleep(10000);
    if(access(active,F_OK)||__atomic_load_n(&done,__ATOMIC_ACQUIRE))return 1;
    for(i=0;i<30;i++){
        if(!pthread_mutex_trylock(&queue_mutex)){locked=1;pthread_mutex_unlock(&queue_mutex);break;}
        usleep(10000);
    }
    if(!locked)return 1; /* A paused audio thread must release the queue lock. */
    if(!expire)unlink(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"));
    deadline=mono()+3;
    while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)&&mono()<deadline)usleep(10000);
    if(!__atomic_load_n(&done,__ATOMIC_ACQUIRE))return 1;
    pthread_join(thread,NULL);
    return condition_result!=ETIMEDOUT;
}
static void unlock_queue(void *unused){(void)unused;pthread_mutex_unlock(&queue_mutex);}
static void *cancel_callback(void *unused)
{
    struct timespec past;
    (void)unused;prctl(PR_SET_NAME,"AE_AudioFrameCB",0,0,0);
    clock_gettime(CLOCK_REALTIME,&past);past.tv_sec--;
    pthread_mutex_lock(&queue_mutex);
    pthread_cleanup_push(unlock_queue,NULL);
    audio_condition(&queue_cond,&queue_mutex,&past);
    pthread_testcancel();
    pthread_cleanup_pop(1);
    return NULL;
}
static int cancellation_probe(void)
{
    pthread_t thread;void *result=NULL;char raw[64];double deadline;
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"));
    snprintf(raw,sizeof(raw),"v1 %llu\n",(unsigned long long)mono()+8);
    if(lease(raw)||pthread_create(&thread,NULL,cancel_callback,NULL))return 1;
    deadline=mono()+1;
    while(access(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"),F_OK)&&mono()<deadline)usleep(10000);
    if(access(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"),F_OK))return 1;
    deadline=mono()+2;
    if(pthread_cancel(thread)||pthread_join(thread,&result)||result!=PTHREAD_CANCELED||mono()>deadline)return 1;
    if(pthread_mutex_trylock(&queue_mutex))return 1;
    pthread_mutex_unlock(&queue_mutex);
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"));
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"));return 0;
}
int main(void)
{
    static const char *bad[]={"garbage\n","v1 0\n","v1 -1\n","v1 18446744073709551615\n","v1 1 extra\n"};
    unsigned i;double start;
    *(void **)(&audio_sleep)=dlsym(RTLD_DEFAULT,"jooan_guard_test_audio_sleep");
    *(void **)(&audio_condition)=dlsym(RTLD_DEFAULT,"jooan_guard_test_audio_condition");
    if(!audio_sleep||!audio_condition)return 1;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        if(lease(bad[i]))return 1;
        start=mono();audio_sleep(10000);if(mono()-start>.3)return 1;
    }
    if(pause_then_resume(0)||pause_then_resume(1)||condition_probe(0)||condition_probe(1))return 1;
    if(cancellation_probe())return 1;
    /* The same wait site on another thread retains normal timeout behavior,
     * even while a fresh lease asks the actual audio thread to pause. */
    {char raw[64];snprintf(raw,sizeof(raw),"v1 %llu\n",(unsigned long long)mono()+8);if(lease(raw))return 1;}
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"));
    {struct timespec past;clock_gettime(CLOCK_REALTIME,&past);past.tv_sec--;
     pthread_mutex_lock(&queue_mutex);
     if(audio_condition(&queue_cond,&queue_mutex,&past)!=ETIMEDOUT)return 1;
     pthread_mutex_unlock(&queue_mutex);}

    if(!access(getenv("JOOAN_GUARD_TEST_AUDIO_ACTIVE"),F_OK))return 1;
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"));
    puts("audio pause: sleep/condition boundaries, mutex release, unrelated callers and lease expiry PASS");return 0;
}
