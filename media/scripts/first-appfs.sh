#!/bin/sh
# SPDX-License-Identifier: MIT
# JA-A12: the first media release, over the stock app partition, run on the camera by
# install-camera.py with a bundle from build-appfs.sh --first. The stock application is running and owns the armed
# watchdog; a rootfs feeder takes it over for the flash. Anything still using the
# partition stops this before the flash, with a reboot into the untouched stock image.
# upgrade-appfs.sh does the same from a running media daemon; it is separate so that the
# path every later update takes stays as it was qualified.
# NOT YET RUN ON A CAMERA: tested only with static checks and its bundle offline.
# Usage: first-appfs.sh NEW_IMAGE_MD5 CURRENT_APPFS_MD5
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
unset LD_LIBRARY_PATH LD_PRELOAD
umask 077
trap '' HUP
cd /tmp
image=/tmp/jipc-flash-candidate.squashfs
backup=/tmp/jipc-appfs-rollback.bin
root=/opt/jooanipc-open
state=/tmp/jipc-install
platform_lock=/run/jooan-local/state.lock
platform_apply=/run/jooan-local/firmware-apply.lock
wifi_trial=/opt/custom/jooan-local/state/wifi-trial
mkdir -p "$state"
log() { printf '%s %s\n' "$(cat /proc/uptime)" "$*"; }
die() { log "ABORT: $*"; exit 1; }
abort_reboot() { trap - EXIT; log "ABORT before flash: $*; rebooting into the unchanged appfs"; sync; /bin/busybox reboot -f; exit 1; }
hash() { md5sum "$1" | awk '{print $1}'; }
same() {
    [ "$(cat "/proc/$1/comm" 2>/dev/null)" = "$2" ] &&
        [ "$(awk '{print $22}' "/proc/$1/stat" 2>/dev/null)" = "$3" ]
}
owner() {
    found=
    for file in /proc/[0-9]*/comm; do
        [ "$(cat "$file" 2>/dev/null)" = "$1" ] || continue
        [ -z "$found" ] || return 1
        found=${file#/proc/}; found=${found%/comm}
    done
    [ -n "$found" ] || return 1
    printf '%s\n' "$found"
}
absent() {
    for file in /proc/[0-9]*/comm; do
        [ "$(cat "$file" 2>/dev/null)" = "$1" ] && return 1
    done
    return 0
}
stop_owner() {
    pid=$1; name=$2; start=$3
    same "$pid" "$name" "$start" || abort_reboot "$name identity changed"
    kill -TERM "$pid" || abort_reboot "cannot stop $name"
    i=0
    while same "$pid" "$name" "$start" && [ "$i" -lt "$4" ]; do
        sleep 1; i=$((i+1))
    done
    if same "$pid" "$name" "$start"; then
        kill -KILL "$pid"
        i=0
        while same "$pid" "$name" "$start" && [ "$i" -lt 15 ]; do
            sleep 1; i=$((i+1))
        done
    fi
    same "$pid" "$name" "$start" && abort_reboot "$name remains alive"
    log "$name stopped"
}
holds_watchdog() {
    # shellcheck disable=SC2010
    ls -l "/proc/$1/fd" 2>/dev/null | grep -q ' -> /dev/watchdog$'
}
# The camera goes back to having no media layer.
restore_config() {
    rm -f "$root/boot-handoff.sh" "$root/camera.ini" "$root/deployment.env" "$root/enabled" "$root/native"
    sync
}

# Once the stock application is stopped, an exit of any kind must end in a reboot and
# not in a camera with nothing running, including one set -e takes by surprise.
phase=checking
on_exit() {
    case "$phase" in
        checking) log 'ABORT: stopped before changing anything' ;;
        stopped) trap - EXIT; log 'ABORT before flash: stopped unexpectedly; rebooting into the unchanged appfs'
                 sync; /bin/busybox reboot -f ;;
        commit) trap - EXIT; log 'ABORT before flash: could not commit the configuration; undoing it and rebooting'
                restore_config; /bin/busybox reboot -f ;;
    esac
}
trap on_exit EXIT

# Preconditions: nothing below changes the camera until all of these pass.
[ "$#" = 2 ] || die 'usage: first-appfs.sh NEW_IMAGE_MD5 CURRENT_APPFS_MD5'
[ "$(cat /sys/class/mtd/mtd4/size)" = 3211264 ] || die 'wrong partition size'
[ "$(cat /sys/class/mtd/mtd4/name)" = appfs ] || die 'wrong partition name'
flags=$(cat /sys/class/mtd/mtd4/flags)
[ "$((flags & 1024))" -ne 0 ] || die 'appfs is not writable through MTD'
[ "$(wc -c < "$image")" = 3211264 ] || die 'wrong image size'
[ "$(hash "$image")" = "$1" ] || die 'image checksum mismatch'
[ "$(hash /dev/mtdblock4)" = "$2" ] || die 'original appfs changed'
grep -q '^/dev/mtdblock4 /mnt/mtd squashfs ro[, ]' /proc/mounts || die 'unexpected appfs mount'
[ ! -e "$root/native" ] && [ ! -e "$root/enabled" ] || die 'this camera already has a media layer'
[ "$(df -k /opt | awk 'END {print $4}')" -ge 64 ] || die 'insufficient config space'
for name in boot-handoff.sh camera.ini deployment.env; do
    [ -s "$root/$name.next" ] || die "missing staged $name"
