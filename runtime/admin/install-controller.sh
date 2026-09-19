#!/bin/sh
# Bootstrap controller preparation is separate from activation. Package code must
# authenticate this entire tree before invoking; no network acquisition occurs.
# Usage: install-controller.sh prepare /absolute/controller-tree
#        /opt/custom/jooan-local/admin/install-controller.sh activate
JL_ROOT=${JL_ROOT:-/opt/custom/jooan-local}
JL_RUN=${JL_RUN:-/run/jooan-local}
JL_ACTIVATE=${JL_ACTIVATE:-/opt/etc/local.rc}
export JL_ROOT JL_RUN
umask 077
[ "$(id -u)" = 0 ] || exit 1
[ "$#" -ge 1 ] || exit 2
jl_action=$1
case "$jl_action" in
    prepare)
        [ "$#" = 2 ] || exit 2
        jl_source=$2
        case "$jl_source" in /*) ;; *) exit 2 ;; esac
        [ -f "$jl_source/boot/common.sh" ] || exit 1
        . "$jl_source/boot/common.sh" || exit 1
        # The controller comes from the already hash-verified read-only OTA
        # SquashFS. BusyBox find on this firmware has no `-type` predicate.
        for jl_file in boot/local.rc boot/boot.sh boot/common.sh \
            admin/install-controller.sh admin/install-runtime.sh \
            admin/mark-healthy.sh admin/ssh-start.sh admin/wifi-transaction.sh \
            shared/jooan-sha256 shared/dropbear.tar.gz shared/dropbear.sha256 \
            shared/libjooan_guard.so shared/guard.sha256 shared/verify-guard.sh; do
            [ -f "$jl_source/$jl_file" ] || exit 1
        done
        JOOAN_SHA256=$jl_source/shared/jooan-sha256
        export JOOAN_SHA256
        jl_verify_archive "$jl_source/shared/dropbear.tar.gz" "$jl_source/shared/dropbear.sha256" || exit 1
        jl_verify_archive "$jl_source/shared/libjooan_guard.so" "$jl_source/shared/guard.sha256" || exit 1
        mkdir -p "$JL_ROOT" || exit 1
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        # Controller/shared-SSH upgrades need their own migration contract.
        # Normal A/B runtime updates never silently replace either component.
        for jl_dir in boot admin shared; do
            [ ! -e "$JL_ROOT/$jl_dir" ] || exit 1
        done
        jl_free=$(df -k "$JL_ROOT" | awk 'END {print $4}')
        case "$jl_free" in ''|*[!0-9]*) exit 1 ;; esac
        # JFFS2 compresses scripts and ELF sections, so apparent source bytes
        # are not a storage estimate. Require measured free space before and
        # after copying instead.
        [ "$jl_free" -ge 80 ] || exit 1
        for jl_dir in boot admin shared; do
            cp -R "$jl_source/$jl_dir" "$JL_ROOT/$jl_dir" || exit 1
        done
        mkdir -p "$JL_STATE" "$JL_CONFIG/ssh" "$JL_ROOT/slots" || exit 1
        chmod 700 "$JL_ROOT" "$JL_STATE" "$JL_CONFIG" "$JL_CONFIG/ssh" || exit 1
        sync
        jl_free_after=$(df -k "$JL_ROOT" | awk 'END {print $4}')
        case "$jl_free_after" in ''|*[!0-9]*) exit 1 ;; esac
        [ "$jl_free_after" -ge 64 ] || exit 1
        jl_unlock
        trap - EXIT
        jl_log 'controller prepared; install runtime before activation'
        ;;
    activate)
        [ "$#" = 1 ] || exit 2
        . "$JL_ROOT/boot/common.sh" || exit 1
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        jl_read_selection || exit 1
        jl_candidate=$JL_STABLE
        [ "$JL_PENDING" = - ] || jl_candidate=$JL_PENDING
        jl_slot_valid "$jl_candidate" || exit 1
        jl_verify_archive "$JL_ROOT/slots/$jl_candidate/runtime.tar.gz" "$JL_ROOT/slots/$jl_candidate/runtime.sha256" || exit 1
        [ -f "$JL_ROOT/boot/local.rc" ] || exit 1
        [ ! -L "$JL_ACTIVATE" ] || exit 1
        # This copy is evidence/manual recovery only. Never source or auto-restore
        # it: an inherited hook may start an unauthenticated root telnet shell.
        if [ -f "$JL_ACTIVATE" ] && [ ! -e "$JL_STATE/prelocal-hook.disabled" ]; then
            cp "$JL_ACTIVATE" "$JL_STATE/prelocal-hook.disabled" || exit 1
            chmod 600 "$JL_STATE/prelocal-hook.disabled" || exit 1
        fi
        jl_free=$(df -k "$JL_ROOT" | awk 'END {print $4}')
        case "$jl_free" in ''|*[!0-9]*) exit 1 ;; esac
        [ "$jl_free" -ge 68 ] || exit 1
        jl_activate_dir=${JL_ACTIVATE%/*}
        mkdir -p "$jl_activate_dir" || exit 1
        cp "$JL_ROOT/boot/local.rc" "$JL_ACTIVATE.new" || exit 1
        chmod 755 "$JL_ACTIVATE.new" || exit 1
        sync
        mv -f "$JL_ACTIVATE.new" "$JL_ACTIVATE" || exit 1
        sync
        killall telnetd 2>/dev/null || :
        jl_unlock
        trap - EXIT
        jl_log 'local-only boot hook activated; no inherited hook will run'
        ;;
    *) exit 2 ;;
esac
