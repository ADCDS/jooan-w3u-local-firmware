/* SPDX-License-Identifier: MIT */
#include "jipc.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* This small bounded XML reader has no DTD, entity expansion or external I/O. */
typedef struct { const char *p; size_t n; } span;
typedef struct { span name, value; } attribute;
typedef struct { span name, text; int parent, child; unsigned attrs; attribute attr[8]; } element;
typedef struct { element el[192]; unsigned count; int action, header; } xml;
typedef struct { char *out; size_t capacity, used; int failed; } writer;
static int xml_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int name_char(unsigned char c) { return isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.'; }
static span local(span s) { for (size_t i = 0; i < s.n; ++i) if (s.p[i] == ':') { s.p += i + 1; s.n -= i + 1; break; } return s; }
static int named(span s, const char *name) { s = local(s); return strlen(name) == s.n && !memcmp(s.p, name, s.n); }
static int same(span a, span b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }
static int decode(span s, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < s.n; ++i) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '&') {
            size_t end = i + 1;
            while (end < s.n && s.p[end] != ';' && end - i < 12) ++end;
            if (end == s.n || s.p[end] != ';') return -1;
            span e = { s.p + i + 1, end - i - 1 };
            if (same(e, (span){"amp",3})) c = '&';
            else if (same(e,(span){"lt",2})) c = '<';
            else if (same(e,(span){"gt",2})) c = '>';
            else if (same(e,(span){"quot",4})) c = '"';
            else if (same(e,(span){"apos",4})) c = '\'';
            else return -1;
            i = end;
        }
        if (!c || (c < 32 && !xml_space((char)c)) || n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}
