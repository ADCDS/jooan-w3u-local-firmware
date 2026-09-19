#!/bin/sh
set -eu
[ "$#" = 1 ] || exit 2
backup=$1
mkdir -p "$backup"
if [ -f /opt/custom/jooan-local/config/wifi.json ]; then
    cp /opt/custom/jooan-local/config/wifi.json "$backup/wifi.json"
else
    : > "$backup/use-oem-network"
fi
wpa_cli -iwlan0 status > "$backup/status.txt" 2>/dev/null || :
exit 0
