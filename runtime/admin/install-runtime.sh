#!/bin/sh
# Locally staged archive only; no downloader, OTA exploit, or shared SSH update.
# Staging directory: runtime.tar.gz, runtime.md5 (one lowercase MD5 digest).
# OTA installer establishes SHA/signature authenticity in tmpfs; MD5 checks only
# corruption during storage. It does not authorize unauthenticated updates.
. "${JL_CONTROL:-/run/jooan-local/controller}/boot/common.sh" || exit 1
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || exit 2
jl_stage=$1
case "$jl_stage" in /*) ;; *) jl_log 'staging path must be absolute'; exit 2 ;; esac
jl_verify_archive "$jl_stage/runtime.tar.gz" "$jl_stage/runtime.md5" || exit 1
jl_lock || exit 1
trap 'jl_unlock' EXIT
jl_read_selection || exit 1
[ ! -e "$JL_STATE/wifi-trial" ] || { jl_log 'finish the Wi-Fi transaction before updating'; exit 1; }
if [ "$JL_PENDING" != - ]; then
    case "$JL_PENDING:$JL_ATTEMPTED" in A:0|A:1|B:0|B:1) ;; *) exit 1 ;; esac
    jl_existing=$JL_ROOT/slots/$JL_PENDING
    [ -f "$jl_existing/runtime.tar.gz" ] && [ -f "$jl_existing/runtime.md5" ] || exit 1
    jl_stage_md5=$(cat "$jl_stage/runtime.md5") || exit 1
    jl_existing_md5=$(cat "$jl_existing/runtime.md5") || exit 1
    [ "$jl_existing_md5" = "$jl_stage_md5" ] || exit 1
    jl_verify_archive "$jl_existing/runtime.tar.gz" "$jl_existing/runtime.md5" || exit 1
    [ -x "${JOOAN_SHA256:-}" ] || exit 1
    jl_existing_sha=$("$JOOAN_SHA256" "$jl_existing/runtime.tar.gz") || exit 1
    jl_stage_sha=$("$JOOAN_SHA256" "$jl_stage/runtime.tar.gz") || exit 1
    [ "${jl_existing_sha%% *}" = "${jl_stage_sha%% *}" ] || exit 1
    jl_unlock
    trap - EXIT
    jl_log "pending runtime $JL_PENDING already matches authenticated stage"
    exit 0
fi
if [ "$#" = 2 ]; then
    jl_target=$2
else
    case "$JL_STABLE" in A) jl_target=B ;; *) jl_target=A ;; esac
fi
jl_slot_valid "$jl_target" || exit 2
jl_dest=$JL_ROOT/slots/$jl_target
mkdir -p "$JL_ROOT/slots" "$JL_STATE" "$JL_CONFIG" || exit 1
# Enter controller-owned recovery before deleting the one persistent runtime.
# The old processes already expanded in /run continue until reboot.
jl_write_selection - - 0 || exit 1
[ "${JOOAN_FAIL_AT:-}" != after-recovery-selection ] || exit 99
rm -rf "$JL_ROOT/slots/A" "$JL_ROOT/slots/B" \
    "$JL_ROOT/slots/A.new" "$JL_ROOT/slots/B.new" \
    "$JL_ROOT/slots/A.previous" "$JL_ROOT/slots/B.previous" || exit 1
sync
[ "${JOOAN_FAIL_AT:-}" != after-runtime-delete ] || exit 99
jl_wait_free_kb "$JL_ROOT" 56 20 || exit 1

jl_archive_bytes=$(wc -c < "$jl_stage/runtime.tar.gz")
jl_size=$(( (jl_archive_bytes + 1023) / 1024 ))
case "$jl_archive_bytes:$jl_size" in *[!0-9:]*|:*|*:) exit 1 ;; esac
jl_need=$((jl_size + 8))
[ $(( $(jl_tree_bytes "$JL_ROOT") + jl_archive_bytes + 4096)) -le 184320 ] || {
    jl_log 'runtime maintenance would exceed 180 KiB persistent tree'; exit 1;
}
# Runtime replacement happens only after the old persistent runtime is removed.
# Preserve 56 KiB through maintenance; health promotion restores the 76 KiB gate.
jl_wait_free_kb "$JL_ROOT" $((jl_need + 56)) 20 || {
    jl_log 'runtime maintenance would violate 56 KiB free-space reserve'; exit 1;
}
mkdir "$jl_dest.new" || exit 1
cp "$jl_stage/runtime.tar.gz" "$jl_stage/runtime.md5" "$jl_dest.new/" || exit 1
jl_verify_archive "$jl_dest.new/runtime.tar.gz" "$jl_dest.new/runtime.md5" || exit 1
sync
mv "$jl_dest.new" "$jl_dest" || exit 1
sync
[ "${JOOAN_FAIL_AT:-}" != after-runtime-dir-rename ] || exit 99
jl_check_maintenance_storage || exit 1
jl_write_selection - "$jl_target" 0 || exit 1
sync
[ "${JOOAN_FAIL_AT:-}" != after-pending-selection-rename ] || exit 99
jl_check_maintenance_storage || exit 1
jl_unlock
trap - EXIT
jl_log "single runtime $jl_target staged; health trial begins on the next boot"
