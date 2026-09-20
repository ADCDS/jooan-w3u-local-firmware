#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MDNS_GROUP "224.0.0.251"

static JoanConfig M;
static pthread_mutex_t hostname_lock=PTHREAD_MUTEX_INITIALIZER;
static char hostname_label[64];
static int mdns_running;
static int mdns_stop_requested;
static int mdns_fd=-1;

static int valid_label(const char *input,char out[64])
{
    size_t i,n;
    if(!input)return -1;
    n=strlen(input);
    if(n>6&&!strcasecmp(input+n-6,".local"))n-=6;
    if(!n||n>63||input[0]=='-'||input[n-1]=='-')return -1;
    for(i=0;i<n;i++){
        unsigned char c=(unsigned char)input[i];
        if(!(isalnum(c)||c=='-'))return -1;
        out[i]=(char)tolower(c);
    }
    out[n]=0;
    return 0;
}

static void default_label(const JoanConfig *cfg,char out[64])
{
    if(valid_label(cfg->public_host,out))snprintf(out,64,"jooan-w3u");
}

int joan_mdns_get_configured_hostname(const JoanConfig *cfg,char out[64])
{
    unsigned char *saved=NULL;size_t n=0;char path[512];
    if(snprintf(path,sizeof(path),"%s/hostname",cfg->state_dir)>=(int)sizeof(path) ||
       joan_read_file(path,&saved,&n,128) || n==0){default_label(cfg,out);free(saved);return 0;}
    while(n&&(saved[n-1]=='\n'||saved[n-1]=='\r'))saved[--n]=0;
    if(valid_label((char*)saved,out))default_label(cfg,out);
    free(saved);return 0;
}

int joan_mdns_get_hostname(const JoanConfig *cfg,char out[64])
{
    char candidate[64];
    pthread_mutex_lock(&hostname_lock);
    if(hostname_label[0])snprintf(candidate,sizeof(candidate),"%s",hostname_label);
    else candidate[0]=0;
    pthread_mutex_unlock(&hostname_lock);
    if(candidate[0]){snprintf(out,64,"%s",candidate);return 0;}
    return joan_mdns_get_configured_hostname(cfg,out);
}

int joan_mdns_set_hostname(const JoanConfig *cfg,const char *hostname)
{
    char label[64],path[512],line[66];int n;
    if(valid_label(hostname,label))return -1;
    if(snprintf(path,sizeof(path),"%s/hostname",cfg->state_dir)>=(int)sizeof(path))return -1;
    n=snprintf(line,sizeof(line),"%s\n",label);
    if(n<=0||joan_write_atomic(path,line,(size_t)n,0600))return -1;
    /* Keep advertising the hostname covered by the currently loaded TLS
     * certificate. The saved value becomes active together on daemon restart. */
    pthread_mutex_lock(&hostname_lock);if(!mdns_running)snprintf(hostname_label,sizeof(hostname_label),"%s",label);pthread_mutex_unlock(&hostname_lock);
    return 0;
}

const char *joan_mdns_status(void)
{
    int running;
    pthread_mutex_lock(&hostname_lock);running=mdns_running;pthread_mutex_unlock(&hostname_lock);
    return running?"running":(M.mdns_enabled?"starting":"disabled");
}

static int iface_addresses(int fd,struct in_addr *out,size_t cap,int join)
{
    struct ifconf ifc;struct ifreq reqs[16];size_t i,count=0;
    memset(&ifc,0,sizeof(ifc));ifc.ifc_len=sizeof(reqs);ifc.ifc_req=reqs;
    if(ioctl(fd,SIOCGIFCONF,&ifc))return 0;
    for(i=0;i<(size_t)ifc.ifc_len/sizeof(struct ifreq)&&count<cap;i++){
        struct ifreq flags=reqs[i];struct sockaddr_in *a=(struct sockaddr_in*)&reqs[i].ifr_addr;
        if(a->sin_family!=AF_INET||ioctl(fd,SIOCGIFFLAGS,&flags) ||
           !(flags.ifr_flags&IFF_UP)||(flags.ifr_flags&IFF_LOOPBACK)||a->sin_addr.s_addr==0)continue;
        out[count++]=a->sin_addr;
        if(join){struct ip_mreq m;inet_pton(AF_INET,MDNS_GROUP,&m.imr_multiaddr);m.imr_interface=a->sin_addr;setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m));}
    }
    return (int)count;
}

