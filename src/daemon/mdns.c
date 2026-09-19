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

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"

static JoanConfig M;
static pthread_mutex_t hostname_lock=PTHREAD_MUTEX_INITIALIZER;
static char hostname_label[64];
static int mdns_running;

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

int joan_mdns_get_hostname(const JoanConfig *cfg,char out[64])
{
    unsigned char *saved=NULL;size_t n=0;char path[512],candidate[64];
    pthread_mutex_lock(&hostname_lock);
    if(hostname_label[0])snprintf(candidate,sizeof(candidate),"%s",hostname_label);
    else candidate[0]=0;
    pthread_mutex_unlock(&hostname_lock);
    if(candidate[0]){snprintf(out,64,"%s",candidate);return 0;}
    if(snprintf(path,sizeof(path),"%s/hostname",cfg->state_dir)>=(int)sizeof(path) ||
       joan_read_file(path,&saved,&n,128) || n==0){default_label(cfg,out);free(saved);return 0;}
    while(n&&(saved[n-1]=='\n'||saved[n-1]=='\r'))saved[--n]=0;
    if(valid_label((char*)saved,out))default_label(cfg,out);
    free(saved);return 0;
}

int joan_mdns_set_hostname(const JoanConfig *cfg,const char *hostname)
{
    char label[64],path[512],line[66];int n;
    if(valid_label(hostname,label))return -1;
    if(snprintf(path,sizeof(path),"%s/hostname",cfg->state_dir)>=(int)sizeof(path))return -1;
    n=snprintf(line,sizeof(line),"%s\n",label);
    if(n<=0||joan_write_atomic(path,line,(size_t)n,0600))return -1;
    pthread_mutex_lock(&hostname_lock);
    snprintf(hostname_label,sizeof(hostname_label),"%s",label);
    pthread_mutex_unlock(&hostname_lock);
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

static size_t build_answer(unsigned char *out,size_t cap,const char *label,struct in_addr address)
{
    char fqdn[80];size_t p=12,rdlen;
    memset(out,0,cap);out[2]=0x84;out[7]=1;
    snprintf(fqdn,sizeof(fqdn),"%s.local",label);
    p=dns_name(out,p,cap,fqdn);if(!p||p+14>cap)return 0;
    out[p++]=0;out[p++]=1;out[p++]=0x80;out[p++]=1;
    out[p++]=0;out[p++]=0;out[p++]=0;out[p++]=120;
    rdlen=4;out[p++]=0;out[p++]=(unsigned char)rdlen;
    memcpy(out+p,&address,4);return p+4;
}

static int open_socket(void)
{
    int fd,one=1;struct sockaddr_in bind_addr;struct timeval timeout={5,0};struct in_addr addresses[8];
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    memset(&bind_addr,0,sizeof(bind_addr));bind_addr.sin_family=AF_INET;bind_addr.sin_port=htons(MDNS_PORT);bind_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(fd,(struct sockaddr*)&bind_addr,sizeof(bind_addr))){close(fd);return -1;}
    iface_addresses(fd,addresses,8,1);return fd;
}

static void *mdns_loop(void *unused)
{
    unsigned char query[1500],answer[512];char asked[128],label[64],wanted[80];struct sockaddr_in group;int fd=-1,timeouts=0;
    (void)unused;memset(&group,0,sizeof(group));group.sin_family=AF_INET;group.sin_port=htons(MDNS_PORT);inet_pton(AF_INET,MDNS_GROUP,&group.sin_addr);
    for(;;){
        ssize_t n;if(fd<0){fd=open_socket();if(fd<0){sleep(5);continue;}pthread_mutex_lock(&hostname_lock);mdns_running=1;pthread_mutex_unlock(&hostname_lock);}
        n=recv(fd,query,sizeof(query),0);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK){if(++timeouts<3)continue;}close(fd);fd=-1;timeouts=0;pthread_mutex_lock(&hostname_lock);mdns_running=0;pthread_mutex_unlock(&hostname_lock);continue;}
        timeouts=0;if(decode_question(query,(size_t)n,asked,sizeof(asked)))continue;
        joan_mdns_get_hostname(&M,label);snprintf(wanted,sizeof(wanted),"%s.local",label);
        if(strcmp(asked,wanted))continue;
        {struct in_addr addresses[8];int count=iface_addresses(fd,addresses,8,0),i;for(i=0;i<count;i++){size_t z=build_answer(answer,sizeof(answer),label,addresses[i]);if(!z)continue;setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&addresses[i],sizeof(addresses[i]));sendto(fd,answer,z,0,(struct sockaddr*)&group,sizeof(group));}}
    }
    return NULL;
}

int joan_mdns_start(const JoanConfig *cfg)
{
    pthread_t thread;char label[64];M=*cfg;
    if(!cfg->mdns_enabled)return 0;
    joan_mdns_get_hostname(cfg,label);
    pthread_mutex_lock(&hostname_lock);snprintf(hostname_label,sizeof(hostname_label),"%s",label);pthread_mutex_unlock(&hostname_lock);
    if(pthread_create(&thread,NULL,mdns_loop,NULL))return -1;
    pthread_detach(thread);return 0;
}
