#!/bin/sh
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
case "$op" in
  snapshot) printf '\377\330FAKEJPEG\377\331' ;;
  ssh-list) printf 'test-key SHA256:example\n' ;;
  routes-list) printf '{"cidrs":[]}\n' ;;
  ptz-presets-list) printf '[]\n' ;;
  timezone-get) printf '{"gmt_tz":"GMT+08:00","tz_name":"Asia/Shanghai"}\n' ;;
  time-set|timezone-set|wifi-stage|wifi-commit|wifi-rollback|ssh-add|ssh-delete|routes-set|ptz-move|ptz-jog|ptz-stop|ptz-home|ptz-preset-set|ptz-preset-goto|ptz-preset-delete|firmware-verify|firmware-apply)
    test "$path" = - || test -r "$path"
    printf '%s %s\n' "$op" "$id"
    ;;
  *) echo "unsupported fake operation: $op" >&2; exit 2 ;;
esac
