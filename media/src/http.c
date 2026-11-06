/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "jipc.h"
#include "ui_assets.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef JIPC_WITH_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#endif

#define HTTP_HEADER_LIMIT 8192
#define HTTP_WORKERS 4
#define HTTP_QUEUE 8
#define HTTP_REPLY_LIMIT 65536
#define HTTP_IDLE 8
#define HTTP_IDLE_US UINT64_C(15000000)
typedef struct connection connection;

typedef struct http_server {
    jipc_app *app;
    int listener, ui_listener, stopping, secure, wake[2];
    unsigned ui_port;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t worker[HTTP_WORKERS];
    unsigned started, count, head, tail;
    connection *queue[HTTP_QUEUE], *idle[HTTP_IDLE];
    int active[HTTP_WORKERS];
#ifdef JIPC_WITH_MBEDTLS
    pthread_mutex_t random_mutex;
    int tls_initialized;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_ssl_config tls_config;
#endif
} http_server;
struct connection {
    http_server *server;
    int fd, initialized, keepalive, failed, challenge;
    unsigned requests;
    uint64_t deadline, idle_until;
#ifdef JIPC_WITH_MBEDTLS
    mbedtls_ssl_context ssl;
    int ssl_initialized;
#endif
};
typedef struct { http_server *server; unsigned index; } worker_arg;
typedef struct {
    char method[12], target[1024], host[256], authorization[512], type[128], origin[384];
    char cookie[1100], csrf[96], requested_with[32], fetch_site[24];
    char *body;
    size_t length, upload; /* upload: total body size left on the connection */
    int keepalive;
} request;

