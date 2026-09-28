#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
} Conn;
static JoanConfig G;
/* One physical command order shared by ONVIF moves, presets, Stop retries. */
static pthread_mutex_t ptz_lock=PTHREAD_MUTEX_INITIALIZER;
static int ptz_stop_uncertain;
static uint64_t ptz_preset_expires;
static uint64_t ptz_stop_retry_ms;
static uint64_t ptz_monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
static pthread_mutex_t worker_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_available=PTHREAD_COND_INITIALIZER;
static unsigned worker_count;
static int server_fd=-1;
static int server_stopping;
#define MAX_WORKERS 8u

static ssize_t cread(Conn*c,void*b,size_t n){return recv(c->fd,b,n,0);}
static ssize_t cwrite(Conn*c,const void*b,size_t n){return send(c->fd,b,n,MSG_NOSIGNAL);}
static int wall(Conn*c,const void*b,size_t n){const unsigned char*p=b;while(n){ssize_t z=cwrite(c,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return-1;p+=z;n-=z;}return 0;}
static const char *reason(int s){switch(s){case 200:return"OK";case 201:return"Created";case 202:return"Accepted";case 204:return"No Content";case 400:return"Bad Request";case 401:return"Unauthorized";case 403:return"Forbidden";case 404:return"Not Found";case 409:return"Conflict";case 413:return"Payload Too Large";case 415:return"Unsupported Media Type";case 428:return"Precondition Required";case 429:return"Too Many Requests";case 500:return"Internal Server Error";case 501:return"Not Implemented";case 502:return"Bad Gateway";case 503:return"Service Unavailable";default:return"Error";}}
static int response(Conn*c,int status,const char*type,const void*body,size_t len,const char*extra){char h[2300];int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'\r\nCache-Control: no-store\r\n%s\r\n",status,reason(status),type?type:"application/octet-stream",(unsigned long)len,extra?extra:"");return n<=0||(size_t)n>=sizeof(h)||wall(c,h,(size_t)n)||wall(c,body,len)?-1:0;}
static int json(Conn*c,int status,const char*s){return response(c,status,"application/json; charset=utf-8",s,strlen(s),NULL);}
static int error_json(Conn*c,int status,const char*code,const char*message){char b[512];snprintf(b,sizeof(b),"{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",code,message);return json(c,status,b);}
static void json_escape(const unsigned char*in,size_t n,char*out,size_t cap){size_t i,o=0;for(i=0;i<n&&o+7<cap;i++){unsigned char x=in[i];if(x=='"'||x=='\\'){out[o++]='\\';out[o++]=x;}else if(x>=32&&x<127)out[o++]=x;else{o+=snprintf(out+o,cap-o,"\\u%04x",x);} }out[o]=0;}

static int json_field(const unsigned char*body,size_t len,const char*key,char*out,size_t cap){char needle[96];const char*p,*e;size_t n;if(!body||snprintf(needle,sizeof(needle),"\"%s\"",key)>=(int)sizeof(needle))return-1;p=strstr((const char*)body,needle);if(!p||(size_t)(p-(const char*)body)>=len)return-1;p+=strlen(needle);while(*p==' '||*p=='\t')p++;if(*p++!=':')return-1;while(*p==' '||*p=='\t')p++;if(*p++!='"')return-1;e=p;while(*e&&*e!='"'&&(size_t)(e-(const char*)body)<len){if(*e=='\\')return-1;e++;}n=(size_t)(e-p);if(*e!='"'||n+1>cap)return-1;memcpy(out,p,n);out[n]=0;return 0;}
static int header_copy(const char*headers,const char*name,char*out,size_t cap){size_t nl=strlen(name);const char*p=headers;while(p&&*p){const char*e=strstr(p,"\r\n");size_t line=e?(size_t)(e-p):strlen(p);if(line>nl&&p[nl]==':'&&!strncasecmp(p,name,nl)){const char*v=p+nl+1;size_t n;while(v<p+line&&(*v==' '||*v=='\t'))v++;n=(size_t)((p+line)-v);if(n>=cap)n=cap-1;memcpy(out,v,n);out[n]=0;return 0;}if(!e)break;p=e+2;}return-1;}

/* Browsers send every cookie for the parent domain too, so the Cookie header
 * can exceed any fixed buffer. Scan the whole header(s) and keep only well-
 * formed joan_session candidates; unrelated cookies can never hide ours. */
static void session_cookies(const char*headers,char*out,size_t cap){const char*p=headers;size_t used=0;unsigned kept=0;out[0]=0;while(p&&*p){const char*e=strstr(p,"\r\n"),*end;size_t line=e?(size_t)(e-p):strlen(p);end=p+line;if(line>7&&!strncasecmp(p,"Cookie:",7)){const char*q=p+7;while(q<end){const char*s,*v;size_t n;while(q<end&&(*q==' '||*q=='\t'||*q==';'))q++;s=q;while(q<end&&*q!=';')q++;if((size_t)(q-s)==13+64&&!strncmp(s,"joan_session=",13)){v=s+13;for(n=0;n<64&&isxdigit((unsigned char)v[n]);n++){}if(n==64&&kept<8&&used+80<cap){used+=(size_t)snprintf(out+used,cap-used,"%sjoan_session=%.64s",used?"; ":"",v);kept++;}}}}if(!e)break;p=e+2;}}
static int parse_request(Conn*c,JoanRequest*r){unsigned char*h=malloc(JOAN_MAX_HEADERS+1),*end=NULL;size_t used=0,head,body_limit;char*line_end,*headers,*q;ssize_t n;int result=-1;if(!h)return-1;memset(r,0,sizeof(*r));r->fd=c->fd;while(used<JOAN_MAX_HEADERS){n=cread(c,h+used,JOAN_MAX_HEADERS-used);if(n<=0){if(!used)result=-3;goto fail;}used+=(size_t)n;h[used]=0;end=(unsigned char*)strstr((char*)h,"\r\n\r\n");if(end)break;}if(!end)goto fail;head=(size_t)(end-h)+4;line_end=strstr((char*)h,"\r\n");if(!line_end)goto fail;*line_end=0;headers=line_end+2;if(sscanf((char*)h,"%11s %511s",r->method,r->path)!=2)goto fail;q=strchr(r->path,'?');if(q){*q++=0;snprintf(r->query,sizeof(r->query),"%s",q);}*end=0;header_copy(headers,"Host",r->host,sizeof(r->host));session_cookies(headers,r->cookie,sizeof(r->cookie));header_copy(headers,"X-CSRF-Token",r->csrf,sizeof(r->csrf));header_copy(headers,"Content-Type",r->content_type,sizeof(r->content_type));header_copy(headers,"Origin",r->origin,sizeof(r->origin));header_copy(headers,"Transfer-Encoding",r->transfer_encoding,sizeof(r->transfer_encoding));if(!r->host[0]||r->transfer_encoding[0])goto fail;{char cl[32]={0};if(!header_copy(headers,"Content-Length",cl,sizeof(cl))){char*ep=NULL;unsigned long z=strtoul(cl,&ep,10);if(!ep||*ep)goto fail;r->content_length=(size_t)z;}}body_limit=!strcmp(r->method,"POST")&&!strcmp(r->path,"/api/v1/update")?JOAN_MAX_UPDATE_BODY:JOAN_MAX_BODY;if(r->content_length>body_limit){result=-2;goto fail;}if(r->content_length){size_t have=used-head,off=0;r->body=malloc(r->content_length+1);if(!r->body)goto fail;if(have>r->content_length)have=r->content_length;memcpy(r->body,end+4,have);off=have;while(off<r->content_length){n=cread(c,r->body+off,r->content_length-off);if(n<=0)goto fail;off+=(size_t)n;}r->body[r->content_length]=0;}free(h);return 0;fail:free(r->body);free(h);return result;}

static int host_ok(const char*host){char name[256],label[64],*colon;struct in_addr a4;size_t n;if(!host||(n=strlen(host))==0||n>=sizeof(name)||strpbrk(host,"/\\@\r\n"))return 0;snprintf(name,sizeof(name),"%s",host);if(name[0]=='['){char*end=strchr(name,']');struct in6_addr a6;if(!end)return 0;*end=0;if(inet_pton(AF_INET6,name+1,&a6)!=1)return 0;return IN6_IS_ADDR_LOOPBACK(&a6)||(a6.s6_addr[0]&0xfe)==0xfc;}colon=strrchr(name,':');if(colon)*colon=0;if(inet_pton(AF_INET,name,&a4)==1){uint32_t a=ntohl(a4.s_addr);return(a>>24)==127||(a>>24)==10||(a>>20)==0xac1||(a>>16)==0xc0a8;}joan_mdns_get_hostname(&G,label);{char wanted[80];snprintf(wanted,sizeof(wanted),"%s.local",label);return !strcasecmp(name,wanted);}}
static int origin_ok(const JoanRequest*r,int required){char expected[520];if(!r->origin[0])return required?0:1;if(!host_ok(r->host))return 0;snprintf(expected,sizeof(expected),"http://%s",r->host);return !strcmp(r->origin,expected);}
static int authorized(Conn*c,JoanRequest*r,JoanAuthz*a,int csrf){int rc;if(!origin_ok(r,0))return error_json(c,403,"origin_rejected","request origin is not this camera"),-1;rc=joan_auth_request(r,csrf,a);if(rc==-2)return error_json(c,403,"csrf_rejected","request token missing or invalid"),-1;if(rc)return error_json(c,401,"authentication_required","authentication required"),-1;return 0;}
static int helper_json(Conn*c,const char*op,const char*path,const char*id){unsigned char*out=NULL;size_t n=0;char esc[4096],body[4352];unsigned timeout=!strcmp(op,"firmware-verify")?60u:!strcmp(op,"wifi-stage")?65u:15u;int rc=joan_run_helper(&G,op,path,id,&out,&n,timeout);if(rc){free(out);return error_json(c,502,"integration_failed","local integration operation failed");}if(!strcmp(op,"ssh-list")){if(n>1800)n=1800;json_escape(out?out:(unsigned char*)"",n,esc,sizeof(esc));snprintf(body,sizeof(body),"{\"ok\":true,\"authorized_keys\":\"%s\"}",esc);}else snprintf(body,sizeof(body),"{\"ok\":true,\"id\":\"%s\"}",id?id:"");free(out);return json(c,200,body);}
/* 66491 goto; 66485/66489 save/update record the live step counter and
 * 66490 deletes: none may run while the head is still travelling, or a
 * preset is saved mid-move. Only a goto opens the travel window.
 * 0 sent; 1 the head is busy; -2 same command pending; -1 no channel. */
int joan_ptz_preset_request(unsigned command,const char*payload,char operation[65]){int rc;
    if(command!=66491&&command!=66485&&command!=66489&&command!=66490)return joan_mqtt_request(command,payload,operation);
    pthread_mutex_lock(&ptz_lock);
    if(ptz_stop_uncertain||ptz_preset_expires>ptz_monotonic_ms())rc=1;
    else{rc=joan_mqtt_request(command,payload,operation);if(!rc&&command==66491)ptz_preset_expires=ptz_monotonic_ms()+20000;}
    pthread_mutex_unlock(&ptz_lock);
    return rc;
}
static int private_routes(const char*input,char*out,size_t cap){char copy[1024],*save=NULL,*item;size_t used=0;if(!input||strlen(input)>=sizeof(copy))return-1;snprintf(copy,sizeof(copy),"%s",input);for(item=strtok_r(copy,", ", &save);item;item=strtok_r(NULL,", ",&save)){char*slash=strchr(item,'/');unsigned prefix;struct in_addr a4;struct in6_addr a6;int ok=0;if(!slash)return-1;*slash++=0;prefix=(unsigned)strtoul(slash,NULL,10);if(inet_pton(AF_INET,item,&a4)==1&&prefix>=8&&prefix<=32){uint32_t a=ntohl(a4.s_addr);ok=((a>>24)==10)||((a>>20)==0xac1)||((a>>16)==0xc0a8);}else if(inet_pton(AF_INET6,item,&a6)==1&&prefix>=7&&prefix<=128)ok=(a6.s6_addr[0]&0xfe)==0xfc;if(!ok)return-1;{int n=snprintf(out+used,cap-used,"%s/%u\n",item,prefix);if(n<=0||(size_t)n>=cap-used)return-1;used+=(size_t)n;}}return used?0:-1;}
static unsigned release_sequence(void){unsigned char*raw=NULL;size_t i,n=0;uint64_t value=0;if(!G.release_sequence_path[0]||joan_read_file(G.release_sequence_path,&raw,&n,32)||!n)goto done;if(raw[n-1]=='\n')n--;if(!n)goto done;for(i=0;i<n;i++){if(raw[i]<'0'||raw[i]>'9'){value=0;goto done;}value=value*10u+(unsigned)(raw[i]-'0');if(value>0xffffffffu){value=0;goto done;}}done:free(raw);return(unsigned)value;}

static int ptz_stop_locked(void)
{
    int rc=joan_oem_ptz(&G,NULL,0);
    if(rc){ptz_stop_uncertain=1;ptz_stop_retry_ms=ptz_monotonic_ms()+1000;}
    else ptz_stop_uncertain=0;
    return rc;
}
static void *ptz_expiry_loop(void *unused)
{
    (void)unused;
    for(;;){struct timespec delay={0,250000000L};nanosleep(&delay,NULL);
        pthread_mutex_lock(&ptz_lock);
        if(ptz_stop_uncertain&&ptz_monotonic_ms()>=ptz_stop_retry_ms)
            (void)ptz_stop_locked();
        pthread_mutex_unlock(&ptz_lock);
    }
    return NULL;
}

/* One ONVIF press is one coarse nudge, timed here instead of by the
 * client's Stop: the live picture lags seconds behind, so a hold cannot be
 * aimed, and a fixed step lands the same every time. 0 moved and stopped;
 * 1 the head is busy; -1 the move or stop was not confirmed. */
int joan_ptz_nudge(const char*direction)
{
    int rc=1;
    pthread_mutex_lock(&ptz_lock);
    if(!ptz_stop_uncertain&&ptz_preset_expires<=ptz_monotonic_ms()){
        if(joan_oem_ptz(&G,direction,3)){ptz_stop_uncertain=1;ptz_stop_retry_ms=ptz_monotonic_ms();rc=-1;}
        else{struct timespec step={0,350000000L};nanosleep(&step,NULL);rc=ptz_stop_locked()?-1:0;}
    }
    pthread_mutex_unlock(&ptz_lock);
    return rc;
}

static int static_file(Conn*c,const char*url){char path[600];unsigned char*data;size_t n;const char*type="application/octet-stream";if(strstr(url,".."))return error_json(c,404,"asset_not_found","asset not found");if(!strcmp(url,"/"))url="/index.html";if(snprintf(path,sizeof(path),"%s%s",G.web_dir,url)>=(int)sizeof(path)||joan_read_file(path,&data,&n,2*1024*1024))return error_json(c,404,"asset_not_found","asset not found");if(strstr(path,".html"))type="text/html; charset=utf-8";else if(strstr(path,".css"))type="text/css; charset=utf-8";else if(strstr(path,".js"))type="application/javascript; charset=utf-8";else if(strstr(path,".webmanifest"))type="application/manifest+json";else if(strstr(path,".svg"))type="image/svg+xml";response(c,200,type,data,n,"Cache-Control: public, max-age=300\r\n");free(data);return 0;}

/* Run an integration op and return its JSON output verbatim (like routes-list). */
static int helper_passthrough(Conn*c,const char*op,const char*path,const char*id){unsigned char*out=NULL;size_t n=0;if(joan_run_helper(&G,op,path,id,&out,&n,15)||!n||(out[0]!='{'&&out[0]!='[')){free(out);return error_json(c,502,"integration_failed","local integration operation failed");}response(c,200,"application/json; charset=utf-8",out,n,NULL);free(out);return 0;}
static int route(Conn*c,JoanRequest*r){JoanAuthz a;char x[256],y[256],id[65],path[512],body[1536];
    /* Keep the implementation names private while exposing the documented,
     * versioned contract. The short aliases remain for the bundled early UI. */
    if(!strcmp(r->path,"/api/v1/session")){
        if(!strcmp(r->method,"POST"))snprintf(r->path,sizeof(r->path),"/api/login");
        else if(!strcmp(r->method,"DELETE"))snprintf(r->path,sizeof(r->path),"/api/logout");
        else snprintf(r->path,sizeof(r->path),"/api/session-info");
    }else if(!strcmp(r->path,"/api/v1/setup/password"))snprintf(r->path,sizeof(r->path),"/api/password");
    else if(!strcmp(r->path,"/api/v1/status"))snprintf(r->path,sizeof(r->path),"/api/status");
    else if(!strcmp(r->path,"/api/v1/network/mdns"))snprintf(r->path,sizeof(r->path),"/api/mdns");
    else if(!strcmp(r->path,"/api/v1/network/routes"))snprintf(r->path,sizeof(r->path),"/api/routes");
    else if(!strcmp(r->path,"/api/v1/network/quality"))snprintf(r->path,sizeof(r->path),"/api/network-quality");
    else if(!strcmp(r->path,"/api/v1/time"))snprintf(r->path,sizeof(r->path),"/api/time");
    else if(!strcmp(r->path,"/api/v1/timezone"))snprintf(r->path,sizeof(r->path),"/api/timezone");
    else if(!strcmp(r->path,"/api/v1/network/wifi"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/wifi/stage":!strcmp(r->method,"PUT")?"/api/wifi/commit":"/api/wifi/rollback");
    else if(!strcmp(r->path,"/api/v1/streams"))snprintf(r->path,sizeof(r->path),"/api/streams");
    else if(!strcmp(r->path,"/api/v1/ssh/authorized-keys"))snprintf(r->path,sizeof(r->path),"/api/ssh/keys");
    else if(!strcmp(r->path,"/api/v1/update"))snprintf(r->path,sizeof(r->path),!strcmp(r->method,"POST")?"/api/firmware/stage":"/api/firmware/apply");
    if(!strcmp(r->path,"/api/v1/setup/status")){
        snprintf(body,sizeof(body),"{\"state\":\"ready\",\"setup_required\":false,\"default_password_warning\":%s,\"temporary_user\":\"admin\",\"release_version\":\"%s\",\"release_sequence\":%u}",joan_auth_setup_required(&G)?"true":"false",JOAN_VERSION,release_sequence());
        return json(c,200,body);
    }
    if(!strcmp(r->path,"/api/health"))return json(c,200,"{\"ok\":true}");
    if(!strcmp(r->path,"/api/login")&&!strcmp(r->method,"POST")){int rc;if(json_field(r->body,r->content_length,"username",x,sizeof(x))||json_field(r->body,r->content_length,"password",y,sizeof(y)))return error_json(c,400,"invalid_json","username and password are required");rc=joan_auth_login(&G,r->remote,x,y,&a);memset(y,0,sizeof(y));if(rc==-2)return error_json(c,429,"rate_limited","try later");if(rc)return error_json(c,401,"invalid_credentials","invalid credentials");snprintf(body,sizeof(body),"{\"ok\":true,\"csrf\":\"%s\",\"default_password_warning\":%s}",a.csrf,a.must_change?"true":"false");snprintf(x,sizeof(x),"Set-Cookie: joan_session=%s; Path=/; HttpOnly; SameSite=Strict\r\n",a.token);return response(c,200,"application/json",body,strlen(body),x);}
    if(authorized(c,r,&a,strcmp(r->method,"GET")&&strcmp(r->method,"HEAD")))return 0;
    if(!strcmp(r->path,"/api/status")){unsigned char*routes=NULL;size_t routes_len=0;const char*routes_json="{\"cidrs\":[]}";if(!joan_run_helper(&G,"routes-list",NULL,NULL,&routes,&routes_len,2)&&routes_len&&routes_len<512&&routes[0]=='{')routes_json=(char*)routes;snprintf(body,sizeof(body),"{\"version\":\"%s\",\"state\":\"ready\",\"local_only\":true,\"active_routes\":%s,\"default_password_warning\":%s,\"ssh_password_sync\":%s,\"rtsp_password_sync\":%s,\"features\":{\"rtsp\":true,\"ptz\":\"onvif\",\"ssh\":\"password-synchronized\",\"mdns\":\"%s\"},\"mqtt_bridge\":\"%s\",\"https_cloud_bridge\":{\"supported\":false,\"blocker\":\"OEM trust/pinning contract not recovered\"}}",JOAN_VERSION,routes_json,a.must_change?"true":"false",joan_auth_ssh_synchronized(&G)?"true":"false",joan_auth_rtsp_synchronized(&G)?"true":"false",joan_mdns_status(),joan_mqtt_bridge_status());free(routes);return json(c,200,body);}
    if(!strcmp(r->path,"/api/time")&&!strcmp(r->method,"PUT")){char epoch[24];char*ep=NULL;long long v;if(json_field(r->body,r->content_length,"epoch",epoch,sizeof(epoch)))return error_json(c,400,"invalid_json","epoch required");v=strtoll(epoch,&ep,10);if(!ep||*ep||v<1577836800LL||v>4102444800LL)return error_json(c,400,"invalid_time","out of range");if(joan_run_helper(&G,"time-set",NULL,epoch,NULL,NULL,10))return error_json(c,502,"time_set_failed","clock set failed");snprintf(body,sizeof(body),"{\"ok\":true,\"epoch\":%lld}",(long long)time(NULL));return json(c,200,body);}
    if(!strcmp(r->path,"/api/timezone")&&!strcmp(r->method,"PUT")){char gmt[16],s;int hh,mm,n=0;if(json_field(r->body,r->content_length,"gmt_tz",gmt,sizeof(gmt)))return error_json(c,400,"invalid_json","gmt_tz required");if(sscanf(gmt,"GMT%c%2d:%2d%n",&s,&hh,&mm,&n)!=3||n!=9||gmt[9]||(s!='+'&&s!='-')||hh>14||mm>59)return error_json(c,400,"invalid_timezone","bad gmt_tz");if(joan_run_helper(&G,"timezone-set",NULL,gmt,NULL,NULL,10))return error_json(c,502,"timezone_set_failed","set failed");snprintf(body,sizeof(body),"{\"ok\":true,\"gmt_tz\":\"%s\"}",gmt);return json(c,200,body);}
    if(!strcmp(r->path,"/api/session-info")){snprintf(body,sizeof(body),"{\"authenticated\":true,\"csrf\":\"%s\",\"default_password_warning\":%s}",a.csrf,a.must_change?"true":"false");return json(c,200,body);}
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
    if(!strcmp(r->path,"/api/streams")&&!strcmp(r->method,"GET")){char rtsp_host[256];snprintf(rtsp_host,sizeof(rtsp_host),"%s",r->host);if(rtsp_host[0]=='['){char*end=strchr(rtsp_host,']');if(end)end[1]=0;}else{char*port=strrchr(rtsp_host,':');if(port)*port=0;}snprintf(body,sizeof(body),"{\"streams\":[{\"id\":\"main\",\"rtsp\":\"rtsp://%s/live/ch0\",\"width\":2304,\"height\":1296},{\"id\":\"sub\",\"rtsp\":\"rtsp://%s/live/ch1\",\"width\":640,\"height\":360}]}",rtsp_host,rtsp_host);return json(c,200,body);}
    if(!strcmp(r->path,"/api/wifi/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"configuration required\"}");if(joan_stage_blob(&G,"wifi",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"wifi-stage",path,id);}
    if(!strcmp(r->path,"/api/wifi/commit")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-commit",NULL,id);}
    if(!strcmp(r->path,"/api/wifi/rollback")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"DELETE"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"wifi-rollback",NULL,id);}
    if(!strcmp(r->path,"/api/mdns")&&!strcmp(r->method,"GET")){joan_mdns_get_hostname(&G,x);snprintf(body,sizeof(body),"{\"hostname\":\"%s\",\"address\":\"%s.local\",\"status\":\"%s\"}",x,x,joan_mdns_status());return json(c,200,body);}
    if(!strcmp(r->path,"/api/mdns")&&(!strcmp(r->method,"PUT")||!strcmp(r->method,"POST"))){if(json_field(r->body,r->content_length,"hostname",x,sizeof(x))||joan_mdns_set_hostname(&G,x))return error_json(c,400,"invalid_hostname","hostname must be a 1-63 character DNS label");joan_mdns_get_configured_hostname(&G,x);snprintf(body,sizeof(body),"{\"ok\":true,\"hostname\":\"%s\",\"address\":\"%s.local\",\"restart_required\":true}",x,x);return json(c,200,body);}
    if(!strcmp(r->path,"/api/routes")&&!strcmp(r->method,"GET")){unsigned char*out=NULL;size_t n=0;if(joan_run_helper(&G,"routes-list",NULL,NULL,&out,&n,5)||!n||(out[0]!='['&&out[0]!='{')){free(out);return error_json(c,502,"routes_unavailable","route policy unavailable");}response(c,200,"application/json; charset=utf-8",out,n,NULL);free(out);return 0;}
    if(!strcmp(r->path,"/api/network-quality")&&!strcmp(r->method,"GET"))return helper_passthrough(c,"network-quality",NULL,NULL);
    if(!strcmp(r->path,"/api/routes")&&!strcmp(r->method,"PUT")){char cidrs[1024],canonical[1200];if(json_field(r->body,r->content_length,"cidrs",cidrs,sizeof(cidrs))||private_routes(cidrs,canonical,sizeof(canonical)))return error_json(c,400,"invalid_routes","only RFC1918 and ULA CIDRs are allowed");if(joan_stage_blob(&G,"routes",canonical,strlen(canonical),id,path))return error_json(c,500,"stage_failed","could not stage routes");return helper_json(c,"routes-set",path,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"GET"))return helper_json(c,"ssh-list",NULL,NULL);
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"POST")){if(!r->body||r->content_length>16384)return json(c,400,"{\"error\":\"invalid key\"}");if(joan_stage_blob(&G,"ssh-key",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"ssh-add",path,id);}
    if(!strcmp(r->path,"/api/ssh/keys")&&!strcmp(r->method,"DELETE")){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"ssh-delete",NULL,id);}
    if(!strcmp(r->path,"/api/firmware/stage")&&!strcmp(r->method,"POST")){if(!r->body||!r->content_length)return json(c,400,"{\"error\":\"image required\"}");if(joan_stage_blob(&G,"firmware",r->body,r->content_length,id,path))return json(c,500,"{\"error\":\"stage failed\"}");return helper_json(c,"firmware-verify",path,id);}
    if(!strcmp(r->path,"/api/firmware/apply")&&(!strcmp(r->method,"POST")||!strcmp(r->method,"PUT"))){if(json_field(r->body,r->content_length,"id",id,sizeof(id)))return json(c,400,"{\"error\":\"id required\"}");return helper_json(c,"firmware-apply",NULL,id);}
    return error_json(c,404,"route_not_found","route not found");
}

/* ONVIF clients authenticate each SOAP request in its own header (onvif.c),
 * so no cookie or CSRF token applies. A page from another origin is still
 * refused, and service addresses are only built from a Host this camera
 * answers to. */
static void onvif_route(Conn*c,JoanRequest*r){char host[256],*reply=NULL;size_t n=0;int status;
    if(r->origin[0]&&!origin_ok(r,0)){error_json(c,403,"origin_rejected","request origin is not this camera");return;}
    if(host_ok(r->host))snprintf(host,sizeof(host),"%s",r->host);
    else{struct sockaddr_in a;socklen_t l=sizeof(a);char ip[INET_ADDRSTRLEN];if(getsockname(c->fd,(struct sockaddr*)&a,&l)||!inet_ntop(AF_INET,&a.sin_addr,ip,sizeof(ip)))snprintf(ip,sizeof(ip),"127.0.0.1");snprintf(host,sizeof(host),"%s:%u",ip,G.port);}
    status=joan_onvif_handle(&G,r,host,&reply,&n);
    if(status<0){error_json(c,500,"onvif_failed","ONVIF response could not be built");return;}
    response(c,status,"application/soap+xml; charset=utf-8",reply,n,NULL);free(reply);}
static void serve(Conn*c,const char*remote){JoanRequest r;int parsed=parse_request(c,&r);if(parsed){if(parsed==-3)return;if(parsed==-2)error_json(c,413,"payload_too_large","request body exceeds route limit");else error_json(c,400,"bad_request","malformed request or unsupported transfer encoding");return;}snprintf(r.remote,sizeof(r.remote),"%s",remote);if(!strncmp(r.path,"/onvif/",7))onvif_route(c,&r);else if(!strncmp(r.path,"/api/",5))route(c,&r);else static_file(c,r.path);if(r.body){memset(r.body,0,r.content_length);free(r.body);}}

typedef struct {
    int fd;
    char remote[64];
} Worker;

static void *serve_worker(void *argument)
{
    Worker *worker=argument;Conn c;struct timeval io_timeout={10,0};
    memset(&c,0,sizeof(c));c.fd=worker->fd;
    setsockopt(c.fd,SOL_SOCKET,SO_RCVTIMEO,&io_timeout,sizeof(io_timeout));
    setsockopt(c.fd,SOL_SOCKET,SO_SNDTIMEO,&io_timeout,sizeof(io_timeout));
    serve(&c,worker->remote);
    close(c.fd);pthread_mutex_lock(&worker_lock);if(worker_count)worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);free(worker);return NULL;
}

int joan_server_run(const JoanConfig*cfg){int s,one=1;struct sockaddr_in a;pthread_t ptz_thread;G=*cfg;pthread_mutex_lock(&worker_lock);server_stopping=0;pthread_mutex_unlock(&worker_lock);
    if(pthread_create(&ptz_thread,NULL,ptz_expiry_loop,NULL))return-1;
    pthread_detach(ptz_thread);
    s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return-1;server_fd=s;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)cfg->port);if(inet_pton(AF_INET,cfg->bind_addr,&a.sin_addr)!=1||bind(s,(struct sockaddr*)&a,sizeof(a))||listen(s,8)){close(s);server_fd=-1;return-1;}
    for(;;){struct sockaddr_in peer;socklen_t pl=sizeof(peer);int fd=accept(s,(struct sockaddr*)&peer,&pl);Worker*w;pthread_t t;if(fd<0){if(errno==EINTR)continue;break;}pthread_mutex_lock(&worker_lock);while(worker_count>=MAX_WORKERS&&!server_stopping)pthread_cond_wait(&worker_available,&worker_lock);if(server_stopping){pthread_mutex_unlock(&worker_lock);close(fd);break;}worker_count++;pthread_mutex_unlock(&worker_lock);w=calloc(1,sizeof(*w));if(!w){pthread_mutex_lock(&worker_lock);worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);close(fd);continue;}w->fd=fd;inet_ntop(AF_INET,&peer.sin_addr,w->remote,sizeof(w->remote));
        if(pthread_create(&t,NULL,serve_worker,w)){pthread_mutex_lock(&worker_lock);worker_count--;pthread_cond_signal(&worker_available);pthread_mutex_unlock(&worker_lock);close(fd);free(w);continue;}pthread_detach(t);
    }close(s);server_fd=-1;return 0;
}
void joan_server_stop(void){int fd;pthread_mutex_lock(&worker_lock);server_stopping=1;fd=server_fd;pthread_cond_broadcast(&worker_available);pthread_mutex_unlock(&worker_lock);if(fd>=0)shutdown(fd,SHUT_RDWR);}
