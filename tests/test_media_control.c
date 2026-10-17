/* SPDX-License-Identifier: MIT
 * Exercise orientation updates through the real public control path using an
 * inert IMP pointer: these operations must persist/restart, never call SDK. */
#include "../src/media.c"
#include <assert.h>
#include <unistd.h>
int main(void)
{
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
    unlink(app.config.config_path);rmdir(temporary);
    pthread_mutex_destroy(&app.control_mutex);pthread_mutex_destroy(&app.config_mutex);
    puts("Orientation controls persist per-sensor bits and request ordered restart");
    return 0;
}