static int wait_socket(connection *c,int write_ready){
    for(;;){
        uint64_t now=jipc_now_us(); if(now>=c->deadline)return -1;
        uint64_t left=(c->deadline-now+999)/1000;
        struct pollfd p={c->fd,write_ready?POLLOUT:POLLIN,0};
        int n=poll(&p,1,left>30000?30000:(int)left);
        if(n>0)return (p.revents&(write_ready?POLLOUT:POLLIN))?0:-1;
        if(n<0&&errno==EINTR)continue;
        if(n==0)continue;
        return -1;
    }
}
#ifdef JIPC_WITH_MBEDTLS
static int tls_random(void *arg,unsigned char *out,size_t n){
    http_server *s=arg;pthread_mutex_lock(&s->random_mutex);
    int ret=mbedtls_ctr_drbg_random(&s->random,out,n);
    pthread_mutex_unlock(&s->random_mutex);return ret;
}
static int tls_send(void *arg,const unsigned char *data,size_t n){
    int fd=*(int*)arg;ssize_t ret=send(fd,data,n,MSG_NOSIGNAL);
    if(ret>=0)return (int)ret;
    if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int tls_recv(void *arg,unsigned char *data,size_t n){
    int fd=*(int*)arg;ssize_t ret=recv(fd,data,n,0);
    if(ret>=0)return (int)ret;
    if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
static int tls_again(connection *c,int ret){
    if(ret==MBEDTLS_ERR_SSL_WANT_READ)return wait_socket(c,0);
    if(ret==MBEDTLS_ERR_SSL_WANT_WRITE)return wait_socket(c,1);
    return -1;
}
static int tls_setup(http_server *s,const jipc_config *config){
    mbedtls_entropy_init(&s->entropy);mbedtls_ctr_drbg_init(&s->random);
    mbedtls_x509_crt_init(&s->cert);mbedtls_pk_init(&s->key);mbedtls_ssl_config_init(&s->tls_config);
    s->tls_initialized=1;
    const unsigned char personal[]="jooanipc-open HTTPS";
    if(mbedtls_ctr_drbg_seed(&s->random,mbedtls_entropy_func,&s->entropy,personal,sizeof(personal)-1)<0)return -1;
    if(mbedtls_x509_crt_parse_file(&s->cert,config->tls_cert)!=0)return -1;
#if MBEDTLS_VERSION_MAJOR >= 3
    if(mbedtls_pk_parse_keyfile(&s->key,config->tls_key,NULL,tls_random,s)<0)return -1;
    if(mbedtls_pk_check_pair(&s->cert.pk,&s->key,tls_random,s)<0)return -1;
#else
    if(mbedtls_pk_parse_keyfile(&s->key,config->tls_key,NULL)<0)return -1;
    if(mbedtls_pk_check_pair(&s->cert.pk,&s->key)<0)return -1;
#endif
    if(mbedtls_ssl_config_defaults(&s->tls_config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)<0)return -1;
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_ssl_conf_min_tls_version(&s->tls_config,MBEDTLS_SSL_VERSION_TLS1_2);
#else
    mbedtls_ssl_conf_min_version(&s->tls_config,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
#endif
    mbedtls_ssl_conf_rng(&s->tls_config,tls_random,s);
    if(mbedtls_ssl_conf_own_cert(&s->tls_config,&s->cert,&s->key)<0)return -1;
    s->secure=1;return 0;
}
static void tls_free(http_server *s){
    if(s->tls_initialized){mbedtls_ssl_config_free(&s->tls_config);mbedtls_pk_free(&s->key);mbedtls_x509_crt_free(&s->cert);mbedtls_ctr_drbg_free(&s->random);mbedtls_entropy_free(&s->entropy);}
}
#endif
static int handshake(connection *c){
    if(!c->server->secure)return 0;
#ifdef JIPC_WITH_MBEDTLS
    mbedtls_ssl_init(&c->ssl);c->ssl_initialized=1;
    if(mbedtls_ssl_setup(&c->ssl,&c->server->tls_config)<0)return -1;
    mbedtls_ssl_set_bio(&c->ssl,&c->fd,tls_send,tls_recv,NULL);
    int ret;while((ret=mbedtls_ssl_handshake(&c->ssl))!=0)if(tls_again(c,ret)<0)return -1;
    return 0;
#else
    return -1;
#endif
}
static ssize_t receive(connection *c,void *data,size_t n){
    for(;;){
#ifdef JIPC_WITH_MBEDTLS
        if(c->server->secure){int ret=mbedtls_ssl_read(&c->ssl,data,n);if(ret>=0)return ret;if(tls_again(c,ret)<0)return -1;continue;}
#endif
        ssize_t ret=recv(c->fd,data,n,0);if(ret>=0)return ret;
        if(errno==EINTR)continue;
        if((errno==EAGAIN||errno==EWOULDBLOCK)&&wait_socket(c,0)==0)continue;
        return -1;
    }
}
static int send_all(connection *c,const void *data,size_t n){
    const unsigned char *p=data;
    while(n){
        ssize_t ret;
#ifdef JIPC_WITH_MBEDTLS
        if(c->server->secure){ret=mbedtls_ssl_write(&c->ssl,p,n>16384?16384:n);if(ret<0){if(tls_again(c,(int)ret)==0)continue;return -1;}}
        else
#endif
        {ret=send(c->fd,p,n,MSG_NOSIGNAL);if(ret<0){if(errno==EINTR)continue;if((errno==EAGAIN||errno==EWOULDBLOCK)&&wait_socket(c,1)==0)continue;return -1;}}
        if(ret<=0)return -1;
        p+=(size_t)ret;n-=(size_t)ret;
    }
    return 0;
}
static const char *reason(int code){switch(code){case 200:return "OK";case 204:return "No Content";case 502:return "Bad Gateway";case 400:return "Bad Request";case 401:return "Unauthorized";case 403:return "Forbidden";case 404:return "Not Found";case 405:return "Method Not Allowed";case 409:return "Conflict";case 411:return "Length Required";case 413:return "Content Too Large";case 415:return "Unsupported Media Type";case 417:return "Expectation Failed";case 429:return "Too Many Requests";case 431:return "Request Header Fields Too Large";case 500:return "Internal Server Error";case 501:return "Not Implemented";case 503:return "Service Unavailable";default:return "Error";}}
/* The UI is same-origin only: no inline code, no framing, no referrers. */
#define HTTP_CSP "default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; connect-src 'self'; style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
static int header_with(connection *c,int status,const char *type,uint64_t length,const char *extra){
    /* A Basic challenge only for clients that did not come from the UI, so a
     * browser never pops its own password dialog over the sign-in page. */
    char out[2048];int n=snprintf(out,sizeof(out),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: %s\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: " HTTP_CSP "\r\n%s%s\r\n",status,reason(status),type,(unsigned long long)length,c->keepalive?"keep-alive":"close",status==401&&c->challenge?"WWW-Authenticate: Basic realm=\"jooanipc-open\", charset=\"UTF-8\"\r\n":"",extra?extra:"");
    if(n<0||(size_t)n>=sizeof(out))return -1;
    return send_all(c,out,(size_t)n);
}
static int header(connection *c,int status,const char *type,uint64_t length){return header_with(c,status,type,length,NULL);}
static int response(connection *c,int status,const char *type,const void *data,size_t n){int rc=header(c,status,type,n)<0?-1:send_all(c,data,n);if(rc)c->failed=1;return rc;}
static void error_response(connection *c,int status,const char *message){
    char escaped[1024],out[1200];if(jipc_json_escape(message,escaped,sizeof(escaped))<0)snprintf(escaped,sizeof(escaped),"\"Request failed\"");
    int n=snprintf(out,sizeof(out),"{\"error\":%s}\n",escaped);if(n>0&&(size_t)n<sizeof(out))response(c,status,"application/json",out,(size_t)n);
}
static int header_name(const char *s){if(!*s)return 0;for(;*s;++s)if(!isalnum((unsigned char)*s)&&!strchr("!#$%&'*+-.^_`|~",*s))return 0;return 1;}
static int copy_header(char *to,size_t capacity,const char *value){if(*to||strlen(value)>=capacity)return -1;strcpy(to,value);return 0;}
static int parse_request(connection *c,char *buffer,size_t capacity,request *r){
    size_t got=0,head=0;memset(r,0,sizeof(*r));
    while(!head){
        if(got>=HTTP_HEADER_LIMIT)return 431;
        /* Timed out or closed before asking anything: nothing to answer. A reply
         * here would be read as the answer to the client's next request. */
        ssize_t n=receive(c,buffer+got,HTTP_HEADER_LIMIT-got);if(n<=0)return got?400:-1;
        got+=(size_t)n;buffer[got]=0;
        char *end=strstr(buffer,"\r\n\r\n");if(end)head=(size_t)(end-buffer)+4;
        if(memchr(buffer,0,head?head:got))return 400;
    }
    char *line=strstr(buffer,"\r\n");if(!line)return 400;*line=0;
    char *space=strchr(buffer,' ');if(!space)return 400;*space++=0;char *version=strchr(space,' ');if(!version)return 400;*version++=0;
    if(strcmp(version,"HTTP/1.0")&&strcmp(version,"HTTP/1.1"))return 400;
    int needs_host=!strcmp(version,"HTTP/1.1");
    r->keepalive=needs_host;
    if(strlen(buffer)>=sizeof(r->method)||strlen(space)>=sizeof(r->target)||*space!='/')return 400;
    for(const char *p=space;*p;++p)if((unsigned char)*p<=32||*p=='\\'||*p=='#'||*p=='%')return 400;
    strcpy(r->method,buffer);strcpy(r->target,space);
    /* A platform package is relayed in pieces, never buffered here. */
    int upload=!strcmp(r->method,"POST")&&!strcmp(r->target,"/api/joan/v1/update");
    size_t limit=upload?JIPC_JOAN_UPLOAD_LIMIT:JIPC_MAX_JSON;
    int length_seen=0,transfer_seen=0;unsigned seen=0;size_t length=0;char *p=line+2;
    while(p<buffer+head-2){
        char *next=strstr(p,"\r\n");if(!next)return 400;*next=0;
        if(*p==' '||*p=='\t')return 400;
        char *colon=strchr(p,':');if(!colon)return 400;*colon++=0;if(!header_name(p))return 400;
        while(*colon==' '||*colon=='\t')++colon;
        char *last=colon+strlen(colon);while(last>colon&&(last[-1]==' '||last[-1]=='\t'))*--last=0;
        for(char *q=colon;*q;++q)if((unsigned char)*q<32&&*q!='\t')return 400;
        if(!strcasecmp(p,"Content-Length")){
            if(length_seen++||!*colon)return 400;
            length=0;
            for(char *q=colon;*q;++q){if(!isdigit((unsigned char)*q))return 400;if(length>(limit-(size_t)(*q-'0'))/10)return 413;length=length*10+(size_t)(*q-'0');}
        }else if(!strcasecmp(p,"Transfer-Encoding")){transfer_seen=1;}
        else if(!strcasecmp(p,"Connection")){
            if(seen&16)return 400;
            seen|=16;
            if(!strcasecmp(colon,"close"))r->keepalive=0;
            else if(!strcasecmp(colon,"keep-alive"))r->keepalive=1;
            else return 400;
        }
        else if(!strcasecmp(p,"Expect"))return 417;
        else if(!strcasecmp(p,"Host")){if((seen&1)||copy_header(r->host,sizeof(r->host),colon)<0)return 400;seen|=1;}
        else if(!strcasecmp(p,"Authorization")){if((seen&2)||copy_header(r->authorization,sizeof(r->authorization),colon)<0)return 400;seen|=2;}
        else if(!strcasecmp(p,"Content-Type")){if((seen&4)||copy_header(r->type,sizeof(r->type),colon)<0)return 400;seen|=4;}
        else if(!strcasecmp(p,"Origin")){if((seen&8)||copy_header(r->origin,sizeof(r->origin),colon)<0)return 400;seen|=8;}
        else if(!strcasecmp(p,"Cookie")){size_t have=strlen(r->cookie),add=strlen(colon);if(have+add+3>sizeof(r->cookie))return 431;if(have){memcpy(r->cookie+have,"; ",2);have+=2;}memcpy(r->cookie+have,colon,add+1);}
        else if(!strcasecmp(p,"X-CSRF-Token")){if((seen&32)||copy_header(r->csrf,sizeof(r->csrf),colon)<0)return 400;seen|=32;}
        else if(!strcasecmp(p,"X-Requested-With")){if((seen&64)||copy_header(r->requested_with,sizeof(r->requested_with),colon)<0)return 400;seen|=64;}
        else if(!strcasecmp(p,"Sec-Fetch-Site")){if((seen&128)||copy_header(r->fetch_site,sizeof(r->fetch_site),colon)<0)return 400;seen|=128;}
        p=next+2;
    }
    if(transfer_seen||(!r->host[0]&&needs_host))return 400;
    if(!r->host[0])strcpy(r->host,"localhost");
    for(const char *q=r->host;*q;++q)if(!isalnum((unsigned char)*q)&&!strchr(".:-[]",*q))return 400;
    int body_method=!strcmp(r->method,"POST")||!strcmp(r->method,"PUT")||!strcmp(r->method,"DELETE");
    if((!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))&&!length_seen)return 411;
    if(!body_method&&length)return 400;
    if(got>head+length)return 400; /* Pipelined/extra framing is not accepted. */
    if(upload){r->body=buffer+head;r->length=got-head;r->upload=length;r->body[r->length]=0;r->keepalive=0;return 0;}
    if(length>JIPC_MAX_JSON||head+length+1>capacity)return 413;
    while(got<head+length){ssize_t n=receive(c,buffer+got,head+length-got);if(n<=0)return 400;got+=(size_t)n;}
    r->body=buffer+head;r->length=length;r->body[length]=0;
    if(strcmp(r->target,"/api/v1/talk")&&memchr(r->body,0,length))return 400;
    return 0;
}
static int basic_authorized(jipc_app *app,const char *auth){
    if(strncasecmp(auth,"Basic ",6))return 0;
    unsigned char decoded[256];int n=jipc_base64_decode(auth+6,decoded,sizeof(decoded)-1);if(n<=0||n>=(int)sizeof(decoded)||memchr(decoded,0,(size_t)n))return 0;
    decoded[n]=0;char *colon=strchr((char*)decoded,':');if(!colon){memset(decoded,0,sizeof(decoded));return 0;}*colon++=0;
    jipc_config config;jipc_config_copy(app,&config);int result=jipc_equal((char*)decoded,config.username)&jipc_equal(colon,config.password);
    memset(decoded,0,sizeof(decoded));memset(config.password,0,sizeof(config.password));return result;
}
static int json_type(const char *type){return !strncasecmp(type,"application/json",16)&&(type[16]==0||type[16]==';');}
static int safe_name(const char *name){size_t n=strlen(name);if(!n||n>255||name[0]=='.')return 0;for(size_t i=0;i<n;++i)if(!isalnum((unsigned char)name[i])&&!strchr("._-",name[i]))return 0;return 1;}
static int recordings_directory(jipc_app *app){jipc_config cfg;jipc_config_copy(app,&cfg);return open(cfg.record_dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);}
static void recordings(connection *c){
    int fd=recordings_directory(c->server->app);if(fd<0){error_response(c,503,"Recording directory is unavailable");return;}
    DIR *dir=fdopendir(fd);if(!dir){close(fd);error_response(c,503,"Recording directory is unavailable");return;}
    char *out=malloc(JIPC_MAX_JSON);if(!out){closedir(dir);error_response(c,503,"Out of memory");return;}
    size_t used=(size_t)snprintf(out,JIPC_MAX_JSON,"{\"recordings\":[");int first=1,truncated=0;struct dirent *entry;
    while((entry=readdir(dir))){struct stat st;if(!safe_name(entry->d_name)||fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<0||!S_ISREG(st.st_mode))continue;
        char item[512];int n=snprintf(item,sizeof(item),"%s{\"name\":\"%s\",\"size\":%llu,\"modified\":%lld}",first?"":",",entry->d_name,(unsigned long long)st.st_size,(long long)st.st_mtime);
        if(n<0||(size_t)n>=sizeof(item)||used+(size_t)n+40>=JIPC_MAX_JSON){truncated=1;break;}
        memcpy(out+used,item,(size_t)n);used+=(size_t)n;first=0;
    }
    int n=snprintf(out+used,JIPC_MAX_JSON-used,"],\"truncated\":%s}\n",truncated?"true":"false");
    if(n>0)response(c,200,"application/json",out,used+(size_t)n);else error_response(c,500,"Unable to list recordings");
    free(out);closedir(dir);
}
static void recording_download(connection *c,const char *name){
    if(!safe_name(name)){error_response(c,400,"Invalid recording name");return;}
    int dir=recordings_directory(c->server->app);if(dir<0){error_response(c,503,"Recording directory is unavailable");return;}
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);close(dir);struct stat st;
    if(fd<0){error_response(c,404,"Recording not found");return;}
    if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<0){close(fd);error_response(c,404,"Recording not found");return;}
    c->deadline=jipc_now_us()+30000000;
    c->keepalive=0; /* A streamed download never reuses partially sent framing. */
    if(header(c,200,"application/octet-stream",(uint64_t)st.st_size)==0){
        unsigned char data[16384];off_t left=st.st_size;
        while(left>0){size_t want=(uint64_t)left>sizeof(data)?sizeof(data):(size_t)left;ssize_t n=read(fd,data,want);if(n<0&&errno==EINTR)continue;if(n<=0||send_all(c,data,(size_t)n)<0)break;left-=n;}
    }
    close(fd);
}
/* A browser's own fetch: same site and, when required, a matching Origin. */
static int same_origin(connection *c,const request *r,int required){
    if(r->fetch_site[0]&&strcmp(r->fetch_site,"same-origin")&&strcmp(r->fetch_site,"none"))return 0;
    if(!r->origin[0])return !required;
    char allowed[512];snprintf(allowed,sizeof(allowed),"%s://%s",c->server->secure?"https":"http",r->host);
    return !strcmp(allowed,r->origin);
}
static void ui_asset_response(connection *c,const char *name){
    for(const ui_asset *a=ui_assets;a->name;++a)if(!strcmp(a->name,name)){response(c,200,a->type,a->data,a->size);return;}
    static const char fallback[]="<!doctype html><meta charset=utf-8><title>jooanipc-open</title><p>This build has no admin UI.\n";
    if(!strcmp(name,"index.html"))response(c,200,"text/html; charset=utf-8",fallback,sizeof(fallback)-1);
    else error_response(c,404,"Not found");
}
/* /api/joan/<rest> -> the platform's /api/<rest> on loopback. The platform
 * authenticates (its session cookie and CSRF token pass through); this side
 * checks the Origin it drops, throttles failed sign-ins per address and adds
 * Secure to its cookie. Two relays at most, so slow platform calls never
 * starve ONVIF. */
