#!/bin/sh
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
root=/opt/custom/jooan-local
run=/run/jooan-local
case "$op" in
    wifi-stage) exec "$root/admin/wifi-transaction.sh" apply "$path" ;;
    wifi-commit) exec "$root/admin/wifi-transaction.sh" commit ;;
    wifi-rollback) exec "$root/admin/wifi-transaction.sh" rollback ;;
    ssh-list)
        [ -f "$root/config/ssh/authorized_keys" ] && cat "$root/config/ssh/authorized_keys"
        ;;
    ssh-add)
        key=$(cat "$path")
        case "$key" in ssh-ed25519\ *) ;; *) exit 1 ;; esac
        case "$key" in *'\n'*|*'\r'*) exit 1 ;; esac
        mkdir -p "$root/config/ssh"
        chmod 700 "$root/config/ssh"
        printf '%s\n' "$key" > "$root/config/ssh/authorized_keys.new"
        chmod 600 "$root/config/ssh/authorized_keys.new"
        mv "$root/config/ssh/authorized_keys.new" "$root/config/ssh/authorized_keys"
        sync
        ;;
    ssh-delete)
        [ "$id" != - ] || exit 2
        : > "$root/config/ssh/authorized_keys.new"
        chmod 600 "$root/config/ssh/authorized_keys.new"
        mv "$root/config/ssh/authorized_keys.new" "$root/config/ssh/authorized_keys"
        sync
        ;;
    snapshot)
        response=$run/snapshot.$$
        trap 'rm -f "$response"' EXIT
        printf 'GET /cgi-bin/net_jpeg.cgi?ch=1 HTTP/1.0\r\nHost: localhost\r\n\r\n' |
            nc 127.0.0.1 8081 > "$response"
        sed '1,/^\r$/d' "$response"
        ;;
    ptz-stop|ptz-move)
        # The daemon's local MQTT bridge carries the command. This hook exists
        # for audit/health and deliberately performs no second motor action.
        exit 0
        ;;
    firmware-verify)
        [ -f "$path" ] && [ "$id" != - ] || exit 2
        size=$(wc -c < "$path")
        case "$size" in ''|*[!0-9]*) exit 1 ;; esac
        [ "$size" -ge 4288 ] && [ "$size" -le 2097344 ] || exit 1
        [ "$(dd if="$path" bs=1 count=5 2>/dev/null)" = jooan ] || exit 1
        dd if="$path" bs=1 skip=16 count=48 2>/dev/null |
            grep -q 'ProductName=A12' || exit 1
        recorded=$(dd if="$path" bs=1 skip=64 count=32 2>/dev/null)
        actual=$(dd if="$path" bs=1 skip=96 2>/dev/null | md5sum | awk '{print $1}')
        [ "$recorded" = "$actual" ] || exit 1
        printf '%s\n' "$actual" > "$run/firmware.$id.verified"
        ;;
    firmware-apply)
        [ "$id" != - ] || exit 2
        image=$run/staging/firmware/$id.bin
        marker=$run/firmware.$id.verified
        [ -f "$image" ] && [ -f "$marker" ] || exit 1
        (
            sleep 2
            boundary=----------------jooanlocal
            head=$run/update-head.$$
            tail=$run/update-tail.$$
            request=$run/update-request.$$
            printf -- '--%s\r\nContent-Disposition: form-data; name="filename"; filename="JOOAN_FW_PKG"\r\nContent-Type: application/octet-stream\r\n\r\n' "$boundary" > "$head"
            printf '\r\n--%s--\r\n' "$boundary" > "$tail"
            length=$(( $(wc -c < "$head") + $(wc -c < "$image") + $(wc -c < "$tail") ))
            {
                printf 'POST /cgi-bin/upload_app.cgi HTTP/1.0\r\nHost: localhost\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %s\r\n\r\n' "$boundary" "$length"
                cat "$head" "$image" "$tail"
            } > "$request"
            nc 127.0.0.1 8081 < "$request" >/dev/null 2>&1 || :
        ) </dev/null >/dev/null 2>&1 &
        ;;
    *) exit 2 ;;
esac
