#!/bin/sh
# Build deployable MIPS32r2/uClibc components. No vendor binary is copied out.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cleanup_source_builds() {
    make -C "$repo/src/daemon" clean >/dev/null 2>&1 || :
    make -C "$repo/src/audio" clean >/dev/null 2>&1 || :
    make -C "$repo/src/guard" clean >/dev/null 2>&1 || :
}
trap cleanup_source_builds EXIT HUP INT TERM
: "${TOOLCHAIN_ROOT:?set TOOLCHAIN_ROOT to the extracted Ingenic GCC 5.4 toolchain}"
: "${OEM_ROOTFS:?set OEM_ROOTFS to a private extraction of the compatible camera rootfs}"
cc=$TOOLCHAIN_ROOT/bin/mips-linux-gnu-gcc
strip=$TOOLCHAIN_ROOT/bin/mips-linux-gnu-strip
[ -x "$cc" ] && [ -x "$strip" ]
for lib in libmbedtls.so.13 libmbedx509.so.1 libmbedcrypto.so.6; do
    [ -f "$OEM_ROOTFS/lib/$lib" ] || { echo "missing $OEM_ROOTFS/lib/$lib" >&2; exit 1; }
done

build=${BUILD_ROOT:-$repo/build}
deps=$build/deps
target=$build/target
generated=$build/generated
case "$build" in /|"$repo") echo "unsafe BUILD_ROOT: $build" >&2; exit 1 ;; esac
rm -rf "$target" "$generated"
mkdir -p "$deps" "$target/bin" "$target/shared" "$generated"

mbedtls=$deps/mbedtls-2.25.0
if [ ! -d "$mbedtls/.git" ]; then
    git clone --depth 1 --branch v2.25.0 https://github.com/Mbed-TLS/mbedtls.git "$mbedtls"
fi
[ "$(git -C "$mbedtls" rev-parse HEAD)" = 1c54b5410fd48d6bcada97e30cac417c5c7eea67 ] || {
    echo "unexpected mbedTLS revision" >&2; exit 1;
}

cflags='-Os -flto -muclibc -march=mips32r2 -mhard-float -D_BSD_SOURCE -ffunction-sections -fdata-sections -Wall -Wextra -Werror'
ldflags="-flto -muclibc -Wl,--gc-sections -L$OEM_ROOTFS/lib -Wl,-rpath-link,$OEM_ROOTFS/lib"
make -C "$repo/src/daemon" clean
make -C "$repo/src/daemon" -j1 TLS=1 CC="$cc" CFLAGS="$cflags" \
    CPPFLAGS="-I. -I$mbedtls/include" LDFLAGS="$ldflags" \
    LDLIBS="$OEM_ROOTFS/lib/libmbedtls.so.13 $OEM_ROOTFS/lib/libmbedx509.so.1 $OEM_ROOTFS/lib/libmbedcrypto.so.6 -lpthread"
"$strip" "$repo/src/daemon/joan-daemon"
cp "$repo/src/daemon/joan-daemon" "$target/bin/"

make -C "$repo/src/audio" clean
make -C "$repo/src/audio" -j1 jooan-audio-router CC="$cc" \
    CFLAGS='-Os -muclibc -march=mips32r2 -mhard-float -ffunction-sections -fdata-sections -Wall -Wextra -Werror -std=c99' \
    LDFLAGS='-muclibc -Wl,--gc-sections'
"$strip" "$repo/src/audio/jooan-audio-router"
cp "$repo/src/audio/jooan-audio-router" "$target/bin/audio-router"

make -C "$repo/src/guard" clean
make -C "$repo/src/guard" -j1 all CC="$cc" \
    CFLAGS='-Os -muclibc -march=mips32r2 -mhard-float -ffunction-sections -fdata-sections'
"$strip" "$repo/src/guard/build/libjooan_guard.so"
cp "$repo/src/guard/build/libjooan_guard.so" "$target/shared/"

"$cc" -Os -muclibc -march=mips32r2 -mhard-float \
    -I"$mbedtls/include" "$repo/src/sha256/jooan-sha256.c" \
    "$OEM_ROOTFS/lib/libmbedcrypto.so.6" -o "$target/shared/jooan-sha256"
"$strip" "$target/shared/jooan-sha256"

release_public_key=$(python3 "$repo/packaging/generate-target-contract.py" public-key \
    --target "$repo/packaging/targets/ja-a12.json")
