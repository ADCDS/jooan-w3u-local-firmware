#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef JOAN_NO_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

typedef struct {
    int fd;
#ifndef JOAN_NO_TLS
    mbedtls_ssl_context *ssl;
#endif
} Conn;
static JoanConfig G;
static pthread_mutex_t ptz_lock=PTHREAD_MUTEX_INITIALIZER;
static char ptz_lease[65]; static time_t ptz_expires;

static ssize_t cread(Conn*c,void*b,size_t n){
#ifndef JOAN_NO_TLS
    if(c->ssl){int r=mbedtls_ssl_read(c->ssl,b,n);return r>0?r:-1;}
#endif
    return recv(c->fd,b,n,0);
}
static ssize_t cwrite(Conn*c,const void*b,size_t n){
#ifndef JOAN_NO_TLS
    if(c->ssl){int r=mbedtls_ssl_write(c->ssl,b,n);return r>0?r:-1;}
#endif
    return send(c->fd,b,n,MSG_NOSIGNAL);
}
static int wall(Conn*c,const void*b,size_t n){const unsigned char*p=b;while(n){ssize_t z=cwrite(c,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return-1;p+=z;n-=z;}return 0;}
static const char *reason(int s){switch(s){case 200:return"OK";case 201:return"Created";case 204:return"No Content";case 400:return"Bad Request";case 401:return"Unauthorized";case 403:return"Forbidden";case 404:return"Not Found";case 409:return"Conflict";case 413:return"Payload Too Large";case 415:return"Unsupported Media Type";case 428:return"Precondition Required";case 429:return"Too Many Requests";case 500:return"Internal Server Error";case 501:return"Not Implemented";case 502:return"Bad Gateway";default:return"Error";}}
static int response(Conn*c,int status,const char*type,const void*body,size_t len,const char*extra){char h[2048];int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; connect-src 'self' wss: ws:; img-src 'self' data:; style-src 'self'; script-src 'self'\r\nCache-Control: no-store\r\n%s\r\n",status,reason(status),type?type:"application/octet-stream",(unsigned long)len,extra?extra:"");return n<=0||(size_t)n>=sizeof(h)||wall(c,h,(size_t)n)||wall(c,body,len)?-1:0;}
static int json(Conn*c,int status,const char*s){return response(c,status,"application/json; charset=utf-8",s,strlen(s),NULL);}
static void json_escape(const unsigned char*in,size_t n,char*out,size_t cap){size_t i,o=0;for(i=0;i<n&&o+7<cap;i++){unsigned char x=in[i];if(x=='"'||x=='\\'){out[o++]='\\';out[o++]=x;}else if(x>=32&&x<127)out[o++]=x;else{o+=snprintf(out+o,cap-o,"\\u%04x",x);} }out[o]=0;}

static int json_field(const unsigned char*body,size_t len,const char*key,char*out,size_t cap){char needle[96];const char*p,*e;size_t n;if(!body||snprintf(needle,sizeof(needle),"\"%s\"",key)>=(int)sizeof(needle))return-1;p=strstr((const char*)body,needle);if(!p||(size_t)(p-(const char*)body)>=len)return-1;p+=strlen(needle);while(*p==' '||*p=='\t')p++;if(*p++!=':')return-1;while(*p==' '||*p=='\t')p++;if(*p++!='"')return-1;e=p;while(*e&&*e!='"'&&(size_t)(e-(const char*)body)<len){if(*e=='\\')return-1;e++;}n=(size_t)(e-p);if(*e!='"'||n+1>cap)return-1;memcpy(out,p,n);out[n]=0;return 0;}
static int header_copy(const char*headers,const char*name,char*out,size_t cap){size_t nl=strlen(name);const char*p=headers;while((p=strstr(p,name))){if((p==headers||p[-1]=='\n')&&!strncasecmp(p,name,nl)&&p[nl]==':'){const char*v=p+nl+1,*e;while(*v==' '||*v=='\t')v++;e=strstr(v,"\r\n");if(!e)return-1;size_t n=(size_t)(e-v);if(n>=cap)n=cap-1;memcpy(out,v,n);out[n]=0;return 0;}p+=nl;}return-1;}

static int parse_request(Conn*c,JoanRequest*r){unsigned char*h=malloc(JOAN_MAX_HEADERS+1),*end;size_t used=0,head;char*line,*save=NULL,*q;ssize_t n;if(!h)return-1;memset(r,0,sizeof(*r));r->fd=c->fd;while(used<JOAN_MAX_HEADERS){n=cread(c,h+used,JOAN_MAX_HEADERS-used);if(n<=0)goto fail;used+=(size_t)n;h[used]=0;end=(unsigned char*)strstr((char*)h,"\r\n\r\n");if(end)break;}if(!end)goto fail;head=(size_t)(end-h)+4;line=strtok_r((char*)h,"\r\n",&save);if(!line||sscanf(line,"%11s %511s",r->method,r->path)!=2)goto fail;q=strchr(r->path,'?');if(q){*q++=0;snprintf(r->query,sizeof(r->query),"%s",q);}header_copy(save,"Cookie",r->cookie,sizeof(r->cookie));header_copy(save,"X-CSRF-Token",r->csrf,sizeof(r->csrf));header_copy(save,"Content-Type",r->content_type,sizeof(r->content_type));header_copy(save,"Upgrade",r->upgrade,sizeof(r->upgrade));header_copy(save,"Sec-WebSocket-Key",r->ws_key,sizeof(r->ws_key));header_copy(save,"Sec-WebSocket-Protocol",r->ws_protocol,sizeof(r->ws_protocol));{char cl[32]={0};if(!header_copy(save,"Content-Length",cl,sizeof(cl)))r->content_length=(size_t)strtoul(cl,NULL,10);}if(r->content_length>JOAN_MAX_BODY)goto fail;if(r->content_length){size_t have=used-head,off=0;r->body=malloc(r->content_length+1);if(!r->body)goto fail;if(have>r->content_length)have=r->content_length;memcpy(r->body,end+4,have);off=have;while(off<r->content_length){n=cread(c,r->body+off,r->content_length-off);if(n<=0)goto fail;off+=(size_t)n;}r->body[r->content_length]=0;}free(h);return 0;fail:free(r->body);free(h);return-1;}

static int authorized(Conn*c,JoanRequest*r,JoanAuthz*a,int csrf){if(joan_auth_request(r,csrf,a))return json(c,401,"{\"error\":\"authentication required\"}"),-1;if(a->must_change&&strcmp(r->path,"/api/password")&&strcmp(r->path,"/api/logout")&&strcmp(r->path,"/api/status"))return json(c,428,"{\"error\":\"password change required\"}"),-1;return 0;}
static int helper_json(Conn*c,const char*op,const char*path,const char*id){unsigned char*out=NULL;size_t n=0;char esc[4096],body[4352];int rc=joan_run_helper(&G,op,path,id,&out,&n,15);if(rc){free(out);return json(c,502,"{\"error\":\"local integration operation failed\",\"code\":\"integration_failed\"}");}if(!strcmp(op,"ssh-list")){if(n>1800)n=1800;json_escape(out?out:(unsigned char*)"",n,esc,sizeof(esc));snprintf(body,sizeof(body),"{\"ok\":true,\"authorized_keys\":\"%s\"}",esc);}else snprintf(body,sizeof(body),"{\"ok\":true,\"id\":\"%s\"}",id?id:"");free(out);return json(c,200,body);}

static void *ptz_expiry_loop(void *unused)
{
    (void)unused;
    for(;;){int stop=0;struct timespec delay={0,250000000L};nanosleep(&delay,NULL);pthread_mutex_lock(&ptz_lock);if(ptz_lease[0]&&ptz_expires<=time(NULL)){ptz_lease[0]=0;ptz_expires=0;stop=1;}pthread_mutex_unlock(&ptz_lock);if(stop)joan_run_helper(&G,"ptz-stop",NULL,NULL,NULL,NULL,3);}
    return NULL;
}

static int static_file(Conn*c,const char*url){char path[600];unsigned char*data;size_t n;const char*type="application/octet-stream";if(strstr(url,".."))return json(c,404,"{\"error\":\"not found\"}");if(!strcmp(url,"/"))url="/index.html";if(snprintf(path,sizeof(path),"%s%s",G.web_dir,url)>=(int)sizeof(path)||joan_read_file(path,&data,&n,2*1024*1024))return json(c,404,"{\"error\":\"not found\"}");if(strstr(path,".html"))type="text/html; charset=utf-8";else if(strstr(path,".css"))type="text/css; charset=utf-8";else if(strstr(path,".js"))type="application/javascript; charset=utf-8";response(c,200,type,data,n,"Cache-Control: public, max-age=300\r\n");free(data);return 0;}

static int sock_write_all(int fd,const void*p,size_t n){const unsigned char*x=p;while(n){ssize_t z=send(fd,x,n,MSG_NOSIGNAL);if(z<=0)return-1;x+=z;n-=z;}return 0;}

static void *redirect_loop(void *arg)
{
    int listener=(int)(intptr_t)arg;
    for(;;){int fd=accept(listener,NULL,NULL);char req[2049],method[12],path[1024],reply[1600];ssize_t n;int z;static const char bad[]="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";static const char method_bad[]="HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";if(fd<0){if(errno==EINTR)continue;break;}n=recv(fd,req,sizeof(req)-1,0);if(n<=0){close(fd);continue;}req[n]=0;if(sscanf(req,"%11s %1023s",method,path)!=2||path[0]!='/'||strpbrk(path,"\r\n")){sock_write_all(fd,bad,sizeof(bad)-1);close(fd);continue;}if(strcmp(method,"GET")&&strcmp(method,"HEAD")){sock_write_all(fd,method_bad,sizeof(method_bad)-1);close(fd);continue;}z=snprintf(reply,sizeof(reply),"HTTP/1.1 308 Permanent Redirect\r\nLocation: https://%s%s\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",G.public_host,path);if(G.port!=443){char ported[1600];z=snprintf(ported,sizeof(ported),"HTTP/1.1 308 Permanent Redirect\r\nLocation: https://%s:%u%s\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",G.public_host,G.port,path);if(z>0&&(size_t)z<sizeof(ported))sock_write_all(fd,ported,(size_t)z);}else if(z>0&&(size_t)z<sizeof(reply))sock_write_all(fd,reply,(size_t)z);close(fd);}
    close(listener);return NULL;
}

static int redirect_start(void)
{
    int s,one=1;struct sockaddr_in a;pthread_t t;
    if(!G.redirect_port)return 0;
    if(strpbrk(G.public_host,"\r\n/"))return-1;
    s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return-1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)G.redirect_port);if(inet_pton(AF_INET,G.bind_addr,&a.sin_addr)!=1||bind(s,(struct sockaddr*)&a,sizeof(a))||listen(s,4)){close(s);return-1;}if(pthread_create(&t,NULL,redirect_loop,(void*)(intptr_t)s)){close(s);return-1;}pthread_detach(t);return 0;
}

