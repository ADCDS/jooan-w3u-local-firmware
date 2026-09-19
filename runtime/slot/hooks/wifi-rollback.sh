#!/bin/sh
set -eu
[ "$#" = 1 ] && [ -d "$1" ] || exit 2
backup=$1
dest=/opt/custom/jooan-local/config/wifi.json
if [ -f "$backup/wifi.json" ]; then
    cp "$backup/wifi.json" "$dest.new"
    chmod 600 "$dest.new"
    mv "$dest.new" "$dest"
else
    rm -f "$dest"
fi
sync
reboot
