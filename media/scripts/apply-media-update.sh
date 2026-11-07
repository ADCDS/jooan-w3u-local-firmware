#!/bin/sh
# SPDX-License-Identifier: MIT
# Installs a media package that the daemon has already verified, from the directory it
# was unpacked in. Usage: apply-media-update.sh check|apply
# check says why the package cannot go on this camera, changing nothing. apply stages
# what the boot checks against, then runs upgrade-appfs.sh, which flashes and restarts.
# The daemon starts this with a clean environment, so JIPC_* are for the tests.
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
unset LD_LIBRARY_PATH LD_PRELOAD
umask 077
trap '' HUP

here=${0%/*} # the camera's busybox has no dirname
root=${JIPC_ROOT:-/opt/jooanipc-open}
appfs=${JIPC_APPFS:-/dev/mtdblock4}
candidate=${JIPC_CANDIDATE:-/tmp/jipc-flash-candidate.squashfs}
run=${JIPC_RUN:-/run/jooanipc-open}
platform=${JIPC_PLATFORM:-/opt/custom/jooan-local}
platform_run=${JIPC_PLATFORM_RUN:-/run/jooan-local}

stop() { echo "$*"; exit 1; }
field() { sed -n "/^files-begin\$/q; s/^$1=//p" "$here/release.manifest"; }
md5() { md5sum "$1" | cut -c1-32; }

release=$(field release_version)
previous=$(field previous_appfs_md5)
image=$(field appfs_md5)
app=$(field app_md5)
imp=$(field imp_md5)
imp_sha256=$(field imp_sha256)
atomic=$(field atomic_md5)
case "$release" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
    *) stop 'The package names no valid release' ;;
esac
for value in "$previous" "$image" "$app" "$imp" "$atomic"; do
    [ "${#value}" = 32 ] || stop 'The package is missing a checksum'
done
[ "${#imp_sha256}" = 64 ] || stop 'The package is missing a checksum'

env=$root/deployment.env
ini=$root/camera.ini
handoff=$root/boot-handoff.sh
boot=$root/native-boot.sh
old=$(sed -n 's/^JIPC_RELEASE=//p' "$env")
[ -n "$old" ] || stop 'The camera has no media release to update'
[ "${old##*/}" != "$release" ] || stop "Release $release is already installed"
[ "$(md5 "$appfs")" = "$previous" ] ||
    stop 'This package was built from a different copy of this camera’s app partition'
[ -s "$run/qualified" ] || stop 'The camera is still checking its last start. Try again in about 4 minutes'
[ ! -e "$platform/state/wifi-trial" ] || stop 'A new Wi-Fi network or address is being tested. Keep it or go back first'
[ ! -d "$platform_run/firmware-apply.lock" ] || stop 'A platform update is being installed. Try again when it has finished'
[ ! -e "$root/boot-trial" ] || stop 'The camera is recovering from a failed start: fix that first'
[ ! -e "$root/native-failed" ] || stop 'The camera is recovering from a failed start: fix that first'
[ "$(md5 "$here/appfs.img")" = "$image" ] || stop 'The image does not match its checksum'

[ "${1:-}" = apply ] || exit 0

sleep 1 # lets the daemon finish answering the request that started this
new=${old%/*}/$release
tidy() { rm -f "$env.next" "$ini.next" "$handoff.next" "$boot.next"; [ ! -e "$candidate" ] || mv "$candidate" "$here/appfs.img"; sync; }
sed -e "s#^JIPC_RELEASE=.*#JIPC_RELEASE=$new#" \
    -e "s#^JIPC_APP_MD5=.*#JIPC_APP_MD5=$app#" \
    -e "s#^JIPC_IMP_MD5=.*#JIPC_IMP_MD5=$imp#" \
    -e "s#^JIPC_ATOMIC_MD5=.*#JIPC_ATOMIC_MD5=$atomic#" \
    -e "s#^JIPC_LIBIMP_SHA256=.*#JIPC_LIBIMP_SHA256=$imp_sha256#" "$env" > "$env.next"
sed -e "s#$old/#$new/#g" "$ini" > "$ini.next"
cp "$handoff" "$handoff.next"
cp "$here/native-boot.sh" "$boot.next"
chmod 700 "$handoff.next" "$boot.next"
chmod 600 "$env.next" "$ini.next"
mv "$here/appfs.img" "$candidate"
sync
sh "$here/upgrade-appfs.sh" "$image" "$previous" || { status=$?; tidy; exit "$status"; }
