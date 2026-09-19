#!/bin/sh
. "${JL_ROOT:-/opt/custom/jooan-local}/boot/common.sh" || exit 1
[ "$#" = 1 ] && jl_slot_valid "$1" || exit 2
jl_init_run || exit 1
jl_keys=$JL_CONFIG/ssh
jl_authorized=$jl_keys/authorized_keys
jl_bin=$JL_RUN/shared/bin
jl_pidfile=$JL_RUN/dropbear.pid

# Provisioning must deliberately install a public key. No default credential.
# Dropping the key file disables future starts; stop any existing master too.
if [ ! -s "$jl_authorized" ]; then
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

# -D must be supported by the selected Dropbear build. Parent directories must
# be root-owned and not group/world-writable. Root's OEM home is '/', not /root.
exec "$jl_bin/dropbear" -s -j -k -E -p 22 \
    -P "$jl_pidfile" -r "$jl_hostkey" -D "$jl_keys" \
    >>"$JL_RUN/dropbear.log" 2>&1
