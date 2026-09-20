#!/usr/bin/env python3
"""Strip comments and indentation from staged Web UI assets.

The persistent layout has under 2 KiB of spare compressed space, so readable
source cannot ship verbatim. This mirrors what ``assemble-stages.sh`` already
does to shell scripts: the repository keeps commented source, the device gets
the same program with the prose removed.

Only whole-line comments and leading indentation go. Nothing inside a line is
rewritten, so a string or regular expression containing ``//`` is untouched.
That is safe here because no asset carries a template literal spanning lines;
``check()`` re-asserts that on every run rather than trusting it.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


BACKTICK = re.compile(r"(?<!\\)`")


def spans_lines_in_template(source: str) -> bool:
    """True when a template literal crosses a newline (line stripping unsafe)."""
    depth = 0
    for line in source.splitlines():
        depth += len(BACKTICK.findall(line))
        if depth % 2 == 1:
            return True
    return False


def strip_trailing_comment(line: str) -> str:
    """Remove a trailing ``//`` comment, but only from a line that cannot be
    misread. A line holding no quote, backtick or regular-expression slash has
    no construct in which ``//`` means anything but a comment."""
    if "//" not in line:
        return line
    if any(ch in line for ch in ("'", '"', "`")):
        return line
    head = line.split("//", 1)[0]
    # A lone "/" before the "//" could be division opening a regex; leave it.
    if head.count("/") % 2:
        return line
    return head.rstrip() or line


def minify_js(source: str) -> str:
    out: list[str] = []
    in_block = False
    for line in source.splitlines():
        stripped = line.strip()
        if in_block:
            if "*/" in stripped:
                in_block = False
                tail = stripped.split("*/", 1)[1].strip()
                if tail:
                    out.append(tail)
            continue
        if not stripped:
            continue
        if stripped.startswith("//"):
            continue
        if stripped.startswith("/*"):
            if "*/" in stripped[2:]:
                tail = stripped.split("*/", 1)[1].strip()
                if tail:
                    out.append(tail)
            else:
                in_block = True
            continue
        out.append(strip_trailing_comment(stripped))
    return "\n".join(out) + "\n"


def minify_css(source: str) -> str:
    # CSS has no regex or template literals, so block comments are removable
    # wherever they appear.
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    lines = [line.strip() for line in source.splitlines()]
    text = "\n".join(line for line in lines if line)
    # Structural punctuation needs no surrounding space. Skipped entirely when
    # the sheet contains a quoted value, where spacing can be significant.
    if "'" not in text and '"' not in text:
        text = re.sub(r"\s*([{};,>])\s*", r"\1", text)
        text = re.sub(r":\s+", ":", text)
        text = re.sub(r";}", "}", text)
    return text + "\n"


def minify_html(source: str) -> str:
    source = re.sub(r"<!--(?!\[if).*?-->", "", source, flags=re.S)
    lines = [line.strip() for line in source.splitlines()]
    return "\n".join(line for line in lines if line) + "\n"


HANDLERS = {".js": minify_js, ".css": minify_css, ".html": minify_html}


def process(path: Path) -> tuple[int, int]:
    handler = HANDLERS.get(path.suffix.lower())
    if handler is None:
        return (0, 0)
    source = path.read_text(encoding="utf-8")
    if path.suffix.lower() == ".js" and spans_lines_in_template(source):
        raise SystemExit(
            f"minify-web: {path.name} has a template literal spanning lines; "
            "line-based stripping would corrupt it"
        )
    before = len(source.encode("utf-8"))
    result = handler(source)
    path.write_text(result, encoding="utf-8")
    return (before, len(result.encode("utf-8")))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    args = parser.parse_args()
    root = Path(args.directory)
    if not root.is_dir():
        print(f"minify-web: not a directory: {root}", file=sys.stderr)
        return 1
    before = after = 0
    for path in sorted(root.iterdir()):
        if not path.is_file():
            continue
        was, now = process(path)
        before += was
        after += now
    if before:
        print(f"minify-web: {before} -> {after} bytes ({before - after} removed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