static int parse_xml(const char *body, xml *doc) {
    const char *p = body, *end = body + strlen(body);
    int stack[32], depth = 0, root_seen = 0;
    memset(doc, 0, sizeof(*doc)); doc->action = doc->header = -1;
    if ((size_t)(end - body) > JIPC_MAX_JSON) return -1;
    while (p < end) {
        if (*p != '<') {
            const char *q = p; while (q < end && *q != '<') ++q;
            if (!depth) for (const char *r = p; r < q; ++r) if (!xml_space(*r)) return -1;
            p = q; continue;
        }
        if (!strncmp(p, "<!--", 4)) {
            const char *q = strstr(p + 4, "-->"); if (!q) return -1; p = q + 3; continue;
        }
        if (!strncmp(p, "<?xml", 5) && !root_seen && !depth) {
            const char *q = strstr(p + 5, "?>"); if (!q) return -1; p = q + 2; continue;
        }
        if (p + 1 == end || p[1] == '!' || p[1] == '?') return -1;
        const char *tag = p++;
        int closing = *p == '/'; if (closing) ++p;
        const char *name = p; while (p < end && name_char((unsigned char)*p)) ++p;
        if (p == name || p - name > 128) return -1;
        span qname = {name, (size_t)(p-name)};
        if (closing) {
            while (p < end && xml_space(*p)) ++p;
            if (p == end || *p++ != '>' || !depth) return -1;
            element *e = &doc->el[stack[--depth]];
            if (!same(e->name,qname)) return -1;
            e->text.n = (size_t)(tag - e->text.p);
            continue;
        }
        if (doc->count == sizeof(doc->el)/sizeof(doc->el[0]) || depth == 32 || (!depth && root_seen)) return -1;
        int idx = (int)doc->count++;
        element *e = &doc->el[idx]; e->name = qname; e->parent = depth ? stack[depth-1] : -1;
        if (depth) doc->el[e->parent].child = 1; else root_seen = 1;
        int empty = 0;
        while (p < end) {
            while (p < end && xml_space(*p)) ++p;
            if (p == end) return -1;
            if (*p == '>') { ++p; break; }
            if (*p == '/' && p + 1 < end && p[1] == '>') { p += 2; empty = 1; break; }
            const char *a = p; while (p < end && name_char((unsigned char)*p)) ++p;
            if (p == a || p - a > 128) return -1;
            span aname = {a, (size_t)(p-a)};
            while (p < end && xml_space(*p)) ++p;
            if (p == end || *p++ != '=') return -1;
            while (p < end && xml_space(*p)) ++p;
            if (p == end || (*p != '\'' && *p != '"')) return -1;
            char quote = *p++; const char *v = p;
            while (p < end && *p != quote && *p != '<') ++p;
            if (p == end || *p != quote) return -1;
            for (unsigned j = 0; j < e->attrs; ++j) if (same(e->attr[j].name,aname)) return -1;
            if (!(aname.n == 5 && !memcmp(aname.p,"xmlns",5)) && !(aname.n > 6 && !memcmp(aname.p,"xmlns:",6))) {
                if (e->attrs == 8) return -1;
                e->attr[e->attrs++] = (attribute){aname,{v,(size_t)(p-v)}};
            }
            ++p;
        }
        e->text = (span){p,0};
        if (!empty) stack[depth++] = idx;
    }
    if (depth || !doc->count || !named(doc->el[0].name,"Envelope")) return -1;
    int body_idx = -1;
    for (unsigned i = 1; i < doc->count; ++i) if (doc->el[i].parent == 0) {
        if (named(doc->el[i].name,"Body")) { if (body_idx >= 0) return -1; body_idx = (int)i; }
        else if (named(doc->el[i].name,"Header")) { if (doc->header >= 0) return -1; doc->header = (int)i; }
        else return -1;
    }
    if (body_idx < 0) return -1;
    for (unsigned i = 1; i < doc->count; ++i) if (doc->el[i].parent == body_idx) {
        if (doc->action >= 0) return -1;
        doc->action = (int)i;
    }
    return doc->action < 0 ? -1 : 0;
}
static int child(const xml *d, int parent, const char *name) {
    int found = -1;
    for (unsigned i = 0; i < d->count; ++i) if (d->el[i].parent == parent && named(d->el[i].name,name)) {
        if (found >= 0) return -1;
        found = (int)i;
    }
    return found;
}
static int text_value(const xml *d, int parent, const char *name, char *out, size_t cap) {
    int i = child(d,parent,name);
    if (i < 0 || d->el[i].child) return -1;
    return decode(d->el[i].text,out,cap);
}
static int attr_value(const xml *d, int node, const char *name, char *out, size_t cap) {
    if (node < 0) return -1;
    for (unsigned i = 0; i < d->el[node].attrs; ++i) if (named(d->el[node].attr[i].name,name)) return decode(d->el[node].attr[i].value,out,cap);
    return -1;
}
static void put(writer *w, const char *format, ...) {
    if (w->failed) return;
    va_list ap; va_start(ap,format); int n = vsnprintf(w->out+w->used,w->capacity-w->used,format,ap); va_end(ap);
    if (n < 0 || (size_t)n >= w->capacity-w->used) { w->failed=1; return; }
    w->used += (size_t)n;
}
static void escaped(writer *w, const char *s) {
    for (; *s; ++s) {
        if (*s=='&') put(w,"&amp;"); else if (*s=='<') put(w,"&lt;"); else if (*s=='>') put(w,"&gt;");
        else if (*s=='\"') put(w,"&quot;"); else if (*s=='\'') put(w,"&apos;"); else put(w,"%c",*s);
    }
}
static void envelope(writer *w) {
    put(w,"<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body>");
}
static int fault(writer *w, const char *code, const char *reason) {
    w->used=0; w->failed=0; envelope(w);
    put(w,"<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:%s</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">",code);
    escaped(w,reason); put(w,"</s:Text></s:Reason></s:Fault></s:Body></s:Envelope>");
    return 500;
}
/* Convert a civil UTC date without relying on process timezone or timegm(). */
static int64_t civil_days(int y,unsigned m,unsigned d) {
    y -= m <= 2; int era=(y>=0?y:y-399)/400; unsigned yo=(unsigned)(y-era*400);
    unsigned adjusted=m>2?m-3:m+9;
    unsigned doy=(153*adjusted+2)/5+d-1;
    return (int64_t)era*146097+(int64_t)(yo*365+yo/4-yo/100+doy)-719468;
}
static int created_time(const char *s, int64_t *value) {
    int y,m,d,h,mi,se,n=0;
    if (sscanf(s,"%4d-%2d-%2dT%2d:%2d:%2d%n",&y,&m,&d,&h,&mi,&se,&n)!=6 || n!=19) return -1;
    if (y<1970||y>9999||m<1||m>12||d<1||h<0||h>23||mi<0||mi>59||se<0||se>59) return -1;
    static const unsigned md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned max=md[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0)); if ((unsigned)d>max) return -1;
    const char *p=s+n;
    if (*p=='.') { unsigned digits=0; ++p; while (isdigit((unsigned char)*p)) { ++p; if (++digits>9)return -1; } if(!digits)return -1; }
    int offset=0;
    if(*p=='Z'&&!p[1]){}
    else if((*p=='+'||*p=='-')&&strlen(p)==6&&p[3]==':'&&
            isdigit((unsigned char)p[1])&&isdigit((unsigned char)p[2])&&
            isdigit((unsigned char)p[4])&&isdigit((unsigned char)p[5])){
        int hours=(p[1]-'0')*10+p[2]-'0',minutes=(p[4]-'0')*10+p[5]-'0';
        if(hours>14||minutes>59||(hours==14&&minutes))return -1;
        offset=(hours*3600+minutes*60)*(*p=='+'?1:-1);
    }else return -1;
    *value=civil_days(y,(unsigned)m,(unsigned)d)*86400+h*3600+mi*60+se-offset;
    return 0;
}
/* The daemon owns one camera. Keep used tokens across SIGHUP/app recreation. */
typedef struct { unsigned char nonce_hash[20]; time_t expires;  int64_t created; } nonce_entry;
static nonce_entry nonces[1024];
static unsigned nonce_next;
static int64_t nonce_floor;
static pthread_mutex_t nonce_mutex=PTHREAD_MUTEX_INITIALIZER;
int jipc_onvif_public_time(const char *body) {
    xml d; return parse_xml(body,&d)==0 && named(d.el[d.action].name,"GetSystemDateAndTime");
}
int jipc_onvif_authorize(jipc_app *app,const char *body) {
    xml d; if(parse_xml(body,&d)<0 || d.header<0)return 0;
    int security=child(&d,d.header,"Security"),token=child(&d,security,"UsernameToken");
    if(security<0||token<0)return 0;
    char user[64],pass[128],nonce[256],created[64],type[256];
    int p=child(&d,token,"Password");
    if(text_value(&d,token,"Username",user,sizeof(user))<0||text_value(&d,token,"Password",pass,sizeof(pass))<0||
       text_value(&d,token,"Nonce",nonce,sizeof(nonce))<0||text_value(&d,token,"Created",created,sizeof(created))<0||
       attr_value(&d,p,"Type",type,sizeof(type))<0)return 0;
    if(strcmp(type,"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest"))return 0;
    int64_t stamp=0; time_t now=time(NULL); if(now==(time_t)-1||created_time(created,&stamp)<0||stamp<(int64_t)now-300||stamp>(int64_t)now+300)return 0;
    unsigned char raw[192],digest[20],hash[20],input[192+64+129]; char expected[64];
    int bytes=jipc_base64_decode(nonce,raw,sizeof(raw)); if(bytes<16||bytes>(int)sizeof(raw))return 0;
    jipc_config c;jipc_config_copy(app,&c);
    size_t cr=strlen(created),pw=strlen(c.password),len=(size_t)bytes+cr+pw;
    memcpy(input,raw,(size_t)bytes);memcpy(input+bytes,created,cr);memcpy(input+bytes+cr,c.password,pw);
    jipc_sha1(input,len,digest);jipc_sha1(raw,(size_t)bytes,hash);
    memset(input,0,sizeof(input));
    if(jipc_base64(digest,sizeof(digest),expected,sizeof(expected))<0)return 0;
    int valid=jipc_equal(user,c.username)&jipc_equal(pass,expected); memset(c.password,0,sizeof(c.password));
    if(!valid)return 0;
    /* Replay protection. A token stays acceptable for 300 s, but Frigate polls
     * GetStatus in a tight loop while the motor moves, so the table cannot hold them
     * all, and refusing everyone once it is full takes PTZ down for minutes. It is a
     * ring, then: a token too old to be in it may be a replay, so anything created no
     * later than the newest one forgotten is refused, and fresh tokens always pass. */
    pthread_mutex_lock(&nonce_mutex);
    if(nonce_floor>(int64_t)now)nonce_floor=(int64_t)now-1; /* the clock stepped back */
    if(stamp<=nonce_floor){pthread_mutex_unlock(&nonce_mutex);return 0;}
    for(unsigned i=0;i<1024;++i)
        if(nonces[i].expires>=now&&!memcmp(nonces[i].nonce_hash,hash,20)){pthread_mutex_unlock(&nonce_mutex);return 0;}
    nonce_entry *slot=&nonces[nonce_next];nonce_next=(nonce_next+1)%1024;
    if(slot->expires>=now&&slot->created>nonce_floor)nonce_floor=slot->created;
    slot->expires=(time_t)(stamp+301);slot->created=stamp;memcpy(slot->nonce_hash,hash,20);
    pthread_mutex_unlock(&nonce_mutex);return 1;
}
static int channel(const xml *d,const jipc_config *c,unsigned *out) {
    char tok[64];if(text_value(d,d->action,"ProfileToken",tok,sizeof(tok))<0)return -1;
    unsigned ch;
    if(!strcmp(tok,"main0"))ch=0;else if(!strcmp(tok,"sub0"))ch=1;else if(!strcmp(tok,"main1"))ch=2;else if(!strcmp(tok,"sub1"))ch=3;else return -1;
    if(ch>=c->sensors*2||ch>=JIPC_VIDEO_STREAMS)return -1;
    *out=ch;return 0;
}
static const char *profile_token(unsigned ch){static const char *t[]={"main0","sub0","main1","sub1"};return t[ch];}
static int host_name(const char *host,char *out,size_t cap) {
    size_t n=strlen(host);if(!n||n>240)return -1;
    for(size_t i=0;i<n;++i)if(!isalnum((unsigned char)host[i])&&host[i]!='.'&&host[i]!='-'&&host[i]!=':'&&host[i]!='['&&host[i]!=']')return -1;
    const char *stop=host+n;
    if(host[0]=='['){const char *close=strchr(host,']');if(!close)return -1;stop=close+1;if(*stop&&*stop!=':')return -1;}
    else {const char *colon=strchr(host,':');if(colon)stop=colon;}
    size_t z=(size_t)(stop-host);if(!z||z>=cap)return -1;memcpy(out,host,z);out[z]=0;return 0;
}
static void date_xml(writer *w,const struct tm *t){put(w,"<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>",t->tm_hour,t->tm_min,t->tm_sec,t->tm_year+1900,t->tm_mon+1,t->tm_mday);}
static void video_config(jipc_app *app,writer *w,const jipc_config *c,unsigned ch){
    unsigned width=jipc_video_width(c,ch),height=jipc_video_height(c,ch);
    unsigned char sps[512],pps[512];size_t sn=sizeof(sps),pn=sizeof(pps);const char *h264=NULL;
    if(jipc_codec_parameters(app,ch,sps,&sn,pps,&pn)==0&&sn>=2){
        if(sps[1]==66)h264="Baseline";else if(sps[1]==77)h264="Main";else if(sps[1]==88)h264="Extended";else if(sps[1]==100)h264="High";
    }
    put(w,"<tt:VideoEncoderConfiguration token=\"enc%u\"><tt:Name>H264 %s</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>%u</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%u</tt:BitrateLimit></tt:RateControl>",ch,profile_token(ch),width,height,c->fps,jipc_video_bitrate(c,ch));
    /* The hardware encoder is configured with the configured GOP. Replay files
     * have no configured GOP; their authoritative codec parameters are in SDP. */
    if(h264&&!strcmp(c->backend,"imp"))put(w,"<tt:H264><tt:GovLength>%u</tt:GovLength><tt:H264Profile>%s</tt:H264Profile></tt:H264>",c->gop_frames?c->gop_frames:c->fps*2,h264);
    put(w,"<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>");
}
static void ptz_config(writer *w,const jipc_config *c,const char *tag){
    put(w,"<%s token=\"ptz0\"><tt:Name>Camera motor</tt:Name><tt:UseCount>%u</tt:UseCount><tt:NodeToken>ptz0</tt:NodeToken><tt:DefaultAbsolutePantTiltPositionSpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace</tt:DefaultAbsolutePantTiltPositionSpace><tt:DefaultRelativePanTiltTranslationSpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace</tt:DefaultRelativePanTiltTranslationSpace><tt:DefaultContinuousPanTiltVelocitySpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:DefaultContinuousPanTiltVelocitySpace><tt:DefaultPTZTimeout>%s</tt:DefaultPTZTimeout></%s>",tag,2u,c->onvif_nudge?"PT0.35S":"PT1.2S",tag);
}
#define PT_SPACES "http://www.onvif.org/ver10/tptz/PanTiltSpaces/"
/* tt:PTZSpaces is a sequence too: absolute, relative, continuous, speed. */
static void ptz_spaces(writer *w){
    const char *range="<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>";
    put(w,"<tt:AbsolutePanTiltPositionSpace><tt:URI>" PT_SPACES "PositionGenericSpace</tt:URI>%s</tt:AbsolutePanTiltPositionSpace>",range);
    put(w,"<tt:RelativePanTiltTranslationSpace><tt:URI>" PT_SPACES "TranslationGenericSpace</tt:URI>%s</tt:RelativePanTiltTranslationSpace>",range);
    put(w,"<tt:RelativePanTiltTranslationSpace><tt:URI>" PT_SPACES "TranslationSpaceFov</tt:URI>%s</tt:RelativePanTiltTranslationSpace>",range);
    put(w,"<tt:ContinuousPanTiltVelocitySpace><tt:URI>" PT_SPACES "VelocityGenericSpace</tt:URI>%s</tt:ContinuousPanTiltVelocitySpace>",range);
    put(w,"<tt:PanTiltSpeedSpace><tt:URI>" PT_SPACES "GenericSpeedSpace</tt:URI><tt:XRange><tt:Min>0</tt:Min><tt:Max>1</tt:Max></tt:XRange></tt:PanTiltSpeedSpace>");
}
static void profile(jipc_app *app,writer *w,const jipc_config *c,unsigned ch,const char *tag){
    put(w,"<trt:%s token=\"%s\" fixed=\"true\"><tt:Name>Sensor %u %s</tt:Name><tt:VideoSourceConfiguration token=\"source%u\"><tt:Name>Sensor %u</tt:Name><tt:UseCount>2</tt:UseCount><tt:SourceToken>sensor%u</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"%u\" height=\"%u\"/></tt:VideoSourceConfiguration>",tag,profile_token(ch),ch/2,ch%2?(c->main_only?"main (shared)":"sub"):"main",ch/2,ch/2,ch/2,jipc_video_width(c,(ch/2)*2),jipc_video_height(c,(ch/2)*2));
    /* tt:Profile is a sequence: VideoSource, AudioSource, VideoEncoder,
     * AudioEncoder, ..., PTZ. Clients that bind by position (zeep, used by
     * Frigate) drop elements that come out of order. */
    if(c->audio)put(w,"<tt:AudioSourceConfiguration token=\"audio_source\"><tt:Name>Microphone</tt:Name><tt:UseCount>%u</tt:UseCount><tt:SourceToken>microphone</tt:SourceToken></tt:AudioSourceConfiguration>",c->sensors*2);
    video_config(app,w,c,ch);
    if(c->audio)put(w,"<tt:AudioEncoderConfiguration token=\"audio_encoder\"><tt:Name>G711 A-law</tt:Name><tt:UseCount>%u</tt:UseCount><tt:Encoding>G711</tt:Encoding><tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate><tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:AudioEncoderConfiguration>",c->sensors*2);
    if(ch<2)ptz_config(w,c,"tt:PTZConfiguration");
    put(w,"</trt:%s>",tag);
}
static int number(const char *text,double *out){char *end;errno=0;double v=strtod(text,&end);if(errno||end==text||*end||!isfinite(v)||v < -1||v > 1)return -1;*out=v;return 0;}
static int slot_token(const char *s){if(strncmp(s,"preset",6))return -1;char *end;errno=0;long v=strtol(s+6,&end,10);return errno||*end||v<1||v>16?-1:(int)v;}
static int control(jipc_app *app,writer *w,const char *operation,int a,int b){char error[256]={0};if(jipc_backend_control(app,operation,a,b,error,sizeof(error))<0)return fault(w,"ActionNotSupported",error[0]?error:"Backend does not support this action");return 0;}
/* Imaging (ver20): brightness, saturation, contrast and sharpness of each
 * lens on ONVIF's 0..100 scale (0..255 on the ISP), and the IR-cut mode:
 * ON = day (filter in), OFF = night, AUTO = automatic or scheduled. Changes
 * go through the persisted controls, like the admin UI's. */
