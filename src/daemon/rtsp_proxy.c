#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef JOAN_NO_TLS
#include <mbedtls/md5.h>
#endif

#define RTSP_REALM "joan-rtsp"
#define RTSP_HEADER_MAX 8192U
#define RTSP_IO_BUFFER 4096U
#define RTSP_MAX_WORKERS 8U
#define RTSP_NONCES 32U
#define RTSP_NONCE_SECONDS 60
#define RTSP_AUTH_ATTEMPTS 3U

#ifdef JOAN_NO_TLS
typedef struct { uint32_t h[4], bits[2]; unsigned char data[64]; size_t used; } Md5;
#endif
typedef struct { char value[33]; time_t expires; int used; } Nonce;
typedef struct { int client, upstream; unsigned slot; } Worker;
typedef struct {
    char username[32], realm[64], nonce[64], uri[1024], response[40];
    char algorithm[16], qop[16], nc[16], cnonce[160];
    unsigned seen;
} Digest;
typedef struct {
    int listener, stopping, running;
    unsigned workers;
    int clients[RTSP_MAX_WORKERS], upstreams[RTSP_MAX_WORKERS];
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t idle;
    JoanConfig cfg;
    Nonce nonces[RTSP_NONCES];
    unsigned nonce_cursor;
} Proxy;

static Proxy P={
    .listener=-1,
    .clients={-1,-1,-1,-1,-1,-1,-1,-1},
    .upstreams={-1,-1,-1,-1,-1,-1,-1,-1},
    .lock=PTHREAD_MUTEX_INITIALIZER,
    .idle=PTHREAD_COND_INITIALIZER
};

#ifdef JOAN_NO_TLS
static uint32_t rol(uint32_t x,unsigned n){return(x<<n)|(x>>(32-n));}
static void md5_block(Md5*m,const unsigned char*p)
{
    static const uint32_t k[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const unsigned char s[64]={
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t w[16],a=m->h[0],b=m->h[1],c=m->h[2],d=m->h[3],f,g,t;
    unsigned i;
    for(i=0;i<16;i++)w[i]=(uint32_t)p[i*4]|((uint32_t)p[i*4+1]<<8)|
        ((uint32_t)p[i*4+2]<<16)|((uint32_t)p[i*4+3]<<24);
    for(i=0;i<64;i++){
        if(i<16){f=(b&c)|(~b&d);g=i;}
        else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)&15;}
        else if(i<48){f=b^c^d;g=(3*i+5)&15;}
        else{f=c^(b|~d);g=(7*i)&15;}
        t=d;d=c;c=b;b=b+rol(a+f+k[i]+w[g],s[i]);a=t;
    }
    m->h[0]+=a;m->h[1]+=b;m->h[2]+=c;m->h[3]+=d;
}
static void md5_init(Md5*m){m->h[0]=0x67452301;m->h[1]=0xefcdab89;m->h[2]=0x98badcfe;m->h[3]=0x10325476;m->bits[0]=m->bits[1]=0;m->used=0;}
static void md5_update(Md5*m,const void*data,size_t len)
{
    const unsigned char*p=data;uint32_t old=m->bits[0];size_t n;
    m->bits[0]+=(uint32_t)(len<<3);if(m->bits[0]<old)m->bits[1]++;m->bits[1]+=(uint32_t)(len>>29);
    while(len){n=64-m->used;if(n>len)n=len;memcpy(m->data+m->used,p,n);m->used+=n;p+=n;len-=n;if(m->used==64){md5_block(m,m->data);m->used=0;}}
}
static void md5_final(Md5*m,unsigned char out[16])
{
    unsigned char tail[72]={0x80};uint32_t lo=m->bits[0],hi=m->bits[1];size_t pad=m->used<56?56-m->used:120-m->used;unsigned i;
    md5_update(m,tail,pad);for(i=0;i<4;i++){tail[i]=(unsigned char)(lo>>(8*i));tail[i+4]=(unsigned char)(hi>>(8*i));}md5_update(m,tail,8);
    for(i=0;i<4;i++){out[i*4]=(unsigned char)m->h[i];out[i*4+1]=(unsigned char)(m->h[i]>>8);out[i*4+2]=(unsigned char)(m->h[i]>>16);out[i*4+3]=(unsigned char)(m->h[i]>>24);}memset(m,0,sizeof(*m));
}
#endif
static void md5_parts(const char*a,const char*b,const char*c,const char*d,const char*e,unsigned char out[16])
{
#ifdef JOAN_NO_TLS
    Md5 m;md5_init(&m);md5_update(&m,a,strlen(a));if(b)md5_update(&m,b,strlen(b));if(c)md5_update(&m,c,strlen(c));if(d)md5_update(&m,d,strlen(d));if(e)md5_update(&m,e,strlen(e));md5_final(&m,out);
#else
    mbedtls_md5_context m;mbedtls_md5_init(&m);(void)mbedtls_md5_starts_ret(&m);(void)mbedtls_md5_update_ret(&m,(const unsigned char*)a,strlen(a));if(b)(void)mbedtls_md5_update_ret(&m,(const unsigned char*)b,strlen(b));if(c)(void)mbedtls_md5_update_ret(&m,(const unsigned char*)c,strlen(c));if(d)(void)mbedtls_md5_update_ret(&m,(const unsigned char*)d,strlen(d));if(e)(void)mbedtls_md5_update_ret(&m,(const unsigned char*)e,strlen(e));(void)mbedtls_md5_finish_ret(&m,out);mbedtls_md5_free(&m);
#endif
}
static void hex16(const unsigned char in[16],char out[33]){static const char h[]="0123456789abcdef";unsigned i;for(i=0;i<16;i++){out[i*2]=h[in[i]>>4];out[i*2+1]=h[in[i]&15];}out[32]=0;}

