#!/bin/sh
. "${JL_CONTROL:-/run/jooan-local/controller}/boot/common.sh" || exit 1
jl_init_run || exit 1

# Atomic directory ownership prevents two sourced boot hooks launching services.
if ! mkdir "$JL_RUN/supervisor.lock" 2>/dev/null; then
    exit 0
fi
trap 'rm -rf "$JL_RUN/supervisor.lock" 2>/dev/null || :' EXIT
printf '%s\n' "$$" > "$JL_RUN/supervisor.pid"

# No RTC: lift the clock to the last persisted time before the runtime (and any
# recording) starts, so files are not stamped years in the past.
jl_restore_time || jl_log 'clock restore failed'

# OEM initialization may start services/routes after local.rc has returned.
# This independent watcher survives runtime/controller failure.
if mkdir "$JL_RUN/local-policy.lock" 2>/dev/null; then
    (
        while :; do
            jl_enforce_local_policy
            sleep 1
        done
    ) &
fi
jl_enforce_local_policy

jl_lock || exit 1
if ! jl_read_selection; then
    jl_unlock
    jl_log 'invalid selection; no runtime started'
    JL_STABLE=- JL_PENDING=- JL_ATTEMPTED=0
else
    if [ "$JL_PENDING" != - ] && [ "$JL_ATTEMPTED" = 1 ]; then
        jl_log "unconfirmed trial $JL_PENDING rolled back to $JL_STABLE"
        jl_write_selection "$JL_STABLE" - 0 || { jl_unlock; exit 1; }
        jl_prune_other_slots "$JL_STABLE" || { jl_unlock; exit 1; }
        JL_PENDING=- JL_ATTEMPTED=0
    fi
    if [ "$JL_PENDING" != - ]; then
        jl_write_selection "$JL_STABLE" "$JL_PENDING" 1 || { jl_unlock; exit 1; }
    fi
    jl_unlock
fi

if [ "$JL_PENDING" = - ]; then
    jl_prune_other_slots "$JL_STABLE" || jl_log 'steady slot cleanup/storage check failed'
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
                if "$JL_CONTROL/admin/mark-healthy.sh" "$jl_running"; then
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
                jl_prune_other_slots "$jl_old_stable" || :
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
if [ -x "$JL_CONTROL/admin/wifi-transaction.sh" ]; then
    "$JL_CONTROL/admin/wifi-transaction.sh" recover || jl_log 'Wi-Fi recovery hook failed'
fi

# This supervisor never invokes a saved OEM/user hook or revives telnet.
jl_time_ticks=0
while :; do
    if [ -x "$JL_CONTROL/admin/wifi-transaction.sh" ]; then
        "$JL_CONTROL/admin/wifi-transaction.sh" tick || :
    fi
    if [ -x "$JL_CONTROL/admin/ssh-start.sh" ]; then
        "$JL_CONTROL/admin/ssh-start.sh" "$jl_running" || :
    fi
    if [ "$jl_running" != - ]; then
        jl_ensure_wifi "$jl_running" || :
    fi
    # Persist the clock about every 30 minutes; NOR flash tolerates few writes.
    jl_time_ticks=$((jl_time_ticks + 1))
    if [ "$jl_time_ticks" -ge 360 ]; then
        jl_time_ticks=0
        jl_save_time || :
    fi
    sleep 5
done
