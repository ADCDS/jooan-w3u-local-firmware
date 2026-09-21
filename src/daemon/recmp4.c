#define _POSIX_C_SOURCE 200809L
#include "joan_daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Play a recorded clip in the browser.
 *
 * The OEM recorder writes AVI: H.264 in Annex-B, 15 fps, one frame per 00dc
 * chunk, with SPS/PPS in band at the head of the first chunk. No browser plays
 * AVI, so the clip is rewritten as a progressive MP4 on the way out. Nothing is
 * re-encoded -- the same H.264 samples are moved into an MP4 container, which
 * costs a scan and a start-code rewrite, not a transcode.
 *
 * The recorder never finalizes a file: the RIFF and movi sizes stay 0 and there
 * is no idx1, so the chunk list has to be walked rather than read from an index.
 * That walk is pass one, which yields the sample table; pass two streams the
 * samples. Audio is dropped: the recorder stores G.711 A-law, which browsers do
 * not decode inside MP4.
 *
 * Deliberately progressive rather than fragmented. moov up front means the
 * player is a plain <video controls> element, so transport, scrubbing and speed
 * come from the browser instead of from JavaScript that would have to be paid
 * for out of the persistent budget. */

#define REC_TIMESCALE 90000u
#define REC_MAX_SAMPLES 200000u   /* ~3.7 h at 15 fps; bounds the table on a 38 MB box */
#define REC_CHUNK_LIMIT (8u * 1024u * 1024u)

typedef struct { uint32_t offset, in_len, out_len; int key; } Sample;

struct JoanRecMp4 {
    FILE *file;
    Sample *samples;
    uint32_t count;
    uint64_t mdat;
    unsigned char *head;      /* ftyp + moov */
    size_t head_len;
    unsigned width, height, frame_ticks;
    unsigned char sps[256], pps[256];
    size_t sps_len, pps_len;
};

typedef struct { unsigned char *p; size_t n, cap; int bad; } Buf;

static int bput(Buf *b, const void *data, size_t len)
{
    if (b->bad) return -1;
    if (b->n + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        unsigned char *grown;
        while (cap < b->n + len) {
            if (cap > 64u * 1024u * 1024u) { b->bad = 1; return -1; }
            cap *= 2;
        }
        grown = realloc(b->p, cap);
        if (!grown) { b->bad = 1; return -1; }
        b->p = grown; b->cap = cap;
    }
    memcpy(b->p + b->n, data, len); b->n += len; return 0;
}
static void b8(Buf *b, unsigned v) { unsigned char x = (unsigned char)v; bput(b, &x, 1); }
static void b16(Buf *b, unsigned v) { unsigned char x[2] = { (unsigned char)(v >> 8), (unsigned char)v }; bput(b, x, 2); }
static void b32(Buf *b, uint32_t v)
{
    unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    bput(b, x, 4);
}
static size_t bbox(Buf *b, const char *tag) { size_t at = b->n; b32(b, 0); bput(b, tag, 4); return at; }
static void bend(Buf *b, size_t at)
{
    uint32_t len = (uint32_t)(b->n - at);
    if (b->bad || at + 4 > b->n) return;
    b->p[at] = (unsigned char)(len >> 24); b->p[at + 1] = (unsigned char)(len >> 16);
    b->p[at + 2] = (unsigned char)(len >> 8); b->p[at + 3] = (unsigned char)len;
}
static void bpad(Buf *b, size_t n) { while (n--) b8(b, 0); }

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Annex-B start codes are 3 or 4 bytes; return the payload start of the next
 * NAL after `from`, or 0 when there is no further start code. */
static size_t next_start(const unsigned char *p, size_t len, size_t from, size_t *code_len)
{
    size_t i;
    for (i = from; i + 3 <= len; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        if (i && !p[i - 1]) { *code_len = 4; return i + 3; }
        *code_len = 3; return i + 3;
    }
    return 0;
}

/* Walk one AVI chunk's NALs. Records SPS/PPS the first time they are seen,
 * reports whether the chunk carries an IDR, and returns its AVCC length. */
static uint32_t scan_nals(JoanRecMp4 *m, const unsigned char *p, size_t len, int *key)
{
    size_t at = 0, code = 0;
    uint32_t out = 0;
    at = next_start(p, len, 0, &code);
    while (at) {
        size_t next = next_start(p, len, at, &code);
        size_t end = next ? next - code : len;
        size_t n = end > at ? end - at : 0;
        if (n) {
            unsigned type = p[at] & 0x1f;
            if (type == 7 && !m->sps_len && n <= sizeof(m->sps)) { memcpy(m->sps, p + at, n); m->sps_len = n; }
            else if (type == 8 && !m->pps_len && n <= sizeof(m->pps)) { memcpy(m->pps, p + at, n); m->pps_len = n; }
            if (type == 5) *key = 1;
            /* Parameter sets live in avcC, not in the sample data. */
            if (type != 7 && type != 8) out += (uint32_t)(4 + n);
        }
        at = next;
    }
    return out;
}

