#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Shared controller helpers. Selection is data, never sourced shell code.
JL_ROOT=${JL_ROOT:-/opt/custom/jooan-local}
JL_RUN=${JL_RUN:-/run/jooan-local}
JL_CONTROL=${JL_CONTROL:-$JL_RUN/controller}
JL_STATE=$JL_ROOT/state
JL_CONFIG=$JL_ROOT/config
export JL_ROOT JL_RUN JL_CONTROL JL_STATE JL_CONFIG
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

# The board has no RTC, so every boot starts near 2021 until a client sets the
# clock -- which misdates SD recordings and the burned-in OSD. Persist a coarse
# last-known-good time and restore it at boot. The clock is only ever moved
# forward, so a client-set (accurate) time is never regressed. Written rarely,
# via /tmp then mv, because /opt is JFFS2 on NOR and nearly full.
jl_time_state() { printf '%s\n' "$JL_STATE/last-time"; }
jl_time_valid() { case "$1" in ''|*[!0-9]*) return 1 ;; esac; return 0; }
jl_restore_time() {
    jl_time_file=$(jl_time_state)
    [ -f "$jl_time_file" ] || return 0
    jl_saved=$(cat "$jl_time_file" 2>/dev/null) || return 0
    jl_time_valid "$jl_saved" || return 0
    jl_now=$(date -u +%s 2>/dev/null) || return 0
    jl_time_valid "$jl_now" || return 0
    [ "$jl_saved" -gt "$jl_now" ] || return 0
    date -s "@$jl_saved" >/dev/null 2>&1 || return 1
    jl_log "clock restored to persisted $jl_saved (no RTC on this board)"
}
jl_save_time() {
    jl_now=$(date -u +%s 2>/dev/null) || return 0
    jl_time_valid "$jl_now" || return 0
    jl_time_file=$(jl_time_state)
    if [ -f "$jl_time_file" ]; then
        jl_saved=$(cat "$jl_time_file" 2>/dev/null) || jl_saved=0
        jl_time_valid "$jl_saved" || jl_saved=0
        [ "$jl_now" -gt "$jl_saved" ] || return 0
    fi
    mkdir -p "$JL_STATE" 2>/dev/null || return 1
    jl_time_tmp=/tmp/jl-last-time.$$
    printf '%s\n' "$jl_now" > "$jl_time_tmp" 2>/dev/null || return 1
    mv -f "$jl_time_tmp" "$jl_time_file" 2>/dev/null || { rm -f "$jl_time_tmp"; return 1; }
}

jl_check_storage() {
    jl_storage_bytes=$(jl_tree_bytes "$JL_ROOT") || return 1
    [ "$jl_storage_bytes" -le 200704 ] || {
        jl_log "persistent regular files total ${jl_storage_bytes} bytes; limit is 200704 bytes (196 KiB)"
        return 1
    }
    jl_wait_free_kb "$JL_ROOT" 64 20
}

jl_check_maintenance_storage() {
    jl_storage_bytes=$(jl_tree_bytes "$JL_ROOT") || return 1
    [ "$jl_storage_bytes" -le 200704 ] || {
        jl_log "maintenance files total ${jl_storage_bytes} bytes; limit is 200704 bytes"
        return 1
    }
    jl_wait_free_kb "$JL_ROOT" 56 20
}

jl_check_current_storage() {
    jl_read_selection || return 1
    if [ "$JL_PENDING" = - ]; then
        jl_check_storage
    else
        jl_check_maintenance_storage
    fi
}

jl_tree_bytes() {
    # The camera's BusyBox find has no -type; test each enumerated path instead.
    find "$1" -print | while IFS= read -r jl_path; do
        [ ! -f "$jl_path" ] || wc -c < "$jl_path"
    done | awk '{n+=$1} END {printf "%.0f\n",n}'
}

