#!/usr/bin/env python3
"""Validate the tiny Dango initramfs without relying on host cpio quirks."""
from __future__ import annotations
import stat
import sys
from pathlib import Path

REQUIRED = {
    "init": (stat.S_IFREG, None),
    "dev": (stat.S_IFDIR, None),
    "dev/console": (stat.S_IFCHR, (5, 1)),
    "dev/null": (stat.S_IFCHR, (1, 3)),
    "proc": (stat.S_IFDIR, None),
    "sys": (stat.S_IFDIR, None),
}

def parse_newc(path: Path):
    data = path.read_bytes()
    off = 0
    out = {}
    while True:
        if off + 110 > len(data):
            raise SystemExit(f"error: truncated newc header at offset {off}")
        h = data[off:off + 110]
        if h[:6] != b"070701":
            raise SystemExit(f"error: bad newc magic at offset {off}: {h[:6]!r}")
        vals = [int(h[i:i + 8], 16) for i in range(6, 110, 8)]
        (_ino, mode, _uid, _gid, _nlink, _mtime, filesize,
         _devmajor, _devminor, rdevmajor, rdevminor, namesize, _check) = vals
        off += 110
        if off + namesize > len(data):
            raise SystemExit("error: truncated newc filename")
        name = data[off:off + namesize].rstrip(b"\0").decode("utf-8")
        off += namesize
        off = (off + 3) & ~3
        if off + filesize > len(data):
            raise SystemExit(f"error: truncated newc payload for {name}")
        off += filesize
        off = (off + 3) & ~3
        if name == "TRAILER!!!":
            break
        out[name] = (mode, rdevmajor, rdevminor)
    return out

def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} INITRAMFS_CPIO", file=sys.stderr)
        return 2
    entries = parse_newc(Path(argv[1]))
    for name, (kind, rdev) in REQUIRED.items():
        if name not in entries:
            print(f"error: initramfs.cpio does not contain {name}", file=sys.stderr)
            return 1
        mode, major, minor = entries[name]
        if stat.S_IFMT(mode) != kind:
            print(f"error: initramfs entry {name} has wrong type: mode={mode:o}", file=sys.stderr)
            return 1
        if rdev is not None and (major, minor) != rdev:
            print(f"error: initramfs entry {name} has wrong device {major}:{minor}", file=sys.stderr)
            return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
