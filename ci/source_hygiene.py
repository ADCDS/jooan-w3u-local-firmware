#!/usr/bin/env python3
"""Reject committed credentials and opaque firmware/vendor executables.

The scan includes tracked and untracked, non-ignored files so it is useful
before the first commit as well as in CI. Generated files should be ignored,
not broadly allow-listed. A deliberate public test vector may put
``hygiene: allow-test-vector`` on the same line as a secret-shaped string.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


FORBIDDEN_SUFFIXES = {
    ".a", ".bin", ".dtb", ".elf", ".fw", ".img", ".jffs2", ".key",
    ".ko", ".o", ".p12", ".pfx", ".rom", ".so", ".squashfs", ".ubifs",
    ".uimage",
}

FORBIDDEN_MAGICS = (
    (b"\x7fELF", "ELF executable/library"),
    (b"!<arch>\n", "Unix object archive"),
    (b"\x27\x05\x19\x56", "U-Boot uImage"),
    (b"hsqs", "SquashFS image"),
    (b"sqsh", "SquashFS image"),
)

SECRET_PATTERNS = (
    (re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
     "private key"),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), "AWS access key"),
    (re.compile(r"\bgh[pousr]_[A-Za-z0-9_]{30,}\b"), "GitHub token"),
    (re.compile(
        r"(?i)\b(?:password|passwd|token|secret|localkey|private[_-]?key)\b"
        r"\s*[:=]\s*['\"]([^'\"]{6,})['\"]"
    ), "literal credential"),
    (re.compile(r"[A-Za-z][A-Za-z0-9+.-]*://[^\s/:]+:[^\s/@]+@"),
     "credential in URL"),
    (re.compile(r"\bchange-me-now\b", re.IGNORECASE),
     "embedded default credential"),
)

SAFE_VALUE_MARKERS = (
    "example", "placeholder", "redacted", "changeme", "test-only",
    "not-a-secret", "xxxxx", "${", "<",
)


def candidate_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard", "-z"],
            check=True, capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return sorted(
            path for path in root.rglob("*")
            if path.is_file() and ".git" not in path.parts
        )
    return sorted(root / raw.decode("utf-8", "surrogateescape")
                  for raw in result.stdout.split(b"\0") if raw)


def scan(root: Path) -> list[str]:
    findings: list[str] = []
    scanner = Path(__file__).resolve()
    for path in candidate_files(root):
        if not path.is_file() or path.resolve() == scanner:
            continue
        rel = path.relative_to(root).as_posix()
        suffix = path.suffix.lower()
        if suffix in FORBIDDEN_SUFFIXES:
            findings.append(f"{rel}: forbidden opaque/vendor-binary suffix {suffix}")
            continue
        try:
            with path.open("rb") as stream:
                head = stream.read(4096)
        except OSError as exc:
            findings.append(f"{rel}: cannot read: {exc}")
            continue
        for magic, description in FORBIDDEN_MAGICS:
            if head.startswith(magic):
                findings.append(f"{rel}: forbidden {description}")
                break
        if b"\0" in head:
            # Binary assets are allowed, but never decode them looking for
            # credentials. Executable/firmware formats were rejected above.
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if "hygiene: allow-test-vector" in line:
                continue
            for pattern, description in SECRET_PATTERNS:
                match = pattern.search(line)
                if not match:
                    continue
                if (description == "embedded default credential" and
                        suffix in {".md", ".rst", ".txt"}):
                    # Documentation must be able to name a forbidden default
                    # while source/configuration must not embed it.
                    continue
                if (description == "embedded default credential" and
                        "hygiene: allow-public-bootstrap" in line):
                    # Product decision: the generic image has one documented,
                    # setup-only password and blocks normal routes until it is
                    # replaced. Keep this exception exact and line-local.
                    continue
                value = match.group(1).lower() if match.lastindex else line.lower()
                if any(marker in value for marker in SAFE_VALUE_MARKERS):
                    continue
                findings.append(f"{rel}:{number}: possible {description}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=None)
    args = parser.parse_args()
    root = Path(args.root or subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    findings = scan(root)
    if findings:
        print("source-hygiene gate failed:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"source-hygiene: PASS ({len(candidate_files(root))} files considered)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