# Keep resolver metadata and only local/private nameserver addresses.
jl_private_resolvers() {
    awk '
    function private_ip(s, a,n,i) {
        s=tolower(s)
        if (s=="::1" || s ~ /^f[cd][0-9a-f][0-9a-f]:/ || s ~ /^fe[89ab][0-9a-f]:/) return 1
        n=split(s,a,"."); if(n!=4) return 0
        for(i=1;i<=4;i++) if(a[i]!~/^[0-9]+$/ || a[i]>255) return 0
        return a[1]==10 || a[1]==127 || (a[1]==172 && a[2]>=16 && a[2]<=31) ||
            (a[1]==192 && a[2]==168) || (a[1]==169 && a[2]==254)
    }
    $1!="nameserver" || private_ip($2) {print}
    '
}

jl_prune_default_routes() {
    # Operate only on default routes. Connected and configured routes survive.
    # Remember the last gateway seen before deleting it: the allowlisted
    # private routes below are installed through it, and after the first
    # prune there is no default route left to learn it from.
    route -n 2>/dev/null | awk '$1=="0.0.0.0" && $3=="0.0.0.0" {print $2, $8}' |
        while read -r jl_gateway jl_interface; do
            [ -n "$jl_interface" ] || continue
            if [ "$jl_gateway" = 0.0.0.0 ]; then
                route del default dev "$jl_interface" 2>/dev/null || :
            else
                printf '%s %s\n' "$jl_gateway" "$jl_interface" \
                    > "$JL_RUN/default-gateway.new" 2>/dev/null &&
                    mv -f "$JL_RUN/default-gateway.new" "$JL_RUN/default-gateway" \
                        2>/dev/null || :
                route del default gw "$jl_gateway" dev "$jl_interface" 2>/dev/null || :
            fi
        done
    route -A inet6 -n 2>/dev/null | awk '$1=="::/0" {print $2, $NF}' |
        while read -r jl_gateway jl_interface; do
            [ -n "$jl_interface" ] || continue
            if [ "$jl_gateway" = :: ]; then
                route -A inet6 del ::/0 dev "$jl_interface" 2>/dev/null || :
            else
                printf '%s %s\n' "$jl_gateway" "$jl_interface" \
                    > "$JL_RUN/default-gateway6.new" 2>/dev/null &&
                    mv -f "$JL_RUN/default-gateway6.new" "$JL_RUN/default-gateway6" \
                        2>/dev/null || :
                route -A inet6 del ::/0 gw "$jl_gateway" dev "$jl_interface" 2>/dev/null || :
            fi
        done
}

# Install the administrator's allowlisted private routes through the gateway
# the default route was pruned from. These are specific prefixes, never a
# default route, so this cannot become an Internet path: the camera still has
# no route to anything outside the allowlist. It only restores the return path
# to management clients on a different local subnet than the camera, which a
# router already has to forward deliberately.
# The BusyBox `ip` applet here is built without the route object, so routes
# are managed with `route`, which needs a dotted netmask and refuses an
# existing route: derive the mask and skip prefixes already present, so this
# stays idempotent for the continuous enforce loop.
jl_netmask_of() {
    awk -v jl_p="$1" 'BEGIN {
        for (i = 0; i < 4; i++) {
            b = jl_p >= 8 ? 8 : (jl_p > 0 ? jl_p : 0)
            jl_p -= b
            o[i] = b == 0 ? 0 : 256 - 2 ^ (8 - b)
        }
        printf "%d.%d.%d.%d", o[0], o[1], o[2], o[3]
    }'
}

