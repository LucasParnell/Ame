#!/usr/bin/env python3
"""Build, inspect and verify an HSDT container for offline DTS analysis.

For the ANE-LX1 stock image, row 0 (key 04 00 00 07) is the handset's DTB.
The table's size (+0x08) and data offset (+0x10) are the unsigned control
fields identified during reverse engineering. The key, verify length (+0x0c),
verify offset (+0x14), stock compressed bytes and proof bytes are preserved.

With --payload, the stream inflates to the stock DTB, zero padding up to the
2 MiB destination size, and then the supplied payload. Without --payload,
build emits a stock-DTB-only control container. Payload builds are analysis
inputs only: the loader rejects inflate results over 2 MiB before verification.
`preflight` models that gate and compares the normalized DTB and proof prefix
with stock; it is not a cryptographic verifier. This script is offline and
never communicates with a device.
"""
import argparse
import gzip
import hashlib
import io
import json
import struct
import zlib
from pathlib import Path

MAGIC = 0x54445348           # 'HSDT'
ENTRY = 0x28
HEADER = 0x0c
DTB_CAP = 0x200000
TABLE_CAP = 0x200000
TARGET_KEY = bytes.fromhex('04000007')
STOCK_DTS_SHA256 = '269fd6ddc8892470916761b373f2063aaecd4bdfa8a503946dbea76fce37ccad'
ARENA_B_BASE = 0x2e000000
ARENA_B_SIZE = 0x02000000
ARENA_A_BASE = 0x30000000
ARENA_A_SIZE = 0x04000000
DTB_BUFFER_BASE = 0x2e400000
OVERFLOW_ADDRESS = DTB_BUFFER_BASE + DTB_CAP
OUTPUT_END = ARENA_A_BASE + ARENA_A_SIZE
PATCH_SITE = 0x10b634
PATCH_WORD = 0x52807921


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_table(img):
    if len(img) < HEADER:
        raise ValueError('image is shorter than the HSDT header')
    magic, version, count = struct.unpack_from('<III', img, 0)
    if magic != MAGIC:
        raise ValueError('not an HSDT image: %#x' % magic)
    if count > TABLE_CAP // ENTRY or HEADER + count * ENTRY > len(img):
        raise ValueError('HSDT entry table exceeds the image or safety cap')
    rows = []
    for i in range(count):
        base = HEADER + i * ENTRY
        key = img[base:base + 4]
        size, vlen, off, voff = struct.unpack_from('<IIII', img, base + 8)
        if size and (off > len(img) or size > len(img) - off):
            raise ValueError('row %d data extent exceeds the image' % i)
        if vlen and (voff > len(img) or vlen > len(img) - voff):
            raise ValueError('row %d proof extent exceeds the image' % i)
        rows.append(dict(index=i, key=key.hex(), size=size, vlen=vlen,
                         off=off, voff=voff, entry_off=base))
    return dict(version=version, count=count, rows=rows)


def used_extent(img, table):
    end = HEADER + table['count'] * ENTRY
    for row in table['rows']:
        if row['size']:
            end = max(end, row['off'] + row['size'])
        if row['vlen']:
            end = max(end, row['voff'] + row['vlen'])
    return end


def target_row(table, entry, require_proof=True, expected_key=TARGET_KEY):
    if not 0 <= entry < table['count']:
        raise ValueError('entry index is outside the HSDT table')
    row = table['rows'][entry]
    if expected_key is not None and bytes.fromhex(row['key']) != expected_key:
        raise ValueError('selected row does not match the expected HSDT key ' +
                         expected_key.hex())
    if require_proof and not row['vlen']:
        raise ValueError('selected row has no verification proof; expected the stock signed DTB row')
    return row


def inflate_at(img, row):
    try:
        return zlib.decompress(img[row['off']:row['off'] + row['size']],
                               16 + zlib.MAX_WBITS)
    except zlib.error as error:
        raise ValueError('row %d is not a valid gzip stream: %s' % (row['index'], error)) from error


def proof_ranges(table):
    return [(row['voff'], row['voff'] + row['vlen'])
            for row in table['rows'] if row['vlen']]


