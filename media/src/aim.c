/* SPDX-License-Identifier: MIT
 * Aiming the pan/tilt lens at a point of the fixed lens's picture.
 *
 * A one-time calibration (tools/aim-calibrate.py) gives the motor steps that centre
 * a point of that picture, each axis as c0 + c1*u + c2*v + c3*u*v + c4*u*u + c5*v*v
 * with u and v from -1 to 1 across it. It holds for the distant scene: the lenses are
 * some way apart, so something next to the camera lands off centre.
 *
 * The calibration lives in state_dir/aim.json, not with the settings, because it
 * belongs to how this camera's lenses are mounted: a reset to defaults keeps it.
 */
#include "jipc.h"
#include <cJSON.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define AIM_LIMIT 100000.0
#define AIM_MIN_SPAN 50.0 /* steps across half the picture: fewer is not a calibration */

static double polynomial(const double c[6], double u, double v)
{
    return c[0] + c[1] * u + c[2] * v + c[3] * u * v + c[4] * u * u + c[5] * v * v;
}

void jipc_aim_steps(const jipc_aim *a, double u, double v, double *x, double *y)
{
    u = u * 2 - 1; v = v * 2 - 1;
    *x = polynomial(a->x, u, v);
    *y = polynomial(a->y, u, v);
}

int jipc_aim_where(const jipc_aim *a, double x, double y, double *u, double *v)
{
    double p = 0, q = 0;
    for (int i = 0; i < 12; ++i) {
        double fx = polynomial(a->x, p, q) - x, fy = polynomial(a->y, p, q) - y;
        double xu = a->x[1] + a->x[3] * q + 2 * a->x[4] * p, xv = a->x[2] + a->x[3] * p + 2 * a->x[5] * q;
        double yu = a->y[1] + a->y[3] * q + 2 * a->y[4] * p, yv = a->y[2] + a->y[3] * p + 2 * a->y[5] * q;
        double det = xu * yv - xv * yu;
        if (fabs(det) < 1) return -1;
        double dp = (fx * yv - fy * xv) / det, dq = (fy * xu - fx * yu) / det;
        p -= dp; q -= dq;
        if (fabs(p) > 8 || fabs(q) > 8) return -1; /* far outside the picture */
        if (fabs(dp) < 1e-4 && fabs(dq) < 1e-4) {
            *u = (p + 1) / 2; *v = (q + 1) / 2;
            return 0;
        }
    }
    return -1;
}

static int read_list(cJSON *root, const char *key, double out[6])
{
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, key), *item;
    int n = 0;
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) != 6) return -1;
    cJSON_ArrayForEach(item, list) {
        if (!cJSON_IsNumber(item) || fabs(item->valuedouble) > AIM_LIMIT) return -1;
        out[n++] = item->valuedouble;
    }
    return 0;
}

static int read_calibration(cJSON *root, jipc_aim *a)
{
    if (read_list(root, "x", a->x) || read_list(root, "y", a->y)) return -1;
    return fabs(a->x[1]) < AIM_MIN_SPAN || fabs(a->y[2]) < AIM_MIN_SPAN ? -1 : 0;
}

static void aim_path(jipc_app *app, char *path, size_t capacity)
{
    snprintf(path, capacity, "%s/aim.json", app->config.state_dir);
}

static int aim_load(jipc_app *app, jipc_aim *a)
{
    char path[600], text[512];
    aim_path(app, path, sizeof path);
    FILE *file = fopen(path, "re");
    if (!file) return -1;
    size_t n = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[n] = 0;
    cJSON *root = cJSON_Parse(text);
    int rc = !root || read_calibration(root, a) ? -1 : 0;
    cJSON_Delete(root);
    return rc;
}

static int list_json(char *out, size_t capacity, const double c[6])
{
    return snprintf(out, capacity, "[%.0f,%.0f,%.0f,%.0f,%.0f,%.0f]", c[0], c[1], c[2], c[3], c[4], c[5]);
}

int jipc_aim_json(jipc_app *app, char *out, size_t capacity)
{
    jipc_aim a;
    int x = 0, y = 0, known = !jipc_platform_steps(app, &x, &y), calibrated = !aim_load(app, &a);
    char steps[48] = "null", view[120] = "null", xs[96] = "[]", ys[96] = "[]";
    if (known) snprintf(steps, sizeof steps, "[%d,%d]", x, y);
    if (calibrated) {
        double u, v;
        list_json(xs, sizeof xs, a.x); list_json(ys, sizeof ys, a.y);
        if (known && !jipc_aim_where(&a, x, y, &u, &v))
            snprintf(view, sizeof view, "{\"u\":%.3f,\"v\":%.3f,\"w\":%.3f,\"h\":%.3f}", u, v,
                     JIPC_FOV_HALF_PAN / fabs(a.x[1]), JIPC_FOV_HALF_TILT / fabs(a.y[2]));
    }
    int n = snprintf(out, capacity, "{\"calibrated\":%s,\"steps\":%s,\"view\":%s,\"x\":%s,\"y\":%s}\n",
                     calibrated ? "true" : "false", steps, view, xs, ys);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}

/* A calibration (x and y), a point of the picture (u and v, 0 to 1), or motor steps. */
int jipc_aim_post(jipc_app *app, const char *json, char *error, size_t capacity)
{
    cJSON *root = cJSON_Parse(json);
    int status = 400;
    double u = 0, v = 0;
    if (!root) { snprintf(error, capacity, "Invalid JSON"); return 400; }
    cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "u"), *cv = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    if (cJSON_GetObjectItemCaseSensitive(root, "x")) {
        jipc_aim a;
        char path[600], text[256], xs[96], ys[96];
        if (read_calibration(root, &a)) { snprintf(error, capacity, "x and y must be six numbers, and each must follow its axis"); goto done; }
        list_json(xs, sizeof xs, a.x); list_json(ys, sizeof ys, a.y);
        int n = snprintf(text, sizeof text, "{\"x\":%s,\"y\":%s}\n", xs, ys);
        aim_path(app, path, sizeof path);
        if (n < 0 || (size_t)n >= sizeof text || jipc_atomic_file(path, text, (size_t)n, 0600)) {
            snprintf(error, capacity, "Cannot save the calibration: %s", strerror(errno));
            status = 500; goto done;
        }
        status = 200;
    } else if (cJSON_IsNumber(cu) && cJSON_IsNumber(cv)) {
        jipc_aim a;
        double x, y;
        u = cu->valuedouble; v = cv->valuedouble;
        if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) { snprintf(error, capacity, "u and v must be between 0 and 1"); goto done; }
        if (aim_load(app, &a)) { snprintf(error, capacity, "Aiming is not calibrated"); status = 409; goto done; }
        jipc_aim_steps(&a, u, v, &x, &y);
        status = jipc_platform_move(app, JIPC_MOVE_STEPS, x, y, 0, error, capacity) ? 409 : 200;
    } else if (cJSON_IsArray(steps) && cJSON_GetArraySize(steps) == 2 &&
               cJSON_IsNumber(cJSON_GetArrayItem(steps, 0)) && cJSON_IsNumber(cJSON_GetArrayItem(steps, 1))) {
        status = jipc_platform_move(app, JIPC_MOVE_STEPS, cJSON_GetArrayItem(steps, 0)->valuedouble,
                                    cJSON_GetArrayItem(steps, 1)->valuedouble, 0, error, capacity) ? 409 : 200;
    } else snprintf(error, capacity, "Send u and v, steps, or a calibration");
done:
    cJSON_Delete(root);
    return status;
}
