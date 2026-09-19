#!/bin/sh
# Shared controller helpers. Selection is data, never sourced shell code.
JL_ROOT=${JL_ROOT:-/opt/custom/jooan-local}
JL_RUN=${JL_RUN:-/run/jooan-local}
JL_STATE=$JL_ROOT/state
JL_CONFIG=$JL_ROOT/config
export JL_ROOT JL_RUN JL_STATE JL_CONFIG
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
umask 077

jl_log() { printf '%s\n' "jooan-local: $*" >&2; }
jl_slot_valid() { case "$1" in A|B) return 0 ;; *) return 1 ;; esac; }

jl_init_run() {
    mkdir -p "$JL_RUN" || return 1
    chmod 700 "$JL_RUN" || return 1
}

jl_lock() {
    jl_init_run || return 1
    jl_lock_tries=0
    while ! mkdir "$JL_RUN/state.lock" 2>/dev/null; do
        jl_lock_tries=$((jl_lock_tries + 1))
        [ "$jl_lock_tries" -lt 10 ] || return 1
        sleep 1
    done
}
jl_unlock() { rm -rf "$JL_RUN/state.lock" 2>/dev/null || :; }

# Exactly three fields: stable slot, trial slot, trial already attempted (0/1).
# A missing record means no runtime has yet been activated.
jl_read_selection() {
    JL_STABLE=- JL_PENDING=- JL_ATTEMPTED=0
    [ -f "$JL_STATE/selection" ] || return 0
    [ "$(wc -l < "$JL_STATE/selection")" = 1 ] || return 1
    IFS=' ' read -r JL_STABLE JL_PENDING JL_ATTEMPTED jl_extra < "$JL_STATE/selection" || return 1
    [ -z "$jl_extra" ] || return 1
    case "$JL_STABLE" in -|A|B) ;; *) return 1 ;; esac
    case "$JL_PENDING" in -|A|B) ;; *) return 1 ;; esac
    case "$JL_ATTEMPTED" in 0|1) ;; *) return 1 ;; esac
    [ "$JL_PENDING" = - ] || [ "$JL_PENDING" != "$JL_STABLE" ] || return 1
    [ "$JL_PENDING" != - ] || [ "$JL_ATTEMPTED" = 0 ]
}

jl_write_selection() {
    mkdir -p "$JL_STATE" || return 1
    printf '%s %s %s\n' "$1" "$2" "$3" > "$JL_STATE/selection.new" || return 1
    chmod 600 "$JL_STATE/selection.new" || return 1
    # Flush the candidate contents before replacing the authoritative record.
    sync
    mv -f "$JL_STATE/selection.new" "$JL_STATE/selection" || return 1
    sync
}

# Hooks must return promptly and must themselves track/reap their own daemons.
# This cap protects boot from a wedged hook; it is not a general process-tree killer.
jl_bounded_hook() {
    jl_limit=$1
    shift
    [ -x "$1" ] || return 1
    "$@" &
    jl_hook_pid=$!
    (
        sleep "$jl_limit"
        kill -TERM "$jl_hook_pid" 2>/dev/null || exit 0
        sleep 1
        kill -KILL "$jl_hook_pid" 2>/dev/null || :
    ) &
    jl_timer_pid=$!
    wait "$jl_hook_pid"
    jl_hook_rc=$?
    kill "$jl_timer_pid" 2>/dev/null || :
    wait "$jl_timer_pid" 2>/dev/null || :
    return "$jl_hook_rc"
}

jl_set_running() {
    printf '%s\n' "$1" > "$JL_RUN/running.new" &&
        mv -f "$JL_RUN/running.new" "$JL_RUN/running"
}

jl_verify_archive() {
    jl_archive=$1
    jl_digestfile=$2
    [ -f "$jl_archive" ] && [ -f "$jl_digestfile" ] || return 1
    jl_expected=$(cat "$jl_digestfile") || return 1
    [ "${#jl_expected}" = 64 ] || return 1
    case "$jl_expected" in *[!0-9a-f]*) return 1 ;; esac
    jl_verifier=${JOOAN_SHA256:-$JL_ROOT/shared/jooan-sha256}
    [ -x "$jl_verifier" ] || return 1
    jl_actual=$("$jl_verifier" "$jl_archive") || return 1
    # Hash-only output is canonical; tolerate conventional '<hash>  <path>'.
    jl_actual=${jl_actual%% *}
    [ "$jl_actual" = "$jl_expected" ]
}

jl_unpack_archive() {
    jl_archive=$1
    jl_destination=$2
    jl_listing=$JL_RUN/archive-list.$$
    # No path traversal, symlink, device, socket, or hardlink entries.
    tar -tzf "$jl_archive" > "$jl_listing" || return 1
    while IFS= read -r jl_entry; do
        case "$jl_entry" in /*|../*|*/../*|*/..|..) return 1 ;; esac
    done < "$jl_listing"
    tar -tvzf "$jl_archive" > "$jl_listing" || return 1
    awk 'substr($0,1,1)!="d" && substr($0,1,1)!="-" {bad=1} END {exit bad}' "$jl_listing" || return 1
    rm -f "$jl_listing"
    [ ! -e "$jl_destination" ] || return 1
    mkdir "$jl_destination" || return 1
    tar -xzf "$jl_archive" -C "$jl_destination" || return 1
}

jl_start_slot() {
    JL_SLOT_STARTED=0
    jl_slot_valid "$1" || return 1
    JL_SLOT=$1
    JL_SLOT_DIR=$JL_RUN/slot-$JL_SLOT
    export JL_SLOT JL_SLOT_DIR
    jl_verify_archive "$JL_ROOT/slots/$JL_SLOT/runtime.tar.gz" "$JL_ROOT/slots/$JL_SLOT/runtime.sha256" || return 1
    if [ ! -d "$JL_SLOT_DIR" ]; then
        jl_unpack_archive "$JL_ROOT/slots/$JL_SLOT/runtime.tar.gz" "$JL_SLOT_DIR" || return 1
    fi
    [ -x "$JL_SLOT_DIR/start.sh" ] && [ -x "$JL_SLOT_DIR/stop.sh" ] &&
        [ -x "$JL_SLOT_DIR/health.sh" ] || return 1
    jl_set_running "$JL_SLOT" || return 1
    JL_SLOT_STARTED=1
    jl_bounded_hook 15 "$JL_SLOT_DIR/start.sh"
}

jl_stop_slot() {
    jl_slot_valid "$1" || return 1
    JL_SLOT=$1 JL_SLOT_DIR=$JL_RUN/slot-$1
    export JL_SLOT JL_SLOT_DIR
    jl_bounded_hook 10 "$JL_SLOT_DIR/stop.sh"
}