static int decode_question(const unsigned char *packet,size_t n,char *name,size_t cap)
{
    size_t p=12,o=0;
    if(n<17)return -1;
    while(p<n&&packet[p]){
        unsigned len=packet[p++];size_t i;
        if((len&0xc0)||!len||p+len>n||o+len+2>cap)return -1;
        if(o)name[o++]='.';
        for(i=0;i<len;i++)name[o++]=(char)tolower(packet[p++]);
    }
    if(p>=n||p+5>n)return -1;
    name[o]=0;
    return 0;
}

static size_t dns_name(unsigned char *out,size_t p,size_t cap,const char *name)
{
    const char *s=name,*dot;size_t len;
    while(*s){dot=strchr(s,'.');len=dot?(size_t)(dot-s):strlen(s);if(!len||len>63||p+len+1>=cap)return 0;out[p++]=(unsigned char)len;memcpy(out+p,s,len);p+=len;if(!dot)break;s=dot+1;}
    if(p>=cap)return 0;
    out[p++]=0;
    return p;
}

static int rr_head(unsigned char*out,size_t*p,size_t cap,const char*name,unsigned type,unsigned ttl,size_t*rdlen)
{size_t q=dns_name(out,*p,cap,name);if(!q||q+10>cap)return-1;*p=q;out[(*p)++]=type>>8;out[(*p)++]=type;out[(*p)++]=type==12?0:0x80;out[(*p)++]=1;out[(*p)++]=ttl>>24;out[(*p)++]=ttl>>16;out[(*p)++]=ttl>>8;out[(*p)++]=ttl;*rdlen=*p;out[(*p)++]=0;out[(*p)++]=0;return 0;}
static void rr_end(unsigned char*out,size_t p,size_t rdlen){size_t n=p-rdlen-2;out[rdlen]=n>>8;out[rdlen+1]=n;}
static size_t build_discovery(unsigned char*out,size_t cap,const char*label,struct in_addr address,unsigned ttl)
{
    static const struct{const char*service;unsigned port;const char*txt;}svc[]={{"_https._tcp.local",443,"path=/"},{"_ssh._tcp.local",22,"auth=password"},{"_rtsp._tcp.local",554,"streams=main,sub"},{"_jooan-camera._tcp.local",443,"model=JA-A12"}};
    char host[80],instance[160];size_t p=12,rd;unsigned i,count=1+(unsigned)(sizeof(svc)/sizeof(svc[0]))*3;memset(out,0,cap);out[2]=0x84;out[6]=count>>8;out[7]=count;snprintf(host,sizeof(host),"%s.local",label);
    if(rr_head(out,&p,cap,host,1,ttl,&rd)||p+4>cap)return 0;
    memcpy(out+p,&address,4);p+=4;rr_end(out,p,rd);
    for(i=0;i<sizeof(svc)/sizeof(svc[0]);i++){
        snprintf(instance,sizeof(instance),"%s.%s",label,svc[i].service);
        if(rr_head(out,&p,cap,svc[i].service,12,ttl,&rd))return 0;
        p=dns_name(out,p,cap,instance);if(!p)return 0;rr_end(out,p,rd);
        if(rr_head(out,&p,cap,instance,33,ttl,&rd)||p+6>cap)return 0;
        out[p++]=0;out[p++]=0;out[p++]=0;out[p++]=0;out[p++]=svc[i].port>>8;out[p++]=svc[i].port;p=dns_name(out,p,cap,host);if(!p)return 0;rr_end(out,p,rd);
        if(rr_head(out,&p,cap,instance,16,ttl,&rd)||p+strlen(svc[i].txt)+1>cap)return 0;
        out[p++]=(unsigned char)strlen(svc[i].txt);memcpy(out+p,svc[i].txt,strlen(svc[i].txt));p+=strlen(svc[i].txt);rr_end(out,p,rd);
    }
    return p;
}

static void announce(int fd,unsigned ttl)
{
    unsigned char answer[1400];char label[64];struct in_addr addresses[8];struct sockaddr_in group;int count,i;memset(&group,0,sizeof(group));group.sin_family=AF_INET;group.sin_port=htons((uint16_t)M.mdns_port);inet_pton(AF_INET,MDNS_GROUP,&group.sin_addr);joan_mdns_get_hostname(&M,label);count=iface_addresses(fd,addresses,8,0);for(i=0;i<count;i++){size_t n=build_discovery(answer,sizeof(answer),label,addresses[i],ttl);if(n){setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&addresses[i],sizeof(addresses[i]));sendto(fd,answer,n,0,(struct sockaddr*)&group,sizeof(group));}}
}

