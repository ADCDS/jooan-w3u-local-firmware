// SPDX-License-Identifier: MIT
/* Privacy's way back, what can and cannot disturb it, and recalibration, with the fake motor
 * driver of test_platform_motor.c. Scenarios by the independent review of this code. */
#define main motor_main
#include "test_platform_motor.c"
#undef main

#define SYNC() (clock_us = previous_us = jipc_now_us())
#define RUN(s) do { SYNC(); for (unsigned i = 0; i < 40000 && ((s).goto_active || (s).homing); ++i) tick(&(s), clock_us + 10000); } while (0)
#define STEPS(s, n) do { SYNC(); for (int i = 0; i < (n); ++i) tick(&(s), clock_us + 10000); } while (0)

static char position[64];
static void setup(platform_state *s, jipc_app *app)
{
    *s = state_new();
    s->pan_sign = s->tilt_sign = -1; /* mounted upside down, as this camera is */
    s->known = 1;
    s->position_trusted = 1;
    memset(app, 0, sizeof *app);
    pthread_mutex_init(&s->mutex, NULL);
    app->platform_state = s;
    s->app = app;
    moving = dx = dy = swapped = 0;
    snprintf(s->position_path, sizeof s->position_path, "%s", position);
    unlink(position);
}
#define AT(x, y) assert(driver_x == (x) && driver_y == (y))
static void from_view(platform_state *s, jipc_app *app)
{
    setup(s, app);
    driver_x = 1500; driver_y = 300;
    assert(!motor_read(s));
}
static void preset(platform_state *s, int slot, int x, int y)
{
    s->presets[slot - 1] = (saved_preset){.valid = 1, .x = x, .y = y};
    snprintf(s->presets[slot - 1].name, sizeof s->presets[slot - 1].name, "Stop %d", slot);
}

int main(void)
{
    char error[128] = "";
    platform_state s;
    jipc_app app;
    jipc_ptz_motion motion;
    jipc_ptz_status status;
    clock_us = previous_us = 1000000;
    snprintf(position, sizeof position, "/tmp/jipc-privacy-test-%d", (int)getpid());

    /* A daemon restart during privacy: the view is what was saved, wherever the head is. */
    setup(&s, &app); s.saved_valid = 1; s.saved_x = 1500; s.saved_y = 300;
    driver_x = 1500; driver_y = 1230; assert(!motor_read(&s));           /* at the pose */
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); RUN(s);
    AT(1500, 300);
    setup(&s, &app); s.saved_valid = 1; s.saved_x = 1500; s.saved_y = 300;
    driver_x = 1500; driver_y = 700; assert(!motor_read(&s));            /* still on its way there */
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); RUN(s);
    AT(1500, 300);
    setup(&s, &app); s.saved_valid = 1; s.saved_x = 1500; s.saved_y = 300;
    driver_x = 1500; driver_y = 1230; assert(!motor_read(&s));           /* and the pose changed meanwhile */
    preset(&s, 3, 3000, 900);
    assert(!jipc_platform_privacy(&app, 1, 3, error, sizeof error)); RUN(s);
    AT(3000, 900);
    assert(!jipc_platform_privacy(&app, 0, 3, error, sizeof error)); RUN(s);
    AT(1500, 300);
    position_track(&s, clock_us); position_track(&s, clock_us + POSITION_QUIET_US + 1);
    assert(s.saved_x == 1500 && s.saved_y == 300);

    /* Nothing stops the head on its way to the pose. */
    from_view(&s, &app); preset(&s, 1, 100, 100);
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error));
    STEPS(s, 50);
    assert(!platform_control(&app, "ptz_stop", 0, 0, NULL, NULL, NULL, error, sizeof error));
    assert(s.goto_active);
    RUN(s);
    assert(driver_y == 1230);

    /* Stop during a patrol leg counts as someone steering. */
    from_view(&s, &app); preset(&s, 3, 3000, 900);
    assert(!platform_control(&app, "preset_patrol", 3, 0, NULL, NULL, NULL, error, sizeof error));
    STEPS(s, 50);
    assert(!platform_control(&app, "ptz_stop", 0, 0, NULL, NULL, NULL, error, sizeof error));
    assert(!jipc_platform_motion(&app, &motion));
    assert(motion.manual_age_s >= 0 && motion.manual_age_s < 5 && !s.patrol_move);

    /* A recalibration stops trusting the steps until it has ended, and a restart meanwhile homes. */
    FILE *info = fopen(MOTOR_INFO, "w");
    assert(info);
    fputs("## motor is Horizontal motor ##\nmax steps 4080\n## motor is Vertical motor ##\nmax steps 1230\n", info);
    fclose(info);
    int marker = open(HOMED_MARKER, O_WRONLY | O_CREAT, 0600);
    assert(marker >= 0);
    close(marker);
    setup(&s, &app); driver_x = 3500; driver_y = 700; assert(!motor_read(&s));
    assert(!platform_control(&app, "ptz_recalibrate", 0, 0, NULL, NULL, NULL, error, sizeof error));
    assert(!jipc_platform_motion(&app, &motion));
    assert(access(HOMED_MARKER, F_OK) && !motion.trusted);
    RUN(s);
    assert(!jipc_platform_motion(&app, &motion));
    assert(!access(HOMED_MARKER, F_OK) && motion.trusted && s.homed);
    unlink(HOMED_MARKER);
    unlink(MOTOR_INFO);

    /* "Looks down" whichever way up it is mounted. */
    setup(&s, &app); s.pan_sign = s.tilt_sign = 1; driver_x = 1500; driver_y = 300; assert(!motor_read(&s));
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_status(&app, &status, error, sizeof error) && status.tilt == -100);
    from_view(&s, &app);
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_status(&app, &status, error, sizeof error) && status.tilt == -100);

    /* The way back survives privacy turned on again while coming back, however often. */
    from_view(&s, &app);
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); STEPS(s, 30);
    assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); STEPS(s, 10);
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); RUN(s);
    AT(1500, 300);
    from_view(&s, &app);
    for (int k = 0; k < 3; ++k) {
        assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); STEPS(s, 20);
        assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); STEPS(s, 5);
    }
    RUN(s);
    AT(1500, 300);

    /* Turned on during a move, the way back is where it was going. */
    from_view(&s, &app); preset(&s, 3, 3000, 900);
    assert(!platform_control(&app, "preset_goto", 3, 0, NULL, NULL, NULL, error, sizeof error));
    STEPS(s, 100);
    assert(!jipc_platform_privacy(&app, 1, 0, error, sizeof error)); RUN(s);
    assert(!jipc_platform_privacy(&app, 0, 0, error, sizeof error)); RUN(s);
    AT(3000, 900);

    unlink(position);
    puts("privacy on the motor: PASS");
    return 0;
}