done
[ -s "$root/native-boot.sh" ] && [ -s "$root/cert.pem" ] && [ -s "$root/key.pem" ] || die 'missing boot script or certificate'
sh -n "$root/boot-handoff.sh.next"
sh -n "$root/native-boot.sh"
absent jooanipc-open || die 'a media daemon is already running'
oem=$(owner jooanipc) || die 'stock application missing or ambiguous'
watch=$(owner my_watch_dog) || die 'stock watchdog missing or ambiguous'
syslog=$(owner syslogd) || die 'stock syslog missing or ambiguous'
holds_watchdog "$watch" || die 'the stock watchdog does not hold /dev/watchdog'
os=$(awk '{print $22}' "/proc/$oem/stat")
ws=$(awk '{print $22}' "/proc/$watch/stat")
ss=$(awk '{print $22}' "/proc/$syslog/stat")
[ ! -d "$platform_apply" ] || die 'a platform update is being installed'
[ ! -e "$wifi_trial" ] || die 'a Wi-Fi change is being tested: keep it or go back first'
[ ! -d "$platform_lock" ] || die 'platform maintenance is in progress'
mkdir -p /run/jooan-local && mkdir "$platform_lock" || die 'cannot take the platform maintenance lock'
log 'preconditions passed; transferring the watchdog and stopping the stock application'

# The hardware timer stays armed when its owner goes, so the feeder follows at once.
phase=stopped
stop_owner "$watch" my_watch_dog "$ws" 5
(trap '' HUP; exec /sbin/watchdog -F -t 2 -T 180 /dev/watchdog) </dev/null >"$state/watchdog.log" 2>&1 &
feeder=$!
sleep 1
kill -0 "$feeder" || abort_reboot 'rootfs watchdog feeder failed'
holds_watchdog "$feeder" || abort_reboot 'rootfs feeder does not own watchdog'
log "rootfs watchdog feeder owns watchdog: $feeder"
stop_owner "$oem" jooanipc "$os" 60
stop_owner "$syslog" syslogd "$ss" 5
taint=$(cat /proc/sys/kernel/tainted)
[ "$((taint & 128))" = 0 ] || abort_reboot 'kernel media fault'
# The stock application's workers can outlast it for a moment.
i=0
until grep -q 'IVDC DDR Addr *: 0x00000000' /proc/jz/isp/isp-ivdc; do
    i=$((i+1))
    [ "$i" -lt 15 ] || abort_reboot 'media DMA did not detach'
    sleep 1
done

cat /dev/mtdblock4 > "$backup"
[ "$(hash "$backup")" = "$2" ] || abort_reboot 'RAM rollback checksum mismatch'
sync
echo 3 > /proc/sys/vm/drop_caches
log 'stock appfs backed up in RAM; checking all task references'
for task in /proc/[0-9]*/task/[0-9]*; do
    if grep -q '1f:04' "$task/maps" 2>/dev/null; then
        abort_reboot "appfs mapping remains: $task"
    fi
done
ls -l /proc/[0-9]*/task/[0-9]*/fd/* /proc/[0-9]*/cwd /proc/[0-9]*/exe /proc/[0-9]*/root > "$state/references" 2>/dev/null || :
if grep -q ' -> /mnt/mtd\(/\|$\)' "$state/references"; then abort_reboot 'appfs descriptor/path remains'; fi
umount /mnt/mtd || abort_reboot 'ordinary appfs unmount refused'

# Commit point: configuration now names the new release and the boot will use it.
log 'appfs unmounted; committing paths and flashing verified image'
phase=commit
mkdir -p "$root/state"
for name in boot-handoff.sh camera.ini deployment.env; do
    mv "$root/$name.next" "$root/$name"
done
chmod 700 "$root" "$root/state" "$root/boot-handoff.sh" "$root/native-boot.sh"
chmod 600 "$root/camera.ini" "$root/deployment.env" "$root/key.pem"
: > "$root/enabled"
: > "$root/native"
chmod 600 "$root/enabled" "$root/native"
sync
phase=flash
if /sbin/flashcp -v "$image" /dev/mtd4 && [ "$(hash /dev/mtd4)" = "$1" ]; then
    log 'flashcp and complete partition readback passed'
else
    log 'flash verification failed; restoring stock appfs and configuration'
    tries=0
    until /sbin/flashcp -v "$backup" /dev/mtd4 && [ "$(hash /dev/mtd4)" = "$2" ]; do
        tries=$((tries + 1))
        # Do not reboot onto a damaged appfs: it holds startapp and the stock boot.
        [ "$tries" -lt 3 ] || die "rollback failed $tries times; camera kept up for manual recovery (docs/RECOVERY.md), images in /tmp"
        log "rollback attempt $tries failed; retrying"
        sleep 2
    done
    restore_config
    log 'stock appfs and configuration restored'
    sync
    /bin/busybox reboot -f
    exit 1
fi
sync
[ "$(df -k /opt | awk 'END {print $4}')" -ge 64 ] || die 'config space low after install; feeder retained'
log 'rebooting into the first media release'
/bin/busybox reboot -f
