#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_env(char *dst,size_t cap,const char*name,const char*fallback){const char*v=getenv(name);snprintf(dst,cap,"%s",v&&*v?v:fallback);}
static void usage(const char*n){fprintf(stderr,"usage: %s [--plain-http] [--bind ADDRESS] [--port N]\n",n);}
static void stop_handler(int signal_number){(void)signal_number;joan_server_stop();}
int main(int argc,char**argv)
{
    JoanConfig c;int i,rc;
    memset(&c,0,sizeof(c));
    copy_env(c.state_dir,sizeof(c.state_dir),"JOAN_STATE_DIR","/var/lib/joan");
    copy_env(c.staging_dir,sizeof(c.staging_dir),"JOAN_STAGING_DIR","/tmp/joan-staging");
    copy_env(c.release_sequence_path,sizeof(c.release_sequence_path),"JOAN_RELEASE_SEQUENCE_PATH","/opt/custom/jooan-local/state/release-sequence");
    copy_env(c.web_dir,sizeof(c.web_dir),"JOAN_WEB_DIR","/usr/share/joan/web");
    copy_env(c.bind_addr,sizeof(c.bind_addr),"JOAN_BIND","0.0.0.0");
    copy_env(c.public_host,sizeof(c.public_host),"JOAN_PUBLIC_HOST","camera.local");
    copy_env(c.integration_helper,sizeof(c.integration_helper),"JOAN_INTEGRATION_HELPER","/usr/libexec/joan-integration");
    copy_env(c.audio_socket,sizeof(c.audio_socket),"JOAN_AUDIO_WS_SOCKET","/run/joan/audio-ws.sock");
    c.port=443;c.redirect_port=80;c.mqtt_port=1883;c.rtsp_port=554;c.mdns_enabled=1;c.mdns_port=5353;
    if(getenv("JOAN_PORT"))c.port=(unsigned)strtoul(getenv("JOAN_PORT"),NULL,10);
    if(getenv("JOAN_MQTT_PORT"))c.mqtt_port=(unsigned)strtoul(getenv("JOAN_MQTT_PORT"),NULL,10);
    if(getenv("JOAN_RTSP_PORT"))c.rtsp_port=(unsigned)strtoul(getenv("JOAN_RTSP_PORT"),NULL,10);
    if(getenv("JOAN_REDIRECT_PORT"))c.redirect_port=(unsigned)strtoul(getenv("JOAN_REDIRECT_PORT"),NULL,10);
    if(getenv("JOAN_MDNS")&&!strcmp(getenv("JOAN_MDNS"),"0"))c.mdns_enabled=0;
    if(getenv("JOAN_MDNS_PORT"))c.mdns_port=(unsigned)strtoul(getenv("JOAN_MDNS_PORT"),NULL,10);
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--plain-http"))c.plain_http=1;
        else if(!strcmp(argv[i],"--bind")&&i+1<argc)snprintf(c.bind_addr,sizeof(c.bind_addr),"%s",argv[++i]);
        else if(!strcmp(argv[i],"--port")&&i+1<argc)c.port=(unsigned)strtoul(argv[++i],NULL,10);
        else{usage(argv[0]);return 2;}
    }
    if(!c.port||c.port>65535||c.mqtt_port>65535||!c.rtsp_port||c.rtsp_port>65535||!c.mdns_port||c.mdns_port>65535){usage(argv[0]);return 2;}
    signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,stop_handler);signal(SIGINT,stop_handler);
    if(joan_auth_init(&c)){fprintf(stderr,"cannot initialize authentication state\n");return 1;}
    if(!c.plain_http&&joan_tls_ensure_identity(&c)){fprintf(stderr,"cannot initialize per-device ECDSA identity; refusing plaintext fallback\n");return 1;}
    if(c.plain_http)fprintf(stderr,"WARNING: explicit plaintext development mode; do not expose beyond a test namespace\n");
    if(joan_mqtt_bridge_start(&c)){fprintf(stderr,"cannot start loopback MQTT compatibility sink\n");return 1;}
    if(joan_mdns_start(&c)){fprintf(stderr,"cannot start mDNS responder\n");return 1;}
    if(joan_fmp4_start(&c)){fprintf(stderr,"cannot start fMP4 stream workers\n");return 1;}
    fprintf(stderr,"joan-daemon %s listening on %s:%u (%s)\n",JOAN_VERSION,c.bind_addr,c.port,c.plain_http?"HTTP DEVELOPMENT MODE":"HTTPS ECDSA");
    rc=joan_server_run(&c);
    joan_mdns_stop();
    joan_mqtt_bridge_stop();
    return rc?1:0;
}
