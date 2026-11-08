#!/usr/bin/env python3
"""Pack an MXI and named resources into an AmeFS application (.amp)."""
import argparse
from pathlib import Path
import re
import struct

MAX_BYTES = 8 * 1024 * 1024
MAX_ENTRIES = 64


def build(executable: bytes, resources: dict[str, bytes]) -> bytes:
    if not executable or executable[:4] != b'MXI1':
        raise ValueError('executable must be an MXI image')
    if len(resources) >= MAX_ENTRIES or 'app.mxi' in resources:
        raise ValueError('too many resources or reserved app.mxi name')
    entries = [('app.mxi', executable)] + sorted(resources.items())
    cursor = 32 + 80 * len(entries)
    table = bytearray()
    for name, payload in entries:
        if (len(name) > 63 or not name or
                any(not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', part)
                    for part in name.split('/'))):
            raise ValueError(f'invalid resource name: {name!r}')
        table += struct.pack('<64sQQ', name.encode('ascii'), cursor, len(payload))
        cursor += len(payload)
    if cursor > MAX_BYTES:
        raise ValueError('package exceeds 8 MiB launch limit')
    return (struct.pack('<8sIIQQ', b'AMEAPP1\0', 1, len(entries), cursor, 0) +
            table + b''.join(payload for _, payload in entries))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--resource', action='append', default=[], metavar='NAME=FILE')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    resources = {}
    for item in args.resource:
        name, separator, path = item.partition('=')
        if not separator or name in resources:
            parser.error('resources must be unique NAME=FILE pairs')
        resources[name] = Path(path).read_bytes()
    try:
        package = build(args.executable.read_bytes(), resources)
    except ValueError as error:
        parser.error(str(error))
    args.output.write_bytes(package)


if __name__ == '__main__':
    main()
