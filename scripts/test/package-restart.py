#!/usr/bin/env python3
"""Package the isolated supervision-restart fixture with the development key."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack
build = Path(sys.argv[1])
manifest = build / 'restart-manifest.json'
manifest.write_text('{"schema":2,"services":[]}\n')
mxpack.build_bundle(build / 'restart.mxb', manifest, [
    'exec:init:' + str(build / 'userland/test.restart.mxi'),
    'exec:test.busy:' + str(build / 'userland/test.busy.mxi')])
