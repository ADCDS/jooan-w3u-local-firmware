#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RTP_TIMESCALE 90000u
#define MAX_NAL (1024u*1024u)
#define MAX_GOP (2u*1024u*1024u)
#define MAX_SAMPLES 180u
/* 6-second OEM GOPs need the last IDR and its following short fragments for
 * late viewers to catch up without waiting for the next keyframe. */
#define RING 16u
#define FRAGMENT_TICKS RTP_TIMESCALE
#define RING_MAX_BYTES (3u*1024u*1024u)

typedef struct { unsigned char *p;size_t n,cap; } Buf;
typedef struct { uint32_t size,duration,flags,timestamp; } Sample;
typedef struct { unsigned char *data;size_t len;uint32_t sequence;time_t published;int keyframe; } Fragment;
typedef struct {
    const char *id,*path;unsigned width,height;
    pthread_t thread;pthread_mutex_t lock;pthread_cond_t changed;
    char status[32];
    unsigned char sps[256],pps[256];size_t sps_len,pps_len;
    unsigned char *init;size_t init_len;
    unsigned char *access;size_t access_len,access_cap;int access_idr;
    unsigned char *gop;size_t gop_len,gop_cap;Sample samples[MAX_SAMPLES];size_t sample_count;
    uint64_t decode_time;uint32_t last_timestamp,sequence;
    int have_keyframe,have_timestamp;
    Fragment ring[RING];unsigned ring_next;size_t ring_bytes;
} Stream;

static Stream streams[2]={
 {.id="main",.path="/live/ch0",.width=2304,.height=1296,.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.status="starting"},
 {.id="sub",.path="/live/ch1",.width=640,.height=360,.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.status="starting"}
};
static unsigned rtsp_port=554;
static char rtsp_state_dir[256];

typedef struct{uint32_t h[4],bits[2];unsigned char block[64];size_t used;}MD5;
static uint32_t rol(uint32_t x,unsigned n){return(x<<n)|(x>>(32-n));}
static void md5_block(MD5*c,const unsigned char*p){uint32_t x[16],a=c->h[0],b=c->h[1],d=c->h[3],e=c->h[2],f,t;unsigned i;static const uint32_t k[64]={0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};static const unsigned char s[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};for(i=0;i<16;i++)x[i]=(uint32_t)p[i*4]|((uint32_t)p[i*4+1]<<8)|((uint32_t)p[i*4+2]<<16)|((uint32_t)p[i*4+3]<<24);for(i=0;i<64;i++){unsigned g;if(i<16){f=(b&e)|(~b&d);g=i;}else if(i<32){f=(d&b)|(~d&e);g=(5*i+1)&15;}else if(i<48){f=b^e^d;g=(3*i+5)&15;}else{f=e^(b|~d);g=(7*i)&15;}t=d;d=e;e=b;b=b+rol(a+f+k[i]+x[g],s[i]);a=t;}c->h[0]+=a;c->h[1]+=b;c->h[2]+=e;c->h[3]+=d;}
static void md5_init(MD5*c){c->h[0]=0x67452301;c->h[1]=0xefcdab89;c->h[2]=0x98badcfe;c->h[3]=0x10325476;c->bits[0]=c->bits[1]=0;c->used=0;}
static void md5_update(MD5*c,const void*v,size_t n){const unsigned char*p=v;uint32_t old=c->bits[0];c->bits[0]+=(uint32_t)n<<3;if(c->bits[0]<old)c->bits[1]++;c->bits[1]+=(uint32_t)(n>>29);while(n){size_t z=64-c->used;if(z>n)z=n;memcpy(c->block+c->used,p,z);c->used+=z;p+=z;n-=z;if(c->used==64){md5_block(c,c->block);c->used=0;}}}
static void md5_final(MD5*c,unsigned char out[16]){unsigned char len[8],pad[64]={0x80};unsigned i;for(i=0;i<4;i++){len[i]=c->bits[0]>>(8*i);len[i+4]=c->bits[1]>>(8*i);}md5_update(c,pad,c->used<56?56-c->used:120-c->used);md5_update(c,len,8);for(i=0;i<4;i++){out[i*4]=c->h[i];out[i*4+1]=c->h[i]>>8;out[i*4+2]=c->h[i]>>16;out[i*4+3]=c->h[i]>>24;}}
static void md5hex(const char*text,char out[33]){MD5 c;unsigned char d[16];md5_init(&c);md5_update(&c,text,strlen(text));md5_final(&c,d);joan_hex(d,16,out);}
typedef struct{char realm[128],nonce[256],qop[32],opaque[128],cnonce[33];unsigned nc;}Digest;
static int digest_attr(const char*h,const char*name,char*out,size_t cap){char needle[40];const char*p,*e;snprintf(needle,sizeof(needle),"%s=",name);p=strcasestr(h,needle);if(!p)return-1;p+=strlen(needle);if(*p=='\"'){p++;e=strchr(p,'\"');}else e=strpbrk(p,",\r\n");if(!e)e=p+strlen(p);if((size_t)(e-p)>=cap)return-1;memcpy(out,p,(size_t)(e-p));out[e-p]=0;return 0;}