static atomic_int relays;
static ssize_t upload_pull(void *context,void *data,size_t capacity){return receive(context,data,capacity);}
static void joan_relay(connection *c,request *r){
    const char *rest=r->target+10;int get=!strcmp(r->method,"GET");
    if(!get&&strcmp(r->method,"POST")&&strcmp(r->method,"PUT")&&strcmp(r->method,"DELETE")){error_response(c,405,"Use GET, POST, PUT or DELETE");return;}
    if(!*rest||strstr(rest,"..")||strstr(rest,"//")){error_response(c,404,"Endpoint not found");return;}
    if(!same_origin(c,r,!get)){error_response(c,403,"Cross-origin requests are not accepted");return;}
    if(r->upload&&strcasecmp(r->type,"application/octet-stream")){error_response(c,415,"Upload the package as application/octet-stream");return;}
    if(!r->upload&&r->length&&!json_type(r->type)&&strncasecmp(r->type,"text/plain",10)){error_response(c,415,"Use application/json or text/plain");return;}
    int login=!strcmp(r->method,"POST")&&(!strcmp(rest,"v1/session")||!strcmp(rest,"login"));
    int logout=(!strcmp(r->method,"DELETE")&&!strcmp(rest,"v1/session"))||!strcmp(rest,"logout");
    if(login&&jipc_auth_blocked(c->fd)){error_response(c,429,"Too many failed sign-ins from this address; retry in a minute");return;}
    if(atomic_fetch_add(&relays,1)>=2){atomic_fetch_sub(&relays,1);error_response(c,503,"The camera is busy; retry in a moment");return;}
    char path[1100],cookies[1100];snprintf(path,sizeof(path),"/api/%s",rest);
    jipc_joan_cookies(r->cookie,cookies,sizeof(cookies));
    jipc_joan_reply reply;int rc;
    if(r->upload){c->deadline=jipc_now_us()+120000000;rc=jipc_joan_stream(r->method,path,r->type,cookies,r->csrf,r->body,r->length,r->upload,upload_pull,c,&reply);}
    else rc=jipc_joan_request(r->method,path,r->type,cookies,r->csrf,r->body,r->length,&reply);
    atomic_fetch_sub(&relays,1);
    c->deadline=jipc_now_us()+10000000;
    if(rc==-3){c->keepalive=0;error_response(c,400,"The upload stopped before the whole package arrived");return;}
    if(rc){c->keepalive=0;error_response(c,502,rc==-2?"The system service's reply was too large":"The camera's system service is not answering");return;}
    if(login&&(reply.status==200||reply.status==401))jipc_auth_result(c->fd,reply.status==200);
    if(logout)jipc_joan_forget(r->cookie);
    char extra[1200]="";size_t used=0;
    for(int i=0;i<reply.cookies;++i){
        int n=snprintf(extra+used,sizeof(extra)-used,"Set-Cookie: %s%s\r\n",reply.set_cookie[i],strstr(reply.set_cookie[i],"Secure")||!c->server->secure?"":"; Secure");
        if(n<0||(size_t)n>=sizeof(extra)-used)break;
        used+=(size_t)n;
    }
    if(header_with(c,reply.status,reply.type[0]?reply.type:"application/json",reply.length,extra)<0||send_all(c,reply.body,reply.length))c->failed=1;
    jipc_joan_reply_free(&reply);
}
static void dispatch(connection *c,request *r){
    jipc_app *app=c->server->app;
    int soap=!strcmp(r->target,"/onvif/device_service")||!strcmp(r->target,"/onvif/media_service")||!strcmp(r->target,"/onvif/ptz_service")||!strcmp(r->target,"/onvif/imaging_service");
    if((!strcmp(r->target,"/health")||!strcmp(r->target,"/healthz"))&&!strcmp(r->method,"GET")){int ready=app->backend_ready&&!app->stop;
        if(ready)response(c,200,"application/json","{\"ok\":true}\n",12);else error_response(c,503,"Not ready");return;}
    c->challenge=!r->cookie[0]&&!r->requested_with[0];
    /* The admin UI's files are public (its sign-in page is one of them). */
    if(!strcmp(r->method,"GET")&&(!strcmp(r->target,"/")||!strncmp(r->target,"/ui/",4))){ui_asset_response(c,r->target[1]?r->target+4:"index.html");return;}
    if(!strncmp(r->target,"/api/joan/",10)){joan_relay(c,r);return;}
    if(jipc_auth_blocked(c->fd)){error_response(c,429,"Too many failed logins from this address; retry in a minute");return;}
    int authorized=basic_authorized(app,r->authorization),offered=r->authorization[0]!=0;
    if(!authorized&&soap&&!strcmp(r->method,"POST")){
        if(jipc_onvif_public_time(r->body))authorized=-1; /* the one unauthenticated SOAP call */
        else{offered|=strstr(r->body,"UsernameToken")!=NULL;authorized=jipc_onvif_authorize(app,r->body);}
    }
    if(offered&&authorized>=0)jipc_auth_result(c->fd,authorized);
    /* The UI: the platform's session cookie, plus its CSRF token and this
     * page's Origin on anything that changes state. */
    if(!authorized&&!soap&&r->cookie[0]){
        char csrf[96]={0};int session=jipc_joan_session(r->cookie,csrf,sizeof(csrf));
        if(session<0){error_response(c,503,"The camera's system service is not answering");return;}
        if(session>0){
            if(strcmp(r->method,"GET")&&(!same_origin(c,r,1)||!r->csrf[0]||!jipc_equal(r->csrf,csrf))){error_response(c,403,"Request token missing or invalid");return;}
            authorized=2; /* the admin UI's own session */
        }
    }
    if(!authorized){error_response(c,401,"Authentication required");return;}
    if(r->origin[0]){char allowed[512];snprintf(allowed,sizeof(allowed),"%s://%s",c->server->secure?"https":"http",r->host);if(strcmp(allowed,r->origin)){error_response(c,403,"Cross-origin requests are not accepted");return;}}
    if(strcmp(r->method,"GET")&&strcmp(r->method,"POST")){error_response(c,405,"Use GET or POST");return;}
    if(soap){
        if(strcmp(r->method,"POST")){error_response(c,405,"SOAP requires POST");return;}
        char *out=malloc(HTTP_REPLY_LIMIT);if(!out){error_response(c,503,"Out of memory");return;}
        int code=jipc_onvif(app,r->target,r->body,r->host,out,HTTP_REPLY_LIMIT);
        if(out[0])response(c,code,"application/soap+xml; charset=utf-8",out,strlen(out));else error_response(c,500,"SOAP response exceeded its limit");free(out);return;
    }
    if(!strcmp(r->method,"POST")&&!json_type(r->type)&&strcmp(r->target,"/api/v1/talk")){error_response(c,415,"Content-Type application/json is required");return;}
    char out[JIPC_MAX_JSON],error[256]={0};
    if(!strcmp(r->target,"/api/v1/status")&&!strcmp(r->method,"GET")){if(jipc_status_json(app,out,sizeof(out))<0)error_response(c,500,"Status unavailable");else response(c,200,"application/json",out,strlen(out));return;}
    /* The stream password, for the admin UI only: its signed-in session, its
     * own fetch (a header other sites cannot send), never over Basic. */
    if(!strcmp(r->target,"/api/v1/config/password")&&!strcmp(r->method,"GET")){
        if(authorized!=2||strcmp(r->requested_with,"jipc-ui")||!same_origin(c,r,0)){error_response(c,403,"Sign in to the admin UI to see the stream password");return;}
        jipc_config cfg;jipc_config_copy(app,&cfg);char user[200],pass[400];
        int ok=jipc_json_escape(cfg.username,user,sizeof(user))>=0&&jipc_json_escape(cfg.password,pass,sizeof(pass))>=0;
        memset(cfg.password,0,sizeof(cfg.password));
        if(!ok){error_response(c,500,"Password unavailable");return;}
        int n=snprintf(out,sizeof(out),"{\"username\":%s,\"password\":%s}\n",user,pass);memset(pass,0,sizeof(pass));
        if(n>0&&(size_t)n<sizeof(out))response(c,200,"application/json",out,(size_t)n);else error_response(c,500,"Password unavailable");
        memset(out,0,sizeof(out));return;
    }
    if(!strcmp(r->target,"/api/v1/controls")){
        if(!strcmp(r->method,"GET")){if(jipc_controls_json(app,out,sizeof(out))<0)error_response(c,503,"Controls are not running");else response(c,200,"application/json",out,strlen(out));}
        else if(jipc_controls_patch(app,r->body,error,sizeof(error))<0)error_response(c,400,error[0]?error:"Invalid controls");
        else if(jipc_controls_json(app,out,sizeof(out))<0)response(c,200,"application/json","{\"ok\":true}\n",12);
        else response(c,200,"application/json",out,strlen(out));
        return;
    }
    if(!strcmp(r->target,"/api/v1/config")){
        if(!strcmp(r->method,"GET")){if(jipc_config_json(app,out,sizeof(out))<0)error_response(c,500,"Configuration unavailable");else response(c,200,"application/json",out,strlen(out));}
        else if(jipc_config_patch(app,r->body,error,sizeof(error))<0)error_response(c,400,error);
        else response(c,200,"application/json","{\"ok\":true}\n",12);
        return;
    }
    if(!strcmp(r->target,"/api/v1/control")&&!strcmp(r->method,"POST")){
        char operation[64]={0};int value=0,value2=0;
        if(jipc_json_string(r->body,"operation",operation,sizeof(operation))!=0||!*operation){error_response(c,400,"A control operation is required");return;}
        for(const char *p=operation;*p;++p)if(!islower((unsigned char)*p)&&*p!='_'){error_response(c,400,"Invalid control operation");return;}
        if(jipc_json_integer(r->body,"value",&value)<0||jipc_json_integer(r->body,"value2",&value2)<0){error_response(c,400,"Control values must be integers");return;}
        char name[65];int named=jipc_json_string(r->body,"name",name,sizeof(name));
        if(named<0){error_response(c,400,"Invalid preset name");return;}
        int result=!strcmp(operation,"preset_set") ? jipc_platform_preset_set(app,value,named?NULL:name,error,sizeof(error)) :
            !strcmp(operation,"preset_rename") ? jipc_platform_preset_rename(app,value,named?NULL:name,error,sizeof(error)) :
            jipc_backend_control(app,operation,value,value2,error,sizeof(error));
        if(result<0)error_response(c,409,error[0]?error:"Control is unavailable");
        else response(c,200,"application/json","{\"ok\":true}\n",12);
        return;
    }
    if(!strcmp(r->target,"/api/v1/recording")&&!strcmp(r->method,"POST")){
        int enabled=-1;if(jipc_json_integer(r->body,"enabled",&enabled)!=0||(enabled!=0&&enabled!=1)){error_response(c,400,"enabled must be 0 or 1");return;}
        if(jipc_record_set(app,enabled)<0)error_response(c,409,"Unable to change recording state");else response(c,200,"application/json","{\"ok\":true}\n",12);
        return;
    }
    if(!strcmp(r->target,"/api/v1/recordings")&&!strcmp(r->method,"GET")){recordings(c);return;}
    const char prefix[]="/api/v1/recordings/";
    if(!strncmp(r->target,prefix,sizeof(prefix)-1)&&!strcmp(r->method,"GET")){recording_download(c,r->target+sizeof(prefix)-1);return;}
    if((!strcmp(r->target,"/api/v1/snapshot")||!strncmp(r->target,"/api/v1/snapshot?channel=",25))&&!strcmp(r->method,"GET")){
        unsigned channel=0;const char *q=strchr(r->target,'?');if(q){char *end;errno=0;unsigned long v=strtoul(q+9,&end,10);if(errno||end==q+9||*end||v>=JIPC_VIDEO_STREAMS){error_response(c,400,"Invalid snapshot channel");return;}channel=(unsigned)v;}
        jipc_frame frame={0};if(jipc_backend_snapshot(app,channel,&frame,error,sizeof(error))<0){error_response(c,503,error[0]?error:"Snapshot unavailable");return;}
        if(frame.codec!=JIPC_JPEG||frame.size<2||frame.size>JIPC_MAX_FRAME||!frame.data||frame.data[0]!=0xff||frame.data[1]!=0xd8)error_response(c,503,"Backend did not return JPEG");
        else response(c,200,"image/jpeg",frame.data,frame.size);
        jipc_frame_free(&frame);return;
    }
    if(!strcmp(r->target,"/api/v1/talk")&&!strcmp(r->method,"POST")){
        if(strcasecmp(r->type,"audio/PCMA")&&strcasecmp(r->type,"audio/PCMA;rate=8000")){error_response(c,415,"Use audio/PCMA at 8000 Hz mono");return;}
        if(!r->length){error_response(c,400,"Audio body is empty");return;}
        /* One talker at a time, keyed by peer address; chunks are queued for
         * the speaker thread and never wait on the audio output. */
        uint64_t owner=UINT64_C(1)<<32;struct sockaddr_storage peer;socklen_t peer_size=sizeof(peer);
        if(!getpeername(c->fd,(struct sockaddr*)&peer,&peer_size)&&peer.ss_family==AF_INET)owner|=ntohl(((struct sockaddr_in*)&peer)->sin_addr.s_addr);
        int talk=jipc_backend_talk(app,owner,r->body,r->length,error,sizeof(error));
        if(talk==JIPC_TALK_OK)response(c,200,"application/json","{\"ok\":true}\n",12);
        else error_response(c,talk==JIPC_TALK_BUSY?409:talk==JIPC_TALK_INVALID?400:503,error[0]?error:"Speaker unavailable");
        return;
    }
    error_response(c,404,"Endpoint not found");
}
static void connection_close(connection *c){
    if(!c)return;
#ifdef JIPC_WITH_MBEDTLS
    if(c->ssl_initialized)mbedtls_ssl_free(&c->ssl);
#endif
    close(c->fd);free(c);
}
static void wake_server(http_server *s){
    const char byte=0; ssize_t ignored=write(s->wake[1],&byte,1);(void)ignored;
}
/* A browser often opens a connection before it has a request for it. Give a
 * client that is about to ask a moment, then wait in the idle list, not in one
 * of the workers. */
