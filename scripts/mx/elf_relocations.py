"""Extract bounded rebasing records from linked AArch64 ELF relocation sections.

Only allocated sections participate. Relative instruction encodings retain their
meaning under a page-aligned slide. Unsupported absolute encodings fail the
build rather than yielding a partly relocated image.
"""
import struct


def relocations(data: bytes, minimum: int, maximum: int) -> list[tuple[int, int, int]]:
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[2] == 183 and not header[12]: return []
    if header[2] != 183 or header[11] != 64:
        raise ValueError('expected AArch64 ELF64 section headers')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + 64*i) for i in range(header[12])]
    records = {}
    relative = {0, 260, 261, 262, 273, 274, 275, 276, 277, 278, 279, 280, 282, 283, 284, 285, 286, 299, 311, 312}
    for section in sections:
        if section[1] != 4: continue  # SHT_RELA
        if section[2] & 2 and section[5]:
            raise ValueError("dynamic relocation tables are not supported by static MXI")
        if section[7] >= len(sections) or not sections[section[7]][2] & 2: continue
        if section[6] >= len(sections) or section[9] != 24 or section[5] % 24:
            raise ValueError('invalid ELF relocation table')
        symbols = sections[section[6]]
        for offset in range(section[4], section[4]+section[5], 24):
            address, info, _addend = struct.unpack_from('<QQq', data, offset)
            kind, symbol = info & 0xffffffff, info >> 32
            if kind in relative: continue
            if kind not in (257, 258):
                raise ValueError(f'unsupported allocated relocation {kind} at {address:#x}')
            if (symbol+1)*24 > symbols[5]: raise ValueError('invalid relocation symbol')
            sym = struct.unpack_from('<IBBHQQ', data, symbols[4]+symbol*24)
            if sym[3] == 0: raise ValueError('unresolved absolute relocation')
            width = 8 if kind == 257 else 4
            target = sections[section[7]]
            within = address - target[3]
            if within < 0 or within + width > target[5] or address % width:
                raise ValueError('invalid absolute relocation target')
            value = int.from_bytes(data[target[4]+within:target[4]+within+width], 'little')
            # SHN_ABS hardware constants stay fixed; image addresses relocate.
            if minimum <= value <= maximum:
                if address in records: raise ValueError('duplicate relocation target')
                records[address] = (address, value, width)
            elif sym[3] != 0xfff1:
                raise ValueError(f'absolute symbol outside image: {value:#x}')
    strings = sections[header[13]]
    names = data[strings[4]:strings[4]+strings[5]]
    for section in sections:
        name = names[section[0]:names.index(0, section[0])]
        if name not in (b'.got', b'.got.plt'): continue
        if section[5] % 8: raise ValueError('unaligned GOT')
        for index in range(0, section[5], 8):
            address = section[3] + index
            value = struct.unpack_from('<Q', data, section[4]+index)[0]
            if minimum <= value <= maximum: records[address] = (address, value, 8)
    return sorted(records.values())
