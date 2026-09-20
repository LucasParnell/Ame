#!/usr/bin/env python3
"""Package isolated kernel test processes with the development signing key."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack
build = Path(sys.argv[1])
manifest = build / 'lifecycle-manifest.json'
manifest.write_text('{"schema":2,"services":[]}\n')
mxpack.build_bundle(build / 'lifecycle.mxb', manifest, [
    'exec:init:' + str(build/'userland/test.lifecycle.mxi'),
    'exec:test.busy:' + str(build/'userland/test.busy.mxi'),
    'exec:test.share:' + str(build/'userland/test.share.mxi')])
