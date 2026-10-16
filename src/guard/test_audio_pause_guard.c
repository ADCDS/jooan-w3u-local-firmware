#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int (*audio_sleep)(unsigned);
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
int main(void)
{
    static const char *bad[]={"garbage\n","v1 0\n","v1 -1\n","v1 18446744073709551615\n","v1 1 extra\n"};
    unsigned i;double start;
    *(void **)(&audio_sleep)=dlsym(RTLD_DEFAULT,"jooan_guard_test_audio_sleep");
    if(!audio_sleep)return 1;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        if(lease(bad[i]))return 1;
        start=mono();audio_sleep(10000);if(mono()-start>.3)return 1;
    }
    if(pause_then_resume(0)||pause_then_resume(1))return 1;
    unlink(getenv("JOOAN_GUARD_TEST_AUDIO_LEASE"));
    puts("audio pause: cooperative wait, unrelated caller, disable and lease expiry PASS");return 0;
}