static int request_waiting(connection *c){
#ifdef JIPC_WITH_MBEDTLS
    if(c->server->secure&&mbedtls_ssl_check_pending(&c->ssl))return 1;
#endif
    struct pollfd p={c->fd,POLLIN,0};return poll(&p,1,100)>0;
}
static void handle(connection *c){
    c->deadline=jipc_now_us()+5000000;c->keepalive=0;c->failed=0;
    if(!c->initialized){
        if(handshake(c))return;
        c->initialized=1;
        if(c->server->secure&&!request_waiting(c)){c->keepalive=1;return;}
    }
    size_t capacity=HTTP_HEADER_LIMIT+JIPC_MAX_JSON+1;char *buffer=malloc(capacity);
    if(!buffer)return;
    request r;int status=parse_request(c,buffer,capacity,&r);
    if(status<0){free(buffer);return;}
    c->deadline=jipc_now_us()+10000000;
    c->keepalive=!status&&r.keepalive&&++c->requests<256;
    if(status)error_response(c,status,reason(status));else dispatch(c,&r);
    free(buffer);
    if(c->failed)c->keepalive=0;
}
static void *worker_main(void *arg){
    worker_arg info=*(worker_arg*)arg;free(arg);http_server *s=info.server;
    for(;;){
        pthread_mutex_lock(&s->mutex);while(!s->stopping&&!s->count)pthread_cond_wait(&s->ready,&s->mutex);
        if(s->stopping){pthread_mutex_unlock(&s->mutex);break;}
        connection *c=s->queue[s->head];s->head=(s->head+1)%HTTP_QUEUE;--s->count;
        s->active[info.index]=c->fd;wake_server(s);pthread_mutex_unlock(&s->mutex);
        handle(c);
        pthread_mutex_lock(&s->mutex);s->active[info.index]=-1;
        if(c->keepalive&&!s->stopping&&!s->app->stop){
            for(unsigned i=0;i<HTTP_IDLE;++i)if(!s->idle[i]){
                c->idle_until=jipc_now_us()+HTTP_IDLE_US;s->idle[i]=c;c=NULL;wake_server(s);break;
            }
        }
        pthread_mutex_unlock(&s->mutex);connection_close(c);
    }
    return NULL;
}
/* Persistent connections park here, outside the worker pool. One owner at a
 * time touches an SSL context. Requests are authenticated afresh on every use;
 * ambiguous/pipelined framing closes the connection instead of losing bytes. */
