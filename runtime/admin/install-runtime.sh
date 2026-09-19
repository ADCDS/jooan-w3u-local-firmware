#!/bin/sh
# Locally staged archive only; no downloader, OTA exploit, or shared SSH update.
# Staging directory: runtime.tar.gz, runtime.sha256 (one lowercase SHA-256).
# Caller establishes manifest authenticity before invoking this integrity check.
. "${JL_ROOT:-/opt/custom/jooan-local}/boot/common.sh" || exit 1
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || exit 2
jl_stage=$1
case "$jl_stage" in /*) ;; *) jl_log 'staging path must be absolute'; exit 2 ;; esac
jl_verify_archive "$jl_stage/runtime.tar.gz" "$jl_stage/runtime.sha256" || exit 1
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
if [ -d "$jl_dest" ]; then
    rm -f "$jl_dest/runtime.tar.gz" "$jl_dest/runtime.sha256" || exit 1
    rm -rf "$jl_dest" || exit 1
    sync
fi
# Reserve >=64 KiB while staging beside the stable slot.
jl_size=$(du -k "$jl_stage/runtime.tar.gz" | awk '{print $1}')
case "$jl_size" in ''|*[!0-9]*) exit 1 ;; esac
jl_need=$((jl_size + 8))
jl_wait_free_kb "$JL_ROOT" $((jl_need + 64)) 20 || {
    jl_log 'compressed update would violate 64 KiB free-space reserve'; exit 1;
}
mkdir "$jl_dest.new" || exit 1
cp "$jl_stage/runtime.tar.gz" "$jl_stage/runtime.sha256" "$jl_dest.new/" || exit 1
jl_verify_archive "$jl_dest.new/runtime.tar.gz" "$jl_dest.new/runtime.sha256" || exit 1
sync
mv "$jl_dest.new" "$jl_dest" || exit 1
sync
jl_wait_free_kb "$JL_ROOT" 64 20 || exit 1
jl_write_selection "$JL_STABLE" "$jl_target" 0 || exit 1
sync
jl_unlock
trap - EXIT
jl_log "slot $jl_target staged; trial begins on the next boot"
