#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef JOAN_NO_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

/* Deliberately tiny loopback-only MQTT 3.1.1 sink. It never resolves or opens
 * an upstream address. Telemetry PUBLISH packets are consumed and discarded;
 * only two explicit local DP namespaces may be published back to the OEM app. */
static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int running, listen_fd = -1, client_fd = -1, subscribed;
static char command_topic[256];
static char state[96] = "stopped";
#ifndef JOAN_NO_TLS
static mbedtls_ssl_context client_ssl;
static int client_tls;
#endif

static ssize_t mqtt_read(int fd, void *data, size_t len)
{
#ifndef JOAN_NO_TLS
    if (client_tls) {
        int rc = mbedtls_ssl_read(&client_ssl, data, len);
        return rc > 0 ? rc : -1;
    }
#endif
    return recv(fd, data, len, MSG_WAITALL);
}

static ssize_t mqtt_write(int fd, const void *data, size_t len)
{
#ifndef JOAN_NO_TLS
    if (client_tls) {
        int rc = mbedtls_ssl_write(&client_ssl, data, len);
        return rc > 0 ? rc : -1;
    }
#endif
    return send(fd, data, len, MSG_NOSIGNAL);
}

static int send_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) { ssize_t n = mqtt_write(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return -1; p += n; len -= (size_t)n; }
    return 0;
}

static int receive_packet(int fd, unsigned char *type, unsigned char *buf, size_t cap, size_t *len)
{
    unsigned char h, c; size_t rem = 0, mul = 1, i; ssize_t n;
    n = mqtt_read(fd, &h, 1); if (n != 1) return -1;
    for (i = 0; i < 4; ++i) { if (mqtt_read(fd, &c, 1) != 1) return -1; rem += (c & 127u) * mul; if (!(c & 128u)) break; mul *= 128u; }
    if (i == 4 || rem > cap) return -1;
    if (rem && mqtt_read(fd, buf, rem) != (ssize_t)rem) return -1;
    *type = h; *len = rem; return 0;
}

static void handle_client(int fd)
{
    unsigned char type, b[8192], reply[8]; size_t n;
    while (running && receive_packet(fd, &type, b, sizeof(b), &n) == 0) {
        switch (type >> 4) {
        case 1: /* CONNECT */
            { static const unsigned char connack[] = {0x20,0x02,0x00,0x00}; if (send_all(fd, connack, sizeof(connack))) return; }
            break;
        case 3: /* PUBLISH: telemetry is intentionally discarded. */
            if ((type & 6u) == 2u && n >= 4) { /* QoS1 PUBACK */
                size_t topic_len = ((size_t)b[0] << 8) | b[1];
                if (topic_len + 4 <= n) { reply[0]=0x40; reply[1]=2; reply[2]=b[2+topic_len]; reply[3]=b[3+topic_len]; if(send_all(fd,reply,4))return; }
            }
            break;
        case 8: /* SUBSCRIBE */
            if (n < 5) return;
            { size_t topic_len = ((size_t)b[2] << 8) | b[3];
              if (topic_len == 0 || topic_len >= sizeof(command_topic) ||
                  topic_len + 5 > n) return;
              memcpy(command_topic, b + 4, topic_len);
              command_topic[topic_len] = 0;
              if (strncmp(command_topic, "qaiot/mqtt/", 11) != 0) return;
            }
            subscribed = 1; reply[0]=0x90; reply[1]=3; reply[2]=b[0]; reply[3]=b[1]; reply[4]=0; if(send_all(fd,reply,5))return;
            break;
        case 12: { static const unsigned char pong[]={0xd0,0}; if(send_all(fd,pong,2))return; } break;
        case 14: return;
        default: break;
        }
    }
}

static void *broker(void *arg)
{
    JoanConfig *cfg = arg; struct sockaddr_in a;
#ifndef JOAN_NO_TLS
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config sc; mbedtls_x509_crt cert; mbedtls_pk_context key;
    char cert_path[512], key_path[512]; int tls_ready = 0;
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_config_init(&sc); mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key); mbedtls_ssl_init(&client_ssl);
    if (!cfg->plain_http) {
        snprintf(cert_path,sizeof(cert_path),"%s/tls-cert.pem",cfg->state_dir);
        snprintf(key_path,sizeof(key_path),"%s/tls-key.pem",cfg->state_dir);
        if (mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,
                                  (const unsigned char *)"joan-mqtt",9) ||
            mbedtls_x509_crt_parse_file(&cert,cert_path) ||
            mbedtls_pk_parse_keyfile(&key,key_path,NULL) ||
            mbedtls_ssl_config_defaults(&sc,MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) ||
            mbedtls_ssl_conf_own_cert(&sc,&cert,&key)) goto out;
        mbedtls_ssl_conf_rng(&sc,mbedtls_ctr_drbg_random,&drbg);
        tls_ready = 1;
    }
