#!/usr/bin/env python3
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack
build = Path(sys.argv[1])
manifest = build / 'app-stream-manifest.json'
manifest.write_text('{"schema":2,"services":[]}\n')
mxpack.build_bundle(build / 'app-stream.mxb', manifest, [
    'exec:init:' + str(build / 'userland/test.app_stream.mxi'),
    'exec:app.stream-test:' + str(build / 'userland/app.stream-test.mxi'),
    'exec:test.busy:' + str(build / 'userland/test.busy.mxi')])
