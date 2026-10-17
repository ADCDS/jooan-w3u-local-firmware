#!/bin/sh
# SPDX-License-Identifier: MIT
# JA-A12 only: additive end-of-local.rc hook, after the guarded OEM bootstrap.
# Credentials stay in /opt; public executables and runtime libraries live on SD.
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
unset LD_PRELOAD LD_LIBRARY_PATH
umask 077
trap '' HUP
calibration_check() {
    awk -v marker="$1" '
    index($0,marker) {seen=1; phase=0; ok=0; next}
    !seen {next}
    /\[motor_step_reset:418\].*start run ptz/ {phase=1; bad=0; ok=0; next}
    /MOTOR_RESET is error|wait go to last position, timeout/ {bad=1; ok=0}
    phase==1 && /\[motor_step_reset:427\].*end run left,start reset/ {phase=2; next}
    phase==2 && /\[motor_step_reset:449\].*reset curMotorMsg.status:0/ {phase=3; next}
    phase==3 && /\[motor_step_reset:459\].*end run reset.*start run/ {phase=4; next}
    phase==4 && /\[motor_step_reset:555\][[:space:]]*reset gst_curMotorMsg x=[0-9]+,[[:space:]]*y=[0-9]+[[:space:]]+status\[0\]/ {ok=!bad}
    END {exit !ok}'
}
# Read-only fixture entry point: consumes syslog on stdin, opens no devices.
if [ "$#" -gt 0 ]; then
    [ "$#" = 2 ] && [ "$1" = --check-calibration ] || exit 2
    calibration_check "$2"
    exit $?
fi
root=/opt/jooanipc-open
run=/run/jooanipc-open
log() { printf '%s\n' "jipc-handoff: $*"; }
fail() { log "$*; leaving current owner alone (trial marker retained if set)"; exit 1; }
mkdir -p "$run" || exit 1
mkdir "$run/boot.lock" 2>/dev/null || exit 0
boot=$(cat /proc/sys/kernel/random/boot_id) || exit 1
[ -f "$root/enabled" ] || exit 0
[ ! -f "$root/boot-trial" ] || fail 'previous boot trial did not qualify; guarded OEM rescue selected'

# A restored or rotated syslog can contain a previous boot's calibration line.
# Insert a unique marker before this boot's OEM application starts, then require
# the complete successful homing episode after it. Missing evidence leaves OEM.
marker="JIPC_HANDOFF_BOOT=$boot"
i=0
while [ ! -f /tmp/message.log ] && [ "$i" -lt 300 ]; do
    sleep 0.2; i=$((i + 1))
done
[ -f /tmp/message.log ] || fail 'syslog did not appear'
printf '\n%s\n' "$marker" >> /tmp/message.log || fail 'cannot mark syslog'
calibrated() {
    # syslogd rotates once; .0 precedes current chronologically.
    { [ ! -f /tmp/message.log.0 ] || cat /tmp/message.log.0; cat /tmp/message.log; } |
        calibration_check "$marker"
}
i=0
until calibrated; do
    [ "$i" -lt 180 ] || fail 'fresh motor calibration was not observed'
    sleep 1; i=$((i + 1))
done
log 'fresh OEM motor calibration observed'

common=/run/jooan-local/controller/boot/common.sh
[ -r "$common" ] || fail 'existing network controller unavailable'
# Trusted, retained controller helpers, already verified by the original hook.
# shellcheck disable=SC1090
. "$common"
wifi_key='' wifi_since=0
control_ready() {
    jl_read_selection || return 1
    case "$JL_STABLE" in A|B) ;; *) return 1 ;; esac
    [ "$JL_PENDING" = - ] && [ "$JL_ATTEMPTED" = 0 ] || return 1
    [ "$(cat /run/jooan-local/running 2>/dev/null)" = "$JL_STABLE" ] || return 1
    [ ! -d /run/jooan-local/state.lock ] || return 1
    [ ! -e /opt/custom/jooan-local/state/wifi-trial ] || return 1
    s=$(cat /run/jooan-local/supervisor.pid 2>/dev/null) || return 1
    case "$s" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$s" 2>/dev/null && jl_runtime_alive || return 1
    d=$(cat /run/jooan-local/dropbear.pid 2>/dev/null) || return 1
    case "$d" in ''|*[!0-9]*) return 1 ;; esac
    [ "$(cat "/proc/$d/comm" 2>/dev/null)" = dropbear ] || return 1
    w=$(jl_wpa_pid) || return 1
    [ "$(cat /run/jooan-local/wifi-applied 2>/dev/null)" = "$w" ] || return 1
    wt=$(awk '{print $22}' "/proc/$w/stat") || return 1
    now=$(awk '{print int($1)}' /proc/uptime) || return 1
    if [ "$wifi_key" != "$w:$wt" ]; then wifi_key=$w:$wt; wifi_since=$now; return 1; fi
    [ "$((now - wifi_since))" -ge 30 ] || return 1
    wpa_cli -iwlan0 status > "$run/wifi-status" 2>/dev/null || return 1
    grep -q '^wpa_state=COMPLETED$' "$run/wifi-status" || return 1
    grep -q '^ip_address=[0-9]' "$run/wifi-status" || return 1
    read -r gateway interface < /run/jooan-local/default-gateway || return 1
    [ "$interface" = wlan0 ] || return 1
    arping -c 1 -w 2 -I "$interface" "$gateway" >/dev/null 2>&1 || return 1
    grep -q ' /mnt/sd_card ' /proc/mounts || return 1
    [ "$(df -k /opt | awk 'END {print $4}')" -ge 64 ] || return 1
}
i=0
until control_ready; do
    [ "$i" -lt 120 ] || fail 'network/controller/storage not committed and healthy'
    sleep 1; i=$((i + 1))
