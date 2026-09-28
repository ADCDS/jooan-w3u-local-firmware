#define _POSIX_C_SOURCE 200809L
/* ONVIF for network video recorders such as Frigate: the few Profile S
 * operations an NVR needs to find the pan/tilt sensor, move it and use the
 * OEM presets. SOAP arrives on the HTTPS listener under /onvif/. Discovery
 * extras (GetServices, GetStreamUri, ...) are left out: the flash budget is
 * tight and Frigate does not call them.
 *
 * jooanipc has its own ONVIF server, and it stays confined to loopback: it
 * dispatches on literal namespace prefixes (zeep-based clients fail on the
 * first call), accepts any password, and its preset operations are stubs.
 * This one matches elements by local name, requires a WS-Security
 * UsernameToken digest of the administrator password on every call except
 * GetSystemDateAndTime, and moves the head only through the PTZ order that
 * http.c keeps for the Web API. */
#include "joan_daemon.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef JOAN_NO_TLS
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#endif

#define VELOCITY_SPACE "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"
/* The OEM keeps at most six presets, in slots 0-5. */
#define PRESET_SLOTS 6u

typedef struct { char *b; size_t n, cap; int bad; } Out;
/* One element found by local name, whatever prefix the client chose: tag is
 * its '<', gt the '>' closing the start tag, body NULL when self-closing. */
typedef struct { const char *tag, *gt, *body; } Elem;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap; int z;
    if (o->bad) return;
    va_start(ap, fmt); z = vsnprintf(o->b + o->n, o->cap - o->n, fmt, ap); va_end(ap);
    if (z < 0 || (size_t)z >= o->cap - o->n) o->bad = 1; else o->n += (size_t)z;
}

/* SOAP 1.2 over HTTP: a Sender fault is the client's error (400), a
 * Receiver fault the camera's (500). */
static int fault(Out *o, const char *code, const char *subcode, const char *reason)
{
    put(o, "<s:Fault><s:Code><s:Value>s:%s</s:Value><s:Subcode><s:Value>ter:%s</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>", code, subcode, reason);
    return code[0] == 'S' ? 400 : 500;
}

static int name_end(char c) { return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || !c; }

static int find(const char *p, const char *end, const char *name, Elem *e)
{
    size_t n = strlen(name);
    while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
        const char *local = ++p, *t = p; char quote = 0;
        if (p >= end || *p == '/' || *p == '?' || *p == '!') continue;
        for (; t < end && !name_end(*t); t++) if (*t == ':') local = t + 1;
        if ((size_t)(t - local) != n || memcmp(local, name, n)) continue;
        for (; t < end && (quote || *t != '>'); t++)
            if (*t == '"' || *t == '\'') quote = quote == *t ? 0 : quote ? quote : *t;
        if (t >= end) return 0;
        e->tag = p - 1; e->gt = t; e->body = t[-1] == '/' ? NULL : t + 1;
        return 1;
    }
    return 0;
}

/* The trimmed, entity-decoded text of the first element called name. */
static int text(const char *p, const char *end, const char *name, char *out, size_t cap)
{
    static const char *const entities[] = { "amp;&", "lt;<", "gt;>", "quot;\"", "apos;'" };
    Elem e; size_t o = 0, lead;
    if (!find(p, end, name, &e)) return -1;
    for (p = e.body; p && p < end && *p != '<'; p++) {
        char c = *p;
        if (c == '&') {
            unsigned i; size_t k = 0;
            for (i = 0; i < 5; i++) {
                k = strlen(entities[i]) - 1;
                if ((size_t)(end - p - 1) >= k && !memcmp(p + 1, entities[i], k)) break;
            }
            if (i == 5) return -1;
            c = entities[i][k]; p += k;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    while (o && strchr(" \t\r\n", out[o - 1])) o--;
    out[o] = 0;
    lead = strspn(out, " \t\r\n");
    memmove(out, out + lead, o - lead + 1);
    return 0;
}

/* A numeric attribute of e's start tag; absent or malformed reads as 0. */
static double number_attr(const Elem *e, const char *name)
{
    size_t n = strlen(name); const char *p;
    for (p = e->tag; p + n + 2 < e->gt; p++)
        if (strchr(" \t\r\n", *p) && !memcmp(p + 1, name, n) && p[n + 1] == '=' && (p[n + 2] == '"' || p[n + 2] == '\''))
            return strtod(p + n + 3, NULL);
    return 0;
}

/* The local name of the Body's first child: the operation requested. */
static int operation(const Elem *body, const char *end, char *out, size_t cap)
{
    const char *p = body->body, *local, *t;
    while (p < end && strchr(" \t\r\n", *p) && *p) p++;
    if (p >= end || *p != '<') return -1;
    for (local = t = p + 1; t < end && !name_end(*t); t++) if (*t == ':') local = t + 1;
    if (t == local || (size_t)(t - local) >= cap) return -1;
    memcpy(out, local, (size_t)(t - local)); out[t - local] = 0;
    return 0;
}

static void xml_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in; in++) {
        const char *r = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        size_t k = r ? strlen(r) : 1;
        if (o + k >= cap) break;
        if (r) memcpy(out + o, r, k); else out[o] = *in;
        o += k;
    }
    out[o] = 0;
}

