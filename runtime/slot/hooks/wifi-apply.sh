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

# At boot this hook runs from start.sh before wpa_supplicant has created its
# control socket -- OEM boot starts it about 54s in on the verified unit.
# Every wpa_cli call would then fail instantly with "Failed to connect to
# non-global ctrl_ifname", the hook would exit, and the committed network
# would silently never be applied. Wait, bounded, for the control interface
# to answer. start.sh backgrounds this hook so the wait never delays the
# daemon, and on the live API path wpa_supplicant is already serving, so it
# answers on the first probe and this costs nothing there.
wpa_ready=0
i=0
while [ "$i" -lt 150 ]; do
    if wpa_cli -iwlan0 ping 2>/dev/null | grep -q PONG; then wpa_ready=1; break; fi
    sleep 1
    i=$((i + 1))
done
[ "$wpa_ready" = 1 ] || exit 1

# Prefer this SSID's 5 GHz BSS when a pre-connect scan finds one strong
# enough for sustained video, falling back to 2.4 GHz otherwise (weak/absent
# 5 GHz, a 2.4 GHz-only AP, or no usable scan). Two network blocks share the
# SSID/PSK; each is restricted to one band by freq_list, and the block that
# matches what the scan supports gets the higher priority. Both stay
# enabled (not select_network) so wpa_supplicant can fall through to the
# lower-priority block if the preferred one cannot associate.
# -70 dBm is a conservative usable-for-streaming floor; a missing or failed
# scan counts as "no usable 5 GHz" rather than blocking the join.
JOAN_5G_MIN_DBM=${JOAN_5G_MIN_DBM:--70}
best5g=""
if command -v iwlist >/dev/null 2>&1; then
    best5g=$(iwlist wlan0 scan 2>/dev/null | awk -v want="$ssid" '
        /^ *Cell [0-9]+/ { ch = ""; sig = "" }
        /Channel [0-9]+\)/ {
            if (match($0, /Channel [0-9]+\)/)) {
                seg = substr($0, RSTART, RLENGTH)
                gsub(/[^0-9]/, "", seg)
                ch = seg + 0
            }
        }
        /Signal level=-?[0-9]+ dBm/ {
            if (match($0, /Signal level=-?[0-9]+ dBm/)) {
                seg = substr($0, RSTART, RLENGTH)
                gsub(/Signal level=/, "", seg)
                gsub(/ dBm/, "", seg)
                sig = seg + 0
            }
        }
        /ESSID:/ {
            essid = $0
            sub(/^.*ESSID:"/, "", essid)
            sub(/".*$/, "", essid)
            if (essid == want && ch >= 36 && sig != "") {
                if (best == "" || sig > best) best = sig
            }
        }
        END { if (best != "") print best }
    ')
    case "$best5g" in ''|*[!-0-9]*) best5g="" ;; esac
fi
prefer_5g=0
[ -n "$best5g" ] && [ "$best5g" -ge "$JOAN_5G_MIN_DBM" ] && prefer_5g=1

FREQ_5G="5180 5200 5220 5240 5260 5280 5300 5320 5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 5720 5745 5765 5785 5805 5825"
FREQ_24G="2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472"

add_band() {
    # add_band <freq-list> <priority>
    band_id=$(wpa_cli -iwlan0 add_network | awk '/^[0-9]+$/{print;exit}')
    case "$band_id" in ''|*[!0-9]*) return 1 ;; esac
    wpa_cli -iwlan0 set_network "$band_id" ssid "\"$ssid\"" | grep -q OK || return 1
    wpa_cli -iwlan0 set_network "$band_id" psk "\"$password\"" | grep -q OK || return 1
    wpa_cli -iwlan0 set_network "$band_id" freq_list "$1" | grep -q OK || return 1
    wpa_cli -iwlan0 set_network "$band_id" priority "$2" | grep -q OK || return 1
    # Let the OEM wpa_supplicant 2.9 select protocol and ciphers from the BSS.
    # Explicit RSN/CCMP constraints leave this SKW6316 build stuck in
    # SCANNING, including against otherwise compatible WPA2 bench APs.
    wpa_cli -iwlan0 enable_network "$band_id" | grep -q OK || return 1
}

if [ "$prefer_5g" = 1 ]; then
    add_band "$FREQ_5G" 2 || exit 1
    add_band "$FREQ_24G" 1 || exit 1
else
    add_band "$FREQ_24G" 2 || exit 1
    add_band "$FREQ_5G" 1 || exit 1
fi
wpa_cli -iwlan0 reassociate | grep -q OK

i=0
while [ "$i" -lt 18 ]; do
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
