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
[ "$JL_PENDING" = - ] || { jl_log 'another trial is already pending'; exit 1; }
if [ "$#" = 2 ]; then
    jl_target=$2
else
    case "$JL_STABLE" in A) jl_target=B ;; *) jl_target=A ;; esac
fi
jl_slot_valid "$jl_target" || exit 2
[ "$jl_target" != "$JL_STABLE" ] || { jl_log 'refusing to replace the stable slot'; exit 1; }
if [ -f "$JL_RUN/running" ]; then
    IFS= read -r jl_running < "$JL_RUN/running" || exit 1
    [ "$jl_target" != "$jl_running" ] || { jl_log 'target slot is still running'; exit 1; }
fi

jl_dest=$JL_ROOT/slots/$jl_target
mkdir -p "$JL_ROOT/slots" "$JL_STATE" "$JL_CONFIG" || exit 1
[ ! -e "$jl_dest.new" ] && [ ! -e "$jl_dest.previous" ] || {
    jl_log 'stale slot staging requires inspection before retry'; exit 1;
}
# The 384 KiB JFFS2 partition fits the controller plus two compressed slots,
# not a third temporary archive. The selected target is proven inactive above;
# discard only that old inactive copy while the stable/running slot remains an
# intact rollback path across power loss.
jl_total=$(jl_tree_bytes "$JL_ROOT")
jl_archive_bytes=$(wc -c < "$jl_stage/runtime.tar.gz")
jl_size=$(( (jl_archive_bytes + 1023) / 1024 ))
jl_old=0
[ ! -d "$jl_dest" ] || jl_old=$(jl_tree_bytes "$jl_dest")
case "$jl_total:$jl_size:$jl_old" in *[!0-9:]*|:*|*:) exit 1 ;; esac
jl_need=$((jl_size + 8))
[ $((jl_total - jl_old + jl_archive_bytes + 4096)) -le 262144 ] || {
    jl_log 'runtime trial would exceed 256 KiB transient tree'; exit 1;
}
if [ -d "$jl_dest" ]; then
    rm -f "$jl_dest/runtime.tar.gz" "$jl_dest/runtime.md5" || exit 1
    rm -rf "$jl_dest" || exit 1
    sync
fi
# A two-slot trial is temporary. Preserve 32 KiB while staging; promotion or
# rollback prunes the superseded slot and restores the 80 KiB steady reserve.
jl_wait_free_kb "$JL_ROOT" $((jl_need + 32)) 20 || {
    jl_log 'compressed trial would violate 32 KiB transient free-space reserve'; exit 1;
}
mkdir "$jl_dest.new" || exit 1
cp "$jl_stage/runtime.tar.gz" "$jl_stage/runtime.md5" "$jl_dest.new/" || exit 1
jl_verify_archive "$jl_dest.new/runtime.tar.gz" "$jl_dest.new/runtime.md5" || exit 1
sync
mv "$jl_dest.new" "$jl_dest" || exit 1
sync
jl_check_transient_storage || exit 1
jl_write_selection "$JL_STABLE" "$jl_target" 0 || exit 1
sync
jl_check_transient_storage || exit 1
jl_unlock
trap - EXIT
jl_log "slot $jl_target staged; trial begins on the next boot"
