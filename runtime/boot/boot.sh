#!/bin/sh
. "${JL_ROOT:-/opt/custom/jooan-local}/boot/common.sh" || exit 1
jl_init_run || exit 1

# Atomic directory ownership prevents two sourced boot hooks launching services.
if ! mkdir "$JL_RUN/supervisor.lock" 2>/dev/null; then
    exit 0
fi
trap 'rm -rf "$JL_RUN/supervisor.lock" 2>/dev/null || :' EXIT
printf '%s\n' "$$" > "$JL_RUN/supervisor.pid"

# OEM initialization may start telnet after local.rc has returned. This watcher
# deliberately survives a controller error; runtime failure must not enable it.
if mkdir "$JL_RUN/telnet-deny.lock" 2>/dev/null; then
    (
        while :; do
            killall telnetd 2>/dev/null || :
            sleep 1
        done
    ) &
fi

jl_lock || exit 1
if ! jl_read_selection; then
    jl_unlock
    jl_log 'invalid selection; no runtime started'
    JL_STABLE=- JL_PENDING=- JL_ATTEMPTED=0
else
    if [ "$JL_PENDING" != - ] && [ "$JL_ATTEMPTED" = 1 ]; then
        jl_log "unconfirmed trial $JL_PENDING rolled back to $JL_STABLE"
        jl_write_selection "$JL_STABLE" - 0 || { jl_unlock; exit 1; }
        JL_PENDING=- JL_ATTEMPTED=0
    fi
    if [ "$JL_PENDING" != - ]; then
        jl_write_selection "$JL_STABLE" "$JL_PENDING" 1 || { jl_unlock; exit 1; }
    fi
    jl_unlock
fi

jl_running=$JL_STABLE
[ "$JL_PENDING" = - ] || jl_running=$JL_PENDING
jl_trial=$JL_PENDING
jl_old_stable=$JL_STABLE
jl_trial_ok=0
if [ "$jl_running" != - ]; then
    if jl_start_slot "$jl_running"; then
        if [ "$jl_trial" != - ]; then
            # A candidate must pass its real health hook; elapsed time is not health.
            jl_checks=0
            while [ "$jl_checks" -lt 12 ]; do
                sleep 5
                if "$JL_ROOT/admin/mark-healthy.sh" "$jl_running"; then
                    jl_trial_ok=1
                    break
                fi
                jl_checks=$((jl_checks + 1))
            done
        fi
    fi
    if [ "$jl_trial" != - ] && [ "$jl_trial_ok" != 1 ]; then
        jl_log "trial $jl_trial failed; restoring runtime $jl_old_stable"
        # Never overlap two slots if the old process set cannot be stopped.
        if [ "$JL_SLOT_STARTED" = 0 ] || jl_stop_slot "$jl_trial"; then
            if jl_lock; then
                jl_write_selection "$jl_old_stable" - 0 || :
                jl_unlock
            fi
            jl_running=$jl_old_stable
            if [ "$jl_running" != - ]; then
                jl_start_slot "$jl_running" || jl_log 'stable runtime failed to start'
            else
                jl_set_running - || :
            fi
        else
            jl_log 'trial cleanup failed; stable runtime deferred until reboot'
            jl_running=-
        fi
    fi
fi

# Selected runtime supplies platform Wi-Fi recovery hooks after extraction.
if [ -x "$JL_ROOT/admin/wifi-transaction.sh" ]; then
    "$JL_ROOT/admin/wifi-transaction.sh" recover || jl_log 'Wi-Fi recovery hook failed'
fi

# This supervisor never invokes a saved OEM/user hook or revives telnet.
while :; do
    if [ -x "$JL_ROOT/admin/wifi-transaction.sh" ]; then
        "$JL_ROOT/admin/wifi-transaction.sh" tick || :
    fi
    if [ "$jl_running" != - ] && [ -x "$JL_ROOT/admin/ssh-start.sh" ]; then
        "$JL_ROOT/admin/ssh-start.sh" "$jl_running" || :
    fi
    sleep 5
done