#endif
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) goto out;
    { int one=1; setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)); }
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons((uint16_t)cfg->mqtt_port);
    if (bind(listen_fd,(struct sockaddr*)&a,sizeof(a)) || listen(listen_fd,1)) goto out;
    pthread_mutex_lock(&mutex); snprintf(state,sizeof(state),"listening:127.0.0.1:%u",cfg->mqtt_port); pthread_mutex_unlock(&mutex);
    while (running) {
        int fd = accept(listen_fd,NULL,NULL); if (fd < 0) { if(errno==EINTR)continue; break; }
#ifndef JOAN_NO_TLS
        client_tls=0;
        if (tls_ready) {
            int rc;
            mbedtls_ssl_free(&client_ssl); mbedtls_ssl_init(&client_ssl);
            if (mbedtls_ssl_setup(&client_ssl,&sc)) { close(fd); continue; }
            mbedtls_ssl_set_bio(&client_ssl,&fd,mbedtls_net_send,mbedtls_net_recv,NULL);
            while ((rc=mbedtls_ssl_handshake(&client_ssl)) != 0 &&
                   (rc==MBEDTLS_ERR_SSL_WANT_READ || rc==MBEDTLS_ERR_SSL_WANT_WRITE)) {}
            if (rc) { mbedtls_ssl_free(&client_ssl); close(fd); continue; }
            client_tls=1;
        }
#endif
        pthread_mutex_lock(&mutex); client_fd=fd; subscribed=0;command_topic[0]=0; snprintf(state,sizeof(state),"connected"); pthread_mutex_unlock(&mutex);
        handle_client(fd);
        pthread_mutex_lock(&mutex);
#ifndef JOAN_NO_TLS
        if(client_tls){mbedtls_ssl_close_notify(&client_ssl);client_tls=0;}
#endif
        close(fd); client_fd=-1; subscribed=0;command_topic[0]=0; snprintf(state,sizeof(state),"listening:127.0.0.1:%u",cfg->mqtt_port); pthread_mutex_unlock(&mutex);
    }
out:
    pthread_mutex_lock(&mutex); if(listen_fd>=0)close(listen_fd);listen_fd=-1;snprintf(state,sizeof(state),"stopped");pthread_mutex_unlock(&mutex);
#ifndef JOAN_NO_TLS
    mbedtls_ssl_free(&client_ssl);mbedtls_pk_free(&key);mbedtls_x509_crt_free(&cert);
    mbedtls_ssl_config_free(&sc);mbedtls_ctr_drbg_free(&drbg);mbedtls_entropy_free(&entropy);
#endif
    free(cfg); return NULL;
}

int joan_mqtt_bridge_start(const JoanConfig *cfg)
{
    JoanConfig *copy;
    if (!cfg->mqtt_port) return 0;
    copy=malloc(sizeof(*copy)); if(!copy)return -1; *copy=*cfg;
    running=1; if(pthread_create(&thread,NULL,broker,copy)){running=0;free(copy);return -1;} return 0;
}

void joan_mqtt_bridge_stop(void)
{
    int fd,client; if(!running)return; running=0; pthread_mutex_lock(&mutex); fd=listen_fd;client=client_fd; pthread_mutex_unlock(&mutex); if(client>=0)shutdown(client,SHUT_RDWR);if(fd>=0)shutdown(fd,SHUT_RDWR); pthread_join(thread,NULL);
}

static size_t mqtt_remaining(unsigned char *out,size_t n){size_t i=0;do{unsigned char c=n%128;n/=128;if(n)c|=128;out[i++]=c;}while(n&&i<4);return i;}
int joan_mqtt_bridge_publish(const char *topic,const void *payload,size_t len)
{
    unsigned char packet[8192],rem[4]; size_t tn,rl,rn,total; int rc=-1;
    if (!topic || (strncmp(topic,"local/dp/security",17) && strncmp(topic,"local/dp/ptz",12))) return -1;
    pthread_mutex_lock(&mutex);
    if(client_fd<0||!subscribed||!command_topic[0]){pthread_mutex_unlock(&mutex);return -1;}
    tn=strlen(command_topic); rl=2+tn+len; rn=mqtt_remaining(rem,rl); total=1+rn+rl; if(total>sizeof(packet)||tn>65535){pthread_mutex_unlock(&mutex);return -1;}
    packet[0]=0x30;memcpy(packet+1,rem,rn);packet[1+rn]=tn>>8;packet[2+rn]=tn;memcpy(packet+3+rn,command_topic,tn);memcpy(packet+3+rn+tn,payload,len);
    rc=send_all(client_fd,packet,total); pthread_mutex_unlock(&mutex); return rc;
}
const char *joan_mqtt_bridge_status(void){return state;}
