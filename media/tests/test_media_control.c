/* SPDX-License-Identifier: MIT
 * Exercise orientation updates through the real public control path using an
 * inert IMP pointer: these operations must persist/restart, never call SDK. */
#include "../src/media.c"
#include <assert.h>
#include <unistd.h>
/* Privacy's frame hold, as a camera that starts with it on goes through it. Both
 * ways it took camera1 down were here: the startup check starves without parameter
 * sets, and the watchdog's check fails between the hold and the first keyframe. */
static void privacy_hold(void)
{
    static const unsigned char idr[]={0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0xce,0x3c,0x80,0,0,0,1,0x65,0x88,0x84};
    static const unsigned char p[]={0,0,0,1,0x41,0x9a,0x24};
    jipc_app *v=calloc(1,sizeof(*v));assert(v);jipc_config_defaults(&v->config);v->config.sensors=1;v->config.main_only=1;
    pthread_mutex_init(&v->config_mutex,NULL);
    assert(!jipc_streams_init(v));
    uint64_t t=jipc_now_us();
    assert(!jipc_video_healthy(v,2,t));                       /* nothing yet: unhealthy */
    v->privacy_hold=1;
    assert(!jipc_publish_video(v,0,1,idr,sizeof(idr),t,0));
    for(unsigned ch=0;ch<2;++ch)assert(!v->streams[ch].sequence&&v->streams[ch].sps_size==4&&v->streams[ch].pps_size==4);
    assert(jipc_video_healthy(v,2,t));                        /* held on purpose */
    v->privacy_hold=0;
    assert(jipc_video_healthy(v,2,t+UINT64_C(14000000)));     /* waiting for a keyframe */
    assert(!jipc_video_healthy(v,2,t+UINT64_C(16000000)));    /* an encoder that never resumes still fails */
    /* It resumes at a keyframe, not on P-frames that decode onto the old picture. */
    assert(!jipc_publish_video(v,0,1,p,sizeof(p),t,0));assert(!v->streams[0].sequence);
    assert(!jipc_publish_video(v,0,1,idr,sizeof(idr),t,0));assert(v->streams[0].sequence==1&&v->streams[1].sequence==1);
    assert(!jipc_publish_video(v,0,1,p,sizeof(p),t,0));assert(v->streams[0].sequence==2);
    assert(jipc_video_healthy(v,2,t+UINT64_C(1000000)));
    assert(!jipc_video_healthy(v,2,t+UINT64_C(16000000)));    /* and frames must keep coming */
    /* Turned on later: stale stamps are fine while held. */
    v->privacy_hold=1;assert(jipc_video_healthy(v,2,t+UINT64_C(40000000)));v->privacy_hold=0;
    assert(jipc_video_healthy(v,2,t+UINT64_C(41000000)));
    assert(!jipc_video_healthy(v,2,t+UINT64_C(60000000)));
}
int main(void)
{
    privacy_hold();
    char temporary[]="/tmp/jipc-flip-XXXXXX",error[256];
    assert(mkdtemp(temporary));
    jipc_app app={0};jipc_config_defaults(&app.config);
    strcpy(app.config.backend,"imp");strcpy(app.config.libimp,"/external/libimp.so");
    strcpy(app.config.password,"fixture-password");
    snprintf(app.config.config_path,sizeof(app.config.config_path),"%s/camera.ini",temporary);
    app.config.flip0=3;app.config.sensors=2;
    pthread_mutex_init(&app.config_mutex,NULL);pthread_mutex_init(&app.control_mutex,NULL);
    media_state state={.imp=(void *)1,.config=app.config};
    app.backend_state=&state;app.backend_ready=1;
    assert(!jipc_backend_control(&app,"vflip",0,0,error,sizeof(error)));
    assert(app.config.flip0==1&&app.config.flip1==0&&app.restart);
    app.restart=0;
    assert(!jipc_backend_control(&app,"hflip",1,1,error,sizeof(error)));
    assert(app.config.flip0==1&&app.config.flip1==1&&app.restart);
    assert(!jipc_backend_control(&app,"flip",0,0,error,sizeof(error)));
    assert(app.config.flip0==0&&app.config.flip1==1);
    assert(jipc_backend_control(&app,"flip",4,0,error,sizeof(error))<0);
    assert(jipc_backend_control(&app,"hflip",2,0,error,sizeof(error))<0);
    assert(jipc_backend_control(&app,"flip",3,2,error,sizeof(error))<0);
    jipc_config loaded;jipc_config_defaults(&loaded);
    assert(!jipc_config_load(&loaded,app.config.config_path,error,sizeof(error)));
    assert(loaded.flip0==0&&loaded.flip1==1);
    /* Motor Stop must not wait behind any encoder or JPEG SDK operation. */
    strcpy(app.config.backend,"replay");strcpy(app.config.state_dir,temporary);
    pthread_mutex_lock(&app.control_mutex);
    assert(!jipc_backend_control(&app,"ptz_stop",0,0,error,sizeof(error)));
    pthread_mutex_unlock(&app.control_mutex);
    jipc_platform_stop(&app);
    unlink(app.config.config_path);rmdir(temporary);
    pthread_mutex_destroy(&app.control_mutex);pthread_mutex_destroy(&app.config_mutex);
    puts("Orientation controls persist per-sensor bits and request ordered restart");
    return 0;
}
