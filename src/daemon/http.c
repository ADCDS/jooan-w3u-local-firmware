#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"
#include "../audio/audio_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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
#ifndef JOAN_NO_TLS
static pthread_mutex_t tls_rng_lock=PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_mutex_t worker_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_available=PTHREAD_COND_INITIALIZER;
static unsigned worker_count;
static int server_fd=-1;
static int server_stopping;
#define MAX_WORKERS 8u
#ifndef JOAN_NO_TLS
static int locked_rng(void*ctx,unsigned char*out,size_t len){int rc;pthread_mutex_lock(&tls_rng_lock);rc=mbedtls_ctr_drbg_random(ctx,out,len);pthread_mutex_unlock(&tls_rng_lock);return rc;}
#endif
static char ptz_lease[65]; static time_t ptz_expires;

#ifndef JOAN_NO_TLS
static int tls_wait(int fd,short events){struct pollfd p;int r;p.fd=fd;p.events=events;p.revents=0;do r=poll(&p,1,10000);while(r<0&&errno==EINTR);return r>0&&(p.revents&events)?0:-1;}
#endif
static ssize_t cread(Conn*c,void*b,size_t n){
#ifndef JOAN_NO_TLS
    if(c->ssl){int r;for(;;){r=mbedtls_ssl_read(c->ssl,b,n);if(r>0)return r;if(r==MBEDTLS_ERR_SSL_WANT_READ){if(tls_wait(c->fd,POLLIN))return-1;continue;}if(r==MBEDTLS_ERR_SSL_WANT_WRITE){if(tls_wait(c->fd,POLLOUT))return-1;continue;}return-1;}}
#endif
    return recv(c->fd,b,n,0);
}
static ssize_t cwrite(Conn*c,const void*b,size_t n){
#ifndef JOAN_NO_TLS
    if(c->ssl){int r;for(;;){r=mbedtls_ssl_write(c->ssl,b,n);if(r>0)return r;if(r==MBEDTLS_ERR_SSL_WANT_READ){if(tls_wait(c->fd,POLLIN))return-1;continue;}if(r==MBEDTLS_ERR_SSL_WANT_WRITE){if(tls_wait(c->fd,POLLOUT))return-1;continue;}return-1;}}
#endif
    return send(c->fd,b,n,MSG_NOSIGNAL);
}
static int wall(Conn*c,const void*b,size_t n){const unsigned char*p=b;while(n){ssize_t z=cwrite(c,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return-1;p+=z;n-=z;}return 0;}
static const char *reason(int s){switch(s){case 200:return"OK";case 201:return"Created";case 202:return"Accepted";case 204:return"No Content";case 400:return"Bad Request";case 401:return"Unauthorized";case 403:return"Forbidden";case 404:return"Not Found";case 409:return"Conflict";case 413:return"Payload Too Large";case 415:return"Unsupported Media Type";case 428:return"Precondition Required";case 429:return"Too Many Requests";case 500:return"Internal Server Error";case 501:return"Not Implemented";case 502:return"Bad Gateway";case 503:return"Service Unavailable";default:return"Error";}}
static int response(Conn*c,int status,const char*type,const void*body,size_t len,const char*extra){char h[2300];int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; connect-src 'self' wss:; img-src 'self' data:; media-src 'self' blob:; style-src 'self'; script-src 'self'\r\nCache-Control: no-store\r\n%s%s\r\n",status,reason(status),type?type:"application/octet-stream",(unsigned long)len,G.plain_http?"":"Strict-Transport-Security: max-age=31536000\r\n",extra?extra:"");return n<=0||(size_t)n>=sizeof(h)||wall(c,h,(size_t)n)||wall(c,body,len)?-1:0;}
static int json(Conn*c,int status,const char*s){return response(c,status,"application/json; charset=utf-8",s,strlen(s),NULL);}
static int error_json(Conn*c,int status,const char*code,const char*message){char b[512];snprintf(b,sizeof(b),"{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",code,message);return json(c,status,b);}
static void json_escape(const unsigned char*in,size_t n,char*out,size_t cap){size_t i,o=0;for(i=0;i<n&&o+7<cap;i++){unsigned char x=in[i];if(x=='"'||x=='\\'){out[o++]='\\';out[o++]=x;}else if(x>=32&&x<127)out[o++]=x;else{o+=snprintf(out+o,cap-o,"\\u%04x",x);} }out[o]=0;}

static int json_field(const unsigned char*body,size_t len,const char*key,char*out,size_t cap){char needle[96];const char*p,*e;size_t n;if(!body||snprintf(needle,sizeof(needle),"\"%s\"",key)>=(int)sizeof(needle))return-1;p=strstr((const char*)body,needle);if(!p||(size_t)(p-(const char*)body)>=len)return-1;p+=strlen(needle);while(*p==' '||*p=='\t')p++;if(*p++!=':')return-1;while(*p==' '||*p=='\t')p++;if(*p++!='"')return-1;e=p;while(*e&&*e!='"'&&(size_t)(e-(const char*)body)<len){if(*e=='\\')return-1;e++;}n=(size_t)(e-p);if(*e!='"'||n+1>cap)return-1;memcpy(out,p,n);out[n]=0;return 0;}
static int json_uint(const unsigned char*body,size_t len,const char*key,unsigned*out){char needle[96],*ep;const char*p;unsigned long v;if(!body||!out||snprintf(needle,sizeof(needle),"\"%s\"",key)>=(int)sizeof(needle))return-1;p=strstr((const char*)body,needle);if(!p||(size_t)(p-(const char*)body)>=len)return-1;p+=strlen(needle);while(*p==' '||*p=='\t')p++;if(*p++!=':')return-1;while(*p==' '||*p=='\t')p++;v=strtoul(p,&ep,10);if(ep==p||v>0xffffffffUL)return-1;while(*ep==' '||*ep=='\t')ep++;if(*ep!=','&&*ep!='}')return-1;*out=(unsigned)v;return 0;}
static int header_copy(const char*headers,const char*name,char*out,size_t cap){size_t nl=strlen(name);const char*p=headers;while(p&&*p){const char*e=strstr(p,"\r\n");size_t line=e?(size_t)(e-p):strlen(p);if(line>nl&&p[nl]==':'&&!strncasecmp(p,name,nl)){const char*v=p+nl+1;size_t n;while(v<p+line&&(*v==' '||*v=='\t'))v++;n=(size_t)((p+line)-v);if(n>=cap)n=cap-1;memcpy(out,v,n);out[n]=0;return 0;}if(!e)break;p=e+2;}return-1;}

static int parse_request(Conn*c,JoanRequest*r){unsigned char*h=malloc(JOAN_MAX_HEADERS+1),*end=NULL;size_t used=0,head,body_limit;char*line_end,*headers,*q;ssize_t n;int result=-1;if(!h)return-1;memset(r,0,sizeof(*r));r->fd=c->fd;while(used<JOAN_MAX_HEADERS){n=cread(c,h+used,JOAN_MAX_HEADERS-used);if(n<=0){if(!used)result=-3;goto fail;}used+=(size_t)n;h[used]=0;end=(unsigned char*)strstr((char*)h,"\r\n\r\n");if(end)break;}if(!end)goto fail;head=(size_t)(end-h)+4;line_end=strstr((char*)h,"\r\n");if(!line_end)goto fail;*line_end=0;headers=line_end+2;if(sscanf((char*)h,"%11s %511s",r->method,r->path)!=2)goto fail;q=strchr(r->path,'?');if(q){*q++=0;snprintf(r->query,sizeof(r->query),"%s",q);}*end=0;header_copy(headers,"Host",r->host,sizeof(r->host));header_copy(headers,"Cookie",r->cookie,sizeof(r->cookie));header_copy(headers,"X-CSRF-Token",r->csrf,sizeof(r->csrf));header_copy(headers,"Content-Type",r->content_type,sizeof(r->content_type));header_copy(headers,"Upgrade",r->upgrade,sizeof(r->upgrade));header_copy(headers,"Sec-WebSocket-Key",r->ws_key,sizeof(r->ws_key));header_copy(headers,"Sec-WebSocket-Protocol",r->ws_protocol,sizeof(r->ws_protocol));header_copy(headers,"Origin",r->origin,sizeof(r->origin));header_copy(headers,"Transfer-Encoding",r->transfer_encoding,sizeof(r->transfer_encoding));if(!r->host[0]||r->transfer_encoding[0])goto fail;{char cl[32]={0};if(!header_copy(headers,"Content-Length",cl,sizeof(cl))){char*ep=NULL;unsigned long z=strtoul(cl,&ep,10);if(!ep||*ep)goto fail;r->content_length=(size_t)z;}}body_limit=!strcmp(r->method,"POST")&&!strcmp(r->path,"/api/v1/update")?JOAN_MAX_UPDATE_BODY:JOAN_MAX_BODY;if(r->content_length>body_limit){result=-2;goto fail;}if(r->content_length){size_t have=used-head,off=0;r->body=malloc(r->content_length+1);if(!r->body)goto fail;if(have>r->content_length)have=r->content_length;memcpy(r->body,end+4,have);off=have;while(off<r->content_length){n=cread(c,r->body+off,r->content_length-off);if(n<=0)goto fail;off+=(size_t)n;}r->body[r->content_length]=0;}free(h);return 0;fail:free(r->body);free(h);return result;}

static int host_ok(const char*host){char name[256],label[64],*colon;struct in_addr a4;size_t n;if(!host||(n=strlen(host))==0||n>=sizeof(name)||strpbrk(host,"/\\@\r\n"))return 0;snprintf(name,sizeof(name),"%s",host);if(name[0]=='['){char*end=strchr(name,']');struct in6_addr a6;if(!end)return 0;*end=0;if(inet_pton(AF_INET6,name+1,&a6)!=1)return 0;return IN6_IS_ADDR_LOOPBACK(&a6)||(a6.s6_addr[0]&0xfe)==0xfc;}colon=strrchr(name,':');if(colon)*colon=0;if(inet_pton(AF_INET,name,&a4)==1){uint32_t a=ntohl(a4.s_addr);return(a>>24)==127||(a>>24)==10||(a>>20)==0xac1||(a>>16)==0xc0a8;}joan_mdns_get_hostname(&G,label);{char wanted[80];snprintf(wanted,sizeof(wanted),"%s.local",label);return!strcasecmp(name,wanted);}}
static int origin_ok(const JoanRequest*r,int required){char expected[520];if(!r->origin[0])return required?0:1;if(!host_ok(r->host))return 0;snprintf(expected,sizeof(expected),"%s://%s",G.plain_http?"http":"https",r->host);return !strcmp(r->origin,expected);}
static int authorized(Conn*c,JoanRequest*r,JoanAuthz*a,int csrf){if(!origin_ok(r,0))return error_json(c,403,"origin_rejected","request origin is not this camera"),-1;if(joan_auth_request(r,csrf,a))return error_json(c,401,"authentication_required","authentication required"),-1;return 0;}
static int helper_json(Conn*c,const char*op,const char*path,const char*id){unsigned char*out=NULL;size_t n=0;char esc[4096],body[4352];unsigned timeout=!strcmp(op,"firmware-verify")?60u:15u;int rc=joan_run_helper(&G,op,path,id,&out,&n,timeout);if(rc){free(out);return error_json(c,502,"integration_failed","local integration operation failed");}if(!strcmp(op,"ssh-list")){if(n>1800)n=1800;json_escape(out?out:(unsigned char*)"",n,esc,sizeof(esc));snprintf(body,sizeof(body),"{\"ok\":true,\"authorized_keys\":\"%s\"}",esc);}else snprintf(body,sizeof(body),"{\"ok\":true,\"id\":\"%s\"}",id?id:"");free(out);return json(c,200,body);}
static int mqtt_accepted(Conn*c,unsigned command,const char*payload){char operation[65],body[192];int rc=joan_mqtt_request(command,payload,operation);if(rc==-2)return error_json(c,409,"operation_pending","an operation with this command is already pending");if(rc)return error_json(c,503,"mqtt_unavailable","OEM command channel is unavailable");snprintf(body,sizeof(body),"{\"accepted\":true,\"operation\":\"%s\",\"status\":\"/api/v1/operations/%s\"}",operation,operation);return json(c,202,body);}
static int private_routes(const char*input,char*out,size_t cap){char copy[1024],*save=NULL,*item;size_t used=0;if(!input||strlen(input)>=sizeof(copy))return-1;snprintf(copy,sizeof(copy),"%s",input);for(item=strtok_r(copy,", ", &save);item;item=strtok_r(NULL,", ",&save)){char*slash=strchr(item,'/');unsigned prefix;struct in_addr a4;struct in6_addr a6;int ok=0;if(!slash)return-1;*slash++=0;prefix=(unsigned)strtoul(slash,NULL,10);if(inet_pton(AF_INET,item,&a4)==1&&prefix>=8&&prefix<=32){uint32_t a=ntohl(a4.s_addr);ok=((a>>24)==10)||((a>>20)==0xac1)||((a>>16)==0xc0a8);}else if(inet_pton(AF_INET6,item,&a6)==1&&prefix>=7&&prefix<=128)ok=(a6.s6_addr[0]&0xfe)==0xfc;if(!ok)return-1;{int n=snprintf(out+used,cap-used,"%s/%u\n",item,prefix);if(n<=0||(size_t)n>=cap-used)return-1;used+=(size_t)n;}}return used?0:-1;}
static unsigned release_sequence(void){unsigned char*raw=NULL;size_t i,n=0;uint64_t value=0;if(!G.release_sequence_path[0]||joan_read_file(G.release_sequence_path,&raw,&n,32)||!n)goto done;if(raw[n-1]=='\n')n--;if(!n)goto done;for(i=0;i<n;i++){if(raw[i]<'0'||raw[i]>'9'){value=0;goto done;}value=value*10u+(unsigned)(raw[i]-'0');if(value>0xffffffffu){value=0;goto done;}}done:free(raw);return(unsigned)value;}

static void *ptz_expiry_loop(void *unused)
{
    (void)unused;
    for(;;){int stop=0;struct timespec delay={0,250000000L};nanosleep(&delay,NULL);pthread_mutex_lock(&ptz_lock);if(ptz_lease[0]&&ptz_expires<=time(NULL)){ptz_lease[0]=0;ptz_expires=0;stop=1;}pthread_mutex_unlock(&ptz_lock);if(stop)joan_run_helper(&G,"ptz-stop",NULL,NULL,NULL,NULL,3);}
    return NULL;
}

static int static_file(Conn*c,const char*url){char path[600];unsigned char*data;size_t n;const char*type="application/octet-stream";if(strstr(url,".."))return error_json(c,404,"asset_not_found","asset not found");if(!strcmp(url,"/"))url="/index.html";if(snprintf(path,sizeof(path),"%s%s",G.web_dir,url)>=(int)sizeof(path)||joan_read_file(path,&data,&n,2*1024*1024))return error_json(c,404,"asset_not_found","asset not found");if(strstr(path,".html"))type="text/html; charset=utf-8";else if(strstr(path,".css"))type="text/css; charset=utf-8";else if(strstr(path,".js"))type="application/javascript; charset=utf-8";else if(strstr(path,".webmanifest"))type="application/manifest+json";else if(strstr(path,".svg"))type="image/svg+xml";response(c,200,type,data,n,"Cache-Control: public, max-age=300\r\n");free(data);return 0;}

static int sock_write_all(int fd,const void*p,size_t n){const unsigned char*x=p;while(n){ssize_t z=send(fd,x,n,MSG_NOSIGNAL);if(z<=0)return-1;x+=z;n-=z;}return 0;}

static void *redirect_loop(void *arg)
{
    int listener=(int)(intptr_t)arg;
    for(;;){int fd=accept(listener,NULL,NULL);char req[2049],method[12],path[1024],reply[1600],label[64],public_host[80];ssize_t n;int z;static const char bad[]="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";static const char method_bad[]="HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";if(fd<0){if(errno==EINTR)continue;break;}n=recv(fd,req,sizeof(req)-1,0);if(n<=0){close(fd);continue;}req[n]=0;if(sscanf(req,"%11s %1023s",method,path)!=2||path[0]!='/'||strpbrk(path,"\r\n")){sock_write_all(fd,bad,sizeof(bad)-1);close(fd);continue;}if(strcmp(method,"GET")&&strcmp(method,"HEAD")){sock_write_all(fd,method_bad,sizeof(method_bad)-1);close(fd);continue;}joan_mdns_get_hostname(&G,label);snprintf(public_host,sizeof(public_host),"%s.local",label);z=snprintf(reply,sizeof(reply),"HTTP/1.1 308 Permanent Redirect\r\nLocation: https://%s%s\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",public_host,path);if(G.port!=443){char ported[1600];z=snprintf(ported,sizeof(ported),"HTTP/1.1 308 Permanent Redirect\r\nLocation: https://%s:%u%s\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",public_host,G.port,path);if(z>0&&(size_t)z<sizeof(ported))sock_write_all(fd,ported,(size_t)z);}else if(z>0&&(size_t)z<sizeof(reply))sock_write_all(fd,reply,(size_t)z);close(fd);}
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
    else if(!strncmp(r->path,"/api/v1/video/",14)){char video_path[sizeof(r->path)];snprintf(video_path,sizeof(video_path),"/api/video/%s",r->path+14);snprintf(r->path,sizeof(r->path),"%s",video_path);}
    else if(!strcmp(r->path,"/api/v1/status"))snprintf(r->path,sizeof(r->path),"/api/status");
    else if(!strcmp(r->path,"/api/v1/network/mdns"))snprintf(r->path,sizeof(r->path),"/api/mdns");
    else if(!strcmp(r->path,"/api/v1/network/routes"))snprintf(r->path,sizeof(r->path),"/api/routes");
    else if(!strcmp(r->path,"/api/v1/time"))snprintf(r->path,sizeof(r->path),"/api/time");
    else if(!strcmp(r->path,"/api/v1/timezone"))snprintf(r->path,sizeof(r->path),"/api/timezone");
    else if(!strcmp(r->path,"/api/v1/network/wifi"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/wifi/stage":!strcmp(r->method,"PUT")?"/api/wifi/commit":"/api/wifi/rollback");
    else if(!strcmp(r->path,"/api/v1/streams"))snprintf(r->path,sizeof(r->path),"/api/streams");
    else if(!strcmp(r->path,"/api/v1/snapshot"))snprintf(r->path,sizeof(r->path),"/api/snapshot");
    else if(!strcmp(r->path,"/api/v1/ptz/lease"))snprintf(r->path,sizeof(r->path),"/api/ptz/lease");
    else if(!strcmp(r->path,"/api/v1/ptz/move"))snprintf(r->path,sizeof(r->path),"/api/ptz/move");
    else if(!strcmp(r->path,"/api/v1/ptz/stop"))snprintf(r->path,sizeof(r->path),"/api/ptz/stop");
    else if(!strcmp(r->path,"/api/v1/ptz/home"))snprintf(r->path,sizeof(r->path),"/api/ptz/home");
    else if(!strcmp(r->path,"/api/v1/ptz/presets"))snprintf(r->path,sizeof(r->path),"/api/ptz/presets");
    else if(!strcmp(r->path,"/api/v1/ssh/authorized-keys"))snprintf(r->path,sizeof(r->path),"/api/ssh/keys");
    else if(!strcmp(r->path,"/api/v1/update"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/firmware/stage":"/api/firmware/apply");
    else if(!strcmp(r->path,"/api/v1/audio/mic"))snprintf(r->path,sizeof(r->path),"/ws/audio/mic");
    else if(!strcmp(r->path,"/api/v1/audio/talk"))snprintf(r->path,sizeof(r->path),"/ws/audio/talk");
    else if(!strncmp(r->path,"/api/v1/operations/",19)){char operation_path[sizeof(r->path)];snprintf(operation_path,sizeof(operation_path),"/api/operations/%s",r->path+19);snprintf(r->path,sizeof(r->path),"%s",operation_path);}
    if(!strcmp(r->path,"/api/v1/setup/status")){
        snprintf(body,sizeof(body),"{\"state\":\"ready\",\"setup_required\":false,\"default_password_warning\":%s,\"temporary_user\":\"admin\",\"release_version\":\"%s\",\"release_sequence\":%u}",joan_auth_setup_required(&G)?"true":"false",JOAN_VERSION,release_sequence());
        return json(c,200,body);
    }
    if(!strcmp(r->path,"/api/health"))return json(c,200,"{\"ok\":true}");
    if(!strcmp(r->path,"/api/login")&&!strcmp(r->method,"POST")){int rc;if(json_field(r->body,r->content_length,"username",x,sizeof(x))||json_field(r->body,r->content_length,"password",y,sizeof(y)))return error_json(c,400,"invalid_json","username and password are required");rc=joan_auth_login(&G,r->remote,x,y,&a);memset(y,0,sizeof(y));if(rc==-2)return error_json(c,429,"rate_limited","try later");if(rc)return error_json(c,401,"invalid_credentials","invalid credentials");snprintf(body,sizeof(body),"{\"ok\":true,\"csrf\":\"%s\",\"default_password_warning\":%s}",a.csrf,a.must_change?"true":"false");snprintf(x,sizeof(x),"Set-Cookie: joan_session=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",a.token,G.plain_http?"":"; Secure");return response(c,200,"application/json",body,strlen(body),x);}
    if(!strncmp(r->path,"/ws/audio/",10)){if(!origin_ok(r,1)){error_json(c,403,"origin_rejected","websocket origin is required");return 0;}if(authorized(c,r,&a,0))return 0;if(jooan_audio_ws_protocol_validate(r->ws_protocol,a.csrf)){error_json(c,403,"audio_token_rejected","audio CSRF subprotocol is invalid");return 0;}return ws_audio(c,r);}
    if(authorized(c,r,&a,strcmp(r->method,"GET")&&strcmp(r->method,"HEAD")))return 0;
    if(!strcmp(r->path,"/api/status")){unsigned char*routes=NULL;size_t routes_len=0;const char*routes_json="{\"cidrs\":[]}";if(!joan_run_helper(&G,"routes-list",NULL,NULL,&routes,&routes_len,2)&&routes_len&&routes_len<512&&routes[0]=='{')routes_json=(char*)routes;snprintf(body,sizeof(body),"{\"version\":\"%s\",\"state\":\"ready\",\"local_only\":true,\"active_routes\":%s,\"default_password_warning\":%s,\"ssh_password_sync\":%s,\"rtsp_password_sync\":%s,\"features\":{\"https_identity\":%s,\"rtsp\":true,\"fmp4_main\":\"%s\",\"fmp4_sub\":\"%s\",\"ptz\":\"helper-gated\",\"audio\":\"socket-gated\",\"ssh\":\"password-synchronized\",\"mdns\":\"%s\"},\"mqtt_bridge\":\"%s\",\"https_cloud_bridge\":{\"supported\":false,\"blocker\":\"OEM trust/pinning contract not recovered\"}}",JOAN_VERSION,routes_json,a.must_change?"true":"false",joan_auth_ssh_synchronized(&G)?"true":"false",joan_auth_rtsp_synchronized(&G)?"true":"false",G.plain_http?"false":"true",joan_fmp4_status("main"),joan_fmp4_status("sub"),joan_mdns_status(),joan_mqtt_bridge_status());free(routes);return json(c,200,body);}
    if(!strcmp(r->path,"/api/time")&&!strcmp(r->method,"PUT")){char epoch[24];char*ep=NULL;long long v;if(json_field(r->body,r->content_length,"epoch",epoch,sizeof(epoch)))return error_json(c,400,"invalid_json","epoch (UTC seconds) is required");v=strtoll(epoch,&ep,10);if(!ep||*ep||v<1577836800LL||v>4102444800LL)return error_json(c,400,"invalid_time","epoch out of range");if(joan_run_helper(&G,"time-set",NULL,epoch,NULL,NULL,10))return error_json(c,502,"time_set_failed","could not set the camera clock");joan_auth_restamp(&a);snprintf(body,sizeof(body),"{\"ok\":true,\"epoch\":%lld}",(long long)time(NULL));return json(c,200,body);}
    if(!strcmp(r->path,"/api/timezone")&&!strcmp(r->method,"GET")){unsigned char*tz=NULL;size_t tzl=0;if(joan_run_helper(&G,"timezone-get",NULL,NULL,&tz,&tzl,5)||!tzl||tz[0]!='{'){free(tz);return json(c,200,"{\"gmt_tz\":\"\",\"tz_name\":\"\"}");}if(tzl>=sizeof(body))tzl=sizeof(body)-1;memcpy(body,tz,tzl);body[tzl]=0;free(tz);return json(c,200,body);}
    if(!strcmp(r->path,"/api/timezone")&&!strcmp(r->method,"PUT")){char gmt[16],name[80],arg[112];int i;if(json_field(r->body,r->content_length,"gmt_tz",gmt,sizeof(gmt)))return error_json(c,400,"invalid_json","gmt_tz (\"GMT+HH:MM\") is required");if(!(gmt[0]=='G'&&gmt[1]=='M'&&gmt[2]=='T'&&(gmt[3]=='+'||gmt[3]=='-')&&gmt[4]>='0'&&gmt[4]<='9'&&gmt[5]>='0'&&gmt[5]<='9'&&gmt[6]==':'&&gmt[7]>='0'&&gmt[7]<='9'&&gmt[8]>='0'&&gmt[8]<='9'&&gmt[9]==0))return error_json(c,400,"invalid_timezone","gmt_tz must look like GMT-03:00");if(json_field(r->body,r->content_length,"tz_name",name,sizeof(name)))name[0]=0;if(name[0]){if(!((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')))return error_json(c,400,"invalid_timezone","tz_name is not a valid zone");for(i=0;name[i];i++){char ch=name[i];if(!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='/'||ch=='+'||ch=='-'))return error_json(c,400,"invalid_timezone","tz_name is not a valid zone");}}if(name[0])snprintf(arg,sizeof(arg),"%s %s",gmt,name);else snprintf(arg,sizeof(arg),"%s",gmt);if(joan_run_helper(&G,"timezone-set",NULL,arg,NULL,NULL,10))return error_json(c,502,"timezone_set_failed","could not write the camera time zone");snprintf(body,sizeof(body),"{\"ok\":true,\"gmt_tz\":\"%s\",\"tz_name\":\"%s\",\"applies\":\"on_restart\"}",gmt,name);return json(c,200,body);}
    if(!strcmp(r->path,"/api/session-info")){snprintf(body,sizeof(body),"{\"authenticated\":true,\"csrf\":\"%s\",\"default_password_warning\":%s}",a.csrf,a.must_change?"true":"false");return json(c,200,body);}
    if(!strncmp(r->path,"/api/operations/",16)&&!strcmp(r->method,"GET")){char result[2049],wrapped[2300];int op=joan_mqtt_operation(r->path+16,result,sizeof(result));if(op<0)return error_json(c,404,"operation_not_found","operation does not exist or failed");if(op>0)return json(c,200,"{\"state\":\"pending\"}");snprintf(wrapped,sizeof(wrapped),"{\"state\":\"complete\",\"response\":%s}",result[0]?result:"null");return json(c,200,wrapped);}
    if(!strcmp(r->path,"/api/logout")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"DELETE"))){joan_auth_logout(&a);return response(c,204,"application/json","",0,"Set-Cookie: joan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");}
    if(!strcmp(r->path,"/api/password")&&!strcmp(r->method,"POST")){
        char escaped[768],command[1024],operation[65];int rtsp_ok=0;
        if(json_field(r->body,r->content_length,"old_password",x,sizeof(x))||
           json_field(r->body,r->content_length,"new_password",y,sizeof(y)))
            return error_json(c,400,"invalid_json","old_password and new_password are required");
        if(joan_auth_change_password(&G,&a,x,y)){
            memset(x,0,sizeof(x));memset(y,0,sizeof(y));
            return error_json(c,400,"password_rejected","password change was rejected");
        }
        json_escape((const unsigned char*)y,strlen(y),escaped,sizeof(escaped));
        snprintf(command,sizeof(command),"{\"cmd\":66516,\"cmd_type\":\"request\",\"sessionid\":0,\"value\":0,\"security_enhanced_switch\":1}");
        if(!joan_mqtt_request(66516,command,operation)){
            snprintf(command,sizeof(command),"{\"cmd\":66517,\"cmd_type\":\"request\",\"sessionid\":0,\"value\":0,\"security_password\":\"%s\"}",escaped);
            rtsp_ok=!joan_mqtt_request(66517,command,operation);
        }
        memset(x,0,sizeof(x));memset(y,0,sizeof(y));memset(escaped,0,sizeof(escaped));memset(command,0,sizeof(command));
        return json(c,200,rtsp_ok?"{\"ok\":true,\"login_required\":true,\"rtsp_password_sync\":\"pending\"}":"{\"ok\":true,\"login_required\":true,\"rtsp_password_sync\":\"unavailable\"}");
    }
    if(!strncmp(r->path,"/api/video/",11)&&!strcmp(r->method,"GET")){char stream[16],kind[32];unsigned char*out=NULL;size_t n=0;uint32_t seq=0,after=0;char extra[96];if(sscanf(r->path,"/api/video/%15[^/]/%31s",stream,kind)!=2|| (strcmp(stream,"main")&&strcmp(stream,"sub")))return error_json(c,404,"stream_not_found","unknown stream");if(!strcmp(kind,"init.mp4")){if(joan_fmp4_init_segment(stream,&out,&n,5000))return error_json(c,503,"stream_unavailable","initialization segment is not ready");response(c,200,"video/mp4",out,n,NULL);free(out);return 0;}if(strcmp(kind,"fragment.mp4"))return error_json(c,404,"route_not_found","unknown video route");if(r->query[0]){char*ep=NULL;if(strncmp(r->query,"after=",6))return error_json(c,400,"invalid_sequence","expected after sequence");after=(uint32_t)strtoul(r->query+6,&ep,10);if(!ep||*ep)return error_json(c,400,"invalid_sequence","expected numeric sequence");}if(joan_fmp4_fragment(stream,after,&out,&n,&seq,10000))return error_json(c,503,"stream_timeout","no new fragment is available");snprintf(extra,sizeof(extra),"X-Joan-Sequence: %u\r\n",seq);response(c,200,"video/mp4",out,n,extra);free(out);return 0;}
    if(!strcmp(r->path,"/api/streams")&&!strcmp(r->method,"GET")){char rtsp_host[256];snprintf(rtsp_host,sizeof(rtsp_host),"%s",r->host);if(rtsp_host[0]=='['){char*end=strchr(rtsp_host,']');if(end)end[1]=0;}else{char*port=strrchr(rtsp_host,':');if(port)*port=0;}snprintf(body,sizeof(body),"{\"streams\":[{\"id\":\"main\",\"rtsp\":\"rtsp://%s/live/ch0\",\"width\":2304,\"height\":1296,\"mime\":\"video/mp4; codecs=\\\"avc1.640032\\\"\",\"init\":\"/api/v1/video/main/init.mp4\",\"fragment\":\"/api/v1/video/main/fragment.mp4\"},{\"id\":\"sub\",\"rtsp\":\"rtsp://%s/live/ch1\",\"width\":640,\"height\":360,\"mime\":\"video/mp4; codecs=\\\"avc1.640016\\\"\",\"init\":\"/api/v1/video/sub/init.mp4\",\"fragment\":\"/api/v1/video/sub/fragment.mp4\"}]}",rtsp_host,rtsp_host);return json(c,200,body);}
    if(!strcmp(r->path,"/api/snapshot")&&!strcmp(r->method,"GET"))return error_json(c,501,"feature_unavailable","snapshot is not available; use live video");
    if(!strcmp(r->path,"/api/wifi/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"configuration required\"}");if(joan_stage_blob(&G,"wifi",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"wifi-stage",path,id);}
    if(!strcmp(r->path,"/api/wifi/commit")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-commit",NULL,id);}
    if(!strcmp(r->path,"/api/wifi/rollback")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"DELETE"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-rollback",NULL,id);}
    if(!strcmp(r->path,"/api/mdns")&&!strcmp(r->method,"GET")){joan_mdns_get_hostname(&G,x);snprintf(body,sizeof(body),"{\"hostname\":\"%s\",\"address\":\"%s.local\",\"status\":\"%s\"}",x,x,joan_mdns_status());return json(c,200,body);}
    if(!strcmp(r->path,"/api/mdns")&&(!strcmp(r->method,"PUT")||!strcmp(r->method,"POST"))){if(json_field(r->body,r->content_length,"hostname",x,sizeof(x))||joan_mdns_set_hostname(&G,x))return error_json(c,400,"invalid_hostname","hostname must be a 1-63 character DNS label");if(!G.plain_http&&joan_tls_ensure_identity(&G))return error_json(c,500,"identity_generation_failed","could not prepare identity for hostname");joan_mdns_get_configured_hostname(&G,x);snprintf(body,sizeof(body),"{\"ok\":true,\"hostname\":\"%s\",\"address\":\"%s.local\",\"restart_required\":true}",x,x);return json(c,200,body);}
    if(!strcmp(r->path,"/api/routes")&&!strcmp(r->method,"GET")){unsigned char*out=NULL;size_t n=0;if(joan_run_helper(&G,"routes-list",NULL,NULL,&out,&n,5)||!n||(out[0]!='['&&out[0]!='{')){free(out);return error_json(c,502,"routes_unavailable","route policy unavailable");}response(c,200,"application/json; charset=utf-8",out,n,NULL);free(out);return 0;}
    if(!strcmp(r->path,"/api/routes")&&!strcmp(r->method,"PUT")){char cidrs[1024],canonical[1200];if(json_field(r->body,r->content_length,"cidrs",cidrs,sizeof(cidrs))||private_routes(cidrs,canonical,sizeof(canonical)))return error_json(c,400,"invalid_routes","only RFC1918 and ULA CIDRs are allowed");if(joan_stage_blob(&G,"routes",canonical,strlen(canonical),id,path))return error_json(c,500,"stage_failed","could not stage routes");return helper_json(c,"routes-set",path,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"GET"))return helper_json(c,"ssh-list",NULL,NULL);
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"POST")){if(!r->body||r->content_length>16384)return json(c,400,"{\"error\":\"invalid key\"}");if(joan_stage_blob(&G,"ssh-key",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"ssh-add",path,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"DELETE")){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"ssh-delete",NULL,id);}
    if(!strcmp(r->path,"/api/ptz/lease")&&!strcmp(r->method,"POST")){unsigned char rnd[32];pthread_mutex_lock(&ptz_lock);if(ptz_lease[0]&&ptz_expires>time(NULL)){pthread_mutex_unlock(&ptz_lock);return error_json(c,409,"ptz_busy","PTZ is leased");}if(joan_random(rnd,sizeof(rnd))){pthread_mutex_unlock(&ptz_lock);return error_json(c,500,"random_failed","could not create lease");}joan_hex(rnd,sizeof(rnd),ptz_lease);ptz_expires=time(NULL)+2;snprintf(body,sizeof(body),"{\"lease\":\"%s\",\"expires_in\":2}",ptz_lease);pthread_mutex_unlock(&ptz_lock);return json(c,201,body);}
    if(!strcmp(r->path,"/api/ptz/move")&&!strcmp(r->method,"POST")){unsigned duration,speed;char canonical[512];if(json_field(r->body,r->content_length,"lease",x,sizeof(x))||json_field(r->body,r->content_length,"command",y,sizeof(y))||json_uint(r->body,r->content_length,"duration_ms",&duration)||json_uint(r->body,r->content_length,"speed",&speed))return error_json(c,400,"invalid_ptz_jog","lease, command, duration_ms and speed are required");if((strcmp(y,"up")&&strcmp(y,"down")&&strcmp(y,"left")&&strcmp(y,"right"))||duration<50||duration>1000||speed<1||speed>5)return error_json(c,400,"invalid_ptz_jog","direction, duration or speed is out of range");pthread_mutex_lock(&ptz_lock);if(ptz_expires<time(NULL)||strcmp(x,ptz_lease)){pthread_mutex_unlock(&ptz_lock);return error_json(c,403,"invalid_lease","invalid PTZ lease");}ptz_expires=time(NULL)+(duration+1999)/1000;pthread_mutex_unlock(&ptz_lock);snprintf(canonical,sizeof(canonical),"{\"command\":\"%s\",\"duration_ms\":%u,\"speed\":%u}\n",y,duration,speed);if(joan_stage_blob(&G,"ptz",canonical,strlen(canonical),id,path))return error_json(c,500,"stage_failed","could not stage PTZ jog");return helper_json(c,"ptz-jog",path,id);}
    if(!strcmp(r->path,"/api/ptz/release")&&!strcmp(r->method,"POST")){int stop=0;if(json_field(r->body,r->content_length,"lease",x,sizeof(x)))return json(c,400,"{\"error\":\"lease required\"}");pthread_mutex_lock(&ptz_lock);if(!strcmp(x,ptz_lease)){ptz_lease[0]=0;ptz_expires=0;stop=1;}pthread_mutex_unlock(&ptz_lock);return stop?helper_json(c,"ptz-stop",NULL,NULL):json(c,403,"{\"error\":\"invalid lease\"}");}
    if(!strcmp(r->path,"/api/ptz/stop")&&!strcmp(r->method,"POST")){if(json_field(r->body,r->content_length,"lease",x,sizeof(x)))return json(c,400,"{\"error\":\"lease required\"}");pthread_mutex_lock(&ptz_lock);if(strcmp(x,ptz_lease)){pthread_mutex_unlock(&ptz_lock);return json(c,403,"{\"error\":\"invalid lease\"}");}ptz_lease[0]=0;ptz_expires=0;pthread_mutex_unlock(&ptz_lock);return helper_json(c,"ptz-stop",NULL,NULL);}
    if(!strcmp(r->path,"/api/ptz/home")&&!strcmp(r->method,"POST")){char action[16]="list",command[512];unsigned token;(void)json_field(r->body,r->content_length,"action",action,sizeof(action));if(!strcmp(action,"set")){snprintf(command,sizeof(command),"{\"cmd\":66485,\"cmd_type\":\"request\",\"name\":\"__home__\",\"mot_index\":0}");return mqtt_accepted(c,66485,command);}if(!strcmp(action,"goto")){if(json_uint(r->body,r->content_length,"token",&token))return error_json(c,400,"invalid_preset","numeric home preset token is required");snprintf(command,sizeof(command),"{\"cmd\":66491,\"cmd_type\":\"request\",\"coordinateID\":%u,\"mot_index\":0}",token);return mqtt_accepted(c,66491,command);}return mqtt_accepted(c,66486,"{\"cmd\":66486,\"cmd_type\":\"request\",\"mot_index\":0}");}
    if(!strcmp(r->path,"/api/ptz/presets")&&!strcmp(r->method,"GET"))return mqtt_accepted(c,66486,"{\"cmd\":66486,\"cmd_type\":\"request\"}");
    if(!strcmp(r->path,"/api/ptz/presets")&&!strcmp(r->method,"POST")){char command[768],escaped[384];unsigned token;if(json_field(r->body,r->content_length,"name",x,sizeof(x))||!x[0]||strlen(x)>64)return error_json(c,400,"invalid_preset","preset name is required");json_escape((unsigned char*)x,strlen(x),escaped,sizeof(escaped));if(!json_uint(r->body,r->content_length,"token",&token)){snprintf(command,sizeof(command),"{\"cmd\":66489,\"cmd_type\":\"request\",\"coordinateID\":%u,\"name\":\"%s\",\"mot_index\":0}",token,escaped);return mqtt_accepted(c,66489,command);}snprintf(command,sizeof(command),"{\"cmd\":66485,\"cmd_type\":\"request\",\"name\":\"%s\",\"mot_index\":0}",escaped);return mqtt_accepted(c,66485,command);}
    if(!strcmp(r->path,"/api/ptz/presets")&&!strcmp(r->method,"PUT")){char command[512];unsigned token;if(json_uint(r->body,r->content_length,"token",&token))return error_json(c,400,"invalid_preset","numeric preset token is required");snprintf(command,sizeof(command),"{\"cmd\":66491,\"cmd_type\":\"request\",\"coordinateID\":%u,\"mot_index\":0}",token);return mqtt_accepted(c,66491,command);}
    if(!strcmp(r->path,"/api/ptz/presets")&&!strcmp(r->method,"DELETE")){char command[512];unsigned token;if(json_uint(r->body,r->content_length,"token",&token))return error_json(c,400,"invalid_preset","numeric preset token is required");snprintf(command,sizeof(command),"{\"cmd\":66490,\"cmd_type\":\"request\",\"ptz_coordinate\":[{\"coordinateID\":%u}],\"mot_index\":0}",token);return mqtt_accepted(c,66490,command);}
    if(!strcmp(r->path,"/api/firmware/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"image required\"}");if(joan_stage_blob(&G,"firmware",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"firmware-verify",path,id);}
    if(!strcmp(r->path,"/api/firmware/apply")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"firmware-apply",NULL,id);}
    return error_json(c,404,"route_not_found","route not found");
}

static void serve(Conn*c,const char*remote){JoanRequest r;int parsed=parse_request(c,&r);if(parsed){if(parsed==-3)return;if(parsed==-2)error_json(c,413,"payload_too_large","request body exceeds route limit");else error_json(c,400,"bad_request","malformed request or unsupported transfer encoding");return;}snprintf(r.remote,sizeof(r.remote),"%s",remote);if(!strncmp(r.path,"/api/",5)||!strncmp(r.path,"/ws/",4))route(c,&r);else static_file(c,r.path);if(r.body){memset(r.body,0,r.content_length);free(r.body);}}

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
    if(!worker->plain_http){int rc;mbedtls_ssl_init(&ssl);if(mbedtls_ssl_setup(&ssl,worker->tls_config))ready=0;if(ready){mbedtls_ssl_set_bio(&ssl,&c.fd,mbedtls_net_send,mbedtls_net_recv,NULL);while((rc=mbedtls_ssl_handshake(&ssl))!=0){if(rc==MBEDTLS_ERR_SSL_WANT_READ){if(tls_wait(c.fd,POLLIN))break;continue;}if(rc==MBEDTLS_ERR_SSL_WANT_WRITE){if(tls_wait(c.fd,POLLOUT))break;continue;}break;}if(rc){mbedtls_ssl_free(&ssl);ready=0;}else c.ssl=&ssl;}}
#endif
    if(ready)serve(&c,worker->remote);
#ifndef JOAN_NO_TLS
    if(c.ssl){mbedtls_ssl_close_notify(c.ssl);mbedtls_ssl_free(c.ssl);}
#endif
    close(c.fd);pthread_mutex_lock(&worker_lock);if(worker_count)worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);free(worker);return NULL;
}

int joan_server_run(const JoanConfig*cfg){int s,one=1;struct sockaddr_in a;pthread_t ptz_thread;G=*cfg;pthread_mutex_lock(&worker_lock);server_stopping=0;pthread_mutex_unlock(&worker_lock);
#ifndef JOAN_NO_TLS
    mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context drbg;mbedtls_ssl_config sc;mbedtls_x509_crt cert;mbedtls_pk_context key;char cp[512],kp[512];
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&drbg);mbedtls_ssl_config_init(&sc);mbedtls_x509_crt_init(&cert);mbedtls_pk_init(&key);
    if(!cfg->plain_http){snprintf(cp,sizeof(cp),"%s/tls-cert.pem",cfg->state_dir);snprintf(kp,sizeof(kp),"%s/tls-key.pem",cfg->state_dir);if(mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,(unsigned char*)"joan-httpd",11)||mbedtls_x509_crt_parse_file(&cert,cp)||mbedtls_pk_parse_keyfile(&key,kp,NULL)||mbedtls_ssl_config_defaults(&sc,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)||mbedtls_ssl_conf_own_cert(&sc,&cert,&key))return-1;mbedtls_ssl_conf_min_version(&sc,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);mbedtls_ssl_conf_renegotiation(&sc,MBEDTLS_SSL_RENEGOTIATION_DISABLED);mbedtls_ssl_conf_session_tickets(&sc,MBEDTLS_SSL_SESSION_TICKETS_DISABLED);mbedtls_ssl_conf_rng(&sc,locked_rng,&drbg);}
#else
    if(!cfg->plain_http){fprintf(stderr,"HTTPS requested but binary was built JOAN_NO_TLS; refusing\n");return-1;}
#endif
    if(pthread_create(&ptz_thread,NULL,ptz_expiry_loop,NULL))return-1;
    pthread_detach(ptz_thread);
    if(!cfg->plain_http&&redirect_start())return-1;
    s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return-1;server_fd=s;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)cfg->port);if(inet_pton(AF_INET,cfg->bind_addr,&a.sin_addr)!=1||bind(s,(struct sockaddr*)&a,sizeof(a))||listen(s,8)){close(s);server_fd=-1;return-1;}
    for(;;){struct sockaddr_in peer;socklen_t pl=sizeof(peer);int fd=accept(s,(struct sockaddr*)&peer,&pl);Worker*w;pthread_t t;if(fd<0){if(errno==EINTR)continue;break;}pthread_mutex_lock(&worker_lock);while(worker_count>=MAX_WORKERS&&!server_stopping)pthread_cond_wait(&worker_available,&worker_lock);if(server_stopping){pthread_mutex_unlock(&worker_lock);close(fd);break;}worker_count++;pthread_mutex_unlock(&worker_lock);w=calloc(1,sizeof(*w));if(!w){pthread_mutex_lock(&worker_lock);worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);close(fd);continue;}w->fd=fd;w->plain_http=cfg->plain_http;inet_ntop(AF_INET,&peer.sin_addr,w->remote,sizeof(w->remote));
#ifndef JOAN_NO_TLS
        w->tls_config=&sc;
#endif
        if(pthread_create(&t,NULL,serve_worker,w)){pthread_mutex_lock(&worker_lock);worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);close(fd);free(w);continue;}pthread_detach(t);
    }close(s);server_fd=-1;return 0;
}
void joan_server_stop(void){int fd;pthread_mutex_lock(&worker_lock);server_stopping=1;fd=server_fd;pthread_cond_broadcast(&worker_available);pthread_mutex_unlock(&worker_lock);if(fd>=0)shutdown(fd,SHUT_RDWR);}