done

[ -r "$root/deployment.env" ] || fail 'deployment manifest missing'
# This root-owned JFFS2 file contains paths and expected hashes, never passwords.
# shellcheck disable=SC1091
. "$root/deployment.env"
case "$JIPC_RELEASE" in /mnt/sd_card/jooanipc-open/releases/*) ;; *) fail 'invalid release path' ;; esac
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

# Capture identities before sending a signal; never kill unrelated PID reuse.
find_owner() {
    found=
    for file in /proc/[0-9]*/comm; do
        [ "$(cat "$file" 2>/dev/null)" = "$1" ] || continue
        [ -z "$found" ] || return 1
        found=${file#/proc/}; found=${found%/comm}
    done
    [ -n "$found" ] || return 1
    printf '%s\n' "$found"
}
same() {
    [ "$(cat "/proc/$1/comm" 2>/dev/null)" = "$2" ] &&
        [ "$(awk '{print $22}' "/proc/$1/stat" 2>/dev/null)" = "$3" ]
}
oem=$(find_owner jooanipc) || fail 'expected exactly one OEM media owner'
watch=$(find_owner my_watch_dog) || fail 'expected exactly one OEM watchdog owner'
os=$(awk '{print $22}' "/proc/$oem/stat") || exit 1
ws=$(awk '{print $22}' "/proc/$watch/stat") || exit 1
find_owner jooanipc-open >/dev/null && fail 'native media owner already exists'
control_ready || fail 'network/controller changed before transfer'

# Persistent failure latch prevents an automatic crash/reboot retry loop. It is
# cleared only after the replacement lives beyond a complete watchdog period.
printf '%s\n' "$boot" > "$root/boot-trial.new" || fail 'cannot write boot trial'
mv "$root/boot-trial.new" "$root/boot-trial" || fail 'cannot persist boot trial'
sync
same "$watch" my_watch_dog "$ws" || fail 'watchdog identity changed'
kill -TERM "$watch" || fail 'cannot stop old watchdog'
i=0
while same "$watch" my_watch_dog "$ws" && [ "$i" -lt 5 ]; do sleep 1; i=$((i + 1)); done
if same "$watch" my_watch_dog "$ws"; then kill -KILL "$watch" || fail 'cannot stop old watchdog'; sleep 1; fi
same "$watch" my_watch_dog "$ws" && fail 'old watchdog did not exit'
same "$oem" jooanipc "$os" || fail 'OEM media identity changed'
kill -TERM "$oem" || fail 'cannot stop OEM media'
i=0
while same "$oem" jooanipc "$os" && [ "$i" -lt 60 ]; do sleep 1; i=$((i + 1)); done
if same "$oem" jooanipc "$os"; then kill -KILL "$oem" || fail 'cannot stop OEM media'; sleep 2; fi
same "$oem" jooanipc "$os" && fail 'OEM media did not exit'

# Check every task's FDs: a zombie leader may retain a live DMA worker.
clean_media() {
    taint=$(cat /proc/sys/kernel/tainted) || return 1
    [ "$((taint & 128))" = 0 ] || return 1
    grep -q 'IVDC DDR Addr *: 0x00000000' /proc/jz/isp/isp-ivdc || return 1
    ls -l /proc/[0-9]*/task/[0-9]*/fd/* > "$run/fds" 2>/dev/null || :
    [ -s "$run/fds" ] || return 1
    ! grep -Eq ' -> /dev/(rmem|tx-isp|framechan|soc_vpu|misc-ivdc|watchdog)' "$run/fds"
}
i=0
until clean_media; do
    [ "$i" -lt 10 ] || fail 'kernel/IVDC/media descriptors did not detach; reset required'
    sleep 1; i=$((i + 1))
done
log 'OEM media detached; transferring the armed watchdog to native media'
(
    trap '' HUP
    echo -800 > /proc/self/oom_score_adj 2>/dev/null || :
    exec "$JIPC_RELEASE/jooanipc-open" --config "$root/camera.ini"
) </dev/null >/tmp/jooanipc-open.log 2>&1 &
native=$!
printf '%s\n' "$native" > /tmp/jooanipc-open.pid
ns=$(awk '{print $22}' "/proc/$native/stat") || fail 'new process vanished'
printf '%s\n' "$ns" > /tmp/jooanipc-open.start
native_ready() {
    same "$native" jooanipc-open "$ns" || return 1
    grep -q 'ready: backend=imp, 4 video streams' /tmp/jooanipc-open.log || return 1
    # Numeric procfs FD names; BusyBox has no readlink applet.
    # shellcheck disable=SC2010
    ls -l "/proc/$native/fd" 2>/dev/null | grep -q ' -> /dev/watchdog$'
}
i=0
until native_ready; do
    kill -0 "$native" 2>/dev/null || fail 'native service exited; armed watchdog will recover'
    [ "$i" -lt 40 ] || fail 'native service did not become ready'
    sleep 1; i=$((i + 1))
done
log "native pid=$native ready; qualifying for 185 seconds"
i=0
while [ "$i" -lt 185 ]; do
    sleep 1; i=$((i + 1))
    native_ready || fail 'native trial lost health; armed watchdog will recover'
done
control_ready || fail 'final controller health check failed'
native_ready || fail 'final native health check failed'
printf '%s\n' "$boot $native" > "$run/qualified"
rm -f "$root/boot-trial"
sync
log 'boot handoff qualified; network controller, SSH and native watchdog retained'
