#!/bin/sh
# SPDX-License-Identifier: MIT
# JA-A12 native->native appfs upgrade, run on the camera by install-camera.py.
# The running media owner is the native jooanipc-open, which also owns the
# armed hardware watchdog (180 s); a rootfs feeder takes it over for the flash.
# Usage: upgrade-appfs.sh NEW_IMAGE_MD5 CURRENT_APPFS_MD5
# All executables used here come from rootfs.
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
# The platform supervisor's maintenance lock. While it exists the supervisor
# neither restarts its runtime nor lets the network watchdog reboot; holding it
# from here on keeps a reboot from landing on a half-written or failed appfs.
platform_lock=/run/jooan-local/state.lock
mkdir -p "$state"
log() { printf '%s %s\n' "$(cat /proc/uptime)" "$*"; }
die() { log "ABORT: $*"; exit 1; }
# After media stops and before the flash commit point, the old appfs and
# configuration are untouched: rebooting restores the qualified native boot.
abort_reboot() { log "ABORT before flash: $*; rebooting into the unchanged appfs"; sync; /bin/busybox reboot -f; exit 1; }
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
    # Numeric procfs descriptors on this BusyBox build; no readlink applet exists.
    # shellcheck disable=SC2010
    ls -l "/proc/$1/fd" 2>/dev/null | grep -q ' -> /dev/watchdog$'
}
restore_config() {
    for name in boot-handoff.sh camera.ini deployment.env; do
        cp "$state/$name.before" "$root/$name"
    done
    chmod 700 "$root/boot-handoff.sh"
    chmod 600 "$root/camera.ini" "$root/deployment.env"
    sync
}

# Preconditions: nothing below changes the camera until all of these pass.
[ "$#" = 2 ] || die 'usage: upgrade-appfs.sh NEW_IMAGE_MD5 CURRENT_APPFS_MD5'
[ "$(cat /sys/class/mtd/mtd4/size)" = 3211264 ] || die 'wrong partition size'
[ "$(cat /sys/class/mtd/mtd4/name)" = appfs ] || die 'wrong partition name'
flags=$(cat /sys/class/mtd/mtd4/flags)
[ "$((flags & 1024))" -ne 0 ] || die 'appfs is not writable through MTD'
[ "$(wc -c < "$image")" = 3211264 ] || die 'wrong image size'
[ "$(hash "$image")" = "$1" ] || die 'image checksum mismatch'
[ "$(hash /dev/mtdblock4)" = "$2" ] || die 'original appfs changed'
grep -q '^/dev/mtdblock4 /mnt/mtd squashfs ro[, ]' /proc/mounts || die 'unexpected appfs mount'
[ ! -e "$root/boot-trial" ] || die 'existing failed native boot trial'
[ "$(df -k /opt | awk 'END {print $4}')" -ge 64 ] || die 'insufficient config space'
for name in boot-handoff.sh camera.ini deployment.env; do
    [ -s "$root/$name.next" ] || die "missing staged $name"
    cp "$root/$name" "$state/$name.before"
done
sh -n "$root/boot-handoff.sh.next"
absent jooanipc || die 'OEM media owner is running; this installer upgrades a qualified native boot only'
absent my_watch_dog || die 'OEM watchdog is running; this installer upgrades a qualified native boot only'
native=$(owner jooanipc-open) || die 'native owner missing or ambiguous'
syslog=$(owner syslogd) || die 'OEM syslog missing or ambiguous'
holds_watchdog "$native" || die 'native owner does not hold /dev/watchdog'
[ ! -d "$platform_lock" ] || die 'platform maintenance is in progress'
mkdir -p /run/jooan-local && mkdir "$platform_lock" || die 'cannot take the platform maintenance lock'
ns=$(awk '{print $22}' "/proc/$native/stat")
ss=$(awk '{print $22}' "/proc/$syslog/stat")
log 'preconditions passed; stopping native media and transferring watchdog'

# The native closes /dev/watchdog without the magic close, so the hardware
# timer stays armed with up to 180 s left; the rootfs feeder takes it over.
stop_owner "$native" jooanipc-open "$ns" 30
(trap '' HUP; exec /sbin/watchdog -F -t 2 -T 180 /dev/watchdog) </dev/null >"$state/watchdog.log" 2>&1 &
feeder=$!
sleep 1
kill -0 "$feeder" || abort_reboot 'rootfs watchdog feeder failed'
holds_watchdog "$feeder" || abort_reboot 'rootfs feeder does not own watchdog'
log "rootfs watchdog feeder owns watchdog: $feeder"
stop_owner "$syslog" syslogd "$ss" 5
taint=$(cat /proc/sys/kernel/tainted)
[ "$((taint & 128))" = 0 ] || abort_reboot 'kernel media fault'
grep -q 'IVDC DDR Addr *: 0x00000000' /proc/jz/isp/isp-ivdc || abort_reboot 'media DMA did not detach'

# Save another recoverable copy in RAM, now that media has released memory.
cat /dev/mtdblock4 > "$backup"
[ "$(hash "$backup")" = "$2" ] || abort_reboot 'RAM rollback checksum mismatch'
sync
echo 3 > /proc/sys/vm/drop_caches
log 'old appfs backed up in RAM; checking all task references'
for task in /proc/[0-9]*/task/[0-9]*; do
    if grep -q '1f:04' "$task/maps" 2>/dev/null; then
        abort_reboot "appfs mapping remains: $task"
    fi
done
ls -l /proc/[0-9]*/task/[0-9]*/fd/* /proc/[0-9]*/cwd /proc/[0-9]*/exe /proc/[0-9]*/root > "$state/references" 2>/dev/null || :
if grep -q ' -> /mnt/mtd\(/\|$\)' "$state/references"; then abort_reboot 'appfs descriptor/path remains'; fi
umount /mnt/mtd || abort_reboot 'ordinary appfs unmount refused'

# Commit point: configuration now names the new release.
log 'appfs unmounted; committing paths and flashing verified image'
for name in boot-handoff.sh camera.ini deployment.env; do
    mv "$root/$name.next" "$root/$name"
done
chmod 700 "$root/boot-handoff.sh"
chmod 600 "$root/camera.ini" "$root/deployment.env"
sync
if /sbin/flashcp -v "$image" /dev/mtd4 && [ "$(hash /dev/mtd4)" = "$1" ]; then
    log 'flashcp and complete partition readback passed'
else
    log 'flash verification failed; restoring original appfs and configuration'
    tries=0
    until /sbin/flashcp -v "$backup" /dev/mtd4 && [ "$(hash /dev/mtd4)" = "$2" ]; do
        tries=$((tries + 1))
        # Do not reboot onto a damaged appfs: it holds startapp and the OEM
        # boot. The feeder and the maintenance lock keep the camera up with
        # both images still in /tmp; docs/RECOVERY.md has the manual steps.
        [ "$tries" -lt 3 ] || die "rollback failed $tries times; camera kept up for manual recovery (docs/RECOVERY.md), images in /tmp"
        log "rollback attempt $tries failed; retrying"
        sleep 2
    done
    restore_config
    log 'original appfs and configuration restored'
    sync
    /bin/busybox reboot -f
    exit 1
fi
sync
[ "$(df -k /opt | awk 'END {print $4}')" -ge 64 ] || die 'config space low after install; feeder retained'
log 'rebooting into internal-flash native release'
/bin/busybox reboot -f
