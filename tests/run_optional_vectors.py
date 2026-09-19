#!/usr/bin/env python3
"""Validate optional module vectors and run their JSON-lines adapters."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


CONFIG = {
    "network_guard": {
        "env": "NETWORK_GUARD_VECTOR_CMD",
        "vector": "tests/vectors/network_guard.json",
        "sources": ("*network*guard*", "*firewall*", "*egress*guard*"),
        "evaluators": ("build/network-guard-vector-eval", "build/network_guard_vector_eval"),
    },
    "audio": {
        "env": "AUDIO_VECTOR_CMD",
        "vector": "tests/vectors/audio.json",
        "sources": ("*audio*", "*speaker*"),
        "evaluators": ("build/audio-vector-eval", "build/audio_vector_eval"),
    },
}


def module_sources(root: Path, patterns: tuple[str, ...]) -> list[Path]:
    source = root / "src"
    if not source.exists():
        return []
    found: set[Path] = set()
    for pattern in patterns:
        found.update(path for path in source.rglob(pattern) if path.is_file())
    return sorted(found)


def validate(document: dict, suite: str) -> list[dict]:
    if document.get("schema") != 1 or document.get("suite") != suite:
        raise ValueError(f"invalid {suite} vector header")
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError(f"{suite} has no cases")
    ids: set[str] = set()
    for case in cases:
        if set(("id", "input", "expected")) - set(case):
            raise ValueError(f"malformed case: {case!r}")
        if case["id"] in ids:
            raise ValueError(f"duplicate case id {case['id']}")
        ids.add(case["id"])
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("suite", choices=CONFIG)
    parser.add_argument("--command")
    parser.add_argument("--root")
    parser.add_argument("--schema-only", action="store_true")
    args = parser.parse_args()
    root = Path(args.root or subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    config = CONFIG[args.suite]
    vector_path = root / config["vector"]
    cases = validate(json.loads(vector_path.read_text()), args.suite)
    if args.schema_only:
        print(f"{args.suite}: PASS ({len(cases)} vectors schema-valid)")
        return 0
    command = args.command or os.environ.get(config["env"])
    if not command:
        command = next((str(root / item) for item in config["evaluators"]
                        if os.access(root / item, os.X_OK)), None)
    sources = module_sources(root, config["sources"])
    if not command:
        if sources:
            print(f"{args.suite}: FAIL: module appeared but no vector adapter; "
                  f"set {config['env']}", file=sys.stderr)
            for source in sources:
                print(f"  {source.relative_to(root)}", file=sys.stderr)
            return 1
        print(f"{args.suite}: SKIP (module not present; {len(cases)} vectors schema-valid)")
        return 0
    payload = "".join(json.dumps({"id": case["id"], "input": case["input"]},
                                 separators=(",", ":")) + "\n" for case in cases)
    result = subprocess.run(shlex.split(command), input=payload, text=True,
                            capture_output=True, cwd=root, timeout=30)
    if result.returncode:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return 1
    replies: dict[str, object] = {}
    for line in result.stdout.splitlines():
        reply = json.loads(line)
        replies[reply["id"]] = reply.get("actual")
    failures = 0
    for case in cases:
        actual = replies.get(case["id"])
        if actual != case["expected"]:
            failures += 1
            print(f"{args.suite}:{case['id']}: {actual!r} != {case['expected']!r}",
                  file=sys.stderr)
    if failures:
        return 1
    print(f"{args.suite}: PASS ({len(cases)} vectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
