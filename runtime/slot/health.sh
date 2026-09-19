#!/bin/sh
set -eu
: "${JL_RUN:=/run/jooan-local}"
pid=$(cat "$JL_RUN/daemon.pid")
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
[ -d "/proc/$pid" ] || exit 1
netstat -lnt 2>/dev/null | grep -q ':443[[:space:]]' || exit 1
# The OEM media process is acceptable only when the exact preload is mapped.
ipc=
for comm in /proc/[0-9]*/comm; do
    [ "$(cat "$comm" 2>/dev/null)" = jooanipc ] || continue
    ipc=${comm%/comm}
    break
done
[ -n "$ipc" ] || exit 1
grep -q '/opt/custom/jooan-local/shared/libjooan_guard.so' "$ipc/maps" || exit 1
exit 0
