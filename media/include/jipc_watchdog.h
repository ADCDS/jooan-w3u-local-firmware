/* SPDX-License-Identifier: MIT */
#ifndef JIPC_WATCHDOG_H
#define JIPC_WATCHDOG_H
int jipc_watchdog_open(void);
int jipc_watchdog_feed(int fd);
#endif