/* Decode the JSON string starting after its opening quote. */
static const char *json_string(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    for (; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            c = *++p;
            if (!c) return NULL;
            if (c == 'u') { unsigned v = 0; int i; for (i = 0; i < 4 && p[1]; i++) { char h = *++p; v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10); } c = v < 0x80 ? (char)v : '?'; }
            else if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') c = ' ';
        }
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static int preset_token(const char *s, unsigned *token)
{
    if (!s[0] || s[1] || s[0] < '0' || s[0] >= (char)('0' + PRESET_SLOTS)) return -1;
    *token = (unsigned)(s[0] - '0');
    return 0;
}

/* Names go into the OEM's JSON unescaped, so refuse what would need it. */
static int preset_name_ok(const char *s)
{
    size_t n = strlen(s), i;
    if (!n || n > 64) return 0;
    for (i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f || s[i] == '"' || s[i] == '\\') return 0;
    return 1;
}

#ifndef JOAN_NO_TLS
static unsigned char seen[32][20];
static unsigned seen_next;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

/* A captured token must not work twice. Created is inside the digest but is
 * not bounded in time: the camera clock is only as good as its last sync. */
static int first_use(const unsigned char digest[20])
{
    unsigned i; int fresh = 1;
    pthread_mutex_lock(&seen_lock);
    for (i = 0; i < 32; i++) if (!memcmp(seen[i], digest, 20)) fresh = 0;
    if (fresh) memcpy(seen[seen_next++ % 32], digest, 20);
    pthread_mutex_unlock(&seen_lock);
    return fresh;
}

/* WS-Security UsernameToken, PasswordDigest profile, for user admin:
 * Base64(SHA-1(nonce + Created + password)). The password is the RTSP copy
 * the Digest proxy already keeps, so ONVIF follows every password change. */
static int authenticated(const JoanConfig *cfg, const char *p, const char *end)
{
    char user[16], digest[64], nonce_text[128], created[64], path[512];
    unsigned char nonce[96], got[32], want[20], input[96 + 64 + 320], *raw = NULL;
    size_t nonce_len, got_len, created_len, raw_len = 0, n;
    int ok = 0;
    if (text(p, end, "Username", user, sizeof(user)) || strcmp(user, "admin") ||
        text(p, end, "Password", digest, sizeof(digest)) ||
        text(p, end, "Nonce", nonce_text, sizeof(nonce_text)) ||
        text(p, end, "Created", created, sizeof(created)) ||
        mbedtls_base64_decode(got, sizeof(got), &got_len, (const unsigned char *)digest, strlen(digest)) || got_len != 20 ||
        mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len, (const unsigned char *)nonce_text, strlen(nonce_text)) || !nonce_len)
        return 0;
    if (snprintf(path, sizeof(path), "%s/rtsp.password", cfg->state_dir) >= (int)sizeof(path) ||
        joan_read_file(path, &raw, &raw_len, 319))
        return 0;
    for (n = raw_len; n && (raw[n - 1] == '\n' || raw[n - 1] == '\r'); n--) {}
    created_len = strlen(created);
    if (n) {
        memcpy(input, nonce, nonce_len);
        memcpy(input + nonce_len, created, created_len);
        memcpy(input + nonce_len + created_len, raw, n);
        if (!mbedtls_sha1_ret(input, nonce_len + created_len + n, want))
            ok = joan_ct_equal(want, got, 20) && first_use(want);
    }
    memset(raw, 0, raw_len); free(raw);
    memset(input, 0, sizeof(input)); memset(want, 0, sizeof(want));
    return ok;
}
#else
static int authenticated(const JoanConfig *cfg, const char *p, const char *end)
{
    (void)cfg; (void)p; (void)end;
    return 0;
}
#endif

static int status_zero(const char *json)
{
    const char *p = strstr(json, "\"status\":");
    return p && p[9] == '0' && (p[10] < '0' || p[10] > '9');
}

