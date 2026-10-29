#!/usr/bin/env python3
"""Report a PanVK build directory whose meson options differ from the pinned set.

`build-panvk-mesa.sh` reuses an existing build directory as-is. A directory
configured with egl, gbm or gallium enabled produces a different library than the
script describes, so say so instead of rebuilding it blind. The resulting ICD is
functionally equivalent for panvk, but the difference should be visible.
"""
import json
import sys
from pathlib import Path

WANT = {'gallium-drivers': [], 'egl': 'disabled', 'gbm': 'disabled', 'opengl': False}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    build = Path(sys.argv[1])
    options = build / 'meson-info/intro-buildoptions.json'
    if not options.is_file():
        return 0
    got = {o['name']: o.get('value') for o in json.loads(options.read_text())
           if o.get('name') in WANT}
    bad = [f'{k}={got.get(k)!r} (want {v!r})' for k, v in WANT.items()
           if got.get(k) != v]
    if not bad:
        return 0
    print('reusing a build dir configured differently: ' + ', '.join(bad),
          file=sys.stderr)
    print('the library is functionally equivalent for panvk, but for an exact '
          'rebuild use a fresh DANGO_PANVK_BUILD', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
