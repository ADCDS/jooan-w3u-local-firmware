#!/bin/sh
# Fetch the public Ingenic GCC 5.4 multilib toolchain used by the target.
set -eu

[ "$#" = 1 ] || { echo "usage: $0 OUTPUT-DIRECTORY" >&2; exit 2; }
out=$1
[ ! -e "$out" ] || { echo "refusing existing output: $out" >&2; exit 1; }
command -v curl >/dev/null
command -v unar >/dev/null
command -v sha256sum >/dev/null

tmp=${out}.download
[ ! -e "$tmp" ] || { echo "refusing existing download directory: $tmp" >&2; exit 1; }
mkdir -p "$tmp/parts" "$tmp/extract"
base=https://raw.githubusercontent.com/cgrrty/Ingenic-SDK-T31-1.1.1-20200508/main/toolchain

cat > "$tmp/parts/SHA256SUMS" <<'SUMS'
62a4155be87bdb76b8c2521fbb72c38db1f6748eb858b7b9a9e1d0a3f67131c0  toolchain.part1.rar
8e7241f631346cc1eeccfdc516b90331f593b6f4b2ce76ac08e3ebdb79772713  toolchain.part2.rar
7145470b18fa7d8cafd9b5d9a358d725d0d2646a9293104e1a96fd649521f3fa  toolchain.part3.rar
1b45eec12c81b0ab32bef9e5e49eeeb7604abd7cacb846467132db5139749889  toolchain.part4.rar
ad56186c947665232a426bfcbddeafeef78c59dd41e5deaf480b6852ba755be2  toolchain.part5.rar
0200affb1f8c3995e285ee76630256ee84e473b54f2f09e4327f88e19ddebcac  toolchain.part6.rar
dc2ed7a9392f0da3d6f92d557ce6ee7649e9c8ae61be56d9bb84ace2142dbafe  toolchain.part7.rar
SUMS

i=1
while [ "$i" -le 7 ]; do
    file=toolchain.part$i.rar
    curl -fL --retry 3 -o "$tmp/parts/$file" "$base/$file"
    i=$((i + 1))
done
(cd "$tmp/parts" && sha256sum -c SHA256SUMS)
unar -f -o "$tmp/extract" "$tmp/parts/toolchain.part1.rar"
archive=$tmp/extract/toolchain/gcc_540/mips-gcc540-glibc222-64bit-r3.3.0.tar.gz
[ -f "$archive" ] || { echo "toolchain archive not extracted" >&2; exit 1; }
(cd "${archive%/*}" && md5sum -c mips-gcc540-glibc222-64bit-r3.3.0.md5)
mkdir -p "$out"
tar -xzf "$archive" -C "$out" --strip-components=1
[ -x "$out/bin/mips-linux-gnu-gcc" ] || { echo "compiler missing after extraction" >&2; exit 1; }
"$out/bin/mips-linux-gnu-gcc" --version | head -1
