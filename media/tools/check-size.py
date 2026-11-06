#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Flash budget for the camera build: the stripped daemon (which embeds the
admin UI) must leave room in the camera's appfs partition.

The appfs is 3,211,264 bytes of xz squashfs holding the vendor files, libimp
and this daemon. On the 2026-11-05 layout a daemon compressing to 394,380
bytes left 4,096 bytes free, so keeping at least 48 KiB free means the daemon
may compress to at most 394,380 + 4,096 - 49,152 = 349,324 bytes. The UI also
has its own raw cap. The xz estimate is per file, close to squashfs's 1 MiB
blocks; build-appfs.sh reports the exact free_bytes for a real image.
"""
import argparse, lzma, os, pathlib, subprocess, sys, tempfile

DAEMON_XZ_MAX = 349_324
UI_RAW_MAX = 200_000

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", default="build/mips/jooanipc-open")
parser.add_argument("--ui", default="ui")
args = parser.parse_args()
here = pathlib.Path(__file__).resolve().parent.parent
binary = (here / args.binary).resolve()
strip = pathlib.Path(os.environ.get("TOOLCHAIN_ROOT", "")) / "bin/mips-linux-gnu-strip"
if not binary.is_file() or not strip.is_file():
    sys.exit(f"media-size: needs {binary} and $TOOLCHAIN_ROOT/bin/mips-linux-gnu-strip")
with tempfile.TemporaryDirectory() as work:
    stripped = pathlib.Path(work) / "jooanipc-open"
    subprocess.run([str(strip), "-o", str(stripped), str(binary)], check=True)
    data = stripped.read_bytes()
xz = len(lzma.compress(data, preset=9 | lzma.PRESET_EXTREME))
ui = sorted(p for p in (here / args.ui).iterdir() if p.suffix in (".html", ".css", ".js", ".svg"))
ui_raw = sum(p.stat().st_size for p in ui)
ui_xz = len(lzma.compress(b"".join(p.read_bytes() for p in ui), preset=9 | lzma.PRESET_EXTREME))
print(f"media-size: daemon stripped={len(data)} xz={xz} (max {DAEMON_XZ_MAX}); "
      f"ui raw={ui_raw} (max {UI_RAW_MAX}) xz={ui_xz}; "
      f"appfs headroom about {DAEMON_XZ_MAX + 49_152 - xz} bytes")
failed = []
if xz > DAEMON_XZ_MAX:
    failed.append(f"daemon compresses to {xz} bytes, over {DAEMON_XZ_MAX}")
if ui_raw > UI_RAW_MAX:
    failed.append(f"admin UI is {ui_raw} bytes, over {UI_RAW_MAX}")
if failed:
    sys.exit("media-size: FAIL: " + "; ".join(failed))
print("media-size: PASS")