static void *server_main(void *arg){
    http_server *s=arg;
    for(;;){
        struct pollfd fds[HTTP_IDLE+3]={{s->listener,POLLIN,0},{s->wake[0],POLLIN,0}};
        fds[HTTP_IDLE+2]=(struct pollfd){s->ui_listener,POLLIN,0};
        int pending[HTTP_IDLE]={0},immediate=0;
        pthread_mutex_lock(&s->mutex);
        if(s->stopping||s->app->stop){pthread_mutex_unlock(&s->mutex);break;}
        for(unsigned i=0;i<HTTP_IDLE;++i){
            connection *c=s->count<HTTP_QUEUE?s->idle[i]:NULL;fds[i+2]=(struct pollfd){c?c->fd:-1,POLLIN,0};
#ifdef JIPC_WITH_MBEDTLS
            if(c&&s->secure)pending[i]=mbedtls_ssl_check_pending(&c->ssl);
#endif
            immediate|=pending[i];
        }
        pthread_mutex_unlock(&s->mutex);
        int ret=poll(fds,HTTP_IDLE+3,immediate?0:250);
        if(ret<0&&errno==EINTR)continue;
        if(ret<0)break;
        if(fds[1].revents&POLLIN){char bytes[64];while(read(s->wake[0],bytes,sizeof(bytes))>0){}}
        pthread_mutex_lock(&s->mutex);
        if(s->stopping||s->app->stop){pthread_mutex_unlock(&s->mutex);break;}
        uint64_t now=jipc_now_us();
        for(unsigned i=0;i<HTTP_IDLE;++i){
            connection *c=s->idle[i];if(!c||c->fd!=fds[i+2].fd)continue;
            if(now>=c->idle_until||(fds[i+2].revents&(POLLERR|POLLHUP|POLLNVAL))){s->idle[i]=NULL;connection_close(c);}
            else if((pending[i]||(fds[i+2].revents&POLLIN))&&s->count<HTTP_QUEUE){
                s->idle[i]=NULL;s->queue[s->tail]=c;s->tail=(s->tail+1)%HTTP_QUEUE;++s->count;pthread_cond_signal(&s->ready);
            }
        }
        pthread_mutex_unlock(&s->mutex);
        int from=fds[0].revents&POLLIN?s->listener:fds[HTTP_IDLE+2].revents&POLLIN?s->ui_listener:-1;
        if(from<0)continue;
        int fd=accept(from,NULL,NULL);if(fd<0)continue;
        int flags=fcntl(fd,F_GETFL,0),yes=1;
        if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0){close(fd);continue;}
        setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
        connection *c=calloc(1,sizeof(*c));if(!c){close(fd);continue;}c->server=s;c->fd=fd;
        pthread_mutex_lock(&s->mutex);
        if(s->stopping||s->count==HTTP_QUEUE)connection_close(c);
        else{s->queue[s->tail]=c;s->tail=(s->tail+1)%HTTP_QUEUE;++s->count;pthread_cond_signal(&s->ready);}
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
static int is_loopback(const struct sockaddr *addr){
    if(addr->sa_family==AF_INET)return (ntohl(((const struct sockaddr_in*)addr)->sin_addr.s_addr)&0xff000000u)==0x7f000000u;
    if(addr->sa_family==AF_INET6)return IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6*)addr)->sin6_addr);
    return 0;
}
static int listener(const jipc_config *config,int secure,unsigned number){
    char port[16];snprintf(port,sizeof(port),"%u",number);struct addrinfo hints={0},*addresses=NULL;
    hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_NUMERICSERV;
    if(getaddrinfo(config->bind,port,&hints,&addresses))return -1;
    int fd=-1;
    for(struct addrinfo *a=addresses;a;a=a->ai_next){
        if(!secure&&!config->allow_plain_http&&!is_loopback(a->ai_addr))continue;
        fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(fd<0)continue;
        int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        if(a->ai_family==AF_INET6)setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof(yes));
        if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0||bind(fd,a->ai_addr,a->ai_addrlen)<0||listen(fd,HTTP_QUEUE)<0){close(fd);fd=-1;continue;}
        int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);fd=-1;continue;}break;
    }
    freeaddrinfo(addresses);return fd;
}
static void destroy_server(http_server *s){
#ifdef JIPC_WITH_MBEDTLS
    tls_free(s);pthread_mutex_destroy(&s->random_mutex);
#endif
    if(s->wake[0]>=0)close(s->wake[0]);
    if(s->wake[1]>=0)close(s->wake[1]);
    pthread_cond_destroy(&s->ready);pthread_mutex_destroy(&s->mutex);free(s);
}
int jipc_http_start(jipc_app *app){
    if(app->http_started||app->http_state)return -1;
    jipc_config config;jipc_config_copy(app,&config);
    if(!!config.tls_cert[0]!=!!config.tls_key[0]){jipc_log("error","HTTPS requires both tls_cert and tls_key");return -1;}
    http_server *s=calloc(1,sizeof(*s));if(!s)return -1;s->app=app;s->listener=s->ui_listener=-1;s->wake[0]=s->wake[1]=-1;
    if(pthread_mutex_init(&s->mutex,NULL)){free(s);return -1;}
    if(pthread_cond_init(&s->ready,NULL)){pthread_mutex_destroy(&s->mutex);free(s);return -1;}
#ifdef JIPC_WITH_MBEDTLS
    if(pthread_mutex_init(&s->random_mutex,NULL)){pthread_cond_destroy(&s->ready);pthread_mutex_destroy(&s->mutex);free(s);return -1;}
    if(config.tls_cert[0]&&tls_setup(s,&config)<0){jipc_log("error","Unable to load HTTPS certificate/private key or initialize TLS");destroy_server(s);return -1;}
#else
    if(config.tls_cert[0]){jipc_log("error","Configured HTTPS requires a build with JIPC_WITH_MBEDTLS");destroy_server(s);return -1;}
#endif
    if(pipe2(s->wake,O_CLOEXEC|O_NONBLOCK)){destroy_server(s);return -1;}
    s->listener=listener(&config,s->secure,config.http_port);if(s->listener<0){jipc_log("error","HTTP bind failed; LAN plaintext requires allow_plain_http");destroy_server(s);return -1;}
    /* The admin UI also answers on the standard HTTPS port, so its address
     * needs no port (NVRs keep http_port). JIPC_HTTPS_UI_PORT (tests; 0 = off)
     * is an environment hook, never a camera.ini key. A failed bind is not
     * fatal: ONVIF on http_port must come up regardless. */
    const char *ui_env=getenv("JIPC_HTTPS_UI_PORT");long ui_port=ui_env&&*ui_env?strtol(ui_env,NULL,10):443;
    if(s->secure&&ui_port>0&&ui_port<65536&&(unsigned)ui_port!=config.http_port){
        s->ui_listener=listener(&config,1,(unsigned)ui_port);
        if(s->ui_listener<0)jipc_log("error","HTTPS UI bind on port %ld failed; the UI stays on port %u",ui_port,config.http_port);
        else s->ui_port=(unsigned)ui_port;
    }
    for(unsigned i=0;i<HTTP_WORKERS;++i)s->active[i]=-1;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,512*1024);
    unsigned workers=config.max_clients<HTTP_WORKERS?config.max_clients:HTTP_WORKERS;if(!workers)workers=1;
    for(unsigned i=0;i<workers;++i){worker_arg *a=malloc(sizeof(*a));if(!a)break;*a=(worker_arg){s,i};if(pthread_create(&s->worker[i],&attr,worker_main,a)){free(a);break;}++s->started;}
    pthread_attr_destroy(&attr);
    if(s->started!=workers||pthread_create(&app->http_thread,NULL,server_main,s)){
        pthread_mutex_lock(&s->mutex);s->stopping=1;pthread_cond_broadcast(&s->ready);pthread_mutex_unlock(&s->mutex);
        for(unsigned i=0;i<s->started;++i)pthread_join(s->worker[i],NULL);
        close(s->listener);if(s->ui_listener>=0)close(s->ui_listener);destroy_server(s);return -1;
    }
    app->http_state=s;app->http_started=1;jipc_log("info","%s listening on %s:%u",s->secure?"HTTPS":"HTTP",config.bind,config.http_port);
    if(s->ui_port)jipc_log("info","HTTPS admin UI also on %s:%u",config.bind,s->ui_port);
    return 0;
}
void jipc_http_stop(jipc_app *app){
    http_server *s=app->http_state;if(!s)return;
    pthread_mutex_lock(&s->mutex);s->stopping=1;shutdown(s->listener,SHUT_RDWR);if(s->ui_listener>=0)shutdown(s->ui_listener,SHUT_RDWR);
    while(s->count){connection_close(s->queue[s->head]);s->head=(s->head+1)%HTTP_QUEUE;--s->count;}
    for(unsigned i=0;i<HTTP_IDLE;++i){connection_close(s->idle[i]);s->idle[i]=NULL;}
    wake_server(s);
    for(unsigned i=0;i<s->started;++i)if(s->active[i]>=0)shutdown(s->active[i],SHUT_RDWR);
    pthread_cond_broadcast(&s->ready);pthread_mutex_unlock(&s->mutex);
    if(app->http_started)pthread_join(app->http_thread,NULL);
    for(unsigned i=0;i<s->started;++i)pthread_join(s->worker[i],NULL);
    close(s->listener);if(s->ui_listener>=0)close(s->ui_listener);app->http_state=NULL;app->http_started=0;destroy_server(s);
}
