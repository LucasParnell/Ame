#!/usr/bin/env python3
"""Generate a private per-device FIDO2 seed, or a kernel-only seed header.

Keep the seed and provisioned kernel image private. Reuse the same seed across
rebuilds for one device; changing it invalidates every existing credential.
"""
import argparse
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--create', type=Path, help='create 32 random bytes; refuses overwrite')
    group.add_argument('--header', type=Path, help='write kernel-only C++ header')
    parser.add_argument('--seed', type=Path, help='existing private seed for header generation')
    args = parser.parse_args()
    if args.create:
        if args.seed:
            parser.error('--seed requires --header')
        # Exclusive creation with private permissions from the first write.
        fd = os.open(args.create, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as output:
            output.write(os.urandom(32))
            output.flush()
            os.fsync(output.fileno())
        return
    seed = args.seed.read_bytes() if args.seed else bytes(32)
    if len(seed) != 32 or (args.seed and not any(seed)):
        parser.error('seed must contain exactly 32 bytes and cannot be all zero')
    args.header.parent.mkdir(parents=True, exist_ok=True)
    body = ','.join(str(n) for n in seed)
    fd = os.open(args.header, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as output:
        output.write('#pragma once\n#include "midix/abi/protocols/fido2.h"\n'
                     f'inline constexpr MxFido2Seed kFido2Seed = {{MX_FIDO2_SEED_VERSION,{int(bool(args.seed))},{{{body}}}}};\n')


if __name__ == '__main__':
    main()
