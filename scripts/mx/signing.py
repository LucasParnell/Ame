"""Shared host signing policy. Release keys are explicit files, never defaults."""
import argparse
import hashlib
import os
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

DEV_SIGNING_SEED = hashlib.sha256(b'Midix MXB1 development signing seed v1').digest()

def private_key(path=None, development=True):
    path = path or os.environ.get('AME_SIGNING_KEY')
    if path:
        key = serialization.load_pem_private_key(Path(path).read_bytes(), password=None)
        if not isinstance(key, Ed25519PrivateKey): raise ValueError('expected an Ed25519 PKCS8 private key')
    elif development:
        key = Ed25519PrivateKey.from_private_bytes(DEV_SIGNING_SEED)
    else:
        raise ValueError('release packaging requires AME_SIGNING_KEY')
    if not development and public_bytes(key.public_key()) == public_bytes(Ed25519PrivateKey.from_private_bytes(DEV_SIGNING_SEED).public_key()):
        raise ValueError('release builds reject the development key')
    return key

def public_bytes(key):
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

def public_key(path):
    data = Path(path).read_bytes()
    key = Ed25519PublicKey.from_public_bytes(data) if len(data) == 32 else serialization.load_pem_public_key(data)
    if not isinstance(key, Ed25519PublicKey): raise ValueError('expected an Ed25519 public key')
    return public_bytes(key)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--development', action='store_true')
    parser.add_argument('--signing-key')
    parser.add_argument('--trusted', nargs='*', default=[])
    args = parser.parse_args()
    signer = private_key(args.signing_key, args.development)
    pub = public_bytes(signer.public_key())
    keys = [public_key(p) for p in args.trusted]
    if not keys:
        if not args.development: raise ValueError('release builds require AME_TRUSTED_KEY_FILES')
        keys = [pub]
    if pub not in keys: raise ValueError('signing key is absent from the configured trust store')
    dev = public_bytes(Ed25519PrivateKey.from_private_bytes(DEV_SIGNING_SEED).public_key())
    if not args.development and dev in keys: raise ValueError('release trust store contains development key')
    if len(keys) > 8 or len(set(keys)) != len(keys): raise ValueError('invalid trust store size or duplicate key')
    text = '#pragma once\n#include <cstdint>\ninline constexpr uint8_t kManifestTrustedKeys[][32] = {\n'
    text += ''.join('    {' + ','.join(str(b) for b in key) + '},\n' for key in keys) + '};\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != text: args.output.write_text(text)

if __name__ == '__main__': main()
