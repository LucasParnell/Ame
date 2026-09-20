#!/usr/bin/env python3
"""Validate an AArch64 kernel's entry against generated board metadata."""
import argparse
import json
from pathlib import Path
import struct

def check(build, board):
    metadata = json.loads((build/'board.json').read_text())
    if metadata['board'] != board: raise ValueError('build board does not match package board')
    elf = (build/'kernel/anko').read_bytes()
    if elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 183:
        raise ValueError('expected a little-endian ELF64 AArch64 kernel')
    entry = struct.unpack_from('<Q', elf, 24)[0]
    if entry != int(metadata['load_base'], 0): raise ValueError('ELF entry differs from configured load address')
    image = (build/'kernel/anko.bin').read_bytes()
    if len(image) < 64 or image[56:60] != b'ARM\x64': raise ValueError('invalid ARM64 Image header')
    if struct.unpack_from('<Q', image, 8)[0] != int(metadata['text_offset'],0):
        raise ValueError('Image text offset differs from configured board')
    return entry

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--board',required=True)
    args=p.parse_args()
    print(hex(check(args.build_dir,args.board)))
