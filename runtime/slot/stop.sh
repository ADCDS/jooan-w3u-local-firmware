#!/bin/sh
set -eu
: "${JL_RUN:=/run/jooan-local}"
for name in daemon audio-router; do
    file=$JL_RUN/$name.pid
    [ -f "$file" ] || continue
    pid=$(cat "$file" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    kill -TERM "$pid" 2>/dev/null || :
done
sleep 1
for name in daemon audio-router; do
    file=$JL_RUN/$name.pid
    [ -f "$file" ] || continue
    pid=$(cat "$file" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    kill -KILL "$pid" 2>/dev/null || :
done
exit 0