/* Send one OEM preset command and wait for its reply, which the Web API
 * polls for instead. 0 on success; otherwise the fault's HTTP status. */
static int oem(Out *o, unsigned command, const char *payload, char *reply, size_t cap)
{
    char operation_id[65]; unsigned i; int rc = joan_ptz_preset_request(command, payload, operation_id);
    if (rc) return fault(o, "Receiver", "Action", rc > 0 || rc == -2 ? "another camera move is pending" : "the camera command channel is unavailable");
    for (i = 0; i < 100; i++) {
        struct timespec pause = { 0, 50000000L };
        if ((rc = joan_mqtt_operation(operation_id, reply, cap)) <= 0) break;
        nanosleep(&pause, NULL);
    }
    if (rc) return fault(o, "Receiver", "Action", "the camera did not answer the command");
    if (!status_zero(reply)) {
        /* e.g. a save answers -2 when a preset already sits at this position */
        char why[64]; const char *code = strstr(reply, "\"status\":");
        snprintf(why, sizeof(why), "the camera refused the command (status %d)", code ? atoi(code + 9) : -1);
        return fault(o, "Receiver", "Action", why);
    }
    return 0;
}

static int presets(Out *o)
{
    char reply[2049], name[128], xml[400]; const char *p = reply; int st;
    if ((st = oem(o, 66486, "{\"cmd\":66486,\"cmd_type\":\"request\"}", reply, sizeof(reply)))) return st;
    put(o, "<tptz:GetPresetsResponse>");
    while ((p = strstr(p, "\"coordinateID\":")) != NULL) {
        char *after; unsigned long id = strtoul(p + 15, &after, 10);
        if (!(p = strstr(after, "\"name\":\"")) || !(p = json_string(p + 8, name, sizeof(name)))) break;
        xml_escape(name, xml, sizeof(xml));
        put(o, "<tptz:Preset token=\"%lu\"><tt:Name>%s</tt:Name></tptz:Preset>", id, xml);
    }
    put(o, "</tptz:GetPresetsResponse>");
    return 200;
}

static int set_preset(Out *o, const char *p, const char *end)
{
    char name[80], token_text[8], payload[256], reply[2049]; unsigned token = 0; int has_token, st;
    if (text(p, end, "PresetName", name, sizeof(name)) || !preset_name_ok(name))
        return fault(o, "Sender", "InvalidArgVal", "PresetName is required: 1-64 characters, no quotes or backslashes");
    has_token = !text(p, end, "PresetToken", token_text, sizeof(token_text)) && token_text[0];
    if (has_token && preset_token(token_text, &token)) return fault(o, "Sender", "InvalidArgVal", "unknown PresetToken");
    if (has_token) snprintf(payload, sizeof(payload), "{\"cmd\":66489,\"cmd_type\":\"request\",\"coordinateID\":%u,\"name\":\"%s\",\"mot_index\":0}", token, name);
    else snprintf(payload, sizeof(payload), "{\"cmd\":66485,\"cmd_type\":\"request\",\"name\":\"%s\",\"mot_index\":0}", name);
    if ((st = oem(o, has_token ? 66489 : 66485, payload, reply, sizeof(reply)))) return st;
    if (!has_token) {
        const char *id = strstr(reply, "\"coordinateID\":");
        if (!id) return fault(o, "Receiver", "Action", "the camera did not report the new preset");
        token = (unsigned)strtoul(id + 15, NULL, 10);
    }
    put(o, "<tptz:SetPresetResponse><tptz:PresetToken>%u</tptz:PresetToken></tptz:SetPresetResponse>", token);
    return 200;
}

/* A press is one coarse nudge along the dominant axis (the head moves one
 * axis at a time and at one speed); see joan_ptz_nudge. */
static int continuous_move(Out *o, const char *p, const char *end)
{
    Elem e; double x = 0, y = 0, ax, ay; int rc = 0;
    if (find(p, end, "PanTilt", &e)) { x = number_attr(&e, "x"); y = number_attr(&e, "y"); }
    ax = x < 0 ? -x : x; ay = y < 0 ? -y : y;
    if (ax >= 0.05 || ay >= 0.05)
        rc = joan_ptz_nudge(ax >= ay ? (x > 0 ? "right" : "left") : (y > 0 ? "up" : "down"));
    if (rc) return fault(o, "Receiver", "Action", rc > 0 ? "another camera move is pending" : "the camera did not confirm the move");
    put(o, "<tptz:ContinuousMoveResponse/>");
    return 200;
}

