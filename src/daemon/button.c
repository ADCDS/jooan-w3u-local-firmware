/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
/* The reset button: held for 5 seconds it resets the camera to defaults, the
 * same way the admin UI does. GPIO 62, which reads 0 while pressed (ResetGpio
 * and ResetLevel in the vendor configuration). Native boots only: where the OEM
 * application runs, the button is its own. It arms only after seeing the button
 * released, so a stuck or unreadable line can never erase anything. */
#include "joan_daemon.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0 /* the old MIPS headers; these descriptors are open for an instant */
#endif

#define BUTTON_GPIO "62"
#define BUTTON_DIR "/sys/class/gpio/gpio" BUTTON_GPIO
#define NATIVE_MARK "/opt/jooanipc-open/native"
#define TICK_NS 250000000L
#define HOLD_TICKS 20 /* 5 seconds */

static char value_path[256];

static void write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, text, strlen(text)) < 0) { /* already exported, or no such line: reading will tell */ }
    close(fd);
}

/* 1 released, 0 pressed, -1 unreadable. */
static int button_level(void)
{
    char c = 0; int fd = open(value_path, O_RDONLY | O_CLOEXEC); ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, &c, 1);
    close(fd);
    return n == 1 && (c == '0' || c == '1') ? c - '0' : -1;
}

static void *button_main(void *arg)
{
    const JoanConfig *cfg = arg; int armed = 0, held = 0;
    const struct timespec tick = { 0, TICK_NS };
    for (;;) {
        int level;
        nanosleep(&tick, NULL);
        level = button_level();
        if (level != 0) { armed = level == 1; held = 0; continue; }
        if (!armed || ++held < HOLD_TICKS) continue;
        armed = 0; held = 0;
        if (joan_oem_media_running()) continue;
        fprintf(stderr, "reset button held: resetting to defaults\n");
        (void)joan_run_helper(cfg, "factory-reset", NULL, NULL, NULL, NULL, 10);
    }
    return NULL;
}

int joan_button_start(const JoanConfig *cfg)
{
    const char *dir = getenv("JOAN_BUTTON_GPIO_DIR"); /* tests only */
    pthread_t thread;
    if (!dir) {
        if (access(NATIVE_MARK, F_OK)) return 0;
        write_text("/sys/class/gpio/export", BUTTON_GPIO);
        write_text(BUTTON_DIR "/direction", "in");
        dir = BUTTON_DIR;
    }
    if (snprintf(value_path, sizeof(value_path), "%s/value", dir) >= (int)sizeof(value_path)) return -1;
    if (pthread_create(&thread, NULL, button_main, (void *)cfg)) return -1;
    pthread_detach(thread);
    return 0;
}
