#!/usr/bin/env python3
"""Reject mismatched prebuilt profiles or stale manifest sizes before signing."""
import pathlib
import sys
import tomllib


def check(directory, expected):
    root = pathlib.Path(directory)
    manifest = tomllib.loads((root / 'manifest.toml').read_text())
    if manifest.get('profile', 'hello') != expected:
        raise ValueError(f'Linux artifact profile does not match {expected}')
    for name, key in [('Image', 'kernel_size'), ('initramfs.cpio', 'initramfs_size'),
                      ('dango-linux.dtb', 'dtb_size')]:
        size = (root / name).stat().st_size
        if size == 0 or size != manifest.get(key):
            raise ValueError(f'{name}: stale or empty artifact')


if __name__ == '__main__':
    try:
        check(*sys.argv[1:])
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
