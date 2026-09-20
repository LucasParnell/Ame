#!/usr/bin/env python3
"""Convert a final AArch64 ELF executable into a Midix MXI1 image."""

from __future__ import annotations

import argparse
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


def build_mxi(elf_path: Path, kind: str, stack_size: int) -> bytes:
    entry, segments = parse_elf(elf_path)

    segment_table_offset = IMAGE_HEADER_SIZE
    cursor = segment_table_offset + len(segments) * SEGMENT_SIZE
    cursor = align_up(cursor, PAGE_SIZE)

    for seg in segments:
        cursor = align_up(cursor, PAGE_SIZE)
        seg["file_offset"] = cursor
        cursor += seg["file_size"]

    image_size = cursor
    blob = bytearray(image_size)

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
        MX_IMAGE_FLAG_STATIC,
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
        b"\x00" * 8,
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
    args = parser.parse_args()

    image = build_mxi(args.input, args.kind, args.stack_size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