jl_apply_allowed_routes() {
    [ -f "$JL_CONFIG/routes.list" ] || return 0
    jl_route_gw= jl_route_if= jl_route_gw6= jl_route_if6=
    if [ -f "$JL_RUN/default-gateway" ]; then
        read -r jl_route_gw jl_route_if < "$JL_RUN/default-gateway" || :
    fi
    if [ -f "$JL_RUN/default-gateway6" ]; then
        read -r jl_route_gw6 jl_route_if6 < "$JL_RUN/default-gateway6" || :
    fi
    while IFS= read -r jl_cidr; do
        [ -n "$jl_cidr" ] || continue
        jl_net=${jl_cidr%/*}
        jl_prefix=${jl_cidr#*/}
        case "$jl_prefix" in ''|*[!0-9]*|0) continue ;; esac
        case "$jl_cidr" in
            *:*)
                [ -n "$jl_route_gw6" ] && [ -n "$jl_route_if6" ] || continue
                route -A inet6 -n 2>/dev/null |
                    awk -v jl_c="$jl_cidr" '$1==jl_c {found=1} END {exit !found}' &&
                    continue
                route -A inet6 add "$jl_cidr" gw "$jl_route_gw6" \
                    dev "$jl_route_if6" 2>/dev/null || :
                ;;
            *)
                [ -n "$jl_route_gw" ] && [ -n "$jl_route_if" ] || continue
                jl_mask=$(jl_netmask_of "$jl_prefix") || continue
                route -n 2>/dev/null |
                    awk -v jl_n="$jl_net" -v jl_m="$jl_mask" \
                        '$1==jl_n && $3==jl_m {found=1} END {exit !found}' &&
                    continue
                route add -net "$jl_net" netmask "$jl_mask" \
                    gw "$jl_route_gw" dev "$jl_route_if" 2>/dev/null || :
                ;;
        esac
    done < "$JL_CONFIG/routes.list"
}

# Apply the committed Wi-Fi network from the supervisor rather than from a
# background job in start.sh. OEM boot does not start wpa_supplicant until
# about a minute in, so the hook cannot configure anything at slot start, and
# a process parked waiting for it across that window does not reliably
# survive OEM init. The supervisor is already long-lived and ticks every few
# seconds, so it simply retries until wpa_supplicant answers, then applies
# once. Attempts are capped so a network that never accepts us cannot thrash
# the radio forever.
jl_wpa_pid() {
    for jl_proc in /proc/[0-9]*; do
        [ "$(cat "$jl_proc/comm" 2>/dev/null)" = wpa_supplicant ] || continue
        printf '%s\n' "${jl_proc#/proc/}"
        return 0
    done
    return 1
}

# jooanipc owns wpa_supplicant. About two minutes into boot it can restart it
# from its own stored network (on this unit a retired bench AP), which drops
# the committed one, and the SKW6316 then hunts for the old network until a
# power cycle. Whether that lands before or after our apply is a race. So the
# committed network belongs to the wpa_supplicant process it was applied to:
# a new process gets it applied again, with its own five attempts.
jl_ensure_wifi() {
    [ -f "$JL_CONFIG/wifi.json" ] || return 0
    jl_wpa=$(jl_wpa_pid) || return 0
    [ "$(cat "$JL_RUN/wifi-applied" 2>/dev/null)" != "$jl_wpa" ] || return 0
    jl_wifi_hook=$JL_RUN/slot-$1/hooks/wifi-apply.sh
    [ -x "$jl_wifi_hook" ] || return 0
    wpa_cli -iwlan0 ping 2>/dev/null | grep -q PONG || return 0
    jl_wifi_owner='' jl_wifi_tries=0
    [ ! -f "$JL_RUN/wifi-attempts" ] ||
        IFS=: read -r jl_wifi_owner jl_wifi_tries < "$JL_RUN/wifi-attempts" || :
    [ "$jl_wifi_owner" = "$jl_wpa" ] || jl_wifi_tries=0
    case "$jl_wifi_tries" in ''|*[!0-9]*) jl_wifi_tries=0 ;; esac
    if [ "$jl_wifi_tries" -ge 5 ]; then
        # Enough for a network that is simply not going to work, but not while a native
        # boot is waiting for this very network: a router can come back after any time.
        [ -e /tmp/jipc-network-wait ] || return 0
        jl_wifi_next=0
        [ ! -f "$JL_RUN/wifi-next" ] || IFS= read -r jl_wifi_next < "$JL_RUN/wifi-next" || :
        case "$jl_wifi_next" in ''|*[!0-9]*) jl_wifi_next=0 ;; esac
        [ "$(jl_uptime)" -ge "$jl_wifi_next" ] || return 0
        printf '%s\n' "$(( $(jl_uptime) + 300 ))" > "$JL_RUN/wifi-next" || :
    fi
    printf '%s:%s\n' "$jl_wpa" "$((jl_wifi_tries + 1))" > "$JL_RUN/wifi-attempts" || :
    jl_wifi_rc=0
    jl_bounded_hook 60 "$jl_wifi_hook" "$JL_CONFIG/wifi.json" || jl_wifi_rc=$?
    case "$jl_wifi_rc" in
        0) printf '%s\n' "$jl_wpa" > "$JL_RUN/wifi-applied" || : ;;
        3)
            # Nothing was tried. So nothing is trying to join, which is what a native boot
            # asks before it waits: the fallback gets its turn. Not retried either.
            rm -f "$JL_RUN/wifi-attempts"
            printf '%s\n' "$jl_wpa" > "$JL_RUN/wifi-applied" || :
            jl_log 'committed Wi-Fi network cannot be tried: its file or tools are broken'
            ;;
        *) jl_log 'committed Wi-Fi network could not be applied' ;;
    esac
}

