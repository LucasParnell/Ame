#!/usr/bin/env python3
"""Package an isolated two-process IPC benchmark; normal system.mxb is untouched."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack

build = Path(sys.argv[1])
manifest = build / 'ipc-bench-manifest.json'
manifest.write_text('{"schema":2,"services":[]}\n')
image = str(build / 'userland/bench.ipc.mxi')
mxpack.build_bundle(build / 'ipc-bench.mxb', manifest,
                    ['exec:init:' + image, 'exec:bench.ipc:' + image])