static int ws_audio(Conn*c,JoanRequest*r){int s;struct sockaddr_un a;char h[1400];fd_set fds;unsigned char b[4096];size_t socket_len=strlen(G.audio_socket);if(strcasecmp(r->upgrade,"websocket")||!r->ws_key[0])return json(c,400,"{\"error\":\"websocket upgrade required\"}");if(socket_len==0||socket_len>=sizeof(a.sun_path))return json(c,502,"{\"error\":\"audio router path invalid\"}");s=socket(AF_UNIX,SOCK_STREAM,0);if(s<0)return json(c,502,"{\"error\":\"audio router unavailable\"}");memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;memcpy(a.sun_path,G.audio_socket,socket_len+1);if(connect(s,(struct sockaddr*)&a,sizeof(a))){close(s);return json(c,502,"{\"error\":\"audio router unavailable\"}");}int n=snprintf(h,sizeof(h),"GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Protocol: %s\r\n\r\n",r->path,r->ws_key,r->ws_protocol);if(n<=0||sock_write_all(s,h,(size_t)n)){close(s);return-1;}for(;;){FD_ZERO(&fds);FD_SET(c->fd,&fds);FD_SET(s,&fds);if(select((c->fd>s?c->fd:s)+1,&fds,NULL,NULL,NULL)<=0)break;if(FD_ISSET(c->fd,&fds)){ssize_t z=cread(c,b,sizeof(b));if(z<=0||sock_write_all(s,b,(size_t)z))break;}if(FD_ISSET(s,&fds)){ssize_t z=recv(s,b,sizeof(b),0);if(z<=0||wall(c,b,(size_t)z))break;}}close(s);return 0;}

