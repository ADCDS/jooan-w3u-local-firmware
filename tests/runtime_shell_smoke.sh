#!/bin/sh
# Host-only tests. No real mounts, routes, processes, or camera are touched.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture=$(mktemp -d /tmp/jooan-runtime-shell.XXXXXX)
case "$fixture" in /tmp/jooan-runtime-shell.*) ;; *) exit 1 ;; esac
trap 'rm -rf "$fixture"' EXIT
JL_ROOT=$fixture/root JL_RUN=$fixture/run JL_CONTROL=$repo/runtime
export JL_ROOT JL_RUN JL_CONTROL
. "$repo/runtime/boot/common.sh"
jl_init_run

# Public resolvers disappear; private/loopback/ULA resolvers and metadata stay.
printf '%s\n' 'search bench.local' 'nameserver 8.8.8.8' 'nameserver 10.42.0.1' \
    'nameserver 172.16.1.1' 'nameserver 172.32.1.1' 'nameserver 192.168.1.1' \
    'nameserver 127.0.0.1' 'nameserver ::1' 'nameserver fd00::1' \
    'nameserver 2001:4860:4860::8888' | jl_private_resolvers > "$fixture/dns"
[ "$(wc -l < "$fixture/dns")" = 7 ]
! grep -q '8.8.8.8\|172.32\|2001:4860' "$fixture/dns"

# Mock the route utility, proving only exact defaults are selected for deletion.
route() {
    case "$*" in
        '-n') printf '%s\n' '0.0.0.0 10.42.0.1 0.0.0.0 UG 0 0 0 wlan0' \
            '10.42.0.0 0.0.0.0 255.255.255.0 U 0 0 0 wlan0' ;;
        '-A inet6 -n') printf '%s\n' '::/0 fe80::1 UG 100 0 0 wlan0' \
            'fd00::/64 :: U 100 0 0 wlan0' ;;
        *) printf '%s\n' "$*" >> "$fixture/route-actions" ;;
    esac
}
jl_prune_default_routes
[ "$(wc -l < "$fixture/route-actions")" = 2 ]
grep -q '^del default gw 10.42.0.1 dev wlan0$' "$fixture/route-actions"
grep -q '^-A inet6 del ::/0 gw fe80::1 dev wlan0$' "$fixture/route-actions"

# Fixture hashes are nonfunctional placeholders, never credentials for a device.
printf '%s\n' 'root:x:0:0:root:/:/bin/sh' 'nobody:x:65534:65534::/:/bin/sh' > "$fixture/oem-passwd"
printf '%s\n' 'admin:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789' > "$fixture/credential"
jl_make_accounts "$fixture/oem-passwd" "$fixture/credential" "$fixture/accounts" "$JL_RUN/admin-home"
awk -F: '$1=="admin" && $2=="x" && $3==0 && $7=="/bin/sh" {ok++} END {exit ok!=1}' "$fixture/accounts/passwd.new"
awk -F: '$1!="admin" && ($2!="!" || $7!="/nonexistent/jooan-disabled-login") {bad=1} END {exit bad}' "$fixture/accounts/passwd.new"
awk -F: '$1=="admin" && $2 ~ /^\$6\$fixture\$/ {ok=1} END {exit !ok}' "$fixture/accounts/shadow.new"
printf '%s\n' 'root:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789' > "$fixture/invalid"
if jl_make_accounts "$fixture/oem-passwd" "$fixture/invalid" "$fixture/accounts" "$JL_RUN/admin-home"; then exit 1; fi
printf '%s\n' 'admin:!' > "$fixture/locked"
jl_make_accounts "$fixture/oem-passwd" "$fixture/locked" "$fixture/locked-accounts" "$JL_RUN/admin-home"
grep -q '^admin:!:' "$fixture/locked-accounts/shadow.new"
printf '%s\n' 'admin:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789:extra' > "$fixture/invalid"
if jl_make_accounts "$fixture/oem-passwd" "$fixture/invalid" "$fixture/accounts" "$JL_RUN/admin-home"; then exit 1; fi

jl_lock
jl_write_selection A B 0
jl_read_selection
[ "$JL_STABLE:$JL_PENDING:$JL_ATTEMPTED" = A:B:0 ]
jl_write_selection B - 0
jl_read_selection
[ "$JL_STABLE:$JL_PENDING:$JL_ATTEMPTED" = B:-:0 ]
jl_write_selection A A 0
if jl_read_selection; then exit 1; fi
jl_unlock

# Stored checksums use BusyBox-compatible MD5 without any persistent verifier.
printf '%s\n' 'archive fixture contents' > "$fixture/archive"
md5sum "$fixture/archive" | awk '{print $1}' > "$fixture/archive.md5"
jl_verify_archive "$fixture/archive" "$fixture/archive.md5"
printf '%s\n' 'modified archive' > "$fixture/archive"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi
printf '%s\n' 'not-a-valid-md5' > "$fixture/archive.md5"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi
# A SHA-256 sidecar cannot accidentally be accepted under the smaller contract.
printf '%064d\n' 0 > "$fixture/archive.md5"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi

# Enforce final storage policy using pure test overrides, never a real partition.
jl_wait_free_kb() { [ "$2" = 80 ]; }
jl_tree_bytes() { printf '%s\n' 180224; }
jl_check_storage
jl_tree_bytes() { printf '%s\n' 180225; }
if jl_check_storage 2>/dev/null; then exit 1; fi
jl_wait_free_kb() { [ "$2" = 32 ]; }
jl_tree_bytes() { printf '%s\n' 262144; }
jl_check_transient_storage
jl_tree_bytes() { printf '%s\n' 262145; }
if jl_check_transient_storage 2>/dev/null; then exit 1; fi

# Promotion/rollback cleanup is idempotent and retains exactly the selected slot.
mkdir -p "$JL_ROOT/slots/A" "$JL_ROOT/slots/B"
printf stable > "$JL_ROOT/slots/A/runtime.tar.gz"
printf candidate > "$JL_ROOT/slots/B/runtime.tar.gz"
jl_tree_bytes() { find "$JL_ROOT" -type f -printf '%s\n' | awk '{n+=$1} END {print n+0}'; }
jl_wait_free_kb() { return 0; }
jl_prune_other_slots A
[ -d "$JL_ROOT/slots/A" ] && [ ! -e "$JL_ROOT/slots/B" ]
jl_prune_other_slots A

for script in "$repo"/runtime/boot/*.sh "$repo"/runtime/boot/local.rc \
    "$repo"/runtime/admin/*.sh "$repo"/runtime/slot/*.sh "$repo"/runtime/slot/hooks/*.sh; do
    sh -n "$script"
done
printf '%s\n' 'PASS: DNS filtering, routes, locked recovery account, transient trial state/pruning, MD5 corruption checks, storage boundaries, shell syntax'
