#!/bin/sh
# Signed, exact-target, resumable installer executed by the IronMan carrier.
set -eu
PATH=${JOOAN_PATH:-/bin:/sbin:/usr/bin:/usr/sbin:/mnt/mtd/run}
export PATH
umask 077

self=${0%/*}
root=${JOOAN_ROOT:-/opt/custom/jooan-local}
activate=${JOOAN_ACTIVATE:-/opt/etc/local.rc}
run=${JOOAN_RUN:-/run/jooan-local}
legacy_root=${JOOAN_LEGACY_ROOT:-/opt/open}
device_model_path=${JOOAN_DEVICE_MODEL_PATH:-/etc/deviceModel}
compat_root=${JOOAN_COMPAT_ROOT:-}
sha=$self/jooan-sha256
auth=$self/jooan-auth-verify
manifest=$self/release.manifest
signature=$self/release.manifest.sig

die() { echo "jooan-local install: $*" >&2; exit 1; }
meta() { sed -n "s/^$1=//p" "$manifest"; }
fail_at() { [ "${JOOAN_FAIL_AT:-}" != "$1" ] || die "injected failure at $1"; }
migration_hash() {
    migration_path=$1
    sed -n "s|^expanded_sha256=\([0-9a-f][0-9a-f]*\)  $migration_path$|\1|p" \
        "$self/migration.contract"
}
bounded_sha() {
    hash_file=$1 hash_output=/tmp/jooan-sha.$$
    "$sha" "$hash_file" > "$hash_output" 2>/dev/null &
    hash_pid=$!
    ( sleep 30; kill -TERM "$hash_pid" 2>/dev/null || : ) \
        </dev/null >/dev/null 2>&1 &
    timer_pid=$!
    if wait "$hash_pid"; then hash_rc=0; else hash_rc=$?; fi
    kill "$timer_pid" 2>/dev/null || :
    [ "$hash_rc" = 0 ] || { rm -f "$hash_output"; return 1; }
    awk '{print $1}' "$hash_output"
    rm -f "$hash_output"
}
write_state() {
    mkdir -p "$root/state" || return 1
    printf '%s\n' "$1" > "$root/state/migration-state.new" || return 1
    chmod 600 "$root/state/migration-state.new" || return 1
    sync
    mv -f "$root/state/migration-state.new" "$root/state/migration-state" || return 1
    sync
    [ "${JOOAN_FAIL_AFTER_STATE:-}" != "$1" ] || return 99
}
stage_trial_runtime() {
    IFS=' ' read -r trial_stable trial_pending trial_attempted trial_extra \
        < "$root/state/selection" || return 1
    [ -z "$trial_extra" ] || return 1
    if [ "$trial_pending" != - ]; then
        case "$trial_pending:$trial_attempted" in A:0|A:1|B:0|B:1) ;; *) return 1 ;; esac
        trial_dest=$root/slots/$trial_pending
        [ -f "$trial_dest/runtime.tar.gz" ] && [ -f "$trial_dest/runtime.md5" ] || return 1
        expected=$(cat "$trial_dest/runtime.md5") || return 1
        actual=$(md5sum "$trial_dest/runtime.tar.gz" | awk '{print $1}') || return 1
        [ "$actual" = "$expected" ] || return 1
        [ "$(bounded_sha "$trial_dest/runtime.tar.gz")" = \
          "$(bounded_sha "$self/runtime.tar.gz")" ] || return 1
        write_state runtime-staged
        return
    fi
    [ "$trial_attempted" = 0 ] || return 1
    case "$trial_stable" in
        A) trial_target=B ;;
        B) trial_target=A ;;
        -)
            trial_running=$(cat "$run/running" 2>/dev/null || :)
            case "$trial_running" in A) trial_target=B ;; B) trial_target=A ;; *) trial_target=A ;; esac
            ;;
        *) return 1 ;;
    esac
    trial_dest=$root/slots/$trial_target
    # Enter durable controller-owned SSH recovery, then remove the sole
    # persistent runtime. Its already-expanded processes survive in /run until
    # reboot, but no power cut can select a deleted archive.
    printf '%s\n' '- - 0' > "$root/state/selection.new" || return 1
    chmod 600 "$root/state/selection.new" || return 1
    sync
    mv -f "$root/state/selection.new" "$root/state/selection" || return 1
    fail_at after-recovery-selection
    rm -rf "$root/slots/A" "$root/slots/B" \
        "$root/slots/A.new" "$root/slots/B.new" || return 1
    sync
    fail_at after-runtime-delete
    require_boot_reserve || return 1
    trial_bytes=$(wc -c < "$self/runtime.tar.gz") || return 1
    trial_md5_bytes=$(wc -c < "$self/runtime.md5") || return 1
    require_copy_headroom $((trial_bytes + trial_md5_bytes)) || return 1
    mkdir "$trial_dest.new" || return 1
    cp "$self/runtime.tar.gz" "$self/runtime.md5" "$trial_dest.new/" || return 1
    require_boot_reserve || return 1
    fail_at after-runtime-copy
    expected=$(cat "$trial_dest.new/runtime.md5") || return 1
    actual=$(md5sum "$trial_dest.new/runtime.tar.gz" | awk '{print $1}') || return 1
    [ "$actual" = "$expected" ] || return 1
    sync
    mv "$trial_dest.new" "$trial_dest" || return 1
    fail_at after-runtime-dir-rename
    printf '%s %s 0\n' - "$trial_target" > "$root/state/selection.new"
    chmod 600 "$root/state/selection.new"
    sync
    mv -f "$root/state/selection.new" "$root/state/selection"
    sync
    fail_at after-pending-selection-rename
    write_state runtime-staged
}
require_boot_reserve() {
    JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control /bin/sh -c \
        '. "$JL_CONTROL/boot/common.sh" && jl_wait_free_kb "$JL_ROOT" 56 20'
}
require_copy_headroom() {
    copy_bytes=$1
    copy_kb=$(( (copy_bytes + 1023) / 1024 ))
    JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control COPY_KB=$copy_kb /bin/sh -c \
        '. "$JL_CONTROL/boot/common.sh" && jl_wait_free_kb "$JL_ROOT" $((COPY_KB + 56)) 20'
}
enter_runtime_recovery() {
    mkdir -p "$root/state" "$root/slots" || return 1
    printf '%s\n' '- - 0' > "$root/state/selection.new" || return 1
    chmod 600 "$root/state/selection.new" || return 1
    sync
    mv -f "$root/state/selection.new" "$root/state/selection" || return 1
    rm -rf "$root/slots/A" "$root/slots/B" \
        "$root/slots/A.new" "$root/slots/B.new" || return 1
    sync
    require_boot_reserve
}
publish_controller_core() {
    new_core_hash=$(bounded_sha "$self/controller.tar.gz") || return 1
    if [ -f "$root/controller.tar.gz" ]; then
        old_core_hash=$(bounded_sha "$root/controller.tar.gz" 2>/dev/null || :)
        new_loader_hash=$(bounded_sha "$self/local.rc") || return 1
        old_loader_hash=$(bounded_sha "$root/local.rc" 2>/dev/null || :)
        if [ "$old_core_hash" = "$new_core_hash" ] &&
           [ "$old_loader_hash" = "$new_loader_hash" ]; then return 0; fi
    fi
    controller_bytes=$(wc -c < "$self/controller.tar.gz") || return 1
    loader_bytes=$(wc -c < "$self/local.rc") || return 1
    if ! require_copy_headroom $((controller_bytes + loader_bytes)); then
        recovery_valid "$root/recovery" || recovery_valid "$root/recovery.old" || return 1
        enter_runtime_recovery || return 1
        require_copy_headroom $((controller_bytes + loader_bytes)) || return 1
    fi
    cp "$self/controller.tar.gz" "$root/controller.tar.gz.new" || return 1
    require_boot_reserve || return 1
    fail_at after-controller-copy
    cp "$self/local.rc" "$root/local.rc.new" || return 1
    require_boot_reserve || return 1
    chmod 600 "$root/controller.tar.gz.new" || return 1
    chmod 755 "$root/local.rc.new" || return 1
    tar -tzf "$root/controller.tar.gz.new" >/dev/null || return 1
    [ "$(bounded_sha "$root/controller.tar.gz.new")" = "$new_core_hash" ] || return 1
    sync
    mv -f "$root/controller.tar.gz.new" "$root/controller.tar.gz" || return 1
    require_boot_reserve || return 1
    fail_at after-controller-rename
    mv -f "$root/local.rc.new" "$root/local.rc" || return 1
    sync
    write_state controller-published
}
recovery_valid() {
    recovery_dir=$1
    [ -f "$recovery_dir/dropbear.tar.gz" ] &&
        [ -f "$recovery_dir/dropbear.md5" ] || return 1
    recovery_expected=$(cat "$recovery_dir/dropbear.md5") || return 1
    [ "${#recovery_expected}" = 32 ] || return 1
    recovery_actual=$(md5sum "$recovery_dir/dropbear.tar.gz" | awk '{print $1}') || return 1
    [ "$recovery_actual" = "$recovery_expected" ] &&
        tar -tzf "$recovery_dir/dropbear.tar.gz" >/dev/null
}
publish_recovery() {
    if ! recovery_valid "$root/recovery" && recovery_valid "$root/recovery.old"; then
        rm -rf "$root/recovery"
        mv "$root/recovery.old" "$root/recovery" || return 1
        sync
    fi
    if recovery_valid "$root/recovery"; then
        old_recovery=$(bounded_sha "$root/recovery/dropbear.tar.gz") || return 1
        new_recovery=$(bounded_sha "$self/recovery.tar.gz") || return 1
        if [ "$old_recovery" = "$new_recovery" ]; then
            rm -rf "$root/recovery.old" "$root/recovery.new"
            sync
            require_boot_reserve
            return
        fi
    fi
    recovery_bytes=$(wc -c < "$self/recovery.tar.gz") || return 1
    recovery_md5_bytes=$(wc -c < "$self/recovery.md5") || return 1
    if ! require_copy_headroom $((recovery_bytes + recovery_md5_bytes)); then
        enter_runtime_recovery || return 1
        require_copy_headroom $((recovery_bytes + recovery_md5_bytes)) || return 1
    fi
    rm -rf "$root/recovery.new"
    mkdir "$root/recovery.new" || return 1
    cp "$self/recovery.tar.gz" "$root/recovery.new/dropbear.tar.gz" || return 1
    cp "$self/recovery.md5" "$root/recovery.new/dropbear.md5" || return 1
    recovery_valid "$root/recovery.new" || return 1
    require_boot_reserve || return 1
    fail_at after-recovery-copy
    sync
    rm -rf "$root/recovery.old"
    [ ! -d "$root/recovery" ] || mv "$root/recovery" "$root/recovery.old" || return 1
    fail_at after-recovery-old-rename
    mv "$root/recovery.new" "$root/recovery" || return 1
    fail_at after-recovery-rename
    sync
    rm -rf "$root/recovery.old"
    require_boot_reserve
}
[ "$(id -u)" = 0 ] || die 'not root'
[ "$(cat "$device_model_path" 2>/dev/null)" = JA-A12 ] || die 'unsupported model'
[ -x "$sha" ] && [ -x "$auth" ] || die 'release verifiers missing'
"$auth" "$manifest" "$signature" "$self" || die 'signed release inventory rejected'
grep -qx 'target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316' "$manifest" || die 'wrong target'
grep -qx 'device_model=JA-A12' "$manifest" || die 'wrong device model'
grep -qx 'model_token=A12' "$manifest" || die 'wrong package token'
grep -qx 'artifact_kind=install' "$manifest" || die 'not an install artifact'
release=$(cat "$self/RELEASE") || die 'release marker missing'
[ "$(meta release_version)" = "$release" ] || die 'release marker mismatch'
sequence=$(meta release_sequence)
minimum=$(meta minimum_sequence)
[ -n "$sequence" ] && [ -n "$minimum" ] || die 'missing release sequence'
case "$sequence:$minimum" in *[!0-9:]*) die 'invalid release sequence' ;; esac
[ "$sequence" -ge "$minimum" ] || die 'release below minimum sequence'
# Exact device and retained userspace ABI gates are generated from target JSON.
while read -r expected file; do
    [ -n "$expected" ] || continue
    check_file=$compat_root$file
    [ -e "$check_file" ] || die "compatible component unavailable: $file"
    actual=$(bounded_sha "$check_file") || die "compatibility hash timed out or failed: $file"
    [ "$actual" = "$expected" ] || die "incompatible camera component: $file"
done < "$self/compatibility.sha256"

grep -qx 'JOOAN-PERSISTENT-CONTRACT-V1' "$self/persistent.contract" || die 'persistent contract missing'
grep -qx 'logical_regular_file_cap_bytes=196608' "$self/persistent.contract" || die 'persistent cap mismatch'
grep -qx 'final_free_reserve_bytes=65536' "$self/persistent.contract" || die 'free-space reserve mismatch'
grep -qx 'state_config_regular_file_reserve_bytes=12288' "$self/persistent.contract" || die 'state/config reserve mismatch'
grep -qx 'external_regular_file_reserve_bytes=4096' "$self/persistent.contract" || die 'external reserve mismatch'
grep -qx 'maintenance_regular_file_cap_bytes=196608' "$self/persistent.contract" || die 'maintenance cap mismatch'
grep -qx 'maintenance_final_free_reserve_bytes=57344' "$self/persistent.contract" || die 'maintenance reserve mismatch'
grep -qx 'JOOAN-MIGRATION-CONTRACT-V1' "$self/migration.contract" || die 'migration contract missing'
grep -qx 'state=legacy-retired' "$self/migration.contract" || die 'migration states incomplete'

installed_prior=0
if [ -f "$root/state/release-sequence" ]; then
    current=$(cat "$root/state/release-sequence") || die 'cannot read installed sequence'
    case "$current" in ''|*[!0-9]*) die 'installed sequence is invalid' ;; esac
    if [ "$sequence" -le "$current" ]; then
        if [ "$sequence" = "$current" ] &&
           [ "$(cat "$root/state/migration-state" 2>/dev/null)" = legacy-retired ]; then
            echo 'jooan-local signed install already complete'
            exit 0
        fi
        die 'downgrade or replay rejected'
    fi
    installed_prior=1
fi
if [ "${JOOAN_PREFLIGHT_ONLY:-0}" = 1 ]; then
    echo 'jooan-local signed preflight passed'
    exit 0
fi

mkdir -p "$run" || die 'cannot create runtime directory'
chmod 700 "$run"
control=$run/controller-install
rm -rf "$control"
mkdir "$control"
tar -tzf "$self/controller.tar.gz" >/dev/null || die 'controller archive CRC mismatch'
tar -xzf "$self/controller.tar.gz" -C "$control" || die 'controller extraction failed'
JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control JOOAN_SHA256=$sha
export JL_ROOT JL_RUN JL_CONTROL JOOAN_SHA256

# Validate every recognized predecessor before preserving keys. Unknown partial
# trees fail without deletion; the operator can recover them manually.
legacy=none
expanded=0
migration_hint=$(cat "$root/state/migration-state" 2>/dev/null || :)
if [ -e "$root/boot" ] || [ -e "$root/admin" ] || [ -e "$root/shared" ]; then
    case "$migration_hint" in
        activated|expanded-controller-retired|legacy-retired)
            [ -f "$root/controller.tar.gz" ] && [ -f "$root/local.rc" ] &&
                [ -f "$activate" ] || die 'partial retirement lacks durable replacement'
            expanded=4
            ;;
    esac
fi
if [ "$expanded" = 0 ] &&
   { [ -e "$root/boot" ] || [ -e "$root/admin" ] || [ -e "$root/shared" ]; }; then
    reclaim_resume=0
    case "$migration_hint" in
        headroom-reclaiming|headroom-reclaimed|controller-published|\
        failclosed-hook-published|activated) reclaim_resume=1 ;;
    esac
    for old in boot admin shared; do
        [ -d "$root/$old" ] || die 'partial expanded-product-0.1 controller'
    done
    for old in boot/common.sh boot/boot.sh boot/local.rc \
        admin/install-controller.sh admin/install-runtime.sh; do
        [ -f "$root/$old" ] || die "expanded-product-0.1 missing $old"
    done
    if [ "$reclaim_resume" = 0 ]; then
        [ -f "$root/shared/libjooan_guard.so" ] &&
            [ -f "$root/shared/guard.sha256" ] &&
            [ -f "$root/shared/jooan-sha256" ] &&
            [ -f "$root/shared/dropbear.tar.gz" ] &&
            [ -f "$root/shared/dropbear.sha256" ] ||
            die 'expanded-product-0.1 replaceable shared files are incomplete'
    fi
    old_list=
    if [ -f "$root/shared/dropbear.tar.gz" ]; then
        old_list=dropbear.tar.gz
    fi
    [ ! -f "$root/shared/jooan-sha256" ] || old_list="jooan-sha256 $old_list"
    [ "$reclaim_resume" = 1 ] || old_list="libjooan_guard.so $old_list"
    for old in $old_list; do
        old_expected=$(migration_hash "shared/$old") || die 'cannot read pinned expanded digest'
        [ "${#old_expected}" = 64 ] || die "invalid pinned expanded digest: $old"
        old_actual=$(bounded_sha "$root/shared/$old") || die 'cannot hash expanded controller'
        [ "$old_actual" = "$old_expected" ] || die "expanded controller mismatch: $old"
    done
    [ -f "$root/state/selection" ] || die 'expanded-product-0.1 has no selection'
    IFS=' ' read -r old_stable old_pending old_attempted old_extra < "$root/state/selection" ||
        die 'cannot read expanded selection'
    [ -z "$old_extra" ] || die 'expanded selection has extra fields'
    case "$old_stable:$old_pending:$old_attempted" in
        A:-:0|B:-:0) old_active=$old_stable ;;
        -:A:0|-:B:0) old_active=$old_pending ;;
        A:B:0|A:B:1) old_active=A ;;
        B:A:0|B:A:1) old_active=B ;;
        *) die 'expanded selection is not safely migratable' ;;
    esac
    old_slot=$root/slots/$old_active
    [ -f "$old_slot/runtime.tar.gz" ] && [ -f "$old_slot/runtime.sha256" ] ||
        die 'expanded active runtime is incomplete'
    old_expected=$(migration_hash slots/runtime.tar.gz) || die 'cannot read pinned runtime digest'
    old_actual=$(bounded_sha "$old_slot/runtime.tar.gz") ||
        die 'cannot hash active runtime'
    [ "$old_actual" = "$old_expected" ] || die 'expanded active runtime mismatch'
    expanded=1
    [ "$reclaim_resume" = 0 ] || expanded=6
fi
if [ "$installed_prior" = 0 ] && [ "$expanded" = 0 ] &&
   [ -f "$root/state/migration-state" ] &&
   [ -f "$root/controller.tar.gz" ] &&
   [ -f "$root/local.rc" ]; then
    case "$migration_hint" in
        expanded-controller-retired) expanded=5 ;;
        runtime-staged) expanded=2 ;;
        activated) expanded=4 ;;
        failclosed-hook-published|legacy-retired) expanded=2 ;;
    esac
fi
if [ -e "$legacy_root" ]; then
    if [ -f "$legacy_root/admin/manifest.md5" ] && [ -s "$legacy_root/admin/ssh/authorized_keys" ] &&
       [ -s "$legacy_root/admin/ssh/host_ed25519" ] &&
       (cd "$legacy_root/admin" && md5sum -c manifest.md5 >/dev/null 2>&1); then
        legacy=manual-admin
    elif [ -f "$legacy_root/current" ]; then
        old_version=$(cat "$legacy_root/current" 2>/dev/null || :)
        case "$old_version" in ''|*[!0-9A-Za-z._-]*) die 'unknown predecessor version' ;; esac
        [ -f "$legacy_root/$old_version/manifest.sha256" ] &&
            [ -f "$legacy_root/$old_version/local.rc" ] || die 'partial predecessor tree'
        legacy=developer-launcher
    else
        die 'unrecognized predecessor tree; refusing destructive migration'
    fi
fi

mkdir -p "$root/state" "$root/config/ssh" || die 'cannot prepare migration state'
chmod 700 "$root" "$root/state" "$root/config" "$root/config/ssh"
if [ "$expanded" = 1 ]; then
    write_state expanded-product-0.1-validated || die 'cannot journal expanded validation'
elif [ "$expanded" = 2 ] || [ "$expanded" = 3 ] || [ "$expanded" = 4 ] ||
     [ "$expanded" = 5 ] || [ "$expanded" = 6 ]; then
    :
else
    write_state legacy-validated || die 'cannot journal legacy validation'
fi
if [ "$legacy" = manual-admin ]; then
    cp "$legacy_root/admin/ssh/authorized_keys" "$root/config/ssh/authorized_keys.new"
    cp "$legacy_root/admin/ssh/host_ed25519" "$root/config/ssh/dropbear_ed25519_host_key.new"
    chmod 600 "$root/config/ssh/"*.new
    mv -f "$root/config/ssh/authorized_keys.new" "$root/config/ssh/authorized_keys"
    mv -f "$root/config/ssh/dropbear_ed25519_host_key.new" \
        "$root/config/ssh/dropbear_ed25519_host_key"
fi
# Never invent a password for a migrated customized Web account. A migrated
# authorized key gets locked-password recovery until the owner rotates the Web
# password, which atomically synchronizes SSH. Clean installs use the documented
# temporary credential because no auth database exists yet.
if [ ! -s "$root/config/ssh/passwd" ]; then
    if [ -f "$root/config/auth.db" ]; then
        if [ -s "$root/config/ssh/authorized_keys" ]; then
            printf '%s\n' 'admin:!' > "$root/config/ssh/passwd.new"
        fi
    else
        printf '%s\n' 'admin:$1$joorec01$e4zY3XKFPREtq8.7yI/lE0' > \
            "$root/config/ssh/passwd.new"
    fi
    if [ -f "$root/config/ssh/passwd.new" ]; then
        chmod 600 "$root/config/ssh/passwd.new"
        mv -f "$root/config/ssh/passwd.new" "$root/config/ssh/passwd"
        sync
    fi
fi
if [ "$expanded" != 2 ] && [ "$expanded" != 3 ] && [ "$expanded" != 4 ] &&
   [ "$expanded" != 5 ] && [ "$expanded" != 6 ]; then
    write_state keys-preserved || die 'cannot journal key preservation'
fi
# The authenticated predecessor admin bundle is larger than the replacement
# compressed controller. Once its irreplaceable keys are durable above, retire
# only that validated sub-tree to create installation headroom. The predecessor
# launcher and OEM updater remain intact until replacement activation.
if [ "$legacy" = manual-admin ]; then
    rm -rf "$legacy_root/admin" || die 'cannot retire validated predecessor admin bundle'
    sync
fi

case "$expanded" in
    1|6) ;;
    *)
        publish_controller_core || die 'cannot publish compressed controller core'
        publish_recovery || die 'cannot publish persistent SSH recovery bundle'
        ;;
esac

if [ "$expanded" = 1 ] || [ "$expanded" = 6 ]; then
    # Reclaim only the inactive runtime and an intentionally disabled unsafe
    # hook backup. The active slot remains the rollback candidate.
    old_inactive=A
    [ "$old_active" = A ] && old_inactive=B
    [ "$expanded" != 1 ] ||
        write_state headroom-reclaiming || die 'cannot journal headroom reclamation'
    rm -rf "$root/slots/$old_inactive"
    require_boot_reserve || die 'inactive-slot reclamation fell below boot reserve'
    fail_at after-reclaim-inactive
    rm -f "$root/state/prelocal-hook.disabled"
    require_boot_reserve || die 'hook-backup reclamation fell below boot reserve'
    fail_at after-reclaim-prelocal
    rm -f "$root/shared/jooan-sha256"
    require_boot_reserve || die 'SHA reclamation fell below boot reserve'
    fail_at after-reclaim-sha
    rm -f "$root/shared/libjooan_guard.so"
    sync
    require_boot_reserve || die 'guard reclamation fell below boot reserve'
    fail_at after-reclaim-guard
    rm -f "$root/shared/dropbear.tar.gz"
    sync
    require_boot_reserve || die 'Dropbear reclamation fell below boot reserve'
    fail_at after-reclaim-dropbear
    rm -f "$root/shared/dropbear.sha256"
    require_boot_reserve || die 'Dropbear sidecar reclamation fell below boot reserve'
    fail_at after-reclaim-dropbear-sidecar
    write_state headroom-reclaimed || die 'cannot journal reclaimed headroom'
    # Convert the retained active slot to the compressed controller's MD5
    # corruption sidecar while its SHA-256 was just authenticated above.
    md5sum "$old_slot/runtime.tar.gz" | awk '{print $1}' > "$old_slot/runtime.md5.new"
    chmod 600 "$old_slot/runtime.md5.new"
    sync
    mv -f "$old_slot/runtime.md5.new" "$old_slot/runtime.md5"
    require_boot_reserve || die 'runtime sidecar conversion fell below boot reserve'
    publish_controller_core || die 'cannot publish compressed controller core'
    publish_recovery || die 'cannot publish persistent SSH recovery bundle'
    # Normalize the retained known-good runtime as stable before switching the
    # controller hook. The replacement trial is staged only after old expanded
    # files are retired and space is recovered.
    printf '%s - 0\n' "$old_active" > "$root/state/selection.new"
    chmod 600 "$root/state/selection.new"
    sync
    mv -f "$root/state/selection.new" "$root/state/selection"
    sync
    # Publish the already-verified fail-closed compressed-controller hook. The
    # generic activate helper's final-size gate cannot run until expanded files
    # are retired, but every replacement component is durable at this point.
    hook_bytes=$(wc -c < "$root/local.rc") || die 'cannot size compressed hook'
    require_copy_headroom "$hook_bytes" || die 'insufficient boot-safe headroom for hook activation'
    cp "$root/local.rc" "$activate.new" || die 'cannot stage compressed hook'
    require_boot_reserve || die 'hook activation copy fell below boot reserve'
    chmod 755 "$activate.new"
    sync
    mv -f "$activate.new" "$activate" || die 'cannot activate compressed hook'
    sync
    require_boot_reserve || die 'hook publication fell below boot reserve'
    fail_at after-hook-rename
    write_state failclosed-hook-published || die 'cannot journal fail-closed hook publication'
    write_state activated || die 'cannot journal activation'
    # New persistent controller, active fallback, pending replacement, and hook
    # now exist. Retire only the obsolete expanded controller and SHA sidecars.
    rm -rf "$root/boot"
    fail_at after-retire-boot
    rm -rf "$root/admin"
    fail_at after-retire-admin
    rm -rf "$root/shared"
    fail_at after-retire-shared
    rm -f "$root/slots/A/runtime.sha256" "$root/slots/B/runtime.sha256"
    sync
    write_state expanded-controller-retired || die 'cannot journal expanded retirement'
    JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control /bin/sh -c \
        '. "$JL_CONTROL/boot/common.sh" && jl_check_current_storage' ||
        die 'migrated persistent layout violates final storage contract'
    stage_trial_runtime || die 'cannot stage replacement runtime maintenance'
    JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control /bin/sh -c \
        '. "$JL_CONTROL/boot/common.sh" && jl_check_current_storage' ||
        die 'replacement runtime violates maintenance storage contract'
elif [ "$expanded" = 3 ]; then
    JL_ACTIVATE=$activate
    export JL_ACTIVATE
    "$control/admin/install-controller.sh" activate || die 'resumed activation failed'
    write_state activated || die 'cannot journal resumed activation'
elif [ "$expanded" = 2 ] || [ "$expanded" = 4 ] || [ "$expanded" = 5 ]; then
    if [ "$expanded" = 4 ]; then
        rm -rf "$root/boot"
        fail_at after-retire-boot
        rm -rf "$root/admin"
        fail_at after-retire-admin
        rm -rf "$root/shared"
        fail_at after-retire-shared
        rm -f "$root/slots/A/runtime.sha256" "$root/slots/B/runtime.sha256"
        sync
        write_state expanded-controller-retired || die 'cannot finish expanded retirement'
        expanded=5
    fi
    tar -tzf "$root/controller.tar.gz" >/dev/null || die 'resumed controller archive CRC mismatch'
    [ -f "$activate" ] && grep -q '/opt/custom/jooan-local' "$activate" ||
        die 'resumed migration has no active compressed hook'
    JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control /bin/sh -c \
        '. "$JL_CONTROL/boot/common.sh" && jl_check_current_storage' ||
        die 'resumed migration violates final storage contract'
    if [ "$expanded" = 5 ]; then
        stage_trial_runtime || die 'cannot resume replacement runtime trial'
        JL_ROOT=$root JL_RUN=$run JL_CONTROL=$control /bin/sh -c \
            '. "$JL_CONTROL/boot/common.sh" && jl_check_current_storage' ||
            die 'resumed runtime violates maintenance storage contract'
    fi
else
    printf '%s\n' 1 > "$root/state/controller.ready.new"
    chmod 600 "$root/state/controller.ready.new"
    sync
    mv -f "$root/state/controller.ready.new" "$root/state/controller.ready"
    "$control/admin/install-runtime.sh" "$self" || die 'runtime staging failed'
    write_state runtime-staged || die 'cannot journal runtime staging'
    JL_ACTIVATE=$activate
    export JL_ACTIVATE
    "$control/admin/install-controller.sh" activate || die 'activation failed'
    write_state activated || die 'cannot journal activation'
fi

# Retire only a fully recognized predecessor and only after replacement
# activation. A failure/power cut before this point leaves the old tree intact.
if [ "$legacy" != none ]; then
    rm -rf "$legacy_root" || die 'cannot retire predecessor tree'
    sync
fi
write_state legacy-retired || die 'cannot finalize migration journal'
printf '%s\n' "$sequence" > "$root/state/release-sequence.new"
chmod 600 "$root/state/release-sequence.new"
sync
mv -f "$root/state/release-sequence.new" "$root/state/release-sequence"
sync
[ "${JOOAN_FAIL_AFTER_STATE:-}" != release-sequence-published ] ||
    die 'injected failure after sequence publication'
killall telnetd 2>/dev/null || :
sync
echo 'jooan-local signed install complete; rebooting through carrier'
exit 0
