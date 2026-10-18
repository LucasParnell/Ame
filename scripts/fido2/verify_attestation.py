#!/usr/bin/env python3
"""Verify Ame's packed attestation over CTAP2, optionally with ClientPIN.

This is an independent check: it parses the CTAP response, verifies the
attestation signature with the leaf certificate, and optionally verifies the
leaf against a development CA. It does not use Safari or WebAuthn.
"""
import argparse
import hashlib
import os
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.x509.oid import ObjectIdentifier

from host import MacHid, decode, encode

AAGUID_OID = ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4")


def status(result, what):
    if not result or result[0]:
        raise RuntimeError(f"{what}: status {result[:1].hex() or 'empty'}")
    return result[1:]


def get_info(hid, cid):
    payload = status(hid.transact(cid, 0x90, b"\x04"), "GetInfo")
    values, end = decode(payload)
    if end != len(payload):
        raise RuntimeError("GetInfo: trailing bytes")
    return values


def pin_token(hid, cid, pin):
    values, _ = decode(status(hid.transact(cid, 0x90, b"\x06" + encode({1: 1, 2: 2})),
                              "ClientPIN getKeyAgreement"))
    cose = values[2]
    authenticator_key = ec.EllipticCurvePublicNumbers(
        int.from_bytes(cose[-2], "big"),
        int.from_bytes(cose[-3], "big"),
        ec.SECP256R1(),
    ).public_key()
    platform_key = ec.generate_private_key(ec.SECP256R1())
    shared = hashlib.sha256(platform_key.exchange(ec.ECDH(), authenticator_key)).digest()
    pin_hash = hashlib.sha256(pin.encode()).digest()[:16]
    encryptor = Cipher(algorithms.AES(shared), modes.CBC(bytes(16))).encryptor()
    encrypted_pin = encryptor.update(pin_hash) + encryptor.finalize()
    numbers = platform_key.public_key().public_numbers()
    platform_cose = {
        1: 2, 3: -25, -1: 1,
        -2: numbers.x.to_bytes(32, "big"),
        -3: numbers.y.to_bytes(32, "big"),
    }
    values, _ = decode(status(hid.transact(
        cid, 0x90, b"\x06" + encode({1: 1, 2: 5, 3: platform_cose, 6: encrypted_pin})),
        "ClientPIN getPINToken"))
    decryptor = Cipher(algorithms.AES(shared), modes.CBC(bytes(16))).decryptor()
    return decryptor.update(values[2]) + decryptor.finalize()


def pin_auth(token, client_hash):
    mac = hmac.HMAC(token, hashes.SHA256())
    mac.update(client_hash)
    return mac.finalize()[:16]


def register(hid, cid, pin=None, rp="ame-attest.local"):
    client_hash = os.urandom(32)
    request = {
        1: client_hash,
        2: {"id": rp},
        3: {"id": os.urandom(16), "name": "Ame attestation test"},
        4: [{"alg": -7, "type": "public-key"}],
    }
    if pin is not None:
        request[8] = pin_auth(pin_token(hid, cid, pin), client_hash)
        request[9] = 1
    response, _ = decode(status(hid.transact(cid, 0x90, b"\x01" + encode(request), 45),
                               "MakeCredential"))
    return client_hash, response


def verify(args):
    hid = MacHid()
    try:
        nonce = os.urandom(8)
        result = hid.transact(0xffffffff, 0x86, nonce)
        if len(result) != 17 or result[:8] != nonce:
            raise RuntimeError("INIT: invalid response")
        cid = int.from_bytes(result[8:12], "big")
        info = get_info(hid, cid)
        print(f"aaguid={info[3].hex()}")
        print("registration: release VOLUME UP, then press it when prompted", flush=True)
        client_hash, response = register(hid, cid, args.pin, args.rp)
        fmt = response[1]
        auth = response[2]
        statement = response[3]
        print(f"fmt={fmt}")
        print(f"alg={statement.get('alg')}")
        print(f"aaguid.authdata={auth[37:53].hex()}")
        if fmt != "packed":
            raise RuntimeError(f"expected packed attestation, got {fmt!r}")
        if statement.get("alg") != -7:
            raise RuntimeError("expected ES256 attestation")
        if auth[37:53] != info[3]:
            raise RuntimeError("authData AAGUID differs from GetInfo AAGUID")
        certificate = statement.get("x5c")
        if not certificate:
            print("x5c=absent (self-attestation)")
            print("signature=unverified: no leaf certificate")
            return 1
        print(f"x5c={len(certificate)} certificate(s), {len(certificate[0])} bytes")
        leaf = x509.load_der_x509_certificate(certificate[0])
        leaf.public_key().verify(
            statement["sig"], auth + client_hash, ec.ECDSA(hashes.SHA256()))
        print("signature=verified")
        extension = leaf.extensions.get_extension_for_oid(AAGUID_OID).value
        if extension.value != b"\x04\x10" + auth[37:53]:
            raise RuntimeError("certificate AAGUID extension differs from authData")
        print("aaguid.extension=verified")
        if args.ca:
            ca = x509.load_pem_x509_certificate(args.ca.read_bytes())
            ca.public_key().verify(leaf.signature, leaf.tbs_certificate_bytes,
                                   ec.ECDSA(hashes.SHA256()))
            print("chain=verified")
        else:
            print("chain=not checked (pass --ca)")
        return 0
    finally:
        hid.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pin", help="ClientPIN, if the authenticator has one configured")
    parser.add_argument("--ca", type=Path, help="development CA certificate (PEM)")
    parser.add_argument("--rp", default="ame-attest.local")
    args = parser.parse_args()
    try:
        return verify(args)
    except Exception as error:
        print(f"verify_attestation.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
