#!/bin/sh
# IronMan executes this as uid 0 from its mounted SquashFS payload.
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/mtd/run
export PATH
self=${0%/*}
root=${JOOAN_ROOT:-/opt/custom/jooan-local}
activate=${JOOAN_ACTIVATE:-/opt/etc/local.rc}
run=${JOOAN_RUN:-/run/jooan-local}
verify=$self/jooan-sha256

die() { echo "jooan-local install: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die 'not root'
[ "$(cat /etc/deviceModel 2>/dev/null)" = JA-A12 ] || die 'unsupported model'
[ -x "$verify" ] || die 'SHA-256 verifier missing'

while read -r expected file; do
    [ -n "$expected" ] || continue
    actual=$($verify "$self/$file" 2>/dev/null | awk '{print $1}')
    [ "$actual" = "$expected" ] || die "payload verification failed: $file"
done < "$self/payload.sha256"

while read -r expected file; do
    [ -n "$expected" ] || continue
    [ -e "$file" ] || die "compatible component unavailable: $file"
    actual=$($verify "$file" 2>/dev/null | awk '{print $1}')
    [ "$actual" = "$expected" ] || die "incompatible camera component: $file"
done < "$self/compatibility.sha256"

if [ "${JOOAN_PREFLIGHT_ONLY:-0}" = 1 ]; then
    echo 'jooan-local preflight passed'
    exit 0
fi

controller=$self/controller
[ -d "$controller" ] || die 'controller tree missing'

if [ ! -f "$root/state/controller.ready" ]; then
    migration=$root/state/key-migration
    if [ "$root" = /opt/custom/jooan-local ] &&
       [ -f /opt/open/admin/manifest.md5 ] && [ -f /opt/open/current ] &&
       [ -s /opt/open/admin/ssh/authorized_keys ] &&
       [ -s /opt/open/admin/ssh/host_ed25519 ]; then
        mkdir -p "$root/state" "$migration.new"
        cp /opt/open/admin/ssh/authorized_keys "$migration.new/authorized_keys"
        cp /opt/open/admin/ssh/host_ed25519 "$migration.new/host_ed25519"
        chmod 600 "$migration.new/"*
        sync
        mv "$migration.new" "$migration"
        sync
        # This exact tree belongs to the predecessor project and otherwise
        # consumes most of the 384 KiB partition. The only irreplaceable
        # inputs are now on the same persistent filesystem, not in tmpfs.
        rm -rf /opt/open
        sync
    fi
    if [ ! -f "$root/boot/common.sh" ]; then
        JL_ROOT=$root JL_RUN=$run JOOAN_SHA256=$verify \
            "$controller/admin/install-controller.sh" prepare "$controller" ||
            die 'controller preparation failed'
    else
        JL_ROOT=$root JL_RUN=$run JOOAN_SHA256=$verify \
            "$controller/admin/install-controller.sh" validate "$controller" ||
            die 'partial controller validation failed'
    fi
fi

migration=$root/state/key-migration
if [ -d "$migration" ]; then
    mkdir -p "$root/config/ssh"
    cp "$migration/authorized_keys" "$root/config/ssh/authorized_keys"
    cp "$migration/host_ed25519" "$root/config/ssh/dropbear_ed25519_host_key"
    chmod 600 "$root/config/ssh/"*
    sync
    rm -rf "$migration"
    sync
fi

JL_ROOT=$root JL_RUN=$run JOOAN_SHA256=$verify "$root/admin/install-runtime.sh" "$self" ||
    die 'runtime staging failed'
JL_ROOT=$root JL_RUN=$run JL_ACTIVATE=$activate "$root/admin/install-controller.sh" activate || die 'activation failed'
killall telnetd 2>/dev/null || :
sync
echo 'jooan-local install complete; rebooting through OEM updater'
exit 0
