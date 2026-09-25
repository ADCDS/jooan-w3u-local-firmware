#!/bin/sh
# Build the host daemon against the vendored mbedTLS (fetched by the target
# build into build/*/deps) and check session lookup under large Cookie headers.
set -eu
root=$(git rev-parse --show-toplevel)
src=$(ls -d "$root"/build/release/deps/mbedtls-2.25.0 "$root"/build/deps/mbedtls-2.25.0 2>/dev/null | head -n 1) || :
[ -n "$src" ] || { echo 'cookie-header: SKIP (vendored mbedTLS not fetched; run a target build first)'; exit 0; }
work=$(mktemp -d)
trap 'rm -rf "$work"; make -C "$root/src/daemon" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM
cp -r "$src" "$work/mbedtls"
make -C "$work/mbedtls/library" -j2 CC=cc CFLAGS=-O2 >/dev/null
make -C "$root/src/daemon" clean >/dev/null
make -C "$root/src/daemon" -j2 CFLAGS='-Os -Wall -Wextra -Werror' \
    CPPFLAGS="-I. -I$work/mbedtls/include" LDFLAGS="-L$work/mbedtls/library" >/dev/null
python3 "$root/tools/tests/test_cookie_header.py"
