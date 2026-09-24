/* Host-only focused regression test for live-edge and discontinuity semantics.
 * Compile with: cc -O2 -pthread -o /tmp/test_fmp4_ring tests/test_fmp4_ring.c
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>

int joan_read_file(const char *path, unsigned char **out, size_t *len, size_t max)
{ (void)path;(void)out;(void)len;(void)max;return -1; }
int joan_random(void *out, size_t len)
{ memset(out,0,len);return 0; }
void joan_hex(const unsigned char *in, size_t len, char *out)
{ static const char digits[]="0123456789abcdef";size_t i;for(i=0;i<len;i++){out[2*i]=digits[in[i]>>4];out[2*i+1]=digits[in[i]&15];}out[2*len]=0; }
#include "../src/daemon/fmp4.c"

static void add(Stream *s, unsigned seq, time_t when)
{
    Fragment *f=&s->ring[s->ring_next++%RING];
    free(f->data);f->data=malloc(1);assert(f->data);
    f->data[0]=(unsigned char)seq;f->len=1;f->sequence=seq;f->published=when;
    f->keyframe=seq==1||seq==7;s->ring_bytes++;
    s->sequence=seq;
}
static int fetch(Stream *s, uint32_t after, unsigned *seq)
{
    unsigned char *data=NULL;size_t len=0;uint32_t n=0;
    int rc=joan_fmp4_fragment(s->id,after,&data,&len,&n,1);
    if(!rc){assert(len>=1 && data[0]>0);*seq=n;free(data);}
    return rc;
}
int main(void)
{
    Stream *s=&streams[0];unsigned seq=0,i;time_t now=time(NULL);
    snprintf(s->status,sizeof(s->status),"connected");
    for(i=1;i<=7;i++)add(s,i,now);
    assert(fetch(s,0,&seq)==0 && seq==7); /* newest IDR is decodable */
    assert(fetch(s,4,&seq)==0 && seq==5); /* continuity in ring */
    assert(fetch(s,2,&seq)==0 && seq==3); /* retained history */
    /* Bootstrap concatenates the most recent IDR and all dependent chunks. */
    s->ring[6].keyframe=0;
    {unsigned char *bytes=NULL;size_t len=0;uint32_t last=0;
     assert(joan_fmp4_fragment("main",0,&bytes,&len,&last,1)==0);
     assert(last==7 && len==7 && bytes[0]==1 && bytes[6]==7);free(bytes);}
    s->ring[0].keyframe=0;/* no retained IDR: bootstrap cannot decode */
    assert(fetch(s,0,&seq)!=0);
    s->ring[6].keyframe=1;
    s->ring[(s->ring_next-1)%RING].published=now-20;
    assert(fetch(s,0,&seq)==-2);          /* stale is not live */
    s->ring[(s->ring_next-1)%RING].published=now;
    s->ring[6].keyframe=0;s->ring[3].keyframe=1; /* a missing delta invalidates anchor chain */
    free(s->ring[4].data);s->ring[4].data=NULL;s->ring[4].len=0;
    assert(fetch(s,0,&seq)!=0);
    reset_stream(s);
    assert(!s->init && !s->init_len && !s->sample_count);
    assert(fetch(s,7,&seq)==-2);          /* old epoch must resync */

    /* RTP timestamps denote sample STARTS: the following AU determines the
     * previous duration, including the final sample at an IDR boundary. */
    {const unsigned char idr[]={0x65},pframe[]={0x41};
     s->sps_len=4;s->pps_len=1;memcpy(s->sps,"\x67\x64\x00\x32",4);s->pps[0]=0x68;
     assert(build_init(s)==0);
     snprintf(s->status,sizeof(s->status),"connected");
     assert(append_nal(s,idr,sizeof(idr))==0);finish_access(s,3000);
     assert(append_nal(s,pframe,sizeof(pframe))==0);finish_access(s,6000);
     assert(s->samples[0].duration==3000);
     assert(append_nal(s,pframe,sizeof(pframe))==0);finish_access(s,12000);
     assert(s->samples[1].duration==6000);
     assert(append_nal(s,idr,sizeof(idr))==0);finish_access(s,15000);
     assert(s->sample_count==1 && s->samples[0].flags==0x02000000);
     assert(s->decode_time==12000);
     assert(append_nal(s,pframe,sizeof(pframe))==0);finish_access(s,18000);
     assert(s->ring[0].keyframe==1); /* the new IDR chunk is independently decodable */
     assert(s->decode_time==15000);
     assert(accept_access_timestamp(s,18000)==0); /* duplicate */
     assert(accept_access_timestamp(s,12000)==0); /* recent replay */
     assert(accept_access_timestamp(s,15000)==0); /* advancing old pictures */
     assert(accept_access_timestamp(s,18000)==0); /* duplicate catches up */
     assert(accept_access_timestamp(s,21000)==1); /* next live picture */
     assert(accept_access_timestamp(s,0)==0);     /* short backward jump */
     assert(accept_access_timestamp(s,300000u)==-1); /* large clock jump */
     reset_stream(s);
    }
    /* Ninety 15fps AUs represent the OEM's six-second GOP. The first IDR
     * chunk is emitted on the next AU, while all later deltas flush in about
     * one-second chunks rather than waiting for the next IDR. */
    {const unsigned char idr[]={0x65},pframe[]={0x41};unsigned char *boot=NULL;
     size_t boot_len=0;uint32_t boot_seq=0;unsigned previous=0;
     s->sps_len=4;s->pps_len=1;memcpy(s->sps,"\x67\x64\x00\x32",4);s->pps[0]=0x68;
     assert(build_init(s)==0);snprintf(s->status,sizeof(s->status),"connected");
     previous=s->sequence;
     for(i=0;i<91;i++){
         assert(append_nal(s,i==0?idr:pframe,1)==0);
         finish_access(s,6000*i);
         if(i==1)assert(s->ring[0].keyframe&&s->sequence==previous+1);
     }
     assert(s->sequence-previous>=6 && s->sequence-previous<=9);
     assert(joan_fmp4_fragment("main",0,&boot,&boot_len,&boot_seq,1)==0);
     assert(boot_seq==s->sequence && boot_len>300);
     assert(boot[4]=='m' && boot[5]=='o' && boot[6]=='o' && boot[7]=='f');
     previous=0;
     for(i=0;i+7<boot_len;i++)if(!memcmp(boot+i+4,"moof",4))previous++;
     assert(previous>=6 && previous<=9);
     free(boot);
     assert(s->decode_time>=450000 && s->decode_time<=540000);
     reset_stream(s);
    }
    puts("fmp4 live ring, 6-second GOP chunking and RTP durations: PASS");
    return 0;
}