static void msleep(unsigned ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000L};while(nanosleep(&t,&t)&&errno==EINTR){}}
static int grow(Buf*b,size_t add){size_t need=b->n+add,cap=b->cap?b->cap:1024;unsigned char*p;if(!b->cap&&b->n)return-1;if(need<=b->cap)return 0;while(cap<need){if(cap>8u*1024u*1024u)return-1;cap*=2;}p=realloc(b->p,cap);if(!p)return-1;b->p=p;b->cap=cap;return 0;}
static int put(Buf*b,const void*p,size_t n){if(grow(b,n)){b->n=1;b->cap=0;return-1;}memcpy(b->p+b->n,p,n);b->n+=n;return 0;}
static void u8(Buf*b,uint8_t v){put(b,&v,1);}static void u16(Buf*b,uint16_t v){unsigned char x[]={v>>8,v};put(b,x,2);}static void u32(Buf*b,uint32_t v){unsigned char x[]={v>>24,v>>16,v>>8,v};put(b,x,4);}static void u64(Buf*b,uint64_t v){u32(b,(uint32_t)(v>>32));u32(b,(uint32_t)v);}
static size_t box(Buf*b,const char*t){size_t p=b->n;u32(b,0);put(b,t,4);return p;}static void endbox(Buf*b,size_t p){uint32_t z;if(!b->cap||p>=b->n||b->n-p<8)return;z=(uint32_t)(b->n-p);b->p[p]=z>>24;b->p[p+1]=z>>16;b->p[p+2]=z>>8;b->p[p+3]=z;}
static void zeros(Buf*b,size_t n){static const unsigned char z[32]={0};while(n){size_t q=n>sizeof(z)?sizeof(z):n;put(b,z,q);n-=q;}}

