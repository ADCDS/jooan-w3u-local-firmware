#!/bin/sh
set -eu
root=$(git rev-parse --show-toplevel)
trap 'make -C "$root/src/daemon" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM
make -C "$root/src/daemon" clean >/dev/null
make -C "$root/src/daemon" -j2 TLS=0 CFLAGS='-Os -Wall -Wextra -Werror -DJOAN_AUDIO_TRIAL_SECONDS=4 -DJOAN_AUDIO_ACK_SECONDS=2'
python3 "$root/tools/tests/test_audio_pause.py"