static int open_socket(void)
{
    int fd,one=1;struct sockaddr_in bind_addr;struct timeval timeout={5,0};struct in_addr addresses[8];
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    memset(&bind_addr,0,sizeof(bind_addr));bind_addr.sin_family=AF_INET;bind_addr.sin_port=htons((uint16_t)M.mdns_port);bind_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(fd,(struct sockaddr*)&bind_addr,sizeof(bind_addr))){close(fd);return -1;}
    iface_addresses(fd,addresses,8,1);return fd;
}

static void *mdns_loop(void *unused)
{
    unsigned char query[1500],answer[1400];char asked[128],label[64],wanted[80];struct sockaddr_in group;int fd=-1,timeouts=0,announced=0;
    (void)unused;memset(&group,0,sizeof(group));group.sin_family=AF_INET;group.sin_port=htons((uint16_t)M.mdns_port);inet_pton(AF_INET,MDNS_GROUP,&group.sin_addr);
    for(;!mdns_stop_requested;){
        struct sockaddr_in peer;socklen_t peer_len=sizeof(peer);ssize_t n;if(fd<0){fd=open_socket();if(fd<0){sleep(5);continue;}pthread_mutex_lock(&hostname_lock);mdns_fd=fd;mdns_running=1;pthread_mutex_unlock(&hostname_lock);announced=0;}
        if(!announced){announce(fd,120);announced=1;}
        n=recvfrom(fd,query,sizeof(query),0,(struct sockaddr*)&peer,&peer_len);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK){if(++timeouts<3)continue;}close(fd);fd=-1;timeouts=0;pthread_mutex_lock(&hostname_lock);mdns_running=0;pthread_mutex_unlock(&hostname_lock);continue;}
        timeouts=0;if(n<12||(query[2]&0x80)||(!query[4]&&!query[5])||decode_question(query,(size_t)n,asked,sizeof(asked)))continue;
        joan_mdns_get_hostname(&M,label);snprintf(wanted,sizeof(wanted),"%s.local",label);
        if(strcmp(asked,wanted)&&strcmp(asked,"_https._tcp.local")&&strcmp(asked,"_ssh._tcp.local")&&strcmp(asked,"_rtsp._tcp.local")&&strcmp(asked,"_jooan-camera._tcp.local")&&!strstr(asked,"._https._tcp.local")&&!strstr(asked,"._ssh._tcp.local")&&!strstr(asked,"._rtsp._tcp.local")&&!strstr(asked,"._jooan-camera._tcp.local"))continue;
        if(ntohs(peer.sin_port)!=M.mdns_port){struct in_addr addresses[8];int count=iface_addresses(fd,addresses,8,0);if(count>0){size_t z=build_discovery(answer,sizeof(answer),label,addresses[0],120);if(z)sendto(fd,answer,z,0,(const struct sockaddr*)&peer,sizeof(peer));}}
        else {struct in_addr addresses[8];int count=iface_addresses(fd,addresses,8,0),i;for(i=0;i<count;i++){size_t z=build_discovery(answer,sizeof(answer),label,addresses[i],120);if(!z)continue;setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&addresses[i],sizeof(addresses[i]));sendto(fd,answer,z,0,(const struct sockaddr*)&group,sizeof(group));}}
    }
    if(fd>=0){announce(fd,0);close(fd);}pthread_mutex_lock(&hostname_lock);mdns_fd=-1;mdns_running=0;pthread_mutex_unlock(&hostname_lock);
    return NULL;
}

int joan_mdns_start(const JoanConfig *cfg)
{
    pthread_t thread;char label[64];unsigned char selftest[1400];struct in_addr loopback;M=*cfg;mdns_stop_requested=0;
    if(!cfg->mdns_enabled)return 0;
    joan_mdns_get_hostname(cfg,label);
    inet_pton(AF_INET,"127.0.0.1",&loopback);if(!build_discovery(selftest,sizeof(selftest),label,loopback,120))return-1;
    pthread_mutex_lock(&hostname_lock);snprintf(hostname_label,sizeof(hostname_label),"%s",label);pthread_mutex_unlock(&hostname_lock);
    if(pthread_create(&thread,NULL,mdns_loop,NULL))return -1;
    pthread_detach(thread);return 0;
}

void joan_mdns_stop(void)
{
    int fd;pthread_mutex_lock(&hostname_lock);mdns_stop_requested=1;fd=mdns_fd;pthread_mutex_unlock(&hostname_lock);if(fd>=0)shutdown(fd,SHUT_RDWR);
}
