#!/bin/sh
. "${JL_CONTROL:-/run/jooan-local/controller}/boot/common.sh" || exit 1
[ "$#" = 1 ] && jl_slot_valid "$1" || exit 2
jl_lock || exit 1
trap 'jl_unlock' EXIT
jl_read_selection || exit 1
[ "$JL_PENDING" = "$1" ] && [ "$JL_ATTEMPTED" = 1 ] || exit 1
IFS= read -r jl_running < "$JL_RUN/running" || exit 1
[ "$jl_running" = "$1" ] || exit 1
JL_SLOT=$1 JL_SLOT_DIR=$JL_RUN/slot-$1
export JL_SLOT JL_SLOT_DIR
jl_bounded_hook 5 "$JL_SLOT_DIR/health.sh" || exit 1
jl_write_selection "$1" - 0 || exit 1
jl_prune_other_slots "$1" ||
    jl_log 'runtime promoted; superseded-slot cleanup/final reserve will retry at boot'
jl_unlock
trap - EXIT
jl_log "runtime slot $1 committed"
exit 0
