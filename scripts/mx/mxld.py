#!/usr/bin/env python3
"""Convert a final AArch64 ELF executable into a Midix MXI1 image."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path


MX_IMAGE_MAGIC = 0x3149584D
MX_IMAGE_VERSION = 1
MX_IMAGE_ABI_VERSION = 2
MX_IMAGE_KIND_EXEC = 1
MX_IMAGE_KIND_LIBRARY = 2
MX_IMAGE_ARCH_AARCH64 = 1
MX_IMAGE_FLAG_STATIC = 1 << 0
MX_IMAGE_FLAG_ARENA = 1 << 1
MX_IMAGE_FLAG_ASLR = 1 << 2
DEFAULT_ARENA = {"base": 0x38000000, "initial": 65536, "reserve": 65536,
                 "limit": 1048576, "type": 1, "compact": False}


def arena_declaration(elf_path: Path, manifest: Path | None = None, service: str | None = None, profile: str | None = None) -> dict:
    declaration = dict(DEFAULT_ARENA)
    if manifest:
        document = json.loads(manifest.read_text())
        if profile:
            from system_descriptor import load_profile
            document = load_profile(manifest, profile)
        declaration.update(document.get("arena_defaults", {}))
        candidates = document.get("services", []) + document.get("applications", [])
        record = next((s for s in candidates if s.get("name") == service or s.get("object") == service), None)
        if service == "init": record = document.get("init", {})
        if record: declaration.update(record.get("arena", {}))
    for key in ("base", "initial", "reserve", "limit", "type"):
        if type(declaration.get(key)) is not int: raise ValueError(f"arena {key}: expected integer")
    a = declaration
    if (not 0 < a["initial"] <= a["reserve"] <= a["limit"] <= 1048576 or
            not 0x10010000 <= a["base"] < 0x40000000 or a["limit"] > 0x40000000 - a["base"] or
            any(a[k] % 4096 for k in ("base", "initial", "reserve", "limit")) or
            not 0 < a["type"] < 1 << 64 or type(a.get("compact")) is not bool):
        raise ValueError(f"{elf_path}: invalid arena declaration")
    return a
MX_CHECKSUM_CRC32C = 1

MX_RIGHT_READ = 1 << 0
MX_RIGHT_WRITE = 1 << 1
MX_RIGHT_EXEC = 1 << 3

MX_IMAGE_SEG_ZERO_FILL = 1 << 0

ELF_MAGIC = b"\x7fELF"
ET_EXEC = 2
ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4

PAGE_SIZE = 4096
IMAGE_HEADER_SIZE = 128
SEGMENT_SIZE = 64
IMAGE_CRC_OFFSET = 112

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


def verify_arena_contract(path: Path, declaration: dict) -> None:
    data = path.read_bytes()
    if len(data) < IMAGE_HEADER_SIZE or struct.unpack_from('<I', data)[0] != MX_IMAGE_MAGIC:
        raise ValueError(f"{path}: missing MXI arena contract")
    flags = struct.unpack_from('<I', data, 20)[0]
    offset = struct.unpack_from('<Q', data, 120)[0]
    expected = struct.pack('<QQQQQII', declaration['initial'], declaration['reserve'],
        declaration['base'], declaration['limit'], declaration['type'], int(declaration['compact']), 0)
    if not flags & MX_IMAGE_FLAG_ARENA or offset > len(data) or data[offset:offset + 48] != expected:
        raise ValueError(f"{path}: executable arena differs from its signed manifest")
    if struct.unpack_from('<I', data, IMAGE_CRC_OFFSET)[0] != crc32c_with_zeroed_field(data, IMAGE_CRC_OFFSET, 4):
        raise ValueError(f"{path}: invalid executable checksum")


def parse_elf(path: Path) -> tuple[int, list[dict[str, int]]]:
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != ELF_MAGIC:
        raise SystemExit(f"{path}: not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise SystemExit(f"{path}: expected ELF64 little-endian")

    eh = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    e_type = eh[1]
    e_machine = eh[2]
    e_entry = eh[4]
    e_phoff = eh[5]
    e_phentsize = eh[9]
    e_phnum = eh[10]

    if e_type not in (ET_EXEC, ET_DYN):
        raise SystemExit(f"{path}: expected ET_EXEC or ET_DYN")
    if e_machine != EM_AARCH64:
        raise SystemExit(f"{path}: expected AArch64 ELF")
    if e_phentsize != 56:
        raise SystemExit(f"{path}: unexpected program-header size {e_phentsize}")
    if e_phoff + e_phnum * e_phentsize > len(data):
        raise SystemExit(f"{path}: program headers exceed file")

    segments: list[dict[str, int]] = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", data, off
        )
        if p_type != PT_LOAD:
            continue
        if p_filesz > p_memsz:
            raise SystemExit(f"{path}: PT_LOAD filesz exceeds memsz")
        if p_offset + p_filesz > len(data):
            raise SystemExit(f"{path}: PT_LOAD exceeds file")
        if p_flags & PF_W and p_flags & PF_X:
            raise SystemExit(f"{path}: refuses W+X segment")

        rights = 0
        if p_flags & PF_R:
            rights |= MX_RIGHT_READ
        if p_flags & PF_W:
            rights |= MX_RIGHT_WRITE
        if p_flags & PF_X:
            rights |= MX_RIGHT_EXEC
        if rights == 0:
            rights = MX_RIGHT_READ

        segments.append(
            {
                "src_offset": p_offset,
                "file_size": p_filesz,
                "memory_size": p_memsz,
                "virtual_address": p_vaddr,
                "alignment": PAGE_SIZE,  # MXI payloads and virtual segments are page aligned.
                "rights": rights,
                "flags": MX_IMAGE_SEG_ZERO_FILL if p_memsz > p_filesz else 0,
                "data": data[p_offset : p_offset + p_filesz],
            }
        )

    if not segments:
        raise SystemExit(f"{path}: no loadable segments")
    if not any((s["rights"] & MX_RIGHT_EXEC) and s["virtual_address"] <= e_entry < s["virtual_address"] + s["memory_size"] for s in segments):
        raise SystemExit(f"{path}: entry point is not inside an executable segment")

    return e_entry, segments


def build_mxi(elf_path: Path, kind: str, stack_size: int, arena: dict | None = None) -> bytes:
    entry, segments = parse_elf(elf_path)
    arena = arena or arena_declaration(elf_path)

    segment_table_offset = IMAGE_HEADER_SIZE
    arena_offset = segment_table_offset + len(segments) * SEGMENT_SIZE
    from elf_relocations import relocations
    minimum = min(s["virtual_address"] for s in segments)
    maximum = max(s["virtual_address"] + s["memory_size"] for s in segments)
    records = relocations(elf_path.read_bytes(), minimum, maximum)
    if len(records) > 16384: raise ValueError("too many image relocations")
    cursor = arena_offset + 48 + 24 + len(records) * 24
    cursor = align_up(cursor, PAGE_SIZE)

    for seg in segments:
        cursor = align_up(cursor, PAGE_SIZE)
        seg["file_offset"] = cursor
        cursor += seg["file_size"]

    image_size = cursor
    blob = bytearray(image_size)
    blob[arena_offset:arena_offset + 48] = struct.pack('<QQQQQII', arena['initial'], arena['reserve'],
        arena['base'], arena['limit'], arena['type'], int(arena['compact']), 0)

    layout_offset = arena_offset + 48
    struct.pack_into("<IIQQ", blob, layout_offset, 1, len(records), minimum, maximum)
    for i, (address, value, width) in enumerate(records):
        struct.pack_into("<QQII", blob, layout_offset + 24 + i * 24, address, value, width, 0)

    for seg in segments:
        blob[seg["file_offset"] : seg["file_offset"] + seg["file_size"]] = seg["data"]

    kind_value = MX_IMAGE_KIND_EXEC if kind == "exec" else MX_IMAGE_KIND_LIBRARY
    header = struct.pack(
        "<IHHIIIIQQQQIIQIIQQQQII8s",
        MX_IMAGE_MAGIC,
        MX_IMAGE_VERSION,
        IMAGE_HEADER_SIZE,
        kind_value,
        MX_IMAGE_ARCH_AARCH64,
        MX_IMAGE_ABI_VERSION,
        MX_IMAGE_FLAG_STATIC | MX_IMAGE_FLAG_ARENA | MX_IMAGE_FLAG_ASLR,
        entry,
        stack_size,
        image_size,
        segment_table_offset,
        len(segments),
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        MX_CHECKSUM_CRC32C,
        struct.pack('<Q', arena_offset),
    )
    if len(header) != IMAGE_HEADER_SIZE:
        raise AssertionError(len(header))
    blob[:IMAGE_HEADER_SIZE] = header

    for index, seg in enumerate(segments):
        record = struct.pack(
            "<QQQQQIII12s",
            seg["file_offset"],
            seg["file_size"],
            seg["memory_size"],
            seg["virtual_address"],
            seg["alignment"],
            seg["rights"],
            seg["flags"],
            0,
            b"\x00" * 12,
        )
        if len(record) != SEGMENT_SIZE:
            raise AssertionError(len(record))
        start = segment_table_offset + index * SEGMENT_SIZE
        blob[start : start + SEGMENT_SIZE] = record

    crc = crc32c_with_zeroed_field(bytes(blob), IMAGE_CRC_OFFSET, 4)
    struct.pack_into("<I", blob, IMAGE_CRC_OFFSET, crc)
    return bytes(blob)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--kind", choices=("exec", "library"), default="exec")
    parser.add_argument("--stack-size", type=lambda s: int(s, 0), default=4096)
    parser.add_argument("--memory-manifest", type=Path)
    parser.add_argument("--service")
    parser.add_argument("--memory-profile")
    args = parser.parse_args()

    image = build_mxi(args.input, args.kind, args.stack_size,
                      arena_declaration(args.input, args.memory_manifest, args.service, args.memory_profile))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
