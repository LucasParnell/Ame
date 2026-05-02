#!/usr/bin/env python3
"""Build the tiny Dango Linux initramfs without requiring root.

The archive is written directly in the Linux initramfs "newc" format so we can
include character device nodes such as /dev/console even when the build is run
by an unprivileged user. Using host mknod(2) would make the Linux artifact build
depend on root privileges or fakeroot, which is fragile for CI and normal
developer machines.
"""

from __future__ import annotations

import os
import stat
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Optional

CPIO_MAGIC_NEWC = "070701"
TRAILER = "TRAILER!!!"
S_IFREG = 0o100000
S_IFDIR = 0o040000
S_IFCHR = 0o020000

@dataclass(frozen=True)
class Entry:
    name: str
    mode: int
    data: bytes = b""
    nlink: int = 1
    rdev_major: int = 0
    rdev_minor: int = 0

def _pad4(out: BinaryIO, size: int) -> None:
    pad = (-size) & 3
    if pad:
        out.write(b"\0" * pad)

def _write_entry(out: BinaryIO, entry: Entry, ino: int) -> None:
    name = entry.name.encode("utf-8") + b"\0"
    size = len(entry.data)
    fields = [
        CPIO_MAGIC_NEWC, f"{ino:08x}", f"{entry.mode:08x}",
        f"{0:08x}", f"{0:08x}", f"{entry.nlink:08x}",
        f"{0:08x}", f"{size:08x}", f"{0:08x}", f"{0:08x}",
        f"{entry.rdev_major:08x}", f"{entry.rdev_minor:08x}",
        f"{len(name):08x}", f"{0:08x}",
    ]
    header = "".join(fields).encode("ascii")
    if len(header) != 110:
        raise AssertionError(f"newc header has wrong size: {len(header)}")
    out.write(header)
    out.write(name)
    _pad4(out, len(header) + len(name))
    out.write(entry.data)
    _pad4(out, size)

def _file_entry(name: str, source: Path, mode: Optional[int] = None) -> Entry:
    data = source.read_bytes()
    if mode is None:
        mode = stat.S_IMODE(source.stat().st_mode)
    return Entry(name=name, mode=S_IFREG | mode, data=data)

def build(agent: Path, output: Path) -> None:
    if not agent.is_file():
        raise SystemExit(f"error: missing agent executable: {agent}")
    entries = [
        Entry(".", S_IFDIR | 0o755, nlink=4),
        Entry("dev", S_IFDIR | 0o755, nlink=2),
        Entry("dev/console", S_IFCHR | 0o600, rdev_major=5, rdev_minor=1),
        Entry("dev/null", S_IFCHR | 0o666, rdev_major=1, rdev_minor=3),
        Entry("proc", S_IFDIR | 0o555, nlink=2),
        Entry("sys", S_IFDIR | 0o555, nlink=2),
        Entry("run", S_IFDIR | 0o755, nlink=2),
        Entry("tmp", S_IFDIR | 0o1777, nlink=2),
        _file_entry("init", agent, 0o755),
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_suffix(output.suffix + ".tmp")
    with tmp.open("wb") as out:
        for ino, entry in enumerate(entries, start=1):
            _write_entry(out, entry, ino)
        _write_entry(out, Entry(TRAILER, S_IFREG | 0o644, nlink=1), len(entries) + 1)
    os.replace(tmp, output)

def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} DANGO_AGENT OUTPUT_CPIO", file=sys.stderr)
        return 2
    build(Path(argv[1]), Path(argv[2]))
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
