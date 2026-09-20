#!/bin/sh
set -eu
op=${1:-}
path=${2:--}
id=${3:--}
root=${JL_ROOT:-/opt/custom/jooan-local}
run=${JL_RUN:-/run/jooan-local}
control=${JL_CONTROL:-$run/controller}
PATH=${JOOAN_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}
export PATH

firmware_mount() {
    fw_image=$1 fw_mount=$2
    mkdir "$fw_mount" || return 1
    mount -t squashfs -o ro,loop "$fw_image" "$fw_mount" || {
        rmdir "$fw_mount" 2>/dev/null || :
        return 1
    }
}
firmware_unmount() {
    fw_mount=$1
    umount "$fw_mount" 2>/dev/null || return 1
    rmdir "$fw_mount" 2>/dev/null || :
}
firmware_metadata() {
    fw_manifest=$1
    release_sequence=$(sed -n 's/^release_sequence=//p' "$fw_manifest")
    minimum_sequence=$(sed -n 's/^minimum_sequence=//p' "$fw_manifest")
    artifact_kind=$(sed -n 's/^artifact_kind=//p' "$fw_manifest")
    release_version=$(sed -n 's/^release_version=//p' "$fw_manifest")
    case "$release_sequence:$minimum_sequence" in ''|:*|*:|*[!0-9:]*) return 1 ;; esac
    [ "$release_sequence" -ge "$minimum_sequence" ] || return 1
    case "$artifact_kind" in install|uninstall) ;; *) return 1 ;; esac
    [ -n "$release_version" ] || return 1
    grep -qx 'target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316' "$fw_manifest" || return 1
    grep -qx 'device_model=JA-A12' "$fw_manifest" || return 1
    grep -qx 'model_token=A12' "$fw_manifest" || return 1
}
private_routes_validate() {
    route_file=$1
    [ -f "$route_file" ] || return 0
    while IFS= read -r cidr; do
        [ -n "$cidr" ] || return 1
        case "$cidr" in *[!0-9A-Fa-f:./]*) return 1 ;; esac
        case "$cidr" in
            10.*/*|192.168.*/*|172.16.*/*|172.17.*/*|172.18.*/*|172.19.*/*|\
            172.20.*/*|172.21.*/*|172.22.*/*|172.23.*/*|172.24.*/*|172.25.*/*|\
            172.26.*/*|172.27.*/*|172.28.*/*|172.29.*/*|172.30.*/*|172.31.*/*|\
            fc*:*/*|fd*:*/*) ;;
            *) return 1 ;;
        esac
        case "$cidr" in 0.0.0.0/0|::/0|*'..'*) return 1 ;; esac
    done < "$route_file"
}
private_routes_apply() {
    route_file=$1
    private_routes_validate "$route_file" || return 1
    # CIDRs are an allowlist for connected private networks, not gateways.
    # DHCP/connected routes remain kernel-owned; continuously remove only
    # default routes so this interface can never create an Internet path.
    . "$control/boot/common.sh" || return 1
    jl_local_network_policy
}
private_routes_json() {
    route_file=$1 first=1
    printf '{"cidrs":['
    if [ -f "$route_file" ]; then
        while IFS= read -r cidr; do
            [ "$first" = 1 ] || printf ','
            printf '"%s"' "$cidr"
            first=0
        done < "$route_file"
    fi
    printf ']}\n'
}

