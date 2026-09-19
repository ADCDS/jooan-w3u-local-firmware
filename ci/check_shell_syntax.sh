#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
failed=0
count=0

git -C "$root" ls-files -co --exclude-standard '*.sh' | LC_ALL=C sort | while IFS= read -r relative; do
    test -n "$relative" || continue
    path=$root/$relative
    first=$(sed -n '1p' "$path")
    shell=sh
    case "$first" in
        *bash*) shell=bash ;;
    esac
    if ! "$shell" -n "$path"; then
        echo "shell-syntax: FAIL $relative ($shell)" >&2
        exit 1
    fi
    count=$((count + 1))
    echo "shell-syntax: ok $relative ($shell)"
done || failed=1

test "$failed" -eq 0
echo "shell-syntax: PASS"
