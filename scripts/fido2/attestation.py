#!/usr/bin/env python3
"""Issue Ame's packed-attestation certificate and emit a C++ header.

The attestation private key is derived from the same 32-byte master seed used
by seed.py. Only the public certificate and public key are written out; the
private key never leaves this process.
"""
import argparse
import hashlib
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID, ObjectIdentifier

AAGUID_OID = ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4")
P256_ORDER = int(
    "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16
)
NOT_BEFORE = datetime(2020, 1, 1, tzinfo=timezone.utc)
NOT_AFTER = datetime(2046, 1, 1, tzinfo=timezone.utc)


def derive(master: bytes, domain: int, parts=()) -> bytes:
    """Mirror Authenticator::derive: keyed BLAKE2b, domain, three length-prefixed slots."""
    digest = hashlib.blake2b(digest_size=32, key=master)
    digest.update(bytes([domain]))
    slots = list(parts) + [b""] * (3 - len(parts))
    if len(slots) != 3:
        raise ValueError("derive accepts at most three parts")
    for part in slots:
        digest.update(len(part).to_bytes(8, "little"))
        digest.update(part)
    return digest.digest()


def attestation_key(master: bytes):
    """Derive the P-256 attestation key with the same retry rule as credentials."""
    if len(master) != 32 or not any(master):
        raise ValueError("seed must contain exactly 32 non-zero bytes")
    for attempt in range(16):
        parts = () if attempt == 0 else (bytes([attempt]),)
        secret = derive(master, 5, parts)
        value = int.from_bytes(secret, "big")
        if 0 < value < P256_ORDER:
            return secret, ec.derive_private_key(value, ec.SECP256R1())
    raise ValueError("could not derive a valid P-256 attestation key")


def public_key_bytes(private_key) -> bytes:
    numbers = private_key.public_key().public_numbers()
    return numbers.x.to_bytes(32, "big") + numbers.y.to_bytes(32, "big")


def read_aaguid(path: Path) -> bytes:
    text = path.read_text()
    match = re.search(r"kAaguid\s*\[16\]\s*=\s*\{([^}]*)\}", text, re.S)
    if not match:
        raise ValueError(f"kAaguid[16] not found in {path}")
    values = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]{2}", match.group(1))]
    if len(values) != 16:
        raise ValueError(f"expected 16 AAGUID bytes in {path}, found {len(values)}")
    aaguid = bytes(values)
    if aaguid == bytes(16):
        raise ValueError("AAGUID must not be all zero")
    return aaguid


def subject(common_name: str) -> x509.Name:
    return x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "GB"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Ame"),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, "FIDO2"),
        x509.NameAttribute(NameOID.COMMON_NAME, common_name),
    ])


def key_usage(ca: bool) -> x509.KeyUsage:
    return x509.KeyUsage(
        digital_signature=True,
        content_commitment=False,
        key_encipherment=False,
        data_encipherment=False,
        key_agreement=False,
        key_cert_sign=ca,
        crl_sign=ca,
        encipher_only=False,
        decipher_only=False,
    )


def write_private_key(path: Path, key) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    path.write_bytes(data)
    path.chmod(0o600)


def write_certificate(path: Path, cert) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))


def c_array(name: str, data: bytes, constant: bool = False) -> str:
    rows = []
    for offset in range(0, len(data), 12):
        rows.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 12]) + ",")
    qualifier = "inline constexpr " if constant else ""
    return f"{qualifier}mx_u8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n"


