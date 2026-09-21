"""Decoder for the panic bundle carried in the QR URL (see anko/panic_report.h).

The kernel packs the panic record into a small binary body, optionally compresses
it with raw LZMA1 (lc=3, lp=0, pb=2, dict_size=4096, no end marker) and wraps it
in a URL fragment as unpadded base64url:

    bundle := 'A' 'P' version:u8 flags:u8 raw_len:u16le body_or_stream

This module is the reference decoder: the panic-screen harness uses it to prove
that what is on screen really carries the report.
"""
import base64
import lzma

MAGIC = b'AP'
VERSION = 1
FLAG_LZMA = 1
FILTERS = [{'id': lzma.FILTER_LZMA1, 'lc': 3, 'lp': 0, 'pb': 2, 'dict_size': 4096}]
URL_PREFIXES = ('https://ame.example/p#', 'http://ame.example/p#')


def strip_prefix(text):
    for prefix in URL_PREFIXES:
        if text.startswith(prefix):
            return text[len(prefix):]
    hash_index = text.find('#')
    return text[hash_index + 1:] if hash_index >= 0 else text


def _b64url_decode(fragment):
    padded = fragment + '=' * ((4 - len(fragment) % 4) % 4)
    return base64.urlsafe_b64decode(padded)


def _u64(body, offset):
    return int.from_bytes(body[offset:offset + 8], 'little'), offset + 8


def _str(body, offset):
    length = body[offset]
    offset += 1
    return body[offset:offset + length].decode('ascii', 'replace'), offset + length


def decode_bundle(text):
    blob = _b64url_decode(strip_prefix(text))
    if len(blob) < 6 or blob[0:2] != MAGIC:
        raise AssertionError(f'bad bundle magic: {blob[:4]!r}')
    version, flags = blob[2], blob[3]
    raw_len = int.from_bytes(blob[4:6], 'little')
    payload = blob[6:]
    if version != VERSION:
        raise AssertionError(f'unsupported bundle version {version}')
    if flags & FLAG_LZMA:
        decomp = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=FILTERS)
        body = decomp.decompress(payload, max_length=raw_len)
    else:
        body = payload[:raw_len]
    if len(body) != raw_len:
        raise AssertionError(f'bundle short: {len(body)} of {raw_len} bytes')

    offset = 0
    out = {'compressed': bool(flags & FLAG_LZMA), 'compressed_len': len(payload),
           'raw_len': raw_len, 'version': version}
    out['core'] = body[offset]
    offset += 1
    out['reason'], offset = _str(body, offset)
    regs = {}
    for name in ('esr', 'far', 'elr', 'spsr', 'sp', 'fp', 'lr'):
        regs[name], offset = _u64(body, offset)
    out['regs'] = regs
    count = body[offset]
    offset += 1
    x = []
    for _ in range(count):
        value, offset = _u64(body, offset)
        x.append(value)
    out['x'] = x
    frames = body[offset]
    offset += 1
    bt = []
    for _ in range(frames):
        value, offset = _u64(body, offset)
        bt.append(value)
    out['bt'] = bt
    out['hw_flags'] = body[offset]
    out['virtio'] = body[offset + 1]
    out['boot_el'] = body[offset + 2]
    offset += 3
    for name in ('pci_base', 'pci_size', 'ram_base', 'ram_size'):
        out[name], offset = _u64(body, offset)
    out['cap_denials'] = int.from_bytes(body[offset:offset + 4], 'little')
    offset += 4
    out['build'], offset = _str(body, offset)
    if offset != len(body):
        raise AssertionError(f'bundle has {len(body) - offset} trailing bytes')
    return out
