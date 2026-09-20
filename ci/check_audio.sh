#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
audio=$root/src/audio

if [ ! -f "$audio/audio_wire.c" ]; then
    echo 'audio-contract: SKIP (module not present)'
    exit 0
fi

tmp=${TMPDIR:-/tmp}/jooan-audio-test.$$
trap 'rm -f "$tmp"; make -C "$audio" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM

${CC:-cc} -O2 -Wall -Wextra -Werror -std=c11 \
    -I "$audio" \
    "$root/tests/test_audio_contract.c" \
    "$audio/g711_alaw.c" "$audio/audio_wire.c" \
    "$audio/audio_guard_wire.c" "$audio/audio_guard.c" \
    -o "$tmp"
"$tmp"

# Exercise the real Unix-stream WebSocket router, repeated PTT leases,
# disconnect release, protocol-error release, and JAGM microphone fan-out.
make -C "$audio" clean >/dev/null
make -C "$audio" -j2 check