def emit_header(path: Path, certificate: bytes, public_key: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = [
        "#pragma once\n",
        '#include "midix/abi/types.h"\n',
        "namespace mochi::fido2 {\n",
        c_array("kAttestationCertificate", certificate, constant=True),
        "inline constexpr mx_u32_t kAttestationCertificateSize = "
        "static_cast<mx_u32_t>(sizeof(kAttestationCertificate));\n",
        c_array("kAttestationPublicKey", public_key, constant=True),
        "static_assert(kAttestationCertificateSize <= 1024,\n"
        '              "attestation certificate exceeds response budget");\n',
        "static_assert(sizeof(kAttestationPublicKey) == 64,\n"
        '              "attestation public key must be 64 bytes");\n',
        "} // namespace mochi::fido2\n",
    ]
    path.write_text("".join(body))


def load_ca(key_path: Path, cert_path: Path):
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    cert = x509.load_pem_x509_certificate(cert_path.read_bytes())
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise ValueError("CA key must be P-256")
    return key, cert


def cmd_init_ca(args) -> int:
    key = ec.generate_private_key(ec.SECP256R1())
    name = subject("Ame FIDO2 Development CA")
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(NOT_BEFORE)
        .not_valid_after(NOT_AFTER)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(key_usage(ca=True), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .sign(key, hashes.SHA256())
    )
    write_private_key(args.key, key)
    write_certificate(args.cert, cert)
    print(f"wrote CA key {args.key} and certificate {args.cert}")
    return 0


def cmd_issue(args) -> int:
    master = args.seed.read_bytes()
    aaguid = read_aaguid(args.aaguid_header)
    _, key = attestation_key(master)
    ca_key, ca_cert = load_ca(args.ca_key, args.ca_cert)
    public_key = public_key_bytes(key)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject("Ame FIDO2 Attestation"))
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(NOT_BEFORE)
        .not_valid_after(NOT_AFTER)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(key_usage(ca=False), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.UnrecognizedExtension(AAGUID_OID, b"\x04\x10" + aaguid),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )
    der = cert.public_bytes(serialization.Encoding.DER)
    if len(der) > 1024:
        raise ValueError(f"certificate is {len(der)} bytes; limit is 1024")
    emit_header(args.out_header, der, public_key)
    args.out_cert.parent.mkdir(parents=True, exist_ok=True)
    args.out_cert.write_bytes(der)
    print(f"wrote {args.out_header} ({len(der)}-byte certificate) and {args.out_cert}")
    return 0


def cmd_public(args) -> int:
    _, key = attestation_key(args.seed.read_bytes())
    print(public_key_bytes(key).hex())
    return 0


def cmd_verify(args) -> int:
    cert = x509.load_der_x509_certificate(args.cert.read_bytes())
    ca = x509.load_pem_x509_certificate(args.ca.read_bytes())
    aaguid = read_aaguid(args.aaguid_header)
    if cert.subject.get_attributes_for_oid(NameOID.COUNTRY_NAME)[0].value != "GB":
        raise ValueError("unexpected subject country")
    for oid in (NameOID.ORGANIZATION_NAME, NameOID.ORGANIZATIONAL_UNIT_NAME, NameOID.COMMON_NAME):
        if not cert.subject.get_attributes_for_oid(oid):
            raise ValueError(f"missing subject field {oid}")
    constraints = cert.extensions.get_extension_for_class(x509.BasicConstraints).value
    if constraints.ca:
        raise ValueError("leaf certificate must have CA=false")
    usage = cert.extensions.get_extension_for_class(x509.KeyUsage).value
    if not usage.digital_signature or usage.key_cert_sign:
        raise ValueError("leaf key usage must be digitalSignature only")
    extension = cert.extensions.get_extension_for_oid(AAGUID_OID).value
    if extension.value != b"\x04\x10" + aaguid:
        raise ValueError("certificate AAGUID extension does not match kAaguid")
    ca.public_key().verify(
        cert.signature,
        cert.tbs_certificate_bytes,
        ec.ECDSA(hashes.SHA256()),
    )
    print(f"certificate OK: {len(cert.public_bytes(serialization.Encoding.DER))} bytes, AAGUID {aaguid.hex()}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init-ca", help="create the development attestation CA")
    init.add_argument("--key", type=Path, required=True)
    init.add_argument("--cert", type=Path, required=True)
    init.set_defaults(func=cmd_init_ca)

    issue = sub.add_parser("issue", help="issue a leaf certificate and C++ header")
    issue.add_argument("--seed", type=Path, required=True)
    issue.add_argument("--aaguid-header", type=Path, required=True)
    issue.add_argument("--ca-key", type=Path, required=True)
    issue.add_argument("--ca-cert", type=Path, required=True)
    issue.add_argument("--out-header", type=Path, required=True)
    issue.add_argument("--out-cert", type=Path, required=True)
    issue.set_defaults(func=cmd_issue)

    public = sub.add_parser("public", help="print the derived public key")
    public.add_argument("--seed", type=Path, required=True)
    public.set_defaults(func=cmd_public)

    verify = sub.add_parser("verify", help="verify a leaf certificate profile")
    verify.add_argument("--cert", type=Path, required=True)
    verify.add_argument("--ca", type=Path, required=True)
    verify.add_argument("--aaguid-header", type=Path, required=True)
    verify.set_defaults(func=cmd_verify)

    args = parser.parse_args()
    try:
        return args.func(args)
    except Exception as error:
        print(f"attestation.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
