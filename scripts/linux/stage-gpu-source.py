#!/usr/bin/env python3
"""Overlay one built-in driver without modifying the supplied Linux source.

Only the root, drivers, and drivers/misc directories are materialized. All
other entries link to the read-only source tree; the build remains incremental.
"""
import argparse
from pathlib import Path


def stage(source: Path, dest: Path, repo: Path):
    branches = {Path("."), Path("drivers"), Path("drivers/misc")}
    for branch in sorted(branches, key=lambda p: len(p.parts)):
        target = dest / branch
        if target.is_symlink():
            raise ValueError(f"overlay directory must not be a symlink: {target}")
        target.mkdir(parents=True, exist_ok=True)
        for entry in (source / branch).iterdir():
            relative = branch / entry.name
            if relative in branches or relative == Path("drivers/misc/Makefile"):
                continue
            link = dest / relative
            if link.is_symlink():
                if link.readlink() == entry:
                    continue
                link.unlink()
            link.symlink_to(entry, target_is_directory=entry.is_dir())
    misc = dest / "drivers/misc"
    # Built-in Dango drivers (Phase G channel, Phase L/M shared regions) plus
    # the shared ABI headers they include. One table so a new driver is added in
    # one place and its object file is named from the same key.
    drivers = {
        "dango_channel.c": repo / "linux-artifacts/guest-driver/dango_channel.c",
        "dango_shm.c": repo / "linux-artifacts/guest-driver/dango_shm.c",
    }
    for name, origin in {**drivers, "midix": repo / "shared/include/midix"}.items():
        link = misc / name
        if not link.is_symlink():
            link.symlink_to(origin, target_is_directory=origin.is_dir())
        elif link.readlink() != origin:
            link.unlink()
            link.symlink_to(origin, target_is_directory=origin.is_dir())
    obj_lines = "".join(f"obj-y += {Path(name).with_suffix('.o').name}\n" for name in drivers)
    makefile = (source / "drivers/misc/Makefile").read_text() + "\n" + obj_lines
    output = misc / "Makefile"
    # A previous symlink-only overlay must never turn this write into a write
    # through to the caller's original source Makefile.
    if output.is_symlink():
        output.unlink()
    if not output.exists() or output.read_text() != makefile:
        output.write_text(makefile)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("dest", type=Path)
    args = parser.parse_args()
    stage(args.source.resolve(), args.dest.resolve(), Path(__file__).resolve().parents[2])
