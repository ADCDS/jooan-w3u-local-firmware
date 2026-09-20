#!/bin/sh
set -eu
root=$(git rev-parse --show-toplevel)
trap 'make -C "$root/src/daemon" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM
make -C "$root/src/daemon" clean >/dev/null
make -C "$root/src/daemon" -j2 TLS=0 CFLAGS='-Os -Wall -Wextra -Werror'
python3 "$root/tools/tests/test_rtsp_proxy.py" "$root/src/daemon/joan-daemon"
