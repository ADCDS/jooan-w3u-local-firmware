#!/bin/sh
. "${JL_CONTROL:-/run/jooan-local/controller}/boot/common.sh" || exit 1
[ "$#" = 1 ] || exit 2
case "$1" in A|B|-) ;; *) exit 2 ;; esac
jl_init_run || exit 1
mkdir "$JL_RUN/ssh-start.lock" 2>/dev/null || exit 0
trap 'rm -rf "$JL_RUN/ssh-start.lock" 2>/dev/null || :' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
jl_keys=$JL_CONFIG/ssh
jl_accounts=$JL_RUN/accounts
jl_home=$JL_RUN/admin-home
jl_authorized_dir=$jl_home/.ssh
jl_bin=$JL_RUN/shared/bin
jl_pidfile=$JL_RUN/dropbear.pid

jl_stop_ssh() {
    if [ -f "$jl_pidfile" ]; then
        IFS= read -r jl_pid < "$jl_pidfile" || jl_pid=
        case "$jl_pid" in ''|*[!0-9]*) ;; *)
            if [ "$(cat "/proc/$jl_pid/comm" 2>/dev/null)" = dropbear ]; then
                kill -TERM "$jl_pid" 2>/dev/null || :
                sleep 1
                if [ "$(cat "/proc/$jl_pid/comm" 2>/dev/null)" = dropbear ]; then
                    kill -KILL "$jl_pid" 2>/dev/null || :
                fi
            fi
        esac
    fi
    rm -f "$jl_pidfile"
}

# The HTTPS daemon owns the canonical salted crypt hash. If it has not yet
# synchronized SSH but an enrolled key exists, project a tmpfs-only locked
# password record; never invent or persist a password for migrated custom auth.
jl_credential=$jl_keys/passwd
if [ -s "$jl_credential" ] && grep -qx 'admin:!' "$jl_credential" &&
   [ ! -s "$jl_keys/authorized_keys" ]; then
    jl_stop_ssh
    exit 1
fi
if [ ! -s "$jl_credential" ]; then
    [ -s "$jl_keys/authorized_keys" ] || { jl_stop_ssh; exit 1; }
    jl_credential=$JL_RUN/recovery-ssh-passwd
    printf '%s\n' 'admin:!' > "$jl_credential" || exit 1
    chmod 600 "$jl_credential" || exit 1
fi
[ "$(id -u)" = 0 ] || exit 1
awk '$2=="/run" && $3=="tmpfs" {ok=1} END {exit !ok}' /proc/mounts || exit 1
for jl_account in /etc/passwd /etc/shadow; do
    [ -f "$jl_account" ] && [ ! -L "$jl_account" ] || { jl_stop_ssh; exit 1; }
done
if [ -f /etc/shells ] && grep -q '^/nonexistent/jooan-disabled-login$' /etc/shells; then
    jl_stop_ssh
    exit 1
fi
mkdir -p "$jl_accounts" "$jl_authorized_dir" || exit 1
chmod 700 "$jl_accounts" "$jl_home" "$jl_authorized_dir" || exit 1
if [ ! -f "$jl_accounts/oem-passwd" ]; then
    # Refuse an unknown account overlay; do not stack mounts over another owner.
    awk '$5=="/etc/passwd" || $5=="/etc/shadow" {bad=1} END {exit bad}' /proc/self/mountinfo || exit 1
    cp /etc/passwd "$jl_accounts/oem-passwd" || exit 1
    chmod 600 "$jl_accounts/oem-passwd" || exit 1
fi
jl_record_hash=$(md5sum "$jl_credential") || { jl_stop_ssh; exit 1; }
jl_record_hash=${jl_record_hash%% *}
jl_old_hash=$(cat "$jl_accounts/credential.md5" 2>/dev/null || :)
jl_mounts=$(awk '$5=="/etc/passwd" || $5=="/etc/shadow" {print $1, $4, $5}' /proc/self/mountinfo)
jl_owned_mounts=$(cat "$jl_accounts/mounts" 2>/dev/null || :)
if [ -n "$jl_owned_mounts" ] && [ "$jl_mounts" != "$jl_owned_mounts" ]; then
    jl_stop_ssh
    exit 1
