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
    s->sequence=seq;
}
static int fetch(Stream *s, uint32_t after, unsigned *seq)
{
    unsigned char *data=NULL;size_t len=0;uint32_t n=0;
    int rc=joan_fmp4_fragment(s->id,after,&data,&len,&n,1);
    if(!rc){assert(len==1 && data[0]==(unsigned char)n);*seq=n;free(data);}
    return rc;
}
int main(void)
{
    Stream *s=&streams[0];unsigned seq=0,i;time_t now=time(NULL);
    snprintf(s->status,sizeof(s->status),"connected");
    for(i=1;i<=7;i++)add(s,i,now);
    assert(fetch(s,0,&seq)==0 && seq==7); /* bootstrap at live edge */
    assert(fetch(s,4,&seq)==0 && seq==5); /* continuity in ring */
    assert(fetch(s,2,&seq)==-2);          /* lag exceeds ring */
    s->ring[(s->ring_next-1)%RING].published=now-20;
    assert(fetch(s,0,&seq)==-2);          /* stale is not live */
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
     assert(s->samples[2].duration==3000);
     assert(s->decode_time==12000);
     reset_stream(s);
    }
    puts("fmp4 live ring and RTP durations: PASS");
    return 0;
}
