#!/bin/sh
# SPDX-License-Identifier: MIT
# JA-A12 native boot: the OEM application never starts. The local controller's
# local.rc (0.2.23+) suppresses jooanipc and my_watch_dog, arms the rootfs
# watchdog feeder, starts Wi-Fi and DHCP, then runs this script. It launches
# the media daemon, which homes pan/tilt and takes over the watchdog, and
# qualifies the boot. Until qualification, $root/native-trial records the
# stage; a boot that leaves it behind makes the next boot latch native-failed
# and use the guarded OEM handoff (boot-handoff.sh) instead.
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
unset LD_PRELOAD LD_LIBRARY_PATH
umask 077
root=/opt/jooanipc-open
run=/run/jooanipc-open
feeder_pid=/tmp/jipc-watchdog-feeder.pid
log() { printf '%s jipc-native: %s\n' "$(cut -d' ' -f1 /proc/uptime)" "$*"; }
# The trial marker is the persistent record of how far this boot got.
stage() {
    printf 'boot=%s\nstage=%s\n' "$boot" "$*" > "$root/native-trial.new" &&
        mv -f "$root/native-trial.new" "$root/native-trial" && sync
}
fail() {
    log "$*; native-trial kept, the next boot uses the OEM handoff"
    stage "failed: $*"
    # Without a launched daemon, stop feeding now: the reset falls back sooner.
    if [ -z "$native" ]; then
        f=$(cat "$feeder_pid" 2>/dev/null)
        [ -n "$f" ] && [ "$(cat "/proc/$f/comm" 2>/dev/null)" = watchdog ] && kill -KILL "$f"
    fi
    exit 1
}
mkdir -p "$run" || exit 1
mkdir "$run/boot.lock" 2>/dev/null || exit 0
[ -f "$root/enabled" ] && [ -f "$root/native" ] || exit 0
native=
boot=$(cat /proc/sys/kernel/random/boot_id) || exit 1
stage start || exit 1

[ -r "$root/deployment.env" ] || fail 'deployment manifest missing'
# shellcheck disable=SC1091
. "$root/deployment.env"
# The handoff script owns release-path and storage qualification; reuse it.
sh "$root/boot-handoff.sh" --check-release-storage "$JIPC_RELEASE" \
    "$(CDPATH='' cd -P "$JIPC_RELEASE" 2>/dev/null && pwd -P)" < /proc/mounts ||
    fail 'release path or storage not qualified'
[ -x "$JIPC_RELEASE/jooanipc-open" ] || fail 'release missing'
for name in jooanipc-open libimp.so libatomic.so.1; do
    case "$name" in
        jooanipc-open) expected=$JIPC_APP_MD5 ;;
        libimp.so) expected=$JIPC_IMP_MD5 ;;
        libatomic.so.1) expected=$JIPC_ATOMIC_MD5 ;;
    esac
    actual=$(md5sum "$JIPC_RELEASE/$name") || fail "cannot checksum $name"
    [ "${actual%% *}" = "$expected" ] || fail "release checksum mismatch: $name"
done
LD_LIBRARY_PATH=$JIPC_RELEASE
OPENIMP_T23_DRIVER_ABI=oem80
OPENIMP_T23_ENCODER=native
OPENIMP_T23_NATIVE_FALLBACK=0
export LD_LIBRARY_PATH OPENIMP_T23_DRIVER_ABI OPENIMP_T23_ENCODER OPENIMP_T23_NATIVE_FALLBACK JIPC_LIBIMP_SHA256
"$JIPC_RELEASE/jooanipc-open" --config "$root/camera.ini" --check-config || fail 'configuration rejected'
grep -qx 'motor_home=1' "$root/camera.ini" || fail 'native boot requires motor_home=1'

# The daemon binds the camera's address: wait for Wi-Fi, DHCP and the gateway.
stage network
network_ready() {
    read -r gateway interface < /run/jooan-local/default-gateway || return 1
    [ "$interface" = wlan0 ] || return 1
    ifconfig wlan0 2>/dev/null | grep -q 'inet addr:' || return 1
    arping -c 1 -w 2 -I wlan0 "$gateway" >/dev/null 2>&1
}
i=0
until network_ready; do
    [ "$i" -lt 300 ] || fail 'Wi-Fi/DHCP did not provide the gateway'
    sleep 1; i=$((i + 1))
done
log 'network ready'

# Hand the armed watchdog from the rootfs feeder to the media daemon.
stage launch
f=$(cat "$feeder_pid" 2>/dev/null)
[ -n "$f" ] && [ "$(cat "/proc/$f/comm" 2>/dev/null)" = watchdog ] || fail 'rootfs watchdog feeder missing'
kill -TERM "$f" || fail 'cannot stop the rootfs watchdog feeder'
i=0
while [ "$(cat "/proc/$f/comm" 2>/dev/null)" = watchdog ] && [ "$i" -lt 10 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "/proc/$f/comm" 2>/dev/null)" = watchdog ] && fail 'rootfs watchdog feeder did not exit'
(
    trap '' HUP
    echo -800 > /proc/self/oom_score_adj 2>/dev/null || :
    exec "$JIPC_RELEASE/jooanipc-open" --config "$root/camera.ini"
) </dev/null >/tmp/jooanipc-open.log 2>&1 &
native=$!
printf '%s\n' "$native" > /tmp/jooanipc-open.pid
ns=$(awk '{print $22}' "/proc/$native/stat") || fail 'media daemon vanished'
printf '%s\n' "$ns" > /tmp/jooanipc-open.start
native_ready() {
    [ "$(cat "/proc/$native/comm" 2>/dev/null)" = jooanipc-open ] || return 1
    [ "$(awk '{print $22}' "/proc/$native/stat" 2>/dev/null)" = "$ns" ] || return 1
    grep -q 'ready: backend=imp, 4 video streams' /tmp/jooanipc-open.log || return 1
    # Numeric procfs FD names; BusyBox has no readlink applet.
    # shellcheck disable=SC2010
    ls -l "/proc/$native/fd" 2>/dev/null | grep -q ' -> /dev/watchdog$'
}
i=0
until native_ready; do
    kill -0 "$native" 2>/dev/null || fail 'media daemon exited; its armed watchdog resets the camera'
    [ "$i" -lt 60 ] || fail 'media daemon did not become ready'
    sleep 1; i=$((i + 1))
done
log "media daemon pid=$native ready; qualifying for 185 seconds"

# One complete watchdog period with media, homing and the network healthy.
stage qualify
i=0
while [ "$i" -lt 185 ]; do
    sleep 1; i=$((i + 1))
    native_ready || fail 'media daemon lost health during qualification'
    grep -q 'motor homing complete' /tmp/jooanipc-open.log && homed=1
    grep -q -E 'motor homing( start)?:.*(error|failed)|platform: motor homing' /tmp/jooanipc-open.log &&
        fail 'pan/tilt homing failed'
done
[ -n "$homed" ] || fail 'pan/tilt homing did not complete'
i=0
until network_ready; do
    native_ready || fail 'media daemon lost health during final network check'
    [ "$i" -lt 60 ] || fail 'network did not stay healthy'
    sleep 1; i=$((i + 1))
done
printf '%s\n' "$boot $native" > "$run/qualified"
rm -f "$root/native-trial" "$root/native-trial.new"
sync
log 'native boot qualified; OEM application never started'
