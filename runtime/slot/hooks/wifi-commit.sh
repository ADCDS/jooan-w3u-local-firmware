#!/bin/sh
set -eu
[ "$#" = 1 ] && [ -f "$1" ] || exit 2
dest=/opt/custom/jooan-local/config/wifi.json
cp "$1" "$dest.new"
chmod 600 "$dest.new"
sync
mv "$dest.new" "$dest"
sync