[ "${#release_public_key}" = 130 ] || { echo 'invalid target release public key length' >&2; exit 1; }
case "$release_public_key" in 04*) ;; *) echo 'invalid target release public key prefix' >&2; exit 1 ;; esac
case "$release_public_key" in *[!0-9a-f]*) echo 'invalid target release public key encoding' >&2; exit 1 ;; esac
printf '#define JOOAN_RELEASE_PUBLIC_KEY_HEX "%s"\n' "$release_public_key" \
    > "$generated/release-public-key.h"
"$cc" -Os -muclibc -march=mips32r2 -mhard-float \
    -I"$mbedtls/include" -include "$generated/release-public-key.h" \
    "$repo/src/sha256/jooan-auth-verify.c" \
    "$OEM_ROOTFS/lib/libmbedcrypto.so.6" -o "$target/shared/jooan-auth-verify"
"$strip" "$target/shared/jooan-auth-verify"
"$cc" -Os -muclibc -march=mips32r2 -mhard-float \
    -I"$mbedtls/include" -DJOOAN_MODEL_TOKEN='"A12"' \
    "$repo/src/sha256/jooan-ironman-inspect.c" \
    "$OEM_ROOTFS/lib/libmbedcrypto.so.6" -o "$target/shared/jooan-ironman-inspect"
"$strip" "$target/shared/jooan-ironman-inspect"

dropbear=$deps/dropbear-2026.94
dropbear_tar=$deps/dropbear-2026.94.tar.bz2
if [ ! -f "$dropbear_tar" ]; then
    curl -fL --retry 3 -o "$dropbear_tar" \
        https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2
fi
printf '%s  %s\n' e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81 "$dropbear_tar" | sha256sum -c -
if [ ! -d "$dropbear" ]; then
    mkdir "$dropbear"
    tar -xjf "$dropbear_tar" -C "$dropbear" --strip-components=1
fi
cp "$repo/runtime/admin/dropbear-localoptions.h" "$dropbear/localoptions.h"
if [ ! -f "$dropbear/Makefile" ]; then
    (cd "$dropbear" && \
      CC="$cc -muclibc" AR="$TOOLCHAIN_ROOT/bin/mips-linux-gnu-gcc-ar" \
      RANLIB="$TOOLCHAIN_ROOT/bin/mips-linux-gnu-gcc-ranlib" \
      CFLAGS='-Os -flto -fPIE -march=mips32r2 -mhard-float -ffunction-sections -fdata-sections' \
      LDFLAGS='-muclibc -flto -fPIE -Wl,--gc-sections' ./configure --host=mips-linux-gnu \
        --disable-zlib --disable-syslog --disable-lastlog --disable-utmp \
        --disable-utmpx --disable-wtmp --disable-wtmpx)
fi
# An extracted toolchain may have moved since Dropbear was configured; do not
# let a cached Makefile retain its former absolute compiler path.
make -C "$dropbear" -j1 PROGRAMS='dropbear dropbearkey' MULTI=1 \
    CC="$cc -muclibc" AR="$TOOLCHAIN_ROOT/bin/mips-linux-gnu-gcc-ar" \
    RANLIB="$TOOLCHAIN_ROOT/bin/mips-linux-gnu-gcc-ranlib"
"$strip" "$dropbear/dropbearmulti"
drop_stage=$build/dropbear-stage
rm -rf "$drop_stage"
mkdir -p "$drop_stage/bin"
cp "$dropbear/dropbearmulti" "$drop_stage/bin/dropbear"
ln -f "$drop_stage/bin/dropbear" "$drop_stage/bin/dropbearkey"
# Recovery is persistent on the 384 KiB JFFS2 partition. Apply the same
# deterministic zopfli gzip compression already required for runtime archives.
recovery_tar=$build/dropbear-recovery.tar
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$drop_stage" -cf "$recovery_tar" .
zopfli --i25 --gzip -c "$recovery_tar" > "$target/shared/dropbear.tar.gz"
rm -f "$recovery_tar"

(cd "$target/shared" && sha256sum libjooan_guard.so | awk '{print $1}' > guard.sha256)
(cd "$target/shared" && sha256sum dropbear.tar.gz | awk '{print $1}' > dropbear.sha256)

python3 "$repo/packaging/write-provenance.py" \
    --repo "$repo" --build-root "$build" --toolchain-root "$TOOLCHAIN_ROOT" \
    --oem-rootfs "$OEM_ROOTFS" --target "$repo/packaging/targets/ja-a12.json" \
    --output "$build/build-provenance.json"

file "$target/bin/joan-daemon" "$target/bin/audio-router" "$target/shared/libjooan_guard.so" \
    "$target/shared/jooan-sha256" "$target/shared/jooan-auth-verify" \
    "$target/shared/jooan-ironman-inspect"
du -h "$target/bin/joan-daemon" "$target/shared/"*
