#!/usr/bin/env python3
"""Check whether a Mesa source tree matches a patch series exactly.

`patch --dry-run --reverse` cannot validate a series as a whole: the first patch
touches lines that later patches rewrite, so it only reverses once the later ones
are actually gone. This copies just the affected files into a scratch tree and
unwinds the series there in reverse order, leaving the real tree untouched.

Exit status: 0 when the tree matches the series, 1 when it does not.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HUNK_PATH = re.compile(r'^(?:\+\+\+|diff --git a/)\s*b?/?a?/?(\S+)', re.M)


def targets(patch: Path) -> list[str]:
    """Every file path the patch touches, in the order they appear."""
    out: list[str] = []
    text = patch.read_text(errors='replace')
    for line in text.splitlines():
        if line.startswith('diff --git '):
            name = line.split(' b/', 1)[-1].strip()
        elif line.startswith('+++ '):
            name = line[4:].strip()
            name = name[2:] if name.startswith('b/') else name
        elif line.startswith('--- '):
            name = line[4:].strip()
            name = name[2:] if name.startswith('a/') else name
        else:
            continue
        name = name.split('\t')[0]
        if name and name != '/dev/null' and name not in out:
            out.append(name)
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, patches = Path(sys.argv[1]), [Path(p) for p in sys.argv[2:]]
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        for patch in patches:
            for name in targets(patch):
                src = source / name
                if not src.is_file():
                    continue
                dst = scratch / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        for patch in reversed(patches):
            result = subprocess.run(
                ['patch', '--silent', '--batch', '--reverse', '-p1', '-d', str(scratch)],
                stdin=patch.open('rb'), capture_output=True)
            if result.returncode != 0:
                print(f'{patch.name} does not reverse cleanly', file=sys.stderr)
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
