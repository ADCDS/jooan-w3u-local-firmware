#!/bin/sh
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

jl_check_storage() {
    jl_storage_bytes=$(jl_tree_bytes "$JL_ROOT") || return 1
    [ "$jl_storage_bytes" -le 184320 ] || {
        jl_log "persistent regular files total ${jl_storage_bytes} bytes; limit is 184320 bytes (180 KiB)"
        return 1
    }
    jl_wait_free_kb "$JL_ROOT" 76 20
}

jl_check_maintenance_storage() {
    jl_storage_bytes=$(jl_tree_bytes "$JL_ROOT") || return 1
    [ "$jl_storage_bytes" -le 184320 ] || {
        jl_log "maintenance files total ${jl_storage_bytes} bytes; limit is 184320 bytes"
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
    route -n 2>/dev/null | awk '$1=="0.0.0.0" && $3=="0.0.0.0" {print $2, $8}' |
        while read -r jl_gateway jl_interface; do
            [ -n "$jl_interface" ] || continue
            if [ "$jl_gateway" = 0.0.0.0 ]; then
                route del default dev "$jl_interface" 2>/dev/null || :
            else
                route del default gw "$jl_gateway" dev "$jl_interface" 2>/dev/null || :
            fi
        done
    route -A inet6 -n 2>/dev/null | awk '$1=="::/0" {print $2, $NF}' |
        while read -r jl_gateway jl_interface; do
            [ -n "$jl_interface" ] || continue
            if [ "$jl_gateway" = :: ]; then
                route -A inet6 del ::/0 dev "$jl_interface" 2>/dev/null || :
            else
                route -A inet6 del ::/0 gw "$jl_gateway" dev "$jl_interface" 2>/dev/null || :
            fi
        done
}

jl_local_network_policy() {
    jl_prune_default_routes
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
    jl_free_path=$1 jl_free_min=$2 jl_free_tries=${3:-15}
    while [ "$jl_free_tries" -gt 0 ]; do
        sync
        jl_free_now=$(df -k "$jl_free_path" | awk 'END {print $4}')
        case "$jl_free_now" in ''|*[!0-9]*) return 1 ;; esac
        [ "$jl_free_now" -ge "$jl_free_min" ] && return 0
        jl_free_tries=$((jl_free_tries - 1))
        [ "$jl_free_tries" -gt 0 ] && sleep 1
    done
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