jl_local_network_policy() {
    jl_prune_default_routes
    jl_apply_allowed_routes
    for jl_resolver in /tmp/resolv.conf /etc/resolv.conf; do
        [ -f "$jl_resolver" ] && [ -w "$jl_resolver" ] || continue
        jl_dns_old=$(cat "$jl_resolver") || continue
        jl_dns_new=$(printf '%s\n' "$jl_dns_old" | jl_private_resolvers) || continue
        if [ "$jl_dns_old" != "$jl_dns_new" ]; then
            printf '%s\n' "$jl_dns_new" > "$jl_resolver" || :
        fi
    done
}

jl_enforce_local_policy() {
    killall telnetd goahead 2>/dev/null || :
    jl_local_network_policy
}

# Generate projected account files without displaying credential material.
# Caller must bind only these tmpfs files, never rewrite OEM /etc on flash.
jl_make_accounts() {
    jl_account_source=$1 jl_credential_file=$2 jl_account_out=$3 jl_admin_home=$4
    [ "$(wc -l < "$jl_credential_file")" = 1 ] || return 1
    IFS=: read -r jl_username jl_password_hash jl_account_extra < "$jl_credential_file" || return 1
    [ "$jl_username" = admin ] && [ -z "$jl_account_extra" ] || return 1
    case "$jl_password_hash" in
        '!') ;;
        \$*)
            case "$jl_password_hash" in *[!A-Za-z0-9./\$=]*) return 1 ;; esac
            [ "${#jl_password_hash}" -ge 20 ] && [ "${#jl_password_hash}" -le 255 ] || return 1
            ;;
        *) return 1 ;;
    esac
    mkdir -p "$jl_account_out" || return 1
    # Preserve OEM uid/gid lookups while denying every non-admin login shell.
    awk -F: 'BEGIN {OFS=":"} $1!="admin" && NF==7 {$2="!"; $7="/nonexistent/jooan-disabled-login"; print}' \
        "$jl_account_source" > "$jl_account_out/passwd.new" || return 1
    printf 'admin:x:0:0:Local administrator:%s:/bin/sh\n' "$jl_admin_home" >> "$jl_account_out/passwd.new" || return 1
    awk -F: '$1!="admin" && NF==7 {print $1 ":!:::::::"}' "$jl_account_source" > "$jl_account_out/shadow.new" || return 1
    printf 'admin:%s:::::::\n' "$jl_password_hash" >> "$jl_account_out/shadow.new" || return 1
    chmod 644 "$jl_account_out/passwd.new" || return 1
    chmod 600 "$jl_account_out/shadow.new" || return 1
    jl_password_hash=
}