static int build_init(Stream*s)
{
    Buf b={0};size_t f,moov,mvhd,trak,tkhd,mdia,mdhd,hdlr,minf,vmhd,dinf,dref,url,stbl,stsd,avc1,avcc,empty,mvex,trex;
    if(s->sps_len<4||!s->pps_len)return-1;
    f=box(&b,"ftyp");put(&b,"iso6",4);u32(&b,1);put(&b,"iso6isomavc1mp41",16);endbox(&b,f);
    moov=box(&b,"moov");mvhd=box(&b,"mvhd");u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,RTP_TIMESCALE);u32(&b,0);u32(&b,0x00010000);u16(&b,0x0100);u16(&b,0);zeros(&b,8);u32(&b,0x00010000);u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,0x00010000);u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,0x40000000);zeros(&b,24);u32(&b,2);endbox(&b,mvhd);
    trak=box(&b,"trak");tkhd=box(&b,"tkhd");u32(&b,7);u32(&b,0);u32(&b,0);u32(&b,1);u32(&b,0);u32(&b,0);zeros(&b,8);u16(&b,0);u16(&b,0);u16(&b,0);u16(&b,0);u32(&b,0x00010000);u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,0x00010000);u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,0x40000000);u32(&b,s->width<<16);u32(&b,s->height<<16);endbox(&b,tkhd);
    mdia=box(&b,"mdia");mdhd=box(&b,"mdhd");u32(&b,0);u32(&b,0);u32(&b,0);u32(&b,RTP_TIMESCALE);u32(&b,0);u16(&b,0x55c4);u16(&b,0);endbox(&b,mdhd);hdlr=box(&b,"hdlr");u32(&b,0);u32(&b,0);put(&b,"vide",4);zeros(&b,12);put(&b,"VideoHandler\0",13);endbox(&b,hdlr);
    minf=box(&b,"minf");vmhd=box(&b,"vmhd");u32(&b,1);zeros(&b,8);endbox(&b,vmhd);dinf=box(&b,"dinf");dref=box(&b,"dref");u32(&b,0);u32(&b,1);url=box(&b,"url ");u32(&b,1);endbox(&b,url);endbox(&b,dref);endbox(&b,dinf);
    stbl=box(&b,"stbl");stsd=box(&b,"stsd");u32(&b,0);u32(&b,1);avc1=box(&b,"avc1");zeros(&b,6);u16(&b,1);zeros(&b,16);u16(&b,(uint16_t)s->width);u16(&b,(uint16_t)s->height);u32(&b,0x00480000);u32(&b,0x00480000);u32(&b,0);u16(&b,1);zeros(&b,32);u16(&b,0x18);u16(&b,0xffff);
    avcc=box(&b,"avcC");u8(&b,1);u8(&b,s->sps[1]);u8(&b,s->sps[2]);u8(&b,s->sps[3]);u8(&b,0xff);u8(&b,0xe1);u16(&b,(uint16_t)s->sps_len);put(&b,s->sps,s->sps_len);u8(&b,1);u16(&b,(uint16_t)s->pps_len);put(&b,s->pps,s->pps_len);endbox(&b,avcc);endbox(&b,avc1);endbox(&b,stsd);
    empty=box(&b,"stts");u32(&b,0);u32(&b,0);endbox(&b,empty);empty=box(&b,"stsc");u32(&b,0);u32(&b,0);endbox(&b,empty);empty=box(&b,"stsz");u32(&b,0);u32(&b,0);u32(&b,0);endbox(&b,empty);empty=box(&b,"stco");u32(&b,0);u32(&b,0);endbox(&b,empty);endbox(&b,stbl);endbox(&b,minf);endbox(&b,mdia);endbox(&b,trak);
    mvex=box(&b,"mvex");trex=box(&b,"trex");u32(&b,0);u32(&b,1);u32(&b,1);u32(&b,0);u32(&b,0);u32(&b,0);endbox(&b,trex);endbox(&b,mvex);endbox(&b,moov);
    pthread_mutex_lock(&s->lock);free(s->init);s->init=b.p;s->init_len=b.n;snprintf(s->status,sizeof(s->status),"ready");pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);return 0;
}

static int append_nal(Stream*s,const unsigned char*p,size_t n)
{
    unsigned char l[4];unsigned type;if(!n||n>MAX_NAL)return-1;type=p[0]&31u;
    if(type==7&&n<=sizeof(s->sps)){if(s->init&&(s->sps_len!=n||memcmp(s->sps,p,n)))return-1;memcpy(s->sps,p,n);s->sps_len=n;if(s->pps_len&&!s->init)build_init(s);}
    if(type==8&&n<=sizeof(s->pps)){if(s->init&&(s->pps_len!=n||memcmp(s->pps,p,n)))return-1;memcpy(s->pps,p,n);s->pps_len=n;if(s->sps_len&&!s->init)build_init(s);}
    if(s->access_len+4+n>MAX_NAL)return-1;
    if(s->access_cap<s->access_len+4+n){size_t cap=s->access_cap?s->access_cap*2:65536;unsigned char*q;while(cap<s->access_len+4+n)cap*=2;q=realloc(s->access,cap);if(!q)return-1;s->access=q;s->access_cap=cap;}
    l[0]=n>>24;l[1]=n>>16;l[2]=n>>8;l[3]=n;memcpy(s->access+s->access_len,l,4);memcpy(s->access+s->access_len+4,p,n);s->access_len+=4+n;if(type==5)s->access_idr=1;return 0;
}