def gzip_stream(body, sync_boundary=None):
    """Create a deterministic gzip member, optionally flushing at a boundary."""
    if sync_boundary is None:
        stream_buffer = io.BytesIO()
        with gzip.GzipFile(fileobj=stream_buffer, mode='wb', mtime=0,
                           compresslevel=9) as stream:
            stream.write(body)
        return stream_buffer.getvalue()
    if not 0 < sync_boundary < len(body):
        raise ValueError('gzip sync boundary must be inside the output body')
    compressor = zlib.compressobj(level=9, method=zlib.DEFLATED,
                                  wbits=-zlib.MAX_WBITS, memLevel=8,
                                  strategy=zlib.Z_FIXED)
    raw = compressor.compress(body[:sync_boundary])
    raw += compressor.flush(zlib.Z_SYNC_FLUSH)
    raw += compressor.compress(body[sync_boundary:])
    raw += compressor.flush(zlib.Z_FINISH)
    header = b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff'
    trailer = struct.pack('<II', zlib.crc32(body) & 0xffffffff,
                          len(body) & 0xffffffff)
    return header + raw + trailer


def build(args):
    source = Path(args.image).read_bytes()
    table = parse_table(source)
    row = target_row(table, args.entry)
    if sha256(source) != STOCK_DTS_SHA256:
        raise SystemExit('source dts hash does not match the captured ANE-LX1 stock image')
    stock_dtb = inflate_at(source, row)
    if len(stock_dtb) > DTB_CAP:
        raise SystemExit('stock DTB exceeds the measured 2 MiB destination')

    payload = Path(args.payload).read_bytes() if args.payload else b''
    state_restore = Path(args.state_restore).read_bytes() if args.state_restore else b''
    if args.payload:
        tail = state_restore + payload
        body = stock_dtb + b'\0' * (DTB_CAP - len(stock_dtb)) + tail
        if not payload:
            raise SystemExit('overflow payload must not be empty')
        if OVERFLOW_ADDRESS + len(tail) > OUTPUT_END:
            raise SystemExit('payload would extend beyond the mapped arena A/B region')
    else:
        if args.state_restore:
            raise SystemExit('--state-restore requires --payload')
        body = stock_dtb

    compressed = gzip_stream(body, DTB_CAP if state_restore else None)
    stream_padding = 0
    if args.stream_size is not None:
        if not state_restore:
            raise SystemExit('--stream-size requires --state-restore')
        if len(compressed) > args.stream_size:
            raise SystemExit('compressed stream exceeds requested --stream-size')
        stream_padding = args.stream_size - len(compressed)
        compressed += bytes(stream_padding)

    old_extent = used_extent(source, table)
    stream_offset = (old_extent + 0xfff) & ~0xfff
    if stream_offset + len(compressed) > len(source):
        raise SystemExit('no unused tail space: need %#x bytes at %#x of %#x'
                         % (len(compressed), stream_offset, len(source)))
    if any(stream_offset < end and stream_offset + len(compressed) > start
           for start, end in proof_ranges(table)):
        raise SystemExit('new stream overlaps an existing verification-proof range')
    if any(stream_offset < other['off'] + other['size'] and
           stream_offset + len(compressed) > other['off']
           for other in table['rows'] if other['size']):
        raise SystemExit('new stream overlaps an existing HSDT data range')

    result = bytearray(source)
    result[stream_offset:stream_offset + len(compressed)] = compressed
    struct.pack_into('<I', result, row['entry_off'] + 0x08, len(compressed))
    struct.pack_into('<I', result, row['entry_off'] + 0x10, stream_offset)
    result = bytes(result)

    payload_path = Path(args.payload).resolve() if args.payload else None
    manifest = dict(
        source=str(Path(args.image).resolve()), source_sha256=sha256(source),
        output=str(Path(args.out).resolve()), output_sha256=sha256(result),
        format='HSDT v1', table_count=table['count'], entry=row['index'], key=row['key'],
        unsigned_fields=[{'offset': row['entry_off'] + 0x08, 'name': 'compressed_size'},
                         {'offset': row['entry_off'] + 0x10, 'name': 'data_offset'}],
        preserved_fields=[{'offset': row['entry_off'], 'name': 'key', 'bytes': 4},
                          {'offset': row['entry_off'] + 0x0c, 'name': 'verify_len', 'bytes': 4},
                          {'offset': row['entry_off'] + 0x14, 'name': 'verify_offset', 'bytes': 4}],
        stock_dtb_bytes=len(stock_dtb), stock_dtb_sha256=sha256(stock_dtb),
        dtb_capacity=DTB_CAP, payload_path=str(payload_path) if payload_path else None,
        payload_bytes=len(payload), payload_sha256=sha256(payload) if payload else None,
        state_restore_bytes=len(state_restore),
        state_restore_sha256=sha256(state_restore) if state_restore else None,
        stream_padding_bytes=stream_padding,
        compression=('gzip Z_FIXED with Z_SYNC_FLUSH at 2 MiB' if state_restore
                     else 'gzip'),
        control_only=not bool(payload), inflated_bytes=len(body), compressed_bytes=len(compressed),
        stream_offset=stream_offset, stream_sha256=sha256(compressed),
        original_entry={'size': row['size'], 'vlen': row['vlen'],
                        'off': row['off'], 'voff': row['voff']},
        proof_sha256=sha256(source[row['voff']:row['voff'] + row['vlen']]),
        arena_b={'base': ARENA_B_BASE, 'size': ARENA_B_SIZE},
        dtb_buffer_base=DTB_BUFFER_BASE,
        overflow_start=OVERFLOW_ADDRESS if payload else None,
        overflow_end=OVERFLOW_ADDRESS + len(tail) if payload else None,
        overflow_bytes=len(tail) if payload else 0,
        patch_site=PATCH_SITE, patch_word='0x%08x' % PATCH_WORD,
    )
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(result)
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


