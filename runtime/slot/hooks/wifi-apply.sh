#!/bin/sh
set -eu
[ "$#" = 1 ] && [ -f "$1" ] || exit 2
candidate=$1
json_tool=${JOAN_JSON_TOOL:-/mnt/mtd/run/json_debug}
[ -x "$json_tool" ] || exit 1
ssid=$("$json_tool" -c r -k /ssid "$candidate" 2>/dev/null |
    sed 's/^[^ ]* [^ ]* //')
password=$("$json_tool" -c r -k /password "$candidate" 2>/dev/null |
    sed 's/^[^ ]* [^ ]* //')
[ -n "$ssid" ] && [ -n "$password" ] || exit 1
# Values are passed as one argv element to wpa_cli; they are never evaluated as
# shell.  Accept ordinary printable passphrases and SSIDs (including spaces),
# but reject the two characters that would alter wpa_supplicant's quoted-string
# grammar and reject control/non-ASCII bytes.
LC_ALL=C
export LC_ALL
case "$ssid$password" in *'"'*|*'\'*|*[!\ -~]*) exit 1 ;; esac
[ "${#ssid}" -le 32 ] && [ "${#password}" -ge 8 ] && [ "${#password}" -le 63 ] || exit 1
id=$(wpa_cli -iwlan0 add_network | awk '/^[0-9]+$/{print;exit}')
case "$id" in ''|*[!0-9]*) exit 1 ;; esac
wpa_cli -iwlan0 set_network "$id" ssid "\"$ssid\"" | grep -q OK
wpa_cli -iwlan0 set_network "$id" psk "\"$password\"" | grep -q OK
# Let the OEM wpa_supplicant 2.9 select protocol and ciphers from the BSS.
# Explicit RSN/CCMP constraints leave this SKW6316 build stuck in SCANNING,
# including against otherwise compatible WPA2 bench access points.
wpa_cli -iwlan0 select_network "$id" | grep -q OK
wpa_cli -iwlan0 enable_network "$id" | grep -q OK
i=0
while [ "$i" -lt 15 ]; do
    state=$(wpa_cli -iwlan0 status 2>/dev/null | awk -F= '$1=="wpa_state"{print $2}')
    [ "$state" = COMPLETED ] && break
    sleep 2
    i=$((i + 1))
done
[ "${state:-}" = COMPLETED ] || exit 1
killall udhcpc 2>/dev/null || :
udhcpc -a -n -q -t 5 -T 3 -i wlan0 -x hostname:JooanW3U \
    -p /var/run/udhcpc_wlan0_pid.txt -s /mnt/mtd/run/default.script >/dev/null 2>&1
ifconfig wlan0 2>/dev/null | grep -q 'inet addr:'
