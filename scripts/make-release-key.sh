#!/usr/bin/env bash
#
# scripts/make-release-key.sh — generate an Ed25519 release signing keypair.
#
# Release builds require a private key that is NOT the development key, plus an
# explicit trust store. This writes one private + one public key to a directory
# OUTSIDE the repository and prints the cmake flags to use them.
#
# Usage: scripts/make-release-key.sh <output-dir> [prefix]
#
#   scripts/make-release-key.sh ~/.ame-keys
#   cmake --preset qemu-release \
#       -DAME_SIGNING_KEY=$HOME/.ame-keys/release-signing-key.pem \
#       -DAME_TRUSTED_KEY_FILES=$HOME/.ame-keys/release-trusted-key.pem
set -euo pipefail

OUT="${1:-}"
PREFIX="${2:-release}"
if [[ -z "$OUT" ]]; then
    printf 'usage: %s <output-dir> [prefix]\n' "$0" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${ROOT}/.venv/bin/python3"
[[ -x "$PY" ]] || PY="$(command -v python3)"

mkdir -p "$OUT"
chmod 700 "$OUT"

"$PY" - "$OUT" "$PREFIX" <<'PY'
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

out, prefix = Path(sys.argv[1]), sys.argv[2]
key = Ed25519PrivateKey.generate()

priv = out / f"{prefix}-signing-key.pem"
priv.write_bytes(key.private_bytes(
    serialization.Encoding.PEM,
    serialization.PrivateFormat.PKCS8,
    serialization.NoEncryption()))
os.chmod(priv, 0o600)

pub = out / f"{prefix}-trusted-key.pem"
pub.write_bytes(key.public_key().public_bytes(
    serialization.Encoding.PEM,
    serialization.PublicFormat.SubjectPublicKeyInfo))

print(f"private key : {priv}  (keep secret, outside any repo)")
print(f"public key  : {pub}")
print()
print("configure a release build with:")
print(f"  cmake --preset qemu-release \\")
print(f"      -DAME_SIGNING_KEY={priv} \\")
print(f"      -DAME_TRUSTED_KEY_FILES={pub}")
PY