static int dispatch(Out *o, const char *op, const char *p, const char *end, const char *base)
{
    char value[8], payload[160], reply[2049]; unsigned token; int st;
    if (!strcmp(op, "GetCapabilities"))
        put(o, "<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Device><tt:XAddr>%s/onvif/device_service</tt:XAddr></tt:Device>"
            "<tt:Media><tt:XAddr>%s/onvif/media</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
            "<tt:RTP_TCP>false</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>"
            "<tt:PTZ><tt:XAddr>%s/onvif/ptz</tt:XAddr></tt:PTZ></tds:Capabilities></tds:GetCapabilitiesResponse>", base, base, base);
    else if (!strcmp(op, "GetProfiles"))
        /* Only sensor A is on the pan/tilt head; sensor B (fixed) has no use
         * here, and its RTSP URL is the same as ever. Velocity space only:
         * no zoom and no relative moves, so an NVR shows arrows and presets. */
        put(o, "<trt:GetProfilesResponse><trt:Profiles token=\"ch0\" fixed=\"true\"><tt:Name>ch0</tt:Name>"
            "<tt:VideoEncoderConfiguration token=\"ve0\"><tt:Name>ve0</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>"
            "<tt:Resolution><tt:Width>2304</tt:Width><tt:Height>1296</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality></tt:VideoEncoderConfiguration>"
            "<tt:PTZConfiguration token=\"ptz0\"><tt:Name>ptz0</tt:Name><tt:UseCount>1</tt:UseCount><tt:NodeToken>ptz0</tt:NodeToken>"
            "<tt:DefaultContinuousPanTiltVelocitySpace>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:DefaultContinuousPanTiltVelocitySpace>"
            "<tt:DefaultPTZTimeout>PT1S</tt:DefaultPTZTimeout></tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>");
    else if (!strcmp(op, "GetPresets")) return presets(o);
    else if (!strcmp(op, "SetPreset")) return set_preset(o, p, end);
    else if (!strcmp(op, "GotoPreset") || !strcmp(op, "RemovePreset")) {
        int go = op[0] == 'G';
        if (text(p, end, "PresetToken", value, sizeof(value)) || preset_token(value, &token))
            return fault(o, "Sender", "InvalidArgVal", "unknown PresetToken");
        snprintf(payload, sizeof(payload), go ? "{\"cmd\":66491,\"cmd_type\":\"request\",\"coordinateID\":%u,\"mot_index\":0}"
                                              : "{\"cmd\":66490,\"cmd_type\":\"request\",\"ptz_coordinate\":[{\"coordinateID\":%u}],\"mot_index\":0}", token);
        if ((st = oem(o, go ? 66491 : 66490, payload, reply, sizeof(reply)))) return st;
        put(o, "<tptz:%sResponse/>", op);
    } else if (!strcmp(op, "ContinuousMove")) return continuous_move(o, p, end);
    else if (!strcmp(op, "Stop")) put(o, "<tptz:StopResponse/>"); /* each nudge stops itself */
    else return fault(o, "Receiver", "ActionNotSupported", "action not supported");
    return 200;
}

int joan_onvif_handle(const JoanConfig *cfg, const JoanRequest *req, const char *host,
                      char **reply, size_t *reply_len)
{
    const char *doc = (const char *)req->body, *end = doc ? doc + req->content_length : NULL;
    char op[40], base[300]; Elem body; Out o; int status = 200;
    *reply = NULL; *reply_len = 0;
    memset(&o, 0, sizeof(o)); o.cap = 4096; /* the largest reply, GetProfiles, is under 3 KiB */
    if (!(o.b = malloc(o.cap))) return -1;
    snprintf(base, sizeof(base), "%s://%s", cfg->plain_http ? "http" : "https", host);
    put(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body>");
    if (!doc || !find(doc, end, "Body", &body) || !body.body || operation(&body, end, op, sizeof(op)))
        status = fault(&o, "Sender", "InvalidArgVal", "a SOAP request is required");
    else if (!strcmp(op, "GetSystemDateAndTime")) {
        /* Unauthenticated by design: clients read it to correct their
         * token timestamps for a camera clock that has drifted. */
        time_t now = time(NULL); struct tm t;
        gmtime_r(&now, &t);
        put(&o, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
            "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
            t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    } else if (joan_auth_throttled(req->remote) || !authenticated(cfg, doc, body.tag)) {
        joan_auth_note(req->remote, 0);
        status = fault(&o, "Sender", "NotAuthorized", "sender not authorized");
    } else {
        joan_auth_note(req->remote, 1);
        status = dispatch(&o, op, body.body, end, base);
    }
    put(&o, "</s:Body></s:Envelope>");
    if (o.bad) { free(o.b); return -1; }
    *reply = o.b; *reply_len = o.n;
    return status;
}
