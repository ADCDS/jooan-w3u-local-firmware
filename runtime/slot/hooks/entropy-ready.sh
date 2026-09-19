#!/bin/sh
set -eu
[ -r /dev/urandom ] || exit 1
i=0
while [ "$i" -lt 30 ]; do
    value=$(cat /proc/sys/kernel/random/entropy_avail 2>/dev/null || echo 0)
    case "$value" in ''|*[!0-9]*) value=0 ;; esac
    [ "$value" -ge 128 ] && exit 0
    sleep 1
    i=$((i + 1))
done
exit 1