def verify(args):
    image = Path(args.image).read_bytes()
    reference = Path(args.reference).read_bytes()
    table, ref_table = parse_table(image), parse_table(reference)
    if sha256(reference) != STOCK_DTS_SHA256:
        raise SystemExit('reference dts hash does not match the captured ANE-LX1 stock image')
    row = target_row(table, args.entry)
    original = target_row(ref_table, args.entry)
    inflated = inflate_at(image, row)
    stock = inflate_at(reference, original)

    # Verify that the HSDT table differs only in the two documented unsigned fields.
    expected_table = bytearray(reference[:HEADER + ref_table['count'] * ENTRY])
    struct.pack_into('<I', expected_table, row['entry_off'] + 0x08, row['size'])
    struct.pack_into('<I', expected_table, row['entry_off'] + 0x10, row['off'])
    table_bytes = image[:HEADER + table['count'] * ENTRY]
    table_unchanged = table_bytes == bytes(expected_table)

    proof = reference[original['voff']:original['voff'] + original['vlen']]
    proof_unchanged = (row['vlen'] == original['vlen'] and row['voff'] == original['voff']
                       and image[row['voff']:row['voff'] + row['vlen']] == proof)
    stock_stream_unchanged = (image[original['off']:original['off'] + original['size']]
                              == reference[original['off']:original['off'] + original['size']])
    if len(inflated) > DTB_CAP:
        prefix_ok = (inflated[:len(stock)] == stock
                     and inflated[len(stock):DTB_CAP] == b'\0' * (DTB_CAP - len(stock)))
        overflow_bytes = len(inflated) - DTB_CAP
        overflow_start = OVERFLOW_ADDRESS
        overflow_end = OVERFLOW_ADDRESS + overflow_bytes
    else:
        prefix_ok = inflated == stock
        overflow_bytes = 0
        overflow_start = overflow_end = None
    loader_accepts_length = len(inflated) <= DTB_CAP

    if args.manifest:
        manifest = json.loads(Path(args.manifest).read_text())
        manifest_overflow_bytes = manifest.get(
            'overflow_bytes', 0 if overflow_bytes == 0 else None)
        manifest_ok = (manifest.get('source_sha256') == sha256(reference)
                       and manifest.get('output_sha256') == sha256(image)
                       and manifest.get('stream_offset') == row['off']
                       and manifest.get('stream_sha256') == sha256(
                           image[row['off']:row['off'] + row['size']])
                       and manifest.get('overflow_start') == overflow_start
                       and manifest.get('overflow_end') == overflow_end
                       and manifest_overflow_bytes == overflow_bytes)
    else:
        manifest_ok = True
    report = dict(
        entry=row['index'], key=row['key'], verify_len=row['vlen'], verify_offset=row['voff'],
        stream_bytes=row['size'], data_offset=row['off'], inflated_bytes=len(inflated),
        control_only=not bool(overflow_bytes), exceeds_capacity=bool(overflow_bytes),
        overflow_start=overflow_start, overflow_end=overflow_end, overflow_bytes=overflow_bytes,
        table_changed_only_at_unsigned_size_and_offset_fields=table_unchanged,
        stock_proof_unchanged=proof_unchanged,
        original_compressed_stream_unchanged=stock_stream_unchanged,
        inflated_prefix_matches_stock=prefix_ok,
        loader_accepts_inflated_length=loader_accepts_length,
        manifest_matches=manifest_ok,
        stream_sha256=sha256(image[row['off']:row['off'] + row['size']]),
    )
    print(json.dumps(report, indent=2))
    if not all((table_unchanged, proof_unchanged, stock_stream_unchanged,
                prefix_ok, loader_accepts_length, manifest_ok)):
        raise SystemExit('container verification failed')


