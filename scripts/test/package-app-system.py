#!/usr/bin/env python3
"""Run the real shell exec code against a prepopulated filesystem image."""
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mx'))
import mxpack

build = Path(sys.argv[1]).resolve()
manifest = json.loads((build / 'manifest.json').read_text())
terminal = '--terminal' in sys.argv[2:]
for service in manifest['services']:
    if service['name'] == 'msh' and not terminal:
        service['object'] = 'test.app_system'
        service['sha256'] = hashlib.sha256((build / 'userland/test.app_system.mxi').read_bytes()).hexdigest()
        service['start_after'] = ['io.fs', 'dev.apps']
        service['bindings'] = []
        service['restart'] = 'never'
name = 'app-terminal' if terminal else 'app-system'
if terminal:
    manifest['services'].append({
        'name': 'test.app_terminal', 'object': 'test.app_terminal', 'bundle': 'system',
        'sha256': hashlib.sha256((build / 'userland/test.app_terminal.mxi').read_bytes()).hexdigest(),
        'principal': 'native_trusted', 'stage': 'storage', 'restart': 'never',
        # This peer verifies a shell restart and must survive it. A start_after
        # dependency participates in init's restart closure and would kill the
        # verifier alongside msh. The peer waits for the prompt itself.
        'provides': [], 'uses': [], 'caps': [], 'start_after': [],
        'bindings': [
            {'channel': channel, 'name': entry, 'rights': rights}
            for channel, entry, rights in (
                ('serial_in_ring', 'serial.in.ring', ['read', 'write', 'map']),
                ('input_tty_db', 'serial.in.doorbell', ['notify']),
                ('serial_out_ring', 'serial.out.ring', ['read', 'write', 'map']),
                ('serial_console_db', 'serial.out.doorbell', ['wait']),
                ('console_activate_db', 'console.activate', ['notify']),
            )
        ],
    })
path = build / (name + '-manifest.json')
path.write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
objects = ['exec:init:' + str(build / 'userland/init.mxi')]
objects += [f"exec:{service['object']}:{build / 'userland' / (service['object'] + '.mxi')}"
            for service in manifest['services']]
mxpack.build_bundle(build / (name + '.mxb'), path, objects)
