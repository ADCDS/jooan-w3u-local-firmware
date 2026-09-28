#!/bin/sh
# wifi-deadman.sh -- record why the camera falls off Wi-Fi, then bring it back.
#
# A diagnostic, not part of the firmware: it runs from RAM on the camera and
# never writes flash. Every TICK seconds it appends one line of Wi-Fi state to
# the microSD card, plus the kernel's new messages and a detail block whenever
# the association, band, data path (ARP to the gateway) or the supervisor's
# apply attempts change.
# The SKW6316 does not reassociate by itself once dropped, so when the data path
# to the gateway has been down for DEAD seconds (after having worked at least
# once), it saves a full snapshot, syncs, and reboots the camera.
#
#   ssh admin@camera 'cat > /run/jooan-local/wifi-deadman.sh &&
#       sh /run/jooan-local/wifi-deadman.sh start' < tools/wifi-deadman.sh
#   ... sh /run/jooan-local/wifi-deadman.sh stop | status
#
# Logs: /mnt/sd_card/jooan-local/wifi-<epoch>.log (+ .snapshot on a reboot).
# Only BusyBox applets this camera has (no nohup/setsid: HUP is ignored).
set -u

TICK=${TICK:-3}
DEAD=${DEAD:-150}
GW=${GW:-192.168.20.1}
OUT=${OUT:-/mnt/sd_card/jooan-local}
RUN=/run/jooan-local
PIDFILE=$RUN/wifi-deadman.pid
SELF=$RUN/wifi-deadman.sh

field() { printf '%s\n' "$1" | sed -n "s/^$2=//p"; }

running() {
    pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    [ -d "/proc/$pid" ] && grep -q wifi-deadman "/proc/$pid/cmdline" 2>/dev/null
}

detail() {
    {
        echo "---- detail at uptime $1: $2"
        wpa_cli -iwlan0 status 2>&1
        echo "-- networks"; wpa_cli -iwlan0 list_networks 2>&1
        echo "-- link"; iwconfig wlan0 2>&1; cat /proc/net/wireless 2>&1
        echo "-- addr/route"; ifconfig wlan0 2>&1; route -n 2>&1
        echo "-- dhcp/wpa processes"; ps | grep -E 'udhcpc|wpa_supplicant|wifi-apply|wpa_cli|iwlist' | grep -v grep
        echo "----"
    } >> "$LOG" 2>&1
}

snapshot() {
    snap=$LOG.snapshot
    {
        echo "==== snapshot at uptime $1: $2"; date
        echo "== meminfo"; head -8 /proc/meminfo
        echo "== wpa status"; wpa_cli -iwlan0 status 2>&1
        echo "== networks"; wpa_cli -iwlan0 list_networks 2>&1
        echo "== scan results (cached)"; wpa_cli -iwlan0 scan_results 2>&1
        echo "== iwconfig"; iwconfig 2>&1
        echo "== ifconfig -a"; ifconfig -a 2>&1
        echo "== route"; route -n 2>&1
        echo "== /proc/net/dev"; cat /proc/net/dev
        echo "== ps"; ps
        echo "== lsmod"; lsmod 2>&1
        echo "== supervisor state"; ls -la "$RUN" 2>&1
        echo "== dmesg"; dmesg 2>&1
        echo "== OEM syslog (/tmp/message.log)"; cat /tmp/message.log 2>&1
    } > "$snap" 2>&1
    sync
}

run() {
    trap '' HUP
    echo $$ > "$PIDFILE"
    mkdir -p "$OUT" || exit 1
    LOG=$OUT/wifi-$(date +%s).log
    echo "wifi-deadman start: $(date) uptime $(cut -d' ' -f1 /proc/uptime) tick=$TICK dead=$DEAD gw=$GW" >> "$LOG"
    detail "$(cut -d' ' -f1 /proc/uptime)" start
    kmsg=$(dmesg | wc -l)
    prev='' armed=0 down_since=''
    while :; do
        up=$(cut -d' ' -f1 /proc/uptime); now=${up%.*}
        st=$(wpa_cli -iwlan0 status 2>/dev/null)
        state=$(field "$st" wpa_state); freq=$(field "$st" freq); bssid=$(field "$st" bssid)
        ip=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
        # ARP, not ping: the IoT firewall drops ICMP to the gateway even when
        # the link is fine; an ARP reply proves the Wi-Fi data path itself.
        if arping -c 1 -w 2 -I wlan0 "$GW" >/dev/null 2>&1; then data=ok; else data=down; fi
        sig=$(awk '$1=="wlan0:"{print $4}' /proc/net/wireless 2>/dev/null)
        nets=$(wpa_cli -iwlan0 list_networks 2>/dev/null | awk 'NR>1' | wc -l)
        tries=$(cat "$RUN/wifi-attempts" 2>/dev/null || echo 0)
        applied=no; [ -f "$RUN/wifi-applied" ] && applied=yes
        bytes=$(sed -n 's/^ *wlan0: *//p' /proc/net/dev | awk '{print "rx=" $1 " tx=" $9}')
        echo "$up state=${state:--} freq=${freq:--} bssid=${bssid:--} ip=${ip:--} gw=$data sig=${sig:--} nets=$nets tries=$tries applied=$applied $bytes" >> "$LOG"
        k=$(dmesg | wc -l)
        if [ "$k" -gt "$kmsg" ]; then dmesg | tail -n $((k - kmsg)) | sed 's/^/kernel: /' >> "$LOG"; fi
        [ "$k" -lt "$kmsg" ] && dmesg | tail -n 20 | sed 's/^/kernel: /' >> "$LOG"
        kmsg=$k
        key="$state $freq $bssid $data $nets $tries $applied"
        [ "$key" = "$prev" ] || detail "$up" "$key"
        prev=$key
        if [ "$data" = ok ]; then armed=1; down_since=''
        elif [ "$armed" = 1 ]; then
            [ -n "$down_since" ] || down_since=$now
            if [ $((now - down_since)) -ge "$DEAD" ]; then
                echo "$up deadman: data path down since $down_since; snapshot and reboot" >> "$LOG"
                snapshot "$up" "down since $down_since"
                reboot
                exit 0
            fi
        fi
        sync
        sleep "$TICK"
    done
}

case "${1:-}" in
    start)
        running && { echo "already running: $(cat "$PIDFILE")"; exit 0; }
        [ "$0" = "$SELF" ] || cp "$0" "$SELF"
        sh "$SELF" run </dev/null >/dev/null 2>&1 &
        sleep 1; running && echo "wifi-deadman running: $(cat "$PIDFILE")" || { echo "failed to start"; exit 1; }
        ;;
    stop) running && kill "$(cat "$PIDFILE")" && echo stopped || echo 'not running' ;;
    status) running && echo "running: $(cat "$PIDFILE")" || echo 'not running'
        ls -t "$OUT"/wifi-*.log 2>/dev/null | head -1 | while read -r f; do tail -n 3 "$f"; done ;;
    run) run ;;
    *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