case "$op" in
    wifi-stage) exec "$control/admin/wifi-transaction.sh" apply "$path" ;;
    wifi-commit) exec "$control/admin/wifi-transaction.sh" commit ;;
    wifi-rollback) exec "$control/admin/wifi-transaction.sh" rollback ;;
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
    routes-list)
        route_file=$root/config/routes.list
        private_routes_apply "$route_file" || exit 1
        private_routes_json "$route_file"
        ;;
    routes-set)
        [ -f "$path" ] || exit 2
        private_routes_validate "$path" || exit 1
        mkdir -p "$root/config"
        chmod 700 "$root/config"
        cp "$path" "$root/config/routes.list.new"
        chmod 600 "$root/config/routes.list.new"
        mv -f "$root/config/routes.list.new" "$root/config/routes.list"
        sync
        private_routes_apply "$root/config/routes.list" || exit 1
        private_routes_json "$root/config/routes.list"
        ;;
    time-set)
        # id is the UTC epoch; the daemon has already range-checked it, but keep
        # the helper defensive since it runs privileged.
        case "$id" in ''|*[!0-9]*) exit 2 ;; esac
        date -s "@$id" >/dev/null 2>&1 || exit 1
        ;;
    timezone-set)
        # id is "<GmtTz> [<IANA name>]" (space-separated). The daemon validated
        # both fields; re-check here since we run privileged. jooanipc burns the
        # OSD clock using /IpcParam/TimeZomeCfg/GmtTz, which it reads at startup,
        # so this write is persistent and the overlay picks it up on the next
        # reboot. json_debug is the OEM's own in-place config writer.
        set -- $id
        tz_gmt=${1:-}
        tz_name=${2:-}
        case "$tz_gmt" in
            GMT[+-][0-9][0-9]:[0-9][0-9]) : ;;
            *) exit 2 ;;
        esac
        [ -z "$tz_name" ] || case "$tz_name" in
            *[!A-Za-z0-9_/+-]*) exit 2 ;;
        esac
        tz_cfg=/opt/conf/config.json
        tz_jd=/mnt/mtd/run/json_debug
        [ -f "$tz_cfg" ] && [ -x "$tz_jd" ] || exit 1
        LD_LIBRARY_PATH=/mnt/mtd/lib:/mnt/mtd/run "$tz_jd" -i -c w \
            -k /IpcParam/TimeZomeCfg/GmtTz -v "$tz_gmt" "$tz_cfg" >/dev/null 2>&1 || exit 1
        if [ -n "$tz_name" ]; then
            LD_LIBRARY_PATH=/mnt/mtd/lib:/mnt/mtd/run "$tz_jd" -i -c w \
                -k /IpcParam/TimeZomeCfg/TimeZone -v "$tz_name" "$tz_cfg" >/dev/null 2>&1 || exit 1
        fi
        sync
        ;;
    timezone-get)
        tz_cfg=/opt/conf/config.json
        [ -f "$tz_cfg" ] || { echo '{"gmt_tz":"","tz_name":""}'; exit 0; }
        tz_gmt=$(sed -n 's/.*"GmtTz"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$tz_cfg" | head -1)
        tz_name=$(sed -n 's/.*"TimeZone"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$tz_cfg" | head -1)
        tz_gmt=$(printf %s "$tz_gmt" | tr -cd 'A-Za-z0-9:+-')
        tz_name=$(printf %s "$tz_name" | tr -cd 'A-Za-z0-9_/+-')
        printf '{"gmt_tz":"%s","tz_name":"%s"}\n' "$tz_gmt" "$tz_name"
        ;;
    snapshot)
        echo 'snapshot backend unavailable with OEM GoAhead disabled' >&2
        exit 69
        ;;
    ptz-jog|ptz-move)
        [ -f "$path" ] || exit 2
        parsed=$(sed -n 's/^{"command":"\(up\|down\|left\|right\)","duration_ms":\([0-9][0-9]*\),"speed":\([0-9][0-9]*\)}$/\1 \2 \3/p' "$path")
        set -- $parsed
        [ "$#" = 3 ] || exit 2
        exec "$JL_SLOT_DIR/hooks/onvif-ptz.sh" move "$1" "$3" "$2"
        ;;
    ptz-stop)
        exec "$JL_SLOT_DIR/hooks/onvif-ptz.sh" stop
        ;;
    firmware-verify)
        [ -f "$path" ] && [ "$id" != - ] || exit 2
        case "$id" in *[!0-9a-f]*|'') exit 2 ;; esac
        [ "${#id}" = 64 ] || exit 2
        inspector=$control/shared/jooan-ironman-inspect
        authenticator=$control/shared/jooan-auth-verify
        [ -x "$inspector" ] && [ -x "$authenticator" ] || exit 1
        inspect=$run/firmware.$id.inspect.new
        payload=$run/firmware.$id.sqfs.new
        mounted=$run/firmware.$id.verify-mount
        trap 'firmware_unmount "$mounted" 2>/dev/null || :; rm -f "$inspect" "$payload"' EXIT HUP INT TERM
        "$inspector" "$path" > "$inspect" || exit 1
        payload_offset=$(sed -n 's/^payload_offset=//p' "$inspect")
        payload_length=$(sed -n 's/^payload_length=//p' "$inspect")
        package_length=$(sed -n 's/^package_length=//p' "$inspect")
        model_token=$(sed -n 's/^model_token=//p' "$inspect")
        package_sha256=$(sed -n 's/^package_sha256=//p' "$inspect")
        case "$payload_offset:$payload_length:$package_length" in *[!0-9:]*) exit 1 ;; esac
        [ "$payload_offset" = 96 ] && [ "$model_token" = A12 ] &&
            [ "$package_sha256" = "$id" ] &&
            [ "$package_length" = "$(wc -c < "$path")" ] || exit 1
        dd if="$path" bs=96 skip=1 2>/dev/null | \
            head -c "$payload_length" > "$payload" || exit 1
        [ "$(wc -c < "$payload")" = "$payload_length" ] || exit 1
        firmware_mount "$payload" "$mounted" || exit 1
        "$authenticator" "$mounted/release.manifest" \
            "$mounted/release.manifest.sig" "$mounted" || exit 1
        firmware_metadata "$mounted/release.manifest" || exit 1
        [ "$(cat "$mounted/RELEASE")" = "$release_version" ] || exit 1
        if [ "$artifact_kind" = install ] && [ -f "$root/state/release-sequence" ]; then
            installed=$(cat "$root/state/release-sequence") || exit 1
            case "$installed" in ''|*[!0-9]*) exit 1 ;; esac
            [ "$release_sequence" -gt "$installed" ] || exit 1
        fi
        firmware_unmount "$mounted" || exit 1
        mv -f "$payload" "$run/firmware.$id.sqfs"
        mv -f "$inspect" "$run/firmware.$id.inspect"
        {
            printf 'package_sha256=%s\n' "$id"
            printf 'release_version=%s\n' "$release_version"
            printf 'release_sequence=%s\n' "$release_sequence"
            printf 'artifact_kind=%s\n' "$artifact_kind"
            printf 'state=verified\n'
        } > "$run/firmware.$id.verified.new"
        mv -f "$run/firmware.$id.verified.new" "$run/firmware.$id.verified"
        trap - EXIT HUP INT TERM
        ;;
    firmware-apply)
        [ "$id" != - ] || exit 2
        case "$id" in *[!0-9a-f]*|'') exit 2 ;; esac
        [ "${#id}" = 64 ] || exit 2
        image=$run/staging/firmware/$id.bin
        payload=$run/firmware.$id.sqfs
        marker=$run/firmware.$id.verified
        inspector=$control/shared/jooan-ironman-inspect
        authenticator=$control/shared/jooan-auth-verify
        [ -f "$image" ] && [ -f "$payload" ] && [ -f "$marker" ] || exit 1
        [ "$(sed -n 's/^package_sha256=//p' "$marker")" = "$id" ] || exit 1
        current=$run/firmware.$id.apply-inspect
        "$inspector" "$image" > "$current" || exit 1
        [ "$(sed -n 's/^package_sha256=//p' "$current")" = "$id" ] || exit 1
        rm -f "$current"
        lock=$run/firmware-apply.lock
        mkdir "$lock" 2>/dev/null || exit 1
        status=$run/firmware.$id.status
        printf 'state=launching\n' > "$status"
        (
            apply_mount=$run/firmware.$id.apply-mount
            cleanup_apply() {
                firmware_unmount "$apply_mount" 2>/dev/null || :
                rmdir "$lock" 2>/dev/null || :
            }
            trap cleanup_apply EXIT HUP INT TERM
            if ! firmware_mount "$payload" "$apply_mount"; then
                printf 'state=failed\nreason=mount\n' > "$status"
                exit 1
            fi
            if ! "$authenticator" "$apply_mount/release.manifest" \
                "$apply_mount/release.manifest.sig" "$apply_mount"; then
                printf 'state=failed\nreason=signature\n' > "$status"
                exit 1
            fi
            if ! firmware_metadata "$apply_mount/release.manifest"; then
                printf 'state=failed\nreason=metadata\n' > "$status"
                exit 1
            fi
            if [ "$artifact_kind" = install ] && [ -f "$root/state/release-sequence" ]; then
                installed=$(cat "$root/state/release-sequence" 2>/dev/null || :)
                case "$installed" in ''|*[!0-9]*)
                    printf 'state=failed\nreason=installed-sequence\n' > "$status"; exit 1 ;;
                esac
                if [ "$release_sequence" -le "$installed" ]; then
                    printf 'state=failed\nreason=replay\n' > "$status"
                    exit 1
                fi
            fi
            printf 'state=running\n' > "$status"
            if /bin/sh "$apply_mount/upgrade.sh" "$image"; then
                printf 'state=complete\n' > "$status"
                sync
                reboot
            else
                rc=$?
                printf 'state=failed\nreason=upgrade\nexit=%s\n' "$rc" > "$status"
                exit "$rc"
            fi
        ) </dev/null >>"$run/firmware.$id.apply.log" 2>&1 &
        apply_pid=$!
        printf '%s\n' "$apply_pid" > "$run/firmware.$id.apply.pid"
        sleep 1
        if ! kill -0 "$apply_pid" 2>/dev/null; then
            wait "$apply_pid" || exit 1
        fi
        ;;
    *) exit 2 ;;
esac