static int publish_gop(Stream*s)
{
    Buf b={0};size_t moof,mfhd,traf,tfhd,tfdt,trun,offset_pos,mdat,i;
    uint32_t data_offset;uint64_t next_decode_time=s->decode_time;Fragment*f;
    if(!s->init||!s->sample_count||!s->gop_len||s->gop_len>MAX_GOP)return-1;
    {int keyframe=s->samples[0].flags==0x02000000;
    moof=box(&b,"moof");mfhd=box(&b,"mfhd");u32(&b,0);u32(&b,s->sequence+1);endbox(&b,mfhd);traf=box(&b,"traf");tfhd=box(&b,"tfhd");u32(&b,0x020000);u32(&b,1);endbox(&b,tfhd);tfdt=box(&b,"tfdt");u32(&b,0x01000000);u64(&b,s->decode_time);endbox(&b,tfdt);trun=box(&b,"trun");u32(&b,0x00000701);u32(&b,(uint32_t)s->sample_count);offset_pos=b.n;u32(&b,0);for(i=0;i<s->sample_count;i++){u32(&b,s->samples[i].duration);u32(&b,s->samples[i].size);u32(&b,s->samples[i].flags);next_decode_time+=s->samples[i].duration;}endbox(&b,trun);endbox(&b,traf);endbox(&b,moof);data_offset=(uint32_t)b.n+8;if(b.cap&&offset_pos+4<=b.n){b.p[offset_pos]=data_offset>>24;b.p[offset_pos+1]=data_offset>>16;b.p[offset_pos+2]=data_offset>>8;b.p[offset_pos+3]=data_offset;}mdat=box(&b,"mdat");put(&b,s->gop,s->gop_len);endbox(&b,mdat);
    if(!b.p||!b.cap||b.n<16||b.n>MAX_GOP+4096){free(b.p);return-1;}
    s->decode_time=next_decode_time;s->sequence++;
    pthread_mutex_lock(&s->lock);f=&s->ring[s->ring_next++%RING];s->ring_bytes-=f->len;free(f->data);f->data=b.p;f->len=b.n;f->sequence=s->sequence;f->published=time(NULL);f->keyframe=keyframe;s->ring_bytes+=f->len;
    /* Bound retention on the camera's small RAM. */
    while(s->ring_bytes>RING_MAX_BYTES){unsigned j,oldest=0;for(j=1;j<RING;j++)if(s->ring[j].data&&(!s->ring[oldest].data||s->ring[j].sequence<s->ring[oldest].sequence))oldest=j;s->ring_bytes-=s->ring[oldest].len;free(s->ring[oldest].data);memset(&s->ring[oldest],0,sizeof(s->ring[oldest]));}
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
    }
    return 0;
}

static void finish_access(Stream*s,uint32_t timestamp)
{
    uint32_t duration=s->have_timestamp?timestamp-s->last_timestamp:6000;size_t i;
    if(!s->access_len)return;
    if(s->have_timestamp&&(!duration||duration>RTP_TIMESCALE)){
        s->gop_len=s->sample_count=0;s->have_keyframe=0;
        s->access_len=s->access_idr=0;s->last_timestamp=timestamp;
        return;
    }
    /* The next AU supplies the previous sample's duration (RTP timestamps
     * denote starts, not ends). This also finalizes the GOP's last sample. */
    if(s->sample_count)s->samples[s->sample_count-1].duration=duration;
    /* Publish ~1s at a time. The IDR closes any outstanding chunk, then
     * starts the new GOP in its own random-access fragment. */
    if(s->sample_count&&(s->access_idr||
        (s->samples[0].flags==0x02000000&&s->sample_count==1)||
        timestamp-s->samples[0].timestamp>=FRAGMENT_TICKS)){
        if(publish_gop(s)){s->have_keyframe=0;s->gop_len=s->sample_count=0;goto drop;}
        s->gop_len=0;s->sample_count=0;
    }
    if(!s->have_keyframe&&!s->access_idr){s->access_len=0;s->access_idr=0;s->last_timestamp=timestamp;s->have_timestamp=1;return;}
    if(s->sample_count>=MAX_SAMPLES||s->gop_len+s->access_len>MAX_GOP){s->gop_len=s->sample_count=0;s->have_keyframe=0;goto drop;}
    if(s->access_idr)s->have_keyframe=1;
    {if(s->gop_cap<s->gop_len+s->access_len){size_t cap=s->gop_cap?s->gop_cap*2:262144;unsigned char*q;while(cap<s->gop_len+s->access_len)cap*=2;q=realloc(s->gop,cap);if(!q)goto drop;s->gop=q;s->gop_cap=cap;}memcpy(s->gop+s->gop_len,s->access,s->access_len);i=s->sample_count++;s->samples[i].size=(uint32_t)s->access_len;s->samples[i].duration=duration; s->samples[i].flags=s->access_idr?0x02000000:0x01010000;s->samples[i].timestamp=timestamp;s->gop_len+=s->access_len;}
drop:s->access_len=0;s->access_idr=0;s->last_timestamp=timestamp;s->have_timestamp=1;
}