static int route(Conn*c,JoanRequest*r){JoanAuthz a;char x[256],y[256],id[65],path[512],body[1536];
    /* Keep the implementation names private while exposing the documented,
     * versioned contract. The short aliases remain for the bundled early UI. */
    if(!strcmp(r->path,"/api/v1/session")){
        if(!strcmp(r->method,"POST"))snprintf(r->path,sizeof(r->path),"/api/login");
        else if(!strcmp(r->method,"DELETE"))snprintf(r->path,sizeof(r->path),"/api/logout");
        else snprintf(r->path,sizeof(r->path),"/api/session-info");
    }else if(!strcmp(r->path,"/api/v1/setup/password"))snprintf(r->path,sizeof(r->path),"/api/password");
    else if(!strcmp(r->path,"/api/v1/status"))snprintf(r->path,sizeof(r->path),"/api/status");
    else if(!strcmp(r->path,"/api/v1/network/wifi"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/wifi/stage":!strcmp(r->method,"PUT")?"/api/wifi/commit":"/api/wifi/rollback");
    else if(!strcmp(r->path,"/api/v1/streams"))snprintf(r->path,sizeof(r->path),"/api/streams");
    else if(!strcmp(r->path,"/api/v1/snapshot"))snprintf(r->path,sizeof(r->path),"/api/snapshot");
    else if(!strcmp(r->path,"/api/v1/ptz/lease"))snprintf(r->path,sizeof(r->path),"/api/ptz/lease");
    else if(!strcmp(r->path,"/api/v1/ptz/move"))snprintf(r->path,sizeof(r->path),"/api/ptz/move");
    else if(!strcmp(r->path,"/api/v1/ptz/stop"))snprintf(r->path,sizeof(r->path),"/api/ptz/stop");
    else if(!strcmp(r->path,"/api/v1/ssh/authorized-keys"))snprintf(r->path,sizeof(r->path),"/api/ssh/keys");
    else if(!strcmp(r->path,"/api/v1/update"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/firmware/stage":"/api/firmware/apply");
    else if(!strcmp(r->path,"/api/v1/audio/mic"))snprintf(r->path,sizeof(r->path),"/ws/audio/mic");
    else if(!strcmp(r->path,"/api/v1/audio/talk"))snprintf(r->path,sizeof(r->path),"/ws/audio/talk");
    if(!strcmp(r->path,"/api/v1/setup/status")){
        snprintf(body,sizeof(body),"{\"state\":\"%s\",\"setup_required\":%s,\"temporary_user\":\"admin\"}",joan_auth_setup_required(&G)?"setup-required":"ready",joan_auth_setup_required(&G)?"true":"false");
        return json(c,200,body);
    }
    if(!strcmp(r->path,"/api/health"))return json(c,200,"{\"ok\":true}");
    if(!strcmp(r->path,"/api/login")&&!strcmp(r->method,"POST")){int rc;if(json_field(r->body,r->content_length,"username",x,sizeof(x))||json_field(r->body,r->content_length,"password",y,sizeof(y)))return json(c,400,"{\"error\":\"invalid JSON\"}");rc=joan_auth_login(&G,r->remote,x,y,&a);memset(y,0,sizeof(y));if(rc==-2)return json(c,429,"{\"error\":\"try later\"}");if(rc)return json(c,401,"{\"error\":\"invalid credentials\"}");snprintf(body,sizeof(body),"{\"ok\":true,\"csrf\":\"%s\",\"must_change\":%s}",a.csrf,a.must_change?"true":"false");snprintf(x,sizeof(x),"Set-Cookie: joan_session=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",a.token,G.plain_http?"":"; Secure");return response(c,200,"application/json",body,strlen(body),x);}
    if(!strncmp(r->path,"/ws/audio/",10)){if(authorized(c,r,&a,0))return 0;return ws_audio(c,r);}
    if(authorized(c,r,&a,strcmp(r->method,"GET")&&strcmp(r->method,"HEAD")))return 0;
    if(!strcmp(r->path,"/api/status")){snprintf(body,sizeof(body),"{\"version\":\"%s\",\"state\":\"%s\",\"must_change\":%s,\"features\":{\"https_identity\":%s,\"firewall\":false,\"rtsp\":true,\"ptz\":\"helper-gated\",\"audio\":\"socket-gated\",\"ssh\":\"helper-gated\"},\"mqtt_bridge\":\"%s\",\"https_cloud_bridge\":{\"supported\":false,\"blocker\":\"OEM trust/pinning contract not recovered\"}}",JOAN_VERSION,a.must_change?"setup-required":"ready",a.must_change?"true":"false",G.plain_http?"false":"true",joan_mqtt_bridge_status());return json(c,200,body);}
    if(!strcmp(r->path,"/api/session-info"))return json(c,200,"{\"authenticated\":true}");
    if(!strcmp(r->path,"/api/logout")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"DELETE"))){joan_auth_logout(&a);return response(c,204,"application/json","",0,"Set-Cookie: joan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");}
    if(!strcmp(r->path,"/api/password")&&!strcmp(r->method,"POST")){
        char escaped[768],command[1024];int rtsp_ok=0;
        if(json_field(r->body,r->content_length,"old_password",x,sizeof(x))||
           json_field(r->body,r->content_length,"new_password",y,sizeof(y)))
            return json(c,400,"{\"error\":\"invalid JSON\"}");
        if(joan_auth_change_password(&G,&a,x,y)){
            memset(x,0,sizeof(x));memset(y,0,sizeof(y));
            return json(c,400,"{\"error\":\"password rejected\"}");
        }
        json_escape((const unsigned char*)y,strlen(y),escaped,sizeof(escaped));
        snprintf(command,sizeof(command),"{\"cmd\":66516,\"cmd_type\":\"request\",\"sessionid\":0,\"value\":0,\"security_enhanced_switch\":1}");
        if(!joan_mqtt_bridge_publish("local/dp/security",command,strlen(command))){
            snprintf(command,sizeof(command),"{\"cmd\":66517,\"cmd_type\":\"request\",\"sessionid\":0,\"value\":0,\"security_password\":\"%s\"}",escaped);
            rtsp_ok=!joan_mqtt_bridge_publish("local/dp/security",command,strlen(command));
        }
        memset(x,0,sizeof(x));memset(y,0,sizeof(y));memset(escaped,0,sizeof(escaped));memset(command,0,sizeof(command));
        return json(c,200,rtsp_ok?"{\"ok\":true,\"login_required\":true,\"rtsp_updated\":true}":"{\"ok\":true,\"login_required\":true,\"rtsp_updated\":false}");
    }
    if(!strcmp(r->path,"/api/streams")&&!strcmp(r->method,"GET"))return json(c,200,"{\"streams\":[{\"id\":\"main\",\"rtsp\":\"rtsp://camera/live/ch0\",\"width\":2304,\"height\":1296},{\"id\":\"sub\",\"rtsp\":\"rtsp://camera/live/ch1\",\"width\":640,\"height\":360}],\"snapshot\":\"/api/snapshot\"}");
    if(!strcmp(r->path,"/api/snapshot")&&!strcmp(r->method,"GET")){unsigned char*out=NULL;size_t n=0;if(joan_run_helper(&G,"snapshot",NULL,NULL,&out,&n,5)||n<4||out[0]!=0xff||out[1]!=0xd8){free(out);return json(c,502,"{\"error\":\"snapshot unavailable\"}");}response(c,200,"image/jpeg",out,n,NULL);free(out);return 0;}
    if(!strcmp(r->path,"/api/wifi/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"configuration required\"}");if(joan_stage_blob(&G,"wifi",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"wifi-stage",path,id);}
    if(!strcmp(r->path,"/api/wifi/commit")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-commit",NULL,id);}
    if(!strcmp(r->path,"/api/wifi/rollback")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"DELETE"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-rollback",NULL,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"GET"))return helper_json(c,"ssh-list",NULL,NULL);
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"POST")){if(!r->body||r->content_length>16384)return json(c,400,"{\"error\":\"invalid key\"}");if(joan_stage_blob(&G,"ssh-key",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"ssh-add",path,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"DELETE")){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"ssh-delete",NULL,id);}
    if(!strcmp(r->path,"/api/ptz/lease")&&!strcmp(r->method,"POST")){unsigned char rnd[32];pthread_mutex_lock(&ptz_lock);if(ptz_lease[0]&&ptz_expires>time(NULL)){pthread_mutex_unlock(&ptz_lock);return json(c,409,"{\"error\":\"PTZ busy\"}");}joan_random(rnd,sizeof(rnd));joan_hex(rnd,sizeof(rnd),ptz_lease);ptz_expires=time(NULL)+2;snprintf(body,sizeof(body),"{\"lease\":\"%s\",\"expires_in\":2}",ptz_lease);pthread_mutex_unlock(&ptz_lock);return json(c,201,body);}
    if(!strcmp(r->path,"/api/ptz/move")&&!strcmp(r->method,"POST")){if(json_field(r->body,r->content_length,"lease",x,sizeof(x))||json_field(r->body,r->content_length,"command",y,sizeof(y)))return json(c,400,"{\"error\":\"lease and command required\"}");if(strcmp(y,"up")&&strcmp(y,"down")&&strcmp(y,"left")&&strcmp(y,"right"))return json(c,400,"{\"error\":\"invalid PTZ command\"}");pthread_mutex_lock(&ptz_lock);if(ptz_expires<time(NULL)||strcmp(x,ptz_lease)){pthread_mutex_unlock(&ptz_lock);return json(c,403,"{\"error\":\"invalid lease\"}");}ptz_expires=time(NULL)+2;pthread_mutex_unlock(&ptz_lock);joan_stage_blob(&G,"ptz",y,strlen(y),id,path);return helper_json(c,"ptz-move",path,id);}
    if(!strcmp(r->path,"/api/ptz/release")&&!strcmp(r->method,"POST")){int stop=0;if(json_field(r->body,r->content_length,"lease",x,sizeof(x)))return json(c,400,"{\"error\":\"lease required\"}");pthread_mutex_lock(&ptz_lock);if(!strcmp(x,ptz_lease)){ptz_lease[0]=0;ptz_expires=0;stop=1;}pthread_mutex_unlock(&ptz_lock);return stop?helper_json(c,"ptz-stop",NULL,NULL):json(c,403,"{\"error\":\"invalid lease\"}");}
    if(!strcmp(r->path,"/api/ptz/stop")&&!strcmp(r->method,"POST")){if(json_field(r->body,r->content_length,"lease",x,sizeof(x)))return json(c,400,"{\"error\":\"lease required\"}");pthread_mutex_lock(&ptz_lock);if(strcmp(x,ptz_lease)){pthread_mutex_unlock(&ptz_lock);return json(c,403,"{\"error\":\"invalid lease\"}");}ptz_lease[0]=0;ptz_expires=0;pthread_mutex_unlock(&ptz_lock);return helper_json(c,"ptz-stop",NULL,NULL);}
    if(!strcmp(r->path,"/api/firmware/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"image required\"}");if(joan_stage_blob(&G,"firmware",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"firmware-verify",path,id);}
    if(!strcmp(r->path,"/api/firmware/apply")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"firmware-apply",NULL,id);}
    if(!strcmp(r->path,"/api/cloud/dp")&&!strcmp(r->method,"POST")){if(json_field(r->body,r->content_length,"topic",x,sizeof(x))||json_field(r->body,r->content_length,"payload",y,sizeof(y))||joan_mqtt_bridge_publish(x,y,strlen(y)))return json(c,400,"{\"error\":\"DP rejected or OEM client not subscribed\"}");return json(c,200,"{\"ok\":true}");}
    return json(c,404,"{\"error\":\"not found\"}");
}

static void serve(Conn*c,const char*remote){JoanRequest r;if(parse_request(c,&r)){json(c,400,"{\"error\":\"bad request\"}");return;}snprintf(r.remote,sizeof(r.remote),"%s",remote);if(!strncmp(r.path,"/api/",5)||!strncmp(r.path,"/ws/",4))route(c,&r);else static_file(c,r.path);free(r.body);}

typedef struct {
    int fd;
    char remote[64];
    int plain_http;
#ifndef JOAN_NO_TLS
    mbedtls_ssl_config *tls_config;
#endif
} Worker;

static void *serve_worker(void *argument)
{
    Worker *worker=argument;Conn c;struct timeval io_timeout={10,0};int ready=1;
    memset(&c,0,sizeof(c));c.fd=worker->fd;
    setsockopt(c.fd,SOL_SOCKET,SO_RCVTIMEO,&io_timeout,sizeof(io_timeout));
    setsockopt(c.fd,SOL_SOCKET,SO_SNDTIMEO,&io_timeout,sizeof(io_timeout));
#ifndef JOAN_NO_TLS
    mbedtls_ssl_context ssl;
    if(!worker->plain_http){int rc;mbedtls_ssl_init(&ssl);if(mbedtls_ssl_setup(&ssl,worker->tls_config))ready=0;if(ready){mbedtls_ssl_set_bio(&ssl,&c.fd,mbedtls_net_send,mbedtls_net_recv,NULL);while((rc=mbedtls_ssl_handshake(&ssl))!=0)if(rc!=MBEDTLS_ERR_SSL_WANT_READ&&rc!=MBEDTLS_ERR_SSL_WANT_WRITE)break;if(rc){mbedtls_ssl_free(&ssl);ready=0;}else c.ssl=&ssl;}}
#endif
    if(ready)serve(&c,worker->remote);
#ifndef JOAN_NO_TLS
    if(c.ssl){mbedtls_ssl_close_notify(c.ssl);mbedtls_ssl_free(c.ssl);}
#endif
    close(c.fd);free(worker);return NULL;
}

int joan_server_run(const JoanConfig*cfg){int s,one=1;struct sockaddr_in a;pthread_t ptz_thread;G=*cfg;
#ifndef JOAN_NO_TLS
    mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context drbg;mbedtls_ssl_config sc;mbedtls_x509_crt cert;mbedtls_pk_context key;char cp[512],kp[512];
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&drbg);mbedtls_ssl_config_init(&sc);mbedtls_x509_crt_init(&cert);mbedtls_pk_init(&key);
    if(!cfg->plain_http){snprintf(cp,sizeof(cp),"%s/tls-cert.pem",cfg->state_dir);snprintf(kp,sizeof(kp),"%s/tls-key.pem",cfg->state_dir);if(mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,(unsigned char*)"joan-httpd",11)||mbedtls_x509_crt_parse_file(&cert,cp)||mbedtls_pk_parse_keyfile(&key,kp,NULL)||mbedtls_ssl_config_defaults(&sc,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)||mbedtls_ssl_conf_own_cert(&sc,&cert,&key))return-1;mbedtls_ssl_conf_rng(&sc,mbedtls_ctr_drbg_random,&drbg);}
#else
    if(!cfg->plain_http){fprintf(stderr,"HTTPS requested but binary was built JOAN_NO_TLS; refusing\n");return-1;}
#endif
    if(pthread_create(&ptz_thread,NULL,ptz_expiry_loop,NULL))return-1;
    pthread_detach(ptz_thread);
    if(!cfg->plain_http&&redirect_start())return-1;
    s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return-1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)cfg->port);if(inet_pton(AF_INET,cfg->bind_addr,&a.sin_addr)!=1||bind(s,(struct sockaddr*)&a,sizeof(a))||listen(s,8)){close(s);return-1;}
    for(;;){struct sockaddr_in peer;socklen_t pl=sizeof(peer);int fd=accept(s,(struct sockaddr*)&peer,&pl);Worker*w;pthread_t t;if(fd<0){if(errno==EINTR)continue;break;}w=calloc(1,sizeof(*w));if(!w){close(fd);continue;}w->fd=fd;w->plain_http=cfg->plain_http;inet_ntop(AF_INET,&peer.sin_addr,w->remote,sizeof(w->remote));
#ifndef JOAN_NO_TLS
        w->tls_config=&sc;
#endif
        if(pthread_create(&t,NULL,serve_worker,w)){close(fd);free(w);continue;}pthread_detach(t);
    }close(s);return 0;
}
