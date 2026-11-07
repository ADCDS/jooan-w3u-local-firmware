#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
root=${JL_ROOT:-/opt/custom/jooan-local}
run=${JL_RUN:-/run/jooan-local}
control=${JL_CONTROL:-$run/controller}
PATH=${JOOAN_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}
export PATH

# This BusyBox has NO rmdir applet, so every `rmdir` here silently did nothing
# and the mount point outlived the run that made it. `mkdir` then refused it
# and the SAME package could never be verified again -- which only ever bites
# a retry, because a different package gets a different id and a fresh name.
# `rm -rf` is what this target actually has.
firmware_mount() {
    fw_image=$1 fw_mount=$2
    if [ -d "$fw_mount" ]; then
        # Reclaim a leftover from a killed run; refuse if something is still
        # mounted on it rather than mounting over the top.
        umount "$fw_mount" 2>/dev/null || :
        rm -rf "$fw_mount" 2>/dev/null || :
        [ -d "$fw_mount" ] && return 1
    fi
    mkdir "$fw_mount" || return 1
    mount -t squashfs -o ro,loop "$fw_image" "$fw_mount" || {
        rm -rf "$fw_mount" 2>/dev/null || :
        return 1
    }
}
firmware_unmount() {
    fw_mount=$1
    umount "$fw_mount" 2>/dev/null || return 1
    rm -rf "$fw_mount" 2>/dev/null || :
}
firmware_metadata() {
    fw_manifest=$1
    release_sequence=$(sed -n 's/^release_sequence=//p' "$fw_manifest")
    minimum_sequence=$(sed -n 's/^minimum_sequence=//p' "$fw_manifest")
    artifact_kind=$(sed -n 's/^artifact_kind=//p' "$fw_manifest")
    release_version=$(sed -n 's/^release_version=//p' "$fw_manifest")
    case "$release_sequence:$minimum_sequence" in ''|:*|*:|*[!0-9:]*) return 1 ;; esac
    [ "$release_sequence" -ge "$minimum_sequence" ] || return 1
    case "$artifact_kind" in install|uninstall) ;; *) return 1 ;; esac
    [ -n "$release_version" ] || return 1
    grep -qx 'target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316' "$fw_manifest" || return 1
    grep -qx 'device_model=JA-A12' "$fw_manifest" || return 1
    grep -qx 'model_token=A12' "$fw_manifest" || return 1
}
private_routes_validate() {
    route_file=$1
    [ -f "$route_file" ] || return 0
    while IFS= read -r cidr; do
        [ -n "$cidr" ] || return 1
        case "$cidr" in *[!0-9A-Fa-f:./]*) return 1 ;; esac
        case "$cidr" in
            10.*/*|192.168.*/*|172.16.*/*|172.17.*/*|172.18.*/*|172.19.*/*|\
            172.20.*/*|172.21.*/*|172.22.*/*|172.23.*/*|172.24.*/*|172.25.*/*|\
            172.26.*/*|172.27.*/*|172.28.*/*|172.29.*/*|172.30.*/*|172.31.*/*|\
            fc*:*/*|fd*:*/*) ;;
            *) return 1 ;;
        esac
        case "$cidr" in 0.0.0.0/0|::/0|*'..'*) return 1 ;; esac
    done < "$route_file"
}
private_routes_apply() {
    route_file=$1
    private_routes_validate "$route_file" || return 1
    # CIDRs are an allowlist for connected private networks, not gateways.
    # DHCP/connected routes remain kernel-owned; continuously remove only
    # default routes so this interface can never create an Internet path.
    . "$control/boot/common.sh" || return 1
    jl_local_network_policy
}
private_routes_json() {
    route_file=$1 first=1
    printf '{"cidrs":['
    if [ -f "$route_file" ]; then
        while IFS= read -r cidr; do
            [ "$first" = 1 ] || printf ','
            printf '"%s"' "$cidr"
            first=0
        done < "$route_file"
    fi
    printf ']}\n'
}