static void build_moov(JoanRecMp4 *m, Buf *b, size_t *stco_at)
{
    uint32_t i, duration = m->count * m->frame_ticks;
    size_t at, trak, mdia, minf, stbl, stsd, avc1;

    at = bbox(b, "ftyp");
    bput(b, "isom", 4); b32(b, 0x200); bput(b, "isomiso2avc1mp41", 16);
    bend(b, at);

    at = bbox(b, "moov");
    { size_t x = bbox(b, "mvhd");
      b32(b, 0); b32(b, 0); b32(b, 0); b32(b, REC_TIMESCALE); b32(b, duration);
      b32(b, 0x00010000); b16(b, 0x0100); b16(b, 0); b32(b, 0); b32(b, 0);
      b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x00010000); b32(b, 0);
      b32(b, 0); b32(b, 0); b32(b, 0x40000000);
      bpad(b, 24); b32(b, 2); bend(b, x); }

    trak = bbox(b, "trak");
    { size_t x = bbox(b, "tkhd");
      b32(b, 0x00000007); b32(b, 0); b32(b, 0); b32(b, 1); b32(b, 0); b32(b, duration);
      b32(b, 0); b32(b, 0); b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0);
      b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x00010000); b32(b, 0);
      b32(b, 0); b32(b, 0); b32(b, 0x40000000);
      b32(b, m->width << 16); b32(b, m->height << 16); bend(b, x); }

    mdia = bbox(b, "mdia");
    { size_t x = bbox(b, "mdhd");
      b32(b, 0); b32(b, 0); b32(b, 0); b32(b, REC_TIMESCALE); b32(b, duration);
      b16(b, 0x55c4); b16(b, 0); bend(b, x); }
    { size_t x = bbox(b, "hdlr");
      b32(b, 0); b32(b, 0); bput(b, "vide", 4); b32(b, 0); b32(b, 0); b32(b, 0);
      bput(b, "VideoHandler", 13); bend(b, x); }

    minf = bbox(b, "minf");
    { size_t x = bbox(b, "vmhd"); b32(b, 1); b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0); bend(b, x); }
    { size_t x = bbox(b, "dinf"); size_t y = bbox(b, "dref"); b32(b, 0); b32(b, 1);
      { size_t z = bbox(b, "url "); b32(b, 1); bend(b, z); } bend(b, y); bend(b, x); }

    stbl = bbox(b, "stbl");
    stsd = bbox(b, "stsd"); b32(b, 0); b32(b, 1);
    avc1 = bbox(b, "avc1");
    bpad(b, 6); b16(b, 1); b16(b, 0); b16(b, 0); bpad(b, 12);
    b16(b, m->width); b16(b, m->height);
    b32(b, 0x00480000); b32(b, 0x00480000); b32(b, 0); b16(b, 1);
    bpad(b, 32); b16(b, 0x0018); b16(b, 0xffff);
    { size_t x = bbox(b, "avcC");
      b8(b, 1); b8(b, m->sps_len > 1 ? m->sps[1] : 0x64); b8(b, m->sps_len > 2 ? m->sps[2] : 0);
      b8(b, m->sps_len > 3 ? m->sps[3] : 0x16); b8(b, 0xff); b8(b, 0xe1);
      b16(b, (unsigned)m->sps_len); bput(b, m->sps, m->sps_len);
      b8(b, 1); b16(b, (unsigned)m->pps_len); bput(b, m->pps, m->pps_len);
      bend(b, x); }
    bend(b, avc1); bend(b, stsd);

    { size_t x = bbox(b, "stts"); b32(b, 0); b32(b, 1); b32(b, m->count); b32(b, m->frame_ticks); bend(b, x); }
    { uint32_t keys = 0; size_t x;
      for (i = 0; i < m->count; i++) if (m->samples[i].key) keys++;
      x = bbox(b, "stss"); b32(b, 0); b32(b, keys);
      for (i = 0; i < m->count; i++) if (m->samples[i].key) b32(b, i + 1);
      bend(b, x); }
    { size_t x = bbox(b, "stsc"); b32(b, 0); b32(b, 1); b32(b, 1); b32(b, 1); b32(b, 1); bend(b, x); }
    { size_t x = bbox(b, "stsz"); b32(b, 0); b32(b, 0); b32(b, m->count);
      for (i = 0; i < m->count; i++) b32(b, m->samples[i].out_len);
      bend(b, x); }
    { size_t x = bbox(b, "stco"); b32(b, 0); b32(b, m->count);
      *stco_at = b->n;                       /* patched once moov's size is known */
      for (i = 0; i < m->count; i++) b32(b, 0);
      bend(b, x); }

    bend(b, stbl); bend(b, minf); bend(b, mdia); bend(b, trak); bend(b, at);
}

