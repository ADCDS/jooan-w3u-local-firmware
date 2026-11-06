/* SPDX-License-Identifier: MIT
 * The timestamp's text and its picture. */
#include "jipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHEN 1793986928LL /* 2026-11-06 17:42:08 UTC */

static void text(void)
{
    char out[64];
    jipc_overlay_text(out, sizeof out, WHEN, 0, JIPC_OVERLAY_YMD, NULL);
    assert(!strcmp(out, "2026-11-06 17:42:08"));
    /* The camera's time zone moves it, across midnight too. */
    jipc_overlay_text(out, sizeof out, WHEN, -180, JIPC_OVERLAY_YMD, "");
    assert(!strcmp(out, "2026-11-06 14:42:08"));
    jipc_overlay_text(out, sizeof out, WHEN, 420, JIPC_OVERLAY_YMD, NULL);
    assert(!strcmp(out, "2026-11-07 00:42:08"));
    jipc_overlay_text(out, sizeof out, WHEN, 0, JIPC_OVERLAY_DMY, NULL);
    assert(!strcmp(out, "06/11/2026 17:42:08"));
    jipc_overlay_text(out, sizeof out, WHEN, 0, JIPC_OVERLAY_MDY, "camera1");
    assert(!strcmp(out, "11/06/2026 17:42:08 camera1"));
    /* The longest there can be fits what the picture takes. */
    size_t n = jipc_overlay_text(out, sizeof out, WHEN, 0, JIPC_OVERLAY_YMD, "0123456789abcdef");
    assert(n == JIPC_OVERLAY_CHARS && strlen(out) == n);
}

#define PX(p, w, x, y) ((p) + (size_t)((y) * (w) + (x)) * 4)
static void picture(void)
{
    unsigned char *p = malloc(JIPC_OVERLAY_BYTES);
    unsigned w = 0, h = 0;
    assert(p);
    /* '-' in font8x8 is row 3, columns 0 to 5. */
    assert(!jipc_overlay_render("-", 3, p, JIPC_OVERLAY_BYTES, &w, &h));
    assert(w == 32 && h == 28); /* 8*3 + 2*2, up to a multiple of 16 */
    unsigned edge = 2;
    /* White where the glyph is, in the middle of its row. */
    const unsigned char *in = PX(p, w, edge + 3 * 3, edge + 3 * 3 + 1);
    assert(in[0] == 255 && in[1] == 255 && in[2] == 255 && in[3] == 255);
    /* A black edge just above it, opaque. */
    const unsigned char *above = PX(p, w, edge + 3 * 3, edge + 3 * 3 - 1);
    assert(above[0] == 0 && above[1] == 0 && above[2] == 0 && above[3] == 255);
    /* Clear elsewhere. */
    assert(PX(p, w, 0, 0)[3] == 0 && PX(p, w, w - 1, h - 1)[3] == 0 && PX(p, w, edge + 7 * 3, edge + 3 * 3)[3] == 0);

    /* At the small lens's scale, one unit of edge. */
    assert(!jipc_overlay_render("1", 2, p, JIPC_OVERLAY_BYTES, &w, &h));
    assert(w == 32 && h == 18 && !(w % 16));
    /* The longest text fits the buffer at the big scale and the small lens at its own. */
    char longest[JIPC_OVERLAY_CHARS + 1];
    memset(longest, '8', JIPC_OVERLAY_CHARS);
    longest[JIPC_OVERLAY_CHARS] = 0;
    assert(!jipc_overlay_render(longest, 3, p, JIPC_OVERLAY_BYTES, &w, &h));
    assert(w <= 880 && h == 28);
    assert(!jipc_overlay_render(longest, 2, p, JIPC_OVERLAY_BYTES, &w, &h));
    assert(w + 2 * 16 <= 640);
    /* What cannot be drawn is refused, and anything outside the font is a space. */
    char too_long[JIPC_OVERLAY_CHARS + 2];
    memset(too_long, '8', JIPC_OVERLAY_CHARS + 1);
    too_long[JIPC_OVERLAY_CHARS + 1] = 0;
    assert(jipc_overlay_render(too_long, 3, p, JIPC_OVERLAY_BYTES, &w, &h) < 0);
    assert(jipc_overlay_render("-", 3, p, 16, &w, &h) < 0);
    assert(jipc_overlay_render("-", 0, p, JIPC_OVERLAY_BYTES, &w, &h) < 0);
    assert(!jipc_overlay_render("\xc3\xa9", 3, p, JIPC_OVERLAY_BYTES, &w, &h));
    for (size_t i = 0; i < (size_t)w * h; ++i) assert(p[i * 4 + 3] == 0);
    free(p);
}

int main(void)
{
    text();
    picture();
    puts("overlay: PASS");
    return 0;
}
