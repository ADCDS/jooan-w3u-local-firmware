/* Small dependency-free SHA-256/HMAC/PBKDF2 core for uClibc host/device parity. */
#include "joan_daemon.h"
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t bits; unsigned char b[64]; size_t n; } S256;
static uint32_t rr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static const uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void init(S256*s){static const uint32_t h[]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->h,h,32);s->bits=0;s->n=0;}
static void block(S256*s,const unsigned char*p){uint32_t w[64],a,b,c,d,e,f,g,h,t1,t2;unsigned i;for(i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];for(;i<64;i++){uint32_t x=w[i-15],y=w[i-2];w[i]=(rr(x,7)^rr(x,18)^(x>>3))+w[i-16]+(rr(y,17)^rr(y,19)^(y>>10))+w[i-7];}a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];for(i=0;i<64;i++){t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+K[i]+w[i];t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;}
static void upd(S256*s,const void*v,size_t n){const unsigned char*p=v;s->bits+=(uint64_t)n*8;while(n){size_t z=64-s->n;if(z>n)z=n;memcpy(s->b+s->n,p,z);s->n+=z;p+=z;n-=z;if(s->n==64){block(s,s->b);s->n=0;}}}
static void fin(S256*s,unsigned char o[32]){unsigned i;s->b[s->n++]=0x80;if(s->n>56){memset(s->b+s->n,0,64-s->n);block(s,s->b);s->n=0;}memset(s->b+s->n,0,56-s->n);for(i=0;i<8;i++)s->b[63-i]=(unsigned char)(s->bits>>(i*8));block(s,s->b);for(i=0;i<8;i++){o[i*4]=s->h[i]>>24;o[i*4+1]=s->h[i]>>16;o[i*4+2]=s->h[i]>>8;o[i*4+3]=s->h[i];}}
void joan_sha256(const void*d,size_t n,unsigned char o[32]){S256 s;init(&s);upd(&s,d,n);fin(&s,o);}
static void hmac(const unsigned char*k,size_t kn,const void*d,size_t n,unsigned char o[32]){unsigned char kb[64]={0},x[32],ip[64],op[64];S256 s;size_t i;if(kn>64){joan_sha256(k,kn,x);k=x;kn=32;}memcpy(kb,k,kn);for(i=0;i<64;i++){ip[i]=kb[i]^0x36;op[i]=kb[i]^0x5c;}init(&s);upd(&s,ip,64);upd(&s,d,n);fin(&s,x);init(&s);upd(&s,op,64);upd(&s,x,32);fin(&s,o);}
void joan_pbkdf2_sha256(const void*pw,size_t pn,const unsigned char*salt,size_t sn,uint32_t rounds,unsigned char*out,size_t on){unsigned char msg[128],u[32],t[32];uint32_t bi=1,r;size_t take,i;if(sn>123||!rounds)return;while(on){memcpy(msg,salt,sn);msg[sn]=bi>>24;msg[sn+1]=bi>>16;msg[sn+2]=bi>>8;msg[sn+3]=bi;hmac(pw,pn,msg,sn+4,u);memcpy(t,u,32);for(r=1;r<rounds;r++){hmac(pw,pn,u,32,u);for(i=0;i<32;i++)t[i]^=u[i];}take=on<32?on:32;memcpy(out,t,take);out+=take;on-=take;bi++;}memset(u,0,sizeof(u));memset(t,0,sizeof(t));}
