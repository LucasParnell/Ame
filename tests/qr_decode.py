"""Independent QR Model-2 decoder used to verify the kernel encoder.

Written from the spec (ISO/IEC 18004): it re-derives the function patterns, the
format information, the placement order, the mask and the block structure, then
reads the payload back out and re-checks every Reed-Solomon block. It shares no
code with kernel/qr.cc, so agreement is real evidence. Matrix equality against
another encoder is *not* used: different encoders may pad differently, and both
are valid as long as the symbol decodes.
"""
import json
from pathlib import Path

LEVELS = ('L', 'M', 'Q', 'H')
LEVEL_CODE_TO_NAME = {1: 'L', 0: 'M', 3: 'Q', 2: 'H'}


def load_tables(path):
    return json.loads(Path(path).read_text())


def version_of(size):
    return (size - 17) // 4


def function_map(version, size, tables):
    fn = [[False] * size for _ in range(size)]

    def finder(cx, cy):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    fn[y][x] = True

    finder(3, 3)
    finder(size - 4, 3)
    finder(3, size - 4)
    for i in range(8, size - 8):
        fn[i][6] = True
        fn[6][i] = True
    # Alignment patterns (version >= 2), skipping the three finder corners.
    positions = tables['alignment'].get(str(version), [])
    for cx in positions:
        for cy in positions:
            if (cx == 6 and cy == 6) or (cx == 6 and cy == size - 7) or (cx == size - 7 and cy == 6):
                continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    fn[cy + dy][cx + dx] = True
    # Version information (version >= 7): two 3x6 blocks.
    if version >= 7:
        for i in range(18):
            a = size - 11 + (i % 3)
            b = i // 3
            fn[b][a] = True
            fn[a][b] = True
    for i in range(9):
        if i != 6:
            fn[8][i] = True
            fn[i][8] = True
    for i in range(8):
        fn[8][size - 1 - i] = True
        fn[size - 1 - i][8] = True
    return fn


def read_format(rows, size, tables):
    bits = 0
    coords = [(8, 0), (8, 1), (8, 2), (8, 3), (8, 4), (8, 5), (8, 7), (8, 8),
              (7, 8), (5, 8), (4, 8), (3, 8), (2, 8), (1, 8), (0, 8)]
    for i, (x, y) in enumerate(coords):
        if rows[y][x] == '1':
            bits |= 1 << i
    for index, word in enumerate(tables['format_info']):
        if word == bits:
            code = index >> 3
            return LEVEL_CODE_TO_NAME[code], index & 7
    raise AssertionError(f'format information 0x{bits:04x} is not a valid code')


def placement_order(version, size, tables):
    fn = function_map(version, size, tables)
    order = []
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5
        upward = ((right + 1) & 2) == 0
        for vert in range(size):
            for j in range(2):
                x = right - j
                y = size - 1 - vert if upward else vert
                if not fn[y][x]:
                    order.append((x, y))
        right -= 2
    return order


def mask_bit(mask, x, y):
    return [
        (x + y) % 2 == 0,
        y % 2 == 0,
        x % 3 == 0,
        (x + y) % 3 == 0,
        (y // 2 + x // 3) % 2 == 0,
        (y * x) % 2 + (y * x) % 3 == 0,
        ((y * x) % 2 + (y * x) % 3) % 2 == 0,
        ((y + x) % 2 + (y * x) % 3) % 2 == 0,
    ][mask]


def gf_mul(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = ((z << 1) ^ (0x11D if z & 0x80 else 0)) & 0xFF
        z ^= ((y >> i) & 1) * x
    return z


def generator(degree):
    gen = [0] * degree
    gen[-1] = 1
    root = 1
    for _ in range(degree):
        for j in range(degree):
            gen[j] = gf_mul(gen[j], root)
            if j + 1 < degree:
                gen[j] ^= gen[j + 1]
        root = gf_mul(root, 2)
    return gen


def ec_remainder(data, degree):
    gen = generator(degree)
    result = [0] * degree
    for byte in data:
        factor = byte ^ result[0]
        result = result[1:] + [0]
        for j in range(degree):
            result[j] ^= gf_mul(gen[j], factor)
    return result


def blocks_of(tables, version, level):
    spec = tables['ec'][str(version)][level]
    out = []
    for group in spec:
        for _ in range(group['blocks']):
            out.append((group['data'], group['ec']))
    return out


def decode(rows, tables):
    size = len(rows)
    version = version_of(size)
    level, mask = read_format(rows, size, tables)
    order = placement_order(version, size, tables)

    bits = []
    for (x, y) in order:
        v = 1 if rows[y][x] == '1' else 0
        if mask_bit(mask, x, y):
            v ^= 1
        bits.append(v)

    groups = blocks_of(tables, version, level)
    total_cw = sum(d + e for d, e in groups)
    if len(bits) < total_cw * 8:
        raise AssertionError('matrix too small for the codeword count')
    bits = bits[:total_cw * 8]
    stream = [int(''.join(str(b) for b in bits[i:i + 8]), 2) for i in range(0, len(bits), 8)]

    # De-interleave: data codewords first (by column across blocks), then EC.
    data_parts = [[] for _ in groups]
    max_data = max(d for d, _ in groups)
    pos = 0
    for i in range(max_data):
        for b, (d, _) in enumerate(groups):
            if i < d:
                data_parts[b].append(stream[pos])
                pos += 1
    ec_parts = [[] for _ in groups]
    ec_len = groups[0][1]
    for i in range(ec_len):
        for b in range(len(groups)):
            ec_parts[b].append(stream[pos])
            pos += 1

    for b, (d, e) in enumerate(groups):
        expect = ec_remainder(data_parts[b], e)
        if expect != ec_parts[b]:
            raise AssertionError(f'block {b}: Reed-Solomon mismatch')

    data = [byte for part in data_parts for byte in part]
    bitstream = ''.join(f'{byte:08b}' for byte in data)
    mode = int(bitstream[:4], 2)
    if mode != 0b0100:
        raise AssertionError(f'unexpected mode {mode:04b} (byte mode expected)')
    cc_bits = 8 if version <= 9 else 16
    count = int(bitstream[4:4 + cc_bits], 2)
    payload = bytes(int(bitstream[4 + cc_bits + 8 * i:4 + cc_bits + 8 * (i + 1)], 2)
                    for i in range(count))
    return {'version': version, 'level': level, 'mask': mask, 'payload': payload,
            'ec_ok': True}
