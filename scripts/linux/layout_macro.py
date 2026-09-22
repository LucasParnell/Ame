#!/usr/bin/env python3
"""Print dango_guest_layout.h macros for one guest layout, as NAME=<decimal>.

The layout header selects its branch with `#if defined(DANGO_GUEST_LAYOUT_LAVAPIPE)`,
so a plain `#define` scan cannot tell which value is active. This evaluates the
branches the way the build would, for the DTS generator and the artifact manifest,
so the guest DTS, the manifest and the kernel bounds all read the same numbers.

Usage: layout_macro.py <header> <layout> NAME [NAME...]
"""
import re
import sys


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    header, layout, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    if layout not in ('minimal', 'lavapipe'):
        sys.stderr.write(f'layout_macro.py: unknown layout {layout!r}\n')
        return 1
    lavapipe = layout == 'lavapipe'

    active = True
    values: dict[str, str] = {}
    with open(header) as handle:
        for line in handle:
            stripped = line.strip()
            if stripped.startswith('#if'):
                active = lavapipe if 'DANGO_GUEST_LAYOUT_LAVAPIPE' in stripped else True
                continue
            if stripped.startswith('#else'):
                active = not lavapipe
                continue
            if stripped.startswith('#endif'):
                active = True
                continue
            if not active:
                continue
            match = re.match(r'#define\s+(\w+)\s+([^\s/]+)', stripped)
            if match:
                values[match.group(1)] = match.group(2)

    missing = [name for name in names if name not in values]
    if missing:
        sys.stderr.write(f'layout_macro.py: {", ".join(missing)} missing for layout {layout}\n')
        return 1

    for name in names:
        raw = values[name].rstrip('uUlL')
        try:
            print(f'{name}={int(raw, 0)}')
        except ValueError:
            sys.stderr.write(f'layout_macro.py: {name} is not an integer literal ({raw!r})\n')
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