# Fixed interface names only: never interpolate request data into a command or
# pathname. Missing utilities/driver fields are represented as JSON null.
net_counter() {
    value=
    [ -r "$1" ] && IFS= read -r value < "$1" || :
    case "$value" in ''|*[!0-9]*|?????????????????????*) printf null ;; *)
        # Decimal strings are intentionally quoted (a JS Number loses precision).
        printf '"%s"' "$value" ;; esac
}
network_quality() {
    LC_ALL=C
    export LC_ALL
    net_sys=${JL_NET_SYSFS:-/sys/class/net}
    net_wireless=${JL_NET_WIRELESS:-/proc/net/wireless}
    # Only status (never get_network/list_network) is queried: those other
    # commands can return the PSK. Permit SSID only when associated, convert
    # non-printable and JSON-significant characters to ?, and cap its length.
    status=$(wpa_cli -iwlan0 status 2>/dev/null || :)
    wifi_state=$(printf '%s\n' "$status" | awk -F= '$1=="wpa_state"{print $2;exit}')
    wifi_ssid=null
    if [ "$wifi_state" = COMPLETED ]; then
        wifi_associated=true
        wifi_ssid=$(printf '%s\n' "$status" | awk '
            /^ssid=/ { s=substr($0,6,64); for(i=1;i<=length(s);i++) {
                c=substr(s,i,1)
                if (c !~ /^[A-Za-z0-9 .,_-]$/) c="?"
                out=out c
            } if (length(out)) printf "\"%s\"",out; exit }')
        [ -n "$wifi_ssid" ] || wifi_ssid=null
    elif [ -n "$wifi_state" ]; then
        wifi_associated=false
    else
        wifi_associated=null
    fi
    # iwconfig is optional on OEM builds. Parse only the rate and dBm signal,
    # never its ESSID, access-point MAC, IP address, or encryption metadata.
    radio=$(iwconfig wlan0 2>/dev/null | awk '
        /Signal level=/ {s=$0;sub(/^.*Signal level=/,"",s);split(s,a," ");if(a[1] ~ /^-[0-9]+$/) sig=a[1]}
        /Bit Rate=/ {s=$0;sub(/^.*Bit Rate=/,"",s);split(s,a," ");if(a[1] ~ /^[0-9]+([.][0-9]+)?$/ && a[2]=="Mb/s")rate=a[1]}
        END {printf "%s:%s",sig,rate}')
    net_signal=${radio%%:*}
    net_rate=${radio#*:}
    # /proc/net/wireless reports dBm on many drivers; invalid/missing data
    # stays unknown rather than pretending a positive quality is a dBm value.
    if [ -z "$net_signal" ] && [ -r "$net_wireless" ]; then
        net_signal=$(awk '$1=="wlan0:" {v=$4;sub(/[.]$/,"",v);if(v ~ /^-[0-9]+$/)print v;exit}' "$net_wireless" 2>/dev/null || :)
    fi
    case "$net_signal" in -[0-9]* ) case "${net_signal#-}" in *[!0-9]*|'') net_signal=null ;; esac ;; *) net_signal=null ;; esac
    [ "${#net_signal}" -le 4 ] || net_signal=null
    [ "$net_signal" = null ] || net_signal=$(awk -v n="$net_signal" 'BEGIN {if(n+0<0)printf "%d",n+0;else printf "null"}')
    case "$net_rate" in ''|*[!0-9.]*|*.*.*|.*|*.) net_rate=null ;; esac
    if [ "$net_rate" != null ]; then
        if [ "${#net_rate}" -le 8 ]; then
            net_rate=$(awk -v n="$net_rate" 'BEGIN {if(n+0>0)printf "%.3f",n+0;else printf "null"}')
        else net_rate=null; fi
    fi
    [ "$wifi_associated" = true ] || { wifi_ssid=null; net_signal=null; net_rate=null; }
    carrier=
    [ -r "$net_sys/eth0/carrier" ] && IFS= read -r carrier < "$net_sys/eth0/carrier" || :
    case "$carrier" in 1) link=true ;; 0) link=false ;; *) link=null ;; esac
    speed=
    [ "$link" = true ] && [ -r "$net_sys/eth0/speed" ] && IFS= read -r speed < "$net_sys/eth0/speed" || :
    case "$speed" in ''|*[!0-9]*|???????*) speed=null ;; esac
    [ "$speed" = null ] || speed=$(awk -v n="$speed" 'BEGIN {if(n+0>0)printf "%d",n+0;else printf "null"}')
    # Band from wpa_supplicant's frequency (iwconfig reports nonsense here);
    # the gateway the platform recorded for the network watchdog.
    wifi_freq=$(printf '%s\n' "$status" | awk -F= '$1=="freq"{print $2;exit}')
    case "$wifi_freq" in ''|*[!0-9]*) wifi_freq=null; wifi_band=null ;;
        *) if [ "$wifi_freq" -ge 4900 ]; then wifi_band='"5 GHz"'; else wifi_band='"2.4 GHz"'; fi ;; esac
    wifi_addr=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    case "$wifi_addr" in ''|*[!0-9.]*) wifi_addr=null ;; *) wifi_addr="\"$wifi_addr\"" ;; esac
    wifi_gw=null
    if [ -r "$run/default-gateway" ] && read -r gw_addr _gw_dev < "$run/default-gateway"; then
        case "$gw_addr" in *[!0-9.]*|'') : ;; *) wifi_gw="\"$gw_addr\"" ;; esac
    fi
    printf '{"wifi":{"associated":%s,"ssid":%s,"signal_dbm":%s,"rate_mbps":%s,"frequency_mhz":%s,"band":%s,"address":%s,"gateway":%s,"rx_bytes":%s,"tx_bytes":%s,"rx_packets":%s,"tx_packets":%s},' \
        "$wifi_associated" "$wifi_ssid" "$net_signal" "$net_rate" "$wifi_freq" "$wifi_band" "$wifi_addr" "$wifi_gw" \
        "$(net_counter "$net_sys/wlan0/statistics/rx_bytes")" "$(net_counter "$net_sys/wlan0/statistics/tx_bytes")" \
        "$(net_counter "$net_sys/wlan0/statistics/rx_packets")" "$(net_counter "$net_sys/wlan0/statistics/tx_packets")"
    printf '"ethernet":{"link":%s,"speed_mbps":%s,"rx_bytes":%s,"tx_bytes":%s,"rx_packets":%s,"tx_packets":%s}}\n' \
        "$link" "$speed" "$(net_counter "$net_sys/eth0/statistics/rx_bytes")" "$(net_counter "$net_sys/eth0/statistics/tx_bytes")" \
        "$(net_counter "$net_sys/eth0/statistics/rx_packets")" "$(net_counter "$net_sys/eth0/statistics/tx_packets")"
}

