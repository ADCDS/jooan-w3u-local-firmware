#!/usr/bin/env python3
"""Refuse shell that the camera's BusyBox cannot run.

The target's BusyBox is a small build, and the applets it leaves out are not
the ones a Linux workstation would miss. Worse, their absence is quiet: a
cleanup written as `rmdir "$dir" 2>/dev/null || :` reports success on a host
and does nothing at all on the camera, and the damage only surfaces later --
that exact line left a firmware mount point behind, after which the same
update package could never be verified again.

Host-side scripts are not checked. They run here, with GNU coreutils, and are
entitled to the whole toolbox; only what ships to the camera is held to it.

Every entry below was established on hardware, not assumed.
"""
import re
import subprocess
import sys
from pathlib import Path

# Applet -> what to use instead on this target.
MISSING = {
    "rmdir": "rm -rf (no rmdir applet; the old call silently did nothing)",
    "basename": "${var##*/}",
    "readlink": "no readlink applet; resolve the path explicitly",
    "setsid": "no setsid applet; background with & and a redirect",
    "nohup": "no nohup applet; background with & and a redirect",
    "which": "command -v",
}

# GNU flags the target's applets do not implement.
MISSING_FLAGS = [
    (re.compile(r"\bfind\b[^\n|;&]*\s-type\b"), "find has no -type on this BusyBox"),
    (re.compile(r"\bfind\b[^\n|;&]*\s-maxdepth\b"), "find has no -maxdepth on this BusyBox"),
]

# A command position: start of line, or after a pipe, semicolon, &&, ||,
# an opening paren/brace, a backtick or $( ... ).
COMMAND_POSITION = r"(?:^|[|;&(){}`]|\$\(|\|\||&&)\s*"

# Directories whose shell actually runs on the camera.
SHIPPED = ("runtime/", "packaging/payload/")


def shipped_scripts(root: Path):
    listed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard", "*.sh"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    return [p for p in listed if p.startswith(SHIPPED)]


def strip_comment(line: str) -> str:
    """Drop a trailing comment, ignoring '#' inside quotes."""
    out, quote = [], None
    for i, ch in enumerate(line):
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "#" and (i == 0 or line[i - 1].isspace()):
            break
        out.append(ch)
    return "".join(out)


def main() -> int:
    root = Path(subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    ).stdout.strip())

    patterns = {
        applet: re.compile(COMMAND_POSITION + re.escape(applet) + r"(?=\s|$)")
        for applet in MISSING
    }
    findings, checked = [], 0

    for relative in shipped_scripts(root):
        checked += 1
        for number, raw in enumerate((root / relative).read_text().splitlines(), 1):
            line = strip_comment(raw)
            if not line.strip():
                continue
            for applet, pattern in patterns.items():
                if pattern.search(line):
                    findings.append(f"{relative}:{number}: {applet} -- use {MISSING[applet]}")
            for pattern, reason in MISSING_FLAGS:
                if pattern.search(line):
                    findings.append(f"{relative}:{number}: {reason}")

    if findings:
        print("target-applets gate failed:", file=sys.stderr)
        for finding in findings:
            print("  " + finding, file=sys.stderr)
        return 1
    print(f"target-applets: PASS ({checked} shipped scripts checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