# JFFS2 may report old erase blocks as occupied briefly after a large removal.
# Bound the wait: a genuinely full partition must still fail closed.
jl_wait_free_kb() {
    jl_free_path=$1 jl_free_min=$2 jl_free_tries=${3:-15} jl_free_waited=0
    while [ "$jl_free_tries" -gt 0 ]; do
        sync
        jl_free_now=$(df -k "$jl_free_path" | awk 'END {print $4}')
        case "$jl_free_now" in ''|*[!0-9]*) return 1 ;; esac
        if [ "$jl_free_now" -ge "$jl_free_min" ]; then
            [ "$jl_free_waited" = 0 ] || jl_log "$jl_free_min KiB free after $jl_free_waited s"
            return 0
        fi
        jl_free_tries=$((jl_free_tries - 1))
        if [ "$jl_free_tries" -gt 0 ]; then sleep 1; jl_free_waited=$((jl_free_waited + 1)); fi
    done
    jl_log "$jl_free_min KiB not free after $jl_free_waited s: $jl_free_now KiB"
    return 1
}

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

jl_prune_other_slots() {
    jl_keep=$1
    case "$jl_keep" in
        A) jl_remove=B ;;
        B) jl_remove=A ;;
        -) jl_remove='A B' ;;
        *) return 1 ;;
    esac
    for jl_slot_remove in $jl_remove; do
        rm -rf "$JL_ROOT/slots/$jl_slot_remove.new" \
            "$JL_ROOT/slots/$jl_slot_remove.previous" \
            "$JL_ROOT/slots/$jl_slot_remove" || return 1
    done
    sync
    jl_check_storage
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
        # The sleep ends with the hook. Left behind, it would hold whatever it
        # inherited for the whole limit, such as the pipe a caller reads to its end.
        trap 'kill "${jl_sleep_pid:-}" 2>/dev/null; exit 0' TERM
        sleep "$jl_limit" &
        jl_sleep_pid=$!
        wait "$jl_sleep_pid"
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
    # Corruption check only. Authenticity comes from the OTA installer's
    # SHA/signature verification in tmpfs, never from an MD5 sidecar.
    jl_archive=$1
    jl_digestfile=$2
    [ -f "$jl_archive" ] && [ -f "$jl_digestfile" ] || return 1
    jl_expected=$(cat "$jl_digestfile") || return 1
    [ "${#jl_expected}" = 32 ] || return 1
    case "$jl_expected" in *[!0-9a-f]*) return 1 ;; esac
    jl_actual=$(md5sum "$jl_archive") || return 1
    # Sidecar is digest only; md5sum output also contains the filename.
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
    jl_verify_archive "$JL_ROOT/slots/$JL_SLOT/runtime.tar.gz" "$JL_ROOT/slots/$JL_SLOT/runtime.md5" || return 1
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

# start.sh records the runtime process; comm guards against PID reuse.
jl_runtime_alive() {
    jl_pid=$(cat "$JL_RUN/daemon.pid" 2>/dev/null) || return 1
    [ "$(cat "/proc/$jl_pid/comm" 2>/dev/null)" = joan-daemon ]
}

# The kernel's OOM report says who held the memory, and dmesg does not survive
# the reboot. The SD card is the only storage with room; a crash loop must not
# fill it.
jl_record_crash() {
    grep -q ' /mnt/sd_card ' /proc/mounts || return 0
    jl_crash_dir=/mnt/sd_card/jooan-local
    mkdir -p "$jl_crash_dir" || return 0
    [ "$(ls "$jl_crash_dir" | wc -l)" -lt 30 ] || return 0
    { date; cat /proc/meminfo /proc/net/sockstat; ps; dmesg; } \
        > "$jl_crash_dir/${1:-runtime}-$(date +%s).log" 2>&1
}

jl_uptime() { cut -d. -f1 "${JL_PROC_UPTIME:-/proc/uptime}"; }

