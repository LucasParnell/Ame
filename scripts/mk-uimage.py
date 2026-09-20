#!/usr/bin/env python3
"""Create a U-Boot legacy uImage wrapper for Ame's ARM64 Image."""

from __future__ import annotations

import argparse
import binascii
import struct
import time
from pathlib import Path


IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM64 = 22
IMAGE_TYPES = {
    "kernel": 2,
    "ramdisk": 3,
}
IH_COMP_NONE = 0
IH_NMLEN = 32


def parse_u32(value: str) -> int:
    parsed = int(value, 0)
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"{value!r} is outside uint32 range")
    return parsed


def build_header(data: bytes, load: int, entry: int, image_type: int, name: str) -> bytes:
    encoded_name = name.encode("ascii", "replace")[:IH_NMLEN]
    encoded_name = encoded_name.ljust(IH_NMLEN, b"\0")
    timestamp = int(time.time())
    data_crc = binascii.crc32(data) & 0xFFFFFFFF

    header_without_crc = struct.pack(
        ">7I4B32s",
        IH_MAGIC,
        0,
        timestamp,
        len(data),
        load,
        entry,
        data_crc,
        IH_OS_LINUX,
        IH_ARCH_ARM64,
        image_type,
        IH_COMP_NONE,
        encoded_name,
    )
    header_crc = binascii.crc32(header_without_crc) & 0xFFFFFFFF
    return struct.pack(
        ">7I4B32s",
        IH_MAGIC,
        header_crc,
        timestamp,
        len(data),
        load,
        entry,
        data_crc,
        IH_OS_LINUX,
        IH_ARCH_ARM64,
        image_type,
        IH_COMP_NONE,
        encoded_name,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--load", required=True, type=parse_u32)
    parser.add_argument("--entry", required=True, type=parse_u32)
    parser.add_argument("--type", choices=sorted(IMAGE_TYPES), default="kernel")
    parser.add_argument("--name", default="Ame")
    args = parser.parse_args()

    data = args.input.read_bytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(
        build_header(data, args.load, args.entry, IMAGE_TYPES[args.type], args.name) + data
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
