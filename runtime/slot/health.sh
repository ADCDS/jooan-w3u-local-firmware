#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
: "${JL_RUN:=/run/jooan-local}"
pid=$(cat "$JL_RUN/daemon.pid")
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
[ -d "/proc/$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = joan-daemon ] || exit 1
netstat -lnt 2>/dev/null | grep -q ':80[[:space:]]' || exit 1
# The OEM application runs only in the fallback modes (handoff, rescue,
# platform-only); a native boot never starts it, and that must not fail a
# runtime trial. Whenever it does run, it is acceptable only with the exact
# containment preload mapped and the guard's control socket up.
for comm in /proc/[0-9]*/comm; do
    [ "$(cat "$comm" 2>/dev/null)" = jooanipc ] || continue
    grep -q '/run/jooan-local/controller/shared/libjooan_guard.so' "${comm%/comm}/maps" || exit 1
    [ -S /tmp/jooan-guard-talkback.sock ] || exit 1
done
exit 0