static int b64(const char*in,unsigned char*out,size_t cap,size_t*len){static const char*a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";unsigned v=0,bits=0;size_t n=0;for(;*in&&*in!=','&&*in!=';'&&*in!='\r'&&*in!='\n';in++){const char*p;if(*in=='=')break;p=strchr(a,*in);if(!p)return-1;v=(v<<6)|(unsigned)(p-a);bits+=6;if(bits>=8){bits-=8;if(n>=cap)return-1;out[n++]=(unsigned char)(v>>bits);v&=(1u<<bits)-1u;}}*len=n;return 0;}

static int rtsp_read_response(int fd,char*out,size_t cap)
{
    size_t used=0,content=0;
    while(used+1<cap){ssize_t z=recv(fd,out+used,cap-used-1,0);char*e,*cl;if(z<=0)return-1;used+=(size_t)z;out[used]=0;e=strstr(out,"\r\n\r\n");if(!e)continue;cl=strcasestr(out,"Content-Length:");if(cl)content=(size_t)strtoul(cl+15,NULL,10);if(used>=(size_t)(e-out)+4+content)break;}
    return 0;
}

static int rtsp_request(int fd,int cseq,const char*method,const char*url,const char*extra,const char*password,Digest*digest,char*out,size_t cap)
{
    char req[2048],authorization[1200]="";unsigned attempt;
    for(attempt=0;attempt<2;attempt++){
        int n;
        if(digest->nonce[0]){char a1[512],a2[768],joined[1200],ha1[33],ha2[33],response[33],opaque[180]="";snprintf(a1,sizeof(a1),"admin:%s:%s",digest->realm,password);snprintf(a2,sizeof(a2),"%s:%s",method,url);md5hex(a1,ha1);md5hex(a2,ha2);digest->nc++;if(strstr(digest->qop,"auth")){snprintf(joined,sizeof(joined),"%s:%s:%08x:%s:auth:%s",ha1,digest->nonce,digest->nc,digest->cnonce,ha2);md5hex(joined,response);if(digest->opaque[0])snprintf(opaque,sizeof(opaque),", opaque=\"%s\"",digest->opaque);snprintf(authorization,sizeof(authorization),"Authorization: Digest username=\"admin\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5, qop=auth, nc=%08x, cnonce=\"%s\"%s\r\n",digest->realm,digest->nonce,url,response,digest->nc,digest->cnonce,opaque);}else{snprintf(joined,sizeof(joined),"%s:%s:%s",ha1,digest->nonce,ha2);md5hex(joined,response);snprintf(authorization,sizeof(authorization),"Authorization: Digest username=\"admin\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5\r\n",digest->realm,digest->nonce,url,response);}}
        n=snprintf(req,sizeof(req),"%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: joan-local\r\n%s%s\r\n",method,url,cseq,authorization,extra?extra:"");if(n<=0||(size_t)n>=sizeof(req)||send(fd,req,(size_t)n,MSG_NOSIGNAL)!=n)return-1;if(rtsp_read_response(fd,out,cap))return-1;if(strstr(out,"RTSP/1.0 200"))return 0;if(!strstr(out,"RTSP/1.0 401")||attempt)return-1;{char*challenge=strcasestr(out,"WWW-Authenticate: Digest");unsigned char random[16];if(!challenge||digest_attr(challenge,"realm",digest->realm,sizeof(digest->realm))||digest_attr(challenge,"nonce",digest->nonce,sizeof(digest->nonce)))return-1;(void)digest_attr(challenge,"qop",digest->qop,sizeof(digest->qop));(void)digest_attr(challenge,"opaque",digest->opaque,sizeof(digest->opaque));if(joan_random(random,sizeof(random)))return-1;joan_hex(random,sizeof(random),digest->cnonce);digest->nc=0;}}
    return-1;
}

/* An RTSP session has its own RTP clock and may also have new codec parameters.
 * Never let an old GOP or init segment escape into the next session. Leave a
 * sequence gap so existing MSE clients know they must fetch a fresh init. */
static void reset_stream(Stream*s)
{
    unsigned i;
    pthread_mutex_lock(&s->lock);
    free(s->init);s->init=NULL;s->init_len=0;
    snprintf(s->status,sizeof(s->status),"reconnecting");
    for(i=0;i<RING;i++){free(s->ring[i].data);memset(&s->ring[i],0,sizeof(s->ring[i]));}
    s->ring_next=0;s->ring_bytes=0;s->sequence+=RING+1;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
    s->sps_len=s->pps_len=0;s->access_len=0;s->access_idr=0;
    s->gop_len=s->sample_count=0;s->last_timestamp=0;s->decode_time=0;s->have_keyframe=s->have_timestamp=0;
}

static int run_stream(Stream*s)
{
    int fd=-1;struct sockaddr_in a;char url[256],control[512],reply[16384],session[128]="",extra[512],password[320],password_path[512];unsigned char packet[MAX_NAL+64],fu[MAX_NAL],*secret=NULL;size_t fu_len=0,secret_len=0;uint32_t fu_ts=0;uint16_t previous_rtp_seq=0;int have_rtp_seq=0;Digest digest;
    memset(&digest,0,sizeof(digest));snprintf(password_path,sizeof(password_path),"%s/rtsp.password",rtsp_state_dir);if(joan_read_file(password_path,&secret,&secret_len,sizeof(password)-1)){pthread_mutex_lock(&s->lock);snprintf(s->status,sizeof(s->status),"password-unsynchronized");pthread_mutex_unlock(&s->lock);return-1;}while(secret_len&&(secret[secret_len-1]=='\n'||secret[secret_len-1]=='\r'))secret_len--;memcpy(password,secret,secret_len);password[secret_len]=0;memset(secret,0,secret_len);free(secret);
    fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return-1;{struct timeval tv={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));}memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)rtsp_port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);if(connect(fd,(struct sockaddr*)&a,sizeof(a)))goto fail;snprintf(url,sizeof(url),"rtsp://127.0.0.1:%u%s",rtsp_port,s->path);if(rtsp_request(fd,1,"DESCRIBE",url,"Accept: application/sdp\r\n",password,&digest,reply,sizeof(reply)))goto fail;
    {char*body=strstr(reply,"\r\n\r\n"),*video,*section_end,*sp,*cp,*base_header;char base[512]="";if(!body)goto fail;body+=4;video=strstr(body,"m=video");if(!video)goto fail;section_end=strstr(video+1,"\nm=");sp=strstr(video,"sprop-parameter-sets=");if(sp&&(!section_end||sp<section_end)){sp+=21;if(!b64(sp,s->sps,sizeof(s->sps),&s->sps_len)){char*comma=strchr(sp,',');if(comma&&(!section_end||comma<section_end))b64(comma+1,s->pps,sizeof(s->pps),&s->pps_len);}}cp=strstr(video,"a=control:");if(!cp||(section_end&&cp>=section_end))goto fail;cp+=10;{char*e=strpbrk(cp,"\r\n");size_t z;if(!e)goto fail;z=(size_t)(e-cp);if(!z||(z==1&&*cp=='*')||z>=sizeof(control))goto fail;base_header=strcasestr(reply,"Content-Base:");if(base_header){char*be;base_header+=13;while(*base_header==' ')base_header++;be=strpbrk(base_header,"\r\n");if(be&&(size_t)(be-base_header)<sizeof(base)){memcpy(base,base_header,(size_t)(be-base_header));base[be-base_header]=0;}}if(!strncasecmp(cp,"rtsp://",7)){memcpy(control,cp,z);control[z]=0;}else{const char*prefix=base[0]?base:url;size_t prefix_len=strlen(prefix),slash=(prefix_len&&prefix[prefix_len-1]=='/')?0:1;if(prefix_len+slash+z>=sizeof(control))goto fail;memcpy(control,prefix,prefix_len);if(slash)control[prefix_len++]='/';memcpy(control+prefix_len,cp,z);control[prefix_len+z]=0;}}}
    snprintf(extra,sizeof(extra),"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");if(rtsp_request(fd,2,"SETUP",control,extra,password,&digest,reply,sizeof(reply)))goto fail;{char*sp=strcasestr(reply,"Session:");if(!sp)goto fail;sp+=8;while(*sp==' ')sp++;size_t z=strcspn(sp,";\r\n");if(z>=sizeof(session))goto fail;memcpy(session,sp,z);session[z]=0;}snprintf(extra,sizeof(extra),"Session: %s\r\n",session);if(rtsp_request(fd,3,"PLAY",url,extra,password,&digest,reply,sizeof(reply)))goto fail;
    if(s->sps_len&&s->pps_len&&!s->init)build_init(s);
    pthread_mutex_lock(&s->lock);snprintf(s->status,sizeof(s->status),"connected");pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
    for(;;){unsigned char ch,channel;uint16_t plen;size_t off;uint32_t ts;int marker;unsigned type;if(recv(fd,&ch,1,MSG_WAITALL)!=1)goto fail;if(ch!='$')continue;if(recv(fd,packet,3,MSG_WAITALL)!=3)goto fail;channel=packet[0];plen=((uint16_t)packet[1]<<8)|packet[2];if(recv(fd,packet,plen,MSG_WAITALL)!=(ssize_t)plen)goto fail;if(channel!=0)continue;if(plen<12||(packet[0]&0xc0)!=0x80)continue;{uint16_t current=((uint16_t)packet[2]<<8)|packet[3];if(have_rtp_seq&&(uint16_t)(previous_rtp_seq+1)!=current)goto fail;previous_rtp_seq=current;have_rtp_seq=1;}off=12+(packet[0]&15u)*4u;if(packet[0]&0x10){if(off+4>plen)continue;off+=4+(((size_t)packet[off+2]<<8)|packet[off+3])*4;}if(off>=plen)continue;marker=(packet[1]&0x80)!=0;ts=((uint32_t)packet[4]<<24)|((uint32_t)packet[5]<<16)|((uint32_t)packet[6]<<8)|packet[7];type=packet[off]&31u;
        if(type>=1&&type<=23){if(append_nal(s,packet+off,plen-off))goto fail;}
        else if(type==24){size_t p=off+1;while(p+2<=plen){size_t z=((size_t)packet[p]<<8)|packet[p+1];p+=2;if(!z||p+z>plen)break;if(append_nal(s,packet+p,z))goto fail;p+=z;}}
        else if(type==28&&off+2<plen){unsigned start=packet[off+1]&0x80,end=packet[off+1]&0x40;if(start){fu_len=1;fu[0]=(packet[off]&0xe0)|(packet[off+1]&0x1f);fu_ts=ts;}if(fu_len&&fu_ts==ts&&fu_len+plen-off-2<=sizeof(fu)){memcpy(fu+fu_len,packet+off+2,plen-off-2);fu_len+=plen-off-2;if(end){if(append_nal(s,fu,fu_len))goto fail;fu_len=0;}}else fu_len=0;}
        if(marker){if(s->have_keyframe&&s->have_timestamp&&
            (!((uint32_t)(ts-s->last_timestamp))||
             (uint32_t)(ts-s->last_timestamp)>RTP_TIMESCALE)){
            /* OEM sometimes replays an older AU/IDR despite monotonic RTP
             * sequence numbers. Reconnect and wait for a clean keyframe. */
            goto fail;
        }else finish_access(s,ts);}
    }
