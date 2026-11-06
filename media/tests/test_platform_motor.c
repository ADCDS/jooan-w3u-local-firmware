/* SPDX-License-Identifier: MIT */
/* Independently emulate the recovered finite-step ABI and its axis swap.
 * A completed move drops coils; smooth nudges must submit only one move. */
#define HOMED_MARKER "/tmp/jipc-test-homed"
#define MOTOR_INFO "/tmp/jipc-test-motor-info"
#include "../src/platform.c"
#include <assert.h>
static int driver_x=1400,driver_y=400,swapped,speed=450,moving,dx,dy;
static unsigned commands,stops,resets;
static pthread_mutex_t *worker_lock;
static int32_t last_reset[4];
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
    if(request==MOTOR_RESET){
        int32_t *r=pointer;assert(!moving);memcpy(last_reset,r,sizeof(last_reset));++resets;
        if(r[0]||r[1]||r[2]||r[3]){driver_x=r[2];driver_y=r[3];return 0;} /* declare position */
        /* The driver's own reset, as observed on the JA-A12: it logs
         * cx=4080,cy=1200 and ends idle at 2040,1199. */
        /* It blocks for 20 s or more on the camera: nobody may be waiting on the lock. */
        if(worker_lock){assert(!pthread_mutex_trylock(worker_lock));pthread_mutex_unlock(worker_lock);}
        driver_x=4080;driver_y=1199;dx=2040-4080;dy=0;swapped=0;moving=1;return 0;
    }
    assert(request==3||request==8);assert(!moving);
    int32_t *delta=pointer;assert((delta[0]!=0)!=(delta[1]!=0));++commands;
    if(delta[0]){swapped=1;dx=delta[0];}
    else{if(request==3)swapped=0;if(swapped)dx=delta[1];else dy=delta[1];}
    moving=1;return 0;
}
static platform_state state_new(void)
{ return (platform_state){.motor_fd=19,.pan_max=4080,.tilt_max=1230,.rate=450}; }
static void tick(platform_state *state,uint64_t now)
{
    advance(now);pthread_mutex_lock(&state->mutex);worker_lock=&state->mutex;
    native_tick(state,now);
    worker_lock=NULL;pthread_mutex_unlock(&state->mutex);
}
static void go(platform_state *state,int x,int y)
{
    state->target_x=x;state->target_y=y;state->goto_active=1;state->goto_until=clock_us+PRESET_LIMIT_US;
    unsigned before=commands;
    for(unsigned i=0;i<2000&&state->goto_active;++i)tick(state,clock_us+10000);
    assert(!state->goto_active&&!state->faulted&&!state->moving&&!swapped);
    assert(driver_x==x&&driver_y==y&&state->x==x&&state->y==y);
    assert(commands-before==2); /* one continuous displacement per axis */
}
static void homing(void)
{
    FILE *info=fopen(MOTOR_INFO,"w");assert(info);
    fputs("## motor is Horizontal motor ##\nmax steps 4080\n## motor is Vertical motor ##\nmax steps 1230\n",info);fclose(info);
    unlink(HOMED_MARKER);
    platform_state s=state_new();s.saved_valid=1;s.saved_x=1378;s.saved_y=397;
    driver_x=900;driver_y=50;unsigned before=resets;
    assert(!motor_home_begin(&s,clock_us));
    assert(speed==450&&resets==before+1&&s.homing==1);
    assert(last_reset[0]==4080&&last_reset[1]==1200&&last_reset[2]==2040&&last_reset[3]==200);
    assert(moving&&dx==2040&&driver_x==2040&&driver_y==200); /* pan run into the end stop */
    jipc_app app={0};pthread_mutex_init(&s.mutex,NULL);app.platform_state=&s;s.app=&app;char error[128];
    assert(platform_control(&app,"ptz",50,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"homing"));
    assert(platform_control(&app,"preset_goto",1,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"homing"));
    for(unsigned i=0;i<20000&&(s.homing||s.goto_active);++i)tick(&s,clock_us+10000);
    assert(s.homed&&!s.homing&&!s.faulted&&!s.goto_active);
    assert(resets==before+2&&!last_reset[0]&&!last_reset[1]&&!last_reset[2]&&!last_reset[3]);
    assert(driver_x==1378&&driver_y==397&&s.x==1378&&s.y==397); /* back at the last position */
    assert(s.pan_max==4080&&s.tilt_max==1230&&!access(HOMED_MARKER,F_OK));
    /* Only a trusted, unfaulted position is persisted for the next boot. */
    snprintf(s.position_path,sizeof(s.position_path),"/tmp/jipc-test-position");unlink(s.position_path);
    s.position_trusted=0;position_track(&s,clock_us);position_track(&s,clock_us+POSITION_QUIET_US);
    assert(access(s.position_path,F_OK));
    s.position_trusted=1;s.faulted=1;position_track(&s,clock_us);position_track(&s,clock_us+POSITION_QUIET_US);
    assert(access(s.position_path,F_OK));
    s.faulted=0;s.saved_x=1;position_track(&s,clock_us);position_track(&s,clock_us+POSITION_QUIET_US);
    FILE *saved=fopen(s.position_path,"r");int px=0,py=0;assert(saved&&fscanf(saved,"%d %d",&px,&py)==2);fclose(saved);
    assert(px==1378&&py==397);unlink(s.position_path);
    pthread_mutex_destroy(&s.mutex);unlink(HOMED_MARKER);unlink(MOTOR_INFO);
    driver_x=1400;driver_y=400;moving=dx=dy=swapped=0; /* the motion tests below start here */
}
/* Exact moves, with this camera's mounted signs: +x and +y take the motor's step
 * counts down. Measured: that turns the view right and up. */
