#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
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
jl_note_boot || :

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
    if [ "$JL_PENDING" != - ] && [ "$JL_ATTEMPTED" = 1 ] && [ "$JL_STABLE" = - ]; then
        # An update deletes the old runtime before staging the candidate, so
        # an interrupted trial has nothing to roll back to. Removing the
        # candidate too would leave no runtime and, on a native boot, no
        # Wi-Fi (the runtime's hook applies the committed network).
        jl_log "unconfirmed trial $JL_PENDING kept: there is no previous runtime"
        jl_write_selection "$JL_PENDING" - 0 || { jl_unlock; exit 1; }
        JL_STABLE=$JL_PENDING JL_PENDING=- JL_ATTEMPTED=0
    elif [ "$JL_PENDING" != - ] && [ "$JL_ATTEMPTED" = 1 ]; then
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
    jl_start_slot "$jl_running" || jl_log "runtime $jl_running did not start; retrying"
    if [ "$jl_trial" != - ]; then
        # A candidate must pass its real health hook; elapsed time is not health.
        # Its first start can fail through no fault of its own: start.sh is
        # bounded to 15 s while entropy-ready.sh waits up to 30 s, and an early
        # native boot (no OEM application running) gathers entropy slowly. So
        # restart it inside the two-minute window, as the supervisor loop does
        # for a stable runtime. The window is elapsed time, not attempts: a
        # failed start alone can take 25 s.
        # The committed network must not wait for the trial either: a native
        # boot gives up on its gateway after 300 s and latches native-failed.
        jl_trial_deadline=$(( $(jl_uptime) + 120 ))
        while [ "$(jl_uptime)" -lt "$jl_trial_deadline" ]; do
            sleep 5
            jl_ensure_wifi "$jl_running" || :
            if ! jl_runtime_alive; then
                jl_stop_slot "$jl_running" || :
                jl_start_slot "$jl_running" || :
                continue
            fi
            if "$JL_CONTROL/admin/mark-healthy.sh" "$jl_running"; then
                jl_trial_ok=1
                break
            fi
        done
    fi
    if [ "$jl_trial" != - ] && [ "$jl_trial_ok" != 1 ] && [ "$jl_old_stable" = - ]; then
        # Nothing to restore (see the unconfirmed-trial case above). Keep the
        # candidate as the runtime: the loop below keeps restarting it, its
        # hook keeps applying Wi-Fi, and SSH recovery stays reachable.
        jl_log "trial $jl_trial failed with no previous runtime; keeping it"
        if jl_lock; then
            jl_write_selection "$jl_trial" - 0 || :
            jl_unlock
        fi
    elif [ "$jl_trial" != - ] && [ "$jl_trial_ok" != 1 ]; then
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
jl_respawn_hold=0
while :; do
    # The OOM killer now takes the runtime rather than jooanipc (local.rc), so
    # bring it back; jooanipc reconnects to the MQTT bridge within ~20s. Never
    # while maintenance holds the state lock, and at most once a minute.
    [ "$jl_respawn_hold" = 0 ] || jl_respawn_hold=$((jl_respawn_hold - 1))
    if [ "$jl_running" != - ] && [ "$jl_respawn_hold" = 0 ] &&
       [ ! -d "$JL_RUN/state.lock" ] && ! jl_runtime_alive; then
        jl_respawn_hold=12
        jl_record_crash || :
        jl_stop_slot "$jl_running" || :
        jl_start_slot "$jl_running" || jl_log 'runtime restart failed'
    fi
    if [ -x "$JL_CONTROL/admin/wifi-transaction.sh" ]; then
        "$JL_CONTROL/admin/wifi-transaction.sh" tick || :
    fi
    if [ -x "$JL_CONTROL/admin/ssh-start.sh" ]; then
        "$JL_CONTROL/admin/ssh-start.sh" "$jl_running" || :
    fi
    if [ "$jl_running" != - ]; then
        jl_ensure_wifi "$jl_running" || :
    fi
    jl_net_watchdog || :
    jl_scheduled_restart || :
    # Persist the clock about every 30 minutes; NOR flash tolerates few writes.
    jl_time_ticks=$((jl_time_ticks + 1))
    if [ "$jl_time_ticks" -ge 360 ]; then
        jl_time_ticks=0
        jl_save_time || :
    fi
    sleep 5
done
