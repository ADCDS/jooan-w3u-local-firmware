#!/bin/sh
# The authenticated installer extracts controller.tar.gz into tmpfs and sets
# JL_CONTROL to that tree before invoking this helper. Persistent bundle files:
# local.rc and one atomically replaceable controller.tar.gz. OTA signature
# authenticates it; gzip/tar CRC detects stored corruption without a sidecar.
# prepare/validate <bundle-dir> stages a first install/controller refresh.
# activate publishes the minimal hook only after a runtime slot is installed.
. "${JL_CONTROL:-/run/jooan-local/controller}/boot/common.sh" || exit 1
JL_ACTIVATE=${JL_ACTIVATE:-/opt/etc/local.rc}
[ "$(id -u)" = 0 ] || exit 1
[ "$#" -ge 1 ] || exit 2
jl_action=$1
case "$jl_action" in
    prepare|validate)
        [ "$#" = 2 ] || exit 2
        jl_source=$2
        case "$jl_source" in /*) ;; *) exit 2 ;; esac
        for jl_file in local.rc controller.tar.gz; do
            [ -f "$jl_source/$jl_file" ] && [ ! -L "$jl_source/$jl_file" ] || exit 1
        done
        tar -tzf "$jl_source/controller.tar.gz" >/dev/null || exit 1
        sh -n "$jl_source/local.rc" || exit 1
        mkdir -p "$JL_ROOT" "$JL_STATE" "$JL_CONFIG/ssh" "$JL_ROOT/slots" || exit 1
        chmod 700 "$JL_ROOT" "$JL_STATE" "$JL_CONFIG" "$JL_CONFIG/ssh" || exit 1
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        if [ "$jl_action" = prepare ]; then
            [ ! -e "$JL_ROOT/controller.tar.gz" ] || exit 1
        fi
        # Expanded installations need an explicit backed-up migration. Never
        # erase their controller underneath a running session as a side effect.
        for jl_dir in boot admin shared; do
            [ ! -e "$JL_ROOT/$jl_dir" ] || {
                jl_log 'expanded controller requires staged migration before compressed installation'; exit 1;
            }
        done
        jl_total=$(jl_tree_bytes "$JL_ROOT") || exit 1
        jl_incoming=0 jl_replaced=0 jl_peak=0
        for jl_file in local.rc controller.tar.gz; do
            jl_bytes=$(wc -c < "$jl_source/$jl_file") || exit 1
            jl_incoming=$((jl_incoming + jl_bytes))
            [ "$jl_bytes" -le "$jl_peak" ] || jl_peak=$jl_bytes
            if [ -f "$JL_ROOT/$jl_file" ]; then
                jl_bytes=$(wc -c < "$JL_ROOT/$jl_file") || exit 1
                jl_replaced=$((jl_replaced + jl_bytes))
            fi
        done
        [ $((jl_total - jl_replaced + jl_incoming + 4096)) -le 180224 ] || {
            jl_log 'compressed controller would exceed 176 KiB persistent-file budget'; exit 1;
        }
        # The archive is one atomic object: a power interruption leaves the old
        # or new complete gzip, never a mismatched archive/sidecar pair.
        jl_wait_free_kb "$JL_ROOT" $(( (jl_peak + 1023) / 1024 + 56 )) 20 || exit 1
        for jl_file in controller.tar.gz local.rc; do
            [ ! -L "$JL_ROOT/$jl_file.new" ] || exit 1
            cp "$jl_source/$jl_file" "$JL_ROOT/$jl_file.new" || exit 1
            case "$jl_file" in local.rc) chmod 755 "$JL_ROOT/$jl_file.new" ;; *) chmod 600 "$JL_ROOT/$jl_file.new" ;; esac
            sync
            mv -f "$JL_ROOT/$jl_file.new" "$JL_ROOT/$jl_file" || exit 1
        done
        tar -tzf "$JL_ROOT/controller.tar.gz" >/dev/null || exit 1
        printf '%s\n' 1 > "$JL_STATE/controller.ready.new" || exit 1
        mv -f "$JL_STATE/controller.ready.new" "$JL_STATE/controller.ready" || exit 1
        sync
        jl_check_current_storage || exit 1
        jl_unlock
        trap - EXIT
        jl_log 'compressed controller prepared; activate after installing the runtime'
        ;;
    activate)
        [ "$#" = 1 ] || exit 2
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        jl_read_selection || exit 1
        jl_candidate=$JL_STABLE
        [ "$JL_PENDING" = - ] || jl_candidate=$JL_PENDING
        jl_slot_valid "$jl_candidate" || exit 1
        tar -tzf "$JL_ROOT/controller.tar.gz" >/dev/null || exit 1
        jl_verify_archive "$JL_ROOT/recovery/dropbear.tar.gz" \
            "$JL_ROOT/recovery/dropbear.md5" || exit 1
        jl_verify_archive "$JL_ROOT/slots/$jl_candidate/runtime.tar.gz" "$JL_ROOT/slots/$jl_candidate/runtime.md5" || exit 1
        [ -f "$JL_ROOT/local.rc" ] && [ ! -L "$JL_ACTIVATE" ] || exit 1
        if [ -f "$JL_ACTIVATE" ] && [ ! -e "$JL_STATE/prelocal-hook.disabled" ]; then
            cp "$JL_ACTIVATE" "$JL_STATE/prelocal-hook.disabled" || exit 1
            chmod 600 "$JL_STATE/prelocal-hook.disabled" || exit 1
        fi
        jl_check_current_storage || exit 1
        jl_hook_bytes=$(wc -c < "$JL_ROOT/local.rc") || exit 1
        jl_wait_free_kb "$JL_ROOT" $(( (jl_hook_bytes + 1023) / 1024 + 56 )) 20 || exit 1
        mkdir -p "${JL_ACTIVATE%/*}" || exit 1
        cp "$JL_ROOT/local.rc" "$JL_ACTIVATE.new" || exit 1
        chmod 755 "$JL_ACTIVATE.new" || exit 1
        sync
        mv -f "$JL_ACTIVATE.new" "$JL_ACTIVATE" || exit 1
        sync
        jl_check_current_storage || exit 1
        killall telnetd goahead 2>/dev/null || :
        jl_unlock
        trap - EXIT
        jl_log 'compressed-controller boot hook activated; inherited hooks remain disabled'
        ;;
    *) exit 2 ;;
esac