static int send_all(int fd,const void*data,size_t len)
{
    const unsigned char*p=data;ssize_t n;
    while(len){n=send(fd,p,len,0);if(n>0){p+=n;len-=(size_t)n;continue;}if(n<0&&errno==EINTR)continue;return-1;}return 0;
}
static int response(int fd,int status,const char*reason,const char*cseq,const char*extra)
{
    char b[768];int n=snprintf(b,sizeof(b),"RTSP/1.0 %d %s\r\n%s%s%sContent-Length: 0\r\n\r\n",status,reason,cseq&&*cseq?"CSeq: ":"",cseq&&*cseq?cseq:"",cseq&&*cseq?"\r\n":"");
    if(n<=0||(size_t)n>=sizeof(b))return-1;
    if(extra&&*extra){char x[1024];int z=snprintf(x,sizeof(x),"RTSP/1.0 %d %s\r\n%s%s%s%sContent-Length: 0\r\n\r\n",status,reason,cseq&&*cseq?"CSeq: ":"",cseq&&*cseq?cseq:"",cseq&&*cseq?"\r\n":"",extra);if(z<=0||(size_t)z>=sizeof(x))return-1;return send_all(fd,x,(size_t)z);}
    return send_all(fd,b,(size_t)n);
}
static int ci_equal(const char*a,size_t n,const char*b){return strlen(b)==n&&!strncasecmp(a,b,n);}
static const char *header_end(const char*b,size_t n){size_t i;for(i=3;i<n;i++)if(b[i-3]=='\r'&&b[i-2]=='\n'&&b[i-1]=='\r'&&b[i]=='\n')return b+i+1;return NULL;}
static int read_header(int fd,char*b,size_t*used,size_t*header_len)
{
    const char*end;ssize_t n;
    for(;;){end=header_end(b,*used);if(end){*header_len=(size_t)(end-b);return 0;}if(*used==RTSP_HEADER_MAX)return-2;n=recv(fd,b+*used,RTSP_HEADER_MAX-*used,0);if(n>0){*used+=(size_t)n;continue;}if(n<0&&errno==EINTR)continue;return-1;}
}
static int request_meta(const char*b,size_t n,char method[32],char uri[1024],
                        char cseq[32],char authorization[2048])
{
    const char*line=b,*end,*colon;size_t z;int first=1;
    cseq[0]=0;authorization[0]=0;
    while(line<b+n){end=memchr(line,'\n',(size_t)(b+n-line));if(!end)return-1;if(end==line||end[-1]!='\r')return-1;z=(size_t)(end-line-1);
        if(first){char first_line[1400],version[16],tail;if(z>=sizeof(first_line))return-1;memcpy(first_line,line,z);first_line[z]=0;if(sscanf(first_line,"%31s %1023s %15s%c",method,uri,version,&tail)!=3||strcmp(version,"RTSP/1.0"))return-1;first=0;}
        else if(z==0)return 0;
        else{if(*line==' '||*line=='\t'||memchr(line,0,z))return-1;colon=memchr(line,':',z);if(!colon)return-1;{const char*v=colon+1;size_t vn=(size_t)(line+z-v);while(vn&&(*v==' '||*v=='\t')){v++;vn--;}while(vn&&(v[vn-1]==' '||v[vn-1]=='\t'))vn--;
            if(ci_equal(line,(size_t)(colon-line),"CSeq")){size_t i;if(cseq[0]||!vn||vn>=32)return-1;for(i=0;i<vn;i++)if(!isdigit((unsigned char)v[i]))return-1;memcpy(cseq,v,vn);cseq[vn]=0;}
            else if(ci_equal(line,(size_t)(colon-line),"Authorization")){if(authorization[0]||vn<7||vn>=2048||strncasecmp(v,"Digest ",7))return-1;memcpy(authorization,v,vn);authorization[vn]=0;}
            else if(ci_equal(line,(size_t)(colon-line),"Content-Length")){size_t i;if(vn!=1||v[0]!='0'){for(i=0;i<vn;i++)if(!isdigit((unsigned char)v[i]))return-1;return-1;}}
        }}line=end+1;
    }return-1;
}
static int copy_value(char*out,size_t cap,const char**cursor,const char*limit)
{
    const char*p=*cursor;size_t n=0;int quoted=0,closed=0;if(p<limit&&*p=='"'){quoted=1;p++;}
    while(p<limit){unsigned char c=(unsigned char)*p;if(quoted&&c=='"'){p++;closed=1;break;}if(!quoted&&(c==','||isspace(c)))break;if(c<' '||c==127||c=='\\')return-1;if(n+1>=cap)return-1;out[n++]=(char)c;p++;}
    if(quoted&&!closed)return-1;
    out[n]=0;*cursor=p;return n?0:-1;
}
static int digest_field(Digest*d,const char*key,size_t kn,char**out,size_t*cap)
{
    unsigned bit;
    if(ci_equal(key,kn,"username")){bit=1U<<0;*out=d->username;*cap=sizeof(d->username);}
    else if(ci_equal(key,kn,"realm")){bit=1U<<1;*out=d->realm;*cap=sizeof(d->realm);}
    else if(ci_equal(key,kn,"nonce")){bit=1U<<2;*out=d->nonce;*cap=sizeof(d->nonce);}
    else if(ci_equal(key,kn,"uri")){bit=1U<<3;*out=d->uri;*cap=sizeof(d->uri);}
    else if(ci_equal(key,kn,"response")){bit=1U<<4;*out=d->response;*cap=sizeof(d->response);}
    else if(ci_equal(key,kn,"algorithm")){bit=1U<<5;*out=d->algorithm;*cap=sizeof(d->algorithm);}
    else if(ci_equal(key,kn,"qop")){bit=1U<<6;*out=d->qop;*cap=sizeof(d->qop);}
    else if(ci_equal(key,kn,"nc")){bit=1U<<7;*out=d->nc;*cap=sizeof(d->nc);}
    else if(ci_equal(key,kn,"cnonce")){bit=1U<<8;*out=d->cnonce;*cap=sizeof(d->cnonce);}
    else{*out=NULL;*cap=0;return 0;}
    if(d->seen&bit)return-1;
    d->seen|=bit;
    return 0;
}
static int parse_digest(const char*value,const char*limit,Digest*d)
{
    const char*p=value+7,*key;char*out;size_t kn,cap;
    memset(d,0,sizeof(*d));
    while(p<limit){while(p<limit&&(isspace((unsigned char)*p)||*p==','))p++;if(p==limit)break;key=p;while(p<limit&&(isalnum((unsigned char)*p)||*p=='-'||*p=='_'))p++;kn=(size_t)(p-key);while(p<limit&&isspace((unsigned char)*p))p++;if(!kn||p==limit||*p!='=')return-1;p++;while(p<limit&&isspace((unsigned char)*p))p++;
        if(digest_field(d,key,kn,&out,&cap))return-1;
        if(out){if(copy_value(out,cap,&p,limit))return-1;}
        else{char ignored[256];if(copy_value(ignored,sizeof(ignored),&p,limit))return-1;}
        while(p<limit&&isspace((unsigned char)*p))p++;
        if(p<limit&&*p!=',')return-1;
    }
    return 0;
}
static int nonce_issue(char out[33])
{
    unsigned char random[16];char value[33];Nonce*n;if(joan_random(random,sizeof(random)))return-1;hex16(random,value);memset(random,0,sizeof(random));
    pthread_mutex_lock(&P.lock);n=&P.nonces[P.nonce_cursor++%RTSP_NONCES];memcpy(n->value,value,sizeof(n->value));n->expires=time(NULL)+RTSP_NONCE_SECONDS;n->used=0;pthread_mutex_unlock(&P.lock);memcpy(out,value,33);return 0;
}
static int nonce_known(const char*value,int consume)
{
    unsigned i;time_t now=time(NULL);int ok=0;pthread_mutex_lock(&P.lock);for(i=0;i<RTSP_NONCES;i++)if(!P.nonces[i].used&&P.nonces[i].expires>=now&&!strcmp(P.nonces[i].value,value)){ok=1;if(consume)P.nonces[i].used=1;break;}pthread_mutex_unlock(&P.lock);return ok;
}
static int hex_response(const char*s,unsigned char out[16])
{
    unsigned i,a,b;if(strlen(s)!=32)return-1;for(i=0;i<16;i++){a=(unsigned)(isdigit((unsigned char)s[i*2])?s[i*2]-'0':tolower((unsigned char)s[i*2])-'a'+10);b=(unsigned)(isdigit((unsigned char)s[i*2+1])?s[i*2+1]-'0':tolower((unsigned char)s[i*2+1])-'a'+10);if(a>15||b>15)return-1;out[i]=(unsigned char)((a<<4)|b);}return 0;
}
static int authenticate(const char*method,const char*uri,const char*authorization)
{
    Digest d;unsigned char*raw=NULL,ha1[16],ha2[16],want[16],got[16];size_t n=0;char path[512],a1[33],a2[33],combo[512],password[320];int z,ok=0;unsigned i;
    if(!authorization[0]||parse_digest(authorization,authorization+strlen(authorization),&d)||strcmp(d.username,"admin")||strcmp(d.realm,RTSP_REALM)||strcmp(d.uri,uri)||strcasecmp(d.qop,"auth")||strlen(d.nc)!=8||!d.cnonce[0]||!d.response[0]||(d.algorithm[0]&&strcasecmp(d.algorithm,"MD5"))||!nonce_known(d.nonce,0))return 0;
    for(i=0;i<8;i++)if(!isxdigit((unsigned char)d.nc[i]))return 0;
    if(snprintf(path,sizeof(path),"%s/rtsp.password",P.cfg.state_dir)>=(int)sizeof(path)||joan_read_file(path,&raw,&n,sizeof(password)-1))return 0;
    while(n&&(raw[n-1]=='\n'||raw[n-1]=='\r'))n--;
    if(!n||memchr(raw,'\n',n)||memchr(raw,'\r',n)){memset(raw,0,n);free(raw);return 0;}
    memcpy(password,raw,n);password[n]=0;memset(raw,0,n);free(raw);
    md5_parts("admin:",RTSP_REALM,":",password,NULL,ha1);md5_parts(method,":",uri,NULL,NULL,ha2);hex16(ha1,a1);hex16(ha2,a2);
    z=snprintf(combo,sizeof(combo),"%s:%s:%s:%s:auth:%s",a1,d.nonce,d.nc,d.cnonce,a2);if(z>0&&(size_t)z<sizeof(combo)){md5_parts(combo,NULL,NULL,NULL,NULL,want);if(!hex_response(d.response,got)&&joan_ct_equal(want,got,16)&&nonce_known(d.nonce,1))ok=1;}
    memset(password,0,sizeof(password));memset(ha1,0,sizeof(ha1));memset(want,0,sizeof(want));memset(got,0,sizeof(got));memset(combo,0,sizeof(combo));return ok;
}
static int challenge(int fd,const char*cseq)
{
    char nonce[33],extra[384];if(nonce_issue(nonce))return-1;snprintf(extra,sizeof(extra),"WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5, qop=\"auth\"\r\n",RTSP_REALM,nonce);return response(fd,401,"Unauthorized",cseq,extra);
}
static int upstream_connect(void)
{
    int fd=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a;struct timeval tv={5,0};if(fd<0)return-1;setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)P.cfg.rtsp_port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(connect(fd,(struct sockaddr*)&a,sizeof(a))){close(fd);return-1;}return fd;
}
static int relay(int a,int b)
{
    struct pollfd p[2];unsigned char data[RTSP_IO_BUFFER];ssize_t n;p[0].fd=a;p[1].fd=b;p[0].events=p[1].events=POLLIN;
    for(;;){p[0].revents=p[1].revents=0;do n=poll(p,2,-1);while(n<0&&errno==EINTR);if(n<=0)return-1;if(p[0].revents&(POLLERR|POLLHUP|POLLNVAL))return 0;if(p[1].revents&(POLLERR|POLLHUP|POLLNVAL))return 0;if(p[0].revents&POLLIN){n=recv(a,data,sizeof(data),0);if(n<=0)return 0;if(send_all(b,data,(size_t)n))return-1;}if(p[1].revents&POLLIN){n=recv(b,data,sizeof(data),0);if(n<=0)return 0;if(send_all(a,data,(size_t)n))return-1;}}
}
static void worker_done(Worker*w)
{
    if(w->upstream>=0)close(w->upstream);
    if(w->client>=0)close(w->client);
    pthread_mutex_lock(&P.lock);P.clients[w->slot]=-1;P.upstreams[w->slot]=-1;
    if(P.workers)P.workers--;
    pthread_cond_broadcast(&P.idle);pthread_mutex_unlock(&P.lock);free(w);
}
static void *worker_main(void*opaque)
{
    Worker*w=opaque;char b[RTSP_HEADER_MAX],method[32],uri[1024],cseq[32],auth[2048];size_t used=0,hn=0;unsigned attempts=0;struct timeval tv={5,0};
    setsockopt(w->client,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(w->client,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    while(attempts++<RTSP_AUTH_ATTEMPTS){int rr=read_header(w->client,b,&used,&hn);if(rr){if(rr==-2)response(w->client,400,"Bad Request",NULL,NULL);goto done;}if(request_meta(b,hn,method,uri,cseq,auth)){response(w->client,400,"Bad Request",NULL,NULL);goto done;}if(!authenticate(method,uri,auth)){if(challenge(w->client,cseq))goto done;memmove(b,b+hn,used-hn);used-=hn;continue;}
        w->upstream=upstream_connect();if(w->upstream<0){response(w->client,503,"Service Unavailable",cseq,NULL);goto done;}pthread_mutex_lock(&P.lock);P.upstreams[w->slot]=w->upstream;pthread_mutex_unlock(&P.lock);if(send_all(w->upstream,b,used))goto done;tv.tv_sec=0;tv.tv_usec=0;setsockopt(w->client,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(w->upstream,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));(void)relay(w->client,w->upstream);goto done;}
done:worker_done(w);return NULL;
}
static void *listener_main(void*unused)
{
    (void)unused;for(;;){int fd=accept(P.listener,NULL,NULL);Worker*w;pthread_t t;unsigned slot;if(fd<0){if(errno==EINTR)continue;pthread_mutex_lock(&P.lock);if(P.stopping){pthread_mutex_unlock(&P.lock);break;}pthread_mutex_unlock(&P.lock);continue;}
        pthread_mutex_lock(&P.lock);if(P.stopping||P.workers>=RTSP_MAX_WORKERS){pthread_mutex_unlock(&P.lock);response(fd,453,"Not Enough Bandwidth",NULL,NULL);close(fd);continue;}for(slot=0;slot<RTSP_MAX_WORKERS&&P.clients[slot]>=0;slot++);if(slot==RTSP_MAX_WORKERS){pthread_mutex_unlock(&P.lock);close(fd);continue;}P.clients[slot]=fd;P.workers++;pthread_mutex_unlock(&P.lock);
        w=calloc(1,sizeof(*w));if(!w){pthread_mutex_lock(&P.lock);P.clients[slot]=-1;P.workers--;pthread_cond_broadcast(&P.idle);pthread_mutex_unlock(&P.lock);close(fd);continue;}w->client=fd;w->upstream=-1;w->slot=slot;if(pthread_create(&t,NULL,worker_main,w)){worker_done(w);continue;}pthread_detach(t);
    }return NULL;
}
int joan_rtsp_proxy_start(const JoanConfig*cfg)
{
    int fd,one=1;struct sockaddr_in a;unsigned i;if(!cfg||!cfg->rtsp_proxy_port)return 0;pthread_mutex_lock(&P.lock);if(P.running){pthread_mutex_unlock(&P.lock);return-1;}P.cfg=*cfg;P.stopping=0;P.workers=0;for(i=0;i<RTSP_MAX_WORKERS;i++)P.clients[i]=P.upstreams[i]=-1;memset(P.nonces,0,sizeof(P.nonces));P.nonce_cursor=0;pthread_mutex_unlock(&P.lock);
    fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return-1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)cfg->rtsp_proxy_port);if(inet_pton(AF_INET,cfg->bind_addr,&a.sin_addr)!=1||bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,RTSP_MAX_WORKERS)){close(fd);return-1;}pthread_mutex_lock(&P.lock);P.listener=fd;P.running=1;pthread_mutex_unlock(&P.lock);if(pthread_create(&P.thread,NULL,listener_main,NULL)){close(fd);pthread_mutex_lock(&P.lock);P.listener=-1;P.running=0;pthread_mutex_unlock(&P.lock);return-1;}return 0;
}
void joan_rtsp_proxy_stop(void)
{
    int listener,clients[RTSP_MAX_WORKERS],upstreams[RTSP_MAX_WORKERS];unsigned i;pthread_mutex_lock(&P.lock);if(!P.running){pthread_mutex_unlock(&P.lock);return;}P.stopping=1;listener=P.listener;for(i=0;i<RTSP_MAX_WORKERS;i++){clients[i]=P.clients[i];upstreams[i]=P.upstreams[i];}pthread_mutex_unlock(&P.lock);if(listener>=0)shutdown(listener,SHUT_RDWR);for(i=0;i<RTSP_MAX_WORKERS;i++){if(clients[i]>=0)shutdown(clients[i],SHUT_RDWR);if(upstreams[i]>=0)shutdown(upstreams[i],SHUT_RDWR);}pthread_join(P.thread,NULL);pthread_mutex_lock(&P.lock);while(P.workers)pthread_cond_wait(&P.idle,&P.lock);if(P.listener>=0)close(P.listener);P.listener=-1;P.running=0;P.stopping=0;pthread_mutex_unlock(&P.lock);
}