# Scheduled restart: config/restart-schedule holds "HH:MM" in the camera's
# time zone (the vendor config's GmtTz, which PUT /api/v1/timezone writes).
# Once a day: only with a plausible clock, never in the first hour after a
# boot (so it cannot repeat), and never during an update, a Wi-Fi trial or a
# boot trial.
jl_scheduled_restart() {
    [ -r "$JL_ROOT/config/restart-schedule" ] || return 0
    IFS= read -r jl_rs_at < "$JL_ROOT/config/restart-schedule" || return 0
    case "$jl_rs_at" in [0-2][0-9]:[0-5][0-9]) ;; *) return 0 ;; esac
    [ "$(jl_uptime)" -ge 3600 ] || return 0
    # Not into a network outage: nothing would be gained and video would be lost.
    [ "${jl_netdog_ok:-}" = 1 ] && [ -z "${jl_netdog_down:-}" ] || return 0
    # Without a clock anything has confirmed, "04:00" means nothing: once a day
    # counted from the start instead.
    if [ ! -e "$JL_RUN/clock-set" ]; then
        [ "$(jl_uptime)" -ge 86400 ] || return 0
        jl_rs_busy || return 0
        jl_log 'scheduled restart, by uptime: the clock has not been confirmed'
        jl_reboot_asked
        return 0
    fi
    jl_rs_now=$(date +%s 2>/dev/null) || return 0
    case "$jl_rs_now" in ''|*[!0-9]*) return 0 ;; esac
    [ "$jl_rs_now" -ge 1700000000 ] || return 0
    jl_rs_off=0
    jl_rs_tz=$(sed -n 's/.*"GmtTz"[[:space:]]*:[[:space:]]*"GMT\([+-]\)\([0-9][0-9]\):\([0-9][0-9]\)".*/\1 \2 \3/p' \
        "${JL_OEM_CONFIG:-/opt/conf/config.json}" 2>/dev/null | head -n 1)
    if [ -n "$jl_rs_tz" ]; then
        set -- $jl_rs_tz
        jl_rs_h=${2#0} jl_rs_m=${3#0}
        jl_rs_off=$(( ${jl_rs_h:-0} * 60 + ${jl_rs_m:-0} ))
        [ "$1" = + ] || jl_rs_off=$((0 - jl_rs_off))
    fi
    jl_rs_local=$(( ((jl_rs_now / 60 + jl_rs_off) % 1440 + 1440) % 1440 ))
    jl_rs_h=${jl_rs_at%%:*} jl_rs_m=${jl_rs_at#*:}
    jl_rs_h=${jl_rs_h#0} jl_rs_m=${jl_rs_m#0}
    [ "$jl_rs_local" = $(( ${jl_rs_h:-0} * 60 + ${jl_rs_m:-0} )) ] || return 0
    jl_rs_busy || return 0
    jl_log "scheduled restart at $jl_rs_at"
    jl_reboot_asked
}
jl_rs_busy() {
    [ ! -d "$JL_RUN/state.lock" ] && [ ! -d "$JL_RUN/firmware-apply.lock" ] && [ ! -e "$JL_STATE/wifi-trial" ] || return 1
    if [ -r "$JL_STATE/selection" ] && read -r _jl_rs_stable jl_rs_pending _jl_rs_x < "$JL_STATE/selection" &&
       [ "${jl_rs_pending:--}" != - ]; then return 1; fi
    jl_no_runtime && return 1
    return 0
}
# A platform update stopped half way (docs/RECOVERY.md): this runtime runs from RAM and none is on
# flash, so the next boot would have no network. Nothing but a person should restart this camera.
jl_no_runtime() {
    [ -r "$JL_STATE/selection" ] && read -r jl_nr_stable jl_nr_pending _jl_nr_x < "$JL_STATE/selection" &&
        [ "${jl_nr_stable:--}" = - ] && [ "${jl_nr_pending:--}" = - ]
}
# A reboot the platform decided on: keep the clock's time (only saved every half
# hour otherwise), and tell the next boot this was no failure of the media layer.
jl_reboot_asked() {
    jl_save_time || :
    jl_ra_media=${JL_MEDIA_ROOT:-/opt/jooanipc-open}
    jl_ra_boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null) || jl_ra_boot=
    # printf, not ":": a failed redirection on a special builtin would end the shell before the reboot.
    [ ! -d "$jl_ra_media" ] || printf '%s\n' "$jl_ra_boot" 2>/dev/null > "$jl_ra_media/reboot-requested" || :
    sync
    reboot
}

# The SKW6316's own firmware can crash: the driver logs an assert ending in
# DUMPDONE and then refuses every transmit, so the camera stays associated
# but silent. Nothing on the board resets the chip; a reboot does. So reboot
# when the gateway stops answering ARP (the IoT firewall may drop ICMP) for
# JL_NETDOG_DOWN seconds, or 30 once the chip has logged a crash. A path that
# never worked this boot first waits for 15 minutes of uptime, and every
# watchdog reboot doubles that wait (to 4 hours) until the network has been up
# for 30 minutes, so a network that is simply gone cannot reboot-loop the
# camera. Never during maintenance or a Wi-Fi trial.
jl_netdog_ok=0
jl_netdog_down=
# Flight recorder: the board loses its RAM logs at a reset and has been
# resetting a few times a day for reasons not yet understood. Keep a short,
# bounded trail in flash: one line per boot with the address the CPU was reset
# at (the kernel's "RESET ERROR PC"), and the kernel's last lines when the
# gateway stops answering, at most three times per boot.
jl_note_boot() {
    jl_fr=$JL_STATE/reset-history
    jl_fr_pc=$(dmesg 2>/dev/null | sed -n 's/.*RESET ERROR PC:\([0-9a-fA-F]*\).*/\1/p' | head -n 1)
    jl_fr_nd=0
    [ ! -r "$JL_STATE/netdog-reboots" ] || IFS= read -r jl_fr_nd < "$JL_STATE/netdog-reboots" || :
    { [ ! -r "$jl_fr" ] || tail -n 19 "$jl_fr"
      printf '%s pc=%s netdog=%s\n' "$(date +%s)" "${jl_fr_pc:--}" "${jl_fr_nd:-0}"; } > "$jl_fr.new" 2>/dev/null &&
        chmod 600 "$jl_fr.new" && mv -f "$jl_fr.new" "$jl_fr" || :
}
# Reset to defaults, requested through integration-helper.sh factory-reset.
# Erased: both passwords (back to the initial value), SSH keys, remembered
# sign-ins, the restart schedule, presets, and the picture, night, light and
# audio controls. Kept, so the camera stays reachable the same way: Wi-Fi, local
# name, allowed networks, network time, time zone, certificates and the installed
# releases. Safe to repeat: the mark goes last, so a power cut resumes it.
jl_apply_reset() {
    [ -e "$JL_STATE/reset-pending" ] || return 0
    jl_log 'reset to defaults'
    rm -f "$JL_CONFIG/auth.db" "$JL_CONFIG/sessions.db" "$JL_CONFIG/restart-schedule" \
        "$JL_CONFIG/ssh/passwd" "$JL_CONFIG/ssh/authorized_keys"
    jl_ar_media=${JL_MEDIA_ROOT:-/opt/jooanipc-open}
    rm -f "$jl_ar_media/state/controls.json" "$jl_ar_media/state/presets.v2.json" "$jl_ar_media/state/presets.v1"
    # Wi-Fi stays, but an address goes back to automatic: on a new router a fixed
    # one leaves the camera on the old subnet, which a reset must be able to undo.
    jl_ar_wifi=$JL_CONFIG/wifi.json
    if grep -q '"address"' "$jl_ar_wifi" 2>/dev/null; then
        sed -e 's/,[[:space:]]*"address"[[:space:]]*:[[:space:]]*"[0-9.]*"//' \
            -e 's/,[[:space:]]*"prefix"[[:space:]]*:[[:space:]]*"[0-9.]*"//' \
            -e 's/,[[:space:]]*"gateway"[[:space:]]*:[[:space:]]*"[0-9.]*"//' "$jl_ar_wifi" > "$jl_ar_wifi.new" &&
            grep -q '"ssid"' "$jl_ar_wifi.new" && chmod 600 "$jl_ar_wifi.new" && mv -f "$jl_ar_wifi.new" "$jl_ar_wifi" || :
        rm -f "$jl_ar_wifi.new"
    fi
    if [ -f "$jl_ar_media/camera.ini" ]; then
        sed 's/^password=.*/password=admin/' "$jl_ar_media/camera.ini" > "$jl_ar_media/camera.ini.new" && # hygiene: allow-public-bootstrap
            chmod 600 "$jl_ar_media/camera.ini.new" && mv -f "$jl_ar_media/camera.ini.new" "$jl_ar_media/camera.ini" || return 0
    fi
    sync
    rm -f "$JL_STATE/reset-pending"
    sync
}

jl_net_loss_count=0
jl_net_loss_armed=1
jl_note_net_loss() {
    [ "$jl_net_loss_armed" = 1 ] && [ "$jl_net_loss_count" -lt 3 ] || return 0
    jl_net_loss_armed=0 jl_net_loss_count=$((jl_net_loss_count + 1))
    { date; printf 'uptime %s\n' "$(jl_uptime)"
      wpa_cli -iwlan0 status 2>/dev/null | grep -E '^(wpa_state|freq|bssid)='
      dmesg 2>/dev/null | tail -c 3500; } > "$JL_STATE/net-loss.log.new" 2>&1 &&
        chmod 600 "$JL_STATE/net-loss.log.new" && mv -f "$JL_STATE/net-loss.log.new" "$JL_STATE/net-loss.log" || :
}

jl_net_watchdog() {
    if [ -d "$JL_RUN/state.lock" ] || [ -d "$JL_RUN/firmware-apply.lock" ] || [ -e "$JL_STATE/wifi-trial" ] ||
       [ ! -r "$JL_RUN/default-gateway" ] ||
       ! read -r jl_netdog_gw jl_netdog_dev < "$JL_RUN/default-gateway"; then
        jl_netdog_down=
        return 0
    fi
    jl_netdog_now=$(jl_uptime)
    if arping -c 1 -w 2 -I "$jl_netdog_dev" "$jl_netdog_gw" >/dev/null 2>&1; then
        jl_netdog_ok=1 jl_netdog_down= jl_net_loss_armed=1
        if [ "$jl_netdog_now" -ge 1800 ] && [ -f "$JL_STATE/netdog-reboots" ]; then
            rm -f "$JL_STATE/netdog-reboots"
        fi
        return 0
    fi
    [ -n "$jl_netdog_down" ] || jl_netdog_down=$jl_netdog_now
    [ $((jl_netdog_now - jl_netdog_down)) -lt 10 ] || jl_note_net_loss || :
    jl_netdog_limit=${JL_NETDOG_DOWN:-300}
    dmesg 2>/dev/null | grep -q DUMPDONE && jl_netdog_limit=30
    [ $((jl_netdog_now - jl_netdog_down)) -ge "$jl_netdog_limit" ] || return 0
    jl_netdog_n=0
    [ ! -f "$JL_STATE/netdog-reboots" ] || IFS= read -r jl_netdog_n < "$JL_STATE/netdog-reboots" || :
    case "$jl_netdog_n" in ''|*[!0-9]*) jl_netdog_n=0 ;; esac
    jl_netdog_wait=0
    if [ "$jl_netdog_n" != 0 ] || [ "$jl_netdog_ok" != 1 ]; then
        jl_netdog_shift=$jl_netdog_n
        [ "$jl_netdog_shift" -le 4 ] || jl_netdog_shift=4
        jl_netdog_wait=$((900 << jl_netdog_shift))
    fi
    [ "$jl_netdog_now" -ge "$jl_netdog_wait" ] || return 0
    if jl_no_runtime; then
        jl_log 'network watchdog: no reboot, there is no runtime on flash (docs/RECOVERY.md)'
        return 0
    fi
    printf '%s\n' "$((jl_netdog_n + 1))" > "$JL_STATE/netdog-reboots" || :
    jl_record_crash netdog || :
    jl_reboot_asked
}
