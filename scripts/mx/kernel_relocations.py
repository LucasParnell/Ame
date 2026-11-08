#!/usr/bin/env python3
"""Fill the kernel's bounded rebasing table in both ELF and raw Image."""
import argparse
from pathlib import Path
import struct
from elf_relocations import relocations

def patch(elf_path, binary_path):
    elf = bytearray(elf_path.read_bytes())
    h = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
    sections = [struct.unpack_from('<IIQQQQIIQQ', elf, h[6]+64*i) for i in range(h[12])]
    symbols = {}
    for section in sections:
        if section[1] != 2: continue
        strings = sections[section[6]]; names = elf[strings[4]:strings[4]+strings[5]]
        for position in range(section[4], section[4]+section[5], 24):
            name, _, _, index, address, size = struct.unpack_from('<IBBHQQ', elf, position)
            symbols[bytes(names[name:names.index(0, name)]).decode()] = (address, index, size)
    start = symbols['__text_start'][0]; end = symbols['__kernel_end'][0]
    address, index, _ = symbols['kaslr_table']; section = sections[index]
    records = relocations(elf, start, end)
    if len(records) > 8192: raise ValueError('kernel relocation table exhausted')
    table = struct.pack('<IIQQ', 0x4b41534c, len(records), start, end-start)
    table += b''.join(struct.pack('<II', location-start, width) for location, _, width in records)
    file_offset = section[4] + address - section[3]
    elf[file_offset:file_offset+len(table)] = table
    binary = bytearray(binary_path.read_bytes()); offset = address - start
    binary[offset:offset+len(table)] = table
    elf_path.write_bytes(elf); binary_path.write_bytes(binary)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path); parser.add_argument('binary', type=Path)
    args = parser.parse_args(); patch(args.elf, args.binary)
