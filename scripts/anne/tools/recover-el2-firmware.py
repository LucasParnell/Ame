"""Recover exact-image ANE firmware from private factory-fastboot captures.

Offline only. Requires fastboot.img, teeos.img, trustfirmware.img,
tee-shared-ram.bin (0x367ff600, 0xa00 bytes), and the pinned signed xloader.
No p20re checkout, modules, or prebuilt patched LK are required. Never writes phone storage,
reads/programs fuses, or prints key material. Plaintext hashes and rebuilt
loader bytes are pinned; successful reconstruction is not a handset boot pass.
"""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("dump", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument('--xloader', type=Path,
                    default=Path(__file__).resolve().parents[3] / 'out/anne-device-private/el2-firmware/xloader.img',
                    help='Pinned signed xloader; defaults to the existing Ame private firmware bundle')
args = parser.parse_args()
os.umask(0o077)
folder = args.dump
xloader = args.xloader.read_bytes()
if hashlib.sha256(xloader).hexdigest() != '07c2ec41717b532b5f8d27968111539901854afacb0c0f35ac8b85dc08be0e81':
    raise ValueError('Unreviewed signed xloader')
args.output.mkdir(parents=True, exist_ok=True, mode=0o700)
shared = (folder / 'tee-shared-ram.bin').read_bytes()
if len(shared) != 0xa00:
    raise ValueError('Expected 0xa00 bytes from 0x367ff600')
# Factory get_encrpyted_img uses teeos_share operation index zero.
# Its dispatch resolves to 0x367fff00: first 16 bytes are SHA-256 input.
seed = shared[0x900:0x910]
scalar = int.from_bytes(hashlib.sha256(seed).digest(), 'big')
private = ec.derive_private_key(scalar, ec.SECP256R1())
expected = {'trustfirmware': '27c06575c5af056fdbedc9385d4bd3c460459f5075ee79dcb65762f71fa5eb85',
            'teeos': '00699c5b984be77db180dc274b567f981d9c17b8465d7efc351456f9107ae239'}
for name in ['trustfirmware', 'teeos']:
    raw = (folder / (name + '.img')).read_bytes()
    public_bytes = raw[0x20:0x61]
    public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), public_bytes)
    secret = private.exchange(ec.ECDH(), public)
    length = struct.unpack_from('<I', raw, 0xb4)[0]
    if not length or length % 16 or length + 256 > len(raw):
        raise ValueError("Invalid encrypted payload length")
    # CRYS_RSA_OAEPMGF1 receives public point || ECDH result and hash mode 2.
    wrapping = hashlib.sha256(public_bytes + secret + bytes(4)).digest()
    decryptor = Cipher(algorithms.AES(wrapping), modes.CBC(raw[0x64:0x74])).decryptor()
    key_iv = decryptor.update(raw[0x74:0xb4]) + decryptor.finalize()
    decryptor = Cipher(algorithms.AES(key_iv[:32]), modes.CBC(key_iv[32:48])).decryptor()
    plain = decryptor.update(raw[256:256+length]) + decryptor.finalize()
    digest = hashlib.sha256(plain).hexdigest()
    if digest != expected[name]:
        raise ValueError('Recovered plaintext hash mismatch: ' + name)
    (args.output / (name + '-decrypted.bin')).write_bytes(plain)
    print(name, 'bytes',len(plain),'sha256',digest,'known-image-match',digest == expected[name])

original = (folder / 'fastboot.img').read_bytes()
if hashlib.sha256(original).hexdigest() != '16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52':
    raise ValueError('Unreviewed stock LK')
loader = bytearray(original)
for offset, before, after in [(0x10b634, 0x528078a1, 0x52807921),
                               (0x21550, 0x340002e0, 0xd503201f),
                               (0x10bd3c, 0xa9be7bfd, 0x1406c0b1)]:
    if struct.unpack_from('<I', loader, offset)[0] != before:
        raise ValueError('Unexpected original patch instruction')
    struct.pack_into('<I', loader, offset, after)
# AArch64: source 0x01000000, destination 0x37200000, bytes 0x283100;
# ldp/stp 16 bytes, decrement/count loop; return zero, preserving callee state.
stub = bytes.fromhex('0920a0d20ae4a6d20b2086d20b05a0f22c35c1a84c3581a86b4100f1a1ffff5400008052c0035fd6')
if struct.unpack_from('<Q', loader, 0x3e0)[0] != 0x2bbcb0:
    raise ValueError('Unexpected linked extent')
loader[0x2bc000:0x2bc000+len(stub)] = stub
lk_hash = hashlib.sha256(loader).hexdigest()
if lk_hash != 'fdcdd361f4ec8ef30b756c13dae054993019127318c6ef820333e4629c9966cf':
    raise ValueError('Rebuilt loader hash mismatch')
(args.output / 'stock-lk-el2-tee.bin').write_bytes(loader)
manifest = dict(version=1, sha256=lk_hash, source_sha256=hashlib.sha256(original).hexdigest(),
    tee_sha256=expected['teeos'], patch_offset=0x10b634, patched_instruction='52807921',
    tee_staging=0x01000000, tee_upload_address=0x11000000, tee_patch_offset=0x10bd3c,
    tee_patch_instruction='1406c0b1', diagnostic_dispatch_patch=0x21550,
    tee_bytes=2633984, tee_hook=0x2bc000, tee_destination=0x37200000, stub_bytes=len(stub))
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(args.output / 'xloader.img').write_bytes(xloader)
print('Rebuilt pinned LK:', lk_hash)
print('Complete private firmware bundle prepared; reconstructed profile is not hardware-verified.')
