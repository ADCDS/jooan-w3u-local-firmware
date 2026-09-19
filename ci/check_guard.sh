#!/bin/sh
set -eu
root=$(git rev-parse --show-toplevel)
trap 'make -C "$root/src/guard" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM
make -C "$root/src/guard" -j2 test