static int to_onvif(int v){return (v*100+127)/255;}
static int imaging_lens(const xml *d,const jipc_config *c,unsigned *lens){
    char token[64];if(text_value(d,d->action,"VideoSourceToken",token,sizeof(token))<0)return -1;
    if(!strcmp(token,"sensor0"))*lens=0;else if(!strcmp(token,"sensor1")&&c->sensors>1)*lens=1;else return -1;
    return 0;
}
static int imaging(jipc_app *app,writer *w,const xml *d,const jipc_config *c,span action){
    unsigned lens=0;
    if(named(action,"GetServiceCapabilities")){put(w,"<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization=\"false\" Presets=\"false\" AdaptablePreset=\"false\"/></timg:GetServiceCapabilitiesResponse>");return 0;}
    if(imaging_lens(d,c,&lens)<0)return fault(w,"InvalidArgVal","Unknown VideoSourceToken");
    if(named(action,"GetImagingSettings")){
        int v[4],mode;if(jipc_controls_picture(app,lens,v,&mode)<0)return fault(w,"Action","Image settings are unavailable");
        put(w,"<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>%d</tt:Brightness><tt:ColorSaturation>%d</tt:ColorSaturation><tt:Contrast>%d</tt:Contrast><tt:IrCutFilter>%s</tt:IrCutFilter><tt:Sharpness>%d</tt:Sharpness></timg:ImagingSettings></timg:GetImagingSettingsResponse>",
            to_onvif(v[0]),to_onvif(v[1]),to_onvif(v[2]),mode==0?"ON":mode==1?"OFF":"AUTO",to_onvif(v[3]));
    }else if(named(action,"GetOptions")){
        put(w,"<timg:GetOptionsResponse><timg:ImagingOptions>");
        const char *names[]={"Brightness","ColorSaturation","Contrast"};
        for(unsigned i=0;i<3;++i)put(w,"<tt:%s><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:%s>",names[i],names[i]);
        put(w,"<tt:IrCutFilterModes>ON</tt:IrCutFilterModes><tt:IrCutFilterModes>OFF</tt:IrCutFilterModes><tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes><tt:Sharpness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Sharpness></timg:ImagingOptions></timg:GetOptionsResponse>");
    }else if(named(action,"GetMoveOptions")){put(w,"<timg:GetMoveOptionsResponse><timg:MoveOptions/></timg:GetMoveOptionsResponse>");
    }else if(named(action,"SetImagingSettings")){
        int settings=child(d,d->action,"ImagingSettings");if(settings<0)return fault(w,"InvalidArgVal","ImagingSettings is required");
        const char *names[]={"Brightness","ColorSaturation","Contrast","Sharpness"},*keys[]={"brightness","saturation","contrast","sharpness"};
        char json[512],text[64],*at=json;size_t left=sizeof(json);int n=snprintf(at,left,"{\"picture\":[%s{",lens?"null,":"");at+=n;left-=(size_t)n;int first=1;
        for(unsigned i=0;i<4;++i){
            if(child(d,settings,names[i])<0)continue;
            double v;char *end;if(text_value(d,settings,names[i],text,sizeof(text))<0)return fault(w,"InvalidArgVal","Invalid imaging value");
            errno=0;v=strtod(text,&end);if(errno||end==text||*end||!isfinite(v)||v<0||v>100)return fault(w,"InvalidArgVal","Imaging values are 0 to 100");
            n=snprintf(at,left,"%s\"%s\":%d",first?"":",",keys[i],(int)(v*255.0/100.0+0.5));if(n<0||(size_t)n>=left)return fault(w,"Action","Request too large");at+=n;left-=(size_t)n;first=0;
        }
        n=snprintf(at,left,"}]");if(n<0||(size_t)n>=left)return fault(w,"Action","Request too large");at+=n;left-=(size_t)n;
        if(child(d,settings,"IrCutFilter")>=0){
            if(text_value(d,settings,"IrCutFilter",text,sizeof(text))<0)return fault(w,"InvalidArgVal","Invalid IrCutFilter");
            const char *mode=!strcmp(text,"ON")?"day":!strcmp(text,"OFF")?"night":!strcmp(text,"AUTO")?"auto":NULL;
            if(!mode)return fault(w,"InvalidArgVal","IrCutFilter must be ON, OFF or AUTO");
            n=snprintf(at,left,",\"night\":{\"mode\":\"%s\"}",mode);if(n<0||(size_t)n>=left)return fault(w,"Action","Request too large");at+=n;left-=(size_t)n;
        }
        if(left<2)return fault(w,"Action","Request too large");
        strcpy(at,"}");
        char error[256]={0};if(jipc_controls_patch(app,json,error,sizeof(error))<0)return fault(w,"InvalidArgVal",error[0]?error:"Rejected");
        put(w,"<timg:SetImagingSettingsResponse/>");
    }else return fault(w,"ActionNotSupported","Action is not implemented by this imaging service");
    return 0;
}
int jipc_onvif(jipc_app *app,const char *path,const char *body,const char *host,char *out,size_t capacity){
    writer w={out,capacity,0,0};xml d;jipc_config c;char hostname[256],base[384];unsigned ch=0;
    if(!capacity)return 500;
    if(parse_xml(body,&d)<0)return fault(&w,"InvalidArgVal","Malformed or unsupported XML");
    jipc_config_copy(app,&c);
    if(host_name(host,hostname,sizeof(hostname))<0)return fault(&w,"InvalidArgVal","Invalid Host");
    snprintf(base,sizeof(base),"%s://%s:%u",c.tls_cert[0]?"https":"http",hostname,c.http_port);
    span action=d.el[d.action].name;envelope(&w);
    if(named(action,"GetSystemDateAndTime")){
        time_t now=time(NULL);struct tm utc;if(!gmtime_r(&now,&utc))return fault(&w,"Action","Clock unavailable");
        put(&w,"<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>%s</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone><tt:UTCDateTime>",c.ntp_server[0]?"NTP":"Manual");date_xml(&w,&utc);put(&w,"</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
    }else if(named(action,"GetDeviceInformation")){
        put(&w,"<tds:GetDeviceInformationResponse><tds:Manufacturer>jooanipc-open</tds:Manufacturer><tds:Model>JOOAN W3-U local replacement</tds:Model><tds:FirmwareVersion>%s</tds:FirmwareVersion><tds:SerialNumber>local-camera</tds:SerialNumber><tds:HardwareId>Ingenic-T23</tds:HardwareId></tds:GetDeviceInformationResponse>",JIPC_VERSION);
    }else if(named(action,"GetCapabilities")){
        put(&w,"<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>%s/onvif/device_service</tt:XAddr><tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration><tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>false</tt:DiscoveryBye><tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup><tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade><tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tt:SupportedVersions></tt:System><tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>%s</tt:TLS1.2><tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig><tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken><tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security></tt:Device><tt:Imaging><tt:XAddr>%s/onvif/imaging_service</tt:XAddr></tt:Imaging><tt:Media><tt:XAddr>%s/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media><tt:PTZ><tt:XAddr>%s/onvif/ptz_service</tt:XAddr></tt:PTZ></tds:Capabilities></tds:GetCapabilitiesResponse>",base,c.tls_cert[0]?"true":"false",base,base,base);
    }else if(named(action,"GetServices")){
        put(&w,"<tds:GetServicesResponse>");const char *ns[]={"http://www.onvif.org/ver10/device/wsdl","http://www.onvif.org/ver10/media/wsdl","http://www.onvif.org/ver20/ptz/wsdl","http://www.onvif.org/ver20/imaging/wsdl"};const char *service[]={"device_service","media_service","ptz_service","imaging_service"};
        for(unsigned i=0;i<4;++i)put(&w,"<tds:Service><tds:Namespace>%s</tds:Namespace><tds:XAddr>%s/onvif/%s</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>4</tt:Minor></tds:Version></tds:Service>",ns[i],base,service[i]);
        put(&w,"</tds:GetServicesResponse>");
    }else if(!strcmp(path,"/onvif/imaging_service")){
        int status=imaging(app,&w,&d,&c,action);if(status)return status;
    }else if(named(action,"GetServiceCapabilities")){
        if(!strcmp(path,"/onvif/ptz_service"))put(&w,"<tptz:GetServiceCapabilitiesResponse><tptz:Capabilities EFlip=\"false\" Reverse=\"false\" GetCompatibleConfigurations=\"true\" MoveStatus=\"true\" StatusPosition=\"true\"/></tptz:GetServiceCapabilitiesResponse>");
        else if(!strcmp(path,"/onvif/media_service"))put(&w,"<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\"><trt:ProfileCapabilities MaximumNumberOfProfiles=\"%u\"/><trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\"/></trt:Capabilities></trt:GetServiceCapabilitiesResponse>",c.sensors*2);
        else put(&w,"<tds:GetServiceCapabilitiesResponse><tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/><tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"%s\" UsernameToken=\"true\" HttpDigest=\"false\" X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" RELToken=\"false\"/><tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" RemoteDiscovery=\"false\" SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\"/></tds:Capabilities></tds:GetServiceCapabilitiesResponse>",c.tls_cert[0]?"true":"false");
    }else if(named(action,"GetConfigurations")||named(action,"GetConfiguration")||named(action,"GetCompatibleConfigurations")){
        if(named(action,"GetConfiguration")){char token[64];if(text_value(&d,d.action,"PTZConfigurationToken",token,sizeof(token))<0||strcmp(token,"ptz0"))return fault(&w,"InvalidArgVal","Unknown PTZConfigurationToken");}
        if(named(action,"GetCompatibleConfigurations")&&channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        const char *op=named(action,"GetConfigurations")?"GetConfigurations":named(action,"GetConfiguration")?"GetConfiguration":"GetCompatibleConfigurations";
        put(&w,"<tptz:%sResponse>",op);if(!named(action,"GetCompatibleConfigurations")||ch<2)ptz_config(&w,&c,"tptz:PTZConfiguration");put(&w,"</tptz:%sResponse>",op);
    }else if(named(action,"GetConfigurationOptions")){
        char token[64];if(text_value(&d,d.action,"ConfigurationToken",token,sizeof(token))<0||strcmp(token,"ptz0"))return fault(&w,"InvalidArgVal","Unknown ConfigurationToken");
        put(&w,"<tptz:GetConfigurationOptionsResponse><tptz:PTZConfigurationOptions><tt:Spaces>");ptz_spaces(&w);put(&w,"</tt:Spaces><tt:PTZTimeout><tt:Min>%s</tt:Min><tt:Max>%s</tt:Max></tt:PTZTimeout></tptz:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>",c.onvif_nudge?"PT0.35S":"PT1.2S",c.onvif_nudge?"PT0.35S":"PT1.2S");
    }else if(named(action,"GetProfiles")){
        put(&w,"<trt:GetProfilesResponse>");for(unsigned i=0;i<c.sensors*2&&i<4;++i)profile(app,&w,&c,i,"Profiles");put(&w,"</trt:GetProfilesResponse>");
    }else if(named(action,"GetProfile")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        put(&w,"<trt:GetProfileResponse>");profile(app,&w,&c,ch,"Profile");put(&w,"</trt:GetProfileResponse>");
    }else if(named(action,"GetStreamUri")||named(action,"GetSnapshotUri")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        if(named(action,"GetStreamUri"))put(&w,"<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:%u/live/ch%u</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>",hostname,c.rtsp_port,ch);
        else put(&w,"<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>%s/api/v1/snapshot?channel=%u</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>",base,ch);
    }else if(named(action,"GetVideoSources")){
        put(&w,"<trt:GetVideoSourcesResponse>");for(unsigned i=0;i<c.sensors&&i<2;++i)put(&w,"<trt:VideoSources token=\"sensor%u\"><tt:Framerate>%u</tt:Framerate><tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution></trt:VideoSources>",i,c.fps,jipc_video_width(&c,i*2),jipc_video_height(&c,i*2));put(&w,"</trt:GetVideoSourcesResponse>");
    }else if(named(action,"GetNodes")||named(action,"GetNode")){
        if(named(action,"GetNode")){char token[64];if(text_value(&d,d.action,"NodeToken",token,sizeof(token))<0||strcmp(token,"ptz0"))return fault(&w,"InvalidArgVal","Unknown NodeToken");}
        const char *op=named(action,"GetNodes")?"GetNodes":"GetNode";put(&w,"<tptz:%sResponse><tptz:PTZNode token=\"ptz0\" FixedHomePosition=\"false\"><tt:Name>Camera motor</tt:Name><tt:SupportedPTZSpaces>",op);ptz_spaces(&w);put(&w,"</tt:SupportedPTZSpaces><tt:MaximumNumberOfPresets>16</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported></tptz:PTZNode></tptz:%sResponse>",op);
    }else if(named(action,"GetStatus")||named(action,"GetPresets")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        jipc_ptz_status status;char error[256]={0};
        if(jipc_platform_status(app,&status,error,sizeof(error))<0)return fault(&w,"Action",error[0]?error:"PTZ status unavailable");
        if(named(action,"GetPresets")){
            put(&w,"<tptz:GetPresetsResponse>");
            for(unsigned i=0;i<16;++i)if(status.preset_mask&(UINT32_C(1)<<i)){
                put(&w,"<tptz:Preset token=\"preset%u\"><tt:Name>",i+1); escaped(&w,status.preset_names[i]); put(&w,"</tt:Name></tptz:Preset>");
            }
            put(&w,"</tptz:GetPresetsResponse>");
        }else{
            time_t now=time(NULL);struct tm utc;char stamp[32];
            if(!gmtime_r(&now,&utc)||!strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&utc)||status.pan < -100||status.pan>100||status.tilt < -100||status.tilt>100)return fault(&w,"Action","PTZ position or clock unavailable");
            put(&w,"<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position><tt:PanTilt x=\"%.4f\" y=\"%.4f\" space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace\"/></tt:Position><tt:MoveStatus><tt:PanTilt>%s</tt:PanTilt></tt:MoveStatus><tt:Error>",status.pan_norm,status.tilt_norm,status.moving?"MOVING":"IDLE");
            escaped(&w,status.error);put(&w,"</tt:Error><tt:UtcTime>%s</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>",stamp);
        }
    }else if(named(action,"ContinuousMove")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        int vel=child(&d,d.action,"Velocity"),pt=child(&d,vel,"PanTilt");char x[64],y[64];double px,py;
        if(vel<0||pt<0||child(&d,vel,"Zoom")>=0||attr_value(&d,pt,"x",x,sizeof(x))<0||attr_value(&d,pt,"y",y,sizeof(y))<0||number(x,&px)<0||number(y,&py)<0)return fault(&w,"InvalidArgVal","PanTilt velocity must be finite in [-1,1]; zoom is unsupported");
        char space[192];if(attr_value(&d,pt,"space",space,sizeof(space))==0&&strcmp(space,"http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"))return fault(&w,"InvalidArgVal","Unsupported PanTilt velocity space");
        char timeout[64];int timeout_node=child(&d,d.action,"Timeout");
        if(timeout_node>=0&&(text_value(&d,d.action,"Timeout",timeout,sizeof(timeout))<0||strcmp(timeout,c.onvif_nudge?"PT0.35S":"PT1.2S")))return fault(&w,"InvalidArgVal","Unsupported motor timeout");
        if(ch>=2)return fault(&w,"ActionNotSupported","This sensor has no motor");
        /* Optional compatibility with the original local Frigate controller:
         * one steady 350 ms nudge; reply after stop so queued Stop is harmless. */
        if(c.onvif_nudge){char error[256];if(jipc_platform_nudge(app,(int)(px*100),(int)(py*100),error,sizeof(error)))return fault(&w,"Action",error);}
        else {int status=control(app,&w,"ptz",(int)(px*100),(int)(py*100));if(status)return status;}
        put(&w,"<tptz:ContinuousMoveResponse/>");
    }else if(named(action,"AbsoluteMove")||named(action,"RelativeMove")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        if(ch>=2)return fault(&w,"ActionNotSupported","This sensor has no motor");
        int rel=named(action,"RelativeMove"),node=child(&d,d.action,rel?"Translation":"Position"),pt=child(&d,node,"PanTilt"),zoom=child(&d,node,"Zoom");
        char x[64],y[64],space[192];double px,py,speed=0;
        if(node<0||pt<0||attr_value(&d,pt,"x",x,sizeof(x))<0||attr_value(&d,pt,"y",y,sizeof(y))<0||number(x,&px)<0||number(y,&py)<0)return fault(&w,"InvalidArgVal","PanTilt x and y must be finite in [-1,1]");
        if(zoom>=0){char z[64];double v;if(attr_value(&d,zoom,"x",z,sizeof(z))<0||number(z,&v)<0||v!=0)return fault(&w,"InvalidArgVal","Zoom is unsupported");}
        int kind=rel?JIPC_MOVE_RELATIVE:JIPC_MOVE_ABSOLUTE;
        if(attr_value(&d,pt,"space",space,sizeof(space))==0){
            if(rel&&!strcmp(space,PT_SPACES "TranslationSpaceFov"))kind=JIPC_MOVE_RELATIVE_FOV;
            else if(strcmp(space,rel?PT_SPACES "TranslationGenericSpace":PT_SPACES "PositionGenericSpace"))return fault(&w,"InvalidArgVal","Unsupported PanTilt space");
        }
        int sp=child(&d,d.action,"Speed"),spt=sp<0?-1:child(&d,sp,"PanTilt");
        if(spt>=0){
            double sx=0,sy=0;
            if((attr_value(&d,spt,"x",x,sizeof(x))==0&&number(x,&sx)<0)||(attr_value(&d,spt,"y",y,sizeof(y))==0&&number(y,&sy)<0))return fault(&w,"InvalidArgVal","Speed must be finite in [0,1]");
            speed=fabs(sx)>fabs(sy)?fabs(sx):fabs(sy);
        }
        char error[256]={0};
        if(jipc_platform_move(app,kind,px,py,speed,error,sizeof(error))<0)return fault(&w,"Action",error[0]?error:"The motor cannot move");
        put(&w,"<tptz:%sResponse/>",rel?"RelativeMove":"AbsoluteMove");
    }else if(named(action,"GotoHomePosition")||named(action,"SetHomePosition")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        if(ch>=2)return fault(&w,"ActionNotSupported","This sensor has no motor");
        char error[256]={0};int hx,hy;
        if(named(action,"SetHomePosition")){
            if(jipc_platform_steps(app,&hx,&hy)<0||jipc_controls_set_home(app,hx,hy,error,sizeof(error))<0)return fault(&w,"Action",error[0]?error:"The position is unavailable");
            put(&w,"<tptz:SetHomePositionResponse/>");
        }else{
            /* Without a saved home: the middle of the travel. */
            int status=jipc_controls_home(app,&hx,&hy)?jipc_platform_move(app,JIPC_MOVE_ABSOLUTE,0,0,0,error,sizeof(error)):jipc_platform_move(app,JIPC_MOVE_STEPS,hx,hy,0,error,sizeof(error));
            if(status<0)return fault(&w,"Action",error[0]?error:"The motor cannot move");
            put(&w,"<tptz:GotoHomePositionResponse/>");
        }
    }else if(named(action,"Stop")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        int status=control(app,&w,"ptz_stop",0,0);if(status)return status;put(&w,"<tptz:StopResponse/>");
    }else if(named(action,"SetPreset")||named(action,"GotoPreset")||named(action,"RemovePreset")){
        if(channel(&d,&c,&ch)<0)return fault(&w,"InvalidArgVal","Unknown ProfileToken");
        char tok[64];int slot=-1;
        if(text_value(&d,d.action,"PresetToken",tok,sizeof(tok))==0)slot=slot_token(tok);
        else if(named(action,"SetPreset"))slot=0;
        if(slot<0)return fault(&w,"InvalidArgVal","PresetToken must be preset1..preset16");
        char name[65];const char *label=NULL;
        if(child(&d,d.action,"PresetName")>=0){if(text_value(&d,d.action,"PresetName",name,sizeof(name))<0)return fault(&w,"InvalidArgVal","Invalid preset name");label=name;}
        if(child(&d,d.action,"Speed")>=0)return fault(&w,"ActionNotSupported","Preset speed selection is unsupported");
        if(ch>=2)return fault(&w,"ActionNotSupported","This sensor has no motor");
        if(named(action,"SetPreset")){char error[256];if(slot ? jipc_platform_preset_set(app,slot,label,error,sizeof(error)) : jipc_platform_preset_create(app,&slot,label,error,sizeof(error)))return fault(&w,"Action",error);}
        else {const char *op=named(action,"GotoPreset")?"preset_goto":"preset_remove";int status=control(app,&w,op,slot,0);if(status)return status;}
        if(named(action,"SetPreset"))put(&w,"<tptz:SetPresetResponse><tptz:PresetToken>preset%d</tptz:PresetToken></tptz:SetPresetResponse>",slot);
        else put(&w,"<tptz:%sResponse/>",named(action,"GotoPreset")?"GotoPreset":"RemovePreset");
    }else return fault(&w,"ActionNotSupported","Action is not implemented by this local camera service");
    put(&w,"</s:Body></s:Envelope>");
    if(w.failed){if(capacity)out[0]=0;return 500;}return 200;
}