# SSIDs leave only as printable, JSON-safe text (as network_quality does).
json_ssid() {
    awk '{ s=substr($0,1,64); out=""; for(i=1;i<=length(s);i++) { c=substr(s,i,1)
           if (c !~ /^[A-Za-z0-9 .,_-]$/) c="?"; out=out c }
           if (length(out)) printf "\"%s\"",out; else printf "null" }'
}

media_root=${JL_MEDIA_ROOT:-/opt/jooanipc-open}
# A reboot asked for here: keep the clock's time, which is otherwise saved only
# every half hour and has no battery behind it, and tell the next boot this was no
# failure of the media layer.
reboot_asked() {
    now=$(date -u +%s 2>/dev/null) || now=
    saved=$(cat "$root/state/last-time" 2>/dev/null) || saved=0
    case "$saved" in ''|*[!0-9]*) saved=0 ;; esac
    case "$now" in ''|*[!0-9]*) ;; *)
        if [ "$now" -gt "$saved" ] && printf '%s\n' "$now" > "/tmp/jl-set-time.$$" 2>/dev/null; then
            mv -f "/tmp/jl-set-time.$$" "$root/state/last-time" 2>/dev/null || rm -f "/tmp/jl-set-time.$$"
        fi ;;
    esac
    # printf, not ":": a failed redirection on a special builtin would end the shell before the reboot.
    boot_now=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null) || boot_now=
    [ ! -d "$media_root" ] || printf '%s\n' "$boot_now" 2>/dev/null > "$media_root/reboot-requested" || :
    sync
    reboot
}
# Refuse a restart while anything must not be interrupted: maintenance (an
# update or a media install holds the state lock, and a platform update does not
# hold it throughout), a Wi-Fi trial, a boot trial of a new runtime, or no runtime
# on flash at all: this one runs from RAM, and the next boot would have no network.
# Prints the reason, or nothing when it is safe.
busy_blocker() {
    [ ! -d "$run/state.lock" ] && [ ! -d "$run/firmware-apply.lock" ] || { echo update; return; }
    [ ! -e "$root/state/wifi-trial" ] || { echo wifi-trial; return; }
    if [ -r "$root/state/selection" ] && read -r sel_stable sel_pending _x < "$root/state/selection"; then
        [ "${sel_pending:--}" = - ] || { echo boot-trial; return; }
        [ "${sel_stable:--}" != - ] || { echo no-runtime; return; }
    fi
}
# ...and while this boot is still proving itself. The page is up well before that
# ends. Bounded by the uptime, because a failed trial's marker can stay for good.
restart_blocker() {
    up=$(cut -d. -f1 "${JL_PROC_UPTIME:-/proc/uptime}" 2>/dev/null) || up=0
    case "$up" in ''|*[!0-9]*) up=0 ;; esac
    if [ "$up" -lt 900 ] && { [ -e "$media_root/native-trial" ] || [ -e "$media_root/boot-trial" ]; }; then
        echo starting; return
    fi
    busy_blocker
}