JoanRecMp4 *joan_recmp4_open(const char *path)
{
    JoanRecMp4 *m; unsigned char header[12], chunk[8]; unsigned char *payload = NULL;
    Buf b; size_t stco_at = 0, cap = 0; uint32_t i; long movi;

    m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->file = fopen(path, "rb");
    if (!m->file) { free(m); return NULL; }
    m->width = 640; m->height = 360; m->frame_ticks = REC_TIMESCALE / 15u;

    if (fread(header, 1, 12, m->file) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "AVI ", 4))
        goto fail;

    /* Walk the top level for hdrl (frame rate, size) and movi (the samples).
     * Sizes in an unfinalized file are unreliable, so movi is found by name and
     * then walked to end of file rather than trusted to declare its length. */
    movi = -1;
    for (;;) {
        long at = ftell(m->file);
        if (fread(chunk, 1, 8, m->file) != 8) break;
        { uint32_t len = le32(chunk + 4);
          if (!memcmp(chunk, "LIST", 4)) {
              unsigned char type[4];
              if (fread(type, 1, 4, m->file) != 4) break;
              if (!memcmp(type, "movi", 4)) { movi = at + 12; break; }
              if (!memcmp(type, "hdrl", 4)) {
                  long end = len > 4 ? at + 8 + (long)len : 0;
                  for (;;) {
                      unsigned char sub[8]; long here = ftell(m->file);
                      if (end && here >= end) break;
                      if (fread(sub, 1, 8, m->file) != 8) break;
                      { uint32_t slen = le32(sub + 4);
                        if (!memcmp(sub, "avih", 4) && slen >= 56) {
                            unsigned char a[56];
                            if (fread(a, 1, 56, m->file) != 56) break;
                            { uint32_t us = le32(a), w = le32(a + 32), h = le32(a + 36);
                              if (us) m->frame_ticks = (unsigned)((double)REC_TIMESCALE * us / 1000000.0 + 0.5);
                              if (w && w < 8192) m->width = w;
                              if (h && h < 8192) m->height = h; }
                            if (slen > 56) fseek(m->file, (long)slen - 56, SEEK_CUR);
                        } else if (!memcmp(sub, "LIST", 4)) {
                            fseek(m->file, 4, SEEK_CUR);
                            continue;
                        } else {
                            fseek(m->file, (long)slen + (slen & 1), SEEK_CUR);
                        } }
                  }
                  continue;
              }
              if (len > 4) fseek(m->file, at + 8 + (long)len + ((long)len & 1), SEEK_SET);
              continue;
          }
          fseek(m->file, at + 8 + (long)len + ((long)len & 1), SEEK_SET); }
    }
    if (movi < 0 || !m->frame_ticks) goto fail;

    /* Pass one: enumerate video chunks and measure their AVCC length. */
    if (fseek(m->file, movi, SEEK_SET)) goto fail;
    for (;;) {
        long at = ftell(m->file);
        uint32_t len; int key = 0; uint32_t out;
        if (fread(chunk, 1, 8, m->file) != 8) break;
        len = le32(chunk + 4);
        if (len > REC_CHUNK_LIMIT) break;
        if (memcmp(chunk + 2, "dc", 2) && memcmp(chunk + 2, "db", 2)) {
            if (fseek(m->file, at + 8 + (long)len + ((long)len & 1), SEEK_SET)) break;
            continue;
        }
        if (len > cap) {
            unsigned char *grown = realloc(payload, len);
            if (!grown) goto fail;
            payload = grown; cap = len;
        }
        if (len && fread(payload, 1, len, m->file) != len) break;
        if (len & 1) fseek(m->file, 1, SEEK_CUR);
        out = scan_nals(m, payload, len, &key);
        if (!out) continue;
        if (m->count >= REC_MAX_SAMPLES) break;
        if (!(m->count & 1023)) {
            Sample *grown = realloc(m->samples, (m->count + 1024) * sizeof(*grown));
            if (!grown) goto fail;
            m->samples = grown;
        }
        m->samples[m->count].offset = (uint32_t)(at + 8);
        m->samples[m->count].in_len = len;
        m->samples[m->count].out_len = out;
        m->samples[m->count].key = key;
        m->mdat += out;
        m->count++;
    }
    free(payload); payload = NULL;
    if (!m->count || !m->sps_len || !m->pps_len) goto fail;

    memset(&b, 0, sizeof(b));
    build_moov(m, &b, &stco_at);
    if (b.bad) { free(b.p); goto fail; }
    /* mdat payload begins after ftyp+moov and the mdat box header. */
    { uint64_t at = b.n + 8, run = at;
      if (at + m->mdat > 0xffffffffu) { free(b.p); goto fail; }
      for (i = 0; i < m->count; i++) {
          unsigned char *slot = b.p + stco_at + i * 4;
          slot[0] = (unsigned char)(run >> 24); slot[1] = (unsigned char)(run >> 16);
          slot[2] = (unsigned char)(run >> 8);  slot[3] = (unsigned char)run;
          run += m->samples[i].out_len;
      } }
    m->head = b.p; m->head_len = b.n;
    return m;
