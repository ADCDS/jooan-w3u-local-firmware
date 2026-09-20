#!/bin/sh
set -eu
: "${JL_RUN:=/run/jooan-local}"
pid=$(cat "$JL_RUN/daemon.pid")
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
[ -d "/proc/$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = joan-daemon ] || exit 1
netstat -lnt 2>/dev/null | grep -q ':443[[:space:]]' || exit 1
# Audio is a release feature, not an optional best-effort sidecar. Qualify the
# router and both local IPC directions before an A/B trial can be promoted.
audio_pid=$(cat "$JL_RUN/audio-router.pid")
case "$audio_pid" in ''|*[!0-9]*) exit 1 ;; esac
[ -d "/proc/$audio_pid" ] &&
    [ "$(cat "/proc/$audio_pid/comm" 2>/dev/null)" = audio-router ] || exit 1
[ -S "$JL_RUN/audio-ws.sock" ] && [ -S "$JL_RUN/mic.sock" ] || exit 1
# The OEM media process is acceptable only when the exact preload is mapped.
ipc=
for comm in /proc/[0-9]*/comm; do
    [ "$(cat "$comm" 2>/dev/null)" = jooanipc ] || continue
    ipc=${comm%/comm}
    break
done
[ -n "$ipc" ] || exit 1
grep -q '/run/jooan-local/controller/shared/libjooan_guard.so' "$ipc/maps" || exit 1
[ -S /tmp/jooan-guard-talkback.sock ] || exit 1
exit 0
