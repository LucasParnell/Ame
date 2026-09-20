#!/usr/bin/env python3
"""Durable-persistence regression for io.fs.

Boots the `durable.mxb` fixture bundle twice against one writable, disposable
copy of a midix-fs-v1 disk image:

  1. the app.durable probe writes /data/persist.txt and calls FS_SYNC
  2. QEMU is stopped, then rebooted against the same image, and the probe
     reads the file back and asserts the exact bytes survived

The supplied image and the build tree are never written: the disk is a copy in a
temporary directory, so this is safe to run concurrently with anything else.

Exit status is 0 only when both boots reach their marker.
"""
import argparse
import contextlib
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import boot  # noqa: E402

MARK_WRITE = 'durability: write phase complete'
MARK_READ = 'durability regression passed'


def build_base_image(root: pathlib.Path, build: pathlib.Path, image: pathlib.Path) -> None:
    subprocess.run(
        [sys.executable, str(root / 'scripts' / 'mx' / 'mkamefs.py'), 'create',
         '--output', str(image), '--build-dir', str(build), '--size', '128M'],
        check=True, stdout=subprocess.DEVNULL)


def log_tail(path: pathlib.Path, lines: int = 12) -> str:
    for name in ('host.log', 'guest.log'):
        log = path / name
        if log.is_file() and log.stat().st_size:
            tail = log.read_text(errors='replace').splitlines()[-lines:]
            return f'{log}:\n' + '\n'.join('    ' + line for line in tail)
    return f'{path}: no log output'


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--keep', action='store_true', help='keep the scratch disk')
    args = parser.parse_args(argv)

    root = pathlib.Path(__file__).resolve().parents[2]
    build = pathlib.Path(args.build).resolve()
    bundle = build / 'durable.mxb'
    if not bundle.is_file():
        raise SystemExit(f'missing {bundle}; build the durable_bundle target first')

    work = pathlib.Path(tempfile.mkdtemp(prefix='ame-durable-'))
    base = work / 'fs-base.img'
    disk = work / 'durable-disk.img'
    try:
        build_base_image(root, build, base)
        shutil.copyfile(base, disk)

        started = time.monotonic()
        write_dir = work / 'write'
        read_dir = work / 'read'
        # boot.run() prints its own JSON summary; swallow it so this script's
        # stdout is a single JSON object for callers to parse.
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            wrote = boot.run(build=str(build), mode='direct', cpus=2, timeout=args.timeout,
                             disk=str(disk), bundle=str(bundle), markers=[MARK_WRITE],
                             output=str(write_dir), snapshot=False)
            read_back = boot.run(build=str(build), mode='direct', cpus=2, timeout=args.timeout,
                                 disk=str(disk), bundle=str(bundle), markers=[MARK_READ],
                                 output=str(read_dir), snapshot=False)
        success = bool(wrote and read_back)
        result = {
            'success': success,
            'write_phase': bool(wrote),
            'read_phase': bool(read_back),
            'elapsed': time.monotonic() - started,
            'disk': str(disk) if args.keep else '(discarded)',
        }
        print(json.dumps(result, indent=2))
        if not success:
            print('write phase logs:\n' + log_tail(write_dir), file=sys.stderr)
            print('read phase logs:\n' + log_tail(read_dir), file=sys.stderr)
        return 0 if success else 1
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
