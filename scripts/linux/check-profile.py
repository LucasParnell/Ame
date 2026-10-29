#!/usr/bin/env python3
"""Reject mismatched prebuilt profiles or stale manifest sizes before signing."""
import pathlib
import sys
import tomllib


def check(directory, expected, guest=None):
    root = pathlib.Path(directory)
    manifest_path = root / 'manifest.toml'
    manifest = tomllib.loads(manifest_path.read_text())
    if manifest.get('profile', 'hello') != expected:
        raise ValueError(
            f'Linux artifact profile does not match {expected}: {manifest_path} declares '
            f'profile={manifest.get("profile", "hello")!r}, the build wants {expected!r}\n'
            f'  Rebuild the guest set with: scripts/linux/build-gpu-linux-docker.sh')
    if guest is not None and manifest.get('guest','minimal') != guest:
        raise ValueError(f'Linux artifact guest layout does not match {guest}: {manifest_path}')
    for name, key in [('Image', 'kernel_size'), ('initramfs.cpio', 'initramfs_size'),
                      ('dango-linux.dtb', 'dtb_size')]:
        path = root / name
        size = path.stat().st_size
        declared = manifest.get(key)
        if size == 0 or size != declared:
            # A kernel-only rebuild leaves the manifest behind, and the bare
            # "stale or empty" message gives no way to tell which side is wrong.
            raise ValueError(
                f'{name}: artifact and manifest disagree\n'
                f'  {path}\n'
                f'  on disk: {size} bytes\n'
                f'  {manifest_path}: {key} = {declared}\n'
                f'  Fix by regenerating the manifest for the files that are present:\n'
                f'    DANGO_LINUX_OUT={root} DANGO_LINUX_PROFILE={expected} '
                f'scripts/linux/build-manifest.sh\n'
                f'  or rebuild the whole guest set:\n'
                f'    scripts/linux/build-gpu-linux-docker.sh')


if __name__ == '__main__':
    try:
        check(*sys.argv[1:])
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
