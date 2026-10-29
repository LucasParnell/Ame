#!/usr/bin/env python3
"""Refresh or check the DTB's initrd bounds after guest userspace changes."""
import argparse
from pathlib import Path
import subprocess


def cells(dtb, name):
    value = subprocess.check_output(['fdtget', '-tx', str(dtb), '/chosen', name], text=True)
    words = value.split()
    if len(words) != 2:
        raise ValueError(f'{name} must contain two address cells')
    return (int(words[0], 16) << 32) | int(words[1], 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    dtb = args.directory / 'dango-linux.dtb'
    start = cells(dtb, 'linux,initrd-start')
    size = (args.directory / 'initramfs.cpio').stat().st_size
    end = start + ((size + 4095) & ~4095)
    actual = cells(dtb, 'linux,initrd-end')
    if args.check:
        if actual != end:
            raise SystemExit(f'stale DTB initrd end {actual:#x}; expected {end:#x}; rebuild the DTB')
    elif actual != end:
        subprocess.run(['fdtput', '-tx', str(dtb), '/chosen', 'linux,initrd-end',
                        f'{end >> 32:x}', f'{end & 0xffffffff:x}'], check=True)
        print(f'Updated DTB initrd end: {actual:#x} -> {end:#x}')


if __name__ == '__main__':
    main()
