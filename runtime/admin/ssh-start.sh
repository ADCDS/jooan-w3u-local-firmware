#!/bin/sh
. "${JL_ROOT:-/opt/custom/jooan-local}/boot/common.sh" || exit 1
[ "$#" = 1 ] && jl_slot_valid "$1" || exit 2
jl_init_run || exit 1
jl_keys=$JL_CONFIG/ssh
jl_authorized=$jl_keys/authorized_keys
jl_authorized_dir=/opt/etc/jooan-ssh
jl_bin=$JL_RUN/shared/bin
jl_pidfile=$JL_RUN/dropbear.pid

# Provisioning must deliberately install a public key. No default credential.
# Dropping the key file disables future starts; stop any existing master too.
if [ ! -s "$jl_authorized" ]; then
    rm -f "$jl_authorized_dir/authorized_keys" "$jl_authorized_dir/authorized_keys.new" 2>/dev/null || :
    if [ -f "$jl_pidfile" ]; then
        IFS= read -r jl_pid < "$jl_pidfile" || jl_pid=
        case "$jl_pid" in ''|*[!0-9]*) ;; *)
            if [ "$(cat "/proc/$jl_pid/comm" 2>/dev/null)" = dropbear ]; then
                kill "$jl_pid" 2>/dev/null || :
            fi
        esac
    fi
    exit 0
fi
mkdir -p "$jl_authorized_dir" || exit 1
chmod 700 "$jl_authorized_dir" || exit 1
jl_publish_key=1
if [ -f "$jl_authorized_dir/authorized_keys" ]; then
    jl_key_hash=$("$JL_ROOT/shared/jooan-sha256" "$jl_authorized") || exit 1
    jl_key_hash=${jl_key_hash%% *}
    jl_published_hash=$("$JL_ROOT/shared/jooan-sha256" "$jl_authorized_dir/authorized_keys") || exit 1
    jl_published_hash=${jl_published_hash%% *}
    [ "$jl_key_hash" != "$jl_published_hash" ] || jl_publish_key=0
fi
if [ "$jl_publish_key" = 1 ]; then
    cp "$jl_authorized" "$jl_authorized_dir/authorized_keys.new" || exit 1
    chmod 600 "$jl_authorized_dir/authorized_keys.new" || exit 1
    mv -f "$jl_authorized_dir/authorized_keys.new" "$jl_authorized_dir/authorized_keys" || exit 1
fi
if [ ! -d "$JL_RUN/shared" ]; then
    jl_verify_archive "$JL_ROOT/shared/dropbear.tar.gz" "$JL_ROOT/shared/dropbear.sha256" || exit 1
    # This build-generated archive is pinned above and intentionally contains
    # one internal hardlink (dropbearkey -> dropbear) for multicall dispatch.
    # Keep the generic slot unpacker link-free; extract this exact archive into
    # a private tmpfs directory and publish it atomically.
    [ ! -e "$JL_RUN/shared.new" ] || exit 1
    mkdir "$JL_RUN/shared.new" || exit 1
    if ! tar -xzf "$JL_ROOT/shared/dropbear.tar.gz" -C "$JL_RUN/shared.new"; then
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

mkdir -p "$jl_keys" || exit 1
chmod 700 "$jl_keys" || exit 1
chmod 600 "$jl_authorized" || exit 1
jl_hostkey=$jl_keys/dropbear_ed25519_host_key
if [ ! -s "$jl_hostkey" ]; then
    [ -x "$jl_bin/dropbearkey" ] || exit 1
    # The device generates its own key; no private key is shipped in a package.
    # A separate hook lets a platform wait for a properly seeded kernel RNG.
    # Old vendor kernels do not necessarily guarantee a seeded /dev/urandom at
    # boot. A platform hook must qualify entropy before generating a new identity.
    [ -x "$JL_RUN/slot-$1/hooks/entropy-ready.sh" ] || exit 1
    jl_bounded_hook 10 "$JL_RUN/slot-$1/hooks/entropy-ready.sh" || exit 1
    # A failed first-generation attempt is not a usable identity; retry without
    # replacing any existing final host key.
    rm -f "$jl_hostkey.new" || exit 1
    jl_bounded_hook 30 "$jl_bin/dropbearkey" -t ed25519 -f "$jl_hostkey.new" >/dev/null 2>&1 || exit 1
    chmod 600 "$jl_hostkey.new" || exit 1
    mv -f "$jl_hostkey.new" "$jl_hostkey" || exit 1
    sync
fi

# Password auth, forwarding, and syslog are disabled at compile time, so their
# runtime switches are intentionally absent from this minimal binary. -D is the
# authorized_keys directory (not daemonization); -F keeps the supervised master
# in the foreground. Root's OEM home is '/', not /root.
exec "$jl_bin/dropbear" -F -p 22 \
    -P "$jl_pidfile" -r "$jl_hostkey" -D "$jl_authorized_dir" \
    >>"$JL_RUN/dropbear.log" 2>&1
