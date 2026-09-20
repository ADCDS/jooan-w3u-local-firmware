#!/bin/sh
# Platform contract: hooks/wifi-snapshot.sh <backup-dir>,
# wifi-apply.sh <candidate-file>, wifi-health.sh,
# wifi-commit.sh <candidate-file>, wifi-rollback.sh <backup-dir>.
# Hooks must confine mutations to network settings and return promptly. Candidate
# contents are private and are never sourced as shell code or logged here.
. "${JL_ROOT:-/opt/custom/jooan-local}/boot/common.sh" || exit 1
jl_init_run || exit 1
[ "$#" -ge 1 ] || exit 2
jl_action=$1
jl_tx=$JL_STATE/wifi-trial

jl_wifi_hooks() {
    jl_slot_valid "$jl_wifi_slot" || return 1
    jl_verify_archive "$JL_ROOT/slots/$jl_wifi_slot/runtime.tar.gz" "$JL_ROOT/slots/$jl_wifi_slot/runtime.sha256" || return 1
    if [ ! -d "$JL_RUN/slot-$jl_wifi_slot" ]; then
        jl_unpack_archive "$JL_ROOT/slots/$jl_wifi_slot/runtime.tar.gz" "$JL_RUN/slot-$jl_wifi_slot" || return 1
    fi
    jl_hooks=$JL_RUN/slot-$jl_wifi_slot/hooks
}

jl_wifi_rollback() {
    [ -d "$jl_tx" ] || return 0
    IFS= read -r jl_wifi_slot < "$jl_tx/slot" || return 1
    jl_wifi_hooks || return 1
    [ ! -e "$JL_STATE/wifi-rolled-back" ] || return 1
    jl_bounded_hook 15 "$jl_hooks/wifi-rollback.sh" "$jl_tx/backup" || return 1
    # The saved configuration remains available for audit/manual recovery.
    mv "$jl_tx" "$JL_STATE/wifi-rolled-back" || return 1
    sync
}

case "$jl_action" in
    apply)
        [ "$#" = 2 ] && [ -f "$2" ] || exit 2
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        # A new explicit administrator request supersedes a terminal record;
        # active trials and incomplete staging still fail closed below.
        rm -rf "$JL_STATE/wifi-rolled-back" "$JL_STATE/wifi-committed" || exit 1
        [ ! -e "$jl_tx" ] && [ ! -e "$jl_tx.new" ] && [ ! -e "$JL_STATE/wifi-rolled-back" ] &&
            [ ! -e "$JL_STATE/wifi-committed" ] || {
            jl_log 'previous Wi-Fi transaction must be archived first'; exit 1;
        }
        IFS= read -r jl_wifi_slot < "$JL_RUN/running" || exit 1
        jl_wifi_hooks || exit 1
        for jl_hook in wifi-snapshot.sh wifi-apply.sh wifi-health.sh wifi-commit.sh wifi-rollback.sh; do
            [ -x "$jl_hooks/$jl_hook" ] || { jl_log 'Wi-Fi platform hooks unavailable'; exit 1; }
        done
        mkdir -p "$jl_tx.new/backup" || exit 1
        printf '%s\n' "$jl_wifi_slot" > "$jl_tx.new/slot" || exit 1
        cp "$2" "$jl_tx.new/candidate.json" || exit 1
        chmod 600 "$jl_tx.new/candidate.json" || exit 1
        jl_bounded_hook 10 "$jl_hooks/wifi-snapshot.sh" "$jl_tx.new/backup" || exit 1
        jl_now=$(cut -d. -f1 /proc/uptime)
        case "$jl_now" in ''|*[!0-9]*) exit 1 ;; esac
        printf '%s\n' "$((jl_now + 90))" > "$jl_tx.new/deadline" || exit 1
        # A complete rollback snapshot and deadline are durable BEFORE mutation.
        mv "$jl_tx.new" "$jl_tx" || exit 1
        sync
        if ! jl_bounded_hook 50 "$jl_hooks/wifi-apply.sh" "$2"; then
            jl_wifi_rollback || :
            exit 1
        fi
        # The boot supervisor enforces deadline with tick; reboot calls recover.
        # Commit must come from a fresh connection on the proposed Wi-Fi network.
        jl_log 'Wi-Fi trial applied; reconnect and commit within 90 seconds'
        ;;
    commit)
        [ "$#" = 1 ] || exit 2
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        [ -d "$jl_tx" ] && [ ! -e "$JL_STATE/wifi-committed" ] || exit 1
        IFS= read -r jl_wifi_slot < "$jl_tx/slot" || exit 1
        jl_wifi_hooks || exit 1
        jl_bounded_hook 10 "$jl_hooks/wifi-health.sh" || exit 1
        IFS= read -r jl_deadline < "$jl_tx/deadline" || exit 1
        jl_now=$(cut -d. -f1 /proc/uptime)
        case "$jl_deadline:$jl_now" in *[!0-9:]*|:*|*:) exit 1 ;; esac
        [ "$jl_now" -lt "$jl_deadline" ] || { jl_wifi_rollback; exit 1; }
        jl_bounded_hook 10 "$jl_hooks/wifi-commit.sh" "$jl_tx/candidate.json" || exit 1
        mv "$jl_tx" "$JL_STATE/wifi-committed" || exit 1
        sync
        ;;
    tick|rollback|recover)
        [ "$#" = 1 ] || exit 2
        jl_lock || exit 1
        trap 'jl_unlock' EXIT
        if [ "$jl_action" = tick ]; then
            [ -d "$jl_tx" ] || exit 0
            IFS= read -r jl_deadline < "$jl_tx/deadline" || exit 1
            jl_now=$(cut -d. -f1 /proc/uptime)
            case "$jl_deadline:$jl_now" in *[!0-9:]*|:*) exit 1 ;; esac
            [ "$jl_now" -ge "$jl_deadline" ] || exit 0
        fi
        jl_wifi_rollback
        ;;
    *) exit 2 ;;
esac
