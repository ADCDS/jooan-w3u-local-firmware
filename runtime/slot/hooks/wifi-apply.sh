#!/bin/sh
set -eu
[ "$#" = 1 ] && [ -f "$1" ] || exit 2
candidate=$1
json_tool=/mnt/mtd/run/json_debug
[ -x "$json_tool" ] || exit 1
ssid=$("$json_tool" -c r -k /ssid "$candidate" 2>/dev/null | awk '{print $3}')
password=$("$json_tool" -c r -k /password "$candidate" 2>/dev/null | awk '{print $3}')
[ -n "$ssid" ] && [ -n "$password" ] || exit 1
case "$ssid$password" in *[!A-Za-z0-9_.@+-]*) exit 1 ;; esac
[ "${#ssid}" -le 32 ] && [ "${#password}" -ge 8 ] && [ "${#password}" -le 63 ] || exit 1
id=$(wpa_cli -iwlan0 add_network | awk '/^[0-9]+$/{print;exit}')
case "$id" in ''|*[!0-9]*) exit 1 ;; esac
wpa_cli -iwlan0 set_network "$id" ssid "\"$ssid\"" | grep -q OK
wpa_cli -iwlan0 set_network "$id" psk "\"$password\"" | grep -q OK
wpa_cli -iwlan0 set_network "$id" scan_ssid 1 | grep -q OK
wpa_cli -iwlan0 select_network "$id" | grep -q OK
wpa_cli -iwlan0 enable_network "$id" | grep -q OK
killall udhcpc 2>/dev/null || :
udhcpc -a -i wlan0 -x hostname:JooanW3U -b \
    -p /var/run/udhcpc_wlan0_pid.txt -s /mnt/mtd/run/default.script >/dev/null 2>&1
exit 0
