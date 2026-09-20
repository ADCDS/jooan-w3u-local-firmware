#!/bin/sh
# Signed destructive removal. This is intentionally not a state rollback.
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
self=${0%/*}
root=/opt/custom/jooan-local
hook=/opt/etc/local.rc
run=/run/jooan-local
sha=$self/jooan-sha256
auth=$self/jooan-auth-verify
manifest=$self/release.manifest

die() { echo "jooan-local uninstall: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die 'not root'
[ "$(cat /etc/deviceModel 2>/dev/null)" = JA-A12 ] || die 'unsupported model'
[ -x "$sha" ] && [ -x "$auth" ] || die 'release verifiers missing'
"$auth" "$manifest" "$self/release.manifest.sig" "$self" || die 'signed release inventory rejected'
grep -qx 'target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316' "$manifest" || die 'wrong target'
grep -qx 'artifact_kind=uninstall' "$manifest" || die 'not an uninstall artifact'
grep -qx 'artifact_semantics=destructive-removal-to-oem-next-boot-not-state-rollback' \
    "$manifest" || die 'uninstall semantics mismatch'

# Stop the active tmpfs slot and supervisors before removing their persistent
# archives. Validate PIDs/comm names so unrelated processes are never signaled.
if [ -f "$run/running" ]; then
    slot=$(cat "$run/running" 2>/dev/null || :)
    case "$slot" in A|B)
        [ ! -x "$run/slot-$slot/stop.sh" ] || \
            JL_ROOT=$root JL_RUN=$run JL_SLOT=$slot JL_SLOT_DIR=$run/slot-$slot \
                "$run/slot-$slot/stop.sh" || :
        ;; esac
fi
for record in "$run/dropbear.pid:dropbear" "$run/supervisor.pid:sh"; do
    pidfile=${record%%:*} expected=${record#*:}
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null || :)
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$expected" ] || continue
    kill -TERM "$pid" 2>/dev/null || :
done
killall joan-daemon audio-router dropbear telnetd 2>/dev/null || :

if [ -f "$hook" ] && grep -q '/opt/custom/jooan-local' "$hook"; then
    rm -f "$hook"
fi
# Do not delete $run here: direct apply executes this script from a read-only
# mount beneath that tmpfs tree. The apply runner unmounts it and reboot clears
# tmpfs after this script returns.
rm -rf "$root" /opt/etc/jooan-ssh
sync
echo 'jooan-local removed; this was uninstall, not restoration of predecessor state'
exit 0
