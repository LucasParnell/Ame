#!/usr/bin/env python3
"""Decode a WebAuthn attestationObject captured from a browser console."""
import argparse
import base64
import sys
from pathlib import Path

from host import decode


def load(text: str) -> bytes:
    text = text.strip()
    if len(text) % 2 == 0 and text and all(c in "0123456789abcdefABCDEF" for c in text):
        return bytes.fromhex(text)
    return base64.b64decode(text, validate=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("value", nargs="?", help="base64 or hex attestationObject")
    parser.add_argument("--file", type=Path, help="read the value from a file")
    args = parser.parse_args()
    if args.file:
        data = load(args.file.read_text())
    elif args.value:
        data = load(args.value)
    else:
        parser.error("provide a base64/hex value or --file")
    value, end = decode(data)
    if end != len(data):
        raise SystemExit("trailing bytes after attestationObject")
    if not isinstance(value, dict):
        raise SystemExit("attestationObject is not a CBOR map")
    fmt = value.get("fmt")
    statement = value.get("attStmt") or {}
    auth = value.get("authData") or b""
    print(f"fmt={fmt}")
    if isinstance(statement, dict):
        print(f"alg={statement.get('alg')}")
        print(f"sig={len(statement.get('sig') or b'')}")
        print(f"x5c={len(statement.get('x5c') or [])}")
    if len(auth) >= 53:
        print(f"flags=0x{auth[32]:02x}")
        print(f"aaguid={auth[37:53].hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
