#!/bin/sh
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
case "$op" in
  snapshot) printf '\377\330FAKEJPEG\377\331' ;;
  ssh-list) printf 'test-key SHA256:example\n' ;;
  routes-list) printf '{"cidrs":[]}\n' ;;
  time-set|timezone-set|wifi-stage|wifi-commit|wifi-rollback|ssh-add|ssh-delete|routes-set|firmware-verify|firmware-apply)
    test "$path" = - || test -r "$path"
    printf '%s %s\n' "$op" "$id"
    # Record requested operations for tests; a fake never changes the host.
    [ -z "${FAKE_HELPER_LOG:-}" ] || printf '%s %s\n' "$op" "$id" >> "$FAKE_HELPER_LOG"
    ;;
  *) echo "unsupported fake operation: $op" >&2; exit 2 ;;
esac