fail:memset(password,0,sizeof(password));if(fd>=0)close(fd);return-1;
}

static void *worker(void*arg){Stream*s=arg;reset_stream(s);for(;;){if(run_stream(s)){reset_stream(s);msleep(1000);}}return NULL;}
int joan_fmp4_start(const JoanConfig*cfg){size_t i;rtsp_port=cfg->rtsp_port?cfg->rtsp_port:554;snprintf(rtsp_state_dir,sizeof(rtsp_state_dir),"%s",cfg->state_dir);for(i=0;i<2;i++)if(pthread_create(&streams[i].thread,NULL,worker,&streams[i]))return-1;for(i=0;i<2;i++)pthread_detach(streams[i].thread);return 0;}
static Stream*find_stream(const char*id){size_t i;for(i=0;i<2;i++)if(!strcmp(id,streams[i].id))return&streams[i];return NULL;}
static struct timespec wait_deadline(unsigned ms){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=ms/1000;t.tv_nsec+=(long)(ms%1000)*1000000L;if(t.tv_nsec>=1000000000L){t.tv_sec++;t.tv_nsec-=1000000000L;}return t;}
static int timedwait(Stream*s,const struct timespec*deadline){return pthread_cond_timedwait(&s->changed,&s->lock,deadline);}
int joan_fmp4_init_segment(const char*id,unsigned char**data,size_t*len,unsigned timeout){Stream*s=find_stream(id);struct timespec deadline=wait_deadline(timeout);if(!s||!data||!len)return-1;pthread_mutex_lock(&s->lock);while(!s->init_len&&!timedwait(s,&deadline)){}if(!s->init_len||strcmp(s->status,"connected")){pthread_mutex_unlock(&s->lock);return-1;}*data=malloc(s->init_len);if(!*data){pthread_mutex_unlock(&s->lock);return-1;}memcpy(*data,s->init,s->init_len);*len=s->init_len;pthread_mutex_unlock(&s->lock);return 0;}
int joan_fmp4_fragment(const char*id,uint32_t after,unsigned char**data,size_t*len,uint32_t*seq,unsigned timeout)
{
    Stream*s=find_stream(id);Fragment*best,*anchor;unsigned i,j;int rc=-1;
    struct timespec deadline=wait_deadline(timeout);
    if(!s||!data||!len||!seq)return-1;
    pthread_mutex_lock(&s->lock);
    for(;;){
        best=NULL;anchor=NULL;
        for(i=0;i<RING;i++){
            Fragment*f=&s->ring[i];
            if(!f->data)continue;
            if(f->sequence>after&&(!best||(after?f->sequence<best->sequence:f->sequence>best->sequence)))best=f;
            if(!after&&f->keyframe&&(!anchor||f->sequence>anchor->sequence))anchor=f;
        }
        if(best){
                if(strcmp(s->status,"connected")||time(NULL)-best->published>4){rc=-2;break;}
            if(after){
                if(best->sequence!=after+1){rc=-2;break;}
                *data=malloc(best->len);if(!*data)break;
                memcpy(*data,best->data,best->len);*len=best->len;*seq=best->sequence;rc=0;break;
            }
            if(anchor&&anchor->sequence<=best->sequence){
                size_t total=0,offset=0;unsigned count=best->sequence-anchor->sequence+1;
                if(count>RING){rc=-2;break;}
                for(j=0;j<count;j++){
                    Fragment*f=NULL;for(i=0;i<RING;i++)if(s->ring[i].data&&s->ring[i].sequence==anchor->sequence+j){f=&s->ring[i];break;}
                    if(!f||f->len>RING_MAX_BYTES-total){total=0;break;}total+=f->len;
                }
                if(total){
                    *data=malloc(total);if(!*data)break;
                    for(j=0;j<count;j++)for(i=0;i<RING;i++)if(s->ring[i].data&&s->ring[i].sequence==anchor->sequence+j){memcpy(*data+offset,s->ring[i].data,s->ring[i].len);offset+=s->ring[i].len;break;}
                    *len=total;*seq=best->sequence;rc=0;break;
                }
            }
            /* Refuse an incomplete GOP. A new request can bootstrap on the
             * next IDR rather than blocking an HTTP worker indefinitely. */
            rc=-2;break;
        }
        if(after&&s->sequence>after&&s->sequence-after>RING){rc=-2;break;}
        if(after&&s->sequence<after&&s->sequence){rc=-2;break;}
        if(timedwait(s,&deadline))break;
    }
    pthread_mutex_unlock(&s->lock);return rc;
}
const char*joan_fmp4_status(const char*id){Stream*s=find_stream(id);return s?s->status:"unavailable";}