fi
if [ "$jl_record_hash" != "$jl_old_hash" ] || [ -z "$jl_owned_mounts" ]; then
    jl_make_accounts "$jl_accounts/oem-passwd" "$jl_credential" "$jl_accounts" "$jl_home" || { jl_stop_ssh; exit 1; }
    jl_stop_ssh
    if [ -z "$jl_owned_mounts" ]; then
        [ -z "$jl_mounts" ] || exit 1
        mv "$jl_accounts/passwd.new" "$jl_accounts/passwd" || exit 1
        mv "$jl_accounts/shadow.new" "$jl_accounts/shadow" || exit 1
        mount -o bind "$jl_accounts/passwd" /etc/passwd || exit 1
        if ! mount -o bind "$jl_accounts/shadow" /etc/shadow; then
            umount /etc/passwd
            exit 1
        fi
        awk '$5=="/etc/passwd" || $5=="/etc/shadow" {print $1, $4, $5}' /proc/self/mountinfo > "$jl_accounts/mounts" || exit 1
    else
        # Preserve the bound inodes. The listener is stopped during the two
        # writes, so no new login observes an incomplete password publication.
        cat "$jl_accounts/passwd.new" > "$jl_accounts/passwd" || exit 1
        cat "$jl_accounts/shadow.new" > "$jl_accounts/shadow" || exit 1
        rm -f "$jl_accounts/passwd.new" "$jl_accounts/shadow.new"
    fi
    printf '%s\n' "$jl_record_hash" > "$jl_accounts/credential.md5" || exit 1
fi

# Authorized keys supplement password auth; removing keys does not disable SSH.
if [ -s "$jl_keys/authorized_keys" ]; then
    cp "$jl_keys/authorized_keys" "$jl_authorized_dir/authorized_keys.new" || exit 1
    chmod 600 "$jl_authorized_dir/authorized_keys.new" || exit 1
    mv -f "$jl_authorized_dir/authorized_keys.new" "$jl_authorized_dir/authorized_keys" || exit 1
else
    rm -f "$jl_authorized_dir/authorized_keys" "$jl_authorized_dir/authorized_keys.new"
fi
if [ ! -d "$JL_RUN/shared" ]; then
    jl_recovery=$JL_ROOT/recovery
    jl_verify_archive "$jl_recovery/dropbear.tar.gz" \
        "$jl_recovery/dropbear.md5" || {
            jl_recovery=$JL_ROOT/recovery.old
            jl_verify_archive "$jl_recovery/dropbear.tar.gz" \
                "$jl_recovery/dropbear.md5" || exit 1
        }
    # The pinned build artifact intentionally has one internal multicall hardlink.
    [ ! -e "$JL_RUN/shared.new" ] || exit 1
    mkdir "$JL_RUN/shared.new" || exit 1
    if ! tar -xzf "$jl_recovery/dropbear.tar.gz" -C "$JL_RUN/shared.new"; then
        rm -rf "$JL_RUN/shared.new"
        exit 1
    fi
    mv "$JL_RUN/shared.new" "$JL_RUN/shared" || exit 1
fi
[ -x "$jl_bin/dropbear" ] || exit 1
if [ -f "$jl_pidfile" ]; then
    IFS= read -r jl_pid < "$jl_pidfile" || jl_pid=
    case "$jl_pid" in ''|*[!0-9]*) ;; *)
        [ "$(cat "/proc/$jl_pid/comm" 2>/dev/null)" != dropbear ] || exit 0
    esac
fi
jl_hostkey=$jl_keys/dropbear_ed25519_host_key
if [ ! -s "$jl_hostkey" ]; then
    [ -x "$jl_bin/dropbearkey" ] || exit 1
    jl_entropy=$JL_CONTROL/shared/entropy-ready.sh
    [ "$1" = - ] || jl_entropy=$JL_RUN/slot-$1/hooks/entropy-ready.sh
    [ -x "$jl_entropy" ] || exit 1
    jl_bounded_hook 10 "$jl_entropy" || exit 1
    rm -f "$jl_hostkey.new" || exit 1
    jl_bounded_hook 30 "$jl_bin/dropbearkey" -t ed25519 -f "$jl_hostkey.new" >/dev/null 2>&1 || exit 1
    chmod 600 "$jl_hostkey.new" || exit 1
    mv -f "$jl_hostkey.new" "$jl_hostkey" || exit 1
    sync
fi

# Default Dropbear daemonization returns to the policy/Wi-Fi supervisor. Do not
# pass -F here. Password auth is enabled; blank passwords/forwarding are disabled.
"$jl_bin/dropbear" -p 22 -P "$jl_pidfile" -r "$jl_hostkey" -D "$jl_authorized_dir" \
    >>"$JL_RUN/dropbear.log" 2>&1