def preflight(args):
    image = Path(args.image).read_bytes()
    reference = Path(args.reference).read_bytes()
    table, ref_table = parse_table(image), parse_table(reference)
    if sha256(reference) != STOCK_DTS_SHA256:
        raise SystemExit('reference dts hash does not match the captured ANE-LX1 stock image')
    if (table['version'] != ref_table['version'] or
            table['count'] != ref_table['count']):
        raise SystemExit('HSDT version or entry count differs from stock')
    row = target_row(table, args.entry, require_proof=False)
    original = target_row(ref_table, args.entry)
    inflated = inflate_at(image, row)
    stock = inflate_at(reference, original)

    loader_accepts_length = len(inflated) <= DTB_CAP
    normalized_image_matches = (
        loader_accepts_length and len(stock) <= DTB_CAP and
        inflated + bytes(DTB_CAP - len(inflated)) ==
        stock + bytes(DTB_CAP - len(stock)))
    proof_extent_ok = (row['voff'] <= len(image) and
                       row['vlen'] <= len(image) - row['voff'])
    proof_length_ok = (row['vlen'] == 0 or 0x1000 <= row['vlen'] <= 0x200000)
    stock_proof_extent_ok = (original['voff'] <= len(reference) and
                             original['vlen'] <= len(reference) - original['voff'])
    proof_prefix_matches = None if row['vlen'] == 0 else (
        proof_length_ok and stock_proof_extent_ok and original['vlen'] >= 0x1000 and
        image[row['voff']:row['voff'] + 0x1000] ==
        reference[original['voff']:original['voff'] + 0x1000])
    verification_skipped = row['vlen'] == 0

    # Permit changes only to the selected row's controls, its selected stream,
    # and its selected proof range. This catches accidental edits elsewhere in
    # the fixed-size partition image.
    expected_table = bytearray(reference[:HEADER + ref_table['count'] * ENTRY])
    for field_offset in (0x08, 0x0c, 0x10, 0x14):
        value = struct.unpack_from('<I', image, row['entry_off'] + field_offset)[0]
        struct.pack_into('<I', expected_table, row['entry_off'] + field_offset, value)
    table_controls_only = image[:len(expected_table)] == bytes(expected_table)
    allowed = [(0, len(expected_table)),
               (row['off'], row['off'] + row['size']),
               (row['voff'], row['voff'] + row['vlen'])]
    allowed.sort()
    merged = []
    for start, end in allowed:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    unchanged_outside_allowed_extents = len(image) == len(reference)
    cursor = 0
    if unchanged_outside_allowed_extents:
        for start, end in merged:
            if start > cursor and image[cursor:start] != reference[cursor:start]:
                unchanged_outside_allowed_extents = False
                break
            cursor = max(cursor, end)
        if unchanged_outside_allowed_extents and cursor < len(image):
            unchanged_outside_allowed_extents = image[cursor:] == reference[cursor:]

    other_extent_overlaps = []
    selected_ranges = [('data', row['off'], row['off'] + row['size']),
                       ('proof', row['voff'], row['voff'] + row['vlen'])]
    table_end = HEADER + table['count'] * ENTRY
    for selected_name, selected_start, selected_end in selected_ranges:
        if selected_start < table_end and selected_end > 0:
            other_extent_overlaps.append({
                'selected': selected_name, 'other': 'HSDT table',
                'start': hex(selected_start), 'end': hex(min(selected_end, table_end)),
            })
    if (selected_ranges[0][1] < selected_ranges[1][2] and
            selected_ranges[0][2] > selected_ranges[1][1]):
        other_extent_overlaps.append({
            'selected': 'data', 'other': row['index'], 'other_range': 'proof',
            'start': hex(max(selected_ranges[0][1], selected_ranges[1][1])),
            'end': hex(min(selected_ranges[0][2], selected_ranges[1][2])),
        })
    for other in table['rows']:
        if other['index'] == row['index']:
            continue
        other_ranges = [('data', other['off'], other['off'] + other['size']),
                        ('proof', other['voff'], other['voff'] + other['vlen'])]
        for selected_name, selected_start, selected_end in selected_ranges:
            if selected_start == selected_end:
                continue
            for other_name, other_start, other_end in other_ranges:
                if other_start != other_end and selected_start < other_end and selected_end > other_start:
                    other_extent_overlaps.append({
                        'selected': selected_name, 'other': other['index'],
                        'other_range': other_name,
                        'start': hex(max(selected_start, other_start)),
                        'end': hex(min(selected_end, other_end)),
                    })

    report = dict(
        entry=row['index'], key=row['key'],
        reference_sha256=sha256(reference), image_sha256=sha256(image),
        format_header_matches=True,
        table_changes_limited_to_selected_row_controls=table_controls_only,
        changed_bytes_confined_to_table_stream_and_proof=unchanged_outside_allowed_extents,
        other_entry_extent_overlaps=other_extent_overlaps,
        compressed_bytes=row['size'], inflated_bytes=len(inflated),
        loader_accepts_inflated_length=loader_accepts_length,
        normalized_2m_image_matches_stock=normalized_image_matches,
        verify_len=row['vlen'], verify_offset=hex(row['voff']),
        proof_length_within_loader_and_copy_bounds=proof_length_ok,
        proof_read_extent_within_partition=proof_extent_ok,
        proof_prefix_0x1000_matches_stock=proof_prefix_matches,
        verification_skipped_by_zero_length=verification_skipped,
        proof_overread_bytes=max(0, row['vlen'] - 0x1000),
        verifier_inputs_match_stock=(None if verification_skipped else
                                     normalized_image_matches and proof_prefix_matches),
        cryptographic_verification=('skipped by verify_len == 0' if verification_skipped
                                    else 'not performed'),
        fdt_board_id_checked_by_emulator=False,
        device_ready=False,
    )
    print(json.dumps(report, indent=2))
    passed = all((report['format_header_matches'], table_controls_only,
                  unchanged_outside_allowed_extents, not other_extent_overlaps,
                  loader_accepts_length, proof_length_ok, proof_extent_ok,
                  (verification_skipped or
                   (normalized_image_matches and proof_prefix_matches))))
    if not passed:
        raise SystemExit('DTS preflight failed; this is not a cryptographic verification')


