/* SPDX-License-Identifier: MIT */
/* Independently emulate the recovered finite-step ABI and its axis swap.
 * A completed move drops coils; smooth nudges must submit only one move. */
#include "../src/platform.c"
#include <assert.h>
static int driver_x=1400,driver_y=400,swapped,speed=450,moving,dx,dy;
static unsigned commands,stops;
static uint64_t clock_us,previous_us;
static void advance(uint64_t now)
{
    unsigned steps=(unsigned)((now-previous_us)*(unsigned)speed/1000000);previous_us=now;clock_us=now;
    if(!moving)return;
    int *delta=dx?&dx:&dy,*position=dx?&driver_x:&driver_y;
    int n=abs(*delta)<(int)steps?abs(*delta):(int)steps;if(*delta<0)n=-n;
    *position+=n;*delta-=n;moving=!!(dx||dy);
}
int __wrap_ioctl(int fd,unsigned long request,...)
{
    assert(fd==19);
    if(request==MOTOR_STOP){swapped=0;moving=dx=dy=0;++stops;return 0;}
    va_list ap;va_start(ap,request);void *pointer=va_arg(ap,void *);va_end(ap);
    if(request==MOTOR_POSITION){*(motor_position*)pointer=(motor_position){driver_x,driver_y,moving,speed};return 0;}
    if(request==MOTOR_SPEED){speed=*(int*)pointer;assert(speed>=100&&speed<=900);return 0;}
    assert(request==3||request==8);assert(!moving);
    int32_t *delta=pointer;assert((delta[0]!=0)!=(delta[1]!=0));++commands;
    if(delta[0]){swapped=1;dx=delta[0];}
    else{if(request==3)swapped=0;if(swapped)dx=delta[1];else dy=delta[1];}
    moving=1;return 0;
}
static platform_state state_new(void)
{ return (platform_state){.motor_fd=19,.pan_max=4080,.tilt_max=1230,.rate=450}; }
static void tick(platform_state *state,uint64_t now){advance(now);native_tick(state,now);}
static void go(platform_state *state,int x,int y)
{
    state->target_x=x;state->target_y=y;state->goto_active=1;state->goto_until=clock_us+PRESET_LIMIT_US;
    unsigned before=commands;
    for(unsigned i=0;i<2000&&state->goto_active;++i)tick(state,clock_us+10000);
    assert(!state->goto_active&&!state->faulted&&!state->moving&&!swapped);
    assert(driver_x==x&&driver_y==y&&state->x==x&&state->y==y);
    assert(commands-before==2); /* one continuous displacement per axis */
}
int main(void)
{
    clock_us=previous_us=1000000;platform_state state=state_new();
    go(&state,1650,680);go(&state,1400,400);go(&state,1280,620);go(&state,1400,400);
    state.goto_active=1;state.goto_until=clock_us-1;native_tick(&state,clock_us);
    assert(state.faulted&&!state.goto_active&&strstr(state.last_error,"preset"));
    state=state_new();state.vx=50;state.rate=540;state.nudge=1;state.lease_until=clock_us+NUDGE_US;
    unsigned before=commands,old_stops=stops;int x=driver_x;uint64_t start=clock_us;
    for(unsigned i=0;i<=35;++i)tick(&state,start+i*10000);
    assert(commands==before+1&&stops==old_stops+1);
    assert(driver_x-x>=170&&driver_x-x<=190);
    assert(!state.moving&&!state.lease_until&&!state.vx);
    for(unsigned i=0;i<400;++i)tick(&state,clock_us+10000);
    assert(commands==before+1&&stops==old_stops+1); /* no delayed restart or idle STOP */
    jipc_app app={0};state=state_new();state.pan_sign=-1;state.tilt_sign=-1;
    pthread_mutex_init(&state.mutex,NULL);app.platform_state=&state;state.app=&app;char error[128];
    uint64_t generation=0;
    assert(!platform_control(&app,"ptz_nudge",50,25,NULL,&generation,NULL,error,sizeof(error)));
    assert(state.vx==-50&&state.vy==0&&state.rate==540);
    uint64_t until=state.lease_until;
    assert(!platform_control(&app,"ptz_nudge",50,25,NULL,NULL,NULL,error,sizeof(error)));
    assert(state.lease_until==until&&state.generation==generation); /* no lease extension */
    assert(!jipc_platform_control(&app,"ptz_stop",0,0,error,sizeof(error)));
    assert(state.generation!=generation&&!state.lease_until);
    assert(!jipc_platform_control(&app,"ptz",50,75,error,sizeof(error)));
    assert(state.vx==0&&state.vy==-75&&state.rate==675);
    old_stops=stops;generation=state.generation;
    assert(!jipc_platform_control(&app,"ptz",50,75,error,sizeof(error)));
    assert(state.generation==generation&&stops==old_stops);
    assert(!jipc_platform_control(&app,"ptz_stop",0,0,error,sizeof(error)));
    state.presets[0]=(saved_preset){.valid=1,.x=5000,.y=0};
    assert(jipc_platform_control(&app,"preset_goto",1,0,error,sizeof(error))<0);
    pthread_mutex_destroy(&state.mutex);
    puts("Motor: steady finite nudges, prompt stop, coalescing, no delayed restart, calibrated preset bounds");
    return 0;
}