case "$op" in
    restart)
        blocker=$(restart_blocker)
        if [ -n "$blocker" ]; then printf '{"ok":false,"reason":"%s"}\n' "$blocker"; exit 0; fi
        printf '{"ok":true}\n'
        # The reply goes out first; the helper's process group is only killed
        # on timeout, so this survives the helper exiting.
        ( trap '' HUP; sleep 2; reboot_asked ) </dev/null >/dev/null 2>&1 &
        ;;
    firmware-status)
        # What the last platform install came to, until the restart clears it.
        latest=$(ls -t "$run"/firmware.*.status 2>/dev/null | head -n 1)
        if [ -z "$latest" ]; then printf '{"state":"idle"}\n'; exit 0; fi
        state=$(sed -n 's/^state=\([a-z-]*\)$/\1/p' "$latest" | head -n 1)
        reason=$(sed -n 's/^reason=\([a-z-]*\)$/\1/p' "$latest" | head -n 1)
        restart=$(sed -n 's/^restart=\([a-z-]*\)$/\1/p' "$latest" | head -n 1)
        printf '{"state":"%s","reason":"%s","restart":"%s"}\n' "${state:-unknown}" "$reason" "$restart"
        ;;
    restart-check)
        # What a Wi-Fi trial asks first: a rollback would cut this boot short.
        blocker=$(restart_blocker)
        if [ -n "$blocker" ]; then printf '{"ok":false,"reason":"%s"}\n' "$blocker"; else printf '{"ok":true}\n'; fi
        ;;
    clock-known)
        # The clock is right, whether or not it needed a step. Schedules wait for this.
        printf '' > "$run/clock-set"
        ;;
    factory-reset)
        # Reset to defaults, from the admin UI or the reset button: mark it and
        # restart. The next boot erases, before anything owning those files runs
        # (jl_apply_reset in common.sh), so a power cut cannot leave half of it.
        # Unlike a restart this is allowed during a boot's trial: someone holding
        # the button just after power-on means it, and it latches nothing.
        blocker=$(busy_blocker)
        if [ -n "$blocker" ]; then printf '{"ok":false,"reason":"%s"}\n' "$blocker"; exit 0; fi
        : > "$root/state/reset-pending"
        sync
        printf '{"ok":true}\n'
        ( trap '' HUP; sleep 2; reboot_asked ) </dev/null >/dev/null 2>&1 &
        ;;
    restart-schedule)
        at=-
        [ ! -r "$root/config/restart-schedule" ] || IFS= read -r at < "$root/config/restart-schedule" || at=-
        case "$at" in [0-2][0-9]:[0-5][0-9]) printf '{"enabled":true,"time":"%s"}\n' "$at" ;;
            *) printf '{"enabled":false,"time":null}\n' ;; esac
        ;;
    restart-schedule-set)
        case "$id" in
            off) rm -f "$root/config/restart-schedule" ;;
            [0-2][0-9]:[0-5][0-9])
                [ "${id%%:*}" -le 23 ] || exit 2
                mkdir -p "$root/config" && chmod 700 "$root/config"
                printf '%s\n' "$id" > "$root/config/restart-schedule.new" &&
                    chmod 600 "$root/config/restart-schedule.new" &&
                    mv -f "$root/config/restart-schedule.new" "$root/config/restart-schedule" || exit 1 ;;
            *) exit 2 ;;
        esac
        sync
        exec "$0" restart-schedule
        ;;
    wifi-scan)
        # Ask for a fresh scan, then list the strongest entry per network: only
        # names, signal, band and security. A scan takes 5 to 7 seconds on this
        # chip and the table changes all at once when it ends, so wait for that
        # (the old table is what comes back before then). The caller allows 15 s.
        scan_table() { wpa_cli -iwlan0 scan_results 2>/dev/null | awk -F'\t' 'NR>1{print $1,$3}'; }
        before=$(scan_table)
        wpa_cli -iwlan0 scan >/dev/null 2>&1 || :
        tries=0
        while [ "$tries" -lt 10 ]; do
            sleep 1
            tries=$((tries + 1))
            [ "$(scan_table)" = "$before" ] || break
        done
        current=$(wpa_cli -iwlan0 status 2>/dev/null | awk -F= '$1=="ssid"{print $2;exit}')
        wpa_cli -iwlan0 scan_results 2>/dev/null | awk -F'\t' 'NR>1 && NF>=5 && $5!="" {
            if (!($5 in best) || $3+0 > sig[$5]) { best[$5]=$2; sig[$5]=$3+0; sec[$5]=($4 ~ /WPA|WEP|RSN/) } }
            END { for (s in best) printf "%s\t%s\t%s\t%s\n", sig[s], best[s], sec[s], s }' |
            sort -rn | head -n 30 | {
                printf '['; first=1
                while IFS="$(printf '\t')" read -r sig freq secure ssid; do
                    name=$(printf '%s\n' "$ssid" | json_ssid)
                    [ "$name" != null ] || continue
                    use=false; [ "$ssid" != "$current" ] || use=true
                    [ "$first" = 1 ] || printf ','; first=0
                    case "$sig" in -[0-9]*|[0-9]*) : ;; *) sig=null ;; esac
                    case "$freq" in *[!0-9]*|'') freq=null ;; esac
                    printf '{"ssid":%s,"signal_dbm":%s,"frequency_mhz":%s,"secure":%s,"in_use":%s}' \
                        "$name" "$sig" "$freq" "$([ "$secure" = 1 ] && echo true || echo false)" "$use"
                done
                printf ']\n'
            }
        ;;
    wifi-state)
        # The Wi-Fi trial as the countdown sees it: seconds left, the proposed
        # network, and whether the camera is on it with an address.
        if [ -d "$root/state/wifi-trial" ]; then
            deadline=0; IFS= read -r deadline < "$root/state/wifi-trial/deadline" || :
            now=$(cut -d. -f1 /proc/uptime)
            case "$deadline:$now" in *[!0-9:]*|:*|*:) left=null ;; *) left=$((deadline - now)); [ "$left" -ge 0 ] || left=0 ;; esac
            ssid=$(sed -n 's/.*"ssid"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$root/state/wifi-trial/candidate.json" 2>/dev/null | head -n 1 | json_ssid)
            gw=$(sed -n 's/.*"gateway"[[:space:]]*:[[:space:]]*"\([0-9.]*\)".*/\1/p' "$root/state/wifi-trial/candidate.json" 2>/dev/null) || gw=
            if [ -z "$gw" ] && [ -r "$run/default-gateway" ]; then read -r gw _gw_dev < "$run/default-gateway" || gw=; fi
            case "$gw" in ''|*[!0-9.]*) gw=null ;; *) gw="\"$gw\"" ;; esac
            status=$(wpa_cli -iwlan0 status 2>/dev/null || :)
            joined=false; printf '%s\n' "$status" | grep -qx 'wpa_state=COMPLETED' && joined=true
            addr=$(printf '%s\n' "$status" | awk -F= '$1=="ip_address"{print $2;exit}')
            case "$addr" in *[!0-9.]*|'') addr=null ;; *) addr="\"$addr\"" ;; esac
            static=$(sed -n 's/.*"address"[[:space:]]*:[[:space:]]*"\([0-9.]*\)".*/\1/p' "$root/state/wifi-trial/candidate.json" 2>/dev/null | head -n 1)
            case "$static" in '') static=null ;; *) static="\"$static\"" ;; esac
            printf '{"pending":true,"seconds_left":%s,"ssid":%s,"joined":%s,"address":%s,"static":%s,"gateway":%s}\n' "$left" "${ssid:-null}" "$joined" "$addr" "$static" "$gw"
        else
            last=null
            [ ! -e "$root/state/wifi-committed" ] || last='"committed"'
            [ ! -e "$root/state/wifi-rolled-back" ] || last='"rolled_back"'
            printf '{"pending":false,"last":%s}\n' "$last"
        fi
        ;;
    ip-get)
        # How the camera gets its address, and what it has now.
        mode=dhcp
        ! grep -q '"address"' "$root/config/wifi.json" 2>/dev/null || mode=static
        line=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*Mask:\([0-9.]*\).*/\1 \2/p')
        addr=${line%% *} mask=${line##* } bits=null
        case "$addr" in ''|*[!0-9.]*) addr=null ;; *)
            bits=$(printf '%s\n' "$mask" | awk -F. '{ n = 0; for (i = 1; i <= 4; i++) { v = $i + 0; while (v > 0) { n += v % 2; v = int(v / 2) } } print n }')
            addr="\"$addr\"" ;; esac
        gw=null
        if [ -r "$run/default-gateway" ] && read -r gw_addr _gw_dev < "$run/default-gateway"; then
            case "$gw_addr" in *[!0-9.]*|'') : ;; *) gw="\"$gw_addr\"" ;; esac
        fi
        printf '{"mode":"%s","address":%s,"prefix":%s,"gateway":%s}\n' "$mode" "$addr" "$bits" "$gw"
        ;;
    ip-stage)
        # $path: "dhcp" or "static <address> <prefix> <gateway>", checked by the
        # daemon. The same 90 s trial as a new network, without touching the link.
        # Whatever can be found out first is, so a mistake costs nothing: an address
        # that something else answers on, or a gateway that does not answer.
        blocker=$(restart_blocker)
        if [ -n "$blocker" ]; then printf '{"ok":false,"reason":"%s"}\n' "$blocker"; exit 0; fi
        wifi_json=$root/config/wifi.json
        if [ ! -f "$wifi_json" ]; then printf '{"ok":false,"reason":"no-network"}\n'; exit 0; fi
        address='' prefix='' gateway=''
        read -r mode address prefix gateway < "$path" || exit 1
        if [ "$mode" = static ]; then
            current=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
            if [ "$address" != "$current" ] && ! arping -D -q -c 2 -w 3 -I wlan0 "$address" >/dev/null 2>&1; then
                printf '{"ok":false,"reason":"address-in-use"}\n'; exit 0
            fi
            if ! arping -f -q -c 3 -w 3 -I wlan0 "$gateway" >/dev/null 2>&1; then
                printf '{"ok":false,"reason":"gateway-silent"}\n'; exit 0
            fi
        fi
        json_tool=${JOAN_JSON_TOOL:-/mnt/mtd/run/json_debug}
        ssid=$("$json_tool" -c r -k /ssid "$wifi_json" 2>/dev/null | sed 's/^[^ ]* [^ ]* //')
        password=$("$json_tool" -c r -k /password "$wifi_json" 2>/dev/null | sed 's/^[^ ]* [^ ]* //')
        [ -n "$ssid" ] && [ -n "$password" ] || exit 1
        candidate=$run/ip-candidate.json
        rm -f "$candidate"
        ( umask 077
          { printf '{"ssid":"%s","password":"%s"' "$ssid" "$password"
            [ "$mode" != static ] || printf ',"address":"%s","prefix":"%s","gateway":"%s"' "$address" "$prefix" "$gateway"
            printf '}\n'; } > "$candidate" )
        rc=0
        JOAN_WIFI_KEEP_LINK=1 "$control/admin/wifi-transaction.sh" apply "$candidate" >/dev/null 2>&1 || rc=$?
        rm -f "$candidate"
        [ "$rc" = 0 ] || exit 1
        printf '{"ok":true}\n'
        ;;
    network-quality) network_quality ;;
    wifi-stage)
        # A new network only. An address comes through ip-stage, which checks it.
        if grep -q -e '"address"' -e '"prefix"' -e '"gateway"' "$path"; then exit 1; fi
        exec "$control/admin/wifi-transaction.sh" apply "$path" ;;
    wifi-commit) exec "$control/admin/wifi-transaction.sh" commit ;;
    wifi-rollback) exec "$control/admin/wifi-transaction.sh" rollback ;;
    ssh-list)
        [ ! -f "$root/config/ssh/authorized_keys" ] || cat "$root/config/ssh/authorized_keys"
        ;;
    ssh-add)
        key=$(cat "$path")
        case "$key" in ssh-ed25519\ *) ;; *) exit 1 ;; esac
        # One line: a second one would be a key of its own, past the limit of 8.
        [ "$(printf '%s' "$key" | wc -l)" -eq 0 ] || exit 1
        case "$key" in *"$(printf '\r')"*) exit 1 ;; esac
        mkdir -p "$root/config/ssh"
        chmod 700 "$root/config/ssh"
        keys=$root/config/ssh/authorized_keys
        # Up to 8 keys; adding a key already present changes nothing.
        blob=$(printf '%s\n' "$key" | awk '{print $2}')
        if [ -f "$keys" ] && awk -v b="$blob" '$2==b{f=1} END{exit !f}' "$keys"; then exit 0; fi
        [ ! -f "$keys" ] || [ "$(grep -c . "$keys")" -lt 8 ] || exit 1
        # grep fails on an empty list, which is what removing every key leaves.
        { [ ! -f "$keys" ] || grep . "$keys" || :; printf '%s\n' "$key"; } > "$keys.new"
        chmod 600 "$keys.new"
        mv "$keys.new" "$keys"
        sync
        ;;
    ssh-delete)
        # id: "all", or the 1-based line of the key to remove.
        keys=$root/config/ssh/authorized_keys
        case "$id" in
            all) : ;;
            [1-8]) [ -f "$keys" ] || exit 1
                   grep . "$keys" | awk -v n="$id" 'NR!=n' > "$keys.new" || :
                   chmod 600 "$keys.new"; mv "$keys.new" "$keys"; sync; exit 0 ;;
            *) exit 2 ;;
        esac
        : > "$root/config/ssh/authorized_keys.new"
        chmod 600 "$root/config/ssh/authorized_keys.new"
        mv "$root/config/ssh/authorized_keys.new" "$root/config/ssh/authorized_keys"
        sync
        ;;
    routes-list)
        route_file=$root/config/routes.list
        private_routes_apply "$route_file" || exit 1
        private_routes_json "$route_file"
        ;;
    routes-set)
        [ -f "$path" ] || exit 2
        private_routes_validate "$path" || exit 1
        mkdir -p "$root/config"
        chmod 700 "$root/config"
        cp "$path" "$root/config/routes.list.new"
        chmod 600 "$root/config/routes.list.new"
        mv -f "$root/config/routes.list.new" "$root/config/routes.list"
        sync
        private_routes_apply "$root/config/routes.list" || exit 1
        private_routes_json "$root/config/routes.list"
        ;;
    time-set)
        # id is the UTC epoch; the daemon has already range-checked it, but keep
        # the helper defensive since it runs privileged.
        case "$id" in ''|*[!0-9]*) exit 2 ;; esac
        # No RTC: persist so this client-set time survives a reboot. Written
        # BEFORE the clock moves -- a large forward jump can expire the caller's
        # deadline instantly, so work queued after date -s may never run.
        mkdir -p "$root/state" 2>/dev/null || :
        if printf '%s\n' "$id" > /tmp/jl-set-time.$$ 2>/dev/null; then
            mv -f /tmp/jl-set-time.$$ "$root/state/last-time" 2>/dev/null ||
                rm -f /tmp/jl-set-time.$$
        fi
        date -s "@$id" >/dev/null 2>&1 || exit 1
        printf '' 2>/dev/null > "$run/clock-set" || :
        ;;
    timezone-set)
        # id is a "GMT+HH:MM" offset; the daemon validated it, re-checked here
        # since we run privileged. jooanipc burns the OSD clock from
        # /IpcParam/TimeZomeCfg/GmtTz, read at startup, so this write is
        # persistent and the overlay picks it up on the next reboot. json_debug
        # is the OEM's own in-place config writer.
        case "$id" in GMT[+-][0-9][0-9]:[0-9][0-9]) : ;; *) exit 2 ;; esac
        tz_cfg=/opt/conf/config.json
        tz_jd=/mnt/mtd/run/json_debug
        [ -f "$tz_cfg" ] && [ -x "$tz_jd" ] || exit 1
        LD_LIBRARY_PATH=/mnt/mtd/lib:/mnt/mtd/run "$tz_jd" -i -c w \
            -k /IpcParam/TimeZomeCfg/GmtTz -v "$id" "$tz_cfg" >/dev/null 2>&1 || exit 1
        sync
        ;;
    firmware-verify)
        [ -f "$path" ] && [ "$id" != - ] || exit 2
        case "$id" in *[!0-9a-f]*|'') exit 2 ;; esac
        [ "${#id}" = 64 ] || exit 2
        inspector=$control/shared/jooan-ironman-inspect
        authenticator=$control/shared/jooan-auth-verify
        [ -x "$inspector" ] && [ -x "$authenticator" ] || exit 1
        inspect=$run/firmware.$id.inspect.new
        payload=$run/firmware.$id.sqfs.new
        mounted=$run/firmware.$id.verify-mount
        trap 'firmware_unmount "$mounted" 2>/dev/null || :; rm -f "$inspect" "$payload"' EXIT HUP INT TERM
        "$inspector" "$path" > "$inspect" || exit 1
        payload_offset=$(sed -n 's/^payload_offset=//p' "$inspect")
        payload_length=$(sed -n 's/^payload_length=//p' "$inspect")
        package_length=$(sed -n 's/^package_length=//p' "$inspect")
        model_token=$(sed -n 's/^model_token=//p' "$inspect")
        package_sha256=$(sed -n 's/^package_sha256=//p' "$inspect")
        case "$payload_offset:$payload_length:$package_length" in *[!0-9:]*) exit 1 ;; esac
        [ "$payload_offset" = 96 ] && [ "$model_token" = A12 ] &&
            [ "$package_sha256" = "$id" ] &&
            [ "$package_length" = "$(wc -c < "$path")" ] || exit 1
        dd if="$path" bs=96 skip=1 2>/dev/null | \
            head -c "$payload_length" > "$payload" || exit 1
        [ "$(wc -c < "$payload")" = "$payload_length" ] || exit 1
        firmware_mount "$payload" "$mounted" || exit 1
        "$authenticator" "$mounted/release.manifest" \
            "$mounted/release.manifest.sig" "$mounted" || exit 1
        firmware_metadata "$mounted/release.manifest" || exit 1
        [ "$(cat "$mounted/RELEASE")" = "$release_version" ] || exit 1
        if [ "$artifact_kind" = install ] && [ -f "$root/state/release-sequence" ]; then
            installed=$(cat "$root/state/release-sequence") || exit 1
            case "$installed" in ''|*[!0-9]*) exit 1 ;; esac
            [ "$release_sequence" -gt "$installed" ] || exit 1
        fi
        firmware_unmount "$mounted" || exit 1
        mv -f "$payload" "$run/firmware.$id.sqfs"
        mv -f "$inspect" "$run/firmware.$id.inspect"
        {
            printf 'package_sha256=%s\n' "$id"
            printf 'release_version=%s\n' "$release_version"
            printf 'release_sequence=%s\n' "$release_sequence"
            printf 'artifact_kind=%s\n' "$artifact_kind"
            printf 'state=verified\n'
        } > "$run/firmware.$id.verified.new"
        mv -f "$run/firmware.$id.verified.new" "$run/firmware.$id.verified"
        trap - EXIT HUP INT TERM
        ;;
    firmware-apply)
        [ "$id" != - ] || exit 2
        case "$id" in *[!0-9a-f]*|'') exit 2 ;; esac
        [ "${#id}" = 64 ] || exit 2
        image=$run/staging/firmware/$id.bin
        payload=$run/firmware.$id.sqfs
        marker=$run/firmware.$id.verified
        inspector=$control/shared/jooan-ironman-inspect
        authenticator=$control/shared/jooan-auth-verify
        [ -f "$image" ] && [ -f "$payload" ] && [ -f "$marker" ] || exit 1
        [ "$(sed -n 's/^package_sha256=//p' "$marker")" = "$id" ] || exit 1
        current=$run/firmware.$id.apply-inspect
        "$inspector" "$image" > "$current" || exit 1
        [ "$(sed -n 's/^package_sha256=//p' "$current")" = "$id" ] || exit 1
        rm -f "$current"
        lock=$run/firmware-apply.lock
        mkdir "$lock" 2>/dev/null || exit 1
        status=$run/firmware.$id.status
        printf 'state=launching\n' > "$status"
        (
            apply_mount=$run/firmware.$id.apply-mount
            cleanup_apply() {
                firmware_unmount "$apply_mount" 2>/dev/null || :
                rm -rf "$lock" 2>/dev/null || :
            }
            trap cleanup_apply EXIT HUP INT TERM
            if ! firmware_mount "$payload" "$apply_mount"; then
                printf 'state=failed\nreason=mount\n' > "$status"
                exit 1
            fi
            if ! "$authenticator" "$apply_mount/release.manifest" \
                "$apply_mount/release.manifest.sig" "$apply_mount"; then
                printf 'state=failed\nreason=signature\n' > "$status"
                exit 1
            fi
            if ! firmware_metadata "$apply_mount/release.manifest"; then
                printf 'state=failed\nreason=metadata\n' > "$status"
                exit 1
            fi
            if [ "$artifact_kind" = install ] && [ -f "$root/state/release-sequence" ]; then
                installed=$(cat "$root/state/release-sequence" 2>/dev/null || :)
                case "$installed" in ''|*[!0-9]*)
                    printf 'state=failed\nreason=installed-sequence\n' > "$status"; exit 1 ;;
                esac
                if [ "$release_sequence" -le "$installed" ]; then
                    printf 'state=failed\nreason=replay\n' > "$status"
                    exit 1
                fi
            fi
            printf 'state=running\n' > "$status"
            if /bin/sh "$apply_mount/upgrade.sh" "$image"; then
                printf 'state=complete\n' > "$status"
                # A media flash keeps this lock until its own reboot, and one that
                # lands in the middle of its write costs the camera. Holding the lock
                # from here keeps a new one from starting.
                i=0
                until mkdir "$run/state.lock" 2>/dev/null; do
                    # Still held after this long means a flash that failed and is being
                    # looked after: restarting could lose the camera. It is left to the owner.
                    if [ "$i" -ge 300 ]; then
                        printf 'state=complete\nrestart=needed\n' > "$status"
                        exit 0
                    fi
                    sleep 1; i=$((i + 1))
                done
                reboot_asked
            else
                rc=$?
                printf 'state=failed\nreason=upgrade\nexit=%s\n' "$rc" > "$status"
                exit "$rc"
            fi
        ) </dev/null >>"$run/firmware.$id.apply.log" 2>&1 &
        apply_pid=$!
        printf '%s\n' "$apply_pid" > "$run/firmware.$id.apply.pid"
        sleep 1
        if ! kill -0 "$apply_pid" 2>/dev/null; then
            wait "$apply_pid" || exit 1
        fi
        ;;
    *) exit 2 ;;
esac
