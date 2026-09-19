#!/bin/sh
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
case "$op" in
  snapshot) printf '\377\330FAKEJPEG\377\331' ;;
  ssh-list) printf 'test-key SHA256:example\n' ;;
  wifi-stage|wifi-commit|wifi-rollback|ssh-add|ssh-delete|ptz-move|ptz-stop|firmware-verify|firmware-apply)
    test "$path" = - || test -r "$path"
    printf '%s %s\n' "$op" "$id"
    ;;
  *) echo "unsupported fake operation: $op" >&2; exit 2 ;;
esac
