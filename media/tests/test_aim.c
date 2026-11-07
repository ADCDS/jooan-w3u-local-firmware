/* SPDX-License-Identifier: MIT
 * The aim calibration: its map and inverse, what it accepts, that it is kept, and
 * that aiming without one is refused. Moving the motors is in tests/integration.py. */
#include "jipc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* camera1's own, measured on 2026-11-06. */
/* The fixed lens's orientation is checked where the file is read: see integration.py. */
static const jipc_aim CAMERA1 = {{2021, -457, 2, -54, 5, 2}, {385, -5, 220, 8, -69, -10}};
#define NEAR(a, b, tolerance) (fabs((a) - (b)) <= (tolerance))

static void maps(void)
{
    double x, y, u, v;
    /* The picture's centre is the constant term, and +u turns the count down on this one. */
    jipc_aim_steps(&CAMERA1, 0.5, 0.5, &x, &y);
    assert(NEAR(x, 2021, 1e-9) && NEAR(y, 385, 1e-9));
    jipc_aim_steps(&CAMERA1, 1, 0.5, &x, &y);
    assert(x < 2021 - 400);
    /* Everywhere across the picture and a good way past it, the inverse finds the point. */
    for (double a = -0.5; a <= 1.5; a += 0.125)
        for (double b = -0.5; b <= 1.5; b += 0.125) {
            jipc_aim_steps(&CAMERA1, a, b, &x, &y);
            assert(!jipc_aim_where(&CAMERA1, x, y, &u, &v));
            assert(NEAR(u, a, 1e-3) && NEAR(v, b, 1e-3));
        }
    /* A position far outside the picture is not a point of it. */
    assert(jipc_aim_where(&CAMERA1, 2021 + 50000, 385, &u, &v) < 0);
}

static void api(void)
{
    char dir[] = "/tmp/jipc-aim-XXXXXX", out[512], error[256], path[64];
    assert(mkdtemp(dir));
    jipc_app *app = calloc(1, sizeof *app);
    assert(app);
    snprintf(app->config.state_dir, sizeof app->config.state_dir, "%s", dir);
    snprintf(path, sizeof path, "%s/aim.json", dir);

    assert(!jipc_aim_json(app, out, sizeof out));
    assert(strstr(out, "\"calibrated\":false") && strstr(out, "\"view\":null"));
    /* Nothing to aim with yet. */
    assert(jipc_aim_post(app, "{\"u\":0.5,\"v\":0.5}", error, sizeof error) == 409);
    /* Rejected, and nothing is written. */
    for (const char **bad = (const char *[]){
             "not json", "{}", "{\"u\":0.5}", "{\"u\":1.5,\"v\":0.5}", "{\"u\":-0.1,\"v\":0.5}", "{\"u\":\"a\",\"v\":0.5}",
             "{\"steps\":[1]}", "{\"steps\":[\"a\",1]}",
             "{\"x\":[1,2,3,4,5,6]}",                                   /* one axis */
             "{\"x\":[1,2,3,4,5],\"y\":[1,200,3,4,5,6]}",               /* five numbers */
             "{\"x\":[1,200,3,4,5,\"a\"],\"y\":[1,2,300,4,5,6]}",       /* not a number */
             "{\"x\":[1,200000,3,4,5,6],\"y\":[1,2,300,4,5,6]}",        /* out of range */
             "{\"x\":[1,2,3,4,5,6],\"y\":[1,2,300,4,5,6]}",             /* x does not follow u */
             "{\"x\":[1,200,3,4,5,6],\"y\":[1,2,3,4,5,6]}",             /* y does not follow v */
             NULL};
         *bad; ++bad) {
        assert(jipc_aim_post(app, *bad, error, sizeof error) == 400);
        assert(error[0]);
        assert(access(path, F_OK));
    }
    /* Accepted and kept. */
    assert(jipc_aim_post(app, "{\"x\":[2021,-457,2,-54,5,2],\"y\":[385,-5,220,8,-69,-10]}", error, sizeof error) == 200);
    assert(!access(path, F_OK));
    assert(!jipc_aim_json(app, out, sizeof out));
    assert(strstr(out, "\"calibrated\":true") && strstr(out, "\"x\":[2021,-457,2,-54,5,2]") && strstr(out, "\"y\":[385,-5,220,8,-69,-10]"));
    /* A damaged file counts as no calibration. */
    FILE *file = fopen(path, "w");
    assert(file && fputs("{\"x\":[1", file) >= 0);
    fclose(file);
    assert(!jipc_aim_json(app, out, sizeof out));
    assert(strstr(out, "\"calibrated\":false"));
    unlink(path);
    rmdir(dir);
    free(app);
}

int main(void)
{
    maps();
    api();
    puts("aim: PASS");
    return 0;
}