static void moves(void)
{
    platform_state s=state_new();s.pan_sign=s.tilt_sign=-1;s.known=1;s.position_trusted=1;
    jipc_app app={0};pthread_mutex_init(&s.mutex,NULL);app.platform_state=&s;s.app=&app;char error[128];
    /* Deadlines come from the real clock; the fake one runs far ahead of it. */
    #define SYNC() (clock_us=previous_us=jipc_now_us())
    #define RUN() do{SYNC();for(unsigned i=0;i<20000&&s.goto_active;++i)tick(&s,clock_us+10000);assert(!s.goto_active&&!s.faulted);}while(0)
    moving=dx=dy=swapped=0;driver_x=2000;driver_y=600;assert(!motor_read(&s));
    /* FOV-relative: 1 brings the frame's edge to the centre. */
    assert(!jipc_platform_move(&app,JIPC_MOVE_RELATIVE_FOV,1,1,0,error,sizeof(error)));
    assert(s.target_x==2000-JIPC_FOV_HALF_PAN&&s.target_y==600-JIPC_FOV_HALF_TILT&&s.goto_active&&!s.goto_rate);
    RUN();assert(driver_x==2000-JIPC_FOV_HALF_PAN&&driver_y==600-JIPC_FOV_HALF_TILT);
    assert(!jipc_platform_move(&app,JIPC_MOVE_RELATIVE_FOV,-0.5,-0.5,1,error,sizeof(error)));
    assert(s.goto_rate==900&&s.target_x==2000-JIPC_FOV_HALF_PAN+203&&s.target_y==600-JIPC_FOV_HALF_TILT+110);
    RUN();assert(speed==900);
    /* Never past the travel. */
    assert(!jipc_platform_move(&app,JIPC_MOVE_RELATIVE_FOV,1,1,0.01,error,sizeof(error)));assert(s.goto_rate==100);
    s.target_x=s.x;s.target_y=s.y;RUN();
    driver_x=100;driver_y=100;assert(!motor_read(&s));
    assert(!jipc_platform_move(&app,JIPC_MOVE_RELATIVE_FOV,1,1,0,error,sizeof(error)));assert(s.target_x==0&&s.target_y==0);RUN();
    /* Absolute: the ONVIF position the status reports, both ways. */
    assert(!jipc_platform_move(&app,JIPC_MOVE_ABSOLUTE,-1,-1,0,error,sizeof(error)));assert(s.target_x==4080&&s.target_y==1230);RUN();
    assert(!jipc_platform_move(&app,JIPC_MOVE_ABSOLUTE,0,0,0,error,sizeof(error)));assert(s.target_x==2040&&s.target_y==615);RUN();
    jipc_ptz_status st;assert(!jipc_platform_status(&app,&st,error,sizeof(error)));assert(fabs(st.pan_norm)<0.001&&fabs(st.tilt_norm)<0.001);
    assert(!jipc_platform_move(&app,JIPC_MOVE_ABSOLUTE,0.5,0.5,0,error,sizeof(error)));RUN();
    assert(!jipc_platform_status(&app,&st,error,sizeof(error)));assert(fabs(st.pan_norm-0.5)<0.001&&fabs(st.tilt_norm-0.5)<0.002&&!st.moving);
    /* Relative in the generic space: 1 is half of the travel. */
    driver_x=2040;driver_y=615;assert(!motor_read(&s));
    assert(!jipc_platform_move(&app,JIPC_MOVE_RELATIVE,0.5,0,0,error,sizeof(error)));assert(s.target_x==1020&&s.target_y==615);RUN();
    /* A long slow move gets the time it needs, not the presets' 45 s. */
    driver_x=0;driver_y=0;assert(!motor_read(&s));
    assert(!jipc_platform_move(&app,JIPC_MOVE_STEPS,4080,1230,0.01,error,sizeof(error)));
    assert(s.goto_until>clock_us+UINT64_C(53000000));
    assert(!motor_stop_motion(&s));
    /* Privacy: the head looks down, and nothing else may move it. */
    driver_x=1500;driver_y=300;assert(!motor_read(&s));
    assert(!jipc_platform_privacy(&app,1,0,error,sizeof(error)));
    assert(s.privacy&&s.target_x==1500&&s.target_y==1230&&s.goto_active);RUN();assert(driver_y==1230);
    assert(jipc_platform_move(&app,JIPC_MOVE_ABSOLUTE,0,0,0,error,sizeof(error))<0&&strstr(error,"privacy"));
    assert(platform_control(&app,"ptz",50,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"privacy"));
    assert(platform_control(&app,"ptz_recalibrate",0,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"privacy"));
    assert(platform_control(&app,"preset_set",1,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"privacy"));
    /* Its waiting place is never saved as the place to come back to. */
    s.saved_valid=1;s.saved_x=1;s.saved_y=1;snprintf(s.position_path,sizeof(s.position_path),"/tmp/jipc-test-position");unlink(s.position_path);
    position_track(&s,clock_us);position_track(&s,clock_us+POSITION_QUIET_US+1);assert(access(s.position_path,F_OK));
    /* And it goes back afterwards. */
    assert(!jipc_platform_privacy(&app,0,0,error,sizeof(error)));
    assert(!s.privacy&&s.target_x==1500&&s.target_y==300);RUN();assert(driver_x==1500&&driver_y==300);
    /* A preset as the waiting place. */
    s.presets[2]=(saved_preset){.valid=1,.x=3000,.y=900};strcpy(s.presets[2].name,"Wall");
    assert(!jipc_platform_privacy(&app,1,3,error,sizeof(error)));assert(s.target_x==3000&&s.target_y==900);RUN();
    assert(!jipc_platform_privacy(&app,0,0,error,sizeof(error)));RUN();assert(driver_x==1500&&driver_y==300);
    /* A patrol's stop is not someone steering, nor the last position; a preset is. */
    assert(!platform_control(&app,"preset_patrol",3,0,NULL,NULL,NULL,error,sizeof(error)));
    jipc_ptz_motion mo;assert(!jipc_platform_motion(&app,&mo));assert(s.patrol_move&&mo.moving);
    uint64_t manual=s.manual_at;RUN();assert(s.manual_at==manual);
    s.position_quiet_until=0;position_track(&s,clock_us);position_track(&s,clock_us+POSITION_QUIET_US+1);assert(access(s.position_path,F_OK));
    assert(!platform_control(&app,"preset_goto",3,0,NULL,NULL,NULL,error,sizeof(error)));assert(!s.patrol_move&&s.manual_at>=manual);RUN();
    /* Recalibrate: the boot's run, then back to where it was, with a time. */
    FILE *info=fopen(MOTOR_INFO,"w");assert(info);
    fputs("## motor is Horizontal motor ##\nmax steps 4080\n## motor is Vertical motor ##\nmax steps 1230\n",info);fclose(info);
    driver_x=1700;driver_y=500;assert(!motor_read(&s));s.saved_x=1;s.saved_y=1;
    assert(!platform_control(&app,"ptz_recalibrate",0,0,NULL,NULL,NULL,error,sizeof(error)));
    assert(s.homing==1&&s.saved_x==1700&&s.saved_y==500);
    /* Saved on flash too: a restart during the run must not go back somewhere older. */
    {FILE *pf=fopen(s.position_path,"r");int px=0,py=0;assert(pf&&fscanf(pf,"%d %d",&px,&py)==2);fclose(pf);assert(px==1700&&py==500);}
    /* While the driver is busy, the status answers without asking it. */
    {jipc_ptz_status busy;assert(!jipc_platform_status(&app,&busy,error,sizeof(error)));int gx,gy;assert(jipc_platform_steps(&app,&gx,&gy)<0);}
    assert(platform_control(&app,"ptz_recalibrate",0,0,NULL,NULL,NULL,error,sizeof(error))<0&&strstr(error,"homing"));
    SYNC();for(unsigned i=0;i<40000&&(s.homing||s.goto_active);++i)tick(&s,clock_us+10000);
    assert(s.homed&&s.homed_at&&driver_x==1700&&driver_y==500);
    /* Privacy during homing: its end goes to the waiting place. */
    driver_x=1700;driver_y=500;assert(!motor_read(&s));
    assert(!platform_control(&app,"ptz_recalibrate",0,0,NULL,NULL,NULL,error,sizeof(error)));
    assert(!jipc_platform_privacy(&app,1,0,error,sizeof(error)));assert(!s.goto_active||s.homing);
    SYNC();for(unsigned i=0;i<40000&&(s.homing||s.goto_active);++i)tick(&s,clock_us+10000);
    assert(driver_y==1230&&s.privacy);
    unlink(s.position_path);unlink(HOMED_MARKER);unlink(MOTOR_INFO);
    #undef RUN
    #undef SYNC
}
int main(void)
{
    clock_us=previous_us=1000000;homing();moves();platform_state state=state_new();
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