def inspect(args):
    image = Path(args.image).read_bytes()
    table = parse_table(image)
    key_rows = [row for row in table['rows'] if row['key'] == TARGET_KEY.hex()]
    summary = dict(image_bytes=len(image), image_sha256=sha256(image),
                   version=table['version'], count=table['count'],
                   target_key=TARGET_KEY.hex(), target_rows=key_rows,
                   verification_lengths=sorted({row['vlen'] for row in table['rows']}),
                   rows_without_proof=[row['index'] for row in table['rows'] if not row['vlen']],
                   referenced_extent=used_extent(image, table),
                   bytes_after_referenced_extent=len(image) - used_extent(image, table))
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='cmd', required=True)

    build_parser = sub.add_parser('build')
    build_parser.add_argument('--image', required=True)
    build_parser.add_argument('--entry', type=int, default=0)
    build_parser.add_argument('--payload', help='overflow payload; omit for stock-DTB control')
    build_parser.add_argument('--state-restore', help='optional inflater-state snapshot to prepend to overflow payload')
    build_parser.add_argument('--stream-size', type=int,
                              help='pad after the gzip member to this compressed input size')
    build_parser.add_argument('--out', required=True)
    build_parser.add_argument('--manifest', required=True)
    build_parser.set_defaults(func=build)

    verify_parser = sub.add_parser('verify')
    verify_parser.add_argument('--image', required=True)
    verify_parser.add_argument('--reference', required=True)
    verify_parser.add_argument('--entry', type=int, default=0)
    verify_parser.add_argument('--manifest')
    verify_parser.set_defaults(func=verify)

    preflight_parser = sub.add_parser(
        'preflight', help='check loader bounds and compare verifier inputs with stock')
    preflight_parser.add_argument('--image', required=True)
    preflight_parser.add_argument('--reference', required=True)
    preflight_parser.add_argument('--entry', type=int, default=0)
    preflight_parser.set_defaults(func=preflight)

    inspect_parser = sub.add_parser('inspect')
    inspect_parser.add_argument('--image', required=True)
    inspect_parser.set_defaults(func=inspect)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
