/* SPDX-License-Identifier: MIT */
#include "../src/platform.c"
#include <assert.h>
typedef struct { jipc_app *app; const char *name; int slot; } create_arg;
static void *create_preset(void *p)
{
    create_arg *a=p;char error[128];
    assert(!jipc_platform_preset_create(a->app,&a->slot,a->name,error,sizeof(error)));
    return NULL;
}
int main(void)
{
    char root[]="/tmp/jipc-presets-XXXXXX",error[256],legacy[640],path[640];assert(mkdtemp(root));
    jipc_app app={0};jipc_config_defaults(&app.config);strcpy(app.config.state_dir,root);
    pthread_mutex_init(&app.config_mutex,NULL);
    snprintf(legacy,sizeof(legacy),"%s/presets.v1",root);snprintf(path,sizeof(path),"%s/presets.v2.json",root);
    const char *v1="JIPC_PRESETS_1 replay\n1 0 0\n2 30 -70\n";
    assert(!jipc_atomic_file(legacy,v1,strlen(v1),0600));jipc_ptz_status status;
    assert(!jipc_platform_status(&app,&status,error,sizeof(error)));assert(status.preset_mask==3);
    assert(!strcmp(status.preset_names[0],"Preset 1"));
    assert(!jipc_platform_preset_set(&app,1,"RUA & <CIMA>",error,sizeof(error)));
    assert(jipc_platform_preset_set(&app,2,"rua & <cima>",error,sizeof(error))<0);
    assert(jipc_platform_preset_set(&app,2,"bad\nlabel",error,sizeof(error))<0);
    assert(jipc_platform_preset_set(&app,2,"\xc0\xaf",error,sizeof(error))<0);
    assert(!jipc_platform_preset_set(&app,2,NULL,error,sizeof(error)));
    assert(!jipc_platform_preset_set(&app,3,"literal \\u0000",error,sizeof(error)));
    create_arg a={&app,"Parallel A",0},b={&app,"Parallel B",0};pthread_t ta,tb;
    assert(!pthread_create(&ta,NULL,create_preset,&a));assert(!pthread_create(&tb,NULL,create_preset,&b));
    pthread_join(ta,NULL);pthread_join(tb,NULL);assert(a.slot!=b.slot&&a.slot>=4&&b.slot>=4);
    assert(!jipc_platform_control(&app,"preset_remove",a.slot,0,error,sizeof(error)));
    assert(!jipc_platform_control(&app,"preset_remove",b.slot,0,error,sizeof(error)));
    jipc_platform_stop(&app);
    struct stat st;assert(!stat(path,&st)&&(st.st_mode&0777)==0600);
    assert(!jipc_platform_status(&app,&status,error,sizeof(error)));
    assert(!strcmp(status.preset_names[0],"RUA & <CIMA>")&&!strcmp(status.preset_names[1],"Preset 2"));
    platform_state *state=app.platform_state;char saved[640];strcpy(saved,state->path);strcpy(state->path,"/proc/jipc-cannot-write/presets");
    assert(jipc_platform_preset_set(&app,1,"failed replacement",error,sizeof(error))<0);
    assert(!strcmp(state->presets[0].name,"RUA & <CIMA>"));strcpy(state->path,saved);
    assert(!jipc_platform_control(&app,"preset_remove",2,0,error,sizeof(error)));
    assert(!jipc_platform_preset_set(&app,1,"Preset 2",error,sizeof(error)));
    assert(jipc_platform_preset_set(&app,2,NULL,error,sizeof(error))<0); /* default name collision */
    assert(!jipc_platform_control(&app,"preset_remove",1,0,error,sizeof(error)));
    assert(!jipc_platform_control(&app,"preset_remove",2,0,error,sizeof(error)));
    assert(!jipc_platform_control(&app,"preset_remove",3,0,error,sizeof(error)));jipc_platform_stop(&app);
    assert(!jipc_platform_status(&app,&status,error,sizeof(error))&&!status.preset_mask);jipc_platform_stop(&app);
    assert(!jipc_atomic_file(path,"{}",2,0600));
    assert(jipc_platform_status(&app,&status,error,sizeof(error))<0&&!app.platform_state); /* no v1 fallback */
    const char *raw="{\"version\":2,\"backend\":\"imp\",\"coordinate_space\":\"motor_steps\",\"presets\":[{\"slot\":4,\"name\":\"PRESET D\",\"x\":1000,\"y\":0}]}";
    assert(!jipc_atomic_file(path,raw,strlen(raw),0600));
    platform_state native={.app=&app};strcpy(native.path,path);assert(!presets_load(&native));
    assert(native.presets[3].x==1000&&native.presets[3].y==0&&!strcmp(native.presets[3].name,"PRESET D"));
    unlink(path);unlink(legacy);rmdir(root);pthread_mutex_destroy(&app.config_mutex);
    puts("Presets: v1 migration, names, raw coordinates, deletion, rollback, invalid catalog isolation");return 0;
}