fail:
    free(payload);
    joan_recmp4_close(m);
    return NULL;
}

long joan_recmp4_length(const JoanRecMp4 *m)
{
    if (!m) return -1;
    return (long)(m->head_len + 8 + m->mdat);
}

/* Emit the byte window [start,end] of the generated MP4. Seeking in a browser
 * is a Range request, and the file does not exist on disk to seek within, so
 * the window is produced on the fly. Whole samples before the window are
 * skipped using the table rather than read, so a seek costs one sample read
 * plus arithmetic, not a pass over the clip. */
static int emit(JoanRecSink sink, void *ctx, const unsigned char *data, size_t len,
                uint64_t at, uint64_t start, uint64_t end)
{
    uint64_t first, last;
    if (at + len <= start || at > end) return 0;
    first = at < start ? start - at : 0;
    last = at + len - 1 > end ? end - at : len - 1;
    return sink(ctx, data + first, (size_t)(last - first + 1));
}

int joan_recmp4_write(JoanRecMp4 *m, JoanRecSink sink, void *ctx, long from, long to)
{
    unsigned char box[8], *payload = NULL, *out = NULL;
    size_t cap = 0, ocap = 0; uint32_t i;
    uint64_t at = 0, start, end, total;
    if (!m || !sink) return -1;
    total = (uint64_t)joan_recmp4_length(m);
    start = from < 0 ? 0 : (uint64_t)from;
    end = to < 0 || (uint64_t)to >= total ? total - 1 : (uint64_t)to;
    if (start > end) return -1;

    if (emit(sink, ctx, m->head, m->head_len, at, start, end)) return -1;
    at += m->head_len;
    { uint32_t len = (uint32_t)(8 + m->mdat);
      box[0] = (unsigned char)(len >> 24); box[1] = (unsigned char)(len >> 16);
      box[2] = (unsigned char)(len >> 8);  box[3] = (unsigned char)len;
      memcpy(box + 4, "mdat", 4);
      if (emit(sink, ctx, box, 8, at, start, end)) return -1; }
    at += 8;

    for (i = 0; i < m->count && at <= end; i++) {
        Sample *s = &m->samples[i];
        size_t used = 0, p = 0, code = 0;
        if (at + s->out_len <= start) { at += s->out_len; continue; }  /* skipped unread */
        if (s->in_len > cap) {
            unsigned char *grown = realloc(payload, s->in_len);
            if (!grown) { free(payload); free(out); return -1; }
            payload = grown; cap = s->in_len;
        }
        if (s->out_len > ocap) {
            unsigned char *grown = realloc(out, s->out_len);
            if (!grown) { free(payload); free(out); return -1; }
            out = grown; ocap = s->out_len;
        }
        if (fseek(m->file, (long)s->offset, SEEK_SET) ||
            fread(payload, 1, s->in_len, m->file) != s->in_len) { free(payload); free(out); return -1; }
        p = next_start(payload, s->in_len, 0, &code);
        while (p) {
            size_t next = next_start(payload, s->in_len, p, &code);
            size_t stop = next ? next - code : s->in_len;
            size_t n = stop > p ? stop - p : 0;
            if (n) {
                unsigned type = payload[p] & 0x1f;
                if (type != 7 && type != 8 && used + 4 + n <= s->out_len) {
                    out[used] = (unsigned char)(n >> 24); out[used + 1] = (unsigned char)(n >> 16);
                    out[used + 2] = (unsigned char)(n >> 8); out[used + 3] = (unsigned char)n;
                    memcpy(out + used + 4, payload + p, n); used += 4 + n;
                }
            }
            p = next;
        }
        if (emit(sink, ctx, out, used, at, start, end)) { free(payload); free(out); return -1; }
        at += s->out_len;
    }
    free(payload); free(out);
    return 0;
}

void joan_recmp4_close(JoanRecMp4 *m)
{
    if (!m) return;
    if (m->file) fclose(m->file);
    free(m->samples); free(m->head); free(m);
}
