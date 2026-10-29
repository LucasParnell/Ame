#!/usr/bin/env python3
"""Decode explicitly selected persistent_ram records from a private DFX backup.

This is offline only. Select header offsets belonging to the tested image;
the common AMEANKO1 tag alone cannot identify a package among older DFX slots.
Record lengths come from validated persistent_ram headers, never stale bytes
following the trace or an opportunistic NUL terminator.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('anne_native_log', Path(__file__).with_name('collect-native-log.py'))
logs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(logs)


extract_record = logs.extract_record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--header-offset', required=True, action='append', type=lambda n: int(n, 0),
                        help='explicit persistent_ram header offset, decimal or 0x hexadecimal; repeat for copies')
    args = parser.parse_args()
    if args.input.stat().st_size > 64 * 1024 * 1024:
        parser.error('backup exceeds the 64 MiB offline limit')
    data = args.input.read_bytes()
    manifest = json.loads((args.package / 'manifest.json').read_text())
    elf = args.package / 'kernel.elf'
    elf_hash = hashlib.sha256(elf.read_bytes()).hexdigest()
    if elf_hash != manifest['diagnostic']['kernel_elf_sha256']:
        raise ValueError('packaged ELF checksum mismatch')
    records = []
    for offset in args.header_offset:
        raw = extract_record(data, offset)
        text = raw.decode('utf-8')
        if not logs.matches_run(text, manifest['diagnostic']):
            raise ValueError('selected trace run_tag does not match this package')
        if manifest['diagnostic'].get('display_capture') and 'DISPLAY begin phase=' not in text:
            raise ValueError('selected record lacks this package profile\'s display capture')
        records.append(dict(header_offset=offset, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                            **logs.decode(text, elf)))
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    for record in records:
        (args.output / f'record-{record["header_offset"]:08x}.txt').write_text(record['text'])
    (args.output / 'decoded.json').write_text(json.dumps(records, indent=2) + '\n')
    summary = dict(source='Huawei DFX backup; explicitly selected bounded persistent_ram records',
                   input_sha256=hashlib.sha256(data).hexdigest(),
                   native_image_sha256=manifest['native_image_sha256'], kernel_elf_sha256=elf_hash,
                   records=[{key: record[key] for key in ('header_offset', 'bytes', 'sha256', 'last_checkpoint',
                             'display', 'services_ready', 'reset', 'online_cpus', 'online_mask',
                             'faults', 'user_faults', 'panic')} for record in records])
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(dict(records=len(records), distinct_traces=len({r['sha256'] for r in records}),
                         painted=all(r['display']['painted'] for r in records),
                         services_ready=all(r['services_ready'] for r in records),
                         reset=all(r['reset'] for r in records),
                         online_cpus=sorted({r['online_cpus'] for r in records}),
                         faults=sum(len(r['faults']) + len(r['user_faults']) + len(r['panic']) for r in records)), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, UnicodeDecodeError) as error:
        raise SystemExit(str(error))
