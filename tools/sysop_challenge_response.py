#!/usr/bin/env python3
"""Generate a valid SYSOP challenge response for esp-ax25 BBS.

Reads the secret from ~/.sysop_secret and accepts challenge indices as CLI args.
Supports either:
- 4 positional integers: 12 5 31 7
- A CHAL line split into args: CHAL 12 5 31 7
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

CHALLENGE_INDEX_COUNT = 4
RESPONSE_LEN = 6
SECRET_PATH = Path("~/.sysop_secret").expanduser()


def _die(msg: str) -> "None":
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(2)


def _load_secret(path: Path) -> str:
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        _die(f"secret file not found: {path}")
    except OSError as exc:
        _die(f"failed to read secret file {path}: {exc}")

    # Trim only line endings from typical editor writes.
    secret = raw.rstrip("\r\n")
    if not secret:
        _die(f"secret in {path} is empty")
    return secret


def _parse_indices(argv: list[str]) -> list[int]:
    args = argv[:]
    if args and args[0].upper() == "CHAL":
        args = args[1:]

    if len(args) != CHALLENGE_INDEX_COUNT:
        _die(
            f"expected {CHALLENGE_INDEX_COUNT} indices (or: CHAL i1 i2 i3 i4), got {len(args)}"
        )

    out: list[int] = []
    for token in args:
        try:
            idx = int(token, 10)
        except ValueError:
            _die(f"invalid index '{token}' (must be integer)")
        if idx <= 0:
            _die(f"invalid index '{token}' (must be >= 1)")
        out.append(idx)
    return out


def _build_response(secret: str, indices: list[int]) -> str:
    secret_len = len(secret)
    required: list[str] = []

    for idx in indices:
        if idx > secret_len:
            _die(f"index {idx} out of range for secret length {secret_len}")
        required.append(secret[idx - 1])

    # Must be exactly 6 chars and include all required chars (with multiplicity).
    response = required[:]
    pad = 0
    while len(response) < RESPONSE_LEN:
        response.append(required[pad % len(required)])
        pad += 1

    return "".join(response)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in {"-h", "--help"}:
        prog = os.path.basename(sys.argv[0]) or "sysop_challenge_response.py"
        print(f"usage: {prog} [CHAL] i1 i2 i3 i4")
        print("reads secret from ~/.sysop_secret and prints 6-char response")
        return 0

    secret = _load_secret(SECRET_PATH)
    indices = _parse_indices(argv)
    response = _build_response(secret, indices)
    print(response)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
