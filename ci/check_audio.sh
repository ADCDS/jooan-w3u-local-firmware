#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
audio=$root/src/audio

if [ ! -f "$audio/audio_wire.c" ]; then
    echo 'audio-contract: SKIP (module not present)'
    exit 0
fi

tmp=${TMPDIR:-/tmp}/jooan-audio-test.$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM

${CC:-cc} -O2 -Wall -Wextra -Werror -std=c11 \
    -I "$audio" \
    "$root/tests/test_audio_contract.c" \
    "$audio/g711_alaw.c" "$audio/audio_wire.c" \
    "$audio/audio_guard_wire.c" "$audio/audio_guard.c" \
    -o "$tmp"
"$tmp"
