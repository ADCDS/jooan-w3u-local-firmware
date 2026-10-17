/* SPDX-License-Identifier: MIT */
/* The OEM request8 Y path keeps the X-axis coordinate swap; request3 restores
 * it. Exercise the real controller against that independently recovered ABI. */
#include "../src/platform.c"
#include <assert.h>

static int driver_x=1400,driver_y=400,swapped;
static unsigned commands;
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd==19);
    if(request==MOTOR_STOP){swapped=0;return 0;}
    va_list ap;va_start(ap,request);void *pointer=va_arg(ap,void *);va_end(ap);
    if(request==MOTOR_POSITION){motor_position *p=pointer;*p=(motor_position){driver_x,driver_y,0,450};return 0;}
    assert(request==3||request==8);
    int32_t *delta=pointer;assert((delta[0]!=0)!=(delta[1]!=0));
    assert(abs(delta[0])<=90&&abs(delta[1])<=90);++commands;
    if(delta[0]){swapped=1;driver_x+=delta[0];}
    else {
        if(request==3)swapped=0;
        if(swapped)driver_x+=delta[1];else driver_y+=delta[1];
    }
    return 0;
}
static void go(platform_state *state,int x,int y,uint64_t now)
{
    state->target_x=x;state->target_y=y;state->goto_active=1;state->goto_until=now+PRESET_LIMIT_US;
    for(unsigned i=0;i<100&&state->goto_active;++i)native_tick(state,now+i*25000);
    assert(!state->goto_active&&!state->faulted&&!state->moving);
    assert(driver_x==x&&driver_y==y&&state->x==x&&state->y==y);
}
int main(void)
{
    platform_state state={.motor_fd=19};uint64_t now=1000000;
    go(&state,1650,680,now);go(&state,1400,400,now);
    go(&state,1280,620,now);go(&state,1400,400,now);
    assert(commands>8);
    state.goto_active=1;state.goto_until=now-1;native_tick(&state,now);
    assert(state.faulted&&!state.goto_active&&strstr(state.last_error,"preset"));
    state=(platform_state){.motor_fd=19,.vx=50,.vy=50,.lease_until=now+MANUAL_LEASE_US};
    for(unsigned i=0;i<=48;++i)native_tick(&state,now+i*25000);
    assert(!state.faulted&&!state.moving&&!state.lease_until&&!state.vx&&!state.vy);
    assert(driver_x>1400&&driver_y>400);
    jipc_app app={0};state=(platform_state){.motor_fd=19,.pan_sign=-1,.tilt_sign=-1,.mutex=PTHREAD_MUTEX_INITIALIZER};
    app.platform_state=&state;state.app=&app;char error[128];
    assert(!jipc_platform_control(&app,"ptz",50,50,error,sizeof(error)));
    assert(state.vx==-50&&state.vy==-50); /* controls follow the flipped image */
    assert(!jipc_platform_control(&app,"ptz_stop",0,0,error,sizeof(error)));
    pthread_mutex_destroy(&state.mutex);
    puts("Native motor: mixed-axis presets converge using request3; timeout reported; manual lease stops");
    return 0;
}
