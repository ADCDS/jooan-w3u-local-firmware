/* Host-only monotonic authentication regression. Simulate a wall-clock jump
 * while retaining real monotonic time; never change the host or camera clock.
 * Build: cc -O2 -pthread -Wall -Wextra -Werror -ffunction-sections -fdata-sections
 *   -Wl,--gc-sections -o /tmp/test_auth_clock tests/test_auth_clock.c
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int joan_ct_equal(const void *a,const void *b,size_t n)
{ const unsigned char *x=a,*y=b;unsigned char d=0;for(size_t i=0;i<n;i++)d|=x[i]^y[i];return d==0; }
static time_t fake_wall = 1700000000;
static time_t fake_mono = 10000;
#define time fake_time
#define clock_gettime fake_clock_gettime
static time_t fake_time(time_t *out) { if(out)*out=fake_wall;return fake_wall; }
static int fake_clock_gettime(clockid_t clock,struct timespec *ts)
{ ts->tv_sec=clock==CLOCK_MONOTONIC?fake_mono:fake_wall;ts->tv_nsec=0;return 0; }
#include "../src/daemon/auth.c"
#undef time
#undef clock_gettime

/* Unused authentication storage helpers are removed by --gc-sections. */
int main(void)
{
    JoanRequest request={0}; JoanAuthz out={0};
    strcpy(sessions[0].token,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    strcpy(sessions[0].csrf,"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    sessions[0].used=1;sessions[0].expires=fake_mono+SESSION_SECONDS;
    snprintf(request.cookie,sizeof(request.cookie),"joan_session=%s",sessions[0].token);
    strcpy(request.csrf,sessions[0].csrf);
    assert(joan_auth_request(&request,1,&out)==0);
    fake_wall+=10u*365u*24u*3600u; /* simulated manual clock correction */
    assert(joan_auth_request(&request,1,&out)==0);
    request.csrf[0]=0;
    assert(joan_auth_request(&request,1,&out)==-2);
    assert(joan_auth_request(&request,0,&out)==0);
    strcpy(request.csrf,sessions[0].csrf);
    /* A stale parent-domain copy before the live cookie must not hide it. */
    snprintf(request.cookie,sizeof(request.cookie),
             "joan_session=%064d; joan_session=%s",0,sessions[0].token);
    assert(joan_auth_request(&request,1,&out)==0);
    /* A cookie merely ending in joan_session= is not the session cookie. */
    snprintf(request.cookie,sizeof(request.cookie),"xjoan_session=%s",sessions[0].token);
    assert(joan_auth_request(&request,0,&out)==-1);
    snprintf(request.cookie,sizeof(request.cookie),"joan_session=%s",sessions[0].token);
    fake_mono+=SESSION_SECONDS+1;
    assert(joan_auth_request(&request,1,&out)==-1);
    puts("auth monotonic expiry, clock jump and CSRF separation: PASS");
    return 0;
}
