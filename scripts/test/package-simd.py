#!/usr/bin/env python3
"""Package the isolated SIMD scheduler regression as init."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack

build = Path(sys.argv[1])
manifest = build / 'simd-manifest.json'
manifest.write_text('{"schema":2,"services":[]}\n')
mxpack.build_bundle(build / 'simd.mxb', manifest, [
    'exec:init:' + str(build / 'userland/test.simd.mxi')])
