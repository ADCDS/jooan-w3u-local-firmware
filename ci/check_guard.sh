#!/bin/sh
set -eu
root=$(git rev-parse --show-toplevel)
trap 'make -C "$root/src/guard" clean >/dev/null 2>&1 || :' EXIT HUP INT TERM
make -C "$root/src/guard" clean >/dev/null
make -C "$root/src/guard" -j2 test

symbols=$(nm -D "$root/src/guard/build/libjooan_guard.so")
for symbol in accept accept4 bind connect send sendto sendmsg sendmmsg sendfile64 syscall write writev \
    open open64 read readv ioctl close dup dup2 dup3 fcntl; do
    printf '%s\n' "$symbols" | grep -Eq " [TW] $symbol$" || {
        echo "guard export missing: $symbol" >&2
        exit 1
    }
done

grep -q 'edd1afa9f89f74f60d23fc56a1347f9b406400d38a23aa46ebcc45983fd09355' \
    "$root/src/guard/guard.h"
if strings "$root/src/guard/build/libjooan_guard.so" | grep -q JOOAN_GUARD_TEST_; then
    echo 'test-only guard override leaked into production DSO' >&2
    exit 1
fi

python3 - "$root/tests/vectors/network_guard.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    vector = json.load(source)
assert vector["schema"] == 1
ids = [case["id"] for case in vector["cases"]]
assert len(ids) == len(set(ids))
required = {
    "confine-oem-bind-to-loopback",
    "allow-external-rtsp-bind",
    "deny-unsynced-external-rtsp-accept",
    "allow-unsynced-loopback-rtsp-accept",
    "confine-oem-p2p-bind",
    "redirect-approved-mqtt",
    "redirect-approved-api",
    "allow-established-rtsp-reply",
    "deny-connected-write-external",
    "deny-direct-syscall-sendmmsg",
}
assert required <= set(ids)
PY
