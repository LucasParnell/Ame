#!/usr/bin/env python3
"""Create and inspect Midix MXB1 bundles."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from signing import private_key, public_bytes, DEV_SIGNING_SEED
import struct
import sys
from pathlib import Path


MX_BUNDLE_MAGIC = 0x3142584D
MX_BUNDLE_VERSION = 2
MX_SIGNED_MANIFEST_MAGIC = 0x314D584D
MX_SIGNED_MANIFEST_VERSION = 2

MX_HASH_SHA256 = 1
MX_CHECKSUM_CRC32C = 1
MX_SIGNATURE_ED25519 = 1

MX_BUNDLE_OBJECT_EXEC = 1
MX_BUNDLE_OBJECT_LIBRARY = 2
MX_BUNDLE_OBJECT_RESOURCE = 3
MX_BUNDLE_OBJECT_MANIFEST = 4

BUNDLE_HEADER_SIZE = 128
BUNDLE_OBJECT_SIZE = 80
SIGNED_MANIFEST_HEADER_SIZE = 128
BUNDLE_CRC_OFFSET = 64

KIND_BY_NAME = {
    "exec": MX_BUNDLE_OBJECT_EXEC,
    "library": MX_BUNDLE_OBJECT_LIBRARY,
    "resource": MX_BUNDLE_OBJECT_RESOURCE,
    "manifest": MX_BUNDLE_OBJECT_MANIFEST,
}

DEV_SIGNING_SEED = hashlib.sha256(b"Midix MXB1 development signing seed v1").digest()

CRC32C_TABLE = []
for _i in range(256):
    _crc = _i
    for _ in range(8):
        _crc = ((_crc >> 1) ^ 0x82F63B78) & 0xFFFFFFFF if (_crc & 1) else (_crc >> 1)
    CRC32C_TABLE.append(_crc & 0xFFFFFFFF)


def align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = ((crc >> 8) ^ CRC32C_TABLE[(crc ^ b) & 0xFF]) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def crc32c_with_zeroed_field(data: bytes, offset: int, size: int) -> int:
    mutable = bytearray(data)
    mutable[offset : offset + size] = b"\x00" * size
    return crc32c(bytes(mutable))


def sign_manifest_body(body: bytes) -> tuple[bytes, bytes]:
    development = os.environ.get("AME_BUILD_DEV", "1") == "1"
    key = private_key(development=development)
    return public_bytes(key.public_key()), key.sign(body)


def signed_manifest(body: bytes) -> bytes:
    if not body.endswith(b"\x00"):
        body += b"\x00"
    public_key, signature = sign_manifest_body(body)
    public_key_id = hashlib.sha256(public_key).digest()
    body_hash = hashlib.sha256(body).digest()
    body_offset = SIGNED_MANIFEST_HEADER_SIZE
    signature_offset = body_offset + len(body)
    header = struct.pack(
        "<IHHIIIIQQQQ32s32s8s",
        MX_SIGNED_MANIFEST_MAGIC,
        MX_SIGNED_MANIFEST_VERSION,
        SIGNED_MANIFEST_HEADER_SIZE,
        0,
        MX_HASH_SHA256,
        MX_SIGNATURE_ED25519,
        len(public_key_id),
        body_offset,
        len(body),
        signature_offset,
        len(signature),
        body_hash,
        public_key_id,
        b"\x00" * 8,
    )
    if len(header) != SIGNED_MANIFEST_HEADER_SIZE:
        raise AssertionError(len(header))
    return header + body + signature


def parse_object_arg(raw: str) -> tuple[str, str, Path]:
    parts = raw.split(":", 2)
    if len(parts) != 3:
        raise SystemExit(f"bad object spec {raw!r}; expected kind:name:path")
    kind, name, path = parts
    if kind not in KIND_BY_NAME:
        raise SystemExit(f"bad object kind {kind!r}")
    if not name:
        raise SystemExit("object name cannot be empty")
    return kind, name, Path(path)


def build_bundle(output: Path, manifest: Path, objects: list[str],
                 manifest_name: str = "manifest.json") -> None:
    if not manifest_name:
        raise SystemExit("manifest object name cannot be empty")
    entries: list[dict[str, object]] = [
        {
            "kind": "manifest",
            "name": manifest_name,
            "data": b"",
            "alignment": 8,
        }
    ]

    for raw in objects:
        kind, name, path = parse_object_arg(raw)
        entries.append(
            {
                "kind": kind,
                "name": name,
                "data": path.read_bytes(),
                "alignment": 4096 if kind in ("exec", "library") else 8,
            }
        )

    names = [str(e["name"]) for e in entries]
    if manifest_name != "manifest.json" or len(entries) > 64 or len(set(names)) != len(names):
        raise ValueError("invalid manifest name, duplicate names, or too many objects")
    if any(not n.isascii() or not n or len(n) >= 100 or any(ord(c) < 32 for c in n) for n in names):
        raise ValueError("invalid object name")
    if any(e["kind"] == "manifest" for e in entries[1:]):
        raise ValueError("only one manifest is allowed")
    doc = json.loads(manifest.read_text())
    doc["schema"] = 2
    doc["catalog"] = [{"name": e["name"], "kind": KIND_BY_NAME[e["kind"]],
                       "size": len(e["data"]), "sha256": hashlib.sha256(e["data"]).hexdigest()}
                      for e in entries[1:]]
    body = json.dumps(doc, sort_keys=True, separators=(",", ":")).encode()
    if len(body) + 1 > 65536: raise ValueError("manifest exceeds 64 KiB")
    entries[0]["data"] = signed_manifest(body)
    strings = bytearray(b"\x00")
    for entry in entries:
        encoded = str(entry["name"]).encode("utf-8") + b"\x00"
        entry["name_offset"] = len(strings)
        strings.extend(encoded)

    object_table_offset = BUNDLE_HEADER_SIZE
    string_table_offset = object_table_offset + len(entries) * BUNDLE_OBJECT_SIZE
    data_offset = align_up(string_table_offset + len(strings), 4096)

    cursor = data_offset
    for entry in entries:
        alignment = int(entry["alignment"])
        cursor = align_up(cursor, alignment)
        entry["offset"] = cursor
        cursor += len(entry["data"])  # type: ignore[arg-type]

    bundle_size = cursor
    blob = bytearray(bundle_size)
    blob[string_table_offset : string_table_offset + len(strings)] = strings

    for entry in entries:
        data = entry["data"]  # type: ignore[assignment]
        offset = int(entry["offset"])
        blob[offset : offset + len(data)] = data  # type: ignore[arg-type]

    manifest_offset = int(entries[0]["offset"])
    manifest_size = len(entries[0]["data"])  # type: ignore[arg-type]
    header = struct.pack(
        "<IHHIIQQQQQQIIII48s",
        MX_BUNDLE_MAGIC,
        MX_BUNDLE_VERSION,
        BUNDLE_HEADER_SIZE,
        0,
        len(entries),
        object_table_offset,
        manifest_offset,
        manifest_size,
        string_table_offset,
        len(strings),
        bundle_size,
        0,
        MX_HASH_SHA256,
        MX_SIGNATURE_ED25519,
        MX_CHECKSUM_CRC32C,
        b"\x00" * 48,
    )
    if len(header) != BUNDLE_HEADER_SIZE:
        raise AssertionError(len(header))
    blob[:BUNDLE_HEADER_SIZE] = header

    for index, entry in enumerate(entries):
        data = entry["data"]  # type: ignore[assignment]
        record = struct.pack(
            "<IIQQQ32sIIQ",
            int(entry["name_offset"]),
            KIND_BY_NAME[str(entry["kind"])],
            int(entry["offset"]),
            len(data),  # type: ignore[arg-type]
            int(entry["alignment"]),
            hashlib.sha256(data).digest(),  # type: ignore[arg-type]
            crc32c(data),  # type: ignore[arg-type]
            0,
            0,
        )
        if len(record) != BUNDLE_OBJECT_SIZE:
            raise AssertionError(len(record))
        start = object_table_offset + index * BUNDLE_OBJECT_SIZE
        blob[start : start + BUNDLE_OBJECT_SIZE] = record

    bundle_crc = crc32c_with_zeroed_field(bytes(blob), BUNDLE_CRC_OFFSET, 4)
    struct.pack_into("<I", blob, BUNDLE_CRC_OFFSET, bundle_crc)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(blob))


def list_bundle(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < BUNDLE_HEADER_SIZE:
        raise SystemExit(f"{path}: too small")
    header = struct.unpack_from("<IHHIIQQQQQQIIII48s", data, 0)
    if header[0] != MX_BUNDLE_MAGIC:
        raise SystemExit(f"{path}: not MXB1")
    object_count = header[4]
    object_table_offset = header[5]
    string_table_offset = header[8]
    string_table_size = header[9]
    strings = data[string_table_offset : string_table_offset + string_table_size]
    for i in range(object_count):
        off = object_table_offset + i * BUNDLE_OBJECT_SIZE
        rec = struct.unpack_from("<IIQQQ32sIIQ", data, off)
        name_off = rec[0]
        end = strings.find(b"\x00", name_off)
        if end < 0:
            name = f"<bad-name-{i}>"
        else:
            name = strings[name_off:end].decode("utf-8", "replace")
        print(name)


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd")

    create = sub.add_parser("create")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--manifest", type=Path, required=True)
    create.add_argument("--manifest-name", default="manifest.json")
    create.add_argument("--object", action="append", default=[])

    list_cmd = sub.add_parser("list")
    list_cmd.add_argument("bundle", type=Path)

    args = parser.parse_args()
    if args.cmd == "create":
        build_bundle(args.output, args.manifest, args.object, args.manifest_name)
    elif args.cmd == "list":
        list_bundle(args.bundle)
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
