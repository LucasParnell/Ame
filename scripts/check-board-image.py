#!/usr/bin/env python3
"""Validate an AArch64 kernel's physical entry against board metadata."""
import argparse
import json
from pathlib import Path
import struct

def check(build, board):
    metadata = json.loads((build/'board.json').read_text())
    if metadata['board'] != board: raise ValueError('build board does not match package board')
    elf = (build/'kernel/anko').read_bytes()
    if len(elf) < 64 or elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 183:
        raise ValueError('expected a little-endian ELF64 AArch64 kernel')
    entry = struct.unpack_from('<Q', elf, 24)[0]
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', elf, 54)
    if phsize != 56 or not phnum or phoff + phsize * phnum > len(elf):
        raise ValueError('invalid ELF program header table')
    entries = []
    for index in range(phnum):
        kind, flags, offset, virtual, physical, filesz, memsz, _ = struct.unpack_from(
            '<II6Q', elf, phoff + index * phsize)
        if kind != 1:
            continue
        if filesz > memsz or offset + filesz > len(elf):
            raise ValueError('invalid ELF load segment')
        if flags & 1 and virtual <= entry and entry + 64 <= virtual + filesz:
            entries.append((physical + entry - virtual, offset + entry - virtual))
    if len(entries) != 1:
        raise ValueError('ELF entry must be backed by one executable load segment')
    physical_entry, entry_offset = entries[0]
    if physical_entry != int(metadata['load_base'], 0):
        raise ValueError('ELF physical entry differs from configured load address')
    image = (build/'kernel/anko.bin').read_bytes()
    if len(image) < 64 or image[56:60] != b'ARM\x64': raise ValueError('invalid ARM64 Image header')
    if image[:64] != elf[entry_offset:entry_offset + 64]:
        raise ValueError('Image header differs from ELF entry contents')
    if struct.unpack_from('<Q', image, 8)[0] != int(metadata['text_offset'],0):
        raise ValueError('Image text offset differs from configured board')
    return physical_entry

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--board',required=True)
    args=p.parse_args()
    print(hex(check(args.build_dir,args.board)))
